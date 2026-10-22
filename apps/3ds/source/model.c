#include "model.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define KEY(k) jstr_eq(key, k)

static int64_t read_time(JsonCur *c) {
  char tmp[40];
  if (!json_str(c, tmp, sizeof tmp)) return 0;
  return util_parse_iso(tmp);
}

static int64_t read_time_ms(JsonCur *c) {
  char tmp[40];
  if (!json_str(c, tmp, sizeof tmp)) return 0;
  int64_t seconds = util_parse_iso(tmp);
  if (!seconds) return 0;
  int ms = 0;
  const char *p = strchr(tmp, '.');
  if (p) {
    for (int scale = 100; scale && *++p >= '0' && *p <= '9'; scale /= 10)
      ms += (*p - '0') * scale;
  }
  return seconds * 1000 + ms;
}

static char *read_raw_dup(JsonCur *c) {
  JStr raw;
  if (json_peek(c) != J_OBJ || !json_raw(c, &raw)) {
    json_skip(c);
    return NULL;
  }
  char *s = malloc(raw.n + 1);
  memcpy(s, raw.p, raw.n);
  s[raw.n] = '\0';
  return s;
}

static void read_pr_ref(JsonCur *c, char *out, size_t cap) {
  if (!json_obj_begin(c)) return;
  char repo[80] = "";
  int number = 0;
  JStr key;
  while (json_obj_next(c, &key)) {
    if (KEY("repository")) json_str(c, repo, sizeof repo);
    else if (KEY("number")) number = (int)json_int(c);
    else json_skip(c);
  }
  if (repo[0] && number) {
    const char *slash = strrchr(repo, '/');
    snprintf(out, cap, "%s#%d", slash ? slash + 1 : repo, number);
  }
}

void thread_free(T3Thread *t) {
  free(t->model_selection_json);
  t->model_selection_json = NULL;
}

void project_free(Project *p) {
  free(p->default_model_json);
  p->default_model_json = NULL;
}

void parse_thread_shell(JsonCur *c, T3Thread *t) {
  thread_free(t);
  memset(t, 0, sizeof *t);
  if (!json_obj_begin(c)) return;
  char settled_override[12] = "";
  int64_t settled_at = 0;
  int64_t visited_ms = 0;
  char linked_pr[64] = "", branch_pr[64] = "";
  JStr key;
  while (json_obj_next(c, &key)) {
    if (KEY("id")) json_str(c, t->id, sizeof t->id);
    else if (KEY("projectId")) json_str(c, t->project_id, sizeof t->project_id);
    else if (KEY("title")) json_str(c, t->title, sizeof t->title);
    else if (KEY("status")) json_str(c, t->status, sizeof t->status);
    else if (KEY("activeRunId")) json_str(c, t->active_run_id, sizeof t->active_run_id);
    else if (KEY("providerInstanceId")) json_str(c, t->provider, sizeof t->provider);
    else if (KEY("runtimeMode")) json_str(c, t->runtime_mode, sizeof t->runtime_mode);
    else if (KEY("interactionMode")) json_str(c, t->interaction_mode, sizeof t->interaction_mode);
    else if (KEY("worktreePath")) json_str(c, t->worktree_path, sizeof t->worktree_path);
    else if (KEY("branch")) json_str(c, t->branch, sizeof t->branch);
    else if (KEY("updatedAt")) t->updated_at = read_time(c);
    else if (KEY("snoozedUntil")) t->snoozed_until = read_time(c);
    else if (KEY("pinnedAt")) t->pinned_at = read_time(c);
    else if (KEY("archivedAt")) t->archived = read_time(c) != 0;
    else if (KEY("lastVisitedAt")) visited_ms = read_time_ms(c);
    else if (KEY("latestRunCompletedAt")) t->latest_completed_ms = read_time_ms(c);
    else if (KEY("settledAt")) settled_at = read_time(c);
    else if (KEY("settledOverride")) json_str(c, settled_override, sizeof settled_override);
    else if (KEY("lastError")) json_str(c, t->last_error, sizeof t->last_error);
    else if (KEY("visibleItemCount")) t->item_count = (int)json_int(c);
    else if (KEY("linkedPullRequest")) read_pr_ref(c, linked_pr, sizeof linked_pr);
    else if (KEY("branchPullRequest")) read_pr_ref(c, branch_pr, sizeof branch_pr);
    else if (KEY("modelSelection")) {
      JsonCur probe = *c;
      t->model_selection_json = read_raw_dup(c);
      if (json_obj_begin(&probe)) {
        JStr k2;
        while (json_obj_next(&probe, &k2)) {
          if (jstr_eq(k2, "model")) json_str(&probe, t->model, sizeof t->model);
          else json_skip(&probe);
        }
      }
    } else if (KEY("lineage")) {
      if (json_obj_begin(c)) {
        JStr k2;
        while (json_obj_next(c, &k2)) {
          if (jstr_eq(k2, "relationshipToParent")) {
            char rel[16];
            json_str(c, rel, sizeof rel);
            t->subagent = strcmp(rel, "subagent") == 0;
          } else if (jstr_eq(k2, "parentThreadId")) json_str(c, t->parent_id, sizeof t->parent_id);
          else json_skip(c);
        }
      }
    } else if (KEY("pendingRuntimeRequest")) {
      if (json_obj_begin(c)) {
        t->pending_blocking = true;
        JStr k2;
        while (json_obj_next(c, &k2)) {
          if (jstr_eq(k2, "id")) json_str(c, t->pending_id, sizeof t->pending_id);
          else if (jstr_eq(k2, "kind")) json_str(c, t->pending_kind, sizeof t->pending_kind);
          else if (jstr_eq(k2, "blocking")) t->pending_blocking = json_bool(c);
          else json_skip(c);
        }
      }
    } else if (KEY("latestVisibleMessage")) {
      if (json_obj_begin(c)) {
        JStr k2;
        while (json_obj_next(c, &k2)) {
          if (jstr_eq(k2, "role")) json_str(c, t->last_role, sizeof t->last_role);
          else if (jstr_eq(k2, "text")) json_str(c, t->last_text, sizeof t->last_text);
          else json_skip(c);
        }
      }
    } else json_skip(c);
  }
  t->settled = strcmp(settled_override, "settled") == 0 ||
               (settled_at > 0 && strcmp(settled_override, "active") != 0);
  // Mark-unread places the visit just one millisecond before completion.
  t->unread = visited_ms > 0 && t->latest_completed_ms > visited_ms;
  util_strlcpy(t->pr, linked_pr[0] ? linked_pr : branch_pr, sizeof t->pr);
  // Collapse newlines so previews stay on one line.
  for (char *p = t->last_text; *p; p++)
    if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
}

void parse_project_shell(JsonCur *c, Project *p) {
  project_free(p);
  memset(p, 0, sizeof *p);
  if (!json_obj_begin(c)) return;
  JStr key;
  while (json_obj_next(c, &key)) {
    if (KEY("id")) json_str(c, p->id, sizeof p->id);
    else if (KEY("title")) json_str(c, p->title, sizeof p->title);
    else if (KEY("workspaceRoot")) json_str(c, p->root, sizeof p->root);
    else if (KEY("defaultModelSelection")) p->default_model_json = read_raw_dup(c);
    else json_skip(c);
  }
}

void parse_provider_usage(JsonCur *c, ProviderUsage *u, bool *ok) {
  memset(u, 0, sizeof *u);
  *ok = false;
  if (!json_obj_begin(c)) return;
  char driver[32] = "";
  JStr key;
  while (json_obj_next(c, &key)) {
    if (KEY("instanceId")) json_str(c, u->instance_id, sizeof u->instance_id);
    else if (KEY("driver")) json_str(c, driver, sizeof driver);
    else if (KEY("displayName")) json_str(c, u->name, sizeof u->name);
    else if (KEY("status")) json_str(c, u->status, sizeof u->status);
    else if (KEY("enabled")) u->enabled = json_bool(c);
    else if (KEY("usageLimits")) {
      if (!json_obj_begin(c)) continue;
      JStr k2;
      while (json_obj_next(c, &k2)) {
        if (jstr_eq(k2, "windows") && json_arr_begin(c)) {
          while (json_arr_next(c)) {
            if (u->window_count >= 6 || !json_obj_begin(c)) {
              json_skip(c);
              continue;
            }
            UsageWindow *w = &u->windows[u->window_count++];
            JStr k3;
            while (json_obj_next(c, &k3)) {
              if (jstr_eq(k3, "label")) json_str(c, w->label, sizeof w->label);
              else if (jstr_eq(k3, "usedPercent")) w->used_pct = (float)json_num(c);
              else if (jstr_eq(k3, "resetsAt")) w->resets_at = read_time(c);
              else json_skip(c);
            }
          }
        } else if (jstr_eq(k2, "unavailable") && json_obj_begin(c)) {
          JStr k3;
          while (json_obj_next(c, &k3)) {
            if (jstr_eq(k3, "message")) json_str(c, u->note, sizeof u->note);
            else if (jstr_eq(k3, "reason") && !u->note[0]) {
              char reason[24];
              json_str(c, reason, sizeof reason);
              util_strlcpy(u->note, strcmp(reason, "unsupported") == 0 ? "Limits not reported for this account"
                                                                         : "Could not read limits",
                           sizeof u->note);
            } else json_skip(c);
          }
        } else json_skip(c);
      }
    } else json_skip(c);
  }
  if (!u->name[0]) util_strlcpy(u->name, driver[0] ? driver : u->instance_id, sizeof u->name);
  *ok = u->instance_id[0] != '\0';
}

void parse_pull_request(JsonCur *c, PullRequest *pr) {
  memset(pr, 0, sizeof *pr);
  if (!json_obj_begin(c)) return;
  JStr key;
  while (json_obj_next(c, &key)) {
    if (KEY("projectId")) json_str(c, pr->project_id, sizeof pr->project_id);
    else if (KEY("projectTitle")) json_str(c, pr->project_title, sizeof pr->project_title);
    else if (KEY("host")) json_str(c, pr->host, sizeof pr->host);
    else if (KEY("repository")) json_str(c, pr->repo, sizeof pr->repo);
    else if (KEY("number")) pr->number = (int)json_int(c);
    else if (KEY("title")) json_str(c, pr->title, sizeof pr->title);
    else if (KEY("headBranch")) json_str(c, pr->head, sizeof pr->head);
    else if (KEY("baseBranch")) json_str(c, pr->base, sizeof pr->base);
    else if (KEY("state")) json_str(c, pr->state, sizeof pr->state);
    else if (KEY("isDraft")) pr->draft = json_bool(c);
    else if (KEY("checksState")) json_str(c, pr->checks, sizeof pr->checks);
    else if (KEY("reviewDecision")) json_str(c, pr->review, sizeof pr->review);
    else if (KEY("mergeability")) json_str(c, pr->mergeability, sizeof pr->mergeability);
    else if (KEY("additions")) pr->additions = (int)json_int(c);
    else if (KEY("deletions")) pr->deletions = (int)json_int(c);
    else if (KEY("updatedAt")) pr->updated_at = read_time(c);
    else if (KEY("viewerReviewRequested")) pr->review_requested = json_bool(c);
    else if (KEY("author")) {
      if (!json_obj_begin(c)) continue;
      JStr k2;
      while (json_obj_next(c, &k2)) {
        if (jstr_eq(k2, "login")) json_str(c, pr->author, sizeof pr->author);
        else json_skip(c);
      }
    } else if (KEY("labels")) {
      if (!json_arr_begin(c)) continue;
      while (json_arr_next(c)) {
        if (!json_obj_begin(c)) continue;
        JStr k2;
        while (json_obj_next(c, &k2)) {
          if (jstr_eq(k2, "name")) {
            char name[40];
            json_str(c, name, sizeof name);
            size_t used = strlen(pr->labels);
            if (used + strlen(name) + 3 < sizeof pr->labels)
              snprintf(pr->labels + used, sizeof pr->labels - used, "%s%s", used ? ", " : "", name);
          } else json_skip(c);
        }
      }
    } else json_skip(c);
  }
}

void parse_usage_summary(JsonCur *c, UsageSummary *u, int64_t today_days) {
  memset(u, 0, sizeof *u);
  if (!json_obj_begin(c)) return;
  // Model costs are summed in a small table to pick the most expensive one.
  struct {
    char model[48];
    double cost;
  } models[24];
  int model_count = 0;
  JStr key;
  while (json_obj_next(c, &key)) {
    if (KEY("buckets") && json_arr_begin(c)) {
      while (json_arr_next(c)) {
        if (!json_obj_begin(c)) continue;
        char day[16] = "", provider[16] = "", model[48] = "";
        double cost = 0, tokens = 0;
        JStr k2;
        while (json_obj_next(c, &k2)) {
          if (jstr_eq(k2, "day")) json_str(c, day, sizeof day);
          else if (jstr_eq(k2, "provider")) json_str(c, provider, sizeof provider);
          else if (jstr_eq(k2, "model")) json_str(c, model, sizeof model);
          else if (jstr_eq(k2, "costUsd")) cost = json_num(c);
          else if (jstr_eq(k2, "totals") && json_obj_begin(c)) {
            JStr k3;
            while (json_obj_next(c, &k3)) {
              // reasoningTokens is a subset of outputTokens; never add it.
              if (jstr_eq(k3, "reasoningTokens")) json_skip(c);
              else tokens += json_num(c);
            }
          } else json_skip(c);
        }
        int y, m, d;
        if (sscanf(day, "%d-%d-%d", &y, &m, &d) == 3) {
          int64_t idx = USAGE_DAYS - 1 - (today_days - util_days_from_civil(y, m, d));
          if (idx >= 0 && idx < USAGE_DAYS) {
            u->day_cost[idx] += cost;
            u->day_tokens[idx] += tokens;
          }
        }
        int pi = 0;
        while (pi < u->provider_count && strcmp(u->providers[pi].provider, provider) != 0) pi++;
        if (pi == u->provider_count && pi < 8) {
          util_strlcpy(u->providers[pi].provider, provider, sizeof u->providers[pi].provider);
          u->provider_count++;
        }
        if (pi < 8) {
          u->providers[pi].cost += cost;
          u->providers[pi].tokens += tokens;
        }
        int mi = 0;
        while (mi < model_count && strcmp(models[mi].model, model) != 0) mi++;
        if (mi == model_count && mi < 24) {
          util_strlcpy(models[mi].model, model, sizeof models[mi].model);
          models[mi].cost = 0;
          model_count++;
        }
        if (mi < 24) models[mi].cost += cost;
      }
    } else if (KEY("sources") && json_arr_begin(c)) {
      while (json_arr_next(c)) {
        if (!json_obj_begin(c)) continue;
        JStr k2;
        while (json_obj_next(c, &k2)) {
          if (jstr_eq(k2, "distinctSessions")) u->sessions += (int)json_int(c);
          else json_skip(c);
        }
      }
    } else if (KEY("pricing") && json_obj_begin(c)) {
      JStr k2;
      while (json_obj_next(c, &k2)) {
        if (jstr_eq(k2, "status")) json_str(c, u->pricing, sizeof u->pricing);
        else json_skip(c);
      }
    } else json_skip(c);
  }
  for (int i = 0; i < model_count; i++) {
    if (models[i].cost > u->top_model_cost) {
      u->top_model_cost = models[i].cost;
      util_strlcpy(u->top_model, models[i].model, sizeof u->top_model);
    }
  }
  u->loaded = true;
}

static ConvItem *conv_push(Conversation *conv, ItemKind kind, char *text) {
  if (!text) return NULL;
  if (conv->count >= MAX_CONV_ITEMS || conv->text_bytes + strlen(text) + 1 > MAX_CONV_TEXT) {
    conv->limited = true;
    free(text);
    return NULL;
  }
  ConvItem *items = realloc(conv->items, sizeof(ConvItem) * (size_t)(conv->count + 1));
  if (!items) {
    free(text);
    return NULL;
  }
  conv->items = items;
  ConvItem *it = &conv->items[conv->count++];
  memset(it, 0, sizeof *it);
  it->kind = kind;
  it->text = text;
  it->exit_code = -1;
  it->additions = it->deletions = -1;
  return it;
}

static char *dupf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  char *s = malloc((size_t)n + 1);
  if (!s) return NULL;
  va_start(ap, fmt);
  vsnprintf(s, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return s;
}

// Keeps conversation memory bounded: long tool output and messages are cut.
static char *clip(char *s, size_t max) {
  if (!s) return s;
  size_t n = strlen(s);
  if (n <= max) return s;
  while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80) max--;
  memcpy(s + max, "...", 4);
  char *trimmed = realloc(s, max + 4);
  return trimmed ? trimmed : s;
}

// Keeps the last `lines` lines of command output.
static char *tail_lines(char *s, int lines) {
  if (!s) return s;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = '\0';
  if (n == 0) {
    free(s);
    return NULL;
  }
  const char *p = s + n;
  int seen = 0;
  while (p > s && seen < lines) {
    p--;
    if (*p == '\n') seen++;
  }
  if (*p == '\n') p++;
  size_t keep = n - (size_t)(p - s);
  if (keep > 600) {
    p = s + n - 600;
    while (((unsigned char)*p & 0xC0) == 0x80) p++;
    keep = n - (size_t)(p - s);
  }
  memmove(s, p, keep + 1);
  char *trimmed = realloc(s, keep + 1);
  return trimmed ? trimmed : s;
}

// Fields of one turn item that the conversation view uses. Everything else is skipped.
typedef struct {
  char type[32], status[16], title[160], name[200], run_id[ID_CAP], request_id[ID_CAP], kind[48];
  char *text, *detail;
  bool streaming, output_failed, answered, message_response;
  unsigned approval_options;
  int exit_code, adds, dels, changes, results;
  int64_t started, ended, updated;
  JBuf steps;  // todo steps or question lines
  QuestionSet *questions;
} RawItem;

static void take_str(JsonCur *c, char **dst) {
  if (*dst) json_skip(c);
  else *dst = json_str_dup(c);
}

static void parse_steps(JsonCur *c, RawItem *r) {
  if (!json_arr_begin(c)) return;
  while (json_arr_next(c)) {
    if (!json_obj_begin(c)) continue;
    char text[300] = "", status[16] = "";
    JStr k;
    while (json_obj_next(c, &k)) {
      if (jstr_eq(k, "text")) json_str(c, text, sizeof text);
      else if (jstr_eq(k, "status")) json_str(c, status, sizeof status);
      else json_skip(c);
    }
    if (!text[0]) continue;
    jb_raw(&r->steps, !strcmp(status, "completed") ? "c" : !strcmp(status, "running") ? "r" : "p");
    jb_raw(&r->steps, text);
    jb_raw(&r->steps, "\n");
  }
}

static void parse_questions(JsonCur *c, RawItem *r) {
  free(r->questions);
  r->questions = questions_parse(c);
  if (!r->questions) return;
  for (int i = 0; i < r->questions->count; i++) {
    const Question *q = &r->questions->questions[i];
    if (!r->text) r->text = strdup(q->prompt);
    jb_raw(&r->steps, "**");
    jb_raw(&r->steps, q->prompt);
    jb_raw(&r->steps, "**\n");
    for (int j = 0; j < q->option_count; j++) {
      jb_raw(&r->steps, "- ");
      jb_raw(&r->steps, q->options[j].label);
      jb_raw(&r->steps, "\n");
    }
    jb_raw(&r->steps, "\n");
  }
}

static void parse_changes(JsonCur *c, RawItem *r) {
  if (!json_arr_begin(c)) return;
  while (json_arr_next(c)) {
    r->changes++;
    json_skip(c);
  }
}

static void parse_results(JsonCur *c, RawItem *r) {
  if (!json_arr_begin(c)) return;
  while (json_arr_next(c)) {
    r->results++;
    json_skip(c);
  }
}

static void parse_turn_item(JsonCur *c, Conversation *conv) {
  if (conv->limited) {
    json_skip(c);
    return;
  }
  if (!json_obj_begin(c)) return;
  RawItem r;
  memset(&r, 0, sizeof r);
  r.exit_code = r.adds = r.dels = -1;
  r.approval_options = APPROVAL_DEFAULT;
  jb_init(&r.steps);
  char ts[40];
  JStr key;
  while (json_obj_next(c, &key)) {
    if (KEY("type")) json_str(c, r.type, sizeof r.type);
    else if (KEY("status")) json_str(c, r.status, sizeof r.status);
    else if (KEY("title")) json_str(c, r.title, sizeof r.title);
    else if (KEY("runId")) json_str(c, r.run_id, sizeof r.run_id);
    else if (KEY("requestId")) json_str(c, r.request_id, sizeof r.request_id);
    else if (KEY("requestKind")) json_str(c, r.kind, sizeof r.kind);
    else if (KEY("responseMode")) {
      char response[16];
      json_str(c, response, sizeof response);
      r.message_response = !strcmp(response, "message");
    } else if (KEY("options") && json_arr_begin(c)) {
      r.approval_options = 0;
      while (json_arr_next(c)) {
        if (!json_obj_begin(c)) continue;
        JStr k2;
        while (json_obj_next(c, &k2)) {
          if (jstr_eq(k2, "decision")) {
            char decision[24];
            json_str(c, decision, sizeof decision);
            r.approval_options |= approval_decision_bit(decision);
          } else json_skip(c);
        }
      }
    } else if (KEY("fileName") || KEY("pattern") || KEY("toolName") || KEY("toModel") || KEY("label") ||
             KEY("appName"))
      json_str(c, r.name, sizeof r.name);
    else if (KEY("startedAt")) r.started = (json_str(c, ts, sizeof ts), util_parse_iso(ts));
    else if (KEY("completedAt")) r.ended = (json_str(c, ts, sizeof ts), util_parse_iso(ts));
    else if (KEY("updatedAt")) r.updated = (json_str(c, ts, sizeof ts), util_parse_iso(ts));
    else if (KEY("text") || KEY("markdown") || KEY("input") || KEY("message") || KEY("summary") ||
             KEY("prompt") || KEY("reason"))
      take_str(c, &r.text);
    else if (KEY("output") || KEY("explanation") || KEY("result") || KEY("progress") || KEY("detail"))
      take_str(c, &r.detail);
    else if (KEY("streaming")) r.streaming = json_bool(c);
    else if (KEY("outputIndicatesFailure")) r.output_failed = json_bool(c);
    else if (KEY("questionAnswer")) {
      r.answered = !json_is_null(c);
      json_skip(c);
    } else if (KEY("exitCode")) r.exit_code = (int)json_int(c);
    else if (KEY("additions")) r.adds = (int)json_int(c);
    else if (KEY("deletions")) r.dels = (int)json_int(c);
    else if (KEY("changes")) parse_changes(c, &r);
    else if (KEY("results")) parse_results(c, &r);
    else if (KEY("patterns") && json_arr_begin(c)) {
      while (json_arr_next(c)) {
        if (!r.name[0]) json_str(c, r.name, sizeof r.name);
        else json_skip(c);
      }
    } else if (KEY("steps")) parse_steps(c, &r);
    else if (KEY("questions")) parse_questions(c, &r);
    else if (KEY("failure") && json_obj_begin(c)) {
      JStr k2;
      while (json_obj_next(c, &k2)) {
        if (jstr_eq(k2, "message")) take_str(c, &r.text);
        else json_skip(c);
      }
    } else json_skip(c);
  }

  const char *t = r.type;
  const char *body = r.text ? r.text : "";
  ConvItem *it = NULL;
  if (!strcmp(t, "user_message")) {
    it = conv_push(conv, ITEM_USER, clip(r.text, 6000));
    r.text = NULL;
  } else if (!strcmp(t, "assistant_message")) {
    it = conv_push(conv, ITEM_ASSISTANT, clip(r.text, 12000));
    r.text = NULL;
  } else if (!strcmp(t, "proposed_plan")) {
    it = conv_push(conv, ITEM_PLAN, clip(r.text, 8000));
    r.text = NULL;
  } else if (!strcmp(t, "todo_list")) {
    it = conv_push(conv, ITEM_TODO, dupf("%.300s", r.detail ? r.detail : ""));
    if (it) it->detail = r.steps.n ? strndup(r.steps.p, r.steps.n) : NULL;
  } else if (!strcmp(t, "command_execution")) {
    // Rows show the command itself; the title is only a fallback.
    it = conv_push(conv, ITEM_WORK, dupf("%.300s", r.text ? r.text : r.title[0] ? r.title : "Ran a command"));
    if (it) {
      it->work = WORK_COMMAND;
      it->detail = tail_lines(r.detail, 4);
      r.detail = NULL;
    }
  } else if (!strcmp(t, "file_change")) {
    if (r.title[0]) it = conv_push(conv, ITEM_WORK, dupf("%s", r.title));
    else if (r.changes > 1) it = conv_push(conv, ITEM_WORK, dupf("Edited %d files", r.changes));
    else it = conv_push(conv, ITEM_WORK, dupf("Edited %s", r.name));
    if (it) it->work = WORK_EDIT;
  } else if (!strcmp(t, "file_search")) {
    it = conv_push(conv, ITEM_WORK, r.title[0] ? dupf("%s", r.title) : dupf("Searched files %s", r.name));
    if (it) it->work = WORK_SEARCH;
  } else if (!strcmp(t, "web_search")) {
    it = conv_push(conv, ITEM_WORK, r.title[0]  ? dupf("%s", r.title)
                                    : r.name[0] ? dupf("Searched the web for %s", r.name)
                                                : dupf("Searched the web"));
    if (it) it->work = WORK_WEB;
  } else if (!strcmp(t, "dynamic_tool")) {
    it = conv_push(conv, ITEM_WORK, dupf("%s", r.title[0] ? r.title : r.name[0] ? r.name : "Tool call"));
    if (it) it->work = WORK_TOOL;
  } else if (!strcmp(t, "reasoning")) {
    // Providers title live reasoning "Thinking"; like the web, finished reasoning reads "Thought".
    bool live = !strcmp(r.status, "running") || r.streaming;
    bool generic = !r.title[0] || !strcmp(r.title, "Thinking") || !strcmp(r.title, "Thought");
    it = conv_push(conv, ITEM_WORK, dupf("%s", !generic ? r.title : live ? "Thinking" : "Thought"));
    if (it) {
      it->work = WORK_THOUGHT;
      it->detail = clip(r.text, 1500);
      r.text = NULL;
    }
  } else if (!strcmp(t, "subagent")) {
    it = conv_push(conv, ITEM_WORK, dupf("%s", r.title[0] ? r.title : "Subagent"));
    if (it) {
      it->work = WORK_AGENT;
      it->detail = clip(r.text, 600);
      r.text = NULL;
    }
  } else if (!strcmp(t, "approval_request")) {
    const char *kind = !strncmp(r.kind, "command", 7) ? "Run a command"
                       : strstr(r.kind, "file")       ? "Change files"
                       : r.kind[0]                    ? r.kind
                                                      : "Approval";
    it = conv_push(conv, ITEM_APPROVAL, dupf("%s%s%s", kind, r.text ? ": " : "", r.text ? r.text : ""));
    if (it) {
      it->detail = clip(r.text, 1500);
      r.text = NULL;
    }
  } else if (!strcmp(t, "user_input_request")) {
    it = conv_push(conv, ITEM_QUESTION, dupf("%s%.600s", r.answered ? "Answered: " : "", body));
    if (it) {
      it->detail = r.steps.n ? strndup(r.steps.p, r.steps.n) : NULL;
      if (!r.answered && strcmp(r.status, "completed") && strcmp(r.status, "cancelled") &&
          strcmp(r.status, "failed") && strcmp(r.status, "interrupted")) {
        it->questions = r.questions;
        r.questions = NULL;
      }
    }
  } else if (!strcmp(t, "secret_request")) {
    it = conv_push(conv, ITEM_NOTICE, dupf("Asked for a secret: %s", r.name));
  } else if (!strcmp(t, "error")) {
    it = conv_push(conv, ITEM_ERROR, dupf("%.1500s", r.text ? r.text : "Something went wrong"));
  } else if (!strcmp(t, "compaction")) {
    it = conv_push(conv, ITEM_NOTICE, dupf("Compacted the context"));
  } else if (!strcmp(t, "handoff")) {
    it = conv_push(conv, ITEM_NOTICE, r.name[0] ? dupf("Switched to %s", r.name) : dupf("Handed off the thread"));
  } else if (!strcmp(t, "fork")) {
    it = conv_push(conv, ITEM_NOTICE, dupf("Forked from another thread"));
  } else if (!strcmp(t, "thread_created")) {
    it = conv_push(conv, ITEM_NOTICE, dupf("Started a new thread"));
  } else if (!strcmp(t, "system_notice") || !strcmp(t, "run_interrupt_result") || !strcmp(t, "notification")) {
    it = conv_push(conv, ITEM_NOTICE, dupf("%.300s", r.text ? r.text : t));
  }
  if (it) {
    it->approval_options = r.approval_options;
    it->message_response = r.message_response;
    util_strlcpy(it->run_id, r.run_id, sizeof it->run_id);
    util_strlcpy(it->request_id, r.request_id, sizeof it->request_id);
    it->streaming = r.streaming;
    it->running = !strcmp(r.status, "running") || !strcmp(r.status, "pending") || !strcmp(r.status, "waiting");
    it->failed = !strcmp(r.status, "failed") || r.output_failed || r.exit_code > 0;
    it->exit_code = r.exit_code;
    it->additions = r.adds;
    it->deletions = r.dels;
    it->started_at = r.started ? r.started : r.updated;
    it->ended_at = r.ended ? r.ended : r.updated;
  }
  free(r.text);
  free(r.detail);
  free(r.questions);
  jb_free(&r.steps);
  if (it) {
    size_t bytes = strlen(it->text) + 1 + (it->detail ? strlen(it->detail) + 1 : 0);
    if (conv->text_bytes + bytes > MAX_CONV_TEXT) {
      free(it->text);
      free(it->detail);
      free(it->questions);
      conv->count--;
      conv->limited = true;
    } else conv->text_bytes += bytes;
  }
}

void parse_projected_items(JsonCur *c, Conversation *conv) {
  while (json_arr_next(c)) {
    if (!json_obj_begin(c)) continue;
    JStr key;
    while (json_obj_next(c, &key)) {
      if (KEY("item")) parse_turn_item(c, conv);
      else json_skip(c);
    }
  }
}

void parse_bounded_snapshot(JsonCur *c, Conversation *conv) {
  if (!json_obj_begin(c)) return;
  JStr key;
  while (json_obj_next(c, &key)) {
    if (KEY("historyCursor")) json_str(c, conv->history_cursor, sizeof conv->history_cursor);
    else if (KEY("hasMoreHistory")) conv->has_more = json_bool(c);
    else if (KEY("payloadBudgetExceeded")) conv->truncated = json_bool(c);
    else if (KEY("projection")) {
      JsonCur queue_cur = *c;
      free(conv->queue);
      conv->queue = queue_parse_projection(&queue_cur);
      if (!json_obj_begin(c)) continue;
      JStr k2;
      while (json_obj_next(c, &k2)) {
        if (jstr_eq(k2, "visibleTurnItems") && json_arr_begin(c)) {
          parse_projected_items(c, conv);
        } else json_skip(c);
      }
    } else json_skip(c);
  }
}

size_t conversation_text_bytes(const Conversation *conv) { return conv->text_bytes; }

void conversation_clear(Conversation *conv) {
  for (int i = 0; i < conv->count; i++) {
    free(conv->items[i].text);
    free(conv->items[i].detail);
    free(conv->items[i].questions);
  }
  free(conv->items);
  free(conv->queue);
  memset(conv, 0, sizeof *conv);
}

bool thread_is_active(const T3Thread *t) {
  return !strcmp(t->status, "preparing") || !strcmp(t->status, "queued") || !strcmp(t->status, "starting") ||
         !strcmp(t->status, "running") || !strcmp(t->status, "waiting");
}

unsigned approval_decision_bit(const char *decision) {
  if (!strcmp(decision, "accept")) return APPROVAL_ACCEPT;
  if (!strcmp(decision, "acceptForSession")) return APPROVAL_SESSION;
  if (!strcmp(decision, "acceptAlways")) return APPROVAL_ALWAYS;
  if (!strcmp(decision, "decline")) return APPROVAL_DECLINE;
  if (!strcmp(decision, "cancel")) return APPROVAL_CANCEL;
  return 0;
}

int thread_rank(const T3Thread *t) {
  if (thread_needs_user(t)) return 0;
  if (thread_is_active(t)) return 1;
  if (t->pinned_at) return 2;
  if (!t->settled) return 3;
  return 4;
}

bool thread_needs_user(const T3Thread *t) {
  return t && ((t->pending_id[0] && t->pending_blocking) || !strcmp(t->status, "waiting"));
}
