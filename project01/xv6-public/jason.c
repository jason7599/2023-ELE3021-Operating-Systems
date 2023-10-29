#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[])
{
    int iter = 3;
    for (int i = 0; i < iter; i++)
        fork();

    schedulerLock(2020059152);

    while (iter--) wait();

    exit();
}