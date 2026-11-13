#include <stdio.h>
#include <stdint.h>
#include <time.h>
static uint64_t ns(){ timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
int main(){
  /* count how many WFE retire per second with SEVL once, and with SEVL every iteration */
  for (int mode=0; mode<2; mode++){
    uint64_t n=0, end=ns()+200000000ull;
    if (mode==0) asm volatile("sevl" ::: "memory");
    while (ns()<end) {
      if (mode==1) asm volatile("sevl" ::: "memory");
      asm volatile("wfe" ::: "memory");
      n++;
    }
    printf("wfe mode %s: %8.2f M/s -> %6.1f ns/op\n", mode? "sevl-each":"sevl-once", n/1e6/0.2, 200000000.0/n);
  }
  return 0;
}
