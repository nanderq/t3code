#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alerts.h"
#include "bg.h"
#include "http.h"
#include "history.h"
#include "json.h"
#include "util.h"
#include "threadops.h"

#define KEY(k) jstr_eq(key, k)
#define PING_INTERVAL_MS 15000
#define RETRY_INTERVAL_MS 5000
// Pings every 15s get a Pong, so this much silence means the link is dead
// (Wi-Fi gone without a TCP reset, as after closing the lid).
#define RX_TIMEOUT_MS 45000
#define CONV_REFRESH_MIN_MS 1500

enum { RPC_SHELL = 1, RPC_CONFIG, RPC_USAGE, RPC_PRS, RPC_COMMAND, RPC_LAUNCH, RPC_ARCHIVE };

static const char *PROTOCOL_HEADER = "x-t3-orchestration-protocol: 2\r\n";

int64_t client_now(const Client *c) { return (int64_t)time(NULL) + c->clock_offset; }

void client_toast(Client *c, const char *msg) {
  util_strlcpy(c->toast, msg, sizeof c->toast);
  c->toast_until_ms = client_ticks_ms() + 3500;
}

void client_init(Client *c) {
  memset(c, 0, sizeof *c);
  c->ws.fd = -1;
  c->ws.closed = true;
}

static void clear_threads(Client *c) {
  for (int i = 0; i < c->thread_count; i++) thread_free(&c->threads[i]);
  c->thread_count = 0;
  for (int i = 0; i < c->project_count; i++) project_free(&c->projects[i]);
  c->project_count = 0;
}

static void reset_session(Client *c) {
  threadops_reset(c);
  ws_close(&c->ws);
  memset(c->pending, 0, sizeof c->pending);
  clear_threads(c);
  c->archives_state = LOAD_NONE;
  free(c->prs);
  c->prs = NULL;
  c->pr_count = 0;
  c->prs_state = LOAD_NONE;
  free(c->tasks);
  c->tasks = NULL;
  c->task_count = 0;
  c->tasks_state = LOAD_NONE;
  c->task_action_busy = false;
  memset(&c->pr_detail, 0, sizeof c->pr_detail);
  c->pr_detail_state = LOAD_NONE;
  c->history_state = LOAD_NONE;
  c->history_loaded = false;
  c->history_request++;
  c->history_prepended = 0;
  c->provider_count = 0;
  memset(&c->catalog, 0, sizeof c->catalog);
  c->providers_state = LOAD_NONE;
  memset(&c->usage, 0, sizeof c->usage);
  c->usage_state = LOAD_NONE;
  conversation_clear(&c->conv);
  c->conv_state = LOAD_NONE;
  c->conv_version++;
}

void client_disconnect(Client *c) {
  c->generation++;
  reset_session(c);
  c->state = CONN_IDLE;
  c->error[0] = '\0';
}

// ---------------------------------------------------------------- RPC plumbing

static PendingRpc *pending_find(Client *c, int id) {
  for (int i = 0; i < MAX_PENDING; i++)
    if (c->pending[i].id == id) return &c->pending[i];
  return NULL;
}

static int rpc_send(Client *c, const char *tag, const char *payload, int kind, const char *label) {
  if (c->ws.closed) {
    client_toast(c, "Not connected");
    return -1;
  }
  PendingRpc *slot = pending_find(c, 0);
  if (!slot) {
    client_toast(c, "Too many requests in flight");
    return -1;
  }
  int id = ++c->next_id;
  JBuf b;
  jb_init(&b);
  char ids[16];
  snprintf(ids, sizeof ids, "%d", id);
  jb_raw(&b, "{\"_tag\":\"Request\",\"id\":");
  jb_str(&b, ids);
  jb_raw(&b, ",\"tag\":");
  jb_str(&b, tag);
  jb_raw(&b, ",\"payload\":");
  jb_raw(&b, payload);
  jb_raw(&b, ",\"headers\":[]}");
  bool ok = ws_send_text(&c->ws, b.p, b.n);
  jb_free(&b);
  if (!ok) return -1;
  slot->id = id;
  slot->kind = kind;
  util_strlcpy(slot->label, label ? label : "", sizeof slot->label);
  return id;
}

int client_rpc_send(Client *c, const char *tag, const char *payload, int kind, const char *done_label) {
  return rpc_send(c, tag, payload, kind, done_label);
}

static void send_ack(Client *c, int id) {
  char msg[64];
  int n = snprintf(msg, sizeof msg, "{\"_tag\":\"Ack\",\"requestId\":\"%d\"}", id);
  ws_send_text(&c->ws, msg, (size_t)n);
}

// Effect encodes failures as a Cause tree; the first `message` inside it is
// the most useful thing to show.
static void extract_error(JStr raw, char *out, size_t cap) {
  const char *m = util_memmem(raw.p, raw.n, "\"message\":", 10);
  if (m) {
    JsonCur cur;
    json_init(&cur, m + 10, (size_t)(raw.p + raw.n - (m + 10)));
    if (json_str(&cur, out, cap) && out[0]) return;
  }
  const char *t = util_memmem(raw.p, raw.n, "Error\"", 6);
  if (t) {
    const char *q = t;
    while (q > raw.p && q[-1] != '"') q--;
    size_t n = (size_t)(t + 5 - q);
    if (n < cap) {
      memcpy(out, q, n);
      out[n] = '\0';
      return;
    }
  }
  util_strlcpy(out, "Request failed", cap);
}

// ---------------------------------------------------------------- shell state

static int thread_cmp(const void *a, const void *b) {
  const T3Thread *x = a, *y = b;
  if (!!x->pinned_at != !!y->pinned_at) return x->pinned_at ? -1 : 1;
  int rx = thread_rank(x), ry = thread_rank(y);
  if (rx != ry) return rx - ry;
  if (x->updated_at != y->updated_at) return x->updated_at > y->updated_at ? -1 : 1;
  return strcmp(x->id, y->id);
}

T3Thread *client_find_thread(Client *c, const char *id) {
  for (int i = 0; i < c->thread_count; i++)
    if (strcmp(c->threads[i].id, id) == 0) return &c->threads[i];
  return NULL;
}

Project *client_find_project(Client *c, const char *id) {
  for (int i = 0; i < c->project_count; i++)
    if (strcmp(c->projects[i].id, id) == 0) return &c->projects[i];
  return NULL;
}

static void remove_thread(Client *c, const char *id) {
  T3Thread *t = client_find_thread(c, id);
  if (!t) return;
  thread_free(t);
  int idx = (int)(t - c->threads);
  memmove(t, t + 1, sizeof(T3Thread) * (size_t)(c->thread_count - idx - 1));
  c->thread_count--;
  c->threads_dirty = true;
}

// Takes ownership of t's heap fields.
static void upsert_thread(Client *c, T3Thread *t) {
  T3Thread *existing = client_find_thread(c, t->id);
  if (existing) {
    thread_free(existing);
    *existing = *t;
  } else {
    if (c->thread_count == c->thread_cap) {
      c->thread_cap = c->thread_cap ? c->thread_cap * 2 : 64;
      c->threads = realloc(c->threads, sizeof(T3Thread) * (size_t)c->thread_cap);
    }
    c->threads[c->thread_count++] = *t;
  }
  c->threads_dirty = true;
  if (c->conv.thread_id[0] && strcmp(c->conv.thread_id, t->id) == 0 && t->updated_at != c->conv_thread_updated_at)
    c->conv_stale = true;
}

static void upsert_project(Client *c, Project *p) {
  Project *existing = client_find_project(c, p->id);
  if (existing) {
    project_free(existing);
    *existing = *p;
    return;
  }
  if (c->project_count == c->project_cap) {
    c->project_cap = c->project_cap ? c->project_cap * 2 : 16;
    c->projects = realloc(c->projects, sizeof(Project) * (size_t)c->project_cap);
  }
  c->projects[c->project_count++] = *p;
}

static void remove_project(Client *c, const char *id) {
  Project *p = client_find_project(c, id);
  if (!p) return;
  project_free(p);
  int idx = (int)(p - c->projects);
  memmove(p, p + 1, sizeof(Project) * (size_t)(c->project_count - idx - 1));
  c->project_count--;
}

static void read_thread_list(Client *c, JsonCur *cur, bool archived) {
  if (!json_arr_begin(cur)) return;
  while (json_arr_next(cur)) {
    T3Thread t = {0};
    parse_thread_shell(cur, &t);
    t.archived = archived;
    if (t.id[0]) upsert_thread(c, &t);
    else thread_free(&t);
  }
}

static void apply_snapshot(Client *c, JsonCur *cur, bool archived) {
  int kept = 0;
  for (int i = 0; i < c->thread_count; i++) {
    if (c->threads[i].archived == archived) thread_free(&c->threads[i]);
    else c->threads[kept++] = c->threads[i];
  }
  c->thread_count = kept;
  c->threads_dirty = true;
  if (!archived) {
    for (int i = 0; i < c->project_count; i++) project_free(&c->projects[i]);
    c->project_count = 0;
  }
  if (!json_obj_begin(cur)) return;
  JStr key;
  while (json_obj_next(cur, &key)) {
    if (KEY("threads")) read_thread_list(c, cur, archived);
    else if (KEY("archivedThreads")) read_thread_list(c, cur, true);
    else if (KEY("projects") && json_arr_begin(cur)) {
      while (json_arr_next(cur)) {
        Project p = {0};
        parse_project_shell(cur, &p);
        if (p.id[0]) upsert_project(c, &p);
        else project_free(&p);
      }
    } else json_skip(cur);
  }
  if (archived) c->archives_state = LOAD_DONE;
  else {
    c->state = CONN_READY;
    c->error[0] = '\0';
  }
}

static void apply_shell_item(Client *c, JsonCur *cur, bool archived) {
  char kind[24] = "";
  // A snapshot carrying resolvedRepositoryIdentityRoots only refreshes project
  // repository metadata, which this client does not show; it is not a reset.
  bool enrichment = false;
  JsonCur probe = *cur;
  if (json_obj_begin(&probe)) {
    JStr key;
    while (json_obj_next(&probe, &key)) {
      if (KEY("kind")) json_str(&probe, kind, sizeof kind);
      else if (KEY("resolvedRepositoryIdentityRoots")) enrichment = true, json_skip(&probe);
      else json_skip(&probe);
    }
  }
  if (!json_obj_begin(cur)) return;
  char location[12], target_id[ID_CAP] = "";
  util_strlcpy(location, archived ? "archive" : "active", sizeof location);
  T3Thread t = {0};
  Project p = {0};
  bool have_thread = false, have_project = false;
  JStr key;
  while (json_obj_next(cur, &key)) {
    if (KEY("snapshot") && !strcmp(kind, "snapshot") && !enrichment) apply_snapshot(c, cur, archived);
    else if (KEY("location")) json_str(cur, location, sizeof location);
    else if (KEY("thread")) {
      parse_thread_shell(cur, &t);
      have_thread = true;
    } else if (KEY("project")) {
      parse_project_shell(cur, &p);
      have_project = true;
    } else if (KEY("threadId") || KEY("projectId")) json_str(cur, target_id, sizeof target_id);
    else json_skip(cur);
  }
  if (!strcmp(kind, "thread.updated") && have_thread && t.id[0]) {
    if (!strcmp(location, "active") || !strcmp(location, "archive")) {
      t.archived = !strcmp(location, "archive");
      if (!t.archived && !t.subagent && c->state == CONN_READY && thread_needs_user(&t) &&
          !thread_needs_user(client_find_thread(c, t.id))) {
        char title[144], label[160];
        util_strlcpy(title, t.title[0] ? t.title : "Untitled", sizeof title);
        snprintf(label, sizeof label, "%s needs you", title);
        client_toast(c, label);
        alerts_play();
      }
      upsert_thread(c, &t);
      return;
    }
    remove_thread(c, t.id);
  } else if (!strcmp(kind, "thread.removed")) {
    // The two streams can deliver the same move in either order.
    T3Thread *existing = client_find_thread(c, target_id);
    if (existing && existing->archived == !strcmp(location, "archive")) remove_thread(c, target_id);
  } else if (!strcmp(kind, "project.updated") && have_project && p.id[0]) {
    upsert_project(c, &p);
    return;
  } else if (!strcmp(kind, "project.removed")) {
    remove_project(c, target_id);
  }
  thread_free(&t);
  project_free(&p);
}

// ---------------------------------------------------------------- responses

// The 3DS clock is set to local time, so its gap from the server's UTC Date
// header is the user's UTC offset, to the nearest hour.
static int utc_offset_hours(const Client *c) {
  int64_t off = -c->clock_offset;
  int h = (int)((off >= 0 ? off + 1800 : off - 1800) / 3600);
  return h >= -12 && h <= 14 ? h : 0;
}

static int64_t today_days(const Client *c) { return (client_now(c) + utc_offset_hours(c) * 3600) / 86400; }

static void on_success(Client *c, PendingRpc *rpc, JsonCur *value) {
  if (threadops_success(c, rpc, value)) return;
  switch (rpc->kind) {
    case RPC_CONFIG: {
      c->provider_count = 0;
      memset(&c->catalog, 0, sizeof c->catalog);
      if (!json_obj_begin(value)) break;
      JStr key;
      while (json_obj_next(value, &key)) {
        if (KEY("providers") && json_arr_begin(value)) {
          while (json_arr_next(value)) {
            bool ok = false;
            ProviderUsage u;
            JsonCur models = *value;
            models_parse_provider(&models, &c->catalog);
            parse_provider_usage(value, &u, &ok);
            if (ok && u.enabled && c->provider_count < MAX_PROVIDERS) c->providers[c->provider_count++] = u;
          }
        } else if (KEY("environment") && json_obj_begin(value)) {
          JStr k2;
          while (json_obj_next(value, &k2)) {
            if (jstr_eq(k2, "serverVersion")) json_str(value, c->server_version, sizeof c->server_version);
            else json_skip(value);
          }
        } else json_skip(value);
      }
      c->providers_state = LOAD_DONE;
      break;
    }
    case RPC_USAGE:
      parse_usage_summary(value, &c->usage, today_days(c));
      c->usage_state = LOAD_DONE;
      c->usage_loaded_ms = client_ticks_ms();
      break;
    case RPC_PRS: {
      free(c->prs);
      c->prs = NULL;
      c->pr_count = 0;
      c->prs_error[0] = '\0';
      int cap = 0;
      if (!json_obj_begin(value)) break;
      JStr key;
      while (json_obj_next(value, &key)) {
        if (KEY("entries") && json_arr_begin(value)) {
          while (json_arr_next(value)) {
            if (c->pr_count == cap) {
              cap = cap ? cap * 2 : 32;
              c->prs = realloc(c->prs, sizeof(PullRequest) * (size_t)cap);
            }
            parse_pull_request(value, &c->prs[c->pr_count++]);
          }
        } else if (KEY("errors") && json_arr_begin(value)) {
          while (json_arr_next(value)) {
            if (!json_obj_begin(value)) continue;
            JStr k2;
            while (json_obj_next(value, &k2)) {
              if (jstr_eq(k2, "message") && !c->prs_error[0]) json_str(value, c->prs_error, sizeof c->prs_error);
              else json_skip(value);
            }
          }
        } else json_skip(value);
      }
      c->prs_state = LOAD_DONE;
      c->prs_loaded_ms = client_ticks_ms();
      break;
    }
    case RPC_TASKS:
    case RPC_TASK_ACTION:
      tasks_result(c, rpc->kind, rpc->id, value, rpc->label);
      break;
    case RPC_PR_DETAIL:
    case RPC_PR_ACTION:
      prdetail_result(c, rpc->kind, rpc->id, value, rpc->label);
      break;
    case RPC_COMMAND:
      if (c->conv.thread_id[0]) c->conv_stale = true;
      if (rpc->label[0]) client_toast(c, rpc->label);
      break;
    case RPC_LAUNCH:
      client_toast(c, "Thread started");
      if (json_obj_begin(value)) {
        JStr key;
        while (json_obj_next(value, &key)) {
          if (KEY("threadId")) json_str(value, c->launched_thread_id, sizeof c->launched_thread_id);
          else json_skip(value);
        }
      }
      break;
  }
}

static void on_failure(Client *c, PendingRpc *rpc, const char *msg) {
  if (threadops_failure(c, rpc, msg)) return;
  switch (rpc->kind) {
    case RPC_SHELL:
      c->state = CONN_ERROR;
      snprintf(c->error, sizeof c->error, "Thread updates stopped: %s", msg);
      break;
    case RPC_ARCHIVE:
      c->archives_state = LOAD_FAILED;
      client_toast(c, msg);
      break;
    case RPC_TASKS:
    case RPC_TASK_ACTION:
      tasks_failure(c, rpc->kind, rpc->id, msg);
      break;
    case RPC_PR_DETAIL:
    case RPC_PR_ACTION:
      prdetail_failure(c, rpc->kind, rpc->id, msg);
      break;
    case RPC_CONFIG: c->providers_state = LOAD_FAILED; break;
    case RPC_USAGE:
      c->usage_state = LOAD_FAILED;
      util_strlcpy(c->usage_error, msg, sizeof c->usage_error);
      break;
    case RPC_PRS:
      c->prs_state = LOAD_FAILED;
      util_strlcpy(c->prs_error, msg, sizeof c->prs_error);
      break;
    default: {
      char t[160];
      snprintf(t, sizeof t, "Failed: %s", msg);
      client_toast(c, t);
    }
  }
}

static void handle_message(Client *c, JsonCur *cur) {
  char tag[24] = "";
  int req = -1;
  JStr values = {0}, exitv = {0}, defect = {0};
  if (!json_obj_begin(cur)) return;
  JStr key;
  while (json_obj_next(cur, &key)) {
    if (KEY("_tag")) json_str(cur, tag, sizeof tag);
    else if (KEY("requestId")) req = (int)json_int(cur);
    else if (KEY("values")) json_raw(cur, &values);
    else if (KEY("exit")) json_raw(cur, &exitv);
    else if (KEY("defect")) json_raw(cur, &defect);
    else json_skip(cur);
  }

  if (!strcmp(tag, "Chunk")) {
    PendingRpc *rpc = pending_find(c, req);
    if (rpc && (rpc->kind == RPC_SHELL || rpc->kind == RPC_ARCHIVE) && values.p) {
      JsonCur v;
      json_init(&v, values.p, values.n);
      if (json_arr_begin(&v))
        while (json_arr_next(&v)) apply_shell_item(c, &v, rpc->kind == RPC_ARCHIVE);
    }
    // Streams pause until each chunk is acknowledged.
    send_ack(c, req);
  } else if (!strcmp(tag, "Exit")) {
    PendingRpc *rpc = pending_find(c, req);
    if (!rpc) return;
    PendingRpc copy = *rpc;
    rpc->id = 0;
    JsonCur e;
    json_init(&e, exitv.p, exitv.n);
    char etag[16] = "";
    JStr value = {0}, cause = {0};
    if (json_obj_begin(&e)) {
      JStr k;
      while (json_obj_next(&e, &k)) {
        if (jstr_eq(k, "_tag")) json_str(&e, etag, sizeof etag);
        else if (jstr_eq(k, "value")) json_raw(&e, &value);
        else if (jstr_eq(k, "cause")) json_raw(&e, &cause);
        else json_skip(&e);
      }
    }
    if (!strcmp(etag, "Success")) {
      if (copy.kind == RPC_SHELL) {
        c->state = CONN_ERROR;
        util_strlcpy(c->error, "Server ended thread updates", sizeof c->error);
        return;
      }
      if (copy.kind == RPC_ARCHIVE) {
        c->archives_state = LOAD_FAILED;
        client_toast(c, "Server ended archive updates; toggle SELECT to retry");
        return;
      }
      JsonCur v;
      json_init(&v, value.p ? value.p : "null", value.p ? value.n : 4);
      on_success(c, &copy, &v);
    } else {
      char msg[140];
      extract_error(cause, msg, sizeof msg);
      on_failure(c, &copy, msg);
    }
  } else if (!strcmp(tag, "Defect") || !strcmp(tag, "ClientProtocolError")) {
    char msg[140];
    extract_error(defect, msg, sizeof msg);
    c->state = CONN_ERROR;
    snprintf(c->error, sizeof c->error, "Server error: %s", msg);
  }
}

static void on_ws_message(void *ud, char *data, size_t len) {
  Client *c = ud;
  c->last_rx_ms = client_ticks_ms();
  JsonCur cur;
  json_init(&cur, data, len);
  if (json_peek(&cur) == J_ARR) {
    json_arr_begin(&cur);
    while (json_arr_next(&cur)) handle_message(c, &cur);
  } else {
    handle_message(c, &cur);
  }
}

// ---------------------------------------------------------------- connect

typedef struct {
  Client *client;
  int generation;
  NetAddr addr;
  char token[2048];
  Ws ws;
  bool ok;
  char err[160];
  int64_t server_time, local_time;
} ConnectJob;

static void connect_work(void *arg) {
  ConnectJob *j = arg;
  // The descriptor is public and cheap; it proves the route answers and
  // gives a Date header to correct the 3DS clock.
  HttpResp r;
  if (http_request(&j->addr, "GET", "/.well-known/t3/environment", NULL, NULL, 0, &r) != 0) {
    util_strlcpy(j->err, r.err, sizeof j->err);
    return;
  }
  j->server_time = r.server_time;
  j->local_time = (int64_t)time(NULL);
  http_resp_free(&r);

  char headers[2200];
  snprintf(headers, sizeof headers, "Authorization: Bearer %s\r\n", j->token);
  if (!ws_connect(&j->ws, &j->addr, "/ws?orchestrationProtocol=2", headers)) {
    util_strlcpy(j->err, j->ws.err, sizeof j->err);
    return;
  }
  j->ok = true;
}

static void connect_done(void *arg) {
  ConnectJob *j = arg;
  Client *c = j->client;
  if (j->generation != c->generation) {
    if (j->ok) ws_close(&j->ws);
    free(j);
    return;
  }
  if (!j->ok) {
    c->state = CONN_ERROR;
    c->auth_expired = j->ws.status == 401;
    util_strlcpy(c->error, j->err, sizeof c->error);
    c->last_ping_ms = client_ticks_ms();
    free(j);
    return;
  }
  if (j->server_time) c->clock_offset = j->server_time - j->local_time;
  c->ws = j->ws;
  free(j);
  c->state = CONN_SYNCING;
  c->last_ping_ms = c->last_rx_ms = client_ticks_ms();
  rpc_send(c, "orchestration.subscribeShell", "{}", RPC_SHELL, NULL);
  c->providers_state = LOAD_BUSY;
  rpc_send(c, "server.getConfig", "{}", RPC_CONFIG, NULL);
}

// A reconnect keeps the last data on screen until the new snapshot replaces it.
static void start_connect(Client *c, bool keep_data) {
  c->generation++;
  if (keep_data) {
    threadops_connection_lost(c);
    ws_close(&c->ws);
    memset(c->pending, 0, sizeof c->pending);
    c->archives_state = LOAD_NONE;
    if (c->usage_state == LOAD_BUSY) c->usage_state = LOAD_NONE;
    if (c->providers_state == LOAD_BUSY) c->providers_state = LOAD_NONE;
    if (c->prs_state == LOAD_BUSY) c->prs_state = LOAD_NONE;
    c->tasks_state = LOAD_NONE;
    c->pr_detail_state = LOAD_NONE;
    c->task_action_busy = false;
    c->pr_detail.action_id = 0;
    c->history_state = LOAD_NONE;
    c->history_request++;
    if (c->conv_state == LOAD_BUSY) c->conv_state = c->conv.count ? LOAD_DONE : LOAD_NONE;
    if (c->conv.thread_id[0]) c->conv_stale = true;
  } else {
    reset_session(c);
  }
  c->state = CONN_CONNECTING;
  c->auth_expired = false;
  c->error[0] = '\0';
  ConnectJob *j = calloc(1, sizeof *j);
  j->client = c;
  j->generation = c->generation;
  j->addr = c->addr;
  j->ws.fd = -1;
  util_strlcpy(j->token, c->env.token, sizeof j->token);
  bg_submit(connect_work, connect_done, j);
}

void client_connect(Client *c, const EnvConfig *env) {
  c->env = *env;
  if (!net_parse_addr(env->addr, &c->addr)) {
    c->state = CONN_ERROR;
    util_strlcpy(c->error, "Invalid address", sizeof c->error);
    return;
  }
  start_connect(c, false);
}

// ---------------------------------------------------------------- conversation

typedef struct {
  Client *client;
  int generation;
  NetAddr addr;
  char token[2048];
  char thread_id[ID_CAP];
  Conversation conv;
  int status;
  char err[120];
  int request;
} ConvJob;

static void conv_work(void *arg) {
  ConvJob *j = arg;
  char path[200], headers[2300];
  // Compact drops the turnItems copies of visibleTurnItems, which is all this client reads.
  snprintf(path, sizeof path, "/api/orchestration/threads/%s/bounded?compactTurnItems=1", j->thread_id);
  snprintf(headers, sizeof headers, "Authorization: Bearer %s\r\n%s", j->token, PROTOCOL_HEADER);
  HttpResp r;
  if (http_request(&j->addr, "GET", path, headers, NULL, 0, &r) != 0) {
    util_strlcpy(j->err, r.err, sizeof j->err);
    return;
  }
  j->status = r.status;
  if (r.status == 200) {
    JsonCur cur;
    json_init(&cur, r.body, r.len);
    util_strlcpy(j->conv.thread_id, j->thread_id, sizeof j->conv.thread_id);
    parse_bounded_snapshot(&cur, &j->conv);
  } else {
    snprintf(j->err, sizeof j->err, r.status == 404 ? "Thread not found" : "Server answered HTTP %d", r.status);
  }
  http_resp_free(&r);
}

static void conv_done(void *arg) {
  ConvJob *j = arg;
  Client *c = j->client;
  if (j->generation != c->generation || j->request != c->history_request || strcmp(c->conv.thread_id, j->thread_id) != 0) {
    conversation_clear(&j->conv);
    free(j);
    return;
  }
  if (j->status == 200) {
    conversation_clear(&c->conv);
    c->conv = j->conv;
    c->conv_state = LOAD_DONE;
    c->conv_version++;
  } else {
    conversation_clear(&j->conv);
    // Keep showing what we had; only an empty view reports the failure.
    if (c->conv.count == 0) c->conv_state = LOAD_FAILED;
    else c->conv_state = LOAD_DONE;
    util_strlcpy(c->conv_error, j->err, sizeof c->conv_error);
    if (j->status == 401) c->auth_expired = true;
  }
  c->conv_fetched_ms = client_ticks_ms();
  free(j);
}

static void fetch_conversation(Client *c) {
  T3Thread *t = client_find_thread(c, c->conv.thread_id);
  c->conv_thread_updated_at = t ? t->updated_at : 0;
  c->conv_stale = false;
  c->conv_state = LOAD_BUSY;
  ConvJob *j = calloc(1, sizeof *j);
  j->client = c;
  j->generation = c->generation;
  j->request = c->history_request;
  j->addr = c->addr;
  util_strlcpy(j->token, c->env.token, sizeof j->token);
  util_strlcpy(j->thread_id, c->conv.thread_id, sizeof j->thread_id);
  bg_submit(conv_work, conv_done, j);
}

void client_open_conversation(Client *c, const char *thread_id) {
  c->history_request++;
  c->history_state = LOAD_NONE;
  c->history_loaded = false;
  c->history_prepended = 0;
  conversation_clear(&c->conv);
  c->conv_error[0] = '\0';
  util_strlcpy(c->conv.thread_id, thread_id, sizeof c->conv.thread_id);
  c->conv_version++;
  fetch_conversation(c);
}

void client_close_conversation(Client *c) {
  c->history_request++;
  c->history_state = LOAD_NONE;
  c->history_loaded = false;
  c->history_prepended = 0;
  conversation_clear(&c->conv);
  c->conv_state = LOAD_NONE;
  c->conv_version++;
}

// ---------------------------------------------------------------- loop

void client_update(Client *c) {
  uint64_t now = client_ticks_ms();
  if (c->state == CONN_SYNCING || c->state == CONN_READY) {
    if (!ws_poll(&c->ws, on_ws_message, c)) {
      c->state = CONN_ERROR;
      util_strlcpy(c->error, c->ws.err[0] ? c->ws.err : "Connection lost", sizeof c->error);
      c->last_ping_ms = now;
    } else if (client_ticks_ms() - c->last_rx_ms > RX_TIMEOUT_MS) {
      // Fresh clock: ws_poll just stamped last_rx_ms later than `now`.
      ws_close(&c->ws);
      c->state = CONN_ERROR;
      util_strlcpy(c->error, "Connection timed out", sizeof c->error);
      c->last_ping_ms = now;
    } else if (now - c->last_ping_ms > PING_INTERVAL_MS) {
      static const char ping[] = "{\"_tag\":\"Ping\"}";
      ws_send_text(&c->ws, ping, sizeof ping - 1);
      c->last_ping_ms = now;
    }
  } else if (c->state == CONN_ERROR && !c->auth_expired && c->env.addr[0] &&
             now - c->last_ping_ms > RETRY_INTERVAL_MS) {
    // Wi-Fi drops whenever the lid closes; reconnect quietly.
    start_connect(c, true);
  }

  if (c->state == CONN_ERROR) threadops_connection_lost(c);
  if (c->threads_dirty) {
    qsort(c->threads, (size_t)c->thread_count, sizeof(T3Thread), thread_cmp);
    c->threads_dirty = false;
  }
  if (c->conv_stale && !c->history_loaded && c->history_state != LOAD_BUSY && c->conv_state != LOAD_BUSY && now - c->conv_fetched_ms > CONV_REFRESH_MIN_MS)
    fetch_conversation(c);
  if (c->toast[0] && now > c->toast_until_ms) c->toast[0] = '\0';
}

// ---------------------------------------------------------------- data views

void client_refresh_usage(Client *c) {
  if (c->state != CONN_READY && c->state != CONN_SYNCING) return;
  int64_t today = today_days(c);
  int y1, m1, d1, y2, m2, d2;
  util_civil_from_days(today - (USAGE_DAYS - 1), &y1, &m1, &d1);
  util_civil_from_days(today, &y2, &m2, &d2);
  // IANA's Etc zones invert the sign: Etc/GMT-2 is UTC+2.
  int tz = utc_offset_hours(c);
  char zone[16];
  if (tz == 0) snprintf(zone, sizeof zone, "UTC");
  else snprintf(zone, sizeof zone, "Etc/GMT%c%d", tz > 0 ? '-' : '+', tz > 0 ? tz : -tz);
  char payload[200];
  snprintf(payload, sizeof payload,
           "{\"sinceDay\":\"%04d-%02d-%02d\",\"untilDay\":\"%04d-%02d-%02d\",\"timeZone\":\"%s\","
           "\"resolution\":\"day\"}",
           y1, m1, d1, y2, m2, d2, zone);
  c->usage_state = LOAD_BUSY;
  if (rpc_send(c, "server.getUsageSummary", payload, RPC_USAGE, NULL) < 0) c->usage_state = LOAD_FAILED;
  c->providers_state = LOAD_BUSY;
  if (rpc_send(c, "server.getConfig", "{}", RPC_CONFIG, NULL) < 0) c->providers_state = LOAD_FAILED;
}

void client_refresh_prs(Client *c) {
  if (c->state != CONN_READY && c->state != CONN_SYNCING) return;
  c->prs_state = LOAD_BUSY;
  if (rpc_send(c, "pullRequests.list", "{\"state\":\"open\",\"involvement\":\"all\",\"limit\":60}", RPC_PRS, NULL) < 0)
    c->prs_state = LOAD_FAILED;
}

void client_load_archived(Client *c) {
  if (c->state != CONN_READY && c->state != CONN_SYNCING) return;
  if (c->archives_state == LOAD_BUSY || c->archives_state == LOAD_DONE) return;
  c->archives_state = LOAD_BUSY;
  if (rpc_send(c, "orchestration.subscribeArchivedShell", "{}", RPC_ARCHIVE, NULL) < 0)
    c->archives_state = LOAD_FAILED;
}

// ---------------------------------------------------------------- commands

static void begin_command(JBuf *b, const char *type, const T3Thread *t) {
  char cmd[37];
  util_uuid(cmd);
  jb_init(b);
  jb_raw(b, "{");
  jb_kv_str(b, "type", type);
  jb_kv_str(b, "commandId", cmd);
  jb_kv_str(b, "threadId", t->id);
}

static void dispatch(Client *c, JBuf *b, const char *done_label) {
  jb_raw(b, "}");
  rpc_send(c, "orchestration.dispatchCommand", b->p, RPC_COMMAND, done_label);
  jb_free(b);
}

void client_send_message(Client *c, const T3Thread *t, const char *text) {
  char mid[37];
  util_uuid(mid);
  JBuf b;
  begin_command(&b, "message.dispatch", t);
  jb_kv_str(&b, "createdBy", "user");
  jb_kv_str(&b, "creationSource", "mobile");
  jb_kv_str(&b, "messageId", mid);
  jb_kv_str(&b, "text", text);
  jb_raw(&b, ",\"attachments\":[]");
  // No deliveryIntent: "auto" would steer a running agent. This waits behind the active run,
  // or starts at once when nothing runs; More → Queue can still promote it to a steer.
  jb_raw(&b, ",\"dispatchMode\":{\"type\":\"queue_after_active\"}");
  dispatch(c, &b, t->active_run_id[0] ? "Message queued" : "Message sent");
}

void client_interrupt(Client *c, const T3Thread *t) {
  if (!t->active_run_id[0]) {
    client_toast(c, "Nothing is running");
    return;
  }
  JBuf b;
  begin_command(&b, "run.interrupt", t);
  jb_kv_str(&b, "runId", t->active_run_id);
  dispatch(c, &b, "Stopping");
}

void client_respond(Client *c, const T3Thread *t, const char *decision) {
  if (!(client_approval_options(c, t) & approval_decision_bit(decision))) return;
  JBuf b;
  begin_command(&b, "runtime-request.respond", t);
  jb_kv_str(&b, "requestId", t->pending_id);
  jb_kv_str(&b, "decision", decision);
  const char *label = !strcmp(decision, "decline") ? "Declined"
                      : !strcmp(decision, "cancel") ? "Approval cancelled"
                      : !strcmp(decision, "acceptForSession") ? "Allowed for session"
                      : !strcmp(decision, "acceptAlways") ? "Always allowed" : "Approved";
  dispatch(c, &b, label);
}

unsigned client_approval_options(const Client *c, const T3Thread *t) {
  if (!t->pending_id[0] || !strcmp(t->pending_kind, "user_input") || !strcmp(t->pending_kind, "auth_refresh"))
    return 0;
  if (!strcmp(c->conv.thread_id, t->id)) {
    for (int i = c->conv.count - 1; i >= 0; i--) {
      const ConvItem *it = &c->conv.items[i];
      if (it->kind == ITEM_APPROVAL && !strcmp(it->request_id, t->pending_id)) return it->approval_options;
    }
  }
  return APPROVAL_DEFAULT;
}

void client_set_settled(Client *c, const T3Thread *t, bool settled) {
  JBuf b;
  begin_command(&b, settled ? "thread.settle" : "thread.unsettle", t);
  if (!settled) jb_kv_str(&b, "reason", "user");
  dispatch(c, &b, settled ? "Marked done" : "Moved back to active");
}

void client_set_pinned(Client *c, const T3Thread *t, bool pinned) {
  JBuf b;
  begin_command(&b, pinned ? "thread.pin" : "thread.unpin", t);
  dispatch(c, &b, pinned ? "Pinned" : "Unpinned");
}

void client_set_archived(Client *c, const T3Thread *t, bool archived) {
  client_load_archived(c);
  JBuf b;
  begin_command(&b, archived ? "thread.archive" : "thread.unarchive", t);
  dispatch(c, &b, archived ? "Archived" : "Unarchived");
}

void client_mark_unread(Client *c, const T3Thread *t) {
  if (!t->latest_completed_ms) {
    client_toast(c, "No completed run to mark unread");
    return;
  }
  JBuf b;
  begin_command(&b, "thread.mark-unread", t);
  dispatch(c, &b, "Marked unread");
}

void client_mark_read(Client *c, const T3Thread *t) {
  int64_t visited_ms = client_now(c) * 1000;
  if (t->latest_completed_ms > visited_ms) visited_ms = t->latest_completed_ms;
  time_t seconds = (time_t)(visited_ms / 1000);
  char date[24], visited[40];
  strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", gmtime(&seconds));
  snprintf(visited, sizeof visited, "%s.%03dZ", date, (int)(visited_ms % 1000));
  JBuf b;
  begin_command(&b, "thread.visit", t);
  jb_kv_str(&b, "visitedAt", visited);
  dispatch(c, &b, NULL);
}

void client_rename(Client *c, const T3Thread *t, const char *title) {
  JBuf b;
  begin_command(&b, "thread.metadata.update", t);
  jb_kv_str(&b, "title", title);
  dispatch(c, &b, "Renamed");
}

bool client_can_dismiss_question(const Client *c, const T3Thread *t) {
  if (!t->pending_id[0] || strcmp(t->pending_kind, "user_input")) return false;
  if (!strcmp(c->conv.thread_id, t->id)) {
    for (int i = c->conv.count - 1; i >= 0; i--) {
      const ConvItem *it = &c->conv.items[i];
      if (it->kind == ITEM_QUESTION && !strcmp(it->request_id, t->pending_id)) return it->message_response;
    }
  }
  return !t->pending_blocking;
}

void client_dismiss_question(Client *c, const T3Thread *t) {
  if (!t->pending_id[0] || strcmp(t->pending_kind, "user_input")) return;
  if (!client_can_dismiss_question(c, t)) {
    client_toast(c, "This question needs an answer; reply or stop");
    return;
  }
  JBuf b;
  begin_command(&b, "thread.user-input.dismiss", t);
  jb_kv_str(&b, "requestId", t->pending_id);
  dispatch(c, &b, "Question dismissed");
}

void client_set_interaction_mode(Client *c, const T3Thread *t, const char *mode) {
  JBuf b;
  begin_command(&b, "thread.interaction-mode.set", t);
  jb_kv_str(&b, "interactionMode", mode);
  dispatch(c, &b, !strcmp(mode, "plan") ? "Plan mode on" : "Plan mode off");
}

void client_set_runtime_mode(Client *c, const T3Thread *t, const char *mode) {
  JBuf b;
  begin_command(&b, "thread.runtime-mode.set", t);
  jb_kv_str(&b, "runtimeMode", mode);
  dispatch(c, &b, "Access mode updated");
}

void client_snooze(Client *c, const T3Thread *t, int64_t until) {
  JBuf b;
  begin_command(&b, until ? "thread.snooze" : "thread.unsnooze", t);
  if (until) {
    time_t epoch = (time_t)until;
    struct tm utc;
    char iso[32];
    if (!gmtime_r(&epoch, &utc) || !strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S.000Z", &utc)) {
      jb_free(&b);
      return;
    }
    jb_kv_str(&b, "snoozedUntil", iso);
  } else jb_kv_str(&b, "reason", "user");
  dispatch(c, &b, until ? "Thread snoozed" : "Thread awake");
}

void client_set_model(Client *c, const T3Thread *t, const char *selection_json) {
  JBuf b;
  begin_command(&b, "thread.model-selection.set", t);
  jb_key(&b, "modelSelection");
  jb_raw(&b, selection_json);
  dispatch(c, &b, "Model updated");
}

void client_new_thread(Client *c, const Project *p, const char *text) {
  client_launch_thread(c, p, text, NULL, NULL);
}

void client_launch_thread(Client *c, const Project *p, const char *text, const char *selection_json,
                          const LaunchWorkspace *workspace) {
  const T3Thread *newest = NULL;
  for (int i = 0; i < c->thread_count; i++) {
    const T3Thread *t = &c->threads[i];
    if (!t->subagent && strcmp(t->project_id, p->id) == 0 && t->model_selection_json && (!newest || t->updated_at > newest->updated_at))
      newest = t;
  }
  const char *model = selection_json ? selection_json : newest ? newest->model_selection_json : p->default_model_json;
  JBuf fallback;
  models_selection(&fallback, &c->catalog, model ? -1 : 0);
  if (!model) model = fallback.n ? fallback.p : NULL;
  if (!model) {
    jb_free(&fallback);
    client_toast(c, "No models available; check the machine's providers");
    return;
  }
  char title[64];
  util_strlcpy(title, text, sizeof title);
  title[strcspn(title, "\n")] = '\0';
  char cmd[37], mid[37];
  util_uuid(cmd);
  util_uuid(mid);
  JBuf b;
  jb_init(&b);
  jb_raw(&b, "{");
  jb_kv_str(&b, "commandId", cmd);
  jb_kv_str(&b, "creationSource", "mobile");
  jb_kv_str(&b, "projectId", p->id);
  jb_kv_str(&b, "title", title[0] ? title : "New thread");
  jb_raw(&b, ",\"generateTitle\":true,\"modelSelection\":");
  jb_raw(&b, model);
  // Same defaults the other clients fall back to.
  jb_kv_str(&b, "runtimeMode", newest && newest->runtime_mode[0] ? newest->runtime_mode : "full-access");
  jb_kv_str(&b, "interactionMode", newest && newest->interaction_mode[0] ? newest->interaction_mode : "default");
  jb_raw(&b, ",\"workspaceStrategy\":");
  launch_workspace_json(&b, workspace);
  jb_raw(&b, ",\"initialMessage\":{");
  jb_kv_str(&b, "messageId", mid);
  jb_kv_str(&b, "text", text);
  jb_raw(&b, ",\"attachments\":[]}}");
  rpc_send(c, "orchestration.launchThread", b.p, RPC_LAUNCH, NULL);
  jb_free(&b);
  jb_free(&fallback);
}

// ---------------------------------------------------------------- pairing

typedef struct {
  char address[160];
  char code[32];
  PairDone done;
  void *ud;
  PairResult result;
} PairJob;

static void form_append(JBuf *b, const char *key, const char *val) {
  if (b->n) jb_raw(b, "&");
  jb_raw(b, key);
  jb_raw(b, "=");
  for (const char *p = val; *p; p++) {
    unsigned char ch = (unsigned char)*p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr("-_.~", ch)) {
      jb_rawn(b, p, 1);
    } else {
      char esc[4];
      snprintf(esc, sizeof esc, "%%%02X", ch);
      jb_raw(b, esc);
    }
  }
}

static void pair_work(void *arg) {
  PairJob *j = arg;
  PairResult *res = &j->result;
  NetAddr addr;
  if (!net_parse_addr(j->address, &addr)) {
    util_strlcpy(res->error, "Enter an http address such as 192.168.1.20:3773", sizeof res->error);
    return;
  }
  HttpResp r;
  if (http_request(&addr, "GET", "/.well-known/t3/environment", NULL, NULL, 0, &r) != 0) {
    util_strlcpy(res->error, r.err, sizeof res->error);
    return;
  }
  if (r.status != 200) {
    snprintf(res->error, sizeof res->error, "%s:%d is not a T3 Code server", addr.host, addr.port);
    http_resp_free(&r);
    return;
  }
  JsonCur cur;
  json_init(&cur, r.body, r.len);
  if (json_obj_begin(&cur)) {
    JStr key;
    while (json_obj_next(&cur, &key)) {
      if (KEY("label")) json_str(&cur, res->env.label, sizeof res->env.label);
      else if (KEY("environmentId")) json_str(&cur, res->env.environment_id, sizeof res->env.environment_id);
      else json_skip(&cur);
    }
  }
  http_resp_free(&r);

  JBuf form;
  form.p = NULL;
  jb_init(&form);
  form_append(&form, "grant_type", "urn:ietf:params:oauth:grant-type:token-exchange");
  form_append(&form, "subject_token", j->code);
  form_append(&form, "subject_token_type", "urn:t3:params:oauth:token-type:environment-bootstrap");
  form_append(&form, "requested_token_type", "urn:ietf:params:oauth:token-type:access_token");
  form_append(&form, "client_label", "Nintendo 3DS");
  form_append(&form, "client_device_type", "mobile");
  form_append(&form, "client_os", "Nintendo 3DS");
  int rc = http_request(&addr, "POST", "/oauth/token", "Content-Type: application/x-www-form-urlencoded\r\n", form.p,
                        form.n, &r);
  jb_free(&form);
  if (rc != 0) {
    util_strlcpy(res->error, r.err, sizeof res->error);
    return;
  }
  if (r.status != 200) {
    if (r.status == 400 || r.status == 401)
      util_strlcpy(res->error, "Pairing code was rejected or already used", sizeof res->error);
    else snprintf(res->error, sizeof res->error, "Pairing failed (HTTP %d)", r.status);
    http_resp_free(&r);
    return;
  }
  json_init(&cur, r.body, r.len);
  if (json_obj_begin(&cur)) {
    JStr key;
    while (json_obj_next(&cur, &key)) {
      if (KEY("access_token")) json_str(&cur, res->env.token, sizeof res->env.token);
      else json_skip(&cur);
    }
  }
  http_resp_free(&r);
  if (!res->env.token[0]) {
    util_strlcpy(res->error, "Server did not return a session", sizeof res->error);
    return;
  }
  snprintf(res->env.addr, sizeof res->env.addr, "%s:%d", addr.host, addr.port);
  if (!res->env.label[0]) util_strlcpy(res->env.label, addr.host, sizeof res->env.label);
  res->ok = true;
}

static void pair_done(void *arg) {
  PairJob *j = arg;
  j->done(j->ud, &j->result);
  free(j);
}

void client_pair(const char *address, const char *code, PairDone done, void *ud) {
  PairJob *j = calloc(1, sizeof *j);
  util_strlcpy(j->address, address, sizeof j->address);
  // Codes are shown in uppercase groups; accept any case and separators.
  size_t n = 0;
  for (const char *p = code; *p && n < sizeof j->code - 1; p++) {
    if (*p == ' ' || *p == '-') continue;
    j->code[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
  }
  j->done = done;
  j->ud = ud;
  bg_submit(pair_work, pair_done, j);
}

void client_answer_questions(Client *c, const T3Thread *t, const char *request_id,
                             const QuestionSet *set, const QuestionDraft *drafts) {
  if (strcmp(t->pending_id, request_id)) {
    client_toast(c, "That question is no longer pending");
    return;
  }
  JBuf answers;
  jb_init(&answers);
  if (!questions_build_answers(&answers, set, drafts)) {
    jb_free(&answers);
    client_toast(c, "Answer every question first");
    return;
  }
  JBuf b;
  begin_command(&b, "runtime-request.respond", t);
  jb_kv_str(&b, "requestId", request_id);
  jb_key(&b, "answers");
  jb_rawn(&b, answers.p, answers.n);
  jb_free(&answers);
  dispatch(c, &b, "Answers submitted");
}

void client_queue_action(Client *c, const T3Thread *t, int index, QueueAction action, const char *text) {
  JBuf b;
  begin_command(&b, queue_command_type(action), t);
  if (!queue_command_fields(&b, c->conv.queue, index, action, t->active_run_id, text)) {
    jb_free(&b);
    client_toast(c, "Queue action unavailable");
    return;
  }
  dispatch(c, &b, queue_done_label(action));
}
