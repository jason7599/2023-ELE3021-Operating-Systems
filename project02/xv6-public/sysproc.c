#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

extern struct spinlock pgdirlock;

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

// ! growproc 여기서 호출한다
// ! !
int
sys_sbrk(void)
{
  int addr;
  int n;

  // cprintf("sysbrk"); // * malloc도 바로 growproc 가는게 아니라 여기를 지나네
  // ? 근데 free는 여기 안 지나는거 같은데?


  if(argint(0, &n) < 0)
    return -1;
  
  // pushcli(); // ! 이게 interrupt 막아주는 거 아닌가. 이거 해도 안됨


  // addr = myproc()->sz; // * * growproc하기 전 sz return한다
  // ! 이거에 대한 race condition인가? growproc은 진짜 ㅈㄴ lock으로 철벽 방어 해뒀는데 여긴 뭐 없잖아
  // ! 그럼 sys_getsize 같은거 하나 만들까?
  
  // addr = myproc()->main->sz; // ! 이렇게 해봤더니 이젠 panic: unknown apicid 뜬다
  // ㄴㄴ switchuvm 문제였던거같음 암튼 이젠 해결
  acquire(&pgdirlock);

  addr = myproc()->sz;

  // ! 놉 아주 가끔씩 trap 14 ㅈㄴ 많이 뜸
  // ! 아주 가끔이 아니네 ㅎㅎ
  if(growproc(n) < 0)
  {
    release(&pgdirlock);
    return -1;
  }

  release(&pgdirlock);
  
  // popcli();
  // if (addr + n != myproc()->sz)
  // {
  //   cprintf("hmmmmmm");
  // }

  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

int
sys_setmemorylimit(void)
{
  int pid, limit;

  if (argint(0, &pid) < 0 || argint(1, &limit) < 0)
    return -1;
  
  return setmemorylimit(pid, limit);
}

int
sys_thread_create(void)
{
  thread_t *thread;
  void *start_routine;
  void *arg;

  if (argptr(0, (char **)&thread, sizeof(thread)) < 0 || 
      argptr(1, (char **)&start_routine, sizeof(start_routine)) < 0 ||
      argptr(2, (char **)&arg, sizeof(arg)) < 0)
    return -1;
  
  return thread_create(thread, start_routine, arg);
}

int
sys_thread_exit(void)
{
  void *retval;

  // cprintf("tid %d: sys_thread_exit\n", myproc()->tid);

  if (argptr(0, (char**)&retval, sizeof(retval)) < 0)
    return -1;
  
  thread_exit(retval);
  return 0;
}

int
sys_thread_join(void)
{
  thread_t thread;
  void **retval;

  if (argint(0, &thread) < 0 || argptr(1, (char**)&retval, sizeof(*retval)) < 0)
    return -1;
  
  return thread_join(thread, retval);
}

int
sys_proclist(void)
{
  proclist();
  return 0;
}