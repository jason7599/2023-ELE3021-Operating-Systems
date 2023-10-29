#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"
#include "fs.h"

int main(int argc, char *argv[])
{
    char data[BSIZE];
    int fd, nblock, i;

    if (argc != 2)
    {
        printf(1, "usage: makehugefile nblock\n");
        exit();
    }

    if ((nblock = atoi(argv[1])) <= 0)
    {
        printf(1, "invalid nblock\n");
        exit();
    }

    if ((fd = open("hugefile", O_CREATE | O_RDWR)) < 0)
    {
        printf(1, "makehugefile fail: open\n");
        exit();
    }

    memset(data, 'a', BSIZE);

    for (i = 0; i < nblock; i++)
    {
        if (write(fd, data, BSIZE) != BSIZE)
        {
            printf(1, "makehugefile fail: write\n");
            exit();
        }
    }
    
    close(fd);
    printf(1, "makehugefile success\n");
    exit();
}