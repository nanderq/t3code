#pragma once

#include <3ds.h>

#include "client.h"

typedef bool (*PanelPrompt)(const char *hint, const char *initial, char *out, size_t cap, int max_len, bool multiline);

bool threadpanels_active(void);
bool threadpanels_can_answer(const Client *c, const T3Thread *t);
void threadpanels_answer(Client *c, const T3Thread *t);
void threadpanels_queue(void);
void threadpanels_input(Client *c, const T3Thread *t, u32 down, const touchPosition *touch, PanelPrompt prompt);
void threadpanels_draw_top(const Client *c);
void threadpanels_draw_bottom(const Client *c, const T3Thread *t);
