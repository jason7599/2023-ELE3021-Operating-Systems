#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

// Array of processes
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct queue mlfq[3];

const int pw = 2020059152;

static struct proc *initproc;

int schedlock = 0; // 현재 process가 lock 걸었으면 1, 아니면 0. CPU 한개니까 lock에 대한 race condition은 없지 않을까?
int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

// 필요하면 여기서 mlfq 배열 초기화해줘도 될거같음.
void
pinit(void)
{
  // int i;

  initlock(&ptable.lock, "ptable");

  // for (i = 0; i < 3; i++) {
  //   mlfq[i].lev = i;
  // }

  // cprintf("init mlfq done\n");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

int
getLevel(void)
{
  return myproc()->qlevel;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
// UNUSED -> EMBRYO 즉 New 상태로
// 여기서 priority나 tq같은거 초기화해주면 될듯.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  // Look for unused proc slot
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  // not found
  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO; // new
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  // 초기화
  p->tq = 4;
  p->priority = 3; 

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
// fork에서만 proc이 queue에 들어가는줄 알았는데, 여기서도 allocproc()하고 runnable로 바꿔주니까
// 여기서도 enqueue해줘야함
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc(); // !!!!
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  enqueue(p, 0);

  release(&ptable.lock);
}

int myffunction(char* str)
{
  cprintf("%s %s\n", str, str);
  return 0xABCD;
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
// QUEUE들어가는 부분 * * * *
// lock 잡은 proc이 fork하면 어쩌지? 
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  enqueue(np, 0); // * * * ** 
  // np->tq = curproc->tq;
  // np->qlevel = curproc->qlevel;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
// RUNNING -> ZOMBIE로 바뀜
// proc 수행 종료. lock 확인해야할듯
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent); // !!!!

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE; // UNUSED로 바뀌는건 wait()에서

  // lock 들고 있었으면 해제
  if (schedlock) schedlock = 0;

  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
// 위 exit()에서 ZOMBIE가 된 자식을 UNUSED로 바꿔줌
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

void
enqueue(struct proc *p, int qlevel)
{
  // cprintf("enqueueing %s to L%d\n", p->name, qlevel);
  // 출력문 빼니까 잘 돌아가는데.

  struct queue *q = &mlfq[qlevel];
  struct proc *tmp = q->head;

  p->qlevel = qlevel;
  // p->tq = 2 * qlevel + 4; 

  if (!tmp) { // empty queue
    q->head = p;
    q->tail = p;
    return;
  }

  if (qlevel == 2) { // priority 기준으로
    while (tmp->next && p->priority >= tmp->next->priority) {
      tmp = tmp->next;
    }
    p->next = tmp->next;
    if (!p->next) // tail 갱신
      q->tail = p; 
    tmp->next = p;
  }
  else { // 마지막에
    q->tail->next = p;
    q->tail = p;
  }

}

// L0 큐의 제일 앞으로 이동
// ptable lock 걸어줘야하나?
void 
movefront(struct proc *p)
{
  // cprintf("move front called\n");

  struct queue *q = &mlfq[0];
  p->qlevel = 0;

  if (!q->head)
    q->tail = p;
  p->next = q->head;
  q->head = p;
  
  // cprintf("move front done\n");
  // sched();
}

struct proc*
dequeue(int qlevel) // pop first(head) process from a queue
{
  struct queue *q = &mlfq[qlevel];
  struct proc* p;

  if (q->head) {
    p = q->head;
    if (p == q->tail) // 크기 1에서 pop
      q->tail = 0;
    q->head = p->next;
    p->next = 0; // 이거 안해줘도 상관은 없을듯
    return p;
  }

  return 0; // == queue is empty
}

// 모든 process L0로 이동
// priority 3으로
// tq 초기화
// *** 지금 proc이 schedulerLock() 호출했었으면 L0 맨앞으로 (tq랑 priority 초기화 없이?)
// 이거할때 현재 proc yield도 해줘야하나?
void
prboost(void)
{
  struct proc *p;
  int qlevel;

  acquire(&ptable.lock); // 필요한가

  // cprintf("prboost\n");
  // ! 실수하고 있었다..
  p = mlfq[0].head;
  while (p) {
    p->tq = 4;
    p = p->next;
  }
  for (qlevel = 1; qlevel <= 2; qlevel++) {
    while ((p = dequeue(qlevel))) {
      p->tq = 4;
      p->priority = 3;
      enqueue(p, 0);
    }
  }

  p = myproc();
  if (p) {
    // cprintf("if (p) enter\n");
    // if (schedlock) {
    /*
    * schedlock 풀고, tq랑 priority 초기화 없이 L0 맨 앞으로 이동
    */

    if (schedlock) {
      schedlock = 0;
      movefront(p);
    }
    else {
      // ? 야매. tq = 1, qlevel = -1로 해줘서 yield할 때 qlevel 0으로 들어가도록..
      p->priority = 3;
      p->tq = 1;
      p->qlevel = -1;
    }
  }

  // cprintf("prboost done\n");

  release(&ptable.lock);

  /*
  * 현재 proc이 Lock을 잡고 있는데 Priority Boosting이 발생하면
  * tq랑 priority 초기화 없이 L0 큐 맨 앞으로 이동.
  * 이러면 그냥 다시 바로 실행이 된다는 거 아닌가? 
  * 즉 실제로 enqueue 해줄 필요가 있나? 
  */
}

void
setPriority(int pid, int priority)
{
  struct queue *q;
  struct proc *t;
  struct proc *t1;
  int found = 0;
  int qlevel = 0;

  if (myproc()->pid == pid) {
    myproc()->priority = priority;
    found = 1;
  } else {
    acquire(&ptable.lock); // 필요한가

    for (; qlevel < 3; qlevel++) {
      q = &mlfq[qlevel];
      t = q->head;
      if (!t) continue;
      if (t->pid == pid) { // head
        found = 1;
        t->priority = priority;
        break;
      }
      while (t && t->next) {
        if (t->next->pid == pid) {
          found = 1;
          t1 = t->next;
          t1->priority = priority;
          // t->next->priority = priority;

          // L2 큐인 경우에만 재정렬이 필요함
          // 그래서 t1을 우선 뽑아내고, 다시 enqueue해서 정렬되도록
          if (qlevel == 2) {
            // t->next = t->next->next; // 
            if (q->tail == t1) // t1이 꼬리였다면 
              q->tail = t;
              
            t->next = t1->next; // t1 뽑
            enqueue(t1, 2);
            break;
          }
        }
        t = t->next;
      }
      if (found) break;
    }
    release(&ptable.lock);
  }

  if (!found) {
    cprintf("setPriority Error: process with pid not found\n");
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
// 출력문 있으면 망가지고 없애면 잘 돌아감. 아니네? 저번에 뭘 잘못한거지
void
scheduler(void)
{
  // cprintf("scheduler start\n");

  struct proc *p;
  struct cpu *c = mycpu();
  int qlevel = 0;
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    acquire(&ptable.lock);
    
    // while queue is not empty
    while ((p = dequeue(qlevel))) {
      // cprintf("popped %s\n", p->name);

      c->proc = p; // proc: process currently running
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context); // CONTEXT SWITCH TO p
      switchkvm();

      c->proc = 0;
      qlevel = 0; // L0부터 다시 시작
    }
    
    if (++qlevel == 3) qlevel = 0;

    release(&ptable.lock);
  }
}

void 
schedulerLock(int password)
{
  int pid;
  int tq;
  int qlevel;
  struct proc *curproc = myproc();

  // 애초에 이 함수를 호출하려면 cpu를 들고 있어야하니까 이런 상황은 없지 않을까
  if (schedlock) {
    cprintf("THIS SHOULD NEVER HAPPEN\n");
    return;
  } 

  if (password == pw) {

    acquire(&tickslock);

    // cprintf("locking %s\n", curproc->name);
    // cprintf("locking %d\n", curproc->pid);

    schedlock = 1;
    ticks = 0;

    release(&tickslock);
    // cprintf("lock ok :)\n");
  }
  else {
    pid = curproc->pid;
    tq = curproc->tq;
    qlevel = curproc->qlevel;

    kill(pid);
    cprintf("Lock failed: incorrect password\n"); // ! 해당 프로세스가 큐에서 사용한 timeQuantum을 출력
    cprintf("pid: %d timequantum: %d queue: L%d\n", pid, 2 * qlevel + 4 - tq, qlevel);
  }
}

void
schedulerUnlock(int password)
{
  int pid;
  int tq;
  int qlevel;
  struct proc *curproc = myproc();

  if (!schedlock) {
    // lock을 걸었어도 중간에 prboost나 sleep 같은걸로 풀렸을 수 있으니 이건 자연스러운 현상일듯
    // cprintf("no lock held\n");
    return;
  }

  if (password == pw) {

    acquire(&ptable.lock);

    // cprintf("unlocking %d\n", curproc->pid);
    // cprintf("unlock ok :)\n");
    schedlock = 0;

    // tq 리셋
    curproc->tq = 4;
    curproc->priority = 3;
    movefront(curproc);
    curproc->state = RUNNABLE;

    sched(); // * 이거는 호출 문제 전혀 없는데

    release(&ptable.lock);
  } 
  else {
    pid = curproc->pid;
    tq = curproc->tq;
    qlevel = curproc->qlevel;
    
    kill(pid);
    cprintf("Unlock failed: incorrect password\n");
    cprintf("pid: %d timequantum: %d queue: L%d\n", pid, 2 * qlevel + 4 - tq, qlevel);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena; // intena = is interrupt enabled?
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");

  intena = mycpu()->intena;
  
  swtch(&p->context, mycpu()->scheduler); // context switch to scheduler

  mycpu()->intena = intena;
}

// called on timer interrupt
void
yield(void)
{
  struct proc* curproc;

  acquire(&ptable.lock);  //DOC: yieldlock

  curproc = myproc();
  curproc->state = RUNNABLE;

  // if (curproc->qlevel == -1) {
  //   movefront(curproc);
  // }

  // cprintf("\nyield %s\n", curproc->name);

  // 매 틱마다 yield해주기는 해야됨.
  // 다만 tq 다 썼으면 좌천시키기
  if (curproc->tq <= 0) {

    if (curproc->qlevel == 2) {
      if (curproc->priority)
        curproc->priority--;

      curproc->tq = 8;
      enqueue(curproc, 2);
    }
    else {
      curproc->tq = 2 * (curproc->qlevel + 1) + 4;
      enqueue(curproc, curproc->qlevel + 1);
    }
  }
  else { // tq 다 쓰지는 않은 경우, 그냥 현재 큐 맨 뒤로 보내기 (L2면 맨뒤는 아니겠고)
    enqueue(curproc, curproc->qlevel);
  }


  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
// schedlock 건 proc이 sleep하는 상황이되면 어쩌지
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  // 그냥 lock 푸는게 맞을듯
  // TODO: lock stack 같은거 만들어서 lock?
  if (schedlock) schedlock = 0;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
// 여기도 enqueue 필요 * ** * * ** *
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->state == SLEEPING && p->chan == chan) {
      p->state = RUNNABLE;
      // tq 초기화 x. 
      enqueue(p, p->qlevel);
    }
  }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
// 
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;

      // 이런 경우가 생길까?
      // lock 걸은 process가 unlock할때 pw 틀리면.
      if (schedlock) {
        // cprintf("schedlock killed??\n");
        schedlock = 0;
      }

      // cprintf("Kill %s\n", p->name);
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

void
printqueue(void)
{
  int qlevel = 0;
  struct proc *t = myproc();
  struct queue *q;

  cprintf("\nMLFQ at tick %d:\n", ticks);

  if (t) {
    cprintf("curproc: [%d tq:%d p:%d L%d]", t->pid, t->tq, t->priority, t->qlevel);
    if (schedlock) cprintf(" LOCKED");
    cprintf("\n");
  }

  for(; qlevel < 3; qlevel++) {
    cprintf("L%d", qlevel);

    q = &mlfq[qlevel];
    t = q->head;

    while (t) {
      cprintf("->[%d tq:%d p:%d]", t->pid, t->tq, t->priority);
      t = t->next;
    }
    cprintf("\n");
  }
  cprintf("\n");
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  // if (0)
  // {
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state == UNUSED)
        continue;
      if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
        state = states[p->state];
      else
        state = "???";
      cprintf("%d %s %s", p->pid, state, p->name);

      // if (p->next) cprintf(" %s", p->next->name);

      if(p->state == SLEEPING){
        getcallerpcs((uint*)p->context->ebp+2, pc);
        for(i=0; i<10 && pc[i] != 0; i++)
          cprintf(" %p", pc[i]);
      }
      cprintf("\n");
    }
  // }
  // else
  // {
    // cprintf("\nprocdump\n");
    // p = myproc();
    // if(p && p->state != UNUSED) {
    //   if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
    //     state = states[p->state];
    //   else
    //     state = "???";
    //   cprintf("%d %s %s", p->pid, state, p->name);
    // }
    // for (i = 0; i < 3; i++)
    // {
    //   q = &mlfq[i];
    //   cprintf("\nL%d :", i);
    //   p = q->head;
    //   while (p)
    //   {
    //     cprintf(" %s", p->name);
    //     p = p->next;
    //   }
    // }
    // cprintf("\n\n");
  // }

}
