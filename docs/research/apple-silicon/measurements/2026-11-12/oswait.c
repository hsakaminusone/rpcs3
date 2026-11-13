#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <os/os_sync_wait_on_address.h>

static _Atomic uint32_t g_val;
static _Atomic uint32_t g_done;
static uint64_t g_wake[1024];

static uint64_t ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static double cpu(void){ struct rusage r; getrusage(RUSAGE_SELF,&r); return r.ru_utime.tv_sec+r.ru_stime.tv_sec+(r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1e6; }

static void* waiter(void*){
  uint32_t last = 0;
  for (;;) {
    uint32_t v = atomic_load_explicit(&g_val, memory_order_acquire);
    while (v == last) {
      os_sync_wait_on_address((void*)&g_val, v, sizeof(v), OS_SYNC_WAIT_ON_ADDRESS_NONE);
      v = atomic_load_explicit(&g_val, memory_order_acquire);
    }
    if (v == 0xffffffffu) break;
    g_wake[v-1] = ns();
    atomic_store_explicit(&g_done, v, memory_order_release);
    last = v;
  }
  return NULL;
}

static void* idler(void*){ for(;;){ uint32_t v=atomic_load_explicit(&g_val,memory_order_acquire); if(v==0xffffffffu)break; os_sync_wait_on_address((void*)&g_val, v, sizeof(v), OS_SYNC_WAIT_ON_ADDRESS_NONE);} return NULL; }

int main(void){
  /* latency */
  atomic_store(&g_val,0); atomic_store(&g_done,0);
  pthread_t c; pthread_create(&c,NULL,waiter,NULL);
  usleep(20000);
  uint64_t v[500];
  for (uint32_t i=1;i<=500;i++){
    usleep(200);
    uint64_t t0=ns();
    atomic_store_explicit(&g_val,i,memory_order_release);
    os_sync_wake_by_address_any((void*)&g_val, sizeof(i), OS_SYNC_WAKE_BY_ADDRESS_NONE);
    while (atomic_load_explicit(&g_done,memory_order_acquire) < i) asm volatile("" ::: "memory");
    v[i-1]=g_wake[i-1]-t0;
  }
  atomic_store(&g_val,0xffffffffu);
  os_sync_wake_by_address_all((void*)&g_val, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
  pthread_join(c,NULL);
  /* sort */
  for (int i=0;i<500;i++) for (int j=i+1;j<500;j++) if (v[j]<v[i]){uint64_t t=v[i];v[i]=v[j];v[j]=t;}
  printf("os_sync wake ns: p50=%6llu p90=%6llu p99=%6llu max=%6llu\n",
    (unsigned long long)v[250],(unsigned long long)v[450],(unsigned long long)v[495],(unsigned long long)v[499]);

  /* cpu burn */
  atomic_store(&g_val,0);
  pthread_create(&c,NULL,idler,NULL);
  usleep(50000);
  double b=cpu(); usleep(500000); double a=cpu();
  atomic_store(&g_val,0xffffffffu);
  os_sync_wake_by_address_all((void*)&g_val, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
  pthread_join(c,NULL);
  printf("os_sync idle-wait CPU over 500 ms wall: %.4f s\n", a-b);
  return 0;
}
