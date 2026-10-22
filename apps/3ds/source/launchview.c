#include "launchview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "threadops.h"
#include "util.h"

static char project_id[ID_CAP], message[2048], branch[96];
static char *selection;
static bool choosing_base, refs_selected, touching;
static int selected, first_row, touch_x, touch_y;

void launchview_begin(const Project *project, const char *selection_json) {
  util_strlcpy(project_id, project->id, sizeof project_id);
  free(selection);
  selection = selection_json ? strdup(selection_json) : NULL;
  message[0] = branch[0] = '\0';
  choosing_base = refs_selected = touching = false;
  selected = first_row = 0;
}

static bool launch_selected(Client *c, Project *p, LaunchPrompt prompt, bool type_base) {
  if (choosing_base) {
    char base[256];
    const char *ref = c->launch_refs.count ? c->launch_refs.refs[selected].name : "HEAD";
    if (type_base) {
      if (!prompt("Worktree base ref", ref, base, sizeof base, 250, false)) return false;
      ref = base;
    } else if (c->launch_refs_state != LOAD_DONE || !c->launch_refs.count) return false;
    LaunchWorkspace workspace = {branch, ref};
    client_launch_thread(c, p, message, selection, &workspace);
    return true;
  }
  char hint[128];
  snprintf(hint, sizeof hint, "New thread in %s", p->title);
  char text[2048];
  if (!prompt(hint, message[0] ? message : NULL, text, sizeof text, 1500, true)) return false;
  util_strlcpy(message, text, sizeof message);
  if (selected == 0) {
    client_launch_thread(c, p, message, selection, NULL);
    return true;
  }
  char seed[96];
  launch_branch_seed(message, seed, sizeof seed);
  if (branch[0]) util_strlcpy(seed, branch, sizeof seed);
  if (!prompt("Branch for the new worktree", seed, branch, sizeof branch, 90, false)) return false;
  client_load_launch_refs(c, p);
  choosing_base = true;
  refs_selected = false;
  selected = first_row = 0;
  return false;
}

bool launchview_input(Client *c, u32 down, u32 held, const touchPosition *touch, LaunchPrompt prompt) {
  Project *p = client_find_project(c, project_id);
  if (!p) return true;
  if (choosing_base && !refs_selected && c->launch_refs_state == LOAD_DONE) {
    selected = c->launch_refs.preferred;
    refs_selected = true;
  }
  int count = choosing_base ? c->launch_refs.count : 2;
  if (selected >= count) selected = count ? count - 1 : 0;
  if (down & KEY_B) {
    if (!choosing_base) return true;
    choosing_base = false;
    selected = 1;
    first_row = 0;
    c->launch_refs_request_id = 0;
  }
  int step = down & KEY_DUP ? -1 : down & KEY_DDOWN ? 1 : down & KEY_DLEFT ? -5 : down & KEY_DRIGHT ? 5 : 0;
  if (step && count) {
    selected += step;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;
  }
  if (selected < first_row) first_row = selected;
  if (selected >= first_row + 5) first_row = selected - 4;
  if (down & KEY_A) return launch_selected(c, p, prompt, false);
  if (choosing_base && (down & KEY_X)) return launch_selected(c, p, prompt, true);
  if (down & KEY_TOUCH) touching = true, touch_x = touch->px, touch_y = touch->py;
  else if (touching && (held & KEY_TOUCH)) touch_x = touch->px, touch_y = touch->py;
  else if (touching) {
    touching = false;
    if (touch_y >= 206) {
      if (touch_x < 105) return launch_selected(c, p, prompt, false);
      if (touch_x >= 216 && choosing_base) return launch_selected(c, p, prompt, true);
      if (choosing_base) choosing_base = false, selected = 1, first_row = 0;
      else return true;
    } else if (touch_y >= 30 && touch_y < 190) {
      int index = first_row + (touch_y - 30) / 32;
      if (index < count) {
        if (index == selected) return launch_selected(c, p, prompt, false);
        selected = index;
      }
    }
  }
  return false;
}

void launchview_draw_top(const Client *c) {
  gfx_text(14, 36, SCALE_TITLE, COL_TEXT, choosing_base ? "Pick a base branch" : "Choose a workspace", -1);
  const char *note = choosing_base ? "The new branch starts from the local ref you choose. The server creates the worktree and runs project setup."
                                  : "Project folder uses the project's directory. New worktree creates a separate branch and folder.";
  WrapLine lines[6];
  int count = gfx_wrap(note, SCALE_BODY, TOP_W - 28, lines, 6);
  for (int i = 0; i < count; i++) gfx_text(14, 68 + (float)i * 17, SCALE_BODY, COL_MUTED, lines[i].start, lines[i].len);
  if (choosing_base) {
    gfx_text_fit(14, 178, SCALE_SMALL, COL_ACCENT, branch, TOP_W - 28);
    const char *status = c->launch_refs_state == LOAD_BUSY ? "Loading local branches..."
                       : c->launch_refs_state == LOAD_FAILED ? c->launch_refs_error
                       : !c->launch_refs.is_repo ? "This project is not a Git repository."
                       : c->launch_refs.has_more ? "Showing the first 24 branches. X enters another ref." : "X enters another base ref.";
    count = gfx_wrap(status, SCALE_SMALL, TOP_W - 28, lines, 3);
    for (int i = 0; i < count; i++) gfx_text(14, 198 + (float)i * 13, SCALE_SMALL, COL_DIM, lines[i].start, lines[i].len);
  }
}

void launchview_draw_bottom(const Client *c) {
  gfx_rect(0, 0, BOT_W, 24, COL_PANEL);
  gfx_text(10, 4, SCALE_BODY, COL_TEXT, choosing_base ? "Base branch" : "Workspace", -1);
  int count = choosing_base ? c->launch_refs.count : 2;
  for (int row = 0; row < 5 && first_row + row < count; row++) {
    int i = first_row + row;
    float y = 30 + (float)row * 32;
    if (selected == i) gfx_rect(0, y, BOT_W, 31, COL_PANEL_HI);
    const char *label = choosing_base ? c->launch_refs.refs[i].name : i == 0 ? "Project folder" : "New worktree";
    const char *detail = choosing_base ? c->launch_refs.refs[i].current ? "Current branch" : c->launch_refs.refs[i].is_default ? "Default branch" : "Local branch"
                                      : i == 0 ? "Use the project's directory" : "Separate branch and folder";
    gfx_text_fit(10, y + 1, SCALE_BODY, COL_TEXT, label, BOT_W - 20);
    gfx_text(10, y + 17, SCALE_SMALL, COL_DIM, detail, -1);
  }
  gfx_button((Rect){6, 206, 99, 28}, GLYPH_A, "Choose", !choosing_base || count > 0, true);
  gfx_button((Rect){111, 206, 99, 28}, GLYPH_B, "Back", true, false);
  if (choosing_base) gfx_button((Rect){216, 206, 98, 28}, GLYPH_X, "Type ref", true, false);
}
