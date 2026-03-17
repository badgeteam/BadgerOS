
#include <stdio.h>

#include <dirent.h>
#include <sys/statvfs.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/syscall.h>

void handler(int signo, siginfo_t *info, void *uctx) {
    printf("Handled signal %d\n", signo);
}

void run(char const *const *argv) {
    for (size_t i = 0; argv[i]; i++) {
        printf("argv[%zu] = %s\n", i, argv[i]);
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (execvp(argv[0], (char**)argv)) {
            perror("execvp");
        }
    } else {
        int wstatus;
        if (waitpid(pid, &wstatus, 0)) {
            if (WIFEXITED(wstatus)) {
                printf("Exit %d\n", WEXITSTATUS(wstatus));
            } else if (WIFSIGNALED(wstatus)) {
                printf("Signal %d\n", WTERMSIG(wstatus));
            }
        }
    }
}

void readline(char *buffer, size_t const buffer_len) {
    for (size_t i = 0; i < buffer_len; i++) {
        int c;
        do {
            c = fgetc(stdin);
        } while(c == EOF);
        buffer[i] = c;
        if (c == '\r' || c == '\n') {
            printf("\n");
            break;
        }
        fputc(c, stdout);
        fflush(stdout);
    }
}

int main(int argc, char **argv, char **envp) {
    setenv("PATH", "/usr/bin:/usr/sbin", 1);
    
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_sigaction = handler;
    if (sigaction(60, &sa, NULL)) {
        perror("sigaction");
    }
    
    if (fork() == 0) {
        printf("Signalling my parent now\n");
        kill(1, 60);
        return 0;
    }
    
    char const whitespace[] = " \t\r\n";
    const size_t buffer_len = 1024;
    char buffer[buffer_len];
    const size_t argbuf_len = 32;
    char const *argbuf[argbuf_len+1];
    while (1) {
        readline(buffer, buffer_len);
        size_t i = 0;
        size_t narg = 0;
        while (i < buffer_len && buffer[i] && narg < argbuf_len) {
            i += strspn(buffer + i, whitespace);
            size_t len = strcspn(buffer + i, whitespace);
            if (len) {
                buffer[i + len] = 0;
                argbuf[narg++] = buffer + i;
                i += len + 1;
            }
        }
        argbuf[narg] = NULL;
        if (narg) {
            run(argbuf);
        }
    }
    
    return 0;
}
