#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void report(const char* tag) {
  qos_class_t qc{}; int rel{};
  int r = pthread_get_qos_class_np(pthread_self(), &qc, &rel);
  int pol{}; sched_param sp{};
  int r2 = pthread_getschedparam(pthread_self(), &pol, &sp);
  const char* qn = qc==QOS_CLASS_USER_INTERACTIVE?"USER_INTERACTIVE":qc==QOS_CLASS_USER_INITIATED?"USER_INITIATED":qc==QOS_CLASS_DEFAULT?"DEFAULT":qc==QOS_CLASS_UTILITY?"UTILITY":qc==QOS_CLASS_BACKGROUND?"BACKGROUND":"UNSPECIFIED/OTHER";
  printf("%-28s qos_ret=%d qos=%s(%d) rel=%d | sched_ret=%d policy=%s prio=%d\n",
         tag, r, qn, (int)qc, rel, r2, pol==SCHED_RR?"SCHED_RR":pol==SCHED_OTHER?"SCHED_OTHER":pol==SCHED_FIFO?"SCHED_FIFO":"?", sp.sched_priority);
}

static void* runner(void* arg) {
  report((const char*)arg);
  return nullptr;
}

int main() {
  report("main default");
  pthread_attr_t a;
  // Case 1: exactly what RPCS3 does
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 0x800000);
  sched_param sp{}; sp.sched_priority = 99;
  int q = pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
  int p = pthread_attr_setschedpolicy(&a, SCHED_RR);
  int s = pthread_attr_setschedparam(&a, &sp);
  printf("attr sets: qos=%d policy=%d param=%d\n", q, p, s);
  pthread_t t; int c = pthread_create(&t, &a, runner, (void*)"rpcs3-style thread");
  printf("create=%d\n", c); if (!c) pthread_join(t,nullptr);

  // Case 2: QoS only
  pthread_attr_init(&a);
  q = pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
  c = pthread_create(&t, &a, runner, (void*)"qos-only thread");
  printf("qos-only create=%d (attr qos=%d)\n", c, q); if (!c) pthread_join(t,nullptr);

  // Case 3: QoS then self set_schedparam
  pthread_attr_init(&a);
  q = pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INITIATED, 0);
  c = pthread_create(&t, &a, [](void*)->void*{
      report("qos-initiated before");
      sched_param s2{}; s2.sched_priority = 47;
      int e = pthread_setschedparam(pthread_self(), SCHED_OTHER, &s2);
      printf("pthread_setschedparam ret=%d\n", e);
      report("qos-initiated after schedparam");
      return nullptr; }, nullptr);
  if (!c) pthread_join(t,nullptr);
  return 0;
}
