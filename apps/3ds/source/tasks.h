#pragma once

#include <stdbool.h>
#include "json.h"
#include "model.h"

#define MAX_TASKS 64
enum { RPC_TASKS = 100, RPC_TASK_ACTION };

typedef struct {
  char id[ID_CAP];
  char title[128];
  char summary[96];
  char last_status[16];
  bool enabled;
} ScheduledTask;

struct Client;
void client_refresh_tasks(struct Client *c);
void client_task_run(struct Client *c, const ScheduledTask *task);
void client_task_toggle(struct Client *c, const ScheduledTask *task);
void tasks_result(struct Client *c, int kind, int id, JsonCur *value, const char *done_label);
void tasks_failure(struct Client *c, int kind, int id, const char *msg);
