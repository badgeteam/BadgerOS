
#include <stdio.h>

int main(int argc, char **argv) {
  FILE *thing = fopen("/dev/null", "wb");
  fwrite("c", 1, 1, thing);
}
