#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <sys/wait.h>


int main(int argc, const char* argv[]) {
    assert(argc == 2);
    int nice_arg = 0;
    if (strcmp(argv[1], "lottery") == 0) {
        /* test lottery scheduling */
        nice_arg = 20;
    } else if (strcmp(argv[1], "edf") == 0) {
        /* test edf scheduling */
        nice_arg = 100;
    } else {
        printf("ERROR: wrong arg: %s", argv[1]);
        exit(-1);
    }
    pid_t cpid = fork();
    if (cpid > 0) {
        /* parent process */
        printf("INFO: child process id: %d\n", cpid);
        waitpid(cpid, NULL, 0);
    } else if (cpid == 0) {
        /* child process */
        printf("INFO: change nice to %d\n", nice_arg);
        nice(nice_arg);
        int i=0;
        while (i<100000000) {
            i++;
        }
        printf("INFO: child process exit\n");
        exit(0);
    } else {
        printf("ERROR: fork error\n");
        assert(0);
    }
    return 0;
}