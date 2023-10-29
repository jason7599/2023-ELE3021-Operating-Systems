//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0) 
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *curproc = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd] == 0){
      curproc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int
sys_read(void)
{
  struct file *f;
  int n;
  char *p;

  // ~ read 첫번쨰 인자는 int fd인데.
  // ~ argfd로 int -> file 하나보네
  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int
sys_write(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

int
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int
sys_fstat(void)
{
  struct file *f;
  struct stat *st;

  if(argfd(0, 0, &f) < 0 || argptr(1, (void*)&st, sizeof(*st)) < 0)
    return -1;
  return filestat(f, st);
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  // sys_open에서 create("hi", T_FILE, 0, 0) 호출되었다고 해보자

  struct inode *ip, *dp;
  char name[DIRSIZ];

  // dp = 만들 파일의 상위 directory
  if((dp = nameiparent(path, name)) == 0)
    return 0; // ! create은 0이 에러
  ilock(dp);

  // dp에 name을 가진 directory entry 찾기
  if((ip = dirlookup(dp, name, 0)) != 0){ // 이미 있는 경우
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  /*
    * 암튼 핵심적인 절차는:
    *   1. ialloc으로 새로운 inode 할당
    *   2. dirlink로 directory entry 등록.
  */

  // * ialloc이라는 함수도 있네. 이걸로 inode 할당하는구나 
  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major; // 얘네는 아직도 모르겠다
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }
  else if (type ==T_SYM)
  {
    ip->type = T_SYM; // ? 이유는 모르겠지만, 이 함수에 ip->type = type 해주는게 없다
  }

  // dirlink로 dp 아래 ip에 대한 directory entry 넣어주기.
  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

// Create the path new as a link to the same inode as old.
// ~ 기존 hard link
int
sys_link(void)
{
  char name[DIRSIZ], *new, *old;
  struct inode *dp, *ip;

  if(argstr(0, &old) < 0 || argstr(1, &new) < 0)
    return -1;

  // old: 원본 파일 이름.
  // new: 새로 만들 파일 이름

  begin_op();
  if((ip = namei(old)) == 0){ // ~ namei 잘 이해는 안되지만 대충 경로의 inode 반환한다고 생각하면 될듯
    end_op();
    return -1;
  }
  // ~ 그럼 ip는 old file의 inode
  
  // 시뮬레이션 돌려보자:
  // old = "file1"
  // new = "linktest"

  ilock(ip);
  if(ip->type == T_DIR){ // ~ directory의 경우 link 안하나보지?
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++; // ~refcount 같은 느낌. old의 nlink++
  iupdate(ip);
  iunlock(ip);

  // dp = nameiparent("linktest", name) 호출
  // ~ dp = new의 상위 directory
  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);  

  // ~ dirlink: dp 아래 file 만드는 셈
  // * * file을 새로 할당하는게 없네? 그냥 meta data (directory entry)만 새로 만드는듯?
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

int
sys_symlink(void)
{
  /*
   new의 ip를 old의 ip로 맞춰주면 될거같음. filewrite이나 fileread에서 결국 인자 f의 ip를 참조해서 가니까
   이러려면 일단 new를 위해 file을 새로 할당해주긴 해야겠음.
   대신 create으로 새로운 ip를 만드는게 아니라 namei로 old의 ip를 가져오면 되지 않을까
   
   아닌가.. file이 꼭 필요한게 맞나? sys_link 보면 file 할당하는건 없고 directory entry만 넣어주는데
   지금 해보니까 create만 해도 ls찍을때 나오긴 한다. 
  */

  /*
  * filealloc이 실제 파일을 생성하는 느낌이 전혀 아니다!
  * 오히려 create로 inode 새로 할당하고 dirlink로 directory entry를 만듦으로서 파일이 생기는 거네.
  * filealloc이랑 fdalloc은 오히려 프로세스가 어떤 파일을 쓸 때 임시적으로 사용되는 자료구조에 가까운 듯 하다.
  * 즉 이 함수에서 file 구조체는 아예 안 써도 될듯 함.
  */

  /*
    inode 구조체에 symog 이라는 inode 포인터 둬서 하는 방식으로 해봤었음.
    근데 이게 되긴 하는데, 원본 rm 해도 바로가기에 내용이 남아있고 막 그랬어.
    내가 보기엔 ref 처리 제대로 안해서 이렇게 되는거 같음. ref 어떻게 처리할지도 문제임.
    바로가기를 열었으면 바로가기의 ref는? 원본의 ref는? 바로가기의 ref를 0으로 하면 또 ilock이 안돼.
  */

  /*
  * 생각을 해보니, 정말 단순한 방법이 있음. 원본 inode 포인터로 갈게 아니라, search path, 즉 원본의 경로를 저장해두면 어떰?
  *
  * 그럼 open할 때 type이 T_SYM이면 이 경로를 읽고, 이걸로 또 namei하는 방식으로. 이러면 좋은게 원본 지워졌을 경우 진짜 없는 파일이라고 처리되겠지.
  * 포인터보다 훨씬 간단하고. 그럼 관건은 이 경로 (사실상 그냥 원본 이름이겠지.)를 inode 어디에 저장하냐인데, 원랜 inode에 char*(혹은 char[]) 로 
  * 원본의 이름 저장하는거 두려했음. 근데 이게 상당히 불편한게, 기존 inode은 커녕 file 구조체에도 이름 정보는 안 담잖아. namei 써서 directory entry
  * 들의 이름에서 찾는거니까 inode에 두는건 좀 기존과 너무 틀어지는 거 같다
  *
  * 그래서, 걍 inode 구조체에 뭐 더 추가하지 말고 그냥 inode의 주소공간 (addrs)에 저장하기로 함. 이게 너무 편한게, addrs에 쓴다는게 그냥 기존의
  * write 로직으로 하면 됨. 다만 filewrite은 인자로 file을 받기 때문에 안되고, filewrite에서 호출하는, inode대상으로 하는 writei를 써야됨.
  */

  char *old, *new;
  struct inode *ip;
  int pathlen;

  if (argstr(0, &old) < 0 || argstr(1, &new) < 0)
    return -1;

  begin_op();

  if (namei(old) == 0) // 원본 파일이 없는 경우.
  {
    end_op();
    return -1;
  }

  if ((ip = create(new, T_SYM, 0, 0)) == 0)
  {
    end_op();
    return -1;
  }

  pathlen = strlen(old) + 1; // null문자까지

  if (writei(ip, old, 0, pathlen) != pathlen)
    panic("symlink: writei");

  iupdate(ip);
  iunlock(ip);
  end_op();

  return 0;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

//PAGEBREAK!
int
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], *path;
  uint off;

  if(argstr(0, &path) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

int
sys_open(void) // ~ cat 흐름을 생각해보자. cat에서 일단 open 호출. path는 단순히 파일 이름, omode = 0: readonly
{
  char *path;
  char sympath[DIRSIZ];
  int fd, omode;
  struct file *f;
  struct inode *ip;

  if(argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  // & open O_CREATE로 파일 생성하는 과정 생각해보자
  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0); // & create로 ip를 새로 만들긴 해야하네
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){ // ~ create 아닌 경우는 path로 ip 찾는거.
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY && omode != O_STAT)
    {
      iunlockput(ip);
      end_op();
      return -1;
    }
    // * 이젠 반대로 readi로 원본 경로 읽어오고, 이에 대해 namei해야됨
    // ! ls으로 호출되는 stat 에서는 이걸 하면 안된다. 
    if (ip->type == T_SYM && omode != O_STAT) 
    {
      if (readi(ip, sympath, 0, ip->size) != ip->size)
        panic("symlink open: readi");

      iunlock(ip); // ip 갈아타기 전에 unlock
      if ((ip = namei(sympath)) == 0)
      {
        end_op();
        return -1;
      }
      ilock(ip);
    }
  }

  // ~ O_CREATE로 파일을 실제로 생성하는거 아니어도 filealloc이랑 fdalloc을 하네.
  // & 보면 볼수록 filealloc이 실제로 파일을 생성하는게 아닌거같다. 오히려 위에 create함수가 핵심인듯.

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){ // ~ file을 할당? filealloc에서 ref = 1 해줌
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlock(ip);
  end_op();

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0; // ? write할 때 off 참조해야되는거 아닌가? 무작정 0으로 해도 되나보지?
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

int
sys_mkdir(void)
{
  char *path;
  struct inode *ip;

  begin_op();
  if(argstr(0, &path) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_mknod(void)
{
  struct inode *ip;
  char *path;
  int major, minor;

  begin_op();
  if((argstr(0, &path)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEV, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_chdir(void)
{
  char *path;
  struct inode *ip;
  struct proc *curproc = myproc();
  
  begin_op();
  if(argstr(0, &path) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(curproc->cwd);
  end_op();
  curproc->cwd = ip;
  return 0;
}

int
sys_exec(void)
{
  char *path, *argv[MAXARG];
  int i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    if(fetchint(uargv+4*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(uarg, &argv[i]) < 0)
      return -1;
  }
  return exec(path, argv);
}

int
sys_pipe(void)
{
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if(argptr(0, (void*)&fd, 2*sizeof(fd[0])) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      myproc()->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}
