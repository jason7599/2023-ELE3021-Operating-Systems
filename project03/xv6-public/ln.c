#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  char *old, *new;
  int dosymlink = 0;

  // 이게 되네
  if(argc != 4 || ((dosymlink = strcmp(argv[1], "-h")) && strcmp(argv[1], "-s")))
  {
    printf(2, "Usage: ln -h|-s old new\n");
    exit();
  }

  old = argv[2];
  new = argv[3];

  if (dosymlink)
  {
    if (symlink(old, new) < 0)
      printf(2, "symlink %s %s: failed\n", old, new);
  }
  else
  {
    if(link(old, new) < 0)
      printf(2, "link %s %s: failed\n", old, new);
  }

  exit();
}
