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

static void __attribute__((noinline)) syncpoint(int nCores)
{
  static volatile int count;

  __sync_synchronize();

  if (__sync_fetch_and_add(&count, 1) != nCores-1) {
    while (count != nCores - 1)
      ;
  }

  __sync_synchronize();
}


static volatile int TICKET;
static volatile int TURN;

static int __attribute__((noinline)) acquire_lock() {
  static __thread int myturn;

  __sync_synchronize();
  myturn = __sync_fetch_and_add(&TICKET, 1);

  if (myturn == TURN) {
    // Acquire lock
  } else {
    // Spin until my turn comes
    while (myturn != TURN)
      ;
  }
  __sync_synchronize();
}

static int __attribute((noinline)) release_lock() {
  __sync_synchronize();
  __sync_fetch_and_add(&TURN, 1);
  __sync_synchronize();
}


#endif //__MT_UTILS_H__
