#ifndef __MT_UTILS_H__
#define __MT_UTILS_H__


static void __attribute__((noinline)) barrier(int nCores)
{
  static volatile int sense;
  static volatile int count;
  static __thread int threadsense;

  __sync_synchronize();

  threadsense = !threadsense;
  if (__sync_fetch_and_add(&count, 1) == nCores-1)
  {
    count = 0;
    sense = threadsense;
  }
  else while(sense != threadsense)
    ;

  __sync_synchronize();
}


#endif //__MT_UTILS_H__
