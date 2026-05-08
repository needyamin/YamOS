#include "stdio.h"
#include "unistd.h"
#include "../libyam/syscall.h"

int main(int argc, char **argv, char **envp);

void _start(int argc, char **argv, char **envp) {
    int rc = main(argc, argv, envp);
    syscall1(SYS_EXIT, (u64)rc);
    for (;;) {
        syscall0(SYS_YIELD);
    }
}

/*
 * Parent forks, child exits immediately; parent yields many times without waitpid,
 * then exits. When the parent dies first, the zombie child is orphaned and should
 * drive scheduler orphan / deferred-reap counters (verified by init).
 */
int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    pid_t pid = fork();
    if (pid < 0) {
        printf("[ORPHAN_TEST] FAIL fork() returned %ld\n", (long)pid);
        return 1;
    }
    if (pid == 0) {
        printf("[ORPHAN_TEST] child pid=%ld exiting\n", (long)getpid());
        syscall1(SYS_EXIT, 77);
    }

    printf("[ORPHAN_TEST] parent pid=%ld forked child=%ld (no waitpid)\n",
           (long)getpid(), (long)pid);
    for (unsigned i = 0; i < 512; i++) {
        syscall0(SYS_YIELD);
    }
    printf("[ORPHAN_TEST] parent exiting without waitpid\n");
    syscall1(SYS_EXIT, 0);
}
