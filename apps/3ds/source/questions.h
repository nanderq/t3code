#pragma once

#include "json.h"

#define QUESTION_MAX 8
#define QUESTION_OPTION_MAX 12
#define QUESTION_TEXT_CAP 1536

typedef struct {
  char label[160];
  char description[384];
  char value[256];
  bool has_value;
} QuestionOption;

typedef struct {
  char id[128];
  char header[160];
  char prompt[2048];
  QuestionOption options[QUESTION_OPTION_MAX];
  int option_count;
  bool multi_select;
  bool allow_custom;
  bool has_initial;
  char initial[QUESTION_TEXT_CAP];
} Question;

typedef struct {
  Question questions[QUESTION_MAX];
  int count;
  bool limited;
} QuestionSet;

typedef struct {
  unsigned selected;
  char custom[QUESTION_TEXT_CAP];
} QuestionDraft;

QuestionSet *questions_parse(JsonCur *c);
const char *question_option_value(const QuestionOption *o);
void questions_init_drafts(const QuestionSet *set, QuestionDraft *drafts);
void question_toggle(const Question *q, QuestionDraft *draft, int option);
bool question_answered(const Question *q, const QuestionDraft *draft);
bool questions_build_answers(JBuf *b, const QuestionSet *set, const QuestionDraft *drafts);
