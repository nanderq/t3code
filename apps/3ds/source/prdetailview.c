#include "prdetailview.h"

#include <stdio.h>
#include <string.h>

#include "gfx.h"

static float wrapped(const char *text, float y, float scale, u32 color, int max_lines, bool draw) {
  WrapLine lines[8];
  int n = gfx_wrap(text, scale, TOP_W - 28, lines, max_lines);
  for (int i = 0; i < n; i++) {
    if (draw && y >= 18 && y + gfx_line_h(scale) <= SCREEN_H)
      gfx_text(14, y, scale, color, lines[i].start, lines[i].len);
    y += gfx_line_h(scale);
  }
  return y;
}

static void line(float y, float scale, u32 color, const char *text) {
  if (y >= 18 && y + gfx_line_h(scale) <= SCREEN_H) gfx_text_fit(14, y, scale, color, text, TOP_W - 28);
}

static float detail_layout(const Client *c, float scroll, bool draw) {
  const PrDetail *d = &c->pr_detail;
  float y = 28 - scroll;
  char text[256];
  snprintf(text, sizeof text, "%s #%d", d->pr.repo, d->pr.number);
  if (draw) line(y, SCALE_SMALL, COL_MUTED, text);
  y = wrapped(d->pr.title, y + 18, SCALE_TITLE, COL_TEXT, 3, draw) + 6;
  snprintf(text, sizeof text, "%s -> %s", d->pr.head, d->pr.base);
  if (draw) line(y, SCALE_SMALL, COL_MUTED, text);
  y += 20;
  if (d->body[0]) y = wrapped(d->body, y, SCALE_BODY, COL_TEXT, 5, draw) + 10;
  snprintf(text, sizeof text, "Checks (%d%s)", d->check_total, d->check_total > d->check_count ? "; first 48 shown" : "");
  if (draw) line(y, SCALE_BODY, COL_TEXT, text);
  y += 22;
  if (!d->check_count) {
    if (draw) line(y, SCALE_SMALL, COL_MUTED, "No checks reported");
    y += 18;
  }
  for (int i = 0; i < d->check_count; i++) {
    const PrCheck *check = &d->checks[i];
    u32 color = !strcmp(check->status, "success") ? COL_OK
                : !strcmp(check->status, "failure") || !strcmp(check->status, "cancelled") ? COL_ERROR : COL_ATTENTION;
    snprintf(text, sizeof text, "%s  %s%s", check->status, check->name, check->required ? " (required)" : "");
    if (draw) line(y, SCALE_SMALL, color, text);
    y += 16;
    if (check->description[0]) {
      if (draw) line(y, SCALE_SMALL, COL_MUTED, check->description);
      y += 16;
    }
    y += 6;
  }
  return y + scroll - 18;
}

float prdetailview_height(const Client *c) { return detail_layout(c, 0, false); }

void prdetailview_draw(const Client *c, float scroll) { detail_layout(c, scroll, true); }

void prdetailview_bottom(const Client *c, bool armed) {
  const PrDetail *d = &c->pr_detail;
  float y = 38;
  gfx_text_fit(14, y, SCALE_TITLE, COL_TEXT, d->pr.title, BOT_W - 28);
  y += 28;
  const char *state = c->pr_detail_state == LOAD_BUSY ? "Loading details..."
                      : c->pr_detail_state == LOAD_FAILED ? c->pr_detail_error
                      : d->action_id ? "Sending action..." : "D-pad: scroll description and checks";
  WrapLine lines[4];
  int n = gfx_wrap(state, SCALE_SMALL, BOT_W - 28, lines, 4);
  for (int i = 0; i < n; i++, y += 15) gfx_text(14, y, SCALE_SMALL, COL_MUTED, lines[i].start, lines[i].len);
  y += 12;
  if (armed) gfx_text(14, y, SCALE_BODY, COL_ATTENTION, "Press Y again to confirm merge.", -1);
  else if (d->pr.draft) gfx_text(14, y, SCALE_SMALL, COL_MUTED, "Ready takes this PR out of draft.", -1);
  else if (prdetail_merge_allowed(c)) {
    char text[80];
    snprintf(text, sizeof text, "Merge method: %s", d->merge_method);
    gfx_text(14, y, SCALE_SMALL, COL_MUTED, text, -1);
  } else gfx_text(14, y, SCALE_SMALL, COL_MUTED, "Merge unavailable; check reviews and access.", -1);
  gfx_text(14, 176, SCALE_SMALL, COL_DIM, "SELECT: reload details", -1);
}
