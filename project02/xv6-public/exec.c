#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"

// https://www.youtube.com/watch?v=bp0qo4-ozEg  40분쯤 부터
// TODO: exec이 실행되면 기존 프로세스의 모든 스레드들이 정리되어야 합니다.
// * thread도 exec을 호출할 수 있다고 가정하자. 그럼 여기서도 그냥 curproc 말고 curproc->main에 작업해줘야되겠음.
// * exec2도 마찬가지로 curproc->main의 stacksize를 지정하게하고.
// * 그리고 reap_all_threads 호출해서 자기 스레드들 다 죽여야된다.

// ~ thread_kill_others 대공사 하고 왔습니다. 이제 thread가 exec 호출해도 문제 없을거임 아마. 
// ~ 그럼 여기서는 curproc->main할 필요 없다 차피 curproc이 스레드든 main이든간에 지만 살아남으면 됨

// ! lock 해결하니까 뭔가 되기는하는데 xv6가 재부팅한다. 
int
exec(char *path, char **argv)
{
  char *s, *last;
  uint argc, sz, sp, ustack[3+MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir; // * pde_t = Page Table
  struct proc *curproc = myproc();
  int i, off;
  int stacksize = curproc->stacksize;

  // thread_kill_others(); // exec 실패하면 이걸 하면 안돼. 성공해야만 이걸 해야됨

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // Check ELF header
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  // * allocate new page table
  if((pgdir = setupkvm()) == 0)
    goto bad;

  // Load program into memory.
  // 각 elf header 마다?
  sz = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    // * allocate memory for each ELF segment
    if((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    // * load mappings?
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  // Allocate two pages at the next page boundary.
  // * * * Make the first inaccessible.  Use the second as the user stack. * * *
  // ! first = 가드용, second = 스택용?
  sz = PGROUNDUP(sz);
  if((sz = allocuvm(pgdir, sz, sz + (stacksize + 1) * PGSIZE)) == 0) // allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
    goto bad;
  clearpteu(pgdir, (char*)(sz - (stacksize + 1) * PGSIZE));
  // * clearpteu: Used to create an inaccessible page beneath the user stack => 가드용 스택?
  sp = sz; // * * * sp = 스택의 시작주소
  // * * * 가드용이랑 스택용 페이지 할당한 다음에 sz를 sp로 해놓고, 아래 작업하면서 sp를 감소시키면 될듯

  // * * * 인자 
  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    
    // * strlen은 뭔지 모르겠는데 & ~3은 align 하려고 한다네
    // * 암튼 sp 감소 시켜서 push
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;

    // * argv[argc]의 내용을 sp에 저장한다 => 인자를 stack에 push
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0) // ! 
    //                va,     p,                 len
      goto bad;
    ustack[3+argc] = sp; // * 각 인자에 대한 pointer를 ustack에 저장하는듯. 나 pmanager에서 parse한 거 같은 느낌?
  }
  ustack[3+argc] = 0; // * 마지막 인자 다음 pointer를 0으로?

  // * * * * 아 main 함수 인자 즉 실행인자 넘기는거라 그렇구나 * * * * * * * * * * * * * * * *
  // * main(int argc, char **argv)니까 return 주소, argc, argv 이 순서대로 stack에 push하는거네 와
  // * caller frame 채우는거. 인자, ret 주소 push하는거 시프때 봤던 내용이다 ㅅㅅㅅㅅㅅㅅㅅㅅㅅㅅㅅㅅㅅㅅㅅ
  ustack[0] = 0xffffffff;  // fake return PC // & <= 이건 어차피 return 안할 거니까 암거나 넣은듯
  ustack[1] = argc;
  ustack[2] = sp - (argc+1)*4;  // argv pointer

  // * 위에서 처럼 copyout 쓰기 전에 sp를 먼저 감소시켜야 됨
  // * 공간을 미리 만들어 둔 다음에 거기다가 copyout을 하는 느낌이네
  sp -= (3+argc+1) * 4;
  // * ustack의 내용을 sp에 저장
  if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0)
    goto bad;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));
  
  // cprintf("exec finishing up.\n");

  // ! ! ! ! thread_kill_others 끝나기전에 exec이 먼저 끝나버리는듯????? 근데 이게 재부팅 원인이 되나?
  // ! 

  thread_kill_others(); // 여기 해도 문제 없네

  // Commit to the user image.
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz = sz;
  curproc->tf->eip = elf.entry;  // * * * main
  curproc->tf->esp = sp; // * * * 
  switchuvm(curproc);
  freevm(oldpgdir);
  return 0;

 bad:
  if(pgdir)
    freevm(pgdir);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

int exec2(char *path, char **argv, int stacksize)
{
  if (stacksize < 1 || stacksize > 100) 
    return -1;
  
  // 흠 ㄴㄴ 이게 맞음. myproc이 thread든 main이든 thread_kill_others하면 myproc만 살아남을거니까
  myproc()->stacksize = stacksize;
  return exec(path, argv);
}
