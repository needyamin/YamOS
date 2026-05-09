#include "stdio.h"
#include "unistd.h"
#include "fcntl.h"
#include "../libyam/syscall.h"

#define EXEC_TEST_FORK_EXEC_LOOPS 6

int main(int argc, char **argv, char **envp);

void _start(int argc, char **argv, char **envp) {
    int rc = main(argc, argv, envp);
    syscall1(SYS_EXIT, (u64)rc);
    for (;;) {
        syscall0(SYS_YIELD);
    }
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    static char *const next_envp[] = {
        "YAMOS_EXEC_TEST=1",
        NULL,
    };
    char pid_arg[24];
    char keep_fd_arg[16];
    char cloexec_fd_arg[16];
    char cwd[128];
    char *next_argv[] = {
        "/bin/hello",
        "from-execve",
        pid_arg,
        cwd,
        keep_fd_arg,
        cloexec_fd_arg,
        NULL,
    };

    printf("[EXEC_TEST] starting pid=%ld\n", (long)getpid());

    printf("[EXEC_TEST] fork/exec: %d loops\n", EXEC_TEST_FORK_EXEC_LOOPS);
    for (unsigned n = 0; n < EXEC_TEST_FORK_EXEC_LOOPS; n++) {
        pid_t fpid = fork();
        if (fpid < 0) {
            printf("[EXEC_TEST] FAIL fork/exec loop %u fork() -> %ld\n", n,
                   (long)fpid);
            return 115;
        }
        if (fpid == 0) {
            char *const hargv[] = { "/bin/hello", NULL };
            char *const henv[] = { NULL };
            if (execve("/bin/hello", hargv, henv) < 0) {
                printf("[EXEC_TEST] FAIL fork/exec loop %u execve child\n", n);
                _exit(116);
            }
            _exit(117);
        }
        int fst = 0;
        pid_t fw = waitpid(fpid, &fst, 0);
        int fcode = (fst >> 8) & 0xFF;
        if (fw != fpid || fcode != 0) {
            printf("[EXEC_TEST] FAIL fork/exec loop %u waitpid=%ld expect=%ld "
                   "status=0x%x exit=%d\n",
                   n, (long)fw, (long)fpid, fst, fcode);
            return 118;
        }
    }
    printf("[EXEC_TEST] fork/exec PASS (%d loops)\n", EXEC_TEST_FORK_EXEC_LOOPS);

    char *const bad_argv[] = { "/bin/does-not-exist", NULL };
    int bad_rc = execve("/bin/does-not-exist", bad_argv, next_envp);
    if (bad_rc >= 0) {
        printf("[EXEC_TEST] FAIL: missing-path exec returned success\n");
        return 120;
    }
    printf("[EXEC_TEST] missing-path exec failed as expected rc=%d\n", bad_rc);

    if (chdir("/tmp") != 0 || !getcwd(cwd, sizeof(cwd))) {
        printf("[EXEC_TEST] FAIL: cwd setup failed\n");
        return 121;
    }

    snprintf(pid_arg, sizeof(pid_arg), "%ld", (long)getpid());

    int keep_fd = open("/bin/hello", O_RDONLY);
    int cloexec_fd = open("/bin/hello", O_RDONLY);
    if (keep_fd < 0 || cloexec_fd < 0) {
        printf("[EXEC_TEST] FAIL: open('/bin/hello') failed keep=%d cloexec=%d\n",
               keep_fd, cloexec_fd);
        return 122;
    }
    if (fcntl(cloexec_fd, F_SETFD, FD_CLOEXEC) != 0) {
        printf("[EXEC_TEST] FAIL: F_SETFD failed on fd %d\n", cloexec_fd);
        return 123;
    }
    int keep_flags = fcntl(keep_fd, F_GETFD);
    int cloexec_flags = fcntl(cloexec_fd, F_GETFD);
    snprintf(keep_fd_arg, sizeof(keep_fd_arg), "%d", keep_fd);
    snprintf(cloexec_fd_arg, sizeof(cloexec_fd_arg), "%d", cloexec_fd);

    printf("[EXEC_TEST] before exec cwd=%s keep_fd=%d flags=0x%x cloexec_fd=%d flags=0x%x\n",
           cwd, keep_fd, keep_flags, cloexec_fd, cloexec_flags);

    printf("[EXEC_TEST] calling execve('/bin/hello')\n");
    execve("/bin/hello", next_argv, next_envp);
    printf("[EXEC_TEST] FAIL: execve('/bin/hello') returned\n");
    return 127;
}
