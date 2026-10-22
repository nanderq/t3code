#pragma once

#include "client.h"
#include "gfx.h"

typedef enum { DIFF_INPUT_NONE, DIFF_INPUT_BACK, DIFF_INPUT_REFRESH } DiffInput;

void diffview_reset(void);
DiffInput diffview_input(const DiffPreview *diff, u32 down, u32 held, const touchPosition *touch);
void diffview_draw_top(const Client *c);
void diffview_draw_bottom(const Client *c);
