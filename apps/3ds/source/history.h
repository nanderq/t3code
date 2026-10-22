#pragma once

#include "model.h"

#define HISTORY_CURSOR_CAP 4097

struct Client;
void client_load_history(struct Client *c);
void parse_history_page(JsonCur *cur, Conversation *conv);
