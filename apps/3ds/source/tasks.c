#include "tasks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "util.h"

static void parse_schedule(JsonCur *cur, char *out, size_t cap) {
  char type[24] = "", time[8] = "", days[32] = "";
  int64_t every = 0;
  if (!json_obj_begin(cur)) return;
  JStr key;
  while (json_obj_next(cur, &key)) {
    if (jstr_eq(key, "type")) json_str(cur, type, sizeof type);
    else if (jstr_eq(key, "everyMs")) every = json_int(cur);
    else if (jstr_eq(key, "timeOfDay")) json_str(cur, time, sizeof time);
    else if (jstr_eq(key, "weekdays") && json_arr_begin(cur)) {
      static const char *names[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
      while (json_arr_next(cur)) {
        int day = (int)json_int(cur);
        size_t n = strlen(days);
        if (day >= 0 && day < 7 && n + 4 < sizeof days)
          snprintf(days + n, sizeof days - n, "%s%s", n ? " " : "", names[day]);
      }
    } else json_skip(cur);
  }
  if (!strcmp(type, "interval")) {
    if (every % 86400000 == 0) snprintf(out, cap, "Every %lldd", (long long)(every / 86400000));
    else if (every % 3600000 == 0) snprintf(out, cap, "Every %lldh", (long long)(every / 3600000));
    else if (every % 60000 == 0) snprintf(out, cap, "Every %lldm", (long long)(every / 60000));
    else snprintf(out, cap, "Every %llds", (long long)(every / 1000));
  } else if (!strcmp(type, "fixed_time")) snprintf(out, cap, "%s %s", time, days[0] ? days : "daily");
  else util_strlcpy(out, !strcmp(type, "webhook") ? "Webhook" : "Unknown schedule", cap);
}

static void parse_task(JsonCur *cur, ScheduledTask *task) {
  memset(task, 0, sizeof *task);
  if (!json_obj_begin(cur)) return;
  JStr key;
  while (json_obj_next(cur, &key)) {
    if (jstr_eq(key, "id")) json_str(cur, task->id, sizeof task->id);
    else if (jstr_eq(key, "title")) json_str(cur, task->title, sizeof task->title);
    else if (jstr_eq(key, "enabled")) task->enabled = json_bool(cur);
    else if (jstr_eq(key, "lastRunStatus")) json_str(cur, task->last_status, sizeof task->last_status);
    else if (jstr_eq(key, "schedule")) parse_schedule(cur, task->summary, sizeof task->summary);
    else json_skip(cur);
  }
}

void client_refresh_tasks(Client *c) {
  if (c->state != CONN_READY || c->tasks_state == LOAD_BUSY) return;
  c->tasks_state = LOAD_BUSY;
  c->tasks_error[0] = '\0';
  c->tasks_request_id = client_rpc_send(c, "scheduledTasks.list", "{}", RPC_TASKS, NULL);
  if (c->tasks_request_id < 0) c->tasks_state = LOAD_FAILED;
}

static void task_action(Client *c, const ScheduledTask *task, bool toggle) {
  if (c->task_action_busy || c->state != CONN_READY) return;
  JBuf b;
  jb_init(&b);
  jb_raw(&b, "{");
  jb_kv_str(&b, "id", task->id);
  if (toggle) jb_raw(&b, task->enabled ? ",\"enabled\":false" : ",\"enabled\":true");
  jb_raw(&b, "}");
  c->task_action_busy = client_rpc_send(c, toggle ? "scheduledTasks.setEnabled" : "scheduledTasks.runNow", b.p,
                                      RPC_TASK_ACTION, toggle ? (task->enabled ? "Task disabled" : "Task enabled") : "Task started") >= 0;
  jb_free(&b);
}

void client_task_run(Client *c, const ScheduledTask *task) { task_action(c, task, false); }
void client_task_toggle(Client *c, const ScheduledTask *task) { task_action(c, task, true); }

void tasks_result(Client *c, int kind, int id, JsonCur *value, const char *done_label) {
  if (kind == RPC_TASK_ACTION) {
    c->task_action_busy = false;
    client_toast(c, done_label);
    c->tasks_state = LOAD_DONE;
    client_refresh_tasks(c);
    return;
  }
  if (id != c->tasks_request_id) return;
  ScheduledTask *tasks = calloc(MAX_TASKS, sizeof *tasks);
  if (!tasks) {
    tasks_failure(c, kind, id, "Not enough memory for tasks");
    return;
  }
  int count = 0, total = 0;
  JStr key;
  if (json_obj_begin(value)) while (json_obj_next(value, &key)) {
    if (jstr_eq(key, "tasks") && json_arr_begin(value)) {
      while (json_arr_next(value)) {
        total++;
        if (count < MAX_TASKS) {
          parse_task(value, &tasks[count]);
          if (tasks[count].id[0]) count++;
        } else json_skip(value);
      }
    } else json_skip(value);
  }
  if (value->err) {
    free(tasks);
    tasks_failure(c, kind, id, "Invalid tasks response");
    return;
  }
  free(c->tasks);
  c->tasks = tasks;
  c->task_count = count;
  c->tasks_state = LOAD_DONE;
  if (total > MAX_TASKS) util_strlcpy(c->tasks_error, "Showing the first 64 tasks", sizeof c->tasks_error);
}

void tasks_failure(Client *c, int kind, int id, const char *msg) {
  if (kind == RPC_TASK_ACTION) {
    c->task_action_busy = false;
    client_toast(c, msg);
  } else if (id == c->tasks_request_id) {
    c->tasks_state = LOAD_FAILED;
    util_strlcpy(c->tasks_error, msg, sizeof c->tasks_error);
  }
}
