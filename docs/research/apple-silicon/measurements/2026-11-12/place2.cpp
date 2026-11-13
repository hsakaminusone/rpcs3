#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include <sched.h>

static std::atomic<uint64_t> g_end{0};
struct res_t { uint64_t iters; int qos; int pol; int prio; } g_res[16];
static uint64_t now_ns(){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec; }

static void* worker(void* p){
  int idx=(int)(intptr_t)p;
  qos_class_t qc{}; int rel{}; pthread_get_qos_class_np(pthread_self(),&qc,&rel);
  int pol{}; sched_param sp{}; pthread_getschedparam(pthread_self(),&pol,&sp);
  volatile uint64_t x=1+idx; uint64_t n=0;
  for(;;){ for(int i=0;i<4096;i++){ x=x*6364136223846793005ull+1442695040888963407ull; x^=x>>29; } n+=4096;
    if((n&0xFFFFF)==0 && now_ns()>=g_end.load(std::memory_order_relaxed)) break; }
  g_res[idx]={n,(int)qc,pol,sp.sched_priority}; return nullptr;
}

/* mode 0: SCHED_OTHER policy default prio; 1: SCHED_RR 99; 2: QoS UI; 3: SCHED_RR 99 then set_native_priority-style max */
static void run(const char* name, int nthreads, int mode, int secs=2){
  std::vector<pthread_t> ts(nthreads);
  g_end.store(now_ns()+(uint64_t)(secs+10)*1000000000ull);
  for(int i=0;i<nthreads;i++){
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a,0x800000);
    if(mode==1||mode==3){ sched_param sp{}; sp.sched_priority=99; pthread_attr_setschedpolicy(&a,SCHED_RR); pthread_attr_setschedparam(&a,&sp); }
    if(mode==2){ pthread_attr_set_qos_class_np(&a,QOS_CLASS_USER_INTERACTIVE,0); }
    pthread_create(&ts[i],&a,worker,(void*)(intptr_t)i);
  }
  usleep(250000); g_end.store(now_ns()+(uint64_t)secs*1000000000ull);
  for(auto t:ts) pthread_join(t,nullptr);
  uint64_t total=0; std::vector<uint64_t> v;
  for(int i=0;i<nthreads;i++){ total+=g_res[i].iters; v.push_back(g_res[i].iters); }
  std::sort(v.begin(),v.end());
  printf("%-28s total=%8.2f Miters/s  min=%7.2f max=%7.2f  qos=%d pol=%d prio=%d\n",
    name,total/1e6/secs,v.front()/1e6/secs,v.back()/1e6/secs,g_res[0].qos,g_res[0].pol,g_res[0].prio);
}
int main(){
  run("8 SCHED_OTHER default prio",8,0);
  run("8 SCHED_RR 99",8,1);
  run("8 QoS UI",8,2);
  run("8 SCHED_RR 99 (again)",8,3);
  return 0;
}
