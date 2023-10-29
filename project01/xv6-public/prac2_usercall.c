#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[])
{
    // __asm__("int $129");
    // __asm__("int $130");
    // exit();

    int iter = 4;
    int pid;

    // schedulerLock(2020059152);

    for (int i = 0; i < iter; i++)
    {
        pid = fork();
        if (pid == 0) { // child
            // printf(1, "child :)\n");
            exec("prac_myuserapp", argv);
            // printf(1, "shiibabl\n");
            exit();
        }
        else if (pid > 0) { // parent
            // printf(1, "parent :)\n");
            // wait();
        }
        else { // error
            printf(1, "EROEIREIREIRUOEU");
        }
        sleep(25 * i);
    }

    for (int i = 0; i < iter; i++)
        wait();

    // printf(1, "done :)\n");
    exit();
}