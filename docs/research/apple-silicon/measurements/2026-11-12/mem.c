#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec; }

static void footprint(const char* tag){
  task_vm_info_data_t vi; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
  kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n);
  if (kr) { printf("%s: task_info failed %d\n", tag, kr); return; }
  printf("%-28s phys_footprint=%8.1f MB  resident=%8.1f MB  internal=%8.1f MB  compressed=%8.1f MB\n",
    tag, vi.phys_footprint/1048576.0, vi.resident_size/1048576.0,
    vi.internal/1048576.0, vi.compressed/1048576.0);
}

#define NBLK 64
#define BLKSZ (1024*1024)   /* 1 MB per block -> 64 MB total */
static void* blk[NBLK];

int main(void){
  footprint("start");
  /* reserve 64 x 1MB, touch every page */
  for (int i=0;i<NBLK;i++){
    blk[i] = mmap(NULL, BLKSZ, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    if (blk[i]==MAP_FAILED){ perror("mmap"); return 1; }
    memset(blk[i], 0x5a, BLKSZ);
  }
  footprint("after touch 64MB");

  /* A: munmap + mmap (current RPCS3 decommit) */
  uint64_t t0 = now_ns();
  for (int i=0;i<NBLK;i++){
    munmap(blk[i], BLKSZ);
    blk[i] = mmap(blk[i], BLKSZ, PROT_NONE, MAP_ANON|MAP_PRIVATE, -1, 0);
    if (blk[i]==MAP_FAILED){ perror("remap"); return 1; }
  }
  uint64_t t1 = now_ns();
  uint64_t dc_a = t1 - t0;
  footprint("after munmap+mmap PROT_NONE");

  /* recommit and touch */
  t0 = now_ns();
  for (int i=0;i<NBLK;i++){
    mprotect(blk[i], BLKSZ, PROT_READ|PROT_WRITE);
    memset(blk[i], 0x33, BLKSZ);
  }
  t1 = now_ns();
  printf("munmap+mmap:  decommit=%llu us  recommit+touch=%llu us\n",
    (unsigned long long)(dc_a/1000), (unsigned long long)((t1-t0)/1000));
  footprint("after recommit+touch");

  /* B: mprotect(PROT_NONE) decommit (keeps mapping) */
  t0 = now_ns();
  for (int i=0;i<NBLK;i++) mprotect(blk[i], BLKSZ, PROT_NONE);
  t1 = now_ns();
  uint64_t a = (t1-t0)/1000;
  footprint("after mprotect PROT_NONE");

  t0 = now_ns();
  for (int i=0;i<NBLK;i++){ mprotect(blk[i], BLKSZ, PROT_READ|PROT_WRITE); memset(blk[i], 0x11, BLKSZ); }
  t1 = now_ns();
  printf("mprotect:     decommit=%llu us  recommit+touch=%llu us\n", (unsigned long long)a, (unsigned long long)((t1-t0)/1000));
  footprint("after mprotect recommit");

  /* C: MADV_FREE_REUSABLE decommit (keeps VA, OS can reclaim) */
  t0 = now_ns();
  for (int i=0;i<NBLK;i++) madvise(blk[i], BLKSZ, MADV_FREE_REUSABLE);
  t1 = now_ns();
  a = (t1-t0)/1000;
  footprint("after MADV_FREE_REUSABLE");

  t0 = now_ns();
  for (int i=0;i<NBLK;i++){ madvise(blk[i], BLKSZ, MADV_FREE_REUSE); memset(blk[i], 0x22, BLKSZ); }
  t1 = now_ns();
  printf("madv_free:    decommit=%llu us  recommit+touch=%llu us\n", (unsigned long long)a, (unsigned long long)((t1-t0)/1000));
  footprint("after MADV_FREE_REUSE");

  for (int i=0;i<NBLK;i++) munmap(blk[i], BLKSZ);
  footprint("after final munmap");
  return 0;
}
