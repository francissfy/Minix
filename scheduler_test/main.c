#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <sys/wait.h>

/* 
 * test for lottery and edf
 */

int main(int argc, const char* argv[]) {
    assert(argc == 2);
    int nice_arg = (int)strtol(argv[1], NULL, 10);
    printf("INFO: change nice to %d\n", nice_arg);

    for(int j = 0; j < 1; j++) {
        pid_t t = fork();
        if (t != 0) {
            nice(nice_arg + 50);
            int i=0;
            while (i<1000000000) {
                i++;
            }
        } else if (t == 0) {
            nice(nice_arg);
            int i=0;
            while (i<1000000000) {
                i++;
            }
        }
    }
    printf("INFO: process exit\n");
    exit(0);
    return 0;
}