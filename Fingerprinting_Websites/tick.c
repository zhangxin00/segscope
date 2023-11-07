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


int inter_count[100001],cycles;
double score[512]={0};

extern char stopspeculate[];


void prepare() { 
  uint16_t value=1;
  __asm__ volatile("mov %0, %%gs" : : "r"(value));
}


uint16_t get_gs() {
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
        int ret,  i,count;
        cpu_set_t get;   
        uint16_t orig_gs = get_gs();
        CPU_ZERO(&get);	
        pin_cpu3();
    
   	if(sched_getaffinity(0,sizeof(get),&get)==-1)
    	{
   	 printf("warning: can not get cpu affinity/n");
    	}
   

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
		
    	

	return count;



}
int read_addr(int cycles)
{
	int i, j, ret = 0, max = -1, maxi = -1,count;
	unsigned long addr;
	int base=0x80800000;
	uint16_t orig_gs = get_gs();
	set_gs(1);
        while (get_gs() == 1);

	for (i = 0; i < cycles; i++) {		

     	count=0;
     		
     	//get the start of the time slice
	
               
        //use the whole time slice to count
	set_gs(1);
  	
    	while (get_gs() == 1)
    	{  
    	++count;
    	}
    	set_gs(orig_gs);
    
    	inter_count[i]=count;
		
	}
	

	return 0;
}

static char *progname;

int usage(void)
{
	printf("%s: [cycles]\n", progname);
	return 1;
}




//just printf
int compute()
{

  	double var=0,avg=0,standard;
  	for(int j=0;j<cycles;j++)
  	{
    	 
   	  avg+=inter_count[j];
  
  	}
   	avg=avg/cycles;
	for(int j=0;j<cycles;j++)
  	 {	   
  	 
	  var+=pow(inter_count[j]-avg,2)/cycles;
  	}

  	 standard=pow(var,0.5);
	for(int j=0;j<cycles;j++)
	{
	      printf("%d\n",inter_count[j]);
	}
	printf("avg=%f, standard=%f\n",avg,standard);
	  
	 
	return 0;
}
 

int main(int argc, char *argv[])
{
	int ret,  i;
        cpu_set_t get;   
        CPU_ZERO(&get);
	progname = argv[0];
	if (argc < 2)
		return usage();


	if (sscanf(argv[1], "%d", &cycles) != 1)
		return usage();


	
        pin_cpu3();
    
   	if(sched_getaffinity(0,sizeof(get),&get)==-1)
    	{
   	 printf("warning: can not get cpu affinity/n");
    	}
    	
  	for(int i=0;i<8;i++) {
	if(CPU_ISSET(i,&get))
	{
		printf("this thread %d is running on processor %d\n", gettid(),i);
	
	}
	}
	read_addr(cycles);
	compute();
	return 0;
	
	
}
