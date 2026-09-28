
// Copyright (c) meow-me-ow Nyan der Meower

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>



char const *const three[] = {
    "nya",
    "mrr",
    "prr",
    "mew",
};
size_t const three_len = sizeof(three) / (sizeof(char const *const));

char const *const four[] = {
    "purr",
    "mrrp",
    "mrap",
    "meow",
    "maow",
    "nyan",
};
size_t const four_len = sizeof(four) / (sizeof(char const *const));

char const *const five[] = {
    "purrs",
    "mreow",
    "mraow",
};
size_t const five_len = sizeof(five) / (sizeof(char const *const));

int main(int argc, char **argv) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    srand(ts.tv_sec);
    srand(ts.tv_nsec);

    if (argc < 2) {
        char const *pick;
        switch (rand() % 3) {
            default:
            case 0: fputs(three[rand() % three_len], stdout); break;
            case 1: fputs(four[rand() % four_len], stdout); break;
            case 2: fputs(five[rand() % five_len], stdout); break;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            fputc(' ', stdout);
        }
        size_t len = strlen(argv[i]);
        while (len > 0) {
            size_t index = len;
            if (index > 5) {
                index = rand() % 3 + 3;
            }
            switch (index) {
                case 1: fputc('m', stdout); break;
                case 2: fputs("mr", stdout); break;
                case 3: fputs(three[rand() % three_len], stdout); break;
                case 4: fputs(four[rand() % four_len], stdout); break;
                case 5: fputs(five[rand() % five_len], stdout); break;
            }
            len -= index;
        }
    }

    fputc('\n', stdout);
    return 0;
}
