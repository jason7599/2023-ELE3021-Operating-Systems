#include "types.h"
#include "stat.h"
#include "user.h"

#define NUM_THREADS 5

void* test(void *arg);
void* multfive(void *arg);
void* increment_some_int(void *arg);
void* unending_thread(void *arg);
void* sleeping_thread(void *arg);

int some_int;

thread_t threads[NUM_THREADS];

void orphan_threads_test(void)
{
  int i;
  void *retval;

  if (fork() == 0) 
  {
    for (i = 0; i < NUM_THREADS; i++)
      thread_create(&threads[i], sleeping_thread, (void*)200);
    
    for (i = 0; i < NUM_THREADS; i++)
      thread_join(threads[i], &retval);
  }
  
  exit(); // 부모가 multithread 자식 wait 안해주고 exit하면?
} 

void thread_fork_test(void)
{
  
}

int main()
{

  exit();
}


void* sleeping_thread(void *arg)
{
    int t = (int)arg;

    printf(1, "sleep %d start\n", t);
    sleep(t);
    printf(1, "sleep %d finish\n", t);

    thread_exit(0);
    return 0;
}

void* test(void *arg)
{
    printf(1, "arg = %d\n", (int)arg);
    thread_exit((void*)1298);
    return 0;
}

void* multfive(void *arg)
{
    int res = (int)arg * 5;
    thread_exit((void*)res);
    return 0;
}

void* increment_some_int(void *arg)
{
    some_int++;
    thread_exit((void*)some_int);
    return 0;
}

void* unending_thread(void *arg)
{
    for(;;);
    thread_exit(0);
    return 0;
}
