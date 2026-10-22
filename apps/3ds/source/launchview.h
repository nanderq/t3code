#pragma once

#include "client.h"
#include "gfx.h"

typedef bool (*LaunchPrompt)(const char *hint, const char *initial, char *out, size_t cap, int max_len, bool multiline);

// `selection_json` is the chosen ModelSelection, or NULL to reuse the newest thread's.
void launchview_begin(const Project *project, const char *selection_json);
bool launchview_input(Client *c, u32 down, u32 held, const touchPosition *touch, LaunchPrompt prompt);
void launchview_draw_top(const Client *c);
void launchview_draw_bottom(const Client *c);
