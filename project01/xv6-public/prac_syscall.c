#include "types.h"
#include "defs.h"

int myfunction(char* str)
{
    cprintf("%s %s\n", str, str);
    return 0xABCD;
}

int sys_myfunction(void) // wrapper function
{
    char* str;

    if (argstr(0, &str) < 0)
        return -1;
    
    return myfunction(str);
}