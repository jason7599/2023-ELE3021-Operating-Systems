#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int main(int argc, char *argv[])
{
    char *filename;
    char *content;
    int fd;

    if (argc != 3)
        exit();

    filename = argv[1];
    content = argv[2];

    if ((fd = open(filename, O_RDWR)) < 0)
    {
        if((fd = open(filename, O_CREATE | O_RDWR)) < 0)
        {
            printf(1, "failed to create file %s\n", filename);
            exit();
        }
    }
    
    if (write(fd, content, strlen(content)) == -1)
        printf(1, "something went wrong!\n");

    close(fd);
    exit();
}