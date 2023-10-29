//
// File descriptors
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++; // ~ 이게 hard link
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE)
    pipeclose(ff.pipe, ff.writable);
  else if(ff.type == FD_INODE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
int
filestat(struct file *f, struct stat *st)
{
  if(f->type == FD_INODE){
    ilock(f->ip);
    stati(f->ip, st);
    iunlock(f->ip);
    return 0;
  }
  return -1;
}

// Read from file f.
// // fileread랑 filewrite에서도 f->ip->type == T_SYM이면 redirect 해줘야 할듯. 
// * 근데 open에서 어차피 redirect 해주긴하고, 애초에 open 없이 read 쓸 수가 없잖아? 아닌가? 
// * ㅇㅇㅇ 걱정 안해도 될 듯. 애초에 f->ip를 지정해주는게 sys_open밖에 없고, 
// * 그전에 ip->type이 T_SYM이면 redirect된 ip로 갈아탈테니까
// * 그럼 사실 f->ip의 type이 T_SYM인 경우는 없어. 
// * ㅇㅋ 그럼 fileread, filewrite 걱정 안해도 되겠다. 인자를 f로 받으니까
int
fileread(struct file *f, char *addr, int n)
{
  int r;

  if(f->readable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return piperead(f->pipe, addr, n);
  if(f->type == FD_INODE){ // ~ open에서 FD_INODE로 해줌
    ilock(f->ip);
    if((r = readi(f->ip, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
    return r;
  }
  panic("fileread");
}

//PAGEBREAK!
// Write to file f.
int
filewrite(struct file *f, char *addr, int n)
{
  // * f = 대상 file이고
  // * addr = 쓸 내용의 주소
  // * n = 쓸 양

  int r;

  if(f->writable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return pipewrite(f->pipe, addr, n);
  if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * 512;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      //                ip,     src,      off,  n
      if ((r = writei(f->ip, addr + i, f->off, n1)) > 0)
        f->off += r;  // * 아? 이러면 f를 늘려가며 쓰는 느낌인가> ㄴㄴ 늘리는건 size

      // * 일단 그럼 file->off의 의미를 좀 알아야겠다.
      // * off 쓰이는 곳이 writei랑 readi랑 여기밖에 없는듯 ㅅ
      // * 아 대충 file 어디서부터 쓸건지를 의미하나봄 쓸떄 이어서 쓰고 하게
      iunlock(f->ip);
      end_op();

      if(r < 0)
        break;
      if(r != n1)
        panic("short filewrite");
      i += r;
    }
    return i == n ? n : -1;
  }
  panic("filewrite");
}

