#include "threadpanels.h"

#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "util.h"

static enum { PANEL_NONE, PANEL_QUESTIONS, PANEL_QUEUE } panel;
static QuestionSet form;
static QuestionDraft drafts[QUESTION_MAX];
static char request_id[128];
static int question_index, option_index;
static int queue_index, queue_action;
static float text_scroll;

static const QuestionSet *pending_questions(const Client *c, const T3Thread *t) {
  if (!t || !t->pending_id[0] || strcmp(t->pending_kind, "user_input")) return NULL;
  for (int i = c->conv.count - 1; i >= 0; i--) {
    const ConvItem *it = &c->conv.items[i];
    if (it->kind == ITEM_QUESTION && !strcmp(it->request_id, t->pending_id)) return it->questions;
  }
  return NULL;
}

bool threadpanels_active(void) { return panel != PANEL_NONE; }

bool threadpanels_can_answer(const Client *c, const T3Thread *t) {
  const QuestionSet *set = pending_questions(c, t);
  return set && set->count && !set->limited;
}

void threadpanels_answer(Client *c, const T3Thread *t) {
  const QuestionSet *set = pending_questions(c, t);
  if (!set || !set->count || set->limited) {
    client_toast(c, "Open this question on another device");
    return;
  }
  form = *set;
  questions_init_drafts(&form, drafts);
  util_strlcpy(request_id, t->pending_id, sizeof request_id);
  question_index = option_index = 0;
  text_scroll = 0;
  panel = PANEL_QUESTIONS;
}

void threadpanels_queue(void) {
  queue_index = queue_action = 0;
  text_scroll = 0;
  panel = PANEL_QUEUE;
}

static Rect form_button(int i) { return (Rect){4 + i * 79, 208, 75, 26}; }
static Rect queue_button(int i) { return (Rect){4 + i * 63, 172, 60, 26}; }

static void move_question(int delta) {
  int next = question_index + delta;
  if (next < 0 || next >= form.count) return;
  question_index = next;
  option_index = 0;
  text_scroll = 0;
}

static void advance_question(Client *c, const T3Thread *t) {
  if (question_index + 1 < form.count) {
    move_question(1);
    return;
  }
  for (int i = 0; i < form.count; i++) {
    if (!question_answered(&form.questions[i], &drafts[i])) {
      question_index = i;
      option_index = 0;
      text_scroll = 0;
      client_toast(c, "Answer every question first");
      return;
    }
  }
  if (c->state != CONN_READY) {
    client_toast(c, "Reconnect before submitting");
    return;
  }
  client_answer_questions(c, t, request_id, &form, drafts);
  panel = PANEL_NONE;
}

static int option_first(const Question *q) {
  int rows = q->option_count + (q->allow_custom ? 1 : 0);
  int first = option_index - 2;
  if (first > rows - 4) first = rows - 4;
  return first > 0 ? first : 0;
}

static void select_option(PanelPrompt prompt) {
  const Question *q = &form.questions[question_index];
  QuestionDraft *draft = &drafts[question_index];
  if (option_index < q->option_count) question_toggle(q, draft, option_index);
  else if (q->allow_custom) {
    char text[QUESTION_TEXT_CAP];
    if (prompt("Your answer", draft->custom, text, sizeof text, QUESTION_TEXT_CAP - 1, true)) {
      util_strlcpy(draft->custom, text, sizeof draft->custom);
      draft->selected = 0;
    }
  }
}

static bool queue_enabled(const Client *c, const T3Thread *t, QueueAction action) {
  const RunQueue *queue = c->conv.queue;
  if (!t || c->state != CONN_READY || c->conv_stale || c->conv_state == LOAD_BUSY ||
      !queue || queue_index < 0 || queue_index >= queue->count) return false;
  for (int i = 0; i < MAX_PENDING; i++)
    for (int j = QUEUE_CANCEL; j <= QUEUE_EDIT; j++)
      if (c->pending[i].id && !strcmp(c->pending[i].label, queue_done_label((QueueAction)j))) return false;
  if (action == QUEUE_UP) return queue_index > 0;
  if (action == QUEUE_DOWN) return queue_index + 1 < queue->count && !(queue->limited && queue_index + 2 == queue->count);
  if (action == QUEUE_EDIT) return queue->items[queue_index].text_complete;
  if (action == QUEUE_STEER) return t->active_run_id[0] && !strcmp(t->status, "running") && !t->pending_id[0];
  return true;
}

static void execute_queue(Client *c, const T3Thread *t, PanelPrompt prompt) {
  QueueAction action = (QueueAction)queue_action;
  if (!queue_enabled(c, t, action)) {
    client_toast(c, "Queue action unavailable");
    return;
  }
  char text[QUEUE_TEXT_CAP];
  if (action == QUEUE_EDIT &&
      !prompt("Edit queued message", c->conv.queue->items[queue_index].text, text, sizeof text, QUEUE_TEXT_CAP - 1, true)) return;
  client_queue_action(c, t, queue_index, action, action == QUEUE_EDIT ? text : NULL);
}

void threadpanels_input(Client *c, const T3Thread *t, u32 down, const touchPosition *touch, PanelPrompt prompt) {
  if (!t || (down & KEY_B)) {
    panel = PANEL_NONE;
    return;
  }
  if (panel == PANEL_QUESTIONS && strcmp(request_id, t->pending_id)) {
    panel = PANEL_NONE;
    client_toast(c, "That question is no longer pending");
    return;
  }
  circlePosition cp;
  hidCircleRead(&cp);
  if (cp.dy > 20 || cp.dy < -20) text_scroll -= (float)cp.dy / 16.0f;
  if (text_scroll < 0) text_scroll = 0;
  if (panel == PANEL_QUESTIONS) {
    const Question *q = &form.questions[question_index];
    int rows = q->option_count + (q->allow_custom ? 1 : 0);
    if ((down & KEY_DUP) && option_index > 0) option_index--;
    if ((down & KEY_DDOWN) && option_index + 1 < rows) option_index++;
    if (down & KEY_L) move_question(-1);
    else if (down & KEY_R) advance_question(c, t);
    else if (down & KEY_A) select_option(prompt);
    else if ((down & KEY_X) && q->allow_custom) {
      option_index = q->option_count;
      select_option(prompt);
    } else if (down & KEY_TOUCH) {
      if (rect_hit(form_button(0), touch->px, touch->py)) move_question(-1);
      else if (rect_hit(form_button(1), touch->px, touch->py)) select_option(prompt);
      else if (rect_hit(form_button(2), touch->px, touch->py)) advance_question(c, t);
      else if (rect_hit(form_button(3), touch->px, touch->py)) panel = PANEL_NONE;
      else if (touch->py >= 40 && touch->py < 184) {
        int row = option_first(q) + (touch->py - 40) / 36;
        if (row < rows) {
          option_index = row;
          select_option(prompt);
        }
      }
    }
  } else if (panel == PANEL_QUEUE) {
    int count = c->conv.queue ? c->conv.queue->count : 0;
    if (queue_index >= count) queue_index = count ? count - 1 : 0;
    if ((down & KEY_DUP) && queue_index > 0) queue_index--, text_scroll = 0;
    if ((down & KEY_DDOWN) && queue_index + 1 < count) queue_index++, text_scroll = 0;
    if ((down & (KEY_DLEFT | KEY_L)) && queue_action > 0) queue_action--;
    if ((down & (KEY_DRIGHT | KEY_R)) && queue_action < QUEUE_EDIT) queue_action++;
    if (down & KEY_A) execute_queue(c, t, prompt);
    if (down & KEY_TOUCH) {
      for (int i = 0; i <= QUEUE_EDIT; i++) {
        if (rect_hit(queue_button(i), touch->px, touch->py)) {
          queue_action = i;
          execute_queue(c, t, prompt);
        }
      }
      if (touch->py >= 32 && touch->py < 160) {
        int first = queue_index > 1 ? queue_index - 1 : 0;
        if (first > count - 4) first = count > 4 ? count - 4 : 0;
        int row = first + (touch->py - 32) / 32;
        if (row < count) queue_index = row, text_scroll = 0;
      }
      if (touch->py >= 208) panel = PANEL_NONE;
    }
  }
}

static float wrapped(float x, float y, float w, const char *text, u32 color, float clip_bottom) {
  WrapLine lines[128];
  int n = gfx_wrap(text, SCALE_BODY, w, lines, 128);
  float h = gfx_line_h(SCALE_BODY);
  for (int i = 0; i < n; i++, y += h)
    if (y >= 26 && y + h <= clip_bottom) gfx_text(x, y, SCALE_BODY, color, lines[i].start, lines[i].len);
  return n * h;
}

void threadpanels_draw_top(const Client *c) {
  const char *title = "Queued message", *text = "The queue is empty.";
  if (panel == PANEL_QUESTIONS) {
    const Question *q = &form.questions[question_index];
    title = q->header;
    text = q->prompt;
  } else if (c->conv.queue && queue_index < c->conv.queue->count) text = c->conv.queue->items[queue_index].text;
  WrapLine lines[128];
  int n = gfx_wrap(text, SCALE_BODY, TOP_W - 24, lines, 128);
  float max = n * gfx_line_h(SCALE_BODY) - 184;
  if (max < 0) max = 0;
  if (text_scroll > max) text_scroll = max;
  wrapped(12, 30 - text_scroll, TOP_W - 24, text, COL_TEXT, 218);
  gfx_rect(0, 0, TOP_W, 24, COL_PANEL);
  gfx_text_fit(12, 4, SCALE_BODY, COL_ATTENTION, title, TOP_W - 24);
  gfx_text(12, 224, SCALE_SMALL, COL_DIM, "Circle Pad: scroll text", -1);
}

void threadpanels_draw_bottom(const Client *c, const T3Thread *t) {
  char line[200];
  if (panel == PANEL_QUESTIONS) {
    const Question *q = &form.questions[question_index];
    const QuestionDraft *draft = &drafts[question_index];
    snprintf(line, sizeof line, "Question %d / %d  %s", question_index + 1, form.count,
             q->multi_select ? "Select one or more" : "Select one");
    gfx_text(8, 8, SCALE_SMALL, COL_TEXT, line, -1);
    gfx_text_fit(8, 24, SCALE_SMALL, COL_MUTED, q->header, BOT_W - 16);
    int rows = q->option_count + (q->allow_custom ? 1 : 0), first = option_first(q);
    for (int i = first; i < rows && i < first + 4; i++) {
      float y = 40 + (i - first) * 36;
      bool custom = i == q->option_count;
      bool selected = custom ? draft->custom[0] != 0 : (draft->selected & (1u << i)) != 0;
      if (i == option_index) gfx_rrect(4, y, BOT_W - 8, 35, 4, COL_PANEL_HI);
      snprintf(line, sizeof line, "%s %s", selected ? "[x]" : "[ ]", custom ? "Other / type answer" : q->options[i].label);
      gfx_text_fit(10, y + 2, SCALE_BODY, selected ? COL_ACCENT : COL_TEXT, line, BOT_W - 20);
      gfx_text_fit(28, y + 20, SCALE_SMALL, COL_MUTED, custom ? draft->custom : q->options[i].description, BOT_W - 40);
    }
    if (rows == 0) gfx_text(8, 48, SCALE_SMALL, COL_MUTED, "No answer choices available.", -1);
    gfx_text(8, 188, SCALE_SMALL, COL_DIM, q->allow_custom ? "D-pad options   X type   L/R questions" : "D-pad options   L/R questions", -1);
    gfx_button(form_button(0), GLYPH_L, "Back", question_index > 0, false);
    gfx_button(form_button(1), GLYPH_A, "Select", rows > 0, false);
    gfx_button(form_button(2), GLYPH_R, question_index + 1 == form.count ? "Submit" : "Next", true, true);
    gfx_button(form_button(3), GLYPH_B, "Cancel", true, false);
  } else {
    const RunQueue *queue = c->conv.queue;
    int count = queue ? queue->count : 0;
    snprintf(line, sizeof line, "Queued (%d%s)", count, queue && queue->limited ? "+" : "");
    gfx_text(8, 8, SCALE_BODY, COL_TEXT, line, -1);
    int first = queue_index > 1 ? queue_index - 1 : 0;
    if (first > count - 4) first = count > 4 ? count - 4 : 0;
    for (int i = first; i < count && i < first + 4; i++) {
      float y = 32 + (i - first) * 32;
      if (i == queue_index) gfx_rrect(4, y, BOT_W - 8, 31, 4, COL_PANEL_HI);
      snprintf(line, sizeof line, "%d. %.180s", queue->items[i].position, queue->items[i].text);
      gfx_text_fit(10, y + 7, SCALE_BODY, i == queue_index ? COL_TEXT : COL_MUTED, line, BOT_W - 20);
    }
    if (!count) gfx_text(8, 48, SCALE_BODY, COL_MUTED, "No queued messages.", -1);
    static const char *labels[] = {"Cancel", "Up", "Down", "Steer", "Edit"};
    for (int i = 0; i <= QUEUE_EDIT; i++)
      gfx_button(queue_button(i), NULL, labels[i], queue_enabled(c, t, (QueueAction)i), queue_action == i);
    gfx_text(8, 202, SCALE_SMALL, COL_DIM, "D-pad: message / action    A: apply", -1);
    gfx_button((Rect){4, 220, BOT_W - 8, 18}, GLYPH_B, "Back", true, false);
  }
}
