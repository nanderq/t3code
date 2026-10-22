#include "gfx.h"

#include <string.h>

#include "util.h"

static C3D_RenderTarget *top_target, *bottom_target;
static C2D_TextBuf text_buf;
static float ascii_w[128];
static float line_feed;

void gfx_init(void) {
  C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
  C2D_Init(8192);
  C2D_Prepare();
  top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
  bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
  text_buf = C2D_TextBufNew(8192);
  fontEnsureMapped();
  CFNT_s *font = fontGetSystemFont();
  line_feed = fontGetInfo(font)->lineFeed;
  for (int i = 0; i < 128; i++) {
    int glyph = fontGlyphIndexFromCodePoint(font, (u32)(i < 32 ? ' ' : i));
    ascii_w[i] = fontGetCharWidthInfo(font, glyph)->charWidth;
  }
}

void gfx_exit(void) {
  C2D_TextBufDelete(text_buf);
  C2D_Fini();
  C3D_Fini();
}

void gfx_frame_begin(void) {
  C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
  C2D_TextBufClear(text_buf);
}

void gfx_frame_end(void) { C3D_FrameEnd(0); }

void gfx_target_top(void) {
  C2D_TargetClear(top_target, COL_BG);
  C2D_SceneBegin(top_target);
}

void gfx_target_bottom(void) {
  C2D_TargetClear(bottom_target, COL_BG);
  C2D_SceneBegin(bottom_target);
}

float gfx_line_h(float scale) { return line_feed * scale; }

static float glyph_w(uint32_t cp) {
  if (cp < 128) return ascii_w[cp];
  CFNT_s *font = fontGetSystemFont();
  return fontGetCharWidthInfo(font, fontGlyphIndexFromCodePoint(font, cp))->charWidth;
}

float gfx_text_w(const char *s, int len, float scale) {
  float w = 0;
  const char *end = len < 0 ? s + strlen(s) : s + len;
  while (s < end && *s) {
    uint32_t cp;
    s += util_utf8_decode(s, &cp);
    if (cp == '\n') break;
    w += glyph_w(cp);
  }
  return w * scale;
}

void gfx_rect(float x, float y, float w, float h, u32 color) { C2D_DrawRectSolid(x, y, 0.5f, w, h, color); }

void gfx_dot(float cx, float cy, float r, u32 color) { C2D_DrawCircleSolid(cx, cy, 0.5f, r, color); }

void gfx_rrect(float x, float y, float w, float h, float r, u32 color) {
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  C2D_DrawRectSolid(x + r, y, 0.5f, w - 2 * r, h, color);
  C2D_DrawRectSolid(x, y + r, 0.5f, r, h - 2 * r, color);
  C2D_DrawRectSolid(x + w - r, y + r, 0.5f, r, h - 2 * r, color);
  C2D_DrawCircleSolid(x + r, y + r, 0.5f, r, color);
  C2D_DrawCircleSolid(x + w - r, y + r, 0.5f, r, color);
  C2D_DrawCircleSolid(x + r, y + h - r, 0.5f, r, color);
  C2D_DrawCircleSolid(x + w - r, y + h - r, 0.5f, r, color);
}

void gfx_tri(float x1, float y1, float x2, float y2, float x3, float y3, u32 color) {
  C2D_DrawTriangle(x1, y1, color, x2, y2, color, x3, y3, color, 0.5f);
}

void gfx_pill(float x, float y, float w, float h, u32 color) {
  float r = h / 2;
  if (w < h) w = h;
  C2D_DrawRectSolid(x + r, y, 0.5f, w - h, h, color);
  C2D_DrawCircleSolid(x + r, y + r, 0.5f, r, color);
  C2D_DrawCircleSolid(x + w - r, y + r, 0.5f, r, color);
}

void gfx_text(float x, float y, float scale, u32 color, const char *s, int len) {
  char tmp[512];
  size_t n = len < 0 ? strlen(s) : (size_t)len;
  if (n >= sizeof tmp) n = sizeof tmp - 1;
  memcpy(tmp, s, n);
  tmp[n] = '\0';
  // The system font has no tab glyph, and each line is drawn on its own.
  for (char *p = tmp; *p; p++)
    if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
  C2D_Text text;
  C2D_TextParse(&text, text_buf, tmp);
  C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

float gfx_text_fit(float x, float y, float scale, u32 color, const char *s, float max_w) {
  float full = gfx_text_w(s, -1, scale);
  if (full <= max_w) {
    gfx_text(x, y, scale, color, s, (int)strcspn(s, "\n"));
    return full;
  }
  float ell = gfx_text_w("...", 3, scale);
  float w = 0;
  const char *p = s;
  while (*p && *p != '\n') {
    uint32_t cp;
    size_t n = util_utf8_decode(p, &cp);
    float gw = glyph_w(cp) * scale;
    if (w + gw + ell > max_w) break;
    w += gw;
    p += n;
  }
  char tmp[512];
  size_t n = (size_t)(p - s);
  if (n > sizeof tmp - 4) n = sizeof tmp - 4;
  memcpy(tmp, s, n);
  memcpy(tmp + n, "...", 4);
  gfx_text(x, y, scale, color, tmp, -1);
  return w + ell;
}

void gfx_text_right(float right, float y, float scale, u32 color, const char *s) {
  gfx_text(right - gfx_text_w(s, -1, scale), y, scale, color, s, -1);
}

int gfx_wrap(const char *s, float scale, float max_w, WrapLine *out, int max) {
  int count = 0;
  const char *line = s;
  while (*line && count < max) {
    const char *p = line, *last_space = NULL;
    float w = 0;
    while (*p && *p != '\n') {
      uint32_t cp;
      size_t n = util_utf8_decode(p, &cp);
      float gw = glyph_w(cp) * scale;
      if (w + gw > max_w && p > line) break;
      if (cp == ' ') last_space = p;
      w += gw;
      p += n;
    }
    const char *end = p;
    const char *next = p;
    if (*p == '\n') {
      next = p + 1;
    } else if (*p) {
      // Break at the last space when one exists, otherwise mid-word.
      if (last_space && last_space > line) {
        end = last_space;
        next = last_space + 1;
      }
    }
    out[count].start = line;
    out[count].len = (int)(end - line);
    count++;
    line = next;
  }
  return count;
}

bool rect_hit(Rect r, int px, int py) { return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h; }

void gfx_button(Rect r, const char *hint, const char *label, bool enabled, bool primary) {
  u32 bg = !enabled ? COL_PANEL : primary ? COL_ACCENT : COL_PANEL_HI;
  u32 fg = !enabled ? COL_DIM : primary ? COL_BG : COL_TEXT;
  gfx_pill(r.x, r.y, r.w, r.h, bg);
  float lw = gfx_text_w(label, -1, SCALE_SMALL);
  float hw = hint ? gfx_text_w(hint, -1, SCALE_SMALL) + 6 : 0;
  float x = r.x + (r.w - lw - hw) / 2;
  float y = r.y + (r.h - gfx_line_h(SCALE_SMALL)) / 2;
  if (hint) {
    gfx_text(x, y, SCALE_SMALL, enabled ? (primary ? COL_BG : COL_ACCENT) : COL_DIM, hint, -1);
    x += hw;
  }
  gfx_text(x, y, SCALE_SMALL, fg, label, -1);
}
