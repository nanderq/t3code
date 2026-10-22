#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

#define TOP_W 400
#define BOT_W 320
#define SCREEN_H 240

#define COL_BG C2D_Color32(0x0C, 0x0C, 0x0F, 0xFF)
#define COL_PANEL C2D_Color32(0x17, 0x17, 0x1C, 0xFF)
#define COL_PANEL_HI C2D_Color32(0x24, 0x22, 0x30, 0xFF)
#define COL_LINE C2D_Color32(0x2A, 0x2A, 0x33, 0xFF)
#define COL_TEXT C2D_Color32(0xEC, 0xEC, 0xF1, 0xFF)
#define COL_MUTED C2D_Color32(0x8C, 0x8C, 0x99, 0xFF)
#define COL_DIM C2D_Color32(0x5A, 0x5A, 0x66, 0xFF)
#define COL_ACCENT C2D_Color32(0x9B, 0x7B, 0xFF, 0xFF)
#define COL_RUNNING C2D_Color32(0x4F, 0xA3, 0xFF, 0xFF)
#define COL_ATTENTION C2D_Color32(0xFF, 0xA8, 0x3D, 0xFF)
#define COL_OK C2D_Color32(0x4C, 0xC9, 0x7A, 0xFF)
#define COL_ERROR C2D_Color32(0xFF, 0x5C, 0x5C, 0xFF)
// Conversation view.
#define COL_BUBBLE C2D_Color32(0x26, 0x24, 0x33, 0xFF)
#define COL_CODE C2D_Color32(0xE8, 0xB8, 0x6D, 0xFF)
#define COL_CODE_BG C2D_Color32(0x1A, 0x19, 0x22, 0xFF)
#define COL_LINK C2D_Color32(0x8F, 0xB3, 0xFF, 0xFF)
#define COL_EMPH C2D_Color32(0xC9, 0xC2, 0xE8, 0xFF)
#define COL_ERROR_BG C2D_Color32(0x33, 0x16, 0x1A, 0xFF)
#define COL_ATTENTION_BG C2D_Color32(0x33, 0x25, 0x12, 0xFF)

// Button glyphs in the system font's private-use area.
#define GLYPH_A "\xEE\x80\x80"
#define GLYPH_B "\xEE\x80\x81"
#define GLYPH_X "\xEE\x80\x82"
#define GLYPH_Y "\xEE\x80\x83"
#define GLYPH_L "\xEE\x80\x84"
#define GLYPH_R "\xEE\x80\x85"
#define GLYPH_DPAD "\xEE\x80\x86"

#define SCALE_TITLE 0.62f
#define SCALE_BODY 0.5f
#define SCALE_SMALL 0.42f

typedef struct {
  const char *start;
  int len;
} WrapLine;

void gfx_init(void);
void gfx_exit(void);
void gfx_frame_begin(void);
void gfx_frame_end(void);
void gfx_target_top(void);
void gfx_target_bottom(void);

float gfx_line_h(float scale);
float gfx_text_w(const char *s, int len, float scale);
void gfx_rect(float x, float y, float w, float h, u32 color);
void gfx_pill(float x, float y, float w, float h, u32 color);
void gfx_dot(float cx, float cy, float r, u32 color);
void gfx_rrect(float x, float y, float w, float h, float r, u32 color);
void gfx_tri(float x1, float y1, float x2, float y2, float x3, float y3, u32 color);
// Draws `len` bytes (or the whole string when len < 0) on one line.
void gfx_text(float x, float y, float scale, u32 color, const char *s, int len);
// One line, cut with an ellipsis to fit max_w. Returns the drawn width.
float gfx_text_fit(float x, float y, float scale, u32 color, const char *s, float max_w);
void gfx_text_right(float right, float y, float scale, u32 color, const char *s);
// Word-wraps s into at most max lines no wider than max_w. Newlines force breaks.
int gfx_wrap(const char *s, float scale, float max_w, WrapLine *out, int max);

typedef struct {
  float x, y, w, h;
} Rect;

bool rect_hit(Rect r, int px, int py);
// A labelled button with a hint glyph such as "A". Disabled buttons are dimmed.
void gfx_button(Rect r, const char *hint, const char *label, bool enabled, bool primary);
