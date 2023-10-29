#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

extern int schedlock;

// uint fakeTicks; 
// int tickIncrementInterval = 1; // for debugging
int doprintqueue = 0;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0); // dpl: 0 => default: kernel mode

  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);
  // only int syscall can be called from user-level. DPL_USER = 3

  SETGATE(idt[128], 1, SEG_KCODE<<3, vectors[128], DPL_USER);
  SETGATE(idt[SCHEDLOCK], 1, SEG_KCODE<<3, vectors[SCHEDLOCK], DPL_USER);
  SETGATE(idt[SCHEDUNLOCK], 1, SEG_KCODE<<3, vectors[SCHEDUNLOCK], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  // int prboostd = 0;
  int tickflag = 0; // debug

  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER: // timer interrupt
    if(cpuid() == 0){
      acquire(&tickslock);

      // if (++fakeTicks % tickIncrementInterval == 0)
      // {
        ticks++;
        tickflag = 1;

      // }
      
      if (ticks == 100) {
        // if (0)
        // {
        prboost();
        ticks = 0;
        // prboostd = 1;
        // }
      }

      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;
  case 128:
    mycall();
    exit();
    break;
  case SCHEDLOCK:
    schedulerLock(2020059152);
    break;
  case SCHEDUNLOCK:
    schedulerUnlock(2020059152);
    break;

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  // timer interrupt
  // schedlock 걸렸으면 yield 안함
  // * priority boosting 하면 yield를 어쩌나.............
  if(
    // tickflag &&
    // !prboostd && 
    !schedlock &&
    myproc() && myproc()->state == RUNNING && // !schedlock && 
     tf->trapno == T_IRQ0+IRQ_TIMER) {
      // if (--myproc()->tq <= 0) {
        myproc()->tq--;
        yield(); // 
      // }
  }

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  
  if (doprintqueue && tickflag)
    printqueue();
}
