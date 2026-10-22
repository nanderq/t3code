#include <3ds.h>
#include <stdlib.h>

#include "bg.h"

typedef struct Job {
  BgFn work, done;
  void *arg;
  struct Job *next;
} Job;

static Thread worker;
static LightLock lock;
static LightSemaphore jobs_ready;
static Job *todo, *finished;
static volatile bool running;
static volatile int in_flight;

static void push(Job **list, Job *j) {
  j->next = NULL;
  while (*list) list = &(*list)->next;
  *list = j;
}

static void worker_main(void *unused) {
  (void)unused;
  while (running) {
    LightSemaphore_Acquire(&jobs_ready, 1);
    LightLock_Lock(&lock);
    Job *j = todo;
    if (j) todo = j->next;
    LightLock_Unlock(&lock);
    if (!j) continue;
    j->work(j->arg);
    LightLock_Lock(&lock);
    push(&finished, j);
    LightLock_Unlock(&lock);
  }
}

void bg_init(void) {
  LightLock_Init(&lock);
  LightSemaphore_Init(&jobs_ready, 0, 64);
  running = true;
  s32 prio = 0x30;
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  // Lower priority than the UI thread, so rendering preempts parsing. The
  // UI blocks on vsync every frame, which gives the worker its time.
  worker = threadCreate(worker_main, NULL, 128 * 1024, prio + 1, -1, false);
}

void bg_shutdown(void) {
  if (!worker) return;
  running = false;
  LightSemaphore_Release(&jobs_ready, 1);
  // A job blocked on the network can take a while; do not hang app exit on it.
  threadJoin(worker, 2000000000ULL);
  threadFree(worker);
  worker = NULL;
}

void bg_submit(BgFn work, BgFn done, void *arg) {
  Job *j = malloc(sizeof *j);
  j->work = work;
  j->done = done;
  j->arg = arg;
  in_flight++;
  LightLock_Lock(&lock);
  push(&todo, j);
  LightLock_Unlock(&lock);
  LightSemaphore_Release(&jobs_ready, 1);
}

void bg_poll(void) {
  LightLock_Lock(&lock);
  Job *list = finished;
  finished = NULL;
  LightLock_Unlock(&lock);
  while (list) {
    Job *j = list;
    list = j->next;
    in_flight--;
    j->done(j->arg);
    free(j);
  }
}

bool bg_busy(void) { return in_flight > 0; }
