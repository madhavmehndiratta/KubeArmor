/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2021 Authors of KubeArmor */

#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME "kubearmor_system_monitor"
#endif

// ========================================================== //
// KubeArmor utilizes Tracee's system call handling functions //
// developed by Aqua Security (https://aquasec.com).          //
// ========================================================== //

#ifdef asm_inline
#undef asm_inline
#define asm_inline asm
#endif

#include <linux/version.h>

#include <linux/nsproxy.h>
#include <linux/ns_common.h>
#include <linux/pid_namespace.h>
#include <linux/proc_ns.h>
#include <linux/mount.h>

#include <linux/un.h>
#include <net/inet_sock.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
#error Minimal required kernel version is 4.14
#endif

#undef container_of
#define container_of(ptr, type, member)                                        \
	({                                                                     \
		const typeof(((type *)0)->member) *__mptr = (ptr);             \
		(type *)((char *)__mptr - offsetof(type, member));             \
	})

// == Structures == //
#define MAX_BUFFER_SIZE   32768
#define MAX_STRING_SIZE   4096
#define MAX_STR_ARR_ELEM  20
#define MAX_LOOP_LIMIT    5 // arbitrarily set

#define NONE_T        0UL
#define INT_T         1UL
#define STR_T         10UL
#define STR_ARR_T     11UL
#define SOCKADDR_T    12UL
#define OPEN_FLAGS_T  13UL
#define EXEC_FLAGS_T  14UL
#define SOCK_DOM_T    15UL
#define SOCK_TYPE_T   16UL

#define MAX_ARGS               6
#define ENC_ARG_TYPE(n, type)  type<<(8*n)
#define ARG_TYPE0(type)        ENC_ARG_TYPE(0, type)
#define ARG_TYPE1(type)        ENC_ARG_TYPE(1, type)
#define ARG_TYPE2(type)        ENC_ARG_TYPE(2, type)
#define ARG_TYPE3(type)        ENC_ARG_TYPE(3, type)
#define ARG_TYPE4(type)        ENC_ARG_TYPE(4, type)
#define ARG_TYPE5(type)        ENC_ARG_TYPE(5, type)
#define DEC_ARG_TYPE(n, type)  ((type>>(8*n))&0xFF)

enum {
    // file
    _SYS_OPEN = 2,
    _SYS_OPENAT = 257,
    _SYS_CLOSE = 3,

    // network
    _SYS_SOCKET = 41,
    _SYS_CONNECT = 42,
    _SYS_ACCEPT = 43,
    _SYS_BIND = 49,
    _SYS_LISTEN = 50,

    // process
    _SYS_EXECVE = 59,
    _SYS_EXECVEAT = 322,
    _DO_EXIT = 351,
};

typedef struct __attribute__((__packed__)) sys_context {
    u64 ts;

    u32 pid_id;
    u32 mnt_id;

    u32 host_ppid;
    u32 host_pid;

    u32 ppid;
    u32 pid;
    u32 uid;

    u32 event_id;
    u32 argnum;
    s64 retval;

    char comm[TASK_COMM_LEN];
} sys_context_t;

BPF_HASH(pid_ns_map, u32, u32);

typedef struct args {
    unsigned long args[6];
} args_t;

BPF_HASH(args_map, u64, args_t);

typedef struct buffers {
    u8 buf[MAX_BUFFER_SIZE];
} bufs_t;

BPF_PERCPU_ARRAY(bufs, bufs_t, 2);
BPF_PERCPU_ARRAY(bufs_offset, u32, 2);

BPF_PERF_OUTPUT(sys_events);

// == Kernel Helpers == //

static __always_inline u32 get_task_pid_ns_id(struct task_struct *task)
{
    return task->nsproxy->pid_ns_for_children->ns.inum;
}

struct mnt_namespace {
    #if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
    atomic_t count;
    #endif
    struct ns_common ns;
};

struct mount {
	struct hlist_node mnt_hash;
	struct mount *mnt_parent;
	struct dentry *mnt_mountpoint;
	struct vfsmount mnt;
};

static __always_inline u32 get_task_mnt_ns_id(struct task_struct *task)
{
    return task->nsproxy->mnt_ns->ns.inum;
}

static __always_inline u32 get_task_ns_ppid(struct task_struct *task)
{
    unsigned int level = task->real_parent->nsproxy->pid_ns_for_children->level;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
    return task->real_parent->pids[PIDTYPE_PID].pid->numbers[level].nr;
#else
    return task->real_parent->thread_pid->numbers[level].nr;
#endif
}

static __always_inline u32 get_task_ns_tgid(struct task_struct *task)
{
    unsigned int level = task->nsproxy->pid_ns_for_children->level;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
    return task->group_leader->pids[PIDTYPE_PID].pid->numbers[level].nr;
#else
    return task->group_leader->thread_pid->numbers[level].nr;
#endif
}

static __always_inline u32 get_task_ns_pid(struct task_struct *task)
{
    unsigned int level = task->nsproxy->pid_ns_for_children->level;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
    return task->pids[PIDTYPE_PID].pid->numbers[level].nr;
#else
    return task->thread_pid->numbers[level].nr;
#endif
}

static __always_inline u32 get_task_ppid(struct task_struct *task)
{
    return task->real_parent->pid;
}

// == Pid NS Management == //

static __always_inline u32 add_pid_ns()
{
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();
    u32 one = 1;

#if defined(MONITOR_HOST)

    u32 pid_ns = get_task_pid_ns_id(task);
    if (pid_ns != PROC_PID_INIT_INO) {
        return 0;
    }

    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (pid_ns_map.lookup(&pid) != 0) {
        return pid;
    }

    pid_ns_map.update(&pid, &one);
    return pid;

#elif defined(MONITOR_HOST_AND_CONTAINER)

    u32 pid_ns = get_task_pid_ns_id(task);
    if (pid_ns == PROC_PID_INIT_INO) { // host
        u32 pid = bpf_get_current_pid_tgid() >> 32;
        if (pid_ns_map.lookup(&pid) != 0) {
            return pid;
        }

        pid_ns_map.update(&pid, &one);
        return pid;
    } else { // container
        if (pid_ns_map.lookup(&pid_ns) != 0) {
            return pid_ns;
        }

        pid_ns_map.update(&pid_ns, &one);
        return pid_ns;
    }

#else /* MONITOR_CONTAINER */

    u32 pid_ns = get_task_pid_ns_id(task);
    if (pid_ns == PROC_PID_INIT_INO) {
        return 0;
    }

    if (pid_ns_map.lookup(&pid_ns) != 0) {
        return pid_ns;
    }

    pid_ns_map.update(&pid_ns, &one);
    return pid_ns;

    return 0;

#endif /* MONITOR_HOST || MONITOR_CONTAINER */
}

static __always_inline u32 remove_pid_ns()
{
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();

#if defined(MONITOR_HOST)

    u32 pid_ns = get_task_pid_ns_id(task);
    if (pid_ns != PROC_PID_INIT_INO) {
        return 0;
    }

    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (pid_ns_map.lookup(&pid) != 0) {
        pid_ns_map.delete(&pid);
        return 0;
    }

#elif defined(MONITOR_HOST_AND_CONTAINER)

    u32 pid_ns = get_task_pid_ns_id(task);
    if (pid_ns == PROC_PID_INIT_INO) { // host
        u32 pid = bpf_get_current_pid_tgid() >> 32;
        if (pid_ns_map.lookup(&pid) != 0) {
            pid_ns_map.delete(&pid);
            return 0;
        }
    } else { // container
        if (get_task_ns_pid(task) == 1) {
            pid_ns_map.delete(&pid_ns);
            return 0;
        }
    }

#else /* !MONITOR_HOST */

    u32 pid_ns = get_task_pid_ns_id(task);
    if (pid_ns == PROC_PID_INIT_INO) {
        return 0;
    }

    if (get_task_ns_pid(task) == 1) {
        pid_ns_map.delete(&pid_ns);
        return 0;
    }

#endif /* !MONITOR_HOST */

    return 0;
}

static __always_inline u32 skip_syscall()
{
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();

#if defined(MONITOR_HOST)

    u32 pid_ns = get_task_pid_ns_id(task);
    if (pid_ns != PROC_PID_INIT_INO) {
        return 1;
    }

    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (pid_ns_map.lookup(&pid) != 0) {
        return 0;
    }

#elif defined(MONITOR_HOST_AND_CONTAINER)

    u32 pid_ns = get_task_pid_ns_id(task);
    if (pid_ns == PROC_PID_INIT_INO) { // host
        u32 pid = bpf_get_current_pid_tgid() >> 32;
        if (pid_ns_map.lookup(&pid) != 0) {
            return 0;
        }
    } else { // container
        u32 pid_ns = get_task_pid_ns_id(task);
        if (pid_ns_map.lookup(&pid_ns) != 0) {
            return 0;
        }
    }

#else /* !MONITOR_HOST */

    u32 pid_ns = get_task_pid_ns_id(task);
    if (pid_ns_map.lookup(&pid_ns) != 0) {
        return 0;
    }

#endif /* !MONITOR_HOST */

    return 1;
}

// == Context Management == //

static __always_inline u32 init_context(sys_context_t *context)
{
    struct task_struct *task = (struct task_struct *)bpf_get_current_task();

    context->ts = bpf_ktime_get_ns();

    context->host_ppid = get_task_ppid(task);
    context->host_pid = bpf_get_current_pid_tgid() >> 32;

#if defined(MONITOR_HOST)

    context->pid_id = 0;
    context->mnt_id = 0;

    context->ppid = get_task_ppid(task);
    context->pid = bpf_get_current_pid_tgid() >> 32;

#elif defined(MONITOR_HOST_AND_CONTAINER)

    u32 pid = get_task_ns_tgid(task);
    if (context->host_pid == pid) { // host
        context->pid_id = 0;
        context->mnt_id = 0;

        context->ppid = get_task_ppid(task);
        context->pid = bpf_get_current_pid_tgid() >> 32;
    } else { // container
        context->pid_id = get_task_pid_ns_id(task);
        context->mnt_id = get_task_mnt_ns_id(task);

        context->ppid = get_task_ns_ppid(task);
        context->pid = pid;
    }

#else /* !MONITOR_HOST */

    context->pid_id = get_task_pid_ns_id(task);
    context->mnt_id = get_task_mnt_ns_id(task);

    context->ppid = get_task_ns_ppid(task);
    context->pid = get_task_ns_tgid(task);

#endif /* !MONITOR_HOST */

    context->uid = bpf_get_current_uid_gid();

    bpf_get_current_comm(&context->comm, sizeof(context->comm));

    return 0;
}

// == Buffer Management == //

// TODO: Add macros for the indices
static __always_inline bufs_t* get_buffer(int idx)
{
    return bufs.lookup(&idx);
}

static __always_inline void set_buffer_offset(int idx, u32 off)
{
    bufs_offset.update(&idx, &off);
}

static __always_inline u32* get_buffer_offset(int idx)
{
    return bufs_offset.lookup(&idx);
}

static __always_inline int save_context_to_buffer(bufs_t *bufs_p, void *ptr)
{
    if (bpf_probe_read(&(bufs_p->buf[0]), sizeof(sys_context_t), ptr) == 0) {
        return sizeof(sys_context_t);
    }

    return 0;
}

static __always_inline int save_str_to_buffer(bufs_t *bufs_p, void *ptr)
{
    u32 *off = get_buffer_offset(0);
    if (off == NULL) {
        return -1;
    }

    if (*off > MAX_BUFFER_SIZE - MAX_STRING_SIZE - sizeof(int)) {
        return 0; // not enough space - return
    }

    u8 type = STR_T;
    bpf_probe_read(&(bufs_p->buf[*off & (MAX_BUFFER_SIZE-1)]), 1, &type);

    *off += 1;

    if (*off > MAX_BUFFER_SIZE - MAX_STRING_SIZE - sizeof(int)) {
        return 0;
    }

    int sz = bpf_probe_read_str(&(bufs_p->buf[*off + sizeof(int)]), MAX_STRING_SIZE, ptr);
    if (sz > 0) {
        if (*off > MAX_BUFFER_SIZE - sizeof(int)) {
            return 0;
        }

        bpf_probe_read(&(bufs_p->buf[*off]), sizeof(int), &sz);

        *off += sz + sizeof(int);
        set_buffer_offset(0, *off);

        return sz + sizeof(int);
    }

    return 0;
}

static __always_inline int save_to_buffer(bufs_t *bufs_p, void *ptr, int size, u8 type)
{
    // the biggest element that can be saved with this function should be defined here
    #define MAX_ELEMENT_SIZE sizeof(struct sockaddr_un)

    if (type == 0) {
        return 0;
    }

    u32 *off = get_buffer_offset(0);
    if (off == NULL) {
        return -1;
    }

    if (*off > MAX_BUFFER_SIZE - MAX_ELEMENT_SIZE) {
        return 0;
    }

    if (bpf_probe_read(&(bufs_p->buf[*off]), 1, &type) != 0) {
        return 0;
    }

    *off += 1;

    if (*off > MAX_BUFFER_SIZE - MAX_ELEMENT_SIZE) {
        return 0;
    }

    if (bpf_probe_read(&(bufs_p->buf[*off]), size, ptr) == 0) {
        *off += size;
        set_buffer_offset(0, *off);
        return size;
    }

    return 0;
}

static __always_inline int save_argv(bufs_t *bufs_p, void *ptr)
{
    const char *argp = NULL;
    bpf_probe_read(&argp, sizeof(argp), ptr);

    if (argp) {
        return save_str_to_buffer(bufs_p, (void *)(argp));
    }

    return 0;
}

static __always_inline int save_str_arr_to_buffer(bufs_t *bufs_p, const char __user *const __user *ptr)
{
    save_to_buffer(bufs_p, NULL, 0, STR_ARR_T);

    #pragma unroll
    for (int i = 0; i < MAX_STR_ARR_ELEM; i++) {
        if (save_argv(bufs_p, (void *)&ptr[i]) == 0) {
             goto out;
        }
    }

    char ellipsis[] = "...";
    save_str_to_buffer(bufs_p, (void *)ellipsis);

out:
    save_to_buffer(bufs_p, NULL, 0, STR_ARR_T);

    return 0;
}

static __always_inline int save_args_to_buffer(u64 types, args_t *args)
{
    if (types == 0) {
        return 0;
    }

    bufs_t *bufs_p = get_buffer(0);
    if (bufs_p == NULL) {
        return 0;
    }

    #pragma unroll
    for (int i = 0; i < MAX_ARGS; i++) {
        switch (DEC_ARG_TYPE(i, types)) {
        case NONE_T:
            break;
        case INT_T:
            save_to_buffer(bufs_p, (void*)&(args->args[i]), sizeof(int), INT_T);
            break;
        case OPEN_FLAGS_T:
            save_to_buffer(bufs_p, (void*)&(args->args[i]), sizeof(int), OPEN_FLAGS_T);
            break;
        case STR_T:
            save_str_to_buffer(bufs_p, (void *)args->args[i]);
            break;
        case SOCK_DOM_T:
            save_to_buffer(bufs_p, (void*)&(args->args[i]), sizeof(int), SOCK_DOM_T);
            break;
        case SOCK_TYPE_T:
            save_to_buffer(bufs_p, (void*)&(args->args[i]), sizeof(int), SOCK_TYPE_T);
            break;
        case SOCKADDR_T:
            if (args->args[i]) {
                short family = 0;
                bpf_probe_read(&family, sizeof(short), (void*)args->args[i]);
                switch (family) {
                case AF_UNIX:
                    save_to_buffer(bufs_p, (void*)(args->args[i]), sizeof(struct sockaddr_un), SOCKADDR_T);
                    break;
                case AF_INET:
                    save_to_buffer(bufs_p, (void*)(args->args[i]), sizeof(struct sockaddr_in), SOCKADDR_T);
                    break;
                case AF_INET6:
                    save_to_buffer(bufs_p, (void*)(args->args[i]), sizeof(struct sockaddr_in6), SOCKADDR_T);
                    break;
                default:
                    save_to_buffer(bufs_p, (void*)&family, sizeof(short), SOCKADDR_T);
                }
            }
            break;
        }
    }

    return 0;
}

static inline struct mount *real_mount(struct vfsmount *mnt)
{
	return container_of(mnt, struct mount, mnt);
}

static __always_inline int save_path_to_str_buf(bufs_t *string_p, const struct path *path)
{
    struct path f_path;
    bpf_probe_read(&f_path, sizeof(struct path), path);
    char slash = '/';
    int zero = 0;
    struct dentry *dentry = f_path.dentry;
    struct vfsmount *vfsmnt = f_path.mnt;
    struct mount *mnt_parent_p;

    struct mount *mnt_p = real_mount(vfsmnt);
    bpf_probe_read(&mnt_parent_p, sizeof(struct mount*), &mnt_p->mnt_parent);

    u32 buf_off = (MAX_BUFFER_SIZE >> 1);
    struct dentry *mnt_root;
    struct dentry *d_parent;
    struct qstr d_name;
    unsigned int len;
    unsigned int off;
    int sz;

    #pragma unroll
    for (int i = 0; i < MAX_LOOP_LIMIT; i++) {
        /* mnt_root = get_mnt_root_ptr_from_vfsmnt(vfsmnt); */
        bpf_probe_read(mnt_root, sizeof(struct dentry *), vfsmnt->mnt_root);
        /* d_parent = get_d_parent_ptr_from_dentry(dentry); */
        bpf_probe_read(d_parent, sizeof(struct dentry *), dentry->d_parent);
        if (dentry == mnt_root || dentry == d_parent) {
            if (dentry != mnt_root) {
                // We reached root, but not mount root - escaped?
                break;
            }
            if (mnt_p != mnt_parent_p) {
                // We reached root, but not global root - continue with mount point path
                bpf_probe_read(&dentry, sizeof(struct dentry*), &mnt_p->mnt_mountpoint);
                bpf_probe_read(&mnt_p, sizeof(struct mount*), &mnt_p->mnt_parent);
                bpf_probe_read(&mnt_parent_p, sizeof(struct mount*), &mnt_p->mnt_parent);
                vfsmnt = &mnt_p->mnt;
                continue;
            }
            // Global root - path fully parsed
            break;
        }
        // Add this dentry name to path

        /* d_name = get_d_name_from_dentry(dentry); */
        bpf_probe_read(&d_name, sizeof(struct qstr), &dentry->d_name);
        len = (d_name.len+1) & (MAX_STRING_SIZE-1);
        off = buf_off - len;

        // Is string buffer big enough for dentry name?
        sz = 0;
        if (off <= buf_off) { // verify no wrap occurred
            len = len & ((MAX_BUFFER_SIZE >> 1)-1);
            sz = bpf_probe_read_str(&(string_p->buf[off & ((MAX_BUFFER_SIZE >> 1)-1)]), len, (void *)d_name.name);
        }
        else
            break;
        if (sz > 1) {
            buf_off -= 1; // remove null byte termination with slash sign
            bpf_probe_read(&(string_p->buf[buf_off & (MAX_BUFFER_SIZE-1)]), 1, &slash);
            buf_off -= sz - 1;
        } else {
            // If sz is 0 or 1 we have an error (path can't be null nor an empty string)
            break;
        }
        dentry = d_parent;
    }

    if (buf_off == (MAX_BUFFER_SIZE >> 1)) {
        // memfd files have no path in the filesystem -> extract their name
        buf_off = 0;
        bpf_probe_read(&d_name, sizeof(struct qstr),&dentry->d_name);
        /* d_name = get_d_name_from_dentry(dentry); */
        bpf_probe_read_str(&(string_p->buf[0]), MAX_STRING_SIZE, (void *)d_name.name);
    } else {
        // Add leading slash
        buf_off -= 1;
        bpf_probe_read(&(string_p->buf[buf_off & (MAX_BUFFER_SIZE-1)]), 1, &slash);
        // Null terminate the path string
        bpf_probe_read(&(string_p->buf[(MAX_BUFFER_SIZE >> 1)-1]), 1, &zero);
    }

    set_buffer_offset(1, buf_off);
    return buf_off;
}

static __always_inline int prepend_path(struct path *path, bufs_t *buf)
{
	struct path f_path;
	struct qstr d_name;
	struct mount *mnt;

	bpf_probe_read(&f_path, sizeof(struct path), path);

    struct dentry *dentry = f_path.dentry;
    struct vfsmount *vfsmnt = f_path.mnt;

	/* bpf_probe_read(vfsmnt, sizeof(struct vfsmount *), f_path.mnt); */
	/* bpf_probe_read(dentry, sizeof(struct dentry *), f_path.dentry); */
	int offset = MAX_STRING_SIZE;

	mnt = real_mount(path->mnt);

	// TODO: check if the dentry is unlinked

	for (int i = 0; i < MAX_LOOP_LIMIT; i++) {
		struct dentry *parent /* = dentry->d_parent */;
		bpf_probe_read(parent, sizeof(struct dentry *), dentry->d_parent);
		if (dentry == vfsmnt->mnt_root) {
			struct mount *m; /* = mnt->mnt_parent; */
			bpf_probe_read(m, sizeof(struct mount *),
				       mnt->mnt_parent);
			if (mnt != m) {
                /* dentry = mnt->mnt_mountpoint; */
				bpf_probe_read(dentry, sizeof(struct dentry *),
					       &mnt->mnt_mountpoint);
				mnt = m;
				continue;
			}
			/* Global root */
			break;
		}

		if (dentry == parent) {
			break;
		}

		// get d_name
		struct qstr *d_name;
		bpf_probe_read(d_name, sizeof(struct qstr *), &dentry->d_name);
		u32 len = d_name->len;

		char *name;
		bpf_probe_read_str(name, len, &d_name->name);
		char *s;

		offset -= len + 1;
		if (offset < 0)
			break;
		int total_off = offset - len - 1;
		s = &(buf->buf[total_off]);
		*s++ = '/';
		bpf_probe_read_str(s, len, name);
		/* if (!prepend_name(buf, offset, d_name)) */
		/* 	break; */
	}
	return 0;
}

static __always_inline int events_perf_submit(struct pt_regs *ctx)
{
    bufs_t *bufs_p = get_buffer(0);
    if (bufs_p == NULL)
        return -1;

    u32 *off = get_buffer_offset(0);
    if (off == NULL)
        return -1;

    void *data = bufs_p->buf;
    int size = *off & (MAX_BUFFER_SIZE-1);

    return sys_events.perf_submit(ctx, data, size);
}

// == Syscall Hooks (Process) == //

int syscall__execve(struct pt_regs *ctx,
    const char __user *filename,
    const char __user *const __user *__argv,
    const char __user *const __user *__envp)
{
    sys_context_t context = {};

    if (!add_pid_ns())
        return 0;

    init_context(&context);

    context.event_id = _SYS_EXECVE;
    context.argnum = 2;
    context.retval = 0;

    set_buffer_offset(0, sizeof(sys_context_t));

    bufs_t *bufs_p = get_buffer(0);
    if (bufs_p == NULL)
        return 0;

    save_context_to_buffer(bufs_p, (void*)&context);

    save_str_to_buffer(bufs_p, (void *)filename);
    save_str_arr_to_buffer(bufs_p, __argv);

    events_perf_submit(ctx);

    return 0;
}

int trace_ret_execve(struct pt_regs *ctx)
{
    sys_context_t context = {};

    if (skip_syscall())
        return 0;

    init_context(&context);

    context.event_id = _SYS_EXECVE;
    context.argnum = 0;
    context.retval = PT_REGS_RC(ctx);

    // TEMP: skip if No such file or directory
    if (context.retval == -2) {
        return 0;
    }

    set_buffer_offset(0, sizeof(sys_context_t));

    bufs_t *bufs_p = get_buffer(0);
    if (bufs_p == NULL)
        return 0;

    save_context_to_buffer(bufs_p, (void*)&context);

    events_perf_submit(ctx);

    return 0;
}

int syscall__execveat(struct pt_regs *ctx,
    const int dirfd,
    const char __user *pathname,
    const char __user *const __user *__argv,
    const char __user *const __user *__envp,
    const int flags)
{
    sys_context_t context = {};

    if (!add_pid_ns())
        return 0;

    init_context(&context);

    context.event_id = _SYS_EXECVEAT;
    context.argnum = 4;
    context.retval = 0;

    set_buffer_offset(0, sizeof(sys_context_t));

    bufs_t *bufs_p = get_buffer(0);
    if (bufs_p == NULL)
        return 0;

    save_context_to_buffer(bufs_p, (void*)&context);

    save_to_buffer(bufs_p, (void*)&dirfd, sizeof(int), INT_T);
    save_str_to_buffer(bufs_p, (void *)pathname);
    save_str_arr_to_buffer(bufs_p, __argv);
    save_to_buffer(bufs_p, (void*)&flags, sizeof(int), EXEC_FLAGS_T);

    events_perf_submit(ctx);

    return 0;
}

int trace_ret_execveat(struct pt_regs *ctx)
{
    sys_context_t context = {};

    if (skip_syscall())
        return 0;

    init_context(&context);

    context.event_id = _SYS_EXECVEAT;
    context.argnum = 0;
    context.retval = PT_REGS_RC(ctx);

    // TEMP: skip if No such file or directory
    if (context.retval == -2) {
        return 0;
    }

    set_buffer_offset(0, sizeof(sys_context_t));

    bufs_t *bufs_p = get_buffer(0);
    if (bufs_p == NULL)
        return 0;

    save_context_to_buffer(bufs_p, (void*)&context);

    events_perf_submit(ctx);

    return 0;
}

int trace_do_exit(struct pt_regs *ctx, long code)
{
    sys_context_t context = {};

    if (skip_syscall())
        return 0;

    init_context(&context);

    context.event_id = _DO_EXIT;
    context.argnum = 0;
    context.retval = code;

    remove_pid_ns();

    set_buffer_offset(0, sizeof(sys_context_t));

    bufs_t *bufs_p = get_buffer(0);
    if (bufs_p == NULL)
        return 0;

    save_context_to_buffer(bufs_p, (void*)&context);

    events_perf_submit(ctx);

    return 0;
}

// == Syscall Hooks (File) == //

static __always_inline int save_args(u32 event_id, struct pt_regs *ctx)
{
    args_t args = {};

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 17, 0)
    args.args[0] = PT_REGS_PARM1(ctx);
    args.args[1] = PT_REGS_PARM2(ctx);
    args.args[2] = PT_REGS_PARM3(ctx);
    args.args[3] = PT_REGS_PARM4(ctx);
    args.args[4] = PT_REGS_PARM5(ctx);
    args.args[5] = PT_REGS_PARM6(ctx);
#else
    struct pt_regs * ctx2 = (struct pt_regs *)ctx->di;
    bpf_probe_read(&args.args[0], sizeof(args.args[0]), &ctx2->di);
    bpf_probe_read(&args.args[1], sizeof(args.args[1]), &ctx2->si);
    bpf_probe_read(&args.args[2], sizeof(args.args[2]), &ctx2->dx);
    bpf_probe_read(&args.args[3], sizeof(args.args[3]), &ctx2->r10);
    bpf_probe_read(&args.args[4], sizeof(args.args[4]), &ctx2->r8);
    bpf_probe_read(&args.args[5], sizeof(args.args[5]), &ctx2->r9);
#endif

    u32 tgid = bpf_get_current_pid_tgid();
    u64 id = ((u64)event_id << 32) | tgid;

    args_map.update(&id, &args);

    return 0;
}

static __always_inline int load_args(u32 event_id, args_t *args)
{
    u32 tgid = bpf_get_current_pid_tgid();
    u64 id = ((u64)event_id << 32) | tgid;

    args_t *saved_args = args_map.lookup(&id);
    if (saved_args == 0) {
        return -1; // missed entry or not a container
    }

    args->args[0] = saved_args->args[0];
    args->args[1] = saved_args->args[1];
    args->args[2] = saved_args->args[2];
    args->args[3] = saved_args->args[3];
    args->args[4] = saved_args->args[4];
    args->args[5] = saved_args->args[5];

    args_map.delete(&id);

    return 0;
}

static __always_inline int get_arg_num(u64 types)
{
    unsigned int i, argnum = 0;

    #pragma unroll
    for(i = 0; i < MAX_ARGS; i++) {
        if (DEC_ARG_TYPE(i, types) != NONE_T)
            argnum++;
    }

    return argnum;
}

static __always_inline int trace_ret_generic(u32 id, struct pt_regs *ctx, u64 types)
{
    sys_context_t context = {};
    args_t args = {};

    if (load_args(id, &args) != 0)
        return 0;

    if (skip_syscall())
        return 0;

    init_context(&context);

    context.event_id = id;
    context.argnum = get_arg_num(types);
    context.retval = PT_REGS_RC(ctx);

    // TEMP: skip if No such file or directory
    if (context.retval == -2) {
        return 0;
    }

    set_buffer_offset(0, sizeof(sys_context_t));

    bufs_t *bufs_p = get_buffer(0);
    if (bufs_p == NULL)
        return 0;

    save_context_to_buffer(bufs_p, (void*)&context);
    save_args_to_buffer(types, &args);

    events_perf_submit(ctx);

    return 0;
}

int syscall__open(struct pt_regs *ctx)
{
    if (skip_syscall())
        return 0;

    return save_args(_SYS_OPEN, ctx);
}

int trace_ret_open(struct pt_regs *ctx)
{
    return trace_ret_generic(_SYS_OPEN, ctx, ARG_TYPE0(STR_T)|ARG_TYPE1(OPEN_FLAGS_T));  
}

int syscall__openat(struct pt_regs *ctx)
{
    if (skip_syscall())
        return 0;

    return save_args(_SYS_OPENAT, ctx);
}

int trace_ret_openat(struct pt_regs *ctx)
{
    return trace_ret_generic(_SYS_OPENAT, ctx, ARG_TYPE0(INT_T)|ARG_TYPE1(STR_T)|ARG_TYPE2(OPEN_FLAGS_T));
}

int kprobe_security_file_open(struct pt_regs *ctx)
{
	args_t args = {};
	struct file *f = (struct file*)PT_REGS_PARM1(ctx);
  /* struct pt_regs * ctx2 = (struct pt_regs *)ctx->di; */
  /*   bpf_probe_read(&args.args[0], sizeof(args.args[0]), &ctx2->di); */

	bufs_t *string_p = get_buffer(1);

	/* bpf_probe_read(&f_path, sizeof(struct path), &f->f_path); */

	// TODO: check which syscall we are in
	u32 event_id = _SYS_OPENAT;

	if (load_args(event_id, &args) != 0)
		return 0;

    /* prepend_path(&f->f_path, string_p); */
save_path_to_str_buf(string_p, &f->f_path);

	u32 tgid = bpf_get_current_pid_tgid();
	u64 id = ((u64)event_id << 32) | tgid;

	bpf_probe_read_str(&args.args[1], sizeof(args.args[1]), string_p);

	args_map.update(&id, &args);
	return 0;
}

int syscall__close(struct pt_regs *ctx)
{ 
    if (skip_syscall())
        return 0;

    return save_args(_SYS_CLOSE, ctx);
}

int trace_ret_close(struct pt_regs *ctx)
{
    return trace_ret_generic(_SYS_CLOSE, ctx, ARG_TYPE0(INT_T));
}

// == Syscall Hooks (Network) == //

int syscall__socket(struct pt_regs *ctx)
{
    if (skip_syscall())
        return 0;

    return save_args(_SYS_SOCKET, ctx);
}

int trace_ret_socket(struct pt_regs *ctx)
{
    return trace_ret_generic(_SYS_SOCKET, ctx, ARG_TYPE0(SOCK_DOM_T)|ARG_TYPE1(SOCK_TYPE_T)|ARG_TYPE2(INT_T));
}

int syscall__connect(struct pt_regs *ctx)
{
    if (skip_syscall())
        return 0;

    return save_args(_SYS_CONNECT, ctx);   
}

int trace_ret_connect(struct pt_regs *ctx)
{
    return trace_ret_generic(_SYS_CONNECT, ctx, ARG_TYPE0(INT_T)|ARG_TYPE1(SOCKADDR_T));
}

int syscall__accept(struct pt_regs *ctx)
{
    if (skip_syscall())
        return 0;

    return save_args(_SYS_ACCEPT, ctx);   
}

int trace_ret_accept(struct pt_regs *ctx)
{
    return trace_ret_generic(_SYS_ACCEPT, ctx, ARG_TYPE0(INT_T)|ARG_TYPE1(SOCKADDR_T));
}

int syscall__bind(struct pt_regs *ctx)
{
    if (skip_syscall())
        return 0;

    return save_args(_SYS_BIND, ctx);   
}

int trace_ret_bind(struct pt_regs *ctx)
{
    return trace_ret_generic(_SYS_BIND, ctx, ARG_TYPE0(INT_T)|ARG_TYPE1(SOCKADDR_T));
}

int syscall__listen(struct pt_regs *ctx)
{
    if (skip_syscall())
        return 0;

    return save_args(_SYS_LISTEN, ctx);   
}

int trace_ret_listen(struct pt_regs *ctx)
{
    return trace_ret_generic(_SYS_LISTEN, ctx, ARG_TYPE0(INT_T)|ARG_TYPE1(INT_T));
}
