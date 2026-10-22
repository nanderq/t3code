#include "queue.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

static void read_id(JsonCur *c, char *out, size_t cap) {
  JStr raw;
  JsonCur probe = *c;
  if (!json_raw(&probe, &raw) || raw.n > cap * 6) {
    *c = probe;
    return;
  }
  char *id = json_str_dup(c);
  if (id && strlen(id) < cap) util_strlcpy(out, id, cap);
  free(id);
}

static int compare_runs(const void *a, const void *b) {
  const QueuedRun *left = a, *right = b;
  if (left->position != right->position) return left->position < right->position ? -1 : 1;
  return (left->ordinal > right->ordinal) - (left->ordinal < right->ordinal);
}

static void parse_runs(JsonCur *c, RunQueue *queue) {
  if (!json_arr_begin(c)) return;
  while (json_arr_next(c)) {
    if (!json_obj_begin(c)) continue;
    QueuedRun run = {0};
    char status[24] = "";
    JStr k;
    while (json_obj_next(c, &k)) {
      if (jstr_eq(k, "id")) read_id(c, run.id, sizeof run.id);
      else if (jstr_eq(k, "userMessageId")) read_id(c, run.message_id, sizeof run.message_id);
      else if (jstr_eq(k, "status")) json_str(c, status, sizeof status);
      else if (jstr_eq(k, "ordinal")) run.ordinal = (int)json_int(c);
      else if (jstr_eq(k, "queuePosition")) {
        if (json_is_null(c)) json_skip(c);
        else run.position = (int)json_int(c);
      } else json_skip(c);
    }
    if (strcmp(status, "queued") || !run.id[0]) continue;
    if (!run.position) run.position = run.ordinal;
    util_strlcpy(run.text, "Queued message", sizeof run.text);
    if (queue->count < QUEUE_MAX) queue->items[queue->count++] = run;
    else {
      queue->limited = true;
      if (compare_runs(&run, &queue->items[queue->count - 1]) < 0) queue->items[queue->count - 1] = run;
    }
    qsort(queue->items, (size_t)queue->count, sizeof run, compare_runs);
  }
}

static void parse_messages(JsonCur *c, RunQueue *queue) {
  if (!json_arr_begin(c)) return;
  while (json_arr_next(c)) {
    if (!json_obj_begin(c)) continue;
    char id[128] = "";
    JStr text = {0}, k;
    bool automatic = false;
    while (json_obj_next(c, &k)) {
      if (jstr_eq(k, "id")) read_id(c, id, sizeof id);
      else if (jstr_eq(k, "text")) json_raw(c, &text);
      else if (jstr_eq(k, "delegatedCompletion") || jstr_eq(k, "notification")) {
        automatic = !json_is_null(c) || automatic;
        json_skip(c);
      } else json_skip(c);
    }
    for (int i = 0; i < queue->count; i++) {
      QueuedRun *run = &queue->items[i];
      if (strcmp(id, run->message_id)) continue;
      run->automatic = automatic;
      if (text.p) {
        JsonCur value;
        json_init(&value, text.p, text.n);
        json_str(&value, run->text, sizeof run->text);
        // Never replace a long message with its preview when editing.
        if (text.n <= QUEUE_TEXT_CAP * 6) {
          json_init(&value, text.p, text.n);
          char *full = json_str_dup(&value);
          run->text_complete = full && strlen(full) < sizeof run->text;
          free(full);
        }
      }
    }
  }
}

RunQueue *queue_parse_projection(JsonCur *c) {
  RunQueue *queue = calloc(1, sizeof *queue);
  if (!queue) {
    json_skip(c);
    return NULL;
  }
  JStr runs = {0}, messages = {0}, k;
  if (json_obj_begin(c)) {
    while (json_obj_next(c, &k)) {
      if (jstr_eq(k, "runs")) json_raw(c, &runs);
      else if (jstr_eq(k, "messages")) json_raw(c, &messages);
      else json_skip(c);
    }
  }
  JsonCur value;
  if (runs.p) {
    json_init(&value, runs.p, runs.n);
    parse_runs(&value, queue);
  }
  if (messages.p) {
    json_init(&value, messages.p, messages.n);
    parse_messages(&value, queue);
  }
  int kept = 0;
  for (int i = 0; i < queue->count; i++)
    if (!queue->items[i].automatic) queue->items[kept++] = queue->items[i];
  queue->count = kept;
  return queue;
}

const char *queue_command_type(QueueAction action) {
  switch (action) {
    case QUEUE_CANCEL: return "queued-run.cancel";
    case QUEUE_UP:
    case QUEUE_DOWN: return "queued-run.reorder";
    case QUEUE_STEER: return "queued-message.promote-to-steer";
    case QUEUE_EDIT: return "queued-run.edit";
  }
  return "";
}

const char *queue_done_label(QueueAction action) {
  switch (action) {
    case QUEUE_CANCEL: return "Queued message cancelled";
    case QUEUE_UP:
    case QUEUE_DOWN: return "Queue reordered";
    case QUEUE_STEER: return "Sent as steer";
    case QUEUE_EDIT: return "Queued message updated";
  }
  return "";
}

bool queue_command_fields(JBuf *b, const RunQueue *queue, int index, QueueAction action,
                          const char *active_run_id, const char *text) {
  if (!queue || index < 0 || index >= queue->count) return false;
  const QueuedRun *run = &queue->items[index];
  if (action == QUEUE_UP && index == 0) return false;
  if (action == QUEUE_DOWN && (index + 1 == queue->count || (queue->limited && index + 2 == queue->count))) return false;
  if (action == QUEUE_STEER && (!active_run_id || !active_run_id[0])) return false;
  if (action == QUEUE_EDIT && (!run->text_complete || !text || !text[0])) return false;
  jb_kv_str(b, action == QUEUE_STEER ? "queuedRunId" : "runId", run->id);
  if (action == QUEUE_UP || action == QUEUE_DOWN) {
    int before = action == QUEUE_UP ? index - 1 : index + 2;
    jb_key(b, "beforeRunId");
    if (before < queue->count) jb_str(b, queue->items[before].id);
    else jb_raw(b, "null");
  } else if (action == QUEUE_STEER) jb_kv_str(b, "targetRunId", active_run_id);
  else if (action == QUEUE_EDIT) jb_kv_str(b, "text", text);
  return true;
}
