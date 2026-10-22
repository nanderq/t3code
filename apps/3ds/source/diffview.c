#include "diffview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define FILE_Y 30
#define FILE_H 32
#define FILE_ROWS 5
#define DIFF_ROWS 15
#define DIFF_COLS 74
#define DIFF_SCALE 0.36f

static int selected, first_file;
static float scroll;
static bool in_file;
static struct {
  bool active, dragging;
  int x, y, start_y, first;
} drag;

void diffview_reset(void) {
  selected = first_file = 0;
  scroll = 0;
  in_file = false;
  memset(&drag, 0, sizeof drag);
}

static void select_file(const DiffPreview *diff, int index) {
  if (index < 0) index = 0;
  if (index >= diff->count) index = diff->count ? diff->count - 1 : 0;
  if (selected != index) scroll = 0;
  selected = index;
  if (selected < first_file) first_file = selected;
  if (selected >= first_file + FILE_ROWS) first_file = selected - FILE_ROWS + 1;
}

DiffInput diffview_input(const DiffPreview *diff, u32 down, u32 held, const touchPosition *touch) {
  if (selected >= diff->count) select_file(diff, selected);
  if (down & KEY_B) {
    if (!in_file) return DIFF_INPUT_BACK;
    in_file = false;
  }
  if (down & KEY_Y) return DIFF_INPUT_REFRESH;
  if ((down & KEY_A) && diff->count) in_file = !in_file;
  if (down & KEY_L) select_file(diff, selected - 1);
  if (down & KEY_R) select_file(diff, selected + 1);
  if (!in_file) {
    if (down & KEY_DUP) select_file(diff, selected - 1);
    if (down & KEY_DDOWN) select_file(diff, selected + 1);
    if (down & KEY_DLEFT) select_file(diff, selected - FILE_ROWS);
    if (down & KEY_DRIGHT) select_file(diff, selected + FILE_ROWS);
  } else {
    if (held & KEY_DUP) scroll -= 0.3f;
    if (held & KEY_DDOWN) scroll += 0.3f;
    if (down & KEY_DLEFT) scroll -= DIFF_ROWS - 1;
    if (down & KEY_DRIGHT) scroll += DIFF_ROWS - 1;
  }
  circlePosition cp;
  hidCircleRead(&cp);
  if (cp.dy > 25 || cp.dy < -25) scroll -= (float)cp.dy / 120.0f;
  int max = diff->count ? diff->files[selected].line_count + (diff->files[selected].truncated ? 1 : 0) - DIFF_ROWS : 0;
  if (max < 0) max = 0;
  if (scroll < 0) scroll = 0;
  if (scroll > max) scroll = (float)max;

  if (down & KEY_TOUCH) {
    drag.active = true;
    drag.dragging = false;
    drag.x = touch->px;
    drag.y = drag.start_y = touch->py;
    drag.first = first_file;
  } else if (drag.active && (held & KEY_TOUCH)) {
    drag.x = touch->px;
    drag.y = touch->py;
    if (abs(drag.y - drag.start_y) > 6 && drag.start_y >= FILE_Y && drag.start_y < 200) drag.dragging = true;
    if (drag.dragging) {
      first_file = drag.first + (drag.start_y - drag.y) / FILE_H;
      int max_first = diff->count - FILE_ROWS;
      if (max_first < 0) max_first = 0;
      if (first_file < 0) first_file = 0;
      if (first_file > max_first) first_file = max_first;
    }
  } else if (drag.active) {
    drag.active = false;
    if (!drag.dragging) {
      if (drag.y >= 206) {
        if (drag.x < 105 && diff->count) in_file = !in_file;
        else if (drag.x < 211) {
          if (in_file) in_file = false;
          else return DIFF_INPUT_BACK;
        } else return DIFF_INPUT_REFRESH;
      } else if (drag.y >= FILE_Y && drag.y < FILE_Y + FILE_ROWS * FILE_H) {
        int index = first_file + (drag.y - FILE_Y) / FILE_H;
        if (index < diff->count) {
          in_file = index == selected;
          select_file(diff, index);
        }
      }
    }
  }
  return DIFF_INPUT_NONE;
}

// Fixed cells keep indentation readable without loading another font on Old 3DS.
static void draw_code_line(float y, u32 color, const char *text) {
  int col = 0;
  const char *p = text;
  while (*p && col < DIFF_COLS - 3) {
    if (*p == '\t') {
      col += 4 - col % 4;
      p++;
      continue;
    }
    uint32_t cp;
    size_t n = util_utf8_decode(p, &cp);
    if (!n) break;
    char glyph[5];
    memcpy(glyph, p, n);
    glyph[n] = '\0';
    if (cp < 32 || cp == 127) util_strlcpy(glyph, " ", sizeof glyph);
    gfx_text(8 + (float)col * 5.2f, y, DIFF_SCALE, color, glyph, -1);
    p += n;
    col++;
  }
  if (*p) gfx_text(8 + (float)(DIFF_COLS - 3) * 5.2f, y, DIFF_SCALE, COL_DIM, "...", -1);
}

void diffview_draw_top(const Client *c) {
  if (c->diff_state == LOAD_BUSY) {
    gfx_text(14, 70, SCALE_BODY, COL_MUTED, "Loading changes...", -1);
    return;
  }
  if (c->diff_state == LOAD_FAILED) {
    gfx_text(14, 60, SCALE_BODY, COL_ERROR, "Couldn't load changes", -1);
    WrapLine lines[8];
    int n = gfx_wrap(c->diff_error, SCALE_SMALL, TOP_W - 28, lines, 8);
    for (int i = 0; i < n; i++) gfx_text(14, 84 + (float)i * 14, SCALE_SMALL, COL_MUTED, lines[i].start, lines[i].len);
    return;
  }
  const DiffPreview *diff = &c->diff;
  if (!diff->count) {
    const char *message = !diff->has_source ? "No branch changes available" : diff->truncated ? "Diff omitted by the server" : "No changes";
    gfx_text(14, 70, SCALE_BODY, COL_MUTED, message, -1);
    return;
  }
  const DiffFile *file = &diff->files[selected < diff->count ? selected : 0];
  gfx_text_fit(8, 21, SCALE_SMALL, COL_TEXT, file->path, TOP_W - 16);
  if (!file->text) {
    gfx_text(12, 64, SCALE_BODY, COL_MUTED, "File contents omitted from preview.", -1);
    gfx_text(12, 86, SCALE_SMALL, COL_DIM, "Read this diff on another device.", -1);
  } else {
    for (int row = 0; row < DIFF_ROWS; row++) {
      int index = (int)scroll + row;
      float y = 41 + (float)row * 12;
      if (index >= file->line_count) {
        if (index == file->line_count && file->truncated)
          gfx_text(8, y, DIFF_SCALE, COL_ATTENTION, "Diff too large, see on another device.", -1);
        break;
      }
      const char *line = file->text + file->lines[index];
      u32 color = line[0] == '+' ? COL_OK : line[0] == '-' ? COL_ERROR : line[0] == '@' ? COL_RUNNING : COL_MUTED;
      draw_code_line(y, color, line);
    }
  }
  char footer[100];
  snprintf(footer, sizeof footer, "%d/%d  line %d/%d  %s", selected + 1, diff->count, (int)scroll + 1,
           file->line_count, in_file ? "D-pad scroll" : "A opens file");
  gfx_text_fit(8, 224, SCALE_SMALL, COL_DIM, footer, TOP_W - 16);
}

void diffview_draw_bottom(const Client *c) {
  const DiffPreview *diff = &c->diff;
  gfx_rect(0, 0, BOT_W, 24, COL_PANEL);
  gfx_text(10, 4, SCALE_BODY, COL_TEXT, in_file ? "Changes: reading file" : "Changes: choose a file", -1);
  for (int row = 0; row < FILE_ROWS && first_file + row < diff->count; row++) {
    int index = first_file + row;
    const DiffFile *file = &diff->files[index];
    float y = FILE_Y + (float)row * FILE_H;
    if (index == selected) gfx_rect(0, y, BOT_W, FILE_H - 1, COL_PANEL_HI);
    gfx_text_fit(10, y + 1, SCALE_SMALL, COL_TEXT, file->path, BOT_W - 20);
    char counts[64];
    snprintf(counts, sizeof counts, "+%d  -%d%s", file->additions, file->deletions,
             file->truncated ? "  truncated" : !file->text ? "  preview omitted" : "");
    gfx_text(10, y + 15, SCALE_SMALL, file->truncated ? COL_ATTENTION : COL_MUTED, counts, -1);
  }
  gfx_text_fit(10, 191, SCALE_SMALL, diff->truncated ? COL_ATTENTION : COL_DIM,
               diff->truncated ? "Diff too large, see on another device." : "L/R file   Circle Pad scroll", BOT_W - 20);
  gfx_button((Rect){6, 206, 99, 28}, GLYPH_A, in_file ? "Files" : "Open", diff->count > 0, true);
  gfx_button((Rect){111, 206, 99, 28}, GLYPH_B, "Back", true, false);
  gfx_button((Rect){216, 206, 98, 28}, GLYPH_Y, "Reload", c->diff_state != LOAD_BUSY, false);
}
