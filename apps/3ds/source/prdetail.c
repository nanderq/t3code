#include "prdetail.h"

#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "util.h"

typedef struct {
  bool approve, comment, merge, ready;
  bool methods[3];
} PrPermissions;

static const char *merge_methods[] = {"merge", "squash", "rebase"};

static void parse_permissions(JsonCur *cur, PrPermissions *out) {
  if (!json_obj_begin(cur)) return;
  JStr key;
  while (json_obj_next(cur, &key)) {
    if ((jstr_eq(key, "actions") || jstr_eq(key, "verdicts") || jstr_eq(key, "mergeMethods")) && json_arr_begin(cur)) {
      char name[32];
      while (json_arr_next(cur)) {
        json_str(cur, name, sizeof name);
        if (jstr_eq(key, "actions")) {
          if (!strcmp(name, "merge")) out->merge = true;
          if (!strcmp(name, "ready")) out->ready = true;
        } else if (jstr_eq(key, "verdicts")) {
          if (!strcmp(name, "approve")) out->approve = true;
          if (!strcmp(name, "comment")) out->comment = true;
        } else for (int i = 0; i < 3; i++) if (!strcmp(name, merge_methods[i])) out->methods[i] = true;
      }
    } else if (jstr_eq(key, "review")) parse_permissions(cur, out);
    else json_skip(cur);
  }
}

static void parse_detail(JsonCur *cur, PrDetail *d) {
  PrPermissions caps = {0}, permissions = {0};
  bool methods[3] = {0};
  if (!json_obj_begin(cur)) return;
  JStr key;
  while (json_obj_next(cur, &key)) {
    if (jstr_eq(key, "title")) json_str(cur, d->pr.title, sizeof d->pr.title);
    else if (jstr_eq(key, "body")) json_str(cur, d->body, sizeof d->body);
    else if (jstr_eq(key, "headBranch")) json_str(cur, d->pr.head, sizeof d->pr.head);
    else if (jstr_eq(key, "baseBranch")) json_str(cur, d->pr.base, sizeof d->pr.base);
    else if (jstr_eq(key, "state")) json_str(cur, d->pr.state, sizeof d->pr.state);
    else if (jstr_eq(key, "isDraft")) d->pr.draft = json_bool(cur);
    else if (jstr_eq(key, "mergeability")) json_str(cur, d->pr.mergeability, sizeof d->pr.mergeability);
    else if (jstr_eq(key, "capabilities")) parse_permissions(cur, &caps);
    else if (jstr_eq(key, "viewerPermissions")) parse_permissions(cur, &permissions);
    else if (jstr_eq(key, "mergeCapabilities") && json_obj_begin(cur)) {
      JStr method;
      while (json_obj_next(cur, &method)) {
        bool found = false;
        for (int i = 0; i < 3; i++) if (jstr_eq(method, merge_methods[i])) {
          methods[i] = json_bool(cur);
          found = true;
          break;
        }
        if (!found) json_skip(cur);
      }
    } else if (jstr_eq(key, "checks") && json_arr_begin(cur)) {
      d->check_count = d->check_total = 0;
      while (json_arr_next(cur)) {
        d->check_total++;
        if (d->check_count == MAX_PR_CHECKS) { json_skip(cur); continue; }
        PrCheck *check = &d->checks[d->check_count++];
        if (!json_obj_begin(cur)) continue;
        JStr field;
        while (json_obj_next(cur, &field)) {
          if (jstr_eq(field, "name")) json_str(cur, check->name, sizeof check->name);
          else if (jstr_eq(field, "status")) json_str(cur, check->status, sizeof check->status);
          else if (jstr_eq(field, "description")) json_str(cur, check->description, sizeof check->description);
          else if (jstr_eq(field, "required")) check->required = json_bool(cur);
          else json_skip(cur);
        }
      }
    } else json_skip(cur);
  }
  bool open = !strcmp(d->pr.state, "open");
  d->can_approve = open && caps.approve && permissions.approve;
  d->can_comment = caps.comment && permissions.comment;
  d->can_ready = open && d->pr.draft && caps.ready && permissions.ready;
  d->can_merge = open && !d->pr.draft && caps.merge && permissions.merge &&
                 strcmp(d->pr.mergeability, "conflicting");
  d->merge_method[0] = '\0';
  for (int i = 0; i < 3; i++) if (methods[i] && caps.methods[i]) {
    util_strlcpy(d->merge_method, merge_methods[i], sizeof d->merge_method);
    break;
  }
  if (!d->merge_method[0]) d->can_merge = false;
}

static void ref_payload(JBuf *b, const PullRequest *pr) {
  jb_init(b);
  jb_raw(b, "{");
  jb_kv_str(b, "projectId", pr->project_id);
  if (pr->host[0]) jb_kv_str(b, "host", pr->host);
  jb_kv_str(b, "repository", pr->repo);
  jb_key(b, "number");
  jb_int(b, pr->number);
}

void client_refresh_pr_detail(Client *c) {
  if (c->state != CONN_READY || !c->pr_detail.pr.number || c->pr_detail_state == LOAD_BUSY) return;
  JBuf b;
  ref_payload(&b, &c->pr_detail.pr);
  jb_raw(&b, "}");
  c->pr_detail_state = LOAD_BUSY;
  c->pr_detail_error[0] = '\0';
  c->pr_detail.request_id = client_rpc_send(c, "pullRequests.detail", b.p, RPC_PR_DETAIL, NULL);
  if (c->pr_detail.request_id < 0) c->pr_detail_state = LOAD_FAILED;
  jb_free(&b);
}

void client_open_pr(Client *c, const PullRequest *pr) {
  memset(&c->pr_detail, 0, sizeof c->pr_detail);
  c->pr_detail.pr = *pr;
  c->pr_detail_state = LOAD_NONE;
  client_refresh_pr_detail(c);
}

void client_pr_review(Client *c, bool approve, const char *body) {
  PrDetail *d = &c->pr_detail;
  if (c->state != CONN_READY || c->pr_detail_state != LOAD_DONE || d->action_id ||
      !(approve ? d->can_approve : d->can_comment)) return;
  JBuf b;
  ref_payload(&b, &d->pr);
  jb_kv_str(&b, "verdict", approve ? "approve" : "comment");
  jb_kv_str(&b, "body", body);
  jb_raw(&b, ",\"comments\":[]}");
  int id = client_rpc_send(c, "pullRequests.submitReview", b.p, RPC_PR_ACTION, approve ? "Review approved" : "Comment sent");
  d->action_id = id > 0 ? id : 0;
  jb_free(&b);
}

bool prdetail_merge_allowed(const Client *c) {
  const PullRequest *pr = &c->pr_detail.pr;
  for (int i = 0; i < c->pr_count; i++) {
    const PullRequest *row = &c->prs[i];
    if (row->number == pr->number && !strcmp(row->project_id, pr->project_id) &&
        !strcmp(row->repo, pr->repo) && !strcmp(row->host, pr->host)) { pr = row; break; }
  }
  return c->pr_detail.can_merge && strcmp(pr->review, "review-required") && strcmp(pr->review, "changes-requested");
}

void client_pr_action(Client *c) {
  PrDetail *d = &c->pr_detail;
  if (c->state != CONN_READY || c->pr_detail_state != LOAD_DONE || d->action_id || !(d->can_ready || prdetail_merge_allowed(c))) return;
  JBuf b;
  ref_payload(&b, &d->pr);
  jb_kv_str(&b, "action", d->pr.draft ? "ready" : "merge");
  if (!d->pr.draft) jb_kv_str(&b, "mergeMethod", d->merge_method);
  jb_raw(&b, "}");
  int id = client_rpc_send(c, "pullRequests.runAction", b.p, RPC_PR_ACTION, d->pr.draft ? "Marked ready" : "Pull request merged");
  d->action_id = id > 0 ? id : 0;
  jb_free(&b);
}

void prdetail_result(Client *c, int kind, int id, JsonCur *value, const char *done_label) {
  PrDetail *d = &c->pr_detail;
  if (kind == RPC_PR_ACTION) {
    client_toast(c, done_label);
    client_refresh_prs(c);
    if (d->action_id != id) return;
    d->action_id = 0;
    client_refresh_pr_detail(c);
    return;
  }
  if (d->request_id != id) return;
  parse_detail(value, d);
  if (value->err) prdetail_failure(c, kind, id, "Invalid pull request detail");
  else c->pr_detail_state = LOAD_DONE;
}

void prdetail_failure(Client *c, int kind, int id, const char *msg) {
  if (kind == RPC_PR_ACTION) {
    if (c->pr_detail.action_id == id) c->pr_detail.action_id = 0;
    client_toast(c, msg);
  } else if (c->pr_detail.request_id == id) {
    c->pr_detail_state = LOAD_FAILED;
    util_strlcpy(c->pr_detail_error, msg, sizeof c->pr_detail_error);
  }
}
