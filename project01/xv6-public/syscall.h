// System call numbers
#define SYS_fork    1
#define SYS_exit    2
#define SYS_wait    3
#define SYS_pipe    4
#define SYS_read    5
#define SYS_kill    6
#define SYS_exec    7
#define SYS_fstat   8
#define SYS_chdir   9
#define SYS_dup    10
#define SYS_getpid 11
#define SYS_sbrk   12
#define SYS_sleep  13
#define SYS_uptime 14
#define SYS_open   15
#define SYS_write  16
#define SYS_mknod  17
#define SYS_unlink 18
#define SYS_link   19
#define SYS_mkdir  20
#define SYS_close  21
#define SYS_myfunction 22
#define SYS_schedulerLock 23
#define SYS_schedulerUnlock 24
#define SYS_getLevel 25
#define SYS_setPriority 26
#define SYS_yield   27

// usys.S에서 이 숫자를 %eax 레지스터에 올리고, int 64(syscall). 그럼 trapno에 64 들어감
// 다음 trap.c에서 trapno == 64인 경우 syscall.c에 있는 syscall()호출
// syscall()에서 아까 올려놓은 %eax값(위 숫자들) 을 key로 써서 이에 맞는 sys_~함수 호출