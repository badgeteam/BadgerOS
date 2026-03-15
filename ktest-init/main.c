
#include <stdio.h>

#include <dirent.h>
#include <sys/statvfs.h>
#include <inttypes.h>
#include <unistd.h>

void run(char **argv) {
    if (fork() == 0) {
        if (execvp(argv[0], argv)) {
            perror("execvp");
        }
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
    
    DIR *dirp = opendir("/");
    struct dirent *dent;
    while ((dent = readdir(dirp))) {
        printf("%s type %02x ino %" PRId64 "\n", dent->d_name, dent->d_type, dent->d_ino);
    }

    run((char *[]){"/usr/bin/bash", NULL});

    return 0;
}
