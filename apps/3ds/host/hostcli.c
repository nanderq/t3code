// Desktop harness for the 3DS client's protocol core. It links the same
// source/ files the console build uses, so pairing, the shell stream, usage,
// PRs, and commands can be checked against a real server without hardware.
//
//   hostcli pair <host:port> <code>          prints a bearer token
//   hostcli watch <host:port> <token> [secs] prints threads, usage, PRs
//   hostcli send <host:port> <token> <threadId> <text>
//   hostcli convo <host:port> <token> <threadId>
//   hostcli settle|unsettle <host:port> <token> <threadId>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../source/bg.h"
#include "../source/client.h"
#include "../source/util.h"

typedef struct Job {
  BgFn done;
  void *arg;
  struct Job *next;
} Job;
static Job *queue;

void bg_init(void) {}
void bg_shutdown(void) {}
bool bg_busy(void) { return false; }
void bg_submit(BgFn work, BgFn done, void *arg) {
  work(arg);
  Job *j = calloc(1, sizeof *j);
  j->done = done;
  j->arg = arg;
  Job **tail = &queue;
  while (*tail) tail = &(*tail)->next;
  *tail = j;
}
void bg_poll(void) {
  while (queue) {
    Job *j = queue;
    queue = j->next;
    j->done(j->arg);
    free(j);
  }
}

uint64_t client_ticks_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool paired;
static void on_paired(void *ud, const PairResult *r) {
  (void)ud;
  paired = true;
  if (!r->ok) {
    fprintf(stderr, "pair failed: %s\n", r->error);
    exit(1);
  }
  printf("label=%s\nenvironmentId=%s\naddress=%s\ntoken=%s\n", r->env.label, r->env.environment_id, r->env.addr,
         r->env.token);
}

static void pump(Client *c, int ms, bool (*until)(Client *)) {
  uint64_t end = client_ticks_ms() + (uint64_t)ms;
  while (client_ticks_ms() < end) {
    bg_poll();
    client_update(c);
    if (until && until(c)) return;
    usleep(10000);
  }
}

static bool ready(Client *c) { return c->state == CONN_READY || c->state == CONN_ERROR; }
static bool loaded(Client *c) {
  return c->usage_state >= LOAD_DONE && c->prs_state >= LOAD_DONE && c->providers_state >= LOAD_DONE;
}
static bool conv_loaded(Client *c) { return c->conv_state >= LOAD_DONE; }

static void connect_or_die(Client *c, const char *addr, const char *token) {
  EnvConfig env = {0};
  util_strlcpy(env.addr, addr, sizeof env.addr);
  util_strlcpy(env.token, token, sizeof env.token);
  client_connect(c, &env);
  pump(c, 20000, ready);
  if (c->state != CONN_READY) {
    fprintf(stderr, "connect failed: state=%d %s\n", c->state, c->error);
    exit(1);
  }
}

static void print_threads(Client *c) {
  char ago[16];
  printf("projects=%d threads=%d clock_offset=%llds\n", c->project_count, c->thread_count,
         (long long)c->clock_offset);
  for (int i = 0; i < c->thread_count && i < 25; i++) {
    T3Thread *t = &c->threads[i];
    Project *p = client_find_project(c, t->project_id);
    util_format_ago(t->updated_at, client_now(c), ago, sizeof ago);
    printf("  [r%d] %-10s %-5s %-20.20s %-44.44s %s %s%s\n", thread_rank(t), t->status, ago, p ? p->title : "?",
           t->title, t->model, t->pending_id[0] ? "PENDING:" : "", t->pending_kind);
  }
}

int main(int argc, char **argv) {
  util_seed_random((uint64_t)time(NULL) ^ (uint64_t)getpid());
  if (argc >= 4 && !strcmp(argv[1], "pair")) {
    client_pair(argv[2], argv[3], on_paired, NULL);
    bg_poll();
    return paired ? 0 : 1;
  }
  if (argc < 4) {
    fprintf(stderr, "usage: hostcli pair|watch|send|convo ...\n");
    return 2;
  }
  static Client c;
  client_init(&c);
  connect_or_die(&c, argv[2], argv[3]);

  if (!strcmp(argv[1], "watch")) {
    // Simulates a console clock set to local time, as on a real 3DS.
    if (getenv("FAKE_UTC_OFFSET_HOURS")) c.clock_offset -= atoi(getenv("FAKE_UTC_OFFSET_HOURS")) * 3600;
    print_threads(&c);
    client_refresh_usage(&c);
    client_refresh_prs(&c);
    pump(&c, 60000, loaded);
    printf("after load: state=%d generation=%d error=%s\n", c.state, c.generation, c.error);
    printf("server=%s providers=%d (state %d)\n", c.server_version, c.provider_count, c.providers_state);
    for (int i = 0; i < c.provider_count; i++) {
      ProviderUsage *u = &c.providers[i];
      printf("  %-16s %-8s windows=%d %s\n", u->name, u->status, u->window_count, u->note);
      for (int w = 0; w < u->window_count; w++) {
        char until[32];
        util_format_until(u->windows[w].resets_at, client_now(&c), until, sizeof until);
        printf("      %-20s %5.1f%% %s\n", u->windows[w].label, u->windows[w].used_pct, until);
      }
    }
    printf("usage state=%d err=%s sessions=%d pricing=%s top=%s $%.2f\n", c.usage_state, c.usage_error,
           c.usage.sessions, c.usage.pricing, c.usage.top_model, c.usage.top_model_cost);
    for (int i = 0; i < c.usage.provider_count; i++)
      printf("  %-10s $%.2f %.0f tokens\n", c.usage.providers[i].provider, c.usage.providers[i].cost,
             c.usage.providers[i].tokens);
    printf("  today $%.2f\n", c.usage.day_cost[USAGE_DAYS - 1]);
    printf("prs state=%d count=%d err=%s\n", c.prs_state, c.pr_count, c.prs_error);
    for (int i = 0; i < c.pr_count && i < 10; i++) {
      PullRequest *pr = &c.prs[i];
      printf("  %s#%d %-50.50s %s %s %s @%s\n", pr->repo, pr->number, pr->title, pr->state, pr->checks, pr->review,
             pr->author);
    }
    int secs = argc >= 5 ? atoi(argv[4]) : 0;
    if (secs > 0) {
      printf("watching %ds for live updates...\n", secs);
      pump(&c, secs * 1000, NULL);
      print_threads(&c);
      printf("state=%d (3=ready) error=%s\n", c.state, c.error);
    }
  } else if (!strcmp(argv[1], "convo") && argc >= 5) {
    client_open_conversation(&c, argv[4]);
    pump(&c, 20000, conv_loaded);
    printf("conv state=%d items=%d more=%d err=%s\n", c.conv_state, c.conv.count, c.conv.has_more, c.conv_error);
    static const char *kinds[] = {"user", "assistant", "plan", "todo", "work", "notice", "error", "approval", "question"};
    static const char *works[] = {"command", "edit", "search", "web", "tool", "thought", "agent"};
    for (int i = 0; i < c.conv.count; i++) {
      const ConvItem *it = &c.conv.items[i];
      printf("--- %s%s%s run=%.12s %s%s exit=%d +%d -%d %llds\n%.200s\n", kinds[it->kind],
             it->kind == ITEM_WORK ? "/" : "", it->kind == ITEM_WORK ? works[it->work] : "", it->run_id,
             it->streaming ? "streaming " : "", it->failed ? "failed " : "", it->exit_code, it->additions,
             it->deletions, (long long)(it->ended_at - it->started_at), it->text);
      if (it->detail) printf("  detail: %.160s\n", it->detail);
    }
  } else if ((!strcmp(argv[1], "settle") || !strcmp(argv[1], "unsettle")) && argc >= 5) {
    T3Thread *t = client_find_thread(&c, argv[4]);
    if (!t) {
      fprintf(stderr, "no such thread\n");
      return 1;
    }
    client_set_settled(&c, t, !strcmp(argv[1], "settle"));
    pump(&c, 3000, NULL);
    t = client_find_thread(&c, argv[4]);
    printf("toast: %s | settled now: %d\n", c.toast, t ? t->settled : -1);
  } else if (!strcmp(argv[1], "send") && argc >= 6) {
    T3Thread *t = client_find_thread(&c, argv[4]);
    if (!t) {
      fprintf(stderr, "no such thread\n");
      return 1;
    }
    client_send_message(&c, t, argv[5]);
    pump(&c, 10000, NULL);
    printf("toast: %s\n", c.toast);
  }
  client_disconnect(&c);
  return 0;
}
