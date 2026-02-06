
#include <stdio.h>

#include <sys/statvfs.h>
#include <unistd.h>

void run(char const *bin, char **argv) {
    if (fork() == 0) {
        execvp(bin, argv);
    }
}

int main(int argc, char **argv, char **envp) {
    printf("Hello, mlibc!\n");

    for (int i = 0; i < argc; i++) {
        printf("argv[%d]: %s\n", i, argv[i]);
    }

    while (*envp) {
        printf("env: %s\n", *envp);
        envp++;
    }

    run("/usr/bin/echo", (char *[]){"/usr/bin/echo", "Hello,", "coreutils", "world!", NULL});

    return 0;
}
