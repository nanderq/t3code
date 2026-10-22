#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "json.h"
#include "questions.h"
#include "queue.h"

#define ID_CAP 128
// Keeps a long thread with loaded history within Old 3DS memory.
#define MAX_CONV_ITEMS 320
#define MAX_CONV_TEXT (1024 * 1024)

typedef struct {
  char id[ID_CAP];
  char title[96];
  char root[160];
  char *default_model_json;  // raw ModelSelection object or NULL
} Project;

typedef struct {
  char id[ID_CAP];
  char project_id[ID_CAP];
  char title[160];
  char status[16];       // idle | preparing | queued | running | waiting | completed | failed ...
  char active_run_id[ID_CAP];
  char provider[48];     // providerInstanceId
  char model[64];
  char runtime_mode[24];
  char interaction_mode[12];
  char *model_selection_json;  // raw object, reused for new threads in the project
  char branch[96];
  char pending_id[ID_CAP];
  char pending_kind[32];
  bool pending_blocking;
  char last_role[12];
  char last_text[240];
  char last_error[200];
  char pr[64];           // "repo#123"
  int64_t updated_at;
  int64_t pinned_at;
  int64_t latest_completed_ms;
  bool archived;
  bool unread;
  bool settled;
  bool subagent;
  int item_count;
  int64_t snoozed_until;
  char worktree_path[512];
  char parent_id[ID_CAP];  // lineage.parentThreadId, set for forks and subagents
} T3Thread;

typedef struct {
  char label[40];
  float used_pct;
  int64_t resets_at;
} UsageWindow;

typedef struct {
  char instance_id[48];
  char name[48];
  char status[16];
  char note[96];
  UsageWindow windows[6];
  int window_count;
  bool enabled;
} ProviderUsage;

#define USAGE_DAYS 30

typedef struct {
  char provider[16];
  double cost;
  double tokens;
} UsageProviderTotal;

typedef struct {
  bool loaded;
  double day_cost[USAGE_DAYS];  // index 0 = oldest, USAGE_DAYS-1 = today
  double day_tokens[USAGE_DAYS];
  UsageProviderTotal providers[8];
  int provider_count;
  char top_model[48];
  double top_model_cost;
  int sessions;
  char pricing[16];
} UsageSummary;

typedef struct {
  char project_id[ID_CAP];
  char project_title[64];
  char host[48];
  char repo[80];
  int number;
  char title[160];
  char author[40];
  char head[80];
  char base[48];
  char state[10];
  bool draft;
  char checks[10];
  char review[20];
  char mergeability[12];
  char labels[96];
  int additions, deletions;
  int64_t updated_at;
  bool review_requested;
} PullRequest;

typedef enum {
  ITEM_USER,
  ITEM_ASSISTANT,
  ITEM_PLAN,      // proposed plan, markdown
  ITEM_TODO,      // task list; detail holds one "<status char><text>" per line
  ITEM_WORK,      // tool call, reasoning, subagent; see WorkKind
  ITEM_NOTICE,    // system notices, interrupts, compaction, handoffs
  ITEM_ERROR,
  ITEM_APPROVAL,  // text is a one-line summary, detail the prompt
  ITEM_QUESTION,  // text is the question summary, detail the full questions
} ItemKind;

typedef enum { WORK_COMMAND, WORK_EDIT, WORK_SEARCH, WORK_WEB, WORK_TOOL, WORK_THOUGHT, WORK_AGENT } WorkKind;

enum {
  APPROVAL_ACCEPT = 1,
  APPROVAL_SESSION = 2,
  APPROVAL_ALWAYS = 4,
  APPROVAL_DECLINE = 8,
  APPROVAL_CANCEL = 16,
  APPROVAL_DEFAULT = APPROVAL_ACCEPT | APPROVAL_SESSION | APPROVAL_DECLINE,
};

typedef struct {
  ItemKind kind;
  WorkKind work;
  char *text;    // heap: message markdown, command, or row label
  char *detail;  // heap or NULL: output tail, reasoning, steps, or prompt
  char run_id[ID_CAP];
  char request_id[ID_CAP];  // approvals and questions
  unsigned approval_options;
  bool message_response;
  bool streaming;
  bool running;  // the item's own status is still in progress
  bool failed;
  int exit_code;  // -1 when unknown
  int additions, deletions;  // -1 when unknown
  int64_t started_at, ended_at;  // epoch seconds, 0 when unknown
  QuestionSet *questions;
} ConvItem;

typedef struct {
  char thread_id[ID_CAP];
  ConvItem *items;
  int count;
  bool has_more;
  bool truncated;  // the server dropped turn items to fit its payload budget
  RunQueue *queue;
  char history_cursor[4097];
  bool limited;
  size_t text_bytes;  // running total of item text, checked against MAX_CONV_TEXT
} Conversation;

void thread_free(T3Thread *t);
void project_free(Project *p);
void conversation_clear(Conversation *c);
size_t conversation_text_bytes(const Conversation *conv);

// Parsers. Each consumes exactly one JSON value at the cursor.
void parse_thread_shell(JsonCur *c, T3Thread *t);
void parse_project_shell(JsonCur *c, Project *p);
void parse_provider_usage(JsonCur *c, ProviderUsage *out, bool *ok);
void parse_pull_request(JsonCur *c, PullRequest *pr);
// `today_days` is days since the Unix epoch for the newest bucket.
void parse_usage_summary(JsonCur *c, UsageSummary *u, int64_t today_days);
// Reads a bounded thread snapshot (`projection.visibleTurnItems`).
void parse_bounded_snapshot(JsonCur *c, Conversation *conv);
void parse_projected_items(JsonCur *c, Conversation *conv);

// Lower is more urgent: waiting on the user, running, then the rest.
int thread_rank(const T3Thread *t);
bool thread_is_active(const T3Thread *t);
unsigned approval_decision_bit(const char *decision);

bool thread_needs_user(const T3Thread *t);
