#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/model.h"

static void command_fields(const RunQueue *queue, int index, QueueAction action, const char *active,
                           const char *text, const char *expected) {
  JBuf b;
  jb_init(&b);
  jb_raw(&b, "{");
  assert(queue_command_fields(&b, queue, index, action, active, text));
  jb_raw(&b, "}");
  assert(!strcmp(b.p, expected));
  printf("%s %s\n", queue_command_type(action), b.p);
  jb_free(&b);
}

int main(void) {
  const char *snapshot =
    "{\"hasMoreHistory\":false,\"projection\":{"
    "\"messages\":["
    "{\"text\":\"Second \\\"message\\\"\",\"id\":\"m2\"},"
    "{\"id\":\"m1\",\"text\":\"First message\"},"
    "{\"id\":\"ma\",\"text\":\"Automatic completion\",\"notification\":{\"source\":{\"kind\":\"subagent\"}}},"
    "{\"id\":\"m3\",\"text\":\"Third message\"}],"
    "\"runs\":["
    "{\"id\":\"r3\",\"status\":\"queued\",\"userMessageId\":\"m3\",\"ordinal\":3,\"queuePosition\":null},"
    "{\"id\":\"r2\",\"status\":\"queued\",\"userMessageId\":\"m2\",\"ordinal\":6,\"queuePosition\":2},"
    "{\"id\":\"active\",\"status\":\"running\",\"userMessageId\":\"mx\",\"ordinal\":4},"
    "{\"id\":\"ra\",\"status\":\"queued\",\"userMessageId\":\"ma\",\"ordinal\":8,\"queuePosition\":4},"
    "{\"id\":\"r1\",\"status\":\"queued\",\"userMessageId\":\"m1\",\"ordinal\":5,\"queuePosition\":1}],"
    "\"visibleTurnItems\":[{\"item\":{\"type\":\"user_input_request\",\"requestId\":\"req\",\"status\":\"pending\","
    "\"questions\":["
    "{\"id\":\"single\",\"header\":\"Pick\",\"question\":\"Which one?\",\"options\":["
    "{\"label\":\"Visible label\",\"description\":\"Details\",\"value\":\" native-id \"}],\"allowCustomAnswer\":false},"
    "{\"id\":\"multi\",\"header\":\"Many\",\"question\":\"Which ones?\",\"multiSelect\":true,\"options\":["
    "{\"label\":\"Alpha\",\"description\":\"First\"},{\"label\":\"Beta\",\"description\":\"Second\",\"value\":\"\"}]},"
    "{\"id\":\"text\",\"header\":\"Text\",\"question\":\"Anything else?\",\"options\":[]},"
    "{\"id\":\"initial\",\"header\":\"Initial\",\"question\":\"Edit?\",\"options\":[],\"initialAnswer\":\"  keep whitespace  \"},"
    "{\"id\":\"empty\",\"header\":\"Empty\",\"question\":\"Edit?\",\"options\":[],\"multiSelect\":true,\"initialAnswer\":\"\"}"
    "]}},{\"item\":{\"type\":\"user_input_request\",\"requestId\":\"old\",\"status\":\"completed\","
    "\"questions\":[{\"id\":\"old\",\"header\":\"Old\",\"question\":\"Done?\",\"options\":[]}],\"questionAnswer\":{\"answers\":{\"old\":\"Done\"}}}}]}}";
  JsonCur cur;
  Conversation conv = {0};
  json_init(&cur, snapshot, strlen(snapshot));
  parse_bounded_snapshot(&cur, &conv);
  assert(!cur.err && conv.count == 2);
  const QuestionSet *set = conv.items[0].questions;
  assert(set && !set->limited && set->count == 5);
  assert(!conv.items[1].questions);
  assert(!strcmp(set->questions[0].options[0].description, "Details"));
  assert(!set->questions[0].allow_custom && set->questions[2].allow_custom);
  QuestionDraft drafts[QUESTION_MAX];
  questions_init_drafts(set, drafts);
  JBuf b;
  jb_init(&b);
  assert(!questions_build_answers(&b, set, drafts));
  assert(b.n == 0);
  question_toggle(&set->questions[0], &drafts[0], 0);
  question_toggle(&set->questions[1], &drafts[1], 0);
  question_toggle(&set->questions[1], &drafts[1], 1);
  question_toggle(&set->questions[1], &drafts[1], 0);
  assert(drafts[1].selected == 2);
  question_toggle(&set->questions[1], &drafts[1], 0);
  strcpy(drafts[2].custom, "  typed answer\n  ");
  assert(questions_build_answers(&b, set, drafts));
  assert(!strcmp(b.p, "{\"single\":\" native-id \",\"multi\":[\"Alpha\",\"\"],\"text\":\"typed answer\",\"initial\":\"  keep whitespace  \",\"empty\":\"\"}"));
  printf("answers %s\n", b.p);
  jb_free(&b);
  strcpy(drafts[1].custom, "  custom multi  ");
  jb_init(&b);
  assert(questions_build_answers(&b, set, drafts));
  assert(strstr(b.p, "\"multi\":\"custom multi\""));
  jb_free(&b);

  const RunQueue *queue = conv.queue;
  assert(queue && queue->count == 3 && !queue->limited);
  assert(!strcmp(queue->items[0].id, "r1") && !strcmp(queue->items[1].id, "r2"));
  assert(!strcmp(queue->items[1].text, "Second \"message\"") && queue->items[1].text_complete);
  for (int i = 0; i < queue->count; i++)
    printf("queued %d %s %s\n", queue->items[i].position, queue->items[i].id, queue->items[i].text);
  command_fields(queue, 1, QUEUE_CANCEL, "active", NULL, "{\"runId\":\"r2\"}");
  command_fields(queue, 1, QUEUE_UP, "active", NULL, "{\"runId\":\"r2\",\"beforeRunId\":\"r1\"}");
  command_fields(queue, 0, QUEUE_DOWN, "active", NULL, "{\"runId\":\"r1\",\"beforeRunId\":\"r3\"}");
  command_fields(queue, 1, QUEUE_DOWN, "active", NULL, "{\"runId\":\"r2\",\"beforeRunId\":null}");
  command_fields(queue, 0, QUEUE_STEER, "active", NULL, "{\"queuedRunId\":\"r1\",\"targetRunId\":\"active\"}");
  command_fields(queue, 0, QUEUE_EDIT, "active", "edited\ntext", "{\"runId\":\"r1\",\"text\":\"edited\\ntext\"}");
  jb_init(&b);
  assert(!queue_command_fields(&b, queue, 0, QUEUE_UP, "active", NULL));
  assert(!queue_command_fields(&b, queue, 2, QUEUE_DOWN, "active", NULL));
  assert(!queue_command_fields(&b, queue, 0, QUEUE_STEER, "", NULL));
  jb_free(&b);
  conversation_clear(&conv);
  assert(!conv.items && !conv.queue);

  jb_init(&b);
  jb_raw(&b, "[");
  for (int i = 0; i <= QUESTION_MAX; i++) {
    if (i) jb_raw(&b, ",");
    jb_raw(&b, "{\"id\":");
    char id[24];
    snprintf(id, sizeof id, "q%d", i);
    jb_str(&b, id);
    jb_raw(&b, ",\"question\":\"Question\",\"options\":[]}");
  }
  jb_raw(&b, "]");
  json_init(&cur, b.p, b.n);
  QuestionSet *limited = questions_parse(&cur);
  assert(!cur.err && limited->count == QUESTION_MAX && limited->limited);
  JBuf answers;
  jb_init(&answers);
  assert(!questions_build_answers(&answers, limited, drafts));
  jb_free(&answers);
  free(limited);
  jb_free(&b);

  jb_init(&b);
  jb_raw(&b, "{\"messages\":[{\"id\":\"long\",\"text\":\"");
  for (int i = 0; i < QUEUE_TEXT_CAP + 10; i++) jb_raw(&b, "x");
  jb_raw(&b, "\"}],\"runs\":[{\"id\":\"r\",\"status\":\"queued\",\"userMessageId\":\"long\",\"ordinal\":1}]}");
  json_init(&cur, b.p, b.n);
  RunQueue *long_queue = queue_parse_projection(&cur);
  assert(!cur.err && long_queue->count == 1 && !long_queue->items[0].text_complete);
  assert(strlen(long_queue->items[0].text) == QUEUE_TEXT_CAP - 1);
  jb_init(&answers);
  assert(!queue_command_fields(&answers, long_queue, 0, QUEUE_EDIT, "active", "replacement"));
  jb_free(&answers);
  free(long_queue);
  jb_free(&b);
  jb_init(&b);
  jb_raw(&b, "{\"runs\":[");
  for (int i = QUEUE_MAX + 2; i > 0; i--) {
    if (i != QUEUE_MAX + 2) jb_raw(&b, ",");
    jb_raw(&b, "{");
    char id[24];
    snprintf(id, sizeof id, "r%d", i);
    jb_kv_str(&b, "id", id);
    jb_kv_str(&b, "status", "queued");
    jb_key(&b, "ordinal");
    jb_int(&b, i);
    jb_raw(&b, "}");
  }
  jb_raw(&b, "]}");
  json_init(&cur, b.p, b.n);
  RunQueue *bounded = queue_parse_projection(&cur);
  assert(!cur.err && bounded->count == QUEUE_MAX && bounded->limited);
  assert(!strcmp(bounded->items[0].id, "r1") && !strcmp(bounded->items[QUEUE_MAX - 1].id, "r32"));
  jb_init(&answers);
  assert(!queue_command_fields(&answers, bounded, QUEUE_MAX - 2, QUEUE_DOWN, "active", NULL));
  jb_free(&answers);
  free(bounded);
  jb_free(&b);
  puts("PASS: snapshot questions, answer values, queue ordering/actions, bounds, and long-text edit guard");
  return 0;
}
