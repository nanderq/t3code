#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/bg.h"
#include "../source/client.h"
#include "../source/history.h"
#include "../source/http.h"
#include "../source/util.h"

static char sent_tag[64], sent_payload[8192], requested_path[13000];
static int next_id, pr_refreshes;
static const char *http_fixture;
static BgFn job_work, job_done;
static void *job_arg;

int client_rpc_send(Client *c, const char *tag, const char *payload, int kind, const char *done_label) {
  (void)c; (void)kind; (void)done_label;
  util_strlcpy(sent_tag, tag, sizeof sent_tag);
  util_strlcpy(sent_payload, payload, sizeof sent_payload);
  return ++next_id;
}
void client_toast(Client *c, const char *msg) { util_strlcpy(c->toast, msg, sizeof c->toast); }
void client_refresh_prs(Client *c) { (void)c; pr_refreshes++; }
void bg_submit(BgFn work, BgFn done, void *arg) {
  assert(!job_arg);
  job_work = work; job_done = done; job_arg = arg;
}
int http_request(const NetAddr *addr, const char *method, const char *path, const char *headers,
                 const char *body, size_t body_len, HttpResp *out) {
  (void)addr; (void)body; (void)body_len;
  assert(!strcmp(method, "GET"));
  assert(strstr(headers, "x-t3-orchestration-protocol: 2\r\n"));
  util_strlcpy(requested_path, path, sizeof requested_path);
  memset(out, 0, sizeof *out);
  out->status = 200;
  out->body = strdup(http_fixture);
  out->len = strlen(http_fixture);
  return 0;
}
void http_resp_free(HttpResp *r) { free(r->body); }

static JsonCur cursor(const char *text) {
  JsonCur cur;
  json_init(&cur, text, strlen(text));
  return cur;
}
static void field(const char *json, const char *name, char *out, size_t cap) {
  JsonCur cur = cursor(json);
  assert(json_obj_begin(&cur));
  JStr key;
  out[0] = '\0';
  while (json_obj_next(&cur, &key)) {
    if (jstr_eq(key, name)) json_str(&cur, out, cap);
    else json_skip(&cur);
  }
  assert(!cur.err);
}
static void finish_job(void) {
  assert(job_arg);
  job_work(job_arg);
  void *arg = job_arg;
  job_arg = NULL;
  job_done(arg);
}

static void test_history(void) {
  Client c = {0};
  c.state = CONN_READY;
  c.conv_state = LOAD_DONE;
  strcpy(c.conv.thread_id, "thread-1");
  JsonCur cur = cursor("{\"historyCursor\":\"opaque+/=\",\"hasMoreHistory\":true,\"projection\":{\"visibleTurnItems\":[{\"item\":{\"type\":\"user_message\",\"text\":\"latest\"}}]}}");
  parse_bounded_snapshot(&cur, &c.conv);
  assert(!cur.err && c.conv.count == 1 && c.conv.has_more);
  http_fixture = "{\"snapshotSequence\":10,\"nextCursor\":\"older\",\"hasMoreHistory\":true,\"items\":[{\"item\":{\"type\":\"future_item\"}},{\"item\":{\"type\":\"assistant_message\",\"text\":\"earlier\"}}]}";
  client_load_history(&c);
  assert(c.history_state == LOAD_BUSY);
  finish_job();
  assert(!strcmp(requested_path, "/api/orchestration/threads/thread-1/history?cursor=opaque%2B%2F%3D"));
  assert(c.conv.count == 2 && !strcmp(c.conv.items[0].text, "earlier") && !strcmp(c.conv.items[1].text, "latest"));
  assert(c.history_prepended == 1 && c.history_loaded && c.conv.has_more);
  client_load_history(&c);
  c.history_request++;
  finish_job();
  assert(c.conv.count == 2);
  c.history_state = LOAD_DONE;
  c.conv.limited = true;
  client_load_history(&c);
  assert(!job_arg);
  conversation_clear(&c.conv);

  JBuf b;
  jb_init(&b);
  jb_raw(&b, "{\"items\":[");
  for (int i = 0; i < MAX_CONV_ITEMS + 1; i++) {
    if (i) jb_raw(&b, ",");
    jb_raw(&b, "{\"item\":{\"type\":\"user_message\",\"text\":\"bounded\"}}");
  }
  jb_raw(&b, "],\"nextCursor\":\"next\",\"hasMoreHistory\":true}");
  cur = cursor(b.p);
  parse_history_page(&cur, &c.conv);
  assert(!cur.err && c.conv.count == MAX_CONV_ITEMS && c.conv.limited);
  conversation_clear(&c.conv);
  c.conv.items = calloc(1, sizeof *c.conv.items);
  c.conv.items[0].text = strdup("keep this");
  c.conv.count = 1;
  c.conv.has_more = true;
  strcpy(c.conv.thread_id, "thread-1");
  strcpy(c.conv.history_cursor, "cursor");
  http_fixture = b.p;
  client_load_history(&c);
  finish_job();
  assert(c.conv.count == 1 && c.conv.limited && !strcmp(c.conv.items[0].text, "keep this"));
  conversation_clear(&c.conv);
  jb_free(&b);

  char *long_text = malloc(12001);
  memset(long_text, 'a', 12000); long_text[12000] = '\0';
  jb_init(&b);
  jb_raw(&b, "{\"items\":[");
  for (int i = 0; i < 100; i++) {
    if (i) jb_raw(&b, ",");
    jb_raw(&b, "{\"item\":{\"type\":\"assistant_message\",\"text\":");
    jb_str(&b, long_text);
    jb_raw(&b, "}}");
  }
  jb_raw(&b, "]}");
  cur = cursor(b.p);
  parse_history_page(&cur, &c.conv);
  assert(!cur.err && c.conv.limited && conversation_text_bytes(&c.conv) <= MAX_CONV_TEXT);
  conversation_clear(&c.conv);
  jb_free(&b);
  free(long_text);

  cur = cursor("{\"items\":[{\"item\":{\"type\":\"user_message\",\"text\":\"one\"}}],\"nextCursor\":null,\"hasMoreHistory\":false}");
  parse_history_page(&cur, &c.conv);
  assert(!cur.err && !c.conv.history_cursor[0] && !c.conv.has_more);
  conversation_clear(&c.conv);
}

static void test_tasks(void) {
  Client c = {0};
  c.state = CONN_READY;
  client_refresh_tasks(&c);
  assert(!strcmp(sent_tag, "scheduledTasks.list") && !strcmp(sent_payload, "{}"));
  JsonCur cur = cursor("{\"tasks\":[{\"id\":\"task-1\",\"title\":\"Hourly\",\"enabled\":true,\"schedule\":{\"type\":\"interval\",\"everyMs\":3600000},\"lastRunStatus\":\"succeeded\"},{\"id\":\"task-2\",\"title\":\"Weekdays\",\"schedule\":{\"type\":\"fixed_time\",\"timeOfDay\":\"09:30\",\"weekdays\":[1,3]},\"lastRunStatus\":\"never\"},{\"id\":\"task-3\",\"title\":\"Hook\",\"schedule\":{\"type\":\"webhook\",\"signature\":null}}]}");
  tasks_result(&c, RPC_TASKS, c.tasks_request_id, &cur, "");
  assert(!cur.err && c.task_count == 3);
  assert(!strcmp(c.tasks[0].summary, "Every 1h"));
  assert(!strcmp(c.tasks[1].summary, "09:30 Mo We"));
  assert(!strcmp(c.tasks[2].summary, "Webhook"));
  client_task_toggle(&c, &c.tasks[0]);
  assert(!strcmp(sent_tag, "scheduledTasks.setEnabled"));
  assert(strstr(sent_payload, "\"enabled\":false"));
  tasks_failure(&c, RPC_TASK_ACTION, next_id, "denied");
  assert(!c.task_action_busy && !strcmp(c.toast, "denied"));
  client_task_run(&c, &c.tasks[0]);
  assert(!strcmp(sent_tag, "scheduledTasks.runNow") && !strcmp(sent_payload, "{\"id\":\"task-1\"}"));
  cur = cursor("{\"tasks\":[]}");
  tasks_result(&c, RPC_TASKS, c.tasks_request_id - 1, &cur, "");
  assert(c.task_count == 3);
  free(c.tasks);
}

static const char *detail_fixture = "{\"title\":\"Details\",\"body\":\"Description\",\"state\":\"open\",\"isDraft\":false,\"headBranch\":\"feature\",\"baseBranch\":\"main\",\"mergeability\":\"mergeable\",\"capabilities\":{\"actions\":[\"merge\",\"ready\"],\"mergeMethods\":[\"squash\"],\"review\":{\"verdicts\":[\"approve\",\"comment\"]}},\"viewerPermissions\":{\"actions\":[\"merge\",\"ready\"],\"verdicts\":[\"approve\",\"comment\"]},\"mergeCapabilities\":{\"merge\":false,\"squash\":true,\"rebase\":false},\"checks\":[{\"name\":\"CI\",\"status\":\"success\",\"description\":\"Passed\",\"required\":true}]}";

static void test_prs(void) {
  Client c = {0};
  c.state = CONN_READY;
  PullRequest pr = {0};
  strcpy(pr.project_id, "project-1"); strcpy(pr.repo, "org/repo"); strcpy(pr.host, "github.com");
  strcpy(pr.review, "review-required"); pr.number = 7;
  client_open_pr(&c, &pr);
  assert(!strcmp(sent_tag, "pullRequests.detail"));
  char value[128];
  field(sent_payload, "repository", value, sizeof value); assert(!strcmp(value, "org/repo"));
  JsonCur cur = cursor(detail_fixture);
  prdetail_result(&c, RPC_PR_DETAIL, c.pr_detail.request_id, &cur, "");
  assert(!cur.err && c.pr_detail_state == LOAD_DONE);
  assert(c.pr_detail.can_approve && c.pr_detail.can_comment && !prdetail_merge_allowed(&c));
  assert(c.pr_detail.check_count == 1 && c.pr_detail.checks[0].required);
  client_pr_action(&c);
  assert(!strcmp(sent_tag, "pullRequests.detail"));
  client_pr_review(&c, true, "Looks \"good\"\nthanks");
  assert(!strcmp(sent_tag, "pullRequests.submitReview"));
  field(sent_payload, "verdict", value, sizeof value); assert(!strcmp(value, "approve"));
  field(sent_payload, "body", value, sizeof value); assert(!strcmp(value, "Looks \"good\"\nthanks"));
  assert(strstr(sent_payload, "\"comments\":[]"));
  int action_id = c.pr_detail.action_id;
  cur = cursor("null");
  prdetail_result(&c, RPC_PR_ACTION, action_id, &cur, "Approved");
  assert(!strcmp(c.toast, "Approved") && pr_refreshes == 1 && !strcmp(sent_tag, "pullRequests.detail"));
  strcpy(c.pr_detail.pr.review, "approved");
  cur = cursor(detail_fixture);
  prdetail_result(&c, RPC_PR_DETAIL, c.pr_detail.request_id, &cur, "");
  assert(prdetail_merge_allowed(&c));
  client_pr_action(&c);
  assert(!strcmp(sent_tag, "pullRequests.runAction"));
  field(sent_payload, "action", value, sizeof value); assert(!strcmp(value, "merge"));
  field(sent_payload, "mergeMethod", value, sizeof value); assert(!strcmp(value, "squash"));
  prdetail_failure(&c, RPC_PR_ACTION, c.pr_detail.action_id, "blocked");
  assert(!c.pr_detail.action_id);
  cur = cursor("{\"state\":\"open\",\"isDraft\":true,\"capabilities\":{\"actions\":[\"ready\"]},\"viewerPermissions\":{\"actions\":[\"ready\"]}}");
  prdetail_result(&c, RPC_PR_DETAIL, c.pr_detail.request_id, &cur, "");
  assert(c.pr_detail.can_ready && !c.pr_detail.can_merge && !c.pr_detail.can_approve);
  client_pr_action(&c);
  field(sent_payload, "action", value, sizeof value); assert(!strcmp(value, "ready"));
}

int main(void) {
  test_history(); test_tasks(); test_prs();
  puts("Tier 2b offline fixtures passed: history, tasks, PR permissions and payloads");
  return 0;
}
