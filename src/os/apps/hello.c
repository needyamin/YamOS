#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "unistd.h"
#include "../libyam/app.h"
#include "pthread.h"

static int ptr_is_user_cstr(const char *p) {
    u64 v = (u64)p;
    return p && v >= 0x1000ULL && v < 0x0000800000000000ULL;
}

static void *thread_worker(void *arg) {
    long id = (long)arg;
    printf("[HELLO] Thread %ld running! pid=%ld pthread_self=%ld\n", id, (long)getpid(), (long)pthread_self());
    return (void *)(id * 2);
}

int main(int argc, char **argv, char **envp);

void _start(int argc, char **argv, char **envp) {
    int rc = main(argc, argv, envp);
    syscall1(SYS_EXIT, (u64)rc);
    for (;;) {
        syscall0(SYS_YIELD);
    }
}

int main(int argc, char **argv, char **envp) {
    int exec_test = 0;
    for (char **e = envp; e && *e; e++) {
        if (strcmp(*e, "YAMOS_EXEC_TEST=1") == 0) {
            exec_test = 1;
            break;
        }
    }

    if (exec_test) {
        int failures = 0;
        long expected_pid = (argc > 2) ? atol(argv[2]) : -1;
        const char *expected_cwd = (argc > 3) ? argv[3] : "";
        int keep_fd = (argc > 4) ? atoi(argv[4]) : -1;
        int cloexec_fd = (argc > 5) ? atoi(argv[5]) : -1;
        char cwd[128];
        long pid = (long)getpid();

        printf("[HELLO_EXEC] validating exec handoff pid=%ld expected=%ld\n",
               pid, expected_pid);
        if (pid != expected_pid) {
            printf("[HELLO_EXEC] FAIL: pid changed across exec\n");
            failures++;
        }

        if (!getcwd(cwd, sizeof(cwd))) {
            printf("[HELLO_EXEC] FAIL: getcwd failed after exec\n");
            failures++;
        } else if (strcmp(cwd, expected_cwd) != 0) {
            printf("[HELLO_EXEC] FAIL: cwd='%s' expected='%s'\n",
                   cwd, expected_cwd);
            failures++;
        } else {
            printf("[HELLO_EXEC] cwd survived exec: %s\n", cwd);
        }

        if (keep_fd < 0 || fcntl(keep_fd, F_GETFD) < 0) {
            printf("[HELLO_EXEC] FAIL: inherited fd %d is not open\n", keep_fd);
            failures++;
        } else {
            printf("[HELLO_EXEC] inherited fd %d survived exec\n", keep_fd);
        }

        if (cloexec_fd >= 0 && fcntl(cloexec_fd, F_GETFD) >= 0) {
            printf("[HELLO_EXEC] FAIL: FD_CLOEXEC fd %d survived exec\n",
                   cloexec_fd);
            failures++;
        } else {
            printf("[HELLO_EXEC] FD_CLOEXEC fd %d closed on exec\n", cloexec_fd);
        }

        if (failures) {
            printf("[HELLO_EXEC] FAIL: %d exec contract check(s) failed\n",
                   failures);
            return 10 + failures;
        }
        printf("[HELLO_EXEC] PASS: exec preserved pid/cwd/fds and closed FD_CLOEXEC\n");
        return 0;
    }

    (void)argc;
    (void)argv;
    static const char msg[] = "[HELLO] userspace bootstrap alive\n";
    syscall3(SYS_WRITE, 1, (u64)msg, sizeof(msg) - 1);
    return 0;
}
