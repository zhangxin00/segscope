/*
 *use a whole time slice to count;
 */

#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sched.h>
#include <pthread.h>
#include <semaphore.h>
#include <x86intrin.h>

int inter_count[100001], cycles;
double score[512] = {0};

extern char stopspeculate[];

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

int multiply(int num1, int num2)
{
	return num1 * num2;
}
static void pin_cpu3()
{
	cpu_set_t mask;

	/* PIN to CPU0 */
	CPU_ZERO(&mask);
	CPU_SET(3, &mask);
	sched_setaffinity(0, sizeof(cpu_set_t), &mask);
}
int count_tick()
{
	int ret, i, count;
	cpu_set_t get;
	CPU_ZERO(&get);
	pin_cpu3();

	if (sched_getaffinity(0, sizeof(get), &get) == -1)
	{
		printf("warning: can not get cpu affinity/n");
	}

	prepare();

	while (check() == 1)
	{
		count++;
	}

	return count;
}

static char *progname;

int usage(void)
{
	printf("%s: [cycles]\n", progname);
	return 1;
}

int main(int argc, char *argv[])
{
	int ret, i;
	cpu_set_t get;
	CPU_ZERO(&get);
	progname = argv[0];
	if (argc < 2)
		return usage();

	pin_cpu3();

	if (sched_getaffinity(0, sizeof(get), &get) == -1)
	{
		printf("warning: can not get cpu affinity/n");
	}

	for (int i = 0; i < 8; i++)
	{
		if (CPU_ISSET(i, &get))
		{
			printf("this thread %d is running on processor %d\n", gettid(), i);
		}
	}
	return 0;
}
