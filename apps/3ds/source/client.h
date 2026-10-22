#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "model.h"
#include "models.h"
#include "diff.h"
#include "launch.h"
#include "net.h"
#include "store.h"
#include "ws.h"
#include "tasks.h"
#include "prdetail.h"

typedef enum {
  CONN_IDLE,
  CONN_CONNECTING,
  CONN_SYNCING,  // socket open, waiting for the first shell snapshot
  CONN_READY,
  CONN_ERROR,
} ConnState;

typedef enum { LOAD_NONE, LOAD_BUSY, LOAD_DONE, LOAD_FAILED } LoadState;

#define MAX_PENDING 32
#define MAX_PROVIDERS 12

typedef struct {
  int id;
  int kind;
  char label[48];
} PendingRpc;

// Live state for the one environment the 3DS is connected to.
typedef struct Client {
  EnvConfig env;
  NetAddr addr;
  ConnState state;
  char error[160];
  char server_version[32];
  bool auth_expired;
  int generation;  // bumps on every connect so late job results are dropped

  Ws ws;
  int next_id;
  PendingRpc pending[MAX_PENDING];
  uint64_t last_ping_ms;
  uint64_t last_rx_ms;
  int64_t clock_offset;  // server epoch - local epoch

  T3Thread *threads;
  int thread_count, thread_cap;
  bool threads_dirty;
  LoadState archives_state;
  Project *projects;
  int project_count, project_cap;

  ProviderUsage providers[MAX_PROVIDERS];
  int provider_count;
  LoadState providers_state;
  UsageSummary usage;
  LoadState usage_state;
  char usage_error[120];
  uint64_t usage_loaded_ms;

  PullRequest *prs;
  int pr_count;
  LoadState prs_state;
  char prs_error[160];
  uint64_t prs_loaded_ms;

  Conversation conv;
  LoadState conv_state;
  char conv_error[120];
  int64_t conv_thread_updated_at;  // updatedAt the conversation was fetched for
  bool conv_stale;
  uint64_t conv_fetched_ms;
  int conv_version;  // bumps whenever conv changes, for render caches
  char launched_thread_id[ID_CAP];  // set when a thread this client started is ready to open

  char toast[160];
  uint64_t toast_until_ms;
  ModelCatalog catalog;

  ScheduledTask *tasks;
  int task_count;
  LoadState tasks_state;
  char tasks_error[120];
  bool task_action_busy;
  PrDetail pr_detail;
  LoadState pr_detail_state;
  char pr_detail_error[120];
  LoadState history_state;
  int history_request;
  int history_prepended;
  bool history_loaded;
  int tasks_request_id;

  DiffPreview diff;
  LoadState diff_state;
  char diff_error[160];
  int diff_request_id;
  LaunchRefs launch_refs;
  LoadState launch_refs_state;
  char launch_refs_error[160];
  int launch_refs_request_id;
} Client;

// Platform hook: monotonic milliseconds.
uint64_t client_ticks_ms(void);

void client_init(Client *c);
void client_connect(Client *c, const EnvConfig *env);
void client_disconnect(Client *c);
// Pumps the socket and timers. Call once per frame.
void client_update(Client *c);
int64_t client_now(const Client *c);

T3Thread *client_find_thread(Client *c, const char *id);
Project *client_find_project(Client *c, const char *id);

void client_refresh_usage(Client *c);
void client_refresh_prs(Client *c);
void client_load_archived(Client *c);
void client_open_conversation(Client *c, const char *thread_id);
void client_close_conversation(Client *c);

void client_send_message(Client *c, const T3Thread *t, const char *text);
void client_interrupt(Client *c, const T3Thread *t);
void client_respond(Client *c, const T3Thread *t, const char *decision);
unsigned client_approval_options(const Client *c, const T3Thread *t);
void client_set_settled(Client *c, const T3Thread *t, bool settled);
void client_set_pinned(Client *c, const T3Thread *t, bool pinned);
void client_set_archived(Client *c, const T3Thread *t, bool archived);
void client_mark_unread(Client *c, const T3Thread *t);
void client_mark_read(Client *c, const T3Thread *t);
void client_rename(Client *c, const T3Thread *t, const char *title);
bool client_can_dismiss_question(const Client *c, const T3Thread *t);
void client_dismiss_question(Client *c, const T3Thread *t);
void client_set_interaction_mode(Client *c, const T3Thread *t, const char *mode);
void client_set_runtime_mode(Client *c, const T3Thread *t, const char *mode);
// Starts a thread using the newest thread's model and modes, falling back to the catalog.
void client_new_thread(Client *c, const Project *p, const char *text);

void client_snooze(Client *c, const T3Thread *t, int64_t until);
void client_set_model(Client *c, const T3Thread *t, const char *selection_json);
// NULL selection reuses the newest thread's model; NULL workspace runs in the project folder.
void client_launch_thread(Client *c, const Project *p, const char *text, const char *selection_json,
                          const LaunchWorkspace *workspace);

void client_toast(Client *c, const char *msg);

typedef struct {
  bool ok;
  char error[160];
  EnvConfig env;
} PairResult;

typedef void (*PairDone)(void *ud, const PairResult *result);
// Exchanges a one-time pairing code for a bearer session in the background.
void client_pair(const char *address, const char *code, PairDone done, void *ud);

void client_answer_questions(Client *c, const T3Thread *t, const char *request_id,
                             const QuestionSet *set, const QuestionDraft *drafts);
void client_queue_action(Client *c, const T3Thread *t, int index, QueueAction action, const char *text);
int client_rpc_send(Client *c, const char *tag, const char *payload, int kind, const char *done_label);
