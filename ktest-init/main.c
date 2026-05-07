
#include <stdio.h>
#include <stdlib.h>

#include <dirent.h>
#include <signal.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

void handler(int signo, siginfo_t *info, void *uctx) {
    char buf[32];
    int  len = snprintf(buf, sizeof(buf), "Handled signal %d\n", signo);
    if (len < 0) {
        return;
    }
    __syscall_fs_write(1, buf, len);
}

void run(char const *const *argv) {
    for (size_t i = 0; argv[i]; i++) {
        printf("argv[%zu] = %s\n", i, argv[i]);
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (execvp(argv[0], (char **)argv)) {
            perror("execvp");
            exit(1);
        }
    } else {
        int   wstatus;
        pid_t res = waitpid(pid, &wstatus, 0);
        if (res > 0) {
            if (WIFEXITED(wstatus)) {
                printf("Exit %d\n", WEXITSTATUS(wstatus));
            } else if (WIFSIGNALED(wstatus)) {
                printf("Signal %d\n", WTERMSIG(wstatus));
            }
        } else {
            perror("waitpid");
        }
    }
}

size_t readline(char *buffer, size_t const buffer_len) {
    size_t i;
    for (i = 0; i < buffer_len; i++) {
        int c;
        do {
            c = fgetc(stdin);
        } while (c == EOF);
        buffer[i] = c;
        if (c == '\r' || c == '\n') {
            break;
        }
    }
    return i;
}

int main(int argc, char **argv, char **envp) {
    setenv("PATH", "/usr/bin:/usr/sbin", 1);

    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags     = 0;
    sa.sa_sigaction = handler;
    if (sigaction(60, &sa, NULL)) {
        perror("sigaction");
    }

    if (fork() == 0) {
        printf("Signalling my parent now\n");
        kill(1, 60);
        return 0;
    }

    run((char const *const[]){"ls", NULL});

    char const   whitespace[] = " \t\r\n";
    size_t const buffer_cap   = 1024;
    char         buffer[buffer_cap + 1];
    size_t const argbuf_cap = 32;
    char const  *argbuf[argbuf_cap + 1];
    while (1) {
        size_t buffer_len = readline(buffer, buffer_cap);
        size_t i          = 0;
        size_t narg       = 0;
        while (i < buffer_len && narg < argbuf_cap) {
            i          += strspn(buffer + i, whitespace);
            size_t len  = strcspn(buffer + i, whitespace);
            if (len) {
                argbuf[narg++]   = buffer + i;
                buffer[i + len]  = 0;
                i               += len + 1;
            }
        }
        argbuf[narg] = NULL;
        if (narg) {
            run(argbuf);
        }
    }

    return 0;
}
