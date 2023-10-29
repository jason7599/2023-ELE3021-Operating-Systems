#include "types.h"
#include "stat.h"
#include "user.h"

#define ITER 50000000
#define PW 2020059152

int main(int argc, char *argv[])
{
    // printf(1, "%d Lock\n", argc);

    int lockid = 6;
    int id = getpid();
    // int id = 10;
    int i = 0;

    // printf(1, "%d Lock\n", id);
    // schedulerLock(PW);

    for (; i < 1000000; i++)
    {
        id = getpid();
        getLevel();

        if (i == 2000)
        {
            if (id == lockid)
            schedulerLock(PW + 1);
        }

        if (i == 80000)
        {
            // printf(1, "%d Unlock\n", id);
            if (id == lockid)
            schedulerUnlock(PW);
        }
    }

    // printf(1, "%d Done!\n", id);
    // for (;;);

    exit();
}
