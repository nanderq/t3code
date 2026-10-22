#include "threadops.h"

#include <string.h>

#include "util.h"

enum { RPC_FORK = 100, RPC_LAUNCH_REFS, RPC_DIFF };

void threadops_reset(Client *c) {
  diff_clear(&c->diff);
  c->diff_state = c->launch_refs_state = LOAD_NONE;
  c->diff_request_id = c->launch_refs_request_id = 0;
  c->diff_error[0] = c->launch_refs_error[0] = '\0';
  c->launched_thread_id[0] = '\0';
  memset(&c->launch_refs, 0, sizeof c->launch_refs);
}

void threadops_connection_lost(Client *c) {
  if (c->diff_state == LOAD_BUSY) {
    c->diff_state = LOAD_FAILED;
    util_strlcpy(c->diff_error, "Connection lost. Open Diff again to retry.", sizeof c->diff_error);
  }
  if (c->launch_refs_state == LOAD_BUSY) {
    c->launch_refs_state = LOAD_FAILED;
    util_strlcpy(c->launch_refs_error, "Connection lost. Go back and choose New worktree again.", sizeof c->launch_refs_error);
  }
  c->diff_request_id = c->launch_refs_request_id = 0;
}

bool threadops_success(Client *c, const PendingRpc *rpc, JsonCur *value) {
  switch (rpc->kind) {
    case RPC_FORK:
      client_toast(c, "Thread forked");
      util_strlcpy(c->launched_thread_id, rpc->label, sizeof c->launched_thread_id);
      return true;
    case RPC_LAUNCH_REFS:
      if (rpc->id != c->launch_refs_request_id) return true;
      launch_parse_refs(value, &c->launch_refs);
      c->launch_refs_state = value->err ? LOAD_FAILED : LOAD_DONE;
      if (value->err) util_strlcpy(c->launch_refs_error, "Invalid branch list", sizeof c->launch_refs_error);
      return true;
    case RPC_DIFF:
      if (rpc->id != c->diff_request_id) return true;
      c->diff_state = diff_parse_preview(value, &c->diff) ? LOAD_DONE : LOAD_FAILED;
      if (c->diff_state == LOAD_FAILED) util_strlcpy(c->diff_error, "Invalid diff preview", sizeof c->diff_error);
      return true;
    default: return false;
  }
}

bool threadops_failure(Client *c, const PendingRpc *rpc, const char *message) {
  switch (rpc->kind) {
    case RPC_LAUNCH_REFS:
      if (rpc->id != c->launch_refs_request_id) return true;
      c->launch_refs_state = LOAD_FAILED;
      util_strlcpy(c->launch_refs_error, message, sizeof c->launch_refs_error);
      return true;
    case RPC_DIFF:
      if (rpc->id != c->diff_request_id) return true;
      c->diff_state = LOAD_FAILED;
      util_strlcpy(c->diff_error, message, sizeof c->diff_error);
      return true;
    default: return false;
  }
}

void client_fork_thread(Client *c, const T3Thread *t, const char *title) {
  char command_id[37], target_id[37];
  util_uuid(command_id);
  util_uuid(target_id);
  JBuf b;
  jb_init(&b);
  jb_raw(&b, "{\"type\":\"thread.fork\"");
  jb_kv_str(&b, "commandId", command_id);
  jb_kv_str(&b, "sourceThreadId", t->id);
  jb_kv_str(&b, "targetThreadId", target_id);
  jb_kv_str(&b, "createdBy", "user");
  jb_kv_str(&b, "creationSource", "mobile");
  if (title && title[0]) jb_kv_str(&b, "title", title);
  jb_raw(&b, ",\"sourcePoint\":{\"type\":\"latest_stable\"}}");
  // dispatchCommand returns a sequence, so keep our target ID until the shell arrives.
  client_rpc_send(c, "orchestration.dispatchCommand", b.p, RPC_FORK, target_id);
  jb_free(&b);
}

void client_load_launch_refs(Client *c, const Project *p) {
  memset(&c->launch_refs, 0, sizeof c->launch_refs);
  c->launch_refs_error[0] = '\0';
  c->launch_refs_state = LOAD_BUSY;
  JBuf b;
  jb_init(&b);
  jb_raw(&b, "{");
  jb_kv_str(&b, "cwd", p->root);
  jb_raw(&b, ",\"refKind\":\"local\",\"limit\":24}");
  c->launch_refs_request_id = client_rpc_send(c, "vcs.listRefs", b.p, RPC_LAUNCH_REFS, NULL);
  if (c->launch_refs_request_id < 0) {
    c->launch_refs_state = LOAD_FAILED;
    util_strlcpy(c->launch_refs_error, "Could not request branches", sizeof c->launch_refs_error);
  }
  jb_free(&b);
}

void client_close_diff(Client *c) {
  diff_clear(&c->diff);
  c->diff_state = LOAD_NONE;
  c->diff_request_id = 0;
}

void client_load_diff(Client *c, const T3Thread *t) {
  client_close_diff(c);
  c->diff_error[0] = '\0';
  Project *p = client_find_project(c, t->project_id);
  const char *cwd = t->worktree_path[0] ? t->worktree_path : p ? p->root : "";
  if (!cwd[0]) {
    c->diff_state = LOAD_FAILED;
    util_strlcpy(c->diff_error, "Thread workspace is unavailable", sizeof c->diff_error);
    return;
  }
  JBuf b;
  jb_init(&b);
  jb_raw(&b, "{");
  jb_kv_str(&b, "cwd", cwd);
  jb_raw(&b, ",\"ignoreWhitespace\":false}");
  c->diff_state = LOAD_BUSY;
  c->diff_request_id = client_rpc_send(c, "review.getDiffPreview", b.p, RPC_DIFF, NULL);
  if (c->diff_request_id < 0) {
    c->diff_state = LOAD_FAILED;
    util_strlcpy(c->diff_error, "Could not request changes", sizeof c->diff_error);
  }
  jb_free(&b);
}
