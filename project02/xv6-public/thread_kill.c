#include "types.h"
#include "stat.h"
#include "user.h"

#define NUM_THREAD 5

void *thread1(void *arg)
{
  sleep(200);
  printf(1, "This code shouldn't be executed!!\n");
  exit();
  return 0;
}

void *thread2(void *arg)
{
  int val = (int)arg;
  sleep(100);
  if (val != 0) {
    printf(1, "Killing process %d\n", val);
    kill(val);
  }
  printf(1, "This code should be executed 5 times.\n");
  // printf(1, "\n");
  thread_exit(0);
  return 0;
}

thread_t t1[NUM_THREAD], t2[NUM_THREAD];

int main(int argc, char *argv[])
{
  int i, retval;
  int pid;
  
  printf(1, "Thread kill test start\n");
  pid = fork();
  if (pid < 0) {
    printf(1, "Fork failed!!\n");
    exit();
  }
  else if (pid == 0) { // 자식: thread1 생성. exit 찍기 전에 죽어야됨.
    // printf(1, "child p%d creates [", getpid());
    for (i = 0; i < NUM_THREAD; i++)
    {
      thread_create(&t1[i], thread1, (void *)i);
      // printf(1, "%d", t1[i]);
      // if (i != NUM_THREAD - 1)
        // printf(1, ",");
      // else
        // printf(1, "]\n");
    }
    // printf(1, "\n");
    sleep(300);
    printf(1, "This code shouldn't be executed!!\n");
    exit();
  }
  else { // 부모: 자식 죽이는 thread2 생성
    // printf(1, "parent: p%d\n", getpid());
    // printf(1, "child : p%d\n", pid);
    // printf(1, "parent p%d creates [", getpid());

    for (i = 0; i < NUM_THREAD; i++) {
      if (i == 0)
        thread_create(&t2[i], thread2, (void *)pid);
      else
        thread_create(&t2[i], thread2, (void *)0);

      // printf(1, "%d", t2[i]);
      // if (i != NUM_THREAD - 1)
        // printf(1, ",");
      // else
        // printf(1, "]\n");
    }
    for (i = 0; i < NUM_THREAD; i++) // 설마 join이 안되는건가?
      thread_join(t2[i], (void **)&retval);
    while (wait() != -1);
    printf(1, "Kill test finished\n");
    exit();
  }
}
