#include "kernel/api/syscall.h"
#include "msr.h"
#include "percpu.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "drivers/serial/serial.h"
#include "fs/vfs.h"
#include "fs/elf.h"
#include "fs/poll.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "ipc/pipe.h"
#include "os/services/compositor/compositor.h"
#include "drivers/drm/drm.h"
#include "drivers/bus/pci.h"
#include "drivers/ai/ai_accel.h"
#include "nexus/channel.h"
#include "nexus/graph.h"
#include "cpu/smp.h"
#include "os/services/installer/installer.h"
#include "os/services/app_registry/app_registry.h"
#include "net/net.h"
#include "net/net_socket.h"
#include "mem/heap.h"

extern void syscall_entry(void);   /* in syscall.asm */

static u8 syscall_stack[16384] ALIGNED(16);

#define YAM_AT_FDCWD (-100)
#define YAM_AT_REMOVEDIR 0x200
#define YAM_F_GETFD 1
#define YAM_F_SETFD 2
#define YAM_FD_CLOEXEC 1

static u64 *syscall_current_pml4(void) {
    task_t *t = this_cpu()->current;
    if (t && t->pml4) return t->pml4;

    u64 cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    u64 *pml4 = (u64 *)vmm_phys_to_virt(cr3 & 0x000FFFFFFFFFF000ULL);
    if (t && t->id != 0) t->pml4 = pml4;
    return pml4;
}

void syscall_init(void) {
    percpu_set_kernel_rsp((u64)&syscall_stack[sizeof(syscall_stack)]);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR, ((u64)0x10 << 48) | ((u64)0x08 << 32));
    wrmsr(MSR_LSTAR, (u64)syscall_entry);
    wrmsr(MSR_SFMASK, 0x700);

    kprintf_color(0xFF00FF88, "[SYSCALL] entry=%p  STAR=0x%lx\n",
                  (void *)syscall_entry, rdmsr(MSR_STAR));
}

/* ---- Individual syscall handlers ---- */

static i64 sys_write_serial(u64 fd, u64 ubuf, u64 len) {
    (void)fd;
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    const char *p = (const char *)ubuf;
    for (u64 i = 0; i < len; i++) serial_putchar(p[i]);
    if (smap) __asm__ volatile ("clac");
    return (i64)len;
}

static i64 sys_getpid(void) { return (i64)this_cpu()->current->id; }
static i64 sys_getppid(void) {
    task_t *t = this_cpu()->current;
    return t->parent ? (i64)t->parent->id : 0;
}
static i64 sys_yield(void) { sched_yield(); return 0; }
static i64 sys_sleepms(u64 ms) { task_sleep_ms(ms); return 0; }
static i64 copy_to_user(u64 uout, const void *src, usize len);

static i64 sys_exit(u64 code) {
    task_t *t = this_cpu()->current;
    kprintf_color(0xFFFF8833, "[SYS] task '%s' exit(%lu)\n", t->name, code);
    sched_exit_current((i32)code);
    for (;;) {
        sched_yield();
        __asm__ volatile ("hlt");
    }
    __builtin_unreachable();
}

static i64 sys_clock_gettime(void) {
    return (i64)this_cpu()->ticks * 10; /* Convert ticks to ms (100Hz timer) */
}

static i64 sys_getrusage(u64 uout) {
    if (!uout) return -1;
    task_t *t = this_cpu()->current;
    yam_rusage_t ru;
    memset(&ru, 0, sizeof(ru));
    ru.pid = t ? t->id : 0;
    ru.ppid = (t && t->parent) ? t->parent->id : 0;
    ru.ticks = t ? t->ticks : 0;
    ru.utime = t ? t->utime : 0;
    ru.stime = t ? t->stime : 0;
    ru.voluntary_switches = t ? t->vol_switches : 0;
    ru.involuntary_switches = t ? t->invol_switches : 0;
    ru.start_tick = t ? t->start_tick : 0;
    ru.rss_pages = t ? t->rss_pages : 0;
    ru.nice = t ? t->nice : 0;
    ru.cpu = t ? t->cpu : 0;
    ru.affinity = t ? t->cpu_affinity : 1;

    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    memcpy((void *)uout, &ru, sizeof(ru));
    if (smap) __asm__ volatile ("clac");
    return 0;
}

static i64 sys_sched_setaffinity(u64 mask) {
    task_t *t = this_cpu()->current;
    if (!t || mask == 0) return -1;

    u64 allowed = 0;
    u32 sched_cpus = smp_sched_cpu_count();
    if (sched_cpus == 0) sched_cpus = 1;
    if (sched_cpus > MAX_CPUS) sched_cpus = MAX_CPUS;
    for (u32 cpu = 0; cpu < sched_cpus; cpu++) allowed |= (1ULL << cpu);

    u64 effective = mask & allowed;
    if (!effective) effective = 1;
    if (effective != mask) {
        kprintf("[SCHED] affinity clamp task='%s' requested=0x%lx effective=0x%lx sched_cpus=%u detected=%u\n",
                t->name, mask, effective, sched_cpus, smp_cpu_count());
    }
    sched_set_affinity(t, effective);
    return 0;
}

static i64 sys_sched_getaffinity(void) {
    task_t *t = this_cpu()->current;
    return t ? (i64)sched_get_affinity(t) : -1;
}

static i64 sys_sched_info(u64 uout) {
    if (!uout) return -1;
    sched_info_t info;
    sched_get_info(&info);

    yam_sched_info_t out;
    memset(&out, 0, sizeof(out));
    out.detected_cpus = info.detected_cpus;
    out.schedulable_cpus = info.schedulable_cpus;
    out.total_tasks = info.total_tasks;
    out.ready_tasks = info.ready_tasks;
    out.blocked_tasks = info.blocked_tasks;
    out.running_tasks = info.running_tasks;
    out.total_switches = info.total_switches;
    out.ticks = info.ticks;
    for (u32 i = 0; i < 8 && i < MAX_CPUS; i++) {
        out.rq_load[i] = info.rq_load[i];
        out.rq_ready[i] = info.rq_ready[i];
    }
    out.lifetime_tasks_created = info.lifetime_tasks_created;
    out.lifetime_processes_forked = info.lifetime_processes_forked;
    out.lifetime_threads_created = info.lifetime_threads_created;
    out.lifetime_tasks_reaped = info.lifetime_tasks_reaped;
    out.lifetime_task_objects_freed = info.lifetime_task_objects_freed;
    out.lifetime_kernel_stacks_freed = info.lifetime_kernel_stacks_freed;
    out.lifetime_user_pml4s_destroyed = info.lifetime_user_pml4s_destroyed;
    out.lifetime_vma_lists_destroyed = info.lifetime_vma_lists_destroyed;
    out.lifetime_fd_tables_closed = info.lifetime_fd_tables_closed;
    out.lifetime_graph_nodes_destroyed = info.lifetime_graph_nodes_destroyed;
    out.lifetime_orphans_detached = info.lifetime_orphans_detached;
    out.lifetime_detached_tasks_reaped = info.lifetime_detached_tasks_reaped;

    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    memcpy((void *)uout, &out, sizeof(out));
    if (smap) __asm__ volatile ("clac");
    return 0;
}

static char g_clipboard[1024];
static u32 g_clipboard_len = 0;

static i64 sys_clipboard_set(u64 utext, u32 len) {
    if (!utext) return -1;
    if (len >= sizeof(g_clipboard)) len = sizeof(g_clipboard) - 1;

    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    memcpy(g_clipboard, (const void *)utext, len);
    if (smap) __asm__ volatile ("clac");

    g_clipboard[len] = '\0';
    g_clipboard_len = len;
    kprintf("[CLIPBOARD] set len=%u text='%s'\n", g_clipboard_len, g_clipboard);
    return (i64)g_clipboard_len;
}

static i64 sys_clipboard_get(u64 uout, u32 cap) {
    if (!uout || cap == 0) return -1;
    u32 n = g_clipboard_len;
    if (n >= cap) n = cap - 1;

    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    memcpy((void *)uout, g_clipboard, n);
    ((char *)uout)[n] = '\0';
    if (smap) __asm__ volatile ("clac");
    return (i64)n;
}

static void copy_user_string(char *dst, usize cap, u64 usrc) {
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!usrc) return;
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    const char *src = (const char *)usrc;
    usize i = 0;
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    if (smap) __asm__ volatile ("clac");
}

static i64 copy_installer_status_to_user(u64 uout, const yam_installer_status_t *st, i64 rc) {
    if (!uout || !st) return -1;
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    memcpy((void *)uout, st, sizeof(*st));
    if (smap) __asm__ volatile ("clac");
    return rc;
}

static i64 sys_installer_status(u64 upackage, u64 uout) {
    char package[32];
    yam_installer_status_t st;
    copy_user_string(package, sizeof(package), upackage);
    i64 rc = installer_status(package, &st);
    return copy_installer_status_to_user(uout, &st, rc);
}

static i64 sys_installer_request(u64 upackage, u64 uout) {
    char package[32];
    yam_installer_status_t st;
    copy_user_string(package, sizeof(package), upackage);
    i64 rc = installer_request(package, &st);
    return copy_installer_status_to_user(uout, &st, rc);
}

static i64 sys_readdir_user(u64 upath, u32 index, u64 uout) {
    char path[256];
    vfs_dirent_t dent;
    yam_dirent_t out;

    copy_user_string(path, sizeof(path), upath);
    if (!path[0] || !uout) return -1;
    if (sys_readdir(path, index, &dent) < 0) return -1;

    memset(&out, 0, sizeof(out));
    strncpy(out.name, dent.name, sizeof(out.name) - 1);
    out.size = dent.size;
    out.is_dir = dent.is_dir ? 1 : 0;
    return copy_to_user(uout, &out, sizeof(out));
}

static i64 sys_stat_user(u64 upath, u64 uout) {
    char path[256];
    yam_stat_t st;
    copy_user_string(path, sizeof(path), upath);
    if (!path[0] || !uout) return -1;
    if (sys_stat(path, &st) < 0) return -1;
    return copy_to_user(uout, &st, sizeof(st));
}

static i64 sys_fstat_user(int fd, u64 uout) {
    yam_stat_t st;
    if (!uout) return -1;
    if (sys_fstat(fd, &st) < 0) return -1;
    return copy_to_user(uout, &st, sizeof(st));
}

static i64 sys_unlink_user(u64 upath) {
    char path[256];
    copy_user_string(path, sizeof(path), upath);
    if (!path[0]) return -1;
    return sys_unlink(path);
}

static i64 sys_rename_user(u64 uold, u64 unew) {
    char oldpath[256];
    char newpath[256];
    copy_user_string(oldpath, sizeof(oldpath), uold);
    copy_user_string(newpath, sizeof(newpath), unew);
    if (!oldpath[0] || !newpath[0]) return -1;
    return sys_rename(oldpath, newpath);
}

static i64 resolve_at_path(int dirfd, u64 upath, char *out, usize cap) {
    char path[256];
    copy_user_string(path, sizeof(path), upath);
    if (!path[0] || !out || cap == 0) return -1;
    if (path[0] == '/' || dirfd == YAM_AT_FDCWD) {
        strncpy(out, path, cap - 1);
        out[cap - 1] = 0;
        return 0;
    }

    file_t *dir = fd_get(dirfd);
    if (!dir || dir->mount_idx < 0) return -1;
    yam_stat_t st;
    if (sys_stat(dir->path, &st) < 0 || (st.mode & YAM_S_IFMT) != YAM_S_IFDIR) return -1;
    if (strcmp(dir->path, "/") == 0) ksnprintf(out, cap, "/%s", path);
    else ksnprintf(out, cap, "%s/%s", dir->path, path);
    return 0;
}

static i64 sys_openat_user(int dirfd, u64 upath, u32 flags) {
    char path[256];
    if (resolve_at_path(dirfd, upath, path, sizeof(path)) < 0) return -1;
    return sys_open(path, flags);
}

static i64 sys_fstatat_user(int dirfd, u64 upath, u64 uout, int flags) {
    (void)flags;
    char path[256];
    yam_stat_t st;
    if (!uout || resolve_at_path(dirfd, upath, path, sizeof(path)) < 0) return -1;
    if (sys_stat(path, &st) < 0) return -1;
    return copy_to_user(uout, &st, sizeof(st));
}

static i64 sys_mkdirat_user(int dirfd, u64 upath, u32 mode) {
    char path[256];
    if (resolve_at_path(dirfd, upath, path, sizeof(path)) < 0) return -1;
    return sys_mkdir(path, mode);
}

static i64 sys_unlinkat_user(int dirfd, u64 upath, int flags) {
    if (flags & YAM_AT_REMOVEDIR) return -1;
    char path[256];
    if (resolve_at_path(dirfd, upath, path, sizeof(path)) < 0) return -1;
    return sys_unlink(path);
}

static i64 sys_renameat_user(int olddirfd, u64 uold, int newdirfd, u64 unew) {
    char oldpath[256];
    char newpath[256];
    if (resolve_at_path(olddirfd, uold, oldpath, sizeof(oldpath)) < 0) return -1;
    if (resolve_at_path(newdirfd, unew, newpath, sizeof(newpath)) < 0) return -1;
    return sys_rename(oldpath, newpath);
}

static i64 sys_open_user(u64 upath, u32 flags) {
    char path[256];
    copy_user_string(path, sizeof(path), upath);
    if (!path[0]) return -1;
    return sys_open(path, flags);
}

static i64 sys_mkdir_user(u64 upath, u32 mode) {
    char path[256];
    copy_user_string(path, sizeof(path), upath);
    if (!path[0]) return -1;
    return sys_mkdir(path, mode);
}

static i64 sys_chdir_user(u64 upath) {
    char path[256];
    copy_user_string(path, sizeof(path), upath);
    if (!path[0]) return -1;
    return sys_chdir(path);
}

static i64 sys_getcwd_user(u64 uout, usize size) {
    char cwd[256];
    isize n = sys_getcwd(cwd, sizeof(cwd));
    if (n < 0 || !uout || size == 0) return -1;
    if ((usize)n + 1 > size) return -1;
    return copy_to_user(uout, cwd, (usize)n + 1) == 0 ? n : -1;
}

static void free_user_string_vector(char **argv, int argc) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) {
        if (argv[i]) kfree(argv[i]);
    }
}

static int copy_user_string_vector(u64 uargv, const char *fallback,
                                   char **argv, int max_argc) {
    int argc = 0;
    if (!argv || max_argc <= 0) return 0;
    memset(argv, 0, (usize)max_argc * sizeof(char *));
    if (!uargv) {
        if (!fallback) return 0;
        argv[0] = (char *)kmalloc(256);
        if (!argv[0]) return 0;
        strncpy(argv[0], fallback, 255);
        argv[0][255] = 0;
        return 1;
    }

    bool smap = (read_cr4() & CR4_SMAP) != 0;
    for (; argc < max_argc; argc++) {
        u64 uptr = 0;
        if (smap) __asm__ volatile ("stac");
        uptr = ((const u64 *)uargv)[argc];
        if (smap) __asm__ volatile ("clac");
        if (!uptr) break;
        argv[argc] = (char *)kmalloc(256);
        if (!argv[argc]) break;
        copy_user_string(argv[argc], 256, uptr);
    }
    if (argc == 0) {
        if (!fallback) return 0;
        argv[0] = (char *)kmalloc(256);
        if (!argv[0]) return 0;
        strncpy(argv[0], fallback, 255);
        argv[0][255] = 0;
        argc = 1;
    }
    return argc;
}

static i64 sys_execve_user(u64 upath, u64 uargv, u64 uenvp) {
    char path[256];
    char *argv[16];
    char *envp[16];
    copy_user_string(path, sizeof(path), upath);
    if (!path[0]) return -1;
    int argc = copy_user_string_vector(uargv, path, argv, 16);
    int envc = copy_user_string_vector(uenvp, NULL, envp, 16);
    i64 rc = elf_exec_resolved_argv_envp(path, argc,
                                         (const char *const *)argv,
                                         envc > 0 ? (const char *const *)envp : NULL);
    free_user_string_vector(argv, argc);
    free_user_string_vector(envp, envc);
    return rc;
}

static i64 sys_spawn_user(u64 upath, u64 uargv, u64 uenvp) {
    char path[256];
    char *argv[16];
    char *envp[16];
    copy_user_string(path, sizeof(path), upath);
    if (!path[0]) return -1;
    int argc = copy_user_string_vector(uargv, path, argv, 16);
    int envc = copy_user_string_vector(uenvp, NULL, envp, 16);
    i64 pid = elf_spawn_resolved_argv_envp(path, argc,
                                           (const char *const *)argv,
                                           envc > 0 ? (const char *const *)envp : NULL);
    free_user_string_vector(argv, argc);
    free_user_string_vector(envp, envc);
    return pid;
}

static i64 sys_waitpid_user(i64 pid, u64 ustatus, u32 options) {
    i32 kstatus = 0;
    i64 waited = sched_waitpid(pid, ustatus ? &kstatus : NULL, options);
    if (waited <= 0) return waited;
    if (ustatus && copy_to_user(ustatus, &kstatus, sizeof(kstatus)) < 0) return -1;
    return waited;
}

static i64 copy_to_user(u64 uout, const void *src, usize len) {
    if (!uout || !src || len == 0) return -1;
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    memcpy((void *)uout, src, len);
    if (smap) __asm__ volatile ("clac");
    return 0;
}

static i64 sys_os_info(u64 uout) {
    yam_os_info_t info;
    memset(&info, 0, sizeof(info));
    info.abi_version = YAM_ABI_VERSION;
    info.syscall_max = SYS_MAX;
    info.page_size = PAGE_SIZE;
    info.pointer_bits = 64;
    info.cpu_count = smp_cpu_count();
    info.flags = YAM_OS_FLAG_PREEMPTIVE |
                 YAM_OS_FLAG_GRAPH_IPC |
                 YAM_OS_FLAG_GUI_COMPOSITOR |
                 YAM_OS_FLAG_DRIVER_SYSCALLS |
                 YAM_OS_FLAG_NETWORK_STACK |
                 YAM_OS_FLAG_INSTALLER_SERVICE |
                 YAM_OS_FLAG_SOCKET_ABI |
                 YAM_OS_FLAG_VFS_SPAWN |
                 YAM_OS_FLAG_THREADS |
                 YAM_OS_FLAG_NONBLOCK_IO |
                 YAM_OS_FLAG_PTHREAD_ABI |
                 YAM_OS_FLAG_SIGNALS |
                 YAM_OS_FLAG_YAM_EXTENSION;
    strncpy(info.os_name, YAM_OS_NAME, sizeof(info.os_name) - 1);
    strncpy(info.kernel_name, YAM_KERNEL_NAME, sizeof(info.kernel_name) - 1);
    return copy_to_user(uout, &info, sizeof(info));
}

static i64 sys_app_register(u64 umanifest) {
    if (!umanifest) return -1;
    yam_app_manifest_t manifest;
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    memcpy(&manifest, (const void *)umanifest, sizeof(manifest));
    if (smap) __asm__ volatile ("clac");

    manifest.name[sizeof(manifest.name) - 1] = 0;
    manifest.publisher[sizeof(manifest.publisher) - 1] = 0;
    manifest.version[sizeof(manifest.version) - 1] = 0;
    manifest.description[sizeof(manifest.description) - 1] = 0;
    return app_registry_register(this_cpu()->current, &manifest);
}

static i64 sys_app_query(u32 index, u64 uout) {
    yam_app_record_t record;
    i64 rc = app_registry_query(index, &record);
    if (rc < 0) return rc;
    return copy_to_user(uout, &record, sizeof(record));
}

typedef struct {
    int domain;
    int type;
    int proto;
    int tcp_fd;
    u16 bound_port;
    bool listening;
    bool nonblocking;
} yam_socket_file_t;

static isize socket_read(file_t *file, void *buf, usize count) {
    yam_socket_file_t *sock = (yam_socket_file_t *)file->private_data;
    if (!sock || sock->type != SOCK_STREAM || sock->tcp_fd < 0) return -1;
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    int rc = tcp_recv(sock->tcp_fd, buf, count);
    if (smap) __asm__ volatile ("clac");
    return rc;
}

static isize socket_write(file_t *file, const void *buf, usize count) {
    yam_socket_file_t *sock = (yam_socket_file_t *)file->private_data;
    if (!sock || sock->type != SOCK_STREAM || sock->tcp_fd < 0) return -1;
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    int rc = tcp_send(sock->tcp_fd, buf, count);
    if (smap) __asm__ volatile ("clac");
    return rc;
}

static int socket_poll(file_t *file) {
    yam_socket_file_t *sock = (yam_socket_file_t *)file->private_data;
    if (!sock || sock->type != SOCK_STREAM || sock->tcp_fd < 0) return 0;
    if (tcp_available(sock->tcp_fd) > 0) return 1;
    return sock->listening ? 1 : 0;
}

static int socket_close(file_t *file) {
    yam_socket_file_t *sock = (yam_socket_file_t *)file->private_data;
    if (sock) {
        if (sock->tcp_fd >= 0) tcp_close(sock->tcp_fd);
        kfree(sock);
        file->private_data = NULL;
    }
    return 0;
}

static file_operations_t socket_fops = {
    .read = socket_read,
    .write = socket_write,
    .mmap = NULL,
    .poll = socket_poll,
    .close = socket_close,
};

static int socket_wrap_tcp_fd(int tcp_fd, int domain, int type, int proto, u16 bound_port, bool listening) {
    yam_socket_file_t *sock = (yam_socket_file_t *)kmalloc(sizeof(*sock));
    if (!sock) {
        if (tcp_fd >= 0) tcp_close(tcp_fd);
        return -1;
    }
    memset(sock, 0, sizeof(*sock));
    sock->domain = domain;
    sock->type = type;
    sock->proto = proto;
    sock->tcp_fd = tcp_fd;
    sock->bound_port = bound_port;
    sock->listening = listening;

    file_t *f = (file_t *)kmalloc(sizeof(file_t));
    if (!f) {
        tcp_close(tcp_fd);
        kfree(sock);
        return -1;
    }
    memset(f, 0, sizeof(*f));
    f->fops = &socket_fops;
    f->private_data = sock;
    f->ref_count = 1;
    f->mount_idx = -1;
    strncpy(f->path, "socket:[tcp]", sizeof(f->path) - 1);

    int fd = fd_alloc(f);
    if (fd < 0) {
        socket_close(f);
        kfree(f);
        return -1;
    }
    return fd;
}

static i64 sys_socket_user(int domain, int type, int proto) {
    if (domain != AF_INET) return -1;
    bool nonblock = (type & SOCK_NONBLOCK) != 0;
    int base_type = type & ~SOCK_NONBLOCK;
    if (base_type != SOCK_STREAM) return -1;
    if (proto != 0 && proto != IPPROTO_TCP) return -1;
    int tcpfd = tcp_socket();
    if (tcpfd < 0) return -1;
    if (nonblock) tcp_set_nonblock(tcpfd, true);
    int fd = socket_wrap_tcp_fd(tcpfd, domain, base_type, proto ? proto : IPPROTO_TCP, 0, false);
    if (fd >= 0) {
        file_t *f = fd_get(fd);
        if (f) {
            yam_socket_file_t *sock = (yam_socket_file_t *)f->private_data;
            if (sock) sock->nonblocking = nonblock;
        }
        kprintf("[SOCKET] socket(AF_INET, SOCK_STREAM%s) -> fd%d tcp%d\n",
                nonblock ? "|NONBLOCK" : "", fd, tcpfd);
    }
    return fd;
}

static i64 copy_sockaddr_in_from_user(u64 uaddr, u32 len, sockaddr_in_t *out) {
    if (!uaddr || !out || len < sizeof(sockaddr_in_t)) return -1;
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    memcpy(out, (const void *)uaddr, sizeof(*out));
    if (smap) __asm__ volatile ("clac");
    if (out->sin_family != AF_INET) return -1;
    return 0;
}

static i64 sys_bind_user(int fd, u64 uaddr, u32 len) {
    file_t *f = fd_get(fd);
    if (!f || f->fops != &socket_fops) return -1;
    yam_socket_file_t *sock = (yam_socket_file_t *)f->private_data;
    sockaddr_in_t addr;
    if (!sock || copy_sockaddr_in_from_user(uaddr, len, &addr) < 0) return -1;
    sock->bound_port = ntohs(addr.sin_port);
    kprintf("[SOCKET] bind fd%d port=%u\n", fd, sock->bound_port);
    return 0;
}

static i64 sys_connect_user(int fd, u64 uaddr, u32 len) {
    file_t *f = fd_get(fd);
    if (!f || f->fops != &socket_fops) return -1;
    yam_socket_file_t *sock = (yam_socket_file_t *)f->private_data;
    sockaddr_in_t addr;
    if (!sock || sock->type != SOCK_STREAM || copy_sockaddr_in_from_user(uaddr, len, &addr) < 0) return -1;
    u32 ip = ntohl(addr.sin_addr.s_addr);
    u16 port = ntohs(addr.sin_port);
    int rc = tcp_connect(sock->tcp_fd, ip, port);
    kprintf("[SOCKET] connect fd%d -> %u.%u.%u.%u:%u rc=%d\n",
            fd, (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port, rc);
    return rc;
}

static i64 sys_listen_user(int fd, int backlog) {
    (void)backlog;
    file_t *f = fd_get(fd);
    if (!f || f->fops != &socket_fops) return -1;
    yam_socket_file_t *sock = (yam_socket_file_t *)f->private_data;
    if (!sock || sock->type != SOCK_STREAM || sock->bound_port == 0) return -1;
    int rc = tcp_listen(sock->tcp_fd, sock->bound_port);
    if (rc == 0) sock->listening = true;
    kprintf("[SOCKET] listen fd%d port=%u rc=%d\n", fd, sock->bound_port, rc);
    return rc;
}

static i64 sys_accept_user(int fd, u64 uaddr, u64 ulen) {
    (void)uaddr;
    (void)ulen;
    file_t *f = fd_get(fd);
    if (!f || f->fops != &socket_fops) return -1;
    yam_socket_file_t *sock = (yam_socket_file_t *)f->private_data;
    if (!sock || !sock->listening) return -1;
    int client_tcp = tcp_accept(sock->tcp_fd);
    if (client_tcp < 0) return client_tcp; /* propagate -EAGAIN */
    if (sock->nonblocking) tcp_set_nonblock(client_tcp, true);
    int client_fd = socket_wrap_tcp_fd(client_tcp, sock->domain, sock->type, sock->proto, 0, false);
    kprintf("[SOCKET] accept fd%d -> fd%d tcp%d\n", fd, client_fd, client_tcp);
    return client_fd;
}

/* ---- fcntl: get/set file descriptor flags ---- */
static i64 sys_fcntl_user(int fd, int cmd, u64 arg) {
    file_t *f = fd_get(fd);
    if (!f) return -1;
    if (cmd == YAM_F_GETFD) {
        return fd_get_flags(fd);
    }
    if (cmd == YAM_F_SETFD) {
        return fd_set_flags(fd, (u32)(arg & YAM_FD_CLOEXEC));
    }
    if (cmd == F_GETFL) {
        int flags = 0;
        if (f->fops == &socket_fops) {
            yam_socket_file_t *sock = (yam_socket_file_t *)f->private_data;
            if (sock && sock->tcp_fd >= 0 && tcp_is_nonblock(sock->tcp_fd))
                flags |= O_NONBLOCK;
        }
        return flags;
    }
    if (cmd == F_SETFL) {
        if (f->fops == &socket_fops) {
            yam_socket_file_t *sock = (yam_socket_file_t *)f->private_data;
            if (sock && sock->tcp_fd >= 0) {
                bool nonblock = (arg & O_NONBLOCK) != 0;
                tcp_set_nonblock(sock->tcp_fd, nonblock);
                sock->nonblocking = nonblock;
                kprintf("[SOCKET] fcntl fd%d O_NONBLOCK=%d\n", fd, nonblock ? 1 : 0);
            }
        }
        return 0;
    }
    return -1;
}

/* ---- select: wait for multiple file descriptors ---- */
typedef u64 yam_fd_set_word_t;
#define YAM_FD_SETWORDS 2
typedef struct { yam_fd_set_word_t bits[YAM_FD_SETWORDS]; } yam_fd_set_t;

static inline bool yfds_isset(const yam_fd_set_t *s, int fd) {
    if (fd < 0 || fd >= 128) return false;
    return (s->bits[fd / 64] >> (fd % 64)) & 1;
}
static inline void yfds_set(yam_fd_set_t *s, int fd) {
    if (fd >= 0 && fd < 128) s->bits[fd / 64] |= (1ULL << (fd % 64));
}

static i64 sys_select_user(int nfds, u64 readfds_u, u64 writefds_u, i64 timeout_ms) {
    if (nfds <= 0 || nfds > 128) return -1;
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    yam_fd_set_t rfds_k, wfds_k;
    memset(&rfds_k, 0, sizeof(rfds_k));
    memset(&wfds_k, 0, sizeof(wfds_k));
    if (readfds_u) {
        if (smap) __asm__ volatile ("stac");
        memcpy(&rfds_k, (const void *)readfds_u, sizeof(yam_fd_set_t));
        if (smap) __asm__ volatile ("clac");
    }
    if (writefds_u) {
        if (smap) __asm__ volatile ("stac");
        memcpy(&wfds_k, (const void *)writefds_u, sizeof(yam_fd_set_t));
        if (smap) __asm__ volatile ("clac");
    }
    pollfd_t pfds[128];
    int pcount = 0;
    int fd_to_poll[128];
    for (int i = 0; i < nfds; i++) {
        bool want_r = yfds_isset(&rfds_k, i);
        bool want_w = yfds_isset(&wfds_k, i);
        if (!want_r && !want_w) continue;
        pfds[pcount].fd = i;
        pfds[pcount].events = 0;
        pfds[pcount].revents = 0;
        if (want_r) pfds[pcount].events |= POLLIN;
        if (want_w) pfds[pcount].events |= POLLOUT;
        fd_to_poll[pcount] = i;
        pcount++;
    }
    if (pcount == 0) return 0;
    int ready = sys_poll(pfds, (u32)pcount, timeout_ms);
    if (readfds_u) {
        if (smap) __asm__ volatile ("stac");
        yam_fd_set_t *rout = (yam_fd_set_t *)readfds_u;
        memset(rout, 0, sizeof(yam_fd_set_t));
        for (int i = 0; i < pcount; i++)
            if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP))
                yfds_set(rout, fd_to_poll[i]);
        if (smap) __asm__ volatile ("clac");
    }
    if (writefds_u) {
        if (smap) __asm__ volatile ("stac");
        yam_fd_set_t *wout = (yam_fd_set_t *)writefds_u;
        memset(wout, 0, sizeof(yam_fd_set_t));
        for (int i = 0; i < pcount; i++)
            if (pfds[i].revents & POLLOUT)
                yfds_set(wout, fd_to_poll[i]);
        if (smap) __asm__ volatile ("clac");
    }
    return ready;
}

/* ---- Wayland / GUI Syscall Handlers ---- */

static i64 sys_wl_create_surface(u64 utitle, i32 x, i32 y, u32 w, u32 h) {
    task_t *t = this_cpu()->current;
    char ktitle[WL_TITLE_MAX];
    ktitle[0] = '\0';
    ktitle[WL_TITLE_MAX - 1] = '\0';
    if (utitle) {
        bool smap = (read_cr4() & CR4_SMAP) != 0;
        if (smap) __asm__ volatile ("stac");
        strncpy(ktitle, (const char *)utitle, WL_TITLE_MAX - 1);
        if (smap) __asm__ volatile ("clac");
    } else {
        strncpy(ktitle, "(untitled)", WL_TITLE_MAX - 1);
    }
    wl_surface_t *s = wl_surface_create(ktitle, x, y, w, h, t->id);
    if (!s) return -1;
    return (i64)s->id;
}

static i64 sys_wl_map_buffer(u32 surface_id, u64 uvaddr) {
    task_t *t = this_cpu()->current;
    u64 *task_pml4 = syscall_current_pml4();
    if (!task_pml4) return -2;
    wl_compositor_t *comp = wl_get_compositor();
    wl_surface_t *s = NULL;
    for (int i = 0; i < WL_MAX_SURFACES; i++) {
        if (comp->surfaces[i].state != WL_SURFACE_FREE && comp->surfaces[i].id == surface_id) {
            s = &comp->surfaces[i]; break;
        }
    }
    if (!s) {
        for (int i = WL_MAX_SURFACES - 1; i >= 0; i--) {
            if (comp->surfaces[i].state != WL_SURFACE_FREE &&
                comp->surfaces[i].owner_task_id == t->id) {
                s = &comp->surfaces[i];
                kprintf("[WL_MAP] task '%s' id=%lu surface %u not found; using owned surface %u\n",
                        t->name, t->id, surface_id, s->id);
                break;
            }
        }
        if (!s) {
            kprintf("[WL_MAP] task '%s' id=%lu surface %u not found\n", t->name, t->id, surface_id);
            return -3;
        }
    }
    if (s->owner_task_id != t->id) {
        kprintf("[WL_MAP] task '%s' id=%lu surface %u owner=%lu mismatch\n",
                t->name, t->id, surface_id, s->owner_task_id);
        return -4;
    }
    if (!s->buffer || !s->buffer->pixels) {
        kprintf("[WL_MAP] task '%s' id=%lu surface %u missing buffer\n", t->name, t->id, surface_id);
        return -5;
    }

    u64 phys_base = vmm_virt_hhdm_to_phys(s->buffer->pixels);
    u64 size = s->buffer->size;
    u64 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (u64 i = 0; i < pages; i++) {
        if (!vmm_map_page(task_pml4, uvaddr + i * PAGE_SIZE, phys_base + i * PAGE_SIZE,
                          VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITE | VMM_FLAG_NX | VMM_FLAG_DONT_FREE)) {
            kprintf("[WL_MAP] task '%s' id=%lu map failed page=%lu/%lu va=0x%lx phys=0x%lx\n",
                    t->name, t->id, i, pages, uvaddr + i * PAGE_SIZE, phys_base + i * PAGE_SIZE);
            return -6;
        }
    }
    kprintf("[WL_DBG] map-ok task='%s' tid=%lu surface=%u va=0x%lx phys=0x%lx pages=%lu bytes=%lu\n",
            t->name, t->id, surface_id, uvaddr, phys_base, pages, size);
    return 0;
}

static i64 sys_wl_commit(u32 surface_id) {
    task_t *t = this_cpu()->current;
    static u32 last_missing_surface = 0;
    static u32 missing_repeat = 0;
    wl_compositor_t *comp = wl_get_compositor();
    wl_surface_t *owned_fallback = NULL;
    for (int i = 0; i < WL_MAX_SURFACES; i++) {
        if (comp->surfaces[i].state != WL_SURFACE_FREE && comp->surfaces[i].id == surface_id) {
            if (comp->surfaces[i].owner_task_id != t->id) {
                kprintf("[WL_DBG] commit-deny task='%s' tid=%lu surface=%u owner=%lu\n",
                        t->name, t->id, surface_id, comp->surfaces[i].owner_task_id);
                return -2;
            }
            wl_surface_commit(&comp->surfaces[i]);
            return 0;
        }
        if (comp->surfaces[i].state != WL_SURFACE_FREE &&
            comp->surfaces[i].owner_task_id == t->id) {
            owned_fallback = &comp->surfaces[i];
        }
    }
    if (owned_fallback) {
        kprintf("[WL_DBG] commit surface %u not found for task='%s'; using owned surface %u\n",
                surface_id, t->name, owned_fallback->id);
        wl_surface_commit(owned_fallback);
        return 0;
    }
    if (last_missing_surface != surface_id) {
        last_missing_surface = surface_id;
        missing_repeat = 0;
    }
    missing_repeat++;
    if (missing_repeat <= 3 || (missing_repeat % 128) == 0) {
        kprintf("[WL_DBG] commit-missing task='%s' tid=%lu surface=%u repeat=%u\n",
                t->name, t->id, surface_id, missing_repeat);
    }
    return -1;
}

static i64 sys_wl_poll_event(u32 surface_id, u64 uev_ptr) {
    task_t *t = this_cpu()->current;
    wl_compositor_t *comp = wl_get_compositor();
    wl_surface_t *s = NULL;
    for (int i = 0; i < WL_MAX_SURFACES; i++) {
        if (comp->surfaces[i].state != WL_SURFACE_FREE && comp->surfaces[i].id == surface_id) {
            s = &comp->surfaces[i]; break;
        }
    }
    if (!s || s->owner_task_id != t->id) return -1;

    input_event_t ev;
    if (wl_surface_pop_event(s, &ev)) {
        bool smap = (read_cr4() & CR4_SMAP) != 0;
        if (smap) __asm__ volatile ("stac");
        memcpy((void *)uev_ptr, &ev, sizeof(input_event_t));
        if (smap) __asm__ volatile ("clac");
        return 1;
    }
    return 0;
}

/* ---- Privileged Driver Syscalls ---- */
static i64 sys_ioport_read(u16 port, u8 width) {
    if (width == 1) return (i64)inb(port);
    if (width == 2) return (i64)inw(port);
    if (width == 4) return (i64)inl(port);
    return -1;
}

static i64 sys_ioport_write(u16 port, u8 width, u64 value) {
    if (width == 1) outb(port, (u8)value);
    else if (width == 2) outw(port, (u16)value);
    else if (width == 4) outl(port, (u32)value);
    return 0;
}

static i64 sys_pci_config_read(u8 bus, u8 slot, u8 func, u8 offset, u8 width) {
    if (width == 4) return (i64)pci_read_32(bus, slot, func, offset);
    if (width == 2) return (i64)pci_read_16(bus, slot, func, offset);
    if (width == 1) return (i64)pci_read_8(bus, slot, func, offset);
    return -1;
}

static i64 sys_map_mmio(u64 phys_addr, u64 virt_addr, u64 size) {
    task_t *t = this_cpu()->current;
    if (!t->pml4) return -1;
    u64 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (u64 i = 0; i < pages; i++) {
        vmm_map_page(t->pml4, virt_addr + i * PAGE_SIZE, phys_addr + i * PAGE_SIZE,
                     VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITE | VMM_FLAG_NX | VMM_FLAG_NOCACHE);
    }
    return 0;
}

/* ---- AI Syscalls ---- */
static i64 sys_ai_device_query(void) { return (i64)ai_device_count(); }
static i64 sys_ai_tensor_alloc_handler(u32 ndim, u64 shape_ptr, u32 dtype) {
    /* Simplified: read shape from user space */
    u32 shape[8] = {0};
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    if (ndim > 8) ndim = 8;
    for (u32 i = 0; i < ndim; i++) shape[i] = ((u32 *)shape_ptr)[i];
    if (smap) __asm__ volatile ("clac");
    return (i64)ai_tensor_alloc(ndim, shape, (tensor_dtype_t)dtype);
}

/* ---- YamGraph IPC Syscalls ---- */
static i64 sys_channel_send(u32 node_id, u32 msg_type, u64 udata, u32 length) {
    task_t *t = this_cpu()->current;
    yam_channel_t *chan = channel_get(node_id);
    if (!chan) return -1;
    
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    bool ret = channel_send(chan, t->graph_node, msg_type, (const void *)udata, length);
    if (smap) __asm__ volatile ("clac");
    
    return ret ? 0 : -2;
}

static i64 sys_channel_recv(u32 node_id, u64 uout) {
    task_t *t = this_cpu()->current;
    yam_channel_t *chan = channel_get(node_id);
    if (!chan) return -1;
    
    yam_message_t msg;
    if (!channel_recv(chan, t->graph_node, &msg)) return -2;
    
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    memcpy((void *)uout, &msg, sizeof(yam_message_t));
    if (smap) __asm__ volatile ("clac");
    
    return 0;
}

static i64 sys_channel_lookup(u64 uname) {
    bool smap = (read_cr4() & CR4_SMAP) != 0;
    if (smap) __asm__ volatile ("stac");
    const char *name = (const char *)uname;
    yam_node_id_t id = yamgraph_find_node_by_name(name);
    if (smap) __asm__ volatile ("clac");
    return (i64)id;
}

/* ---- Threads & Signals ---- */

/*
 * User stacks for ELF tasks live in src/fs/elf.c (high canonical region). Threads may use
 * other mappings in the lower canonical half — accept any plausible ring-3 pointer.
 */
static bool user_rsp_plausible(u64 rsp) {
    if (rsp < 0x1000ULL || (rsp & 7ULL) != 0)
        return false;
    /* Lower canonical half (positive canonical user addresses). */
    return rsp < 0x0000800000000000ULL;
}

/*
 * deliver_pending_signals — called just before returning to user space (syscall path).
 * For a caught signal: push saved user RIP onto the user stack so the handler's RET resumes
 * the interrupted syscall/instruction; point user RIP at the handler and pass signum in RDI
 * (Sys V amd64). Patches syscall_entry's saved RCX/RDI/user_rsp slots read by sysret.
 */
static void deliver_pending_signals(task_t *t, u64 *user_rip_ptr, u64 *user_rflags_ptr) {
    (void)user_rip_ptr;
    (void)user_rflags_ptr;
    if (!t || !t->signal_pending)
        return;

    percpu_t *pc = this_cpu();
    u64 *rip_slot = (u64 *)pc->syscall_rip_slot;
    u64 *rsp_slot = (u64 *)pc->syscall_rsp_slot;
    u64 *rdi_slot = (u64 *)pc->syscall_rdi_slot;
    if (!rip_slot || !rsp_slot || !rdi_slot)
        return;

    for (int sig = 1; sig < 32; sig++) {
        u32 bit = 1u << sig;
        if (!(t->signal_pending & bit))
            continue;
        if (t->sig_mask & bit)
            continue; /* blocked */
        t->signal_pending &= ~bit;
        void *handler = t->sig_handlers[sig];
        if (!handler) {
            /* Default action: terminate for most signals */
            if (sig != 17 && sig != 18 && sig != 20) { /* not SIGCHLD/SIGCONT/SIGWINCH */
                kprintf_color(0xFFFF3333, "[SYS] signal %d default: kill task %lu\n", sig, t->id);
                sched_exit_current(128 + sig);
            }
            continue;
        }

        u64 ursp = *rsp_slot;
        u64 urip = *rip_slot;
        u64 new_rsp = ursp - sizeof(u64);
        if (!user_rsp_plausible(new_rsp)) {
            kprintf_color(0xFFFFAA00,
                          "[SYS] signal %d delivery skipped: bad user rsp=%lx task=%lu\n",
                          sig, ursp, t->id);
            continue;
        }

        /* Resume address after handler RET */
        if (copy_to_user(new_rsp, &urip, sizeof(u64)) != 0) {
            kprintf_color(0xFFFFAA00,
                          "[SYS] signal %d delivery skipped: user stack fault rsp=%lx task=%lu\n",
                          sig, new_rsp, t->id);
            continue;
        }

        *rip_slot = (u64)handler;
        *rsp_slot = new_rsp;
        *rdi_slot = (u64)sig;
        return;
    }
}

static i64 sys_thread_create(u64 entry, u64 stack_top, u64 arg, u64 tls_base) {
    task_t *t = sched_thread_create(NULL, entry, stack_top, arg, tls_base);
    if (!t) return -1;
    kprintf_color(0xFF88FFAA, "[THREAD] created tid=%lu entry=%lx tls=%lx\n",
                  t->id, entry, tls_base);
    return (i64)t->id;
}

static i64 sys_thread_exit(u64 code) {
    task_t *t = this_cpu()->current;
    kprintf_color(0xFFFF8833, "[THREAD] tid=%lu exit(%lu)\n", t->id, code);
    sched_exit_current((i32)code);
    for (;;) {
        sched_yield();
        __asm__ volatile ("hlt");
    }
    __builtin_unreachable();
}

static i64 sys_sigaction(int signum, u64 uact, u64 uoldact) {
    if (signum < 1 || signum >= 32) return -1;
    /* SIGKILL (9) and SIGSTOP (17) cannot be caught */
    if (signum == 9 || signum == 17) return -1;
    task_t *t = this_cpu()->current;
    bool smap = (read_cr4() & CR4_SMAP) != 0;

    if (uoldact) {
        yam_sigaction_t old;
        memset(&old, 0, sizeof(old));
        old.sa_handler = (u64)t->sig_handlers[signum];
        old.sa_mask    = t->sig_mask;
        if (smap) __asm__ volatile ("stac");
        memcpy((void *)uoldact, &old, sizeof(old));
        if (smap) __asm__ volatile ("clac");
    }

    if (uact) {
        yam_sigaction_t act;
        if (smap) __asm__ volatile ("stac");
        memcpy(&act, (const void *)uact, sizeof(act));
        if (smap) __asm__ volatile ("clac");
        /* SIG_DFL=0, SIG_IGN=1 handled as NULL/ignored */
        t->sig_handlers[signum] = (act.sa_handler <= 1) ? NULL : (void *)act.sa_handler;
        /* merge sa_mask into thread blocked mask on signal delivery (not tracking per-handler mask yet) */
    }

    return 0;
}

static i64 sys_sigprocmask(int how, u64 uset, u64 uoldset) {
    task_t *t = this_cpu()->current;
    bool smap = (read_cr4() & CR4_SMAP) != 0;

    if (uoldset) {
        u64 old = t->sig_mask;
        if (smap) __asm__ volatile ("stac");
        memcpy((void *)uoldset, &old, sizeof(old));
        if (smap) __asm__ volatile ("clac");
    }

    if (uset) {
        u64 set;
        if (smap) __asm__ volatile ("stac");
        memcpy(&set, (const void *)uset, sizeof(set));
        if (smap) __asm__ volatile ("clac");
        /* Cannot block SIGKILL or SIGSTOP */
        set &= ~((1u << 9) | (1u << 17));
        if (how == 0 /* SIG_BLOCK */)   t->sig_mask |= set;
        else if (how == 1 /* SIG_UNBLOCK */) t->sig_mask &= ~set;
        else if (how == 2 /* SIG_SETMASK */) t->sig_mask = set;
        else return -1;
    }

    return 0;
}

static i64 sys_sigreturn(void) {
    /*
     * Full POSIX rt_sigreturn/ucontext restore is not wired yet. Delivery uses one pushed
     * resume address so the handler RET resumes after the interrupted syscall.
     */
    task_t *t = this_cpu()->current;
    /* Re-enable any signals that were masked during handler */
    deliver_pending_signals(t, NULL, NULL);
    return 0;
}

static void syscall_restore_current_address_space(void) {
    u64 *pml4 = syscall_current_pml4();
    u64 phys = vmm_virt_hhdm_to_phys(pml4);
    __asm__ volatile("mov %0, %%cr3" :: "r"(phys) : "memory");
}

/* ---- Dispatch ---- */
i64 syscall_dispatch(u64 nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
#define SYSCALL_RETURN(expr) do { \
        i64 __ret = (i64)(expr); \
        syscall_restore_current_address_space(); \
        deliver_pending_signals(this_cpu()->current, NULL, NULL); \
        return __ret; \
    } while (0)

    switch (nr) {
    case SYS_WRITE:    SYSCALL_RETURN(sys_write_serial(a1, a2, a3));
    case SYS_EXIT:     SYSCALL_RETURN(sys_exit(a1));
    case SYS_GETPID:   SYSCALL_RETURN(sys_getpid());
    case SYS_YIELD:    SYSCALL_RETURN(sys_yield());
    case SYS_SLEEPMS:  SYSCALL_RETURN(sys_sleepms(a1));
    case SYS_OPEN:     SYSCALL_RETURN(sys_open_user(a1, (u32)a2));
    case SYS_CLOSE:    SYSCALL_RETURN(sys_close((int)a1));
    case SYS_READ:     SYSCALL_RETURN(sys_read((int)a1, (void *)a2, (usize)a3));
    case SYS_WRITE_FD: SYSCALL_RETURN(sys_write((int)a1, (const void *)a2, (usize)a3));
    case SYS_MMAP:     SYSCALL_RETURN((u64)sys_mmap((void *)a1, (usize)a2, (u32)a3, (u32)a4, (int)a5, 0));
    case SYS_MUNMAP:   SYSCALL_RETURN(sys_munmap((void *)a1, (usize)a2));
    case SYS_PIPE:     SYSCALL_RETURN(sys_pipe((int *)a1));
    case SYS_POLL:     SYSCALL_RETURN(sys_poll((pollfd_t *)a1, (u32)a2, (i64)a3));

    /* v0.3.0 process management */
    case SYS_FORK:          SYSCALL_RETURN(sys_fork(this_cpu()->syscall_resume_rsp));
    case SYS_WAITPID:       SYSCALL_RETURN(sys_waitpid_user((i64)a1, a2, (u32)a3));
    case SYS_BRK:           SYSCALL_RETURN(sys_brk(a1));
    case SYS_MPROTECT:      SYSCALL_RETURN(sys_mprotect((void *)a1, (usize)a2, (u32)a3));
    case SYS_CLOCK_GETTIME: SYSCALL_RETURN(sys_clock_gettime());
    case SYS_GETRUSAGE:     SYSCALL_RETURN(sys_getrusage(a1));
    case SYS_GETPPID:       SYSCALL_RETURN(sys_getppid());
    case SYS_DUP:           SYSCALL_RETURN(sys_dup((int)a1));
    case SYS_DUP2:          SYSCALL_RETURN(sys_dup2((int)a1, (int)a2));
    case SYS_KILL:          SYSCALL_RETURN(sys_kill(a1, (u32)a2));
    case SYS_SCHED_SETAFFINITY: SYSCALL_RETURN(sys_sched_setaffinity(a1));
    case SYS_SCHED_GETAFFINITY: SYSCALL_RETURN(sys_sched_getaffinity());
    case SYS_SCHED_INFO:        SYSCALL_RETURN(sys_sched_info(a1));
    case SYS_FUTEX:         SYSCALL_RETURN(sys_futex((u32 *)a1, (int)a2, (u32)a3, a4));
    case SYS_CLIPBOARD_SET: SYSCALL_RETURN(sys_clipboard_set(a1, (u32)a2));
    case SYS_CLIPBOARD_GET: SYSCALL_RETURN(sys_clipboard_get(a1, (u32)a2));
    case SYS_INSTALLER_STATUS:  SYSCALL_RETURN(sys_installer_status(a1, a2));
    case SYS_INSTALLER_REQUEST: SYSCALL_RETURN(sys_installer_request(a1, a2));
    case SYS_OS_INFO:           SYSCALL_RETURN(sys_os_info(a1));
    case SYS_APP_REGISTER:      SYSCALL_RETURN(sys_app_register(a1));
    case SYS_APP_QUERY:         SYSCALL_RETURN(sys_app_query((u32)a1, a2));
    case SYS_SOCKET:            SYSCALL_RETURN(sys_socket_user((int)a1, (int)a2, (int)a3));
    case SYS_BIND:              SYSCALL_RETURN(sys_bind_user((int)a1, a2, (u32)a3));
    case SYS_CONNECT:           SYSCALL_RETURN(sys_connect_user((int)a1, a2, (u32)a3));
    case SYS_LISTEN:            SYSCALL_RETURN(sys_listen_user((int)a1, (int)a2));
    case SYS_ACCEPT:            SYSCALL_RETURN(sys_accept_user((int)a1, a2, a3));
    case SYS_SENDTO:            SYSCALL_RETURN(-1);
    case SYS_RECVFROM:          SYSCALL_RETURN(-1);
    case SYS_SPAWN:             SYSCALL_RETURN(sys_spawn_user(a1, a2, a3));
    case SYS_EXECVE:            SYSCALL_RETURN(sys_execve_user(a1, a2, a3));
    case SYS_STAT:              SYSCALL_RETURN(sys_stat_user(a1, a2));
    case SYS_FSTAT:             SYSCALL_RETURN(sys_fstat_user((int)a1, a2));
    case SYS_FTRUNCATE:         SYSCALL_RETURN(sys_ftruncate((int)a1, (isize)a2));
    case SYS_RENAME:            SYSCALL_RETURN(sys_rename_user(a1, a2));
    case SYS_OPENAT:            SYSCALL_RETURN(sys_openat_user((int)a1, a2, (u32)a3));
    case SYS_FSTATAT:           SYSCALL_RETURN(sys_fstatat_user((int)a1, a2, a3, (int)a4));
    case SYS_MKDIRAT:           SYSCALL_RETURN(sys_mkdirat_user((int)a1, a2, (u32)a3));
    case SYS_UNLINKAT:          SYSCALL_RETURN(sys_unlinkat_user((int)a1, a2, (int)a3));
    case SYS_RENAMEAT:          SYSCALL_RETURN(sys_renameat_user((int)a1, a2, (int)a3, a4));
    /* Phase 3: Non-blocking I/O */
    case SYS_SELECT:            SYSCALL_RETURN(sys_select_user((int)a1, a2, a3, (i64)a4));
    case SYS_FCNTL:             SYSCALL_RETURN(sys_fcntl_user((int)a1, (int)a2, a3));

    /* Wayland */
    case SYS_WL_CREATE_SURFACE: SYSCALL_RETURN(sys_wl_create_surface(a1, (i32)a2, (i32)a3, (u32)a4, (u32)a5));
    case SYS_WL_MAP_BUFFER:     SYSCALL_RETURN(sys_wl_map_buffer((u32)a1, a2));
    case SYS_WL_COMMIT:         SYSCALL_RETURN(sys_wl_commit((u32)a1));
    case SYS_WL_POLL_EVENT:     SYSCALL_RETURN(sys_wl_poll_event((u32)a1, a2));
    case SYS_LSEEK:             SYSCALL_RETURN(sys_lseek((int)a1, (isize)a2, (int)a3));
    case SYS_MKDIR:             SYSCALL_RETURN(sys_mkdir_user(a1, (u32)a2));
    case SYS_UNLINK:            SYSCALL_RETURN(sys_unlink_user(a1));
    case SYS_READDIR:           SYSCALL_RETURN(sys_readdir_user(a1, (u32)a2, a3));
    case SYS_CHDIR:             SYSCALL_RETURN(sys_chdir_user(a1));
    case SYS_GETCWD:            SYSCALL_RETURN(sys_getcwd_user(a1, (usize)a2));

    /* Drivers */
    case SYS_IOPORT_READ:     SYSCALL_RETURN(sys_ioport_read((u16)a1, (u8)a2));
    case SYS_IOPORT_WRITE:    SYSCALL_RETURN(sys_ioport_write((u16)a1, (u8)a2, a3));
    case SYS_PCI_CONFIG_READ: SYSCALL_RETURN(sys_pci_config_read((u8)a1, (u8)a2, (u8)a3, (u8)a4, (u8)a5));
    case SYS_MAP_MMIO:        SYSCALL_RETURN(sys_map_mmio(a1, a2, a3));

    /* AI */
    case SYS_AI_DEVICE_QUERY: SYSCALL_RETURN(sys_ai_device_query());
    case SYS_AI_TENSOR_ALLOC: SYSCALL_RETURN(sys_ai_tensor_alloc_handler((u32)a1, a2, (u32)a3));
    case SYS_AI_TENSOR_FREE:  ai_tensor_free((u32)a1); SYSCALL_RETURN(0);
    case SYS_AI_SUBMIT_JOB:   SYSCALL_RETURN(ai_job_submit((ai_op_type_t)a1, (const u32 *)a2, (u32)a3, (u32)a4, (u8)a5));
    case SYS_AI_WAIT_JOB:     ai_job_wait((u32)a1); SYSCALL_RETURN(0);

    /* YamGraph IPC */
    case SYS_CHANNEL_SEND:    SYSCALL_RETURN(sys_channel_send((u32)a1, (u32)a2, a3, (u32)a4));
    case SYS_CHANNEL_RECV:    SYSCALL_RETURN(sys_channel_recv((u32)a1, a2));
    case SYS_CHANNEL_LOOKUP:  SYSCALL_RETURN(sys_channel_lookup(a1));

    /* Threads & Signals */
    case SYS_THREAD_CREATE:   SYSCALL_RETURN(sys_thread_create(a1, a2, a3, a4));
    case SYS_THREAD_EXIT:     SYSCALL_RETURN(sys_thread_exit(a1));
    case SYS_SIGACTION:       SYSCALL_RETURN(sys_sigaction((int)a1, a2, a3));
    case SYS_SIGPROCMASK:     SYSCALL_RETURN(sys_sigprocmask((int)a1, a2, a3));
    case SYS_SIGRETURN:       SYSCALL_RETURN(sys_sigreturn());

    default: SYSCALL_RETURN(-1);
    }
#undef SYSCALL_RETURN
}
