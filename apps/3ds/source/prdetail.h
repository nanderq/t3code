#pragma once

#include <stdbool.h>
#include "json.h"
#include "model.h"

#define MAX_PR_CHECKS 48
enum { RPC_PR_DETAIL = 110, RPC_PR_ACTION };

typedef struct {
  char name[96];
  char status[20];
  char description[128];
  bool required;
} PrCheck;

typedef struct {
  PullRequest pr;
  char body[2048];
  PrCheck checks[MAX_PR_CHECKS];
  int check_count, check_total;
  bool can_approve, can_comment, can_merge, can_ready;
  char merge_method[8];
  int request_id, action_id;
} PrDetail;

struct Client;
void client_open_pr(struct Client *c, const PullRequest *pr);
void client_refresh_pr_detail(struct Client *c);
void client_pr_review(struct Client *c, bool approve, const char *body);
void client_pr_action(struct Client *c);
void prdetail_result(struct Client *c, int kind, int id, JsonCur *value, const char *done_label);
void prdetail_failure(struct Client *c, int kind, int id, const char *msg);

bool prdetail_merge_allowed(const struct Client *c);
