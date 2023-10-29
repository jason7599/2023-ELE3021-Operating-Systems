#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

char buf[8192];

int main(int argc, char *argv[])
{
    int fd, i;

    int bufsize = 10;

    fd = open("file1", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf(1, "error: create file failed\n");
        exit();
    }
    for (i = 0; i < bufsize; i++)
        buf[i] = 'a' + i;
    write(fd, buf, bufsize);
    close(fd);

    fd = open("file2", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf(1, "error: create file failed\n");
        exit();
    }
    for (i = 0; i < bufsize; i++)
        buf[i] = 'b' + i;
    write(fd, buf, bufsize);
    close(fd); 

    fd = open("file3", O_CREATE | O_RDWR);
    if (fd < 0) {
        printf(1, "error: create file failed\n");
        exit();
    }
    for (i = 0; i < bufsize; i++)
        buf[i] = 'c' + i;
    write(fd, buf, bufsize);
    close(fd);

    exit();
}
