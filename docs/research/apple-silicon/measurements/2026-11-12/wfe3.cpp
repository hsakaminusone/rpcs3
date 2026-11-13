#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <atomic>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>
#include <vector>
#include <algorithm>

static std::atomic<uint32_t> g_go{0};
static std::atomic<uint32_t> g_stop{0};

static double cpu_seconds(){
  rusage ru{}; getrusage(RUSAGE_SELF,&ru);
  return ru.ru_utime.tv_sec+ru.ru_stime.tv_sec+(ru.ru_utime.tv_usec+ru.ru_stime.tv_usec)/1e6;
}

/* Each thread waits until g_go != 0 with the given primitive. */
template<int MODE> static void* waiter(void*){
  if constexpr (MODE==2) asm volatile("sevl" ::: "memory");   /* arm the event once */
  while (!g_go.load(std::memory_order_acquire)) {
    if constexpr (MODE==0) asm volatile("isb" ::: "memory");
    else if constexpr (MODE==1) asm volatile("yield" ::: "memory");
    else asm volatile("wfe" ::: "memory");
  }
  g_stop.store(1);
  return nullptr;
}

template<int MODE> static void burn(const char* name){
  g_go.store(0); g_stop.store(0);
  pthread_t c; pthread_create(&c,nullptr,waiter<MODE>,nullptr);
  usleep(50000);
  double before = cpu_seconds();
  usleep(500000);
  double after = cpu_seconds();
  g_go.store(1); pthread_join(c,nullptr);
  printf("%-6s waiting-thread CPU over 500 ms wall: %.3f s  (%.0f%% of a core)\n",
    name, after-before, (after-before)*200.0);
}

/* Wake latency: the waiter parks, then we publish and time until it reacts. */
static uint64_t g_t_wake;
template<int MODE> static void* lat_waiter(void*){
  if constexpr (MODE==2) asm volatile("sevl" ::: "memory");
  for (uint32_t want=1; want<=500; want++){
    while (g_go.load(std::memory_order_acquire) != want) {
      if constexpr (MODE==0) asm volatile("isb" ::: "memory");
      else if constexpr (MODE==1) asm volatile("yield" ::: "memory");
      else asm volatile("wfe" ::: "memory");
    }
    timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    g_t_wake = (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
    g_stop.store(want);
  }
  return nullptr;
}

static void lat(const char* name){
  static std::vector<double>* dummy = nullptr; (void)dummy;
  g_go.store(0); g_stop.store(0);
  pthread_t c;
  if (name[0]=='i') pthread_create(&c,nullptr,lat_waiter<0>,nullptr);
  else if (name[0]=='y') pthread_create(&c,nullptr,lat_waiter<1>,nullptr);
  else pthread_create(&c,nullptr,lat_waiter<2>,nullptr);
  usleep(50000);
  std::vector<double> v;
  for (uint32_t i=1;i<=500;i++){
    usleep(300);
    timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    uint64_t t0 = (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
    g_go.store(i,std::memory_order_release);
    while (g_stop.load(std::memory_order_acquire) < i) asm volatile("" ::: "memory");
    v.push_back((double)(g_t_wake - t0));
  }
  g_go.store(0xffffffffu); pthread_join(c,nullptr);
  std::sort(v.begin(),v.end());
  printf("%-6s wake ns: p50=%6.0f p90=%6.0f p99=%7.0f max=%7.0f\n", name,
    v[v.size()/2], v[v.size()*90/100], v[v.size()*99/100], v.back());
}

int main(){
  lat("isb"); lat("yield"); lat("wfe");
  burn<0>("isb"); burn<1>("yield"); burn<2>("wfe");
  return 0;
}
