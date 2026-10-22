#pragma once

#include <stdbool.h>

#include "client.h"

// Lays out the open conversation like the web timeline: your messages as
// bubbles, answers as markdown, and a finished turn's tool calls folded under
// "Worked for ...". The layout is cached as draw operations and rebuilt only
// when the conversation, the thread state, or the fold state changes.

// Rebuilds the layout if needed. Returns true when it changed.
bool convview_sync(Client *c, const T3Thread *t);
float convview_height(void);
// Draws the part of the layout between `scroll` and `scroll + view_h` with its
// top at screen y `top`.
void convview_draw(const T3Thread *t, float top, float view_h, float scroll, int64_t now);

void convview_toggle_folds(void);
bool convview_folds_open(void);
int convview_fold_count(void);

// Your messages, for jumping between turns.
int convview_turn_count(void);
float convview_turn_y(int i);
const char *convview_turn_text(int i);
