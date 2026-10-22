#pragma once

#include "json.h"

#define QUEUE_MAX 32
#define QUEUE_TEXT_CAP 1536

typedef struct {
  char id[128];
  char message_id[128];
  char text[QUEUE_TEXT_CAP];
  int position;
  int ordinal;
  bool text_complete;
  bool automatic;
} QueuedRun;

typedef struct {
  QueuedRun items[QUEUE_MAX];
  int count;
  bool limited;
} RunQueue;

typedef enum { QUEUE_CANCEL, QUEUE_UP, QUEUE_DOWN, QUEUE_STEER, QUEUE_EDIT } QueueAction;

RunQueue *queue_parse_projection(JsonCur *c);
const char *queue_command_type(QueueAction action);
const char *queue_done_label(QueueAction action);
bool queue_command_fields(JBuf *b, const RunQueue *queue, int index, QueueAction action,
                          const char *active_run_id, const char *text);
