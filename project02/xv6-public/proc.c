#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct spinlock pgdirlock;
int wassupyo = 120;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&pgdirlock, "pgdir");
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

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->tid = 0; // // LWP가 아닌 Process들은 이걸 0으로 해서 구분을 하자
  p->stacksize = 1; // * 기본적으로 1개 
  p->memlim = 0; // * 기본적으로 0 = 제한 없음
  p->main = p; // * 기본적으로 자신이 main. 그럼 main이 자신이 아닌 애들이 LWP라고 구분하자

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE; // * = bottom of the stack?
  // & kstack에 할당한 다음에 + 한거니까 sp는 이제 kstack의 윗?부분을 가르키게 되네
  // & 애초에 그럼 아래 로직은 user stack을 채우는게 아니라 kstack을 채워주는거네 * * * * *

  // Leave room for trap frame.
  sp -= sizeof *p->tf;              // & 1. trapframe push
  p->tf = (struct trapframe*)sp;    // & trapframe: kernel 모드에서 user 모드로 돌아갈 때를 위해 user 모드에서의 register를 저장한 것

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;       // & 2. return할 주소. trapret: trapframe을 다시 복구 시켜서 user 모드로 다시 돌아감

  sp -= sizeof *p->context;         // & 3. context push
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  // & 이 process로 switching 되면 forkret으로 가도록 설정
  // & 위 주석대로 그럼 forkret으로 보내고, return하면 trapret으로 감
  // & trapret에서는 tf의 정보를 register에 restore.
  // & 즉 tf를 수정해놓는게 핵심이겠네 * * * * * * 
  // & forkret -> trapret -> tf.eip * * * * * * 

  return p;
}

// allocproc이랑 fork랑 잘 섞어야할듯
// * 이 아니라 allocproc을 써도 되겠다 그냥. 겹치는 부분이 너무 많음
// * 그럼 growproc할 때 curproc이 아니라 main이 하게끔할까
int
thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg)
{
  // 계획 (old): 
  // 1. ptable 빈칸 찾기
  // 2. curproc의 정보 공유하기
  // 3. 함수 실행? pc로?
  
  struct proc *main = myproc()->main;
  struct proc *p;
  uint sp, ustack[2];
  pde_t *pgdir;

  // * * * exec2 system call은 thread와 관계 없습니다. * * * exec2로 받은 stacksize랑 thread는 무관하다
  // * 그럼 그냥 exec에서 한거처럼 하면 될듯 => 암튼 스택용이랑 가드용 페이지 둘 다 결국 만들긴 해야함. 
  // * memlim 에 대해 "프로세스의 메모리는 thread의 메모리를 고려해야 합니다" 라고 적힌거도 thread 생성할 때 메모리 확인이 필요하다는 거 같음
  // * 그런거면 다 맞아떨어진다. stacksize로 받은 여러 page 중 하나를 thread에 주는게 아니라 메모리를 더 할당받는거고, 스택용이랑 가드용 페이지도 따로
  // * 그럼 memlim 체크도 할 겸 growproc으로 페이지 2개를 더 받고, exec 처럼 clearpteu 로 아래 페이지를 가드용으로 만들어보자
  // * growproc 자체가 allocuvm을 호출하고, allocuvm은 또 kalloc이랑 mappages 호출해서 curproc의 pgdir도 커지고 mapping도 될 거임.

  // ! ! ! ! ! sbrk.. sz 조심해야됨.
  // ! create에서 sp로 이거저것 하는 동안에 딴놈이 growproc으로 지랄해서 panic 뜨는듯.
  // ! sz
  // ! lock 때문에 이거 순서 바꿔야할듯

  // & 2. allocproc으로 ptable의 빈칸 찾아서 p 할당. allocproc에서 kstack에 필요한 자료 다 push해줌.

  acquire(&pgdirlock); // ! ! ! ! allocproc에서 main 꽂기 전부터 lock 잡아보자

  if ((p = allocproc()) == 0) // ptable이 가득인 경우. 사실상 이 문제는 안 생길듯
  {
    return -1;
  }

  // & 1. 페이지 두개 할당 시도. clearpteu로 하나를 가드용으로 

  // * growproc에서도 main을 키움.
  if (growproc(2 * PGSIZE) == -1) // 아마 memlim 초과 
  {
    cprintf("growproc fail\n");
    thread_free(p);
    return -1;
  }
  
  pgdir = p->pgdir = main->pgdir; 
  sp = p->sz = main->sz;

  clearpteu(pgdir, (char*)(sp - 2 * PGSIZE));

  *p->tf = *main->tf;  // TODO: 전부 복사할 이유는 없을거같은데 이거 안하면 trap 13 뜬다
  p->tf->eip = (uint)start_routine;

  // & 4. * * * * Caller frame 채우기 * * * * * 
  // &    시프 07-machine-procedures 32 pg
  // &    인자, ret. 주소 push
  
  // sp = main->sz; // * growproc 이후 curproc->sz는 스택용 페이지의 가장 위쪽이니까 이걸로 설정 가능
  // * sz는 상대적인 크기니까 esp에 넣어주면 안되지 않나 생각했었는데 cpu는 논리주소만 다루니까 esp 같은 register도 논리주소가 맞을듯
  // * 이제 이 sp를 감소시켜 가며 arg 랑 ret 저장해주면 됨. exec ustack 참고
  // p->stacksize = curproc->stacksize; // ! stacksize는 공유 안하는게 맞겠다. thread는 stacksize가 1 그대로인게 맞지
  // p->memlim = curproc->memlim;  // * memlim은 공유 의미 없는게, 어차피 growproc할때 main의 memlim으로 따짐

  ustack[0] = 0xffffffff;
  ustack[1] = (uint)arg; 

  sp -= 8; // 감소 시킨 다음에 copyout
  if (copyout(pgdir, sp, ustack, 8) < 0)
  {
    // TODO: 여기서도 growproc한거랑 allocproc한거 취소해줘야하는데 이 문구 출력되면 그 때 더 생각해보자
    cprintf("ustack fail?\n"); 
    thread_free(p);
    release(&ptable.lock);
    return -1;
  }

  p->tf->esp = sp; // esp: stack top
  p->pid = main->pid; // main thread의 pid를 공유
  p->tid = *thread = nextpid; // 그냥 nextpid로 하는거 너무 야맨가? allocproc할때마다 nextpid 1씩 증가하니까 겹칠 일은 없음.
  p->parent = main->parent; // * 부모도 그대로 가져오자 그냥. 사실 부모 별 의미 없을듯. 자원 회수만 main에서 잘 하면
  p->main = main; // * 다만 main은 하나로

  release(&pgdirlock);

  // & 3. start_routine 실행되게 kstack 수정하기. 
  // &    context->eip   = forkret  : 이 프로세스로 context switch 될 때 이 프로세스가 실행할 IP
  // &    return address = trapret  : 실행한 함수가 return되면 갈 주소. 즉 forkret을 수행하고, trapret으로 return
  // &                     trapret  : trapframe에 저장해둔 register들 restore. 
  // &    그래서 trapframe의 eip만 start_routine으로 해주면 trapret 실행될 때 적용이 됨

  
  // & 5. p 변수들 초기화 / 공유


  // * filedup은 그냥 file의 사본을 만드는 건 줄 알았는데, filedup이랑 idup을 보니 원본 file에 대한 포인터를 반환한다.
  // * file 구조체에 ref count를 둬서 dup할때 ++하고, close할 때 --해서 0되면 닫는 방식이라 함. 
  // * 그런거면 이렇게 dup으로도 파일 공유를 할 수 있을 듯? 
  // ? 근데 좀 불안한게, fork에서 이렇게 file을 복사해옴. fork도 원래 file 공유하는게 맞나?
  for(int i = 0; i < NOFILE; i++)
    if(main->ofile[i])
      p->ofile[i] = filedup(main->ofile[i]);
  p->cwd = idup(main->cwd);

  safestrcpy(p->name, main->name, sizeof(main->name));

  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock); // ! lock 범위 너무 큰가? 근데 이렇게 크게 잡았더니 page fault는 안남
  // ! 근데 문제는 종종 fail 뜬다는 건데. 이건 포기해야겠다

  return 0;
}

// 모든 thread는 이 함수로 종료되야 함.
// 지금 thread_create로 테스트 해보니까 pagefault 뜨는걸 보니, return할 주소가 이상해서 그런듯.
// 그럼 이 thread_exit을 통해, exit이 그렇듯 return 하지 않도록 짜면 어떨까
void
thread_exit(void *retval)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  // & fileclose가 꼭 이 file을 닫는게 아니라 refcount -- 하고 0 되면 close함.
  // & 즉 thread_create 할때 filedup으로 refcount ++ 되었을테니 여기서 fileclose하면 되겠음.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd); // & 얘도 마찬가지
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // cprintf("wapkeup %s\n", curproc->main->name);
  // cprintf("t%d: wakeup(%d)\n", curproc->tid, curproc->pid);
  wakeup1(curproc->main); // ! join도 부모가 아니라 main이 하게끔

  // cprintf("thread_exit: wake up master pid %d\n", curproc->pid);

  // 자식들 처리하기는 여기서 안함. 자식이 없을거니까.
  // thread가 thread_create로 "자식" thread 만들었는데 이 자식보다 부모가 먼저 thread_exit 하면?
  // 그냥 main proc, 즉 thread_create하는 애를 모든 thread들의 공통 부모 thread로 만들면 이건 걱정 안해도 될 듯?
  // 즉 thread_create, thread_join은 thread가 아니라 한 중앙 process가 하게끔 하자
  
  // ! .. fork에 의해 생성된 child process를 wait하는 thread는 반드시 fork를 실행했던 thread가 되어야한다..
  // ! 이러면 thread도 자식이 있을 수 있는 거고, thread도 wait을 할 수 있는거네.
  // ! 할만할 듯. 그럼 그냥 exit이랑 똑같이 고아 만들기 하면 되겠네. 와 그냥 exit이랑 거의 똑같아지네 이제
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p == p->main && p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  curproc->state = ZOMBIE;
  
  // if (curproc->killed)
    // cprintf("thread_exit finish: tid %d pid %d\n", curproc->tid, curproc->pid);

  sched();
  panic("thread zombie exit");
}

// * wait 느낌. 다만 page table을 freevm으로 다 닫아버리면 안됨. 닫을거면 이 thread가 쓰는 부분만 닫아줘야하는데.. 
// * 회수 & 정리: 페이지 테이블, 메모리, 스택
int
thread_join(thread_t thread, void **retval)
{
  struct proc *p;
  struct proc *curproc = myproc();
  int found;
  
  // * thread_kill_others에서 wait처럼 이중으로 바꿨더니 문제 뭔가 해결됬음 여기도 바꾸자
  // * 근데 wait이나 thread_kill_others는 여러개를 기다릴 수 있으니까 좀 말이 되는데 이건 하나만이잖아
  // * 몰라 걍 해보자
  // ! 바꿨는데도 재부팅 문제 있는거보면 얘가 문제는 아니었나봄. 그래도 이대로 하자 혹시 모르니까
  // ! kill 해도 exit 안하고 sleep 상태 있는 상황 발견
  // * * * * * * * * wait 처럼 curproc->killed도 확인을 해줘야하네 * * * * * * *
  // * 이거하니까 해결
  // * 이게 계속 join에서 기다리기만 하니까 죽어도 sleep을 벗어나질 않은거같음
  
  acquire(&ptable.lock);

  for (;;)
  {
    found = 0;

    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->tid != thread)
        continue;
      
      if (p->state == ZOMBIE)
      {
        *(uint*)retval = *(uint*)(p->tf->esp + 4);

        thread_free(p);

        release(&ptable.lock);
        return 0;
      }

      found = 1;
    }

    if (!found || curproc->killed)
    {
      // cprintf("no child threads!!!!\n");
      release(&ptable.lock);
      return -1;
    }

    sleep(curproc, &ptable.lock);
  }
  // for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) 
  // {
  //   if(p->tid != thread)
  //     continue;
  //
  //   // TODO: 논리적으론 이게 맞는거 같은데 wait에서는 이렇게 안해서 불안
  //   // TODO: 테스트 해봤더니 문제 없긴한데 타이밍 꼬이면 잘못 될 수가 있나
  //   if(p->state != ZOMBIE)
  //   {
  //     sleep(curproc, &ptable.lock);
  //   }
  //
  //   // ? sleep에서 깰 때 p->state이 ZOMBIE임이 보장되기만 하면 문제 없겠는데.
  //   // ? sleep 깰 때 lock 다시 잡으니까. 
  //   // ? 그럼 p가 정말 ZOMBIE 상태로 갈 때만 curproc을 wakeup하면 되는데. 문제 없을듯?
  //
  //   // * * * * 지렸다
  //   // * caller stack 또 활용해서
  //   // * p->tf->esp + 4: 이게 retval 시작 주소. thread_exit 때 이놈이 인자로 푸쉬했으니까
  //   // cprintf("retval is prolly %d\n", *(uint*)(p->tf->esp + 4));
  //   *(uint*)retval = *(uint*)(p->tf->esp + 4);
  //   thread_free(p);
  //
  //   release(&ptable.lock);
  // 
  //   return 0;
  // }
  // 
  // release(&ptable.lock);
  // 
  // cprintf("no child threads!!!!\n");
  // return -1;
}

// * thread 자원 회수. exit, exec, join에서 쓸거.
// * 여기서 state = UNUSED로 바꿀거니까 lock 필요할듯. 근데 join의 경우 sleep 때문에 이미 잡은 상태로 호출할 것
// * * * 그럼 꼭 lock 잡은채로 이거 호출하도록. 그리고 p는 ZOMBIE인 상태여야하고.
void
thread_free(struct proc *p)
{
  // TODO TODO TODO * * * * pgdir, sz 회수 * * * * * * * * * * * * * * * * * * * * * * * * * * *
  // TODO: freevm으로 자기 pgdir 다 닫으면 안됨. 그래서 얘가 쓰는 부분만 닫아줘야 할 거 같은데
  // TODO: 그리고 memlim 생각해보면 join 할때 sz 다시 줄여주는게 맞을듯. 근데 문제는 빈칸 생기면 어쩜

  kfree(p->kstack); // & kstack은 어차피 따로 할당했으니 free 해줘야함
  p->kstack = 0;
  p->pid = 0;
  p->tid = 0;
  p->parent = 0;
  p->main = 0;
  p->name[0] = 0;
  p->killed = 0;
  p->state = UNUSED;
}

// * Zombie인 자식들 전부 reap 해줌. 자신을 제외
// * kill 이후 exit에서 쓸 예정. 즉 thread들 모두 ZOMBIE 상태여야 됨
// * exec에서도 이걸 써먹을 수 있으면 좋겠는데 exec 전에 kill을 해야 하겠네? ;
// * wait 처럼 ptable 돌면서 ZOMBIE 애들 찾기. 여러개일 수 있어
// TODO: 이러면 curproc도 여기서 reaping 되겠는데? 괜찮나? main thread는 이거 후에 exit되는데
// 일단 그럼 curproc인 경우도 continue 하자
// * * * 얘도 lock 잡은 채로 호출하자. exit때문에 어쩔 수 없다
// * 대공사 하자. exec이랑 일반 예외상황(메인이 join 안하고 exit하는 경우)에서도 이걸 써먹으려면 죽이는 것까지도 여기서 하는게 좋겠다.
// * curproc의 thread들 전부 kill + 자원 처리.
void
thread_kill_others(void)
{
  // exec할때도 이걸로 스레드 다 죽여야함. 근데 thread가 exec을 호출할 수 있다고 하면 여기서 curproc을 기준으로 잡으면 안되겠네.
  // 여기서도 curproc->main을 기준으로 해야겠네. 그럼 만약 thread가 이걸 호출하면 자기 자신을 kill하고 reap해버리게 되겠는데?
  // 그럼 상황이 어떻게 될지 생각해보자:
  //    1. thread가 exec 호출해서 thread_kil_others 호출.
  //    2. main이 아닌 thread들 전부 죽일 것임. 여기에 자신도 포함
  //      2-1. 자신을 killed = 1 마킹
  //      2-2. 자신이 ZOMBIE가 아니니까 sleep.
  //      2-3. trap에서 얘를 보고 exit 호출, 이건 thread_exit으로 이어짐
  //      2-4. thread_exit덕에 본인은 이제 ZOMBIE. // ! 근데 wakeup은 main을 깨우니까 본인은 안 깨겠다.
  //      2-5. 그리고 본인이 깰 수 있긴한가? ZOMBIE에서 깨어날 수가 있는건가? 아니네. 안되겠네 이러면.
  // 테스트 안해봐서 확신은 못하겠지만 이렇게 하면 문제 생길거같다.
  //
  // 결국 얘를 쓰려면 thread_kil_others는 main만 호출해야된다.. 이 말은 exec도 main만 호출해야한다는건데..
  // 지금 올라온 테스트 코드 보니까 thread에서도 exec 호출한다 ㅅㅂ
  //
  // 좀 극단적인 방법을 생각해봄: thread_create에서 eip 건드리고 그랬듯 main의 tf를 건드려서 이 함수가 호출되게 할까? ㅋㅋ
  //
  // ! 문제상황: thread가 thread_kil_others 호출하면 안되는 디자인인데 thread가 exec을 호출하면 thread_kil_others를 호출해야함.
  // !          문제가 되는 이유는 thread가 자살하면 이상해질게 뻔하니까.
  //
  // * 아니면? exec할 때 main을 바꿔치기할까? 오?? ㅈㄴ 야매스럽지만 thread_kil_others 호출될 때 curproc이 main이면 되니까.
  // 자신이 main이 아니면: 자신을 메인으로 만들고, 원래 메인의 메인을 자신으로 바꾸면? 
  // 원래 메인뿐 아니라 다른 모든 애들도 자신을 메인으로 갖게 바꾸면? 좀 무식한 야매 방법이긴하네.
  // 아닌가? 전부 바꿔줄 필요도 없으려나? 
  // 
  // thread t가 exec을 호출하는 상황을 생각해보자: (즉 curproc == t)
  //     1. exec에서 t가 main이 아님을 보고,
  //        t->main->main = t;  기존 main 서열정리
  //        t->main = t;        t를 오피셜 main으로 지정. 
  //     2. 당당한 main으로써 t가 thread_kil_others 호출.
  //     3. (자신을 제외하고) t를 main으로 가지는 모든 thread들 죽인다. 
  //        이 때 t를 main으로 가질 "thread"는 기존 main밖에 없네. 
  //        이외 상황에서 다른 thread를 main으로 가지는 thread는 없으니까.
  //        암튼 그래서 기존 main을 kill 마킹해줌.  
  //     4. 그럼 이제 기존 main에 exit이 호출됨.
  //        기존 main은 이제 thread인 셈이니 thread_exit을 호출.
  //        ! 이 부분이 문제네. thread_exit은 curproc을 main으로 갖는 thread들이 없다는 걸 전제했으니. 
  //        ! 그럼 이렇게라면 전부 바꿔줘야하네
  // 
  // * * * * 암튼 핵심은 thread가 thread_kil_others 호출하면 얘만 살아남고 나머지는 다 뒤져야함. 즉 얘가 main이 되야됨. * * * * *
  //
  // ? 그냥 언뜻 든 생각: 복잡한 kill 과정 안 거치고 그냥 바로 reaping해도 되는거면 이런 고생 없을텐데. thread는 될 거 같기도하고.
  // ?                   근데 되는지 안되는지도 모르고 어차피 이제와서 갈아타기도 늦었다
  // 
  // 그럼 해결방안은 크게 두가지가 있겠다:
  //    a. 처음 말한거처럼 ptable 다 돌면서 다 바꿔주기
  // *  b. 기존 main만 바꿔치기하고, 기존 main을 kill하는 거까진 좋음. 근데 기존 main이 exit할 때 thread_exit말고 exit을 해야됨.
  // *     그럼 ptable p로 순회할때 p == oldmain인 경우 p->main = p 로 다시 해줄까? 오,, 이거도 야매긴한데 그나마 깔끔한 야매다
  // *
  // *     다시 시뮬레이션 ON: 
  // *        1. exec에서는 조건 따질거 없이 그냥 thread_kil_others 호출
  // *        2. thread_kil_others에서 따짐. t가 main이 아니면:
  // *           oldmain = t->main;
  // *           t->main->main = t;
  // *           t->main = t;
  // *        3. ptable 순회하면서 p->killed = 1 이건 마찬가지로 해줘.
  // *           다만 이제 p == oldmain인지 확인하고, 맞다면:
  // *           p->main = p; 로 다시 main이 되게끔.
  // *           이러면 oldmain은 exit할 때 thread_exit으로 안 가고 exit으로 가서, 
  // *           또 thread_kil_others를 호출해서 자식들 다 죽여주겠다. 그리고 ZOMBIE 상태로 가겠지.
  // *        4. 그럼 oldmain이 exit했으니 t가 깨어나.. 야 하는데 ㅆ삐ㅏㄹ
  // *           oldmain은 exit을 호출하므로 자기의 부모를 wakeup하겠다.
  // *           그러면 야매 한발짝 더 나아가서 3번 과정에서 p->parent = t; 해줄까? ㅋㅋ ㄱㄱㄱ
  // *
  // * b 방법 괜찮은거 같다.
  //
  // ? 언뜻 든 생각 2: 애초부터 thread의 parent를 main으로 했으면 어땠을까?
  /*
  * 
  * newmain != oldmain 인 경우:
  * 
  * 1. newmain->main = newmain; 으로 독립. oldmain이 exit에서 reap_all 할때 newmain 안 죽이도록 관계 끊는거임.
  * 2. oldmain->parent = newmain; 존나 야매. exit하면 parent wakeup하니까 newmain 꺠우게끔.
  * 3. oldmain->killed = 1; 이걸로 oldmain 죽임. oldmain은 여전히 main이기 때문에 exit을 호출.
  * 4. sleep으로 oldmain 사망할 때를 기다리고 reap_thread 해줘. oldmain 쓰레드 아니긴한데 몰라 함수명 바꿀까
  * 
  * 이러면 끝이네 oldmain만 죽이면 oldmain이 나머지 다 죽여주니까 ㅇㅋㅇㅋㅇㅋㅇㅋㅇㅋㅇ커ㅏㅋㅇㅋ 
  * 
  * newmain == oldmain인 경우 걍 원래 했던대로 하면 되는거고
  * 
  */
  /*
  ! exec 테스트해봤더니 panic: sched ptable.lock 뜬다. 이거 ptable.lock 안 잡은 채로 sched 호출해서 그럼.
  ! 일단 이 함수는 lock 이미 잡았다고 가정하고 짠거긴해. 그리고 exec에서는 lock 안잡아. 그럼 여기서 잡아줘야하는데
  ! 그럼 그냥 lock 안 잡고 있었으면 잡고, 잡고 있었으면 그냥 하고 해보자
  ! 해결
  !
  ! 근데 이젠 xv6이 재부팅됨 ㅎㅎ
  */

  struct proc *newmain = myproc();
  struct proc *oldmain = newmain->main;
  struct proc *p;
  int havethreads;

  acquire(&ptable.lock);
  // * 걍 lock 여기서 잡자
  // * exit에서 이거 호출 순서 바꾸고 나서 보니까 exit이 lock 잡기 전에 이거 호출할거네 어차피

  //   // TODO: 이거도 이중반복문으로 바꿔야할듯? 그럼 그냥 조건문만 하나 추가하고 아래 로직 똑같이 쓰자. 
  // & thread가 호출한 경우. 현재로써는 thread가 exec 호출한 경우 외에는 없음.
  // & 이런 경우 oldmain을 죽여주고 newmain이 main이 되도록 갱신해야 함.
  if (newmain != oldmain) 
  {
    newmain->main = newmain; // oldmain이 exit할 때 자신까지 죽지 않게 독립
    oldmain->parent = newmain; // exit에선 parent를 wakeup 하니까 . 좀 야매
  }
  
  // 이전 구현:
  // ptable 한번만 순회. kill 마킹해주고 바로 sleep으로 기다림. 

  // wait 처럼 이중 반복해보자
  // * * * ㅅㅂ 이중반복문으로 하니까 재부팅 문제 해결이다
  // * 괜히 wait이랑 다르게 짜보겠다고 ㅈㄹ했더니 문제됬었네..
  // ! ㄴ thread_kill 두번 돌리면 재부팅된다 ㅋㅋㅋㅋㅋㅋㅋㅋㅋ
  // * 그ㅐㄹ도 일단 확실히 그냥 단일 반복문에 문제가 있었던듯. 
  // * 이중반복문으로:
  // *    thread가 ZOMBIE이면 자원 바로 회수.
  // *    ZOMBIE가 아니면, 즉 아직 종료안되어있으면 kill 마킹.
  // *    kill 마킹해놓으면 이따 다시 ptable 돌때 ZOMBIE인지 확인하면 됨
  // * 이전 디자인은 좀 순차적인 느낌이었다면 이젠 좀 병렬?적인 느낌이랄까 
  // TODO 그럼 join도 이렇게 바꾸고 위에 oldmain 죽이는거도 고쳐야됨

  for(;;)
  {
    havethreads = 0;

    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if ((p != newmain && p->main == newmain) || (newmain != oldmain && p == oldmain))
      {
        if (p->state == ZOMBIE)
        {
          // p가 kill 되었건 thread_exit으로 알아서 잘 종료되었다는 뜻이니까 이 경우 p는 더 이상 걱정 안해줘도 됨.
          // cprintf("p%d reap t%d\n", p->pid, p->tid);  
          thread_free(p); // TODO: oldmain의 경우 page table 같은거 함부로 닫으면 안될거임. 
                          // TODO: 나중에 thread_free 제대로 짜게 되면 여기 고려해야할 듯
        }
        else
        {
          p->killed = 1;
          if (p->state == SLEEPING)
            p->state = RUNNABLE;

          havethreads = 1;
        }
      }
    }
  
    // & 여기선 curproc->killed 확인할 필요 없다고 생각한다
    // & 특히 kill로 main 죽여서 main이 이거 실행하고 있는거면 main은 무조건 killed 상태겠지
    if (!havethreads) 
    {
      // cprintf("no threads of pid %d to kill found!\n", newmain->pid);
      break;
    }
    
    // cprintf("proc %s havethreads\n", newmain->name);
    sleep(newmain, &ptable.lock);
  }

  // if (lock) 
  release(&ptable.lock);
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
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

  release(&ptable.lock);
}

// kill 참고
// * "프로세스의 메모리는 thread의 메모리를 고려해야 합니다."
// * 프로세스의 thread들의 메모리 총합이 이 limit을 넘기면 안된다는 뜻 같음
// 아 이거도 main을 건드려야겠네
int
setmemorylimit(int pid, int limit)
{
  struct proc *p;

  if (limit < 0) return -1;

  acquire(&pgdirlock); // ? 필요한가? 혹시 몰라서 일단 해야지

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){

      // 할당받은 메모리가 limit보다 큰 경우
      // ^ 와 limit = 0인 경우를 고려 안했었네
      // ^ 인자로 0을 넘긴거면 제한을 풀겠다는 것
      if (limit && p->sz > limit) break; 

      p->main->memlim = limit;
      release(&pgdirlock);
      return 0;
    }
  }
  release(&pgdirlock);
  return -1;
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
// ! 추가적인 메모리를 할당받을 때
// ! 여기서 그럼 limit 검사해줘야 할 듯
// TODO: 여러 thread가 동시에 메모리 할당을 요청하더라도 할당해주는 공간이 겹치면 안된다 
// * thread가 thread 만드려할 때나 sbrk같은걸로 이 growproc 호출할 건데, 이 thread 혼자 grow하면 안되니까 curproc말고 main에 해주자.
// TODO: growproc하면 stacksize도 키워줘야 하나?
int
growproc(int n)
{
  // ! thread_sbrk 실험해보면 panic: thread zombie exit 뜬다. 이거 thread_exit에 내가 써둔건데 
  // ! sched() 호출이 안된건가? 

  // * "sbrk에 의해 할당된 메모리는 프로세스 내의 모든 스레드가 공유 가능합니다."
  // * 그럼 growproc 할 때 한놈만 자라서는 안돼. 그럼 ptable 순회하면서 다 통일시켜줘야 하겠지? 
  // * 맞네

  // ! 문제 1. panic: thread zombie exit 뜸. 
  // * switchuvm을 main 말고 curproc으로 하니까 해결.
  // ! 문제 2. trap 14 즉 page fault 뜸.
  // * ptable 순회하면서 sz 통일시켜주니 해결.
  // ! 문제 3. test 아주 가끔씩 fail 떠.
  // * 아 sz 읽어오는 것도 보호를 해줘야겠네 lock 위치 바꿔보자
  // ! 문제 4. 또 아주 가끔씩 trap 14 엄청 많이 뜨는데? 
  // * 이거 sysproc에 myproc()->sz 호출하는게 수상하다

  struct proc *curproc = myproc();
  struct proc *main = curproc->main;
  struct proc *p;

  uint sz = curproc->sz; // ~ 이제부터는 항상 curproc->sz == main->sz가 되도록 유지해야돼 그럼. 
  int memlim = main->memlim; // & memlim 정보는 thread에 없. setmemorylimit할때마다 갱신시켜줄 바엔 그냥 여기서 main꺼를 참고하게 하자

  if (!holding(&pgdirlock))
    panic("pgdirlock");

  // acquire(&ptable.lock); // & lock 무조건 필요하다 그리고 sz 읽어오는 거도 lock으로 보호해줘야 함 * * * *

  if(n > 0)
  {
    if (!memlim || memlim >= sz + n)  // ? sz + n == memlim까지는 되는게 맞나?
    {
      if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      {
        return -1;
      }
    }
    else // & memlim이 0이 아니고 sz + n > memlim일때. 
    {
      cprintf("\nproc %s with memlim %d tried to grow to %d bytes\n", curproc->name, memlim, sz + n);
      return -1;
    }
  } 
  else if (n < 0)
  {
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
    {
      return -1;
    }
  }

  // & 모두의 sz 통일시키기. 이거 안하면 page fault 뜬다.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->main == main)
      p->sz = sz;
  }

  switchuvm(curproc); 
  // ! ! ! ! ! switchuvm 무조건 curproc으로 해줘야 하네
  // ! 여태껏은 main == curproc이어서 문제가 없었던 거구나.
  // release(&ptable.lock);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
// * memlim, stack size 복사
// TODO: thread fork도 생각해야됨
// * "fork에 의해 생성된 child process를 wait하는 thread는 반드시 fork를 실행했던 thread가 되어야하는지? 
// *    ⇒ 그렇습니다." 
// * 이러면 thread도 자식을 가질 수 있어야됨. 
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
  // * stacksize도 일단 복사해와야하고, stacksize에 맞는 영역 다 복사해와야하겠다
  // * 근데 exec에서 sz = allocuvm(~) 해주니까 영역부분은 생각 안해도 될듯??

  // ! ! ! LWP 간에 주소공간 공유는 프로세스의 페이지 테이블을 공유하는 것
  // ! 그럼 LWP의 fork은 copyuvm이 아니라 np->pgdir = curproc->pgdir 해주면 되나?
  // ! 아니지 그건 fork이 아니라 thread_create에서 해주면 될듯

  // * * * curproc이 thread일 수도 있다 * * * *

  // ~ 여기서 pgdir 복사해 올 때도 lock 필요하겠다.
  acquire(&pgdirlock);

  // ~ curproc이 thread이어도 curproc->pgdir == main->pgdir이니까 main으로 갈 필요 없다
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    np->main = 0;

    release(&pgdirlock);
    return -1;
  }

  np->sz = curproc->sz; // ~ sz도 마찬가지로 main->sz랑 같을 것임. 
 
  release(&pgdirlock);

  np->parent = curproc; 
 // * memlim이랑 stacksize는 그래도 main에만 저장할거임
  np->memlim = curproc->main->memlim;
  np->stacksize = curproc->main->stacksize; 

  // Clear %eax so that fork returns 0 in the child.
  *np->tf = *curproc->tf; // ? 얘는 왜 * 앞에 붙이고 한거지
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
// // reap_thread로 thread 전부 정리하기. thread는 이거 호출하면 안됨.
// * 아니면 kill 할 때 싹 다 killed 1 해주고, exit에서 curproc이 thread면 대신 thread_exit 호출하는건 어떨까?
// * 일반 thread면 원래 thread_exit 호출하겠지만 main이 kill되면 trap에서 exit 호출할테니까

// * kill에서는 main만 killed 마킹. main은 exit에서 thread_kil_others로 자식들 killed 마킹.

// ! 개망했다. "하나의 스레드에서 exit이 호출되면 그 프로세스 내의 모든 스레드가 모두 종료되어야 합니다." ㅋ
// ! 그럼 갈아엎어야함. 생각 좀 해보자. 사실상 지금 꼬인 이유는 kill 때문임. kill , 
// ! thread_kill_others 여기서도 쓰면 어떨까 아니 이미 쓰기는 하는데 thread 가 exit 호출했어도 호출하면
// ! 다른 thread들 전부 잘 종료시키고, 본인도 죽겠네. 
// ! 생각해보니 kill했던거랑 같은 셈인가? 맞는듯? 이전에 kill에 쓴 디자인이 걍 exit인듯?
// ! thread에 kill 하면 main이 killed 1 되고 main이 exit할때 나머지 다 조지고 떠나심
// ! 그럼 맞는거같은데. 그럼 exit도 그냥 똑같이 main을 죽이는거랑 다름 없지 않나?
// ! 근데 thread_kill_others의 로직이 이 exit에 의존하잖아 서로 의존하게 됐네 좆됐다
// ! thread_kill_others는 유지하는 쪽으로 생각해보자
// ! 그럼 문제는 exit과 thread_exit을 구분하는 건데
// ! 그럼 exit과 thread_exit의 차이를 생각해보자. 나눠야 하는 이유가 진짜 있는지를
// ! 안 나누고 exit에서 걍 몰라라 개나소나 thread_kill_others 호출하면 재귀도 아니고 뭔 ㅆㅂ 
// ! 아니면 그냥 trap에서 확인할까? trap에서 killed = 1이면 또 thread인지 아닌지를 구분하는거지. 오??
// ! 해보자
// ! 아니 보류. trap에서 myproc()으로 계속 비교하고 그러니 더럽고, 기분탓인지 xv6 좀 느려지는 거 같음

// ! ! ! ! 문제상황: thread가 "직접" exit을 호출한 것과 kill되어서 exit이 호출된 것인지를 구분해야해 ! ! ! !

// * * * * 아이디어 생김. 
// *
// * 지금 문제는 thread가 exit을 호출했을 때, thread_exit 말고 kill others 까지 하는 일반 exit 로직을 실행해야 하는거잖아?
// * 생각해보니 내가 여기 exit에서 thread의 경우 thread_exit으로 보낸 이유는 결국 kill 때문이었어
// * 즉 지금 디자인에서 애초에 exit->thread_exit으로 가는 경우는 main의 kill 때문에 자기까지 죽는 경우를 고려한거잖아. 
// * trap에서 curproc->killed의 경우 구분 없이 exit 호출하니까. 그니까 현재 내 로직에서 exit->thread_exit 가는 경우는 main이 kill 된 경우만이다.
// * * * * * 그럼 thread가 "직접" exit을 호출한다면 main이 killed되지 않은 상황이겠다 * * * * * *
// * 그럼 curproc->main != curproc && curproc->main == killed인 경우에만 thread_exit으로 가면 되겠네.
// * thread가 직접 호출했으면 main은 살아있을테니 thread_kill_others로 나머지 다 조지고 자신도 ZOMBIE로 가면 되고.
// * 
// * 아니지 잠시만. 이러면 main 비교할 것도 없이 그냥 자신의 killed만 확인하면 되는거 아냐? 맞네
// * main이 kill되면 main의 exit에서 thread_kill_others로 thread들 전부 kill 마킹해줘서 얘들에 또 exit 호출되는거니까
// * * * * * 그럼 curproc이 thread이고 + killed까지 되어있으면 thread_exit으로 가면 되겠다 * * * * * * *
// * exit에서 thread가 killed되어 있다 = main의 사망이니까 thread_exit으로 간다.
// * thread가 killed되어 있지 않다 = 자발적 exit이니까 테스트코드 명세대로 모든 스레드 종료
// * 좋다 ㅅㅅㅅ

void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // & thread에 exit이 호출되는 경우는 두 가지:
  // &  1. thread의 main이 thread_kill_others로 자식 thread들 전부 kill 마킹해준 경우.
  // &  2. 테스트코드 명세처럼 thread가 자발적으로 exit 호출한 경우.
  if (curproc->main != curproc && curproc->killed)
  {
    thread_exit(0);
  }
  // & 나머지는 curproc이 thread가 아니거나, thread의 자발적 exit이니까 그대로 가면 된다 * * *


  // & 자식들 다 죽이고 자원회수. 즉 이 함수가 끝날때면 thread들은 모두 UNUSED 상태까지 됨
  thread_kill_others();
  // * * * 이거 원래 curproc->state = ZOMBIE 다음에 호출했던거 여기서 하니까 재부팅 해결되는데?? * * * * * * 
  // * 분명 아래 코드 중에 thread_kill_others 전에 실행되면 안되는 것들이 있으니까 이러겠지 호출 내려보면서 확인해보자
  // ! 아니 ㅆㅃ 출력문 지우니까 또 재부팅 나네 하 ㅋㅋㅋㅋㅋㅋㅋㅋㅋㅋㅋㅋㅋ

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

  // thread_kill_others(); // * 여기도 OK. 그럼 file 문제는 아니었던거고

  // Parent might be sleeping in wait().
  // if (curproc->killed)
    // cprintf("p%d: wakeup(%d)\n", curproc->pid, curproc->parent->pid);
  wakeup1(curproc->parent); // ! kill others 끝난다음에 wakeup 해줘야하나? 순서 의미 있나 lock 들었는데 <- 이게 맞았네

  // thread_kill_others(); // !!!! 여기서하니까 재부팅되네?
  // ! 그럼 wakeup이 문제였던거네? wakeup 한 다음에 thread_kill_others 하면 안되는 이유를 생각해보자 그럼
  // ! ! ! ! curproc->parent 깨우기 전에 kill_others 해야 한다 ! ! ! !
  // ! 다시 순서 생각해보자.. wakeup 한 다음에 kill_others 하는 상황에서
  // !    1. main (curproc)이 자기 부모를 깨운다
  // !    2. kill others로 본인 쓰레드 중 하나 t를 killed = 1 해준다
  // !    3. t에 thread_exit이 호출된다.
  // !    4. t가 main을 깨운다. 
  // ! 뭐가 문제여 ㅅㅂ
  // ! 아 자기 자식들 다 마무리시키고 
  // ! 모르겠다 ㄹㅇ. pmanager에서 kill 테스트할땐 왜 재부팅 문제 없었는지도 이해 안가고
  // ! 그냥 해결된거에 감사하고 넘어가자..

  // Pass abandoned children to init.
  // 이거 생각해보니까, 고아 thread들 모두 initproc에 입양되겠네. 아 이건 맞지
  // 애초에 thread는 main의 부모를 공유하기로 했으니까
  
  // ! 아니 이거 불안하다. wait에서 main인 녀석만 기다리게 했으니까, wakeup1(initproc)도 main에 대해서만 해야겠는데? 
  // ! multithread 자식이 고아가 되는 상황을 생각해보자.
  // ! parent가 있고 child가 있는데 child에 t1, t2, t3 thread들이 있다
  // ! parent 먼저 exit하면 여기서 t1 t2 t3 모두 initproc에 입양된다. ㅇㅋ 여기까진 좋음. 부모 공유할거니까
  // ! 근데 wakeup이 좀 걱정되네. init은 계속 wait에서 돌고 있음. 그리고 wait은 main만 따지게 해둠.
  // ! 그럼 init은 main 하나만 wait하는건데 t1 t2 t3 전부 zombie면 3번 깨우겠네? 
  // ! 근데 wait에서 wakeup하는거 보니까 어차피 다시 깨어나서 다시 ptable 순회함. 이러면 뭐 크게 문제될 건 없을듯?
  // ! 다만 main 아닌 자식 하나만 zombie인 상태에서 wakeup 시키면 
  // ! 하 모르겠네
  // ! 이러면 그냥 wait을 좀 고치자. main이 아니더라도 내 자식이면 havekids = 1 해보자 
  // ! 아니 그냥 깨워주는 거만 main이 ZOMBIE인지 확인하면 될텐데. 
  // ! 그럼 좀 더럽더라도, p->main == p 인 애들만 찾고, 아니다 너무 더럽다 2중반복문이야
  // ! 아니지 그럼 그냥 p == p->main && p->state == ZOMBIE 인 경우에만 깨워주면 되겠는데? 맞네
  // ! 아 그리고 어차피 내가 지금 고민하는 상황은 join 안하고 exit한 상황이겠네 ㅇㅋ 해결
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      // cprintf("p%d passed to init\n", p->pid);
      p->parent = initproc; // ! 이거 지우니까 재부팅 문제 좀 줄어드는데?? 아니네 뭐냐 ㅆㅂ
      // ! 가끔 되고 가끔 안되는거보면 race condition인가?
      if(p == p->main && p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
  
  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE; // ! main은 여기서 바로 죽는게 아니라 일단 ZOMBIE상태로

  // ! 원래 여기서 thread_kill_others 했는데 재부팅 문제 있었음. 여기가 뭐가 문제지? 
  // cprintf("p%d exit\n", curproc->pid);

  /*
  ! ! ! ! ! ! ! ! 재부팅 이유 알 것 같다. thread 여럿 있는 자식이 고아가 될 때 문제되는 거 같은데? ! ! ! ! ! ! ! !
  ! 
  */

  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);

  for(;;){ // ? 이 무한 반복문의 이유를 알고 싶다.
  // 알거같다. 아니다 모르겠다.

    // Scan through table looking for exited children.
    havekids = 0; 
    // ? 5. 이걸 다시 0으로 해주네?
    // ?    방금 순회할 때는 내 자식이었던게 내 자식이 아니게 될 수 있나?
    // ?    ZOMBIE 상태에서 기다려도 내 자식이잖아. 그냥 curproc->killed 확인하려고 이러는건가?

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){

      // ! ! ! ! ! ! ! wait의 배신인가. 자식이 thread 여러개면 자신을 부모로 가지는 p도 여럿이겠네? 
      // ! 그러면 p를 조질게 아니라 p->main을 조지면 되는건가?
      // * * * * 와 이러니까 재부팅 해결이다..........................
      // * 자식 중 main만 wait. main이 ZOMBIE 되기 전에 thread들을 다 회수할꺼니까
      if(p->parent != curproc || p != p->main)
        continue;
      
      havekids = 1;
      

      // ? 자식이 있는데 얘가 ZOMBIE가 아니면 다시 반복하는 듯. 이럴 거면 ZOMBIE가 아니면 sleep 하면 안됨?
      // ? 다시 반복하면 ptable 다시 순회해야하잖아
      // ? 자식 찾았는데 아직 ZOMBIE가 아니면 어떻게 될지 생각해보자.
      // ? --------- START HERE -------------
      // ? 1. 자식 확인했는데 ZOMBIE 아님. havekids = 1

      // if(p->state == ZOMBIE){ 
      //   // Found one.
      //   pid = p->pid;
      //   kfree(p->kstack);
      //   p->kstack = 0;
      //   freevm(p->pgdir);
      //   p->pid = 0;
      //   p->tid = 0;
      //   p->parent = 0;
      //   p->main = 0;
      //   p->name[0] = 0;
      //   p->killed = 0;
      //   p->state = UNUSED;
      //   release(&ptable.lock);
      //   return pid;
      // }

      if(p->state == ZOMBIE){ 
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->tid = 0;
        p->parent = 0;
        p->main = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }

      // ? 2. 얜 ZOMBIE 아니니까 계속 나머지 ptable 순회. 자식은 여럿일 수 있으니까. 
      // ?    근데 thread_join하는건 tid 인자로 해서
      // ?    딱 한놈만 기다릴거니까 이게 필요 없지 않나? 즉 tid 맞는애만 찾으면 거기서 기다리면 안됨?
    } 

    // No point waiting if we don't have any children.
    // ? 3. 본인이 kill 되었는지도 확인하는구나. 확실히 그냥 sleep만 쭉 걸어두면 이걸 못하긴하네.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    // ? 4. 쳐잠
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep

    // ? sleep 한 다음에도 killed 확인해줘야 하는거 아님>?
    // ? 아 근데 자는 중에 kill되는건 처리가 되니까 걱정할 필요가 없어서 그런거 같다.
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
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p); // TODO: 이거도 좀 생각해봐야 해. 같은 process의 thread면 이거 안해도 되는거 아냐? 
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

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
  int intena;
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
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
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
  // ! lk == ptable.lock은 맞는데 acquire는 안된 상태라 여기로 와도 lock 안 잡힌 상태
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  // if(!holding(&ptable.lock))
  // {
  //   cprintf("%s calls sched, in sleep() without holding ptable.lock\n", p->name);
  // ! thread_exec calls sched, in sleep() without holding ptable.lock
  // ! lock을 안 잡은채로 sched 호출함.
  // ! 그럼 thread_kill_others할때 lock을 잡는게 맞네. exit에서 이미 잡고 해서 안했는데 필요하다
  // }
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
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
    {
      p->state = RUNNABLE;
      // cprintf("woke up %d\n", p->pid);
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
int
kill(int pid)
{
  // ~ idea 1.
  // ~ 하나 이상의 스레드가 kill 되면 프로세스 내의 모든 스레드가 종료되어야 합니다.
  // ~ 그럼 p->main만 killed를 마킹해주면 어떨까. killed 체크해서 exit 해주는건 trap에서 하니까,
  // ~ 이러면 exit 함수만 좀 고쳐주면 되겠는데. exit할때 자기 thread들 전부 죽이게끔

  // & idea 2. (SCRAPPED)
  // & 여기서 main이랑 얘의 모든 thread들 kill 마킹해주기. 그리고 exit에서는 thread_exit 호출.
  // & 이러면.. ㅅㅂ fileclose 이런거 한번씩 다 할 수 있으니까 좋은거같기도 하고 ㅅㅂ ㅁㄹ 일단 이대로 테스트해본다
  // & 해보니까 애들 zombie로 남아버린다. thread_exit 가서 zombie로 가는데 부모 안 바꿔줬으니 이런듯. 고아 되어서. 
  // & 즉 zombie 처리 해주는게 필요해. wait 이나 join 처럼. 오?
  // & 그럼 이렇게 kill 할 때 다 kill 마킹해주고, main의 exit에서 join을 하게 할까?
  // & 그럼 순서는:
  // &    1. kill로 main이랑 얘 thread 전부 kill 마킹.
  // &    2. trap에서 killed 된 애들 exit 호출시킴.
  // &    3. thread의 exit은 thread_exit으로 이동.
  // &    4. main thread의 exit은 기존 exit 로직 수행
  // &       main은 wait 로직처럼 thread들 전부 reap 해주기.
  // &
  // & 성공
  // &
  // & thread_kil_others 로직을 바꿔봄. 이젠 thread_kil_others 자체에서 curproc의 thread들을 전부 kill 마킹해주고, 자원회수도 함.
  // & 그리고 thread_kil_others curproc을 죽이는걸 안하니까, 이 kill에서 thread_kil_others 해준 다음에 .. 아니 이렇게 되면 다시 idea 1이 낫겠다?

  // ~ idea 1 again. * * * * * * 
  // ~ main만 kill 마킹해주고, exit할 때 thread_kil_others 호출하자. 이게 왜 나을 것 같냐면, 어차피 exit할 때 그런 작업이 필요할 거 같긴해서.
  // ~ 예외상황이지만, 이 아이디어면, join 안하고 exit하는 경우에서도 thread들 다 죽었는지 확인할 수 있을테니까
  // ~ 그리고 exec할 때도 main 빼고 일단 다 죽이고 해야 하니까 똑같이 thread_kil_others 재활용할 수 있겠다. 이걸로 다시 해보자
  // ~ 그럼 kill 순서는:
  // ~    1. main만 killed = 1. 이러면 thread들이 trap때문에 exit을 거치는 일은 없을 거야. 그럼 exit에서 curproc이 thread인지 확인 필요 x
  // ~       아니다. thread_kil_others kill 마킹된 thread들도 결국 exit을 거치긴 하네. 그럼 thread인지 확인은 해야됨
  // ~    2. 그럼 main만 exit을 거칠테고, 이 때 thread_kil_others 나머지 thread들 살인.

  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid) {
      p->main->killed = 1; // ! main만 조지기
      if(p->main->state == SLEEPING)
        p->main->state = RUNNABLE;
      
      // cprintf("kill: pid %d done\n", p->pid);

      release(&ptable.lock);
      return 0;
    }
  }
  
  release(&ptable.lock);
  return -1;
}

void
proclist(void)
{
  /*
    " 현재 실행 중인 프로세스들의 정보를 출력합니다. (RUNNABLE, RUNNING, SLEEPING)
      각 프로세스의 이름, pid, 스택용 페이지의 개수, 할당받은 메모리의 크기, 메모리의 최대 제한이 있습니다.
      * Process가 실행한 Thread의 정보를 고려하여 출력하여야 합니다. 
        TODO <= Thread가 실행줌ㅇ이면 main이 실행중이라고 출력하자
      Thread의 경우 출력하지 않습니다.
      프로세스의 정보를 얻기 위한 시스템 콜은 자유롭게 정의해도 됩니다.
  */

  // procdump은 lock 안걸었는데 여기엔 해볼래
  // 근데 이거 출력할떄마다 무조건 pmanager만 run 상태네,

  struct proc *p;
  // struct proc *curproc = myproc();
  char *state;

  acquire(&ptable.lock);

  cprintf("\nNAME\t\t\tPID\tSTATE\tSIZE\tSTCKSZE\tMEMLIM\n");
  cprintf("_______________________________________________________________\n");

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p != p->main) continue; // Thread의 경우 출력하지 않습니다. p는 0으로 초기화 될 일 없을듯

    if (p->state == RUNNING)
    {
      state = "run";
    }
    else if (p->state == RUNNABLE)
    {
      state = "ready";
    }
    else if (p->state == SLEEPING)
    {
      state = "sleep";
    }
    else continue;
    
 // cprintf("NAME\t\tPID\tSTATE\tSIZE\tSTCKSZE\tMEMLIM\n");
    cprintf("%s\n\t\t\t%d\t%s\t%d\t%d\t%d\n", p->name, p->pid, state, p->sz, p->stacksize, p->memlim);
    cprintf("_______________________________________________________________\n");
  }

  cprintf("\n");
  release(&ptable.lock);
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
  // int i;
  struct proc *p;
  char *state;
  // uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %d %s %s", p->pid, p->tid, state, p->name);
    if (p->killed)
      cprintf(" KILLED");
    // if(p->state == SLEEPING){
    //   getcallerpcs((uint*)p->context->ebp+2, pc);
    //   for(i=0; i<10 && pc[i] != 0; i++)
    //     cprintf(" %p", pc[i]);
    // }
    cprintf("\n");
  }
}
