#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../source/snooze.h"
// Shell items and RPC replies are exercised without starting a server.
#include "../source/client.c"

void bg_init(void) {}
void bg_shutdown(void) {}
bool bg_busy(void) { return false; }
void bg_poll(void) {}
void bg_submit(BgFn work, BgFn done, void *arg) { assert(!"Unexpected background job"); }
uint64_t client_ticks_ms(void) { return 1000; }

static void shell(Client *c, const char *json) {
  JsonCur cur;
  json_init(&cur, json, strlen(json));
  apply_shell_item(c, &cur, false);
  assert(!cur.err && cur.p == cur.end);
}

static void test_alerts(void) {
  Client c;
  client_init(&c);
  c.state = CONN_SYNCING;
  const char *snapshot = "{\"kind\":\"snapshot\",\"snapshot\":{\"threads\":[{\"id\":\"t\",\"title\":\"Task\",\"status\":\"waiting\"}]}}";
  const char *running = "{\"kind\":\"thread.updated\",\"location\":\"active\",\"thread\":{\"id\":\"t\",\"title\":\"Task\",\"status\":\"running\"}}";
  const char *waiting = "{\"kind\":\"thread.updated\",\"location\":\"active\",\"thread\":{\"id\":\"t\",\"title\":\"Task\",\"status\":\"waiting\"}}";
  shell(&c, snapshot);
  assert(c.state == CONN_READY && !c.toast[0]);
  shell(&c, waiting);
  assert(!c.toast[0]);
  shell(&c, running);
  shell(&c, waiting);
  assert(!strcmp(c.toast, "Task needs you"));
  c.toast[0] = '\0';
  shell(&c, waiting);
  assert(!c.toast[0]);
  shell(&c, running);
  shell(&c, "{\"kind\":\"thread.updated\",\"location\":\"active\",\"thread\":{\"id\":\"t\",\"title\":\"Task\",\"status\":\"running\",\"pendingRuntimeRequest\":{\"id\":\"q\",\"kind\":\"user_input\",\"blocking\":false}}}");
  assert(!c.toast[0]);
  shell(&c, "{\"kind\":\"thread.updated\",\"location\":\"active\",\"thread\":{\"id\":\"t\",\"title\":\"Task\",\"status\":\"running\",\"pendingRuntimeRequest\":{\"id\":\"q\",\"kind\":\"user_input\",\"blocking\":true}}}");
  assert(!strcmp(c.toast, "Task needs you"));
  c.toast[0] = '\0';
  c.state = CONN_SYNCING;
  shell(&c, snapshot);
  assert(!c.toast[0]);
  shell(&c, running);
  shell(&c, "{\"kind\":\"thread.updated\",\"location\":\"archived\",\"thread\":{\"id\":\"t\",\"status\":\"waiting\"}}");
  assert(!c.toast[0] && c.thread_count == 0);
  client_disconnect(&c);
  free(c.threads);
}

static void config(Client *c, const char *json) {
  JsonCur cur;
  json_init(&cur, json, strlen(json));
  PendingRpc rpc = {.kind = RPC_CONFIG};
  on_success(c, &rpc, &cur);
  assert(!cur.err && cur.p == cur.end);
}

static void test_catalog(void) {
  Client c;
  client_init(&c);
  config(&c, "{\"providers\":[{\"instanceId\":\"off\",\"enabled\":false,\"models\":[{\"slug\":\"hidden\"}]},{\"models\":[{\"slug\":\"custom-model\",\"name\":\"Custom model\"}],\"displayName\":\"My Codex\",\"instanceId\":\"codex_custom\",\"enabled\":true},{\"instanceId\":\"claude\",\"driver\":\"Claude\",\"enabled\":true,\"models\":[{\"slug\":\"sonnet\",\"name\":\"Sonnet\"}]}]}");
  assert(c.provider_count == 2 && c.catalog.provider_count == 2 && c.catalog.model_count == 2);
  assert(!strcmp(c.catalog.providers[0].name, "My Codex"));
  assert(!strcmp(c.catalog.models[1].id, "sonnet") && c.catalog.models[1].provider == 1);
  JBuf selection;
  models_selection(&selection, &c.catalog, 0);
  assert(!strcmp(selection.p, "{\"instanceId\":\"codex_custom\",\"model\":\"custom-model\"}"));
  jb_free(&selection);
  JBuf large;
  jb_init(&large);
  jb_raw(&large, "{\"providers\":[{\"instanceId\":\"codex\",\"enabled\":true,\"models\":[");
  for (int i = 0; i < CATALOG_MODELS + 5; i++) {
    if (i) jb_raw(&large, ",");
    jb_raw(&large, "{\"slug\":\"model\",\"name\":\"Model\"}");
  }
  jb_raw(&large, "]}]}");
  config(&c, large.p);
  assert(c.catalog.provider_count == 1 && c.catalog.model_count == CATALOG_MODELS);
  jb_free(&large);
  jb_init(&large);
  jb_raw(&large, "{\"providers\":[");
  for (int i = 0; i < CATALOG_PROVIDERS + 5; i++) {
    if (i) jb_raw(&large, ",");
    jb_raw(&large, "{\"instanceId\":\"codex\",\"enabled\":true,\"models\":[]}");
  }
  jb_raw(&large, "]}");
  config(&c, large.p);
  assert(c.catalog.provider_count == CATALOG_PROVIDERS && c.catalog.model_count == 0);
  jb_free(&large);
}

static void test_snooze(void) {
  setenv("TZ", "UTC", 1);
  tzset();
  int64_t local = util_parse_iso("2026-10-31T23:30:00.000Z");
  assert(snooze_deadline(0, local, -7200) == local - 7200 + 3600);
  assert(snooze_deadline(1, local, 3600) == local + 3600 + 10800);
  assert(snooze_deadline(2, local, -7200) == util_parse_iso("2026-11-01T07:00:00.000Z"));
  local = util_parse_iso("2028-02-28T00:30:00.000Z");
  assert(snooze_deadline(2, local, 0) == util_parse_iso("2028-02-29T09:00:00.000Z"));
  assert(snooze_deadline(-1, local, 0) == 0);
  T3Thread t = {0};
  const char *json = "{\"id\":\"t\",\"snoozedUntil\":\"2026-11-01T07:00:00.000Z\"}";
  JsonCur cur;
  json_init(&cur, json, strlen(json));
  parse_thread_shell(&cur, &t);
  assert(t.snoozed_until == util_parse_iso("2026-11-01T07:00:00.000Z"));
  json = "{\"id\":\"t\",\"snoozedUntil\":null}";
  json_init(&cur, json, strlen(json));
  parse_thread_shell(&cur, &t);
  assert(!t.snoozed_until);
}

static void test_lineage(void) {
  T3Thread t = {0};
  const char *json = "{\"id\":\"c\",\"lineage\":{\"parentThreadId\":\"p\",\"relationshipToParent\":\"subagent\","
                     "\"rootThreadId\":\"p\"}}";
  JsonCur cur;
  json_init(&cur, json, strlen(json));
  parse_thread_shell(&cur, &t);
  assert(t.subagent && !strcmp(t.parent_id, "p"));
  thread_free(&t);
  T3Thread root = {0};
  json = "{\"id\":\"p\",\"lineage\":{\"parentThreadId\":null,\"relationshipToParent\":null,\"rootThreadId\":\"p\"}}";
  json_init(&cur, json, strlen(json));
  parse_thread_shell(&cur, &root);
  assert(!root.subagent && !root.parent_id[0]);
  thread_free(&root);
}

static char sent[8192];
static void capture(void *ud, char *data, size_t len) {
  assert(len < sizeof sent);
  memcpy(sent, data, len + 1);
}

static JStr field(JStr object, const char *name) {
  JsonCur cur;
  json_init(&cur, object.p, object.n);
  assert(json_obj_begin(&cur));
  JStr key, value = {0};
  while (json_obj_next(&cur, &key)) {
    if (jstr_eq(key, name)) json_raw(&cur, &value);
    else json_skip(&cur);
  }
  assert(!cur.err && value.p);
  return value;
}

static void equal(JStr value, const char *expected) {
  assert(value.n == strlen(expected) && !memcmp(value.p, expected, value.n));
}

static JStr receive(Ws *peer, const char *tag) {
  sent[0] = '\0';
  assert(ws_poll(peer, capture, NULL) && sent[0]);
  JStr request = {sent, strlen(sent)};
  char quoted[100];
  snprintf(quoted, sizeof quoted, "\"%s\"", tag);
  equal(field(request, "tag"), quoted);
  return field(request, "payload");
}

static void test_commands(void) {
  Client c;
  client_init(&c);
  int fds[2];
  assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  c.ws.fd = fds[0];
  c.ws.closed = false;
  Ws peer = {.fd = fds[1]};
  net_set_nonblocking(peer.fd, true);
  T3Thread t = {0};
  util_strlcpy(t.id, "t", sizeof t.id);
  client_snooze(&c, &t, util_parse_iso("2026-11-01T07:00:00.000Z"));
  JStr payload = receive(&peer, "orchestration.dispatchCommand");
  equal(field(payload, "type"), "\"thread.snooze\"");
  equal(field(payload, "snoozedUntil"), "\"2026-11-01T07:00:00.000Z\"");
  client_snooze(&c, &t, 0);
  payload = receive(&peer, "orchestration.dispatchCommand");
  equal(field(payload, "type"), "\"thread.unsnooze\"");
  equal(field(payload, "reason"), "\"user\"");
  util_strlcpy(t.active_run_id, "r", sizeof t.active_run_id);
  client_send_message(&c, &t, "hi");
  payload = receive(&peer, "orchestration.dispatchCommand");
  equal(field(payload, "type"), "\"message.dispatch\"");
  equal(field(payload, "dispatchMode"), "{\"type\":\"queue_after_active\"}");
  assert(!strstr(sent, "deliveryIntent"));
  t.active_run_id[0] = '\0';
  const char *model = "{\"instanceId\":\"custom\",\"model\":\"model-b\"}";
  client_set_model(&c, &t, model);
  payload = receive(&peer, "orchestration.dispatchCommand");
  equal(field(payload, "type"), "\"thread.model-selection.set\"");
  equal(field(payload, "modelSelection"), model);
  PendingRpc *rpc = pending_find(&c, c.next_id);
  assert(rpc);
  JsonCur value;
  json_init(&value, "{}", 2);
  on_success(&c, rpc, &value);
  assert(!strcmp(c.toast, "Model updated"));
  Project p = {0};
  util_strlcpy(p.id, "project", sizeof p.id);
  config(&c, "{\"providers\":[{\"instanceId\":\"custom\",\"enabled\":true,\"models\":[{\"slug\":\"model-b\"}]}]}");
  client_new_thread(&c, &p, "Hello\nWorld");
  payload = receive(&peer, "orchestration.launchThread");
  equal(field(payload, "modelSelection"), model);
  equal(field(payload, "title"), "\"Hello\"");
  equal(field(field(payload, "initialMessage"), "text"), "\"Hello\\nWorld\"");
  util_strlcpy(t.project_id, p.id, sizeof t.project_id);
  util_strlcpy(t.runtime_mode, "read-only", sizeof t.runtime_mode);
  util_strlcpy(t.interaction_mode, "plan", sizeof t.interaction_mode);
  t.model_selection_json = strdup("{\"instanceId\":\"old\",\"model\":\"model-a\",\"options\":[]}");
  t.updated_at = 2;
  upsert_thread(&c, &t);
  client_new_thread(&c, &p, "Reuse");
  payload = receive(&peer, "orchestration.launchThread");
  equal(field(payload, "modelSelection"), t.model_selection_json);
  equal(field(payload, "runtimeMode"), "\"read-only\"");
  equal(field(payload, "interactionMode"), "\"plan\"");
  client_launch_thread(&c, &p, "Choose", model, NULL);
  payload = receive(&peer, "orchestration.launchThread");
  equal(field(payload, "modelSelection"), model);
  equal(field(payload, "interactionMode"), "\"plan\"");
  client_disconnect(&c);
  free(c.threads);
  ws_close(&peer);
}

int main(void) {
  test_alerts();
  test_catalog();
  test_snooze();
  test_lineage();
  test_commands();
  puts("Tier 2a: alerts, catalog limits, snooze clocks, lineage, and command payloads passed");
  return 0;
}

