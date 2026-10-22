#include "questions.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static void read_field(JsonCur *c, char *out, size_t cap, bool *limited) {
  JStr raw;
  JsonCur probe = *c;
  if (!json_raw(&probe, &raw)) {
    *c = probe;
    *limited = true;
    return;
  }
  // Escaped input can be larger than the decoded field, but allocation stays bounded.
  if (raw.n > cap * 6) {
    *c = probe;
    *limited = true;
    return;
  }
  char *s = json_str_dup(c);
  if (!s) {
    *limited = true;
    return;
  }
  if (strlen(s) >= cap) *limited = true;
  util_strlcpy(out, s, cap);
  free(s);
}

QuestionSet *questions_parse(JsonCur *c) {
  QuestionSet *set = calloc(1, sizeof *set);
  if (!set) {
    json_skip(c);
    return NULL;
  }
  if (!json_arr_begin(c)) {
    set->limited = true;
    return set;
  }
  while (json_arr_next(c)) {
    if (set->count == QUESTION_MAX) {
      set->limited = true;
      json_skip(c);
      continue;
    }
    Question *q = &set->questions[set->count++];
    q->allow_custom = true;
    if (!json_obj_begin(c)) {
      set->limited = true;
      continue;
    }
    JStr k;
    while (json_obj_next(c, &k)) {
      if (jstr_eq(k, "id")) read_field(c, q->id, sizeof q->id, &set->limited);
      else if (jstr_eq(k, "header")) read_field(c, q->header, sizeof q->header, &set->limited);
      else if (jstr_eq(k, "question")) read_field(c, q->prompt, sizeof q->prompt, &set->limited);
      else if (jstr_eq(k, "multiSelect")) q->multi_select = json_bool(c);
      else if (jstr_eq(k, "allowCustomAnswer")) q->allow_custom = json_bool(c);
      else if (jstr_eq(k, "initialAnswer")) {
        q->has_initial = true;
        read_field(c, q->initial, sizeof q->initial, &set->limited);
      } else if (jstr_eq(k, "options") && json_arr_begin(c)) {
        while (json_arr_next(c)) {
          if (q->option_count == QUESTION_OPTION_MAX) {
            set->limited = true;
            json_skip(c);
            continue;
          }
          QuestionOption *o = &q->options[q->option_count++];
          if (!json_obj_begin(c)) {
            set->limited = true;
            continue;
          }
          JStr k2;
          while (json_obj_next(c, &k2)) {
            if (jstr_eq(k2, "label")) read_field(c, o->label, sizeof o->label, &set->limited);
            else if (jstr_eq(k2, "description")) read_field(c, o->description, sizeof o->description, &set->limited);
            else if (jstr_eq(k2, "value")) {
              o->has_value = true;
              read_field(c, o->value, sizeof o->value, &set->limited);
            } else json_skip(c);
          }
          if (!o->label[0]) set->limited = true;
        }
      } else json_skip(c);
    }
    if (!q->id[0] || !q->prompt[0]) set->limited = true;
    for (int i = 0; i < set->count - 1; i++)
      if (!strcmp(set->questions[i].id, q->id)) set->limited = true;
  }
  if (c->err) set->limited = true;
  return set;
}

const char *question_option_value(const QuestionOption *o) {
  return o->has_value ? o->value : o->label;
}

void questions_init_drafts(const QuestionSet *set, QuestionDraft *drafts) {
  memset(drafts, 0, sizeof(*drafts) * QUESTION_MAX);
  for (int i = 0; i < set->count; i++)
    if (set->questions[i].allow_custom && set->questions[i].has_initial)
      util_strlcpy(drafts[i].custom, set->questions[i].initial, sizeof drafts[i].custom);
}

void question_toggle(const Question *q, QuestionDraft *draft, int option) {
  if (option < 0 || option >= q->option_count) return;
  draft->custom[0] = '\0';
  if (q->multi_select) draft->selected ^= 1u << option;
  else draft->selected = 1u << option;
}

static void custom_range(const Question *q, const QuestionDraft *draft, const char **start, size_t *len) {
  const char *s = q->allow_custom ? draft->custom : "";
  size_t n = strlen(s);
  if (!q->has_initial) {
    while (n && isspace((unsigned char)*s)) s++, n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
  }
  *start = s;
  *len = n;
}

bool question_answered(const Question *q, const QuestionDraft *draft) {
  const char *s;
  size_t n;
  custom_range(q, draft, &s, &n);
  return n > 0 || (draft->selected & ((1u << q->option_count) - 1)) || (q->allow_custom && q->has_initial);
}

bool questions_build_answers(JBuf *b, const QuestionSet *set, const QuestionDraft *drafts) {
  if (!set || set->limited || !set->count) return false;
  for (int i = 0; i < set->count; i++)
    if (!question_answered(&set->questions[i], &drafts[i])) return false;
  jb_raw(b, "{");
  for (int i = 0; i < set->count; i++) {
    const Question *q = &set->questions[i];
    const QuestionDraft *d = &drafts[i];
    const char *s;
    size_t n;
    custom_range(q, d, &s, &n);
    jb_key(b, q->id);
    if (n || !d->selected) jb_strn(b, s, n);
    else {
      if (q->multi_select) jb_raw(b, "[");
      int written = 0;
      for (int j = 0; j < q->option_count; j++) {
        if (!(d->selected & (1u << j))) continue;
        const char *value = question_option_value(&q->options[j]);
        bool duplicate = false;
        for (int k = 0; k < j; k++)
          if ((d->selected & (1u << k)) && !strcmp(value, question_option_value(&q->options[k]))) duplicate = true;
        if (duplicate) continue;
        if (written++) jb_raw(b, ",");
        jb_str(b, value);
        if (!q->multi_select) break;
      }
      if (q->multi_select) jb_raw(b, "]");
    }
  }
  jb_raw(b, "}");
  return true;
}
