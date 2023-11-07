/* For Intel, we use rdtsc as baseline */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define _GUN_SOURCE
#ifdef _MSC_VER
#include <intrin.h> /* for rdtscp and clflush */
#pragma optimize("gt", on)
#else
#include <x86intrin.h> /* for rdtscp and clflush */
#endif
#define __USE_GNU
#include <sched.h>
#include <pthread.h>
/* sscanf_s only works in MSVC. sscanf should work with other compilers*/
#ifndef _MSC_VER
#define sscanf_s sscanf
#endif
#define T 1000
int cc[T];
unsigned long long reth1;
unsigned long long retl0;

void prepare() {
  uint16_t value=1;
  __asm__ volatile("mov %0, %%gs" : : "r"(value));
}

unsigned long long get_cpu_cycle()
    {
        __asm__ __volatile__(
        "rdtsc" :                   // rdpru is more accurate than rdtsc on AMD processors
        "=d" (reth1),
        "=a" (retl0)
        );
        return ((reth1 << 32)|(retl0));
    }

/* Report best guess in value[0] and runner-up in value[1] */
double count_sum=0;
long time_sum=0;
void test_garnularity()
{
	int i;
	unsigned int junk = 0;
	register uint64_t time1, time2;
        unsigned long long tsc0,tsc1;
        
        //warm up
        
        for (i=0; i<1000; i++)
	{

		unsigned int count=0;
		prepare();
		
		__asm__ __volatile__(
		//"CLI\n\t"
		"mov $0,%%eax\n\t"
		"L:\n\t" 
		"incl %%eax\n\t"
		"mov %%gs,%%ecx\n\t"
		"cmp $1,%%ecx\n\t" 
		"je L\n\t"
		"mov %%eax, %0" : "=r"(count)
		);
		printf("warm-up: %d\n",i);

		}
	// SegScope-based timer (asm version)

	for (i=0; i<T; i++)
	{

		unsigned int count=0;
		prepare();
		time1=get_cpu_cycle();
		for (volatile int z = 0; z < 100; z++)
		{
		} /* Delay (can also mfence) */

		__asm__ __volatile__(
		//"CLI\n\t"
		"mov $0,%%eax\n\t"
		"T:\n\t" 
		"incl %%eax\n\t"
		"mov %%gs,%%ecx\n\t"
		"cmp $1,%%ecx\n\t" 
		"je T\n\t"
		"mov %%eax, %0" : "=r"(count)
		);
		time2=get_cpu_cycle();
		printf("%llu,%llu,%lf\n",time2-time1,count,(double)(time2-time1)/count);
		cc[i]=count;
		time_sum+=(time2-time1);
		count_sum+=count;


		}
		printf("the granularity is: %lf\n",time_sum/count_sum);



}
int filter(int avg,int std)
{
	for(int j=0;j<T;j++)
  	{

   	  if(cc[j]>avg+2*std || cc[j]<avg-2*std)
   	         {cc[j]=0;}

  	}


}
int compute()
{


  	double var=0,avg=0,standard;
  	int Tnum=T;
  	for(int j=0;j<T;j++)
  	{

   	  if(cc[j]>0) {avg+=cc[j];}
   	  else {Tnum--;}

  	}
   	avg=avg/Tnum;
	for(int j=0;j<T;j++)
  	 {	   

	 if(cc[j]>0) { var+=pow(cc[j]-avg,2)/Tnum;}
  	}
	filter(avg,standard);
  	standard=pow(var,0.5);
  	printf("avg=%f, standard=%f,Tnum=%d\n",avg,standard,Tnum);
  	

	return 0;
}


static void pin_cpu3()
{
	cpu_set_t mask;

	/* PIN to CPU */
	CPU_ZERO(&mask);
	CPU_SET(3, &mask);
	sched_setaffinity(0, sizeof(cpu_set_t), &mask);
}

int main(int argc, const char* * argv)
{
	pin_cpu3();
	test_garnularity();
	compute();
	return (0);
}
