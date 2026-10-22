#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bg.h"
#include "client.h"
#include "http.h"
#include "util.h"

void parse_history_page(JsonCur *cur, Conversation *conv) {
  if (!json_obj_begin(cur)) return;
  JStr key;
  while (json_obj_next(cur, &key)) {
    if (jstr_eq(key, "nextCursor")) json_str(cur, conv->history_cursor, sizeof conv->history_cursor);
    else if (jstr_eq(key, "hasMoreHistory")) conv->has_more = json_bool(cur);
    else if (jstr_eq(key, "items") && json_arr_begin(cur)) parse_projected_items(cur, conv);
    else json_skip(cur);
  }
}

typedef struct {
  Client *client;
  int generation, request;
  NetAddr addr;
  char token[2048];
  char thread_id[ID_CAP];
  char cursor[HISTORY_CURSOR_CAP];
  Conversation page;
  char error[120];
  bool ok;
} HistoryJob;

static void history_work(void *arg) {
  HistoryJob *j = arg;
  char path[HISTORY_CURSOR_CAP * 3 + 200], headers[2300];
  int n = snprintf(path, sizeof path, "/api/orchestration/threads/%s/history?cursor=", j->thread_id);
  static const char hex[] = "0123456789ABCDEF";
  for (const unsigned char *p = (const unsigned char *)j->cursor; *p; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
        *p == '-' || *p == '_' || *p == '.' || *p == '~') path[n++] = (char)*p;
    else { path[n++] = '%'; path[n++] = hex[*p >> 4]; path[n++] = hex[*p & 15]; }
  }
  path[n] = '\0';
  snprintf(headers, sizeof headers, "Authorization: Bearer %s\r\nx-t3-orchestration-protocol: 2\r\n", j->token);
  HttpResp r;
  if (http_request(&j->addr, "GET", path, headers, NULL, 0, &r) != 0) {
    util_strlcpy(j->error, r.err, sizeof j->error);
    return;
  }
  if (r.status == 200) {
    JsonCur cur;
    json_init(&cur, r.body, r.len);
    parse_history_page(&cur, &j->page);
    j->ok = !cur.err;
    if (!j->ok) util_strlcpy(j->error, "Invalid history response", sizeof j->error);
  } else snprintf(j->error, sizeof j->error, "History answered HTTP %d", r.status);
  http_resp_free(&r);
}

static void history_done(void *arg) {
  HistoryJob *j = arg;
  Client *c = j->client;
  if (j->generation != c->generation || j->request != c->history_request ||
      strcmp(j->thread_id, c->conv.thread_id)) goto done;
  c->history_state = j->ok ? LOAD_DONE : LOAD_FAILED;
  if (!j->ok) { client_toast(c, j->error); goto done; }
  Conversation *page = &j->page, *conv = &c->conv;
  if (page->limited || conv->count + page->count > MAX_CONV_ITEMS ||
      conversation_text_bytes(conv) + conversation_text_bytes(page) > MAX_CONV_TEXT) {
    conv->limited = true;
    c->conv_version++;
    client_toast(c, "History memory limit reached");
    goto done;
  }
  int total = conv->count + page->count;
  ConvItem *items = total ? malloc(sizeof *items * (size_t)total) : NULL;
  if (total && !items) {
    c->history_state = LOAD_FAILED;
    client_toast(c, "Not enough memory for history");
    goto done;
  }
  if (page->count) memcpy(items, page->items, sizeof *items * (size_t)page->count);
  if (conv->count) memcpy(items + page->count, conv->items, sizeof *items * (size_t)conv->count);
  free(conv->items);
  conv->items = items;
  conv->count = total;
  conv->text_bytes += page->text_bytes;
  c->history_prepended = page->count;
  page->count = 0;
  conv->has_more = page->has_more && page->history_cursor[0] && strcmp(j->cursor, page->history_cursor);
  util_strlcpy(conv->history_cursor, page->history_cursor, sizeof conv->history_cursor);
  c->history_loaded = true;
  c->conv_version++;
  if (!c->history_prepended) client_toast(c, conv->has_more ? "No visible items in this page; load earlier again" : "Start of history");
done:
  conversation_clear(&j->page);
  free(j);
}

void client_load_history(Client *c) {
  if (c->state != CONN_READY || c->conv_state == LOAD_BUSY || c->history_state == LOAD_BUSY ||
      !c->conv.has_more || !c->conv.history_cursor[0] || c->conv.limited) return;
  HistoryJob *j = calloc(1, sizeof *j);
  if (!j) { client_toast(c, "Not enough memory for history"); return; }
  j->client = c;
  j->generation = c->generation;
  j->request = ++c->history_request;
  j->addr = c->addr;
  util_strlcpy(j->token, c->env.token, sizeof j->token);
  util_strlcpy(j->thread_id, c->conv.thread_id, sizeof j->thread_id);
  util_strlcpy(j->cursor, c->conv.history_cursor, sizeof j->cursor);
  c->history_state = LOAD_BUSY;
  bg_submit(history_work, history_done, j);
}
