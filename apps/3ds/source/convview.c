#include "convview.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "util.h"

#define X0 10.0f
#define X1 (TOP_W - 12.0f)
#define CODE_SCALE 0.42f
#define PARA_GAP 5.0f
#define ITEM_GAP 7.0f
#define LIVE_ROWS 8
#define CODE_MAX_LINES 60
#define COL_CODE_TEXT C2D_Color32(0xD8, 0xD8, 0xE2, 0xFF)
#define COL_ERROR_TEXT C2D_Color32(0xFF, 0xC4, 0xC4, 0xFF)

// ---------------------------------------------------------------- draw operations

typedef enum { OP_TEXT, OP_RECT, OP_RRECT, OP_CHEVRON, OP_DOT } OpKind;

typedef struct {
  uint8_t kind;
  uint8_t flag;  // text: bold; chevron: open
  uint16_t len;
  uint32_t str;
  float x, y, w, h;
  float scale;  // text scale, corner radius, or dot radius
  u32 color;
} Op;

typedef struct {
  float y;
  uint32_t str;
} Turn;

static Op *ops;
static int op_count, op_cap;
static char *arena;
static size_t arena_len, arena_cap;
static Turn *turns;
static int turn_count, turn_cap;
static float cy, height;
static float live_y = -1;
static int64_t live_since;
static bool live_waiting;
static bool folds_open;
static int fold_count;

// Inputs of the last build; any change rebuilds.
static int built_version = -1;
static bool built_folds, built_active;
static char built_run[ID_CAP], built_pending[ID_CAP], built_thread[ID_CAP];

static const char *root;
static size_t root_len;

static Op *push_op(OpKind kind) {
  if (op_count == op_cap) {
    int cap = op_cap ? op_cap * 2 : 1024;
    Op *grown = realloc(ops, sizeof(Op) * (size_t)cap);
    if (!grown) return NULL;
    ops = grown;
    op_cap = cap;
  }
  Op *o = &ops[op_count++];
  memset(o, 0, sizeof *o);
  o->kind = (uint8_t)kind;
  return o;
}

static uint32_t intern(const char *s, int len) {
  if (arena_len + (size_t)len + 1 > arena_cap) {
    size_t cap = arena_cap ? arena_cap : 16384;
    while (cap < arena_len + (size_t)len + 1) cap *= 2;
    char *grown = realloc(arena, cap);
    if (!grown) return 0;
    arena = grown;
    arena_cap = cap;
  }
  uint32_t at = (uint32_t)arena_len;
  memcpy(arena + at, s, (size_t)len);
  arena[at + (size_t)len] = '\0';
  arena_len += (size_t)len + 1;
  return at;
}

static void op_text(float x, float y, float scale, u32 color, bool bold, const char *s, int len) {
  if (len < 0) len = (int)strlen(s);
  if (len == 0) return;
  if (len > 500) len = 500;
  uint32_t str = intern(s, len);
  Op *o = push_op(OP_TEXT);
  if (!o) return;
  *o = (Op){OP_TEXT, bold, (uint16_t)len, str, x, y, 0, gfx_line_h(scale), scale, color};
}

static void op_rect(float x, float y, float w, float h, u32 color) {
  Op *o = push_op(OP_RECT);
  if (o) *o = (Op){OP_RECT, 0, 0, 0, x, y, w, h, 0, color};
}

static int op_rrect(float x, float y, float w, float h, float r, u32 color) {
  Op *o = push_op(OP_RRECT);
  if (!o) return -1;
  *o = (Op){OP_RRECT, 0, 0, 0, x, y, w, h, r, color};
  return op_count - 1;
}

static void op_dot(float cx, float cy_, float r, u32 color) {
  Op *o = push_op(OP_DOT);
  if (o) *o = (Op){OP_DOT, 0, 0, 0, cx - r, cy_ - r, r * 2, r * 2, r, color};
}

// ---------------------------------------------------------------- text helpers

static float text_w(const char *s, int len, float scale) { return gfx_text_w(s, len, scale); }

// Bytes of `s` that fit in `max_w`, cut on a character boundary.
static int fit_len(const char *s, int len, float scale, float max_w) {
  float w = 0;
  int i = 0;
  while (i < len) {
    uint32_t cp;
    int n = (int)util_utf8_decode(s + i, &cp);
    if (n <= 0) n = 1;
    float cw = text_w(s + i, n, scale);
    if (w + cw > max_w && i > 0) break;
    w += cw;
    i += n;
  }
  return i;
}

// One line of text, cut with "..." when it doesn't fit. Returns its width.
static float op_text_fit(float x, float y, float scale, u32 color, const char *s, int len, float max_w) {
  if (len < 0) len = (int)strlen(s);
  float w = text_w(s, len, scale);
  if (w <= max_w) {
    op_text(x, y, scale, color, false, s, len);
    return w;
  }
  float ell = text_w("...", 3, scale);
  int n = fit_len(s, len, scale, max_w - ell);
  char buf[520];
  if (n > 510) n = 510;
  memcpy(buf, s, (size_t)n);
  memcpy(buf + n, "...", 3);
  op_text(x, y, scale, color, false, buf, n + 3);
  return text_w(buf, n + 3, scale);
}

// Shows paths relative to the project folder, as the web does.
static const char *shorten(const char *s, char *buf, size_t cap) {
  if (!root_len) return s;
  size_t o = 0;
  while (*s && o + 1 < cap) {
    if (!strncmp(s, root, root_len) && s[root_len] == '/') {
      s += root_len + 1;
      continue;
    }
    buf[o++] = *s++;
  }
  buf[o] = '\0';
  return buf;
}

static int first_line_len(const char *s) { return (int)strcspn(s, "\n"); }

static void fmt_duration(int64_t secs, char *out, size_t cap) {
  if (secs < 60) snprintf(out, cap, "%llds", (long long)(secs < 1 ? 1 : secs));
  else if (secs < 3600) snprintf(out, cap, "%lldm %llds", (long long)(secs / 60), (long long)(secs % 60));
  else snprintf(out, cap, "%lldh %lldm", (long long)(secs / 3600), (long long)(secs % 3600 / 60));
}

// ---------------------------------------------------------------- inline markdown

enum { ST_BOLD = 1, ST_EMPH = 2, ST_CODE = 4, ST_LINK = 8, ST_STRIKE = 16 };

typedef struct {
  const char *s;
  int len;
  uint8_t st;
} Run;

#define MAX_RUNS 768
static Run runs[MAX_RUNS];

static int add_run(int count, const char *s, int len, uint8_t st) {
  if (len <= 0 || count >= MAX_RUNS) return count;
  Run *prev = count ? &runs[count - 1] : NULL;
  if (prev && prev->st == st && prev->s + prev->len == s) {
    prev->len += len;
    return count;
  }
  runs[count] = (Run){s, len, st};
  return count + 1;
}

static bool word_char(char c) { return isalnum((unsigned char)c) || ((unsigned char)c & 0x80); }

// Finds the closing emphasis marker for an opener of `m` copies of `ch`.
static const char *find_closer(const char *p, const char *end, char ch, int m) {
  for (const char *q = p + 1; q + m <= end; q++) {
    if (*q != ch) continue;
    int k = 0;
    while (q + k < end && q[k] == ch) k++;
    if (k >= m && q[-1] != ' ' && q[-1] != '\n' && !(ch == '_' && q + k < end && word_char(q[k])))
      return q + (k - m);
    q += k - 1;
  }
  return NULL;
}

static int parse_inline(const char *s, int n, uint8_t st, int count) {
  const char *p = s, *end = s + n, *text = s;
  while (p < end && count < MAX_RUNS) {
    char ch = *p;
    if (ch == '\\' && p + 1 < end && strchr("\\`*_[]()#+-.!~|<>", p[1])) {
      count = add_run(count, text, (int)(p - text), st);
      text = p + 1;
      p += 2;
      continue;
    }
    if (ch == '`') {
      int ticks = 0;
      while (p + ticks < end && p[ticks] == '`') ticks++;
      const char *close = NULL;
      for (const char *q = p + ticks; q < end; q++) {
        if (*q != '`') continue;
        int k = 0;
        while (q + k < end && q[k] == '`') k++;
        if (k == ticks) {
          close = q;
          break;
        }
        q += k - 1;
      }
      if (close) {
        count = add_run(count, text, (int)(p - text), st);
        const char *cs = p + ticks, *ce = close;
        if (ce - cs >= 2 && *cs == ' ' && ce[-1] == ' ') cs++, ce--;
        count = add_run(count, cs, (int)(ce - cs), st | ST_CODE);
        p = close + ticks;
        text = p;
        continue;
      }
      p += ticks;
      continue;
    }
    if (ch == '*' || ch == '_' || ch == '~') {
      int k = 0;
      while (p + k < end && p[k] == ch) k++;
      int m = ch == '~' ? 2 : k >= 2 ? 2 : 1;
      bool opens = k >= m && p + m < end && p[m] != ' ' && p[m] != '\n' &&
                   !(ch == '_' && p > s && word_char(p[-1]));
      const char *close = opens ? find_closer(p + m - 1, end, ch, m) : NULL;
      if (close && close > p + m - 1 + 0) {
        uint8_t add = ch == '~' ? ST_STRIKE : m == 2 ? ST_BOLD : ST_EMPH;
        count = add_run(count, text, (int)(p - text), st);
        count = parse_inline(p + m, (int)(close - (p + m)), st | add, count);
        p = close + m;
        text = p;
        continue;
      }
      p += k;
      continue;
    }
    if (ch == '[' || (ch == '!' && p + 1 < end && p[1] == '[')) {
      const char *lb = ch == '!' ? p + 1 : p;
      const char *rb = memchr(lb, ']', (size_t)(end - lb));
      if (rb && rb + 1 < end && rb[1] == '(') {
        const char *rp = memchr(rb, ')', (size_t)(end - rb));
        if (rp) {
          count = add_run(count, text, (int)(p - text), st);
          if (rb > lb + 1) count = parse_inline(lb + 1, (int)(rb - lb - 1), st | ST_LINK, count);
          else count = add_run(count, rb + 2, (int)(rp - rb - 2), st | ST_LINK);
          p = rp + 1;
          text = p;
          continue;
        }
      }
    }
    if (ch == '<' && end - p > 8 && (!strncmp(p + 1, "http://", 7) || !strncmp(p + 1, "https://", 8))) {
      const char *gt = memchr(p, '>', (size_t)(end - p));
      if (gt) {
        count = add_run(count, text, (int)(p - text), st);
        count = add_run(count, p + 1, (int)(gt - p - 1), st | ST_LINK);
        p = gt + 1;
        text = p;
        continue;
      }
    }
    p++;
  }
  return add_run(count, text, (int)(end - text), st);
}

// ---------------------------------------------------------------- flowing text

typedef struct {
  float x0, x1;  // line extent
  float y;       // top of the current line
  float scale;
  u32 color;
  bool emit;     // false measures only
  bool code_bg;  // inline code gets a background
  int max_lines; // 0 for no limit
  int lines;     // lines started so far, including the current one
  float max_x;   // right edge of the widest line
  bool clipped;
  bool hard_breaks;  // newlines start new lines instead of acting as spaces
} Flow;

static bool flow_newline(Flow *f, float *x) {
  if (f->max_lines && f->lines >= f->max_lines) {
    f->clipped = true;
    return false;
  }
  f->y += gfx_line_h(f->scale);
  f->lines++;
  *x = f->x0;
  return true;
}

static void flow_word(Flow *f, const char *s, int len, uint8_t st, float x, float w, float trailing) {
  if (!f->emit) return;
  float lh = gfx_line_h(f->scale);
  u32 color = st & ST_CODE ? COL_CODE : st & ST_LINK ? COL_LINK : st & ST_EMPH ? COL_EMPH : st & ST_BOLD ? COL_TEXT : f->color;
  if ((st & ST_CODE) && f->code_bg) op_rrect(x - 1.5f, f->y + 1, w + 3 + trailing, lh - 1, 2, COL_CODE_BG);
  op_text(x, f->y, f->scale, color, st & ST_BOLD, s, len);
  if (st & ST_STRIKE) op_rect(x, f->y + lh * 0.55f, w + trailing, 1, color);
}

// Lays runs out word by word from x, wrapping at f->x1. Returns the end x.
static float flow_runs(Flow *f, const Run *rs, int n, float x) {
  if (f->lines == 0) f->lines = 1;
  bool pending_space = false;
  float space = text_w(" ", 1, f->scale);
  for (int r = 0; r < n && !f->clipped; r++) {
    const char *p = rs[r].s, *end = p + rs[r].len;
    uint8_t st = rs[r].st;
    while (p < end && !f->clipped) {
      if (*p == '\n' && f->hard_breaks) {
        if (!flow_newline(f, &x)) break;
        pending_space = false;
        p++;
        continue;
      }
      if (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') {
        pending_space = true;
        p++;
        continue;
      }
      const char *q = p;
      while (q < end && *q != ' ' && *q != '\n' && *q != '\t' && *q != '\r') q++;
      int len = (int)(q - p);
      float w = text_w(p, len, f->scale) + ((st & ST_BOLD) ? 0.6f : 0);
      float gap = pending_space && x > f->x0 ? space : 0;
      if (x + gap + w > f->x1 && x > f->x0) {
        if (!flow_newline(f, &x)) break;
        gap = 0;
      }
      x += gap;
      // A word longer than the line breaks anywhere.
      while (w > f->x1 - x && len > 0) {
        int fit = fit_len(p, len, f->scale, f->x1 - x);
        float fw = text_w(p, fit, f->scale);
        flow_word(f, p, fit, st, x, fw, 0);
        if (x + fw > f->max_x) f->max_x = x + fw;
        p += fit;
        len -= fit;
        if (!flow_newline(f, &x)) break;
        w = text_w(p, len, f->scale);
      }
      if (f->clipped) break;
      bool space_follows_in_code = (st & ST_CODE) && q < end && *q == ' ';
      flow_word(f, p, len, st, x, w, space_follows_in_code ? space : 0);
      x += w;
      if (x > f->max_x) f->max_x = x;
      pending_space = false;
      p = q;
    }
  }
  return x;
}

static Flow flow_at(float x0, float x1, float scale, u32 color) {
  Flow f = {x0, x1, cy, scale, color, true, true, 0, 0, x0, false, false};
  return f;
}

// Flows markdown inline text and advances the cursor past it.
// User messages keep their line breaks; agent markdown follows CommonMark soft breaks.
static bool para_hard_breaks;

static void para(const char *s, int len, float x0, float x1, float scale, u32 color, uint8_t st, int max_lines) {
  int n = parse_inline(s, len, st, 0);
  Flow f = flow_at(x0, x1, scale, color);
  f.max_lines = max_lines;
  f.hard_breaks = para_hard_breaks;
  flow_runs(&f, runs, n, x0);
  cy = f.y + gfx_line_h(scale);
}

// ---------------------------------------------------------------- markdown blocks

static const char *line_end(const char *p, const char *end) {
  const char *e = memchr(p, '\n', (size_t)(end - p));
  return e ? e : end;
}

static const char *skip_ws(const char *s, const char *e) {
  while (s < e && (*s == ' ' || *s == '\t')) s++;
  return s;
}

static bool is_blank(const char *s, const char *e) {
  for (; s < e; s++)
    if (*s != ' ' && *s != '\t' && *s != '\r') return false;
  return true;
}

static int heading_level(const char *s, const char *e) {
  int n = 0;
  while (s + n < e && s[n] == '#') n++;
  return n >= 1 && n <= 6 && (s + n == e || s[n] == ' ') ? n : 0;
}

static bool is_fence(const char *s, const char *e) {
  return e - s >= 3 && (!strncmp(s, "```", 3) || !strncmp(s, "~~~", 3));
}

static bool is_rule(const char *s, const char *e) {
  char c = 0;
  int count = 0;
  for (; s < e; s++) {
    if (*s == ' ' || *s == '\r') continue;
    if (*s != '-' && *s != '*' && *s != '_') return false;
    if (c && *s != c) return false;
    c = *s;
    count++;
  }
  return count >= 3;
}

// Length of a list marker and its space, or 0.
static int list_marker(const char *s, const char *e, bool *ordered) {
  if (s + 1 < e && (*s == '-' || *s == '*' || *s == '+') && s[1] == ' ') {
    *ordered = false;
    return 2;
  }
  int d = 0;
  while (s + d < e && d < 4 && isdigit((unsigned char)s[d])) d++;
  if (d > 0 && s + d + 1 < e && (s[d] == '.' || s[d] == ')') && s[d + 1] == ' ') {
    *ordered = true;
    return d + 2;
  }
  return 0;
}

static bool is_table_sep(const char *s, const char *e) {
  s = skip_ws(s, e);
  if (s >= e || (*s != '|' && *s != '-' && *s != ':')) return false;
  bool dash = false;
  for (; s < e; s++) {
    if (*s == '-') dash = true;
    else if (*s != '|' && *s != ':' && *s != ' ' && *s != '\r') return false;
  }
  return dash;
}

static bool starts_block(const char *s, const char *e) {
  bool ordered;
  return heading_level(s, e) || is_fence(s, e) || (is_rule(s, e) && e - s >= 3) || list_marker(s, e, &ordered) ||
         *s == '>' || *s == '|';
}

static void code_block(const char *p, const char *end, float x0, float x1) {
  float lh = gfx_line_h(CODE_SCALE);
  int bg = op_rrect(x0, cy, x1 - x0, 0, 4, COL_CODE_BG);
  float y = cy + 5, w = x1 - x0 - 14;
  int lines = 0, total = 0;
  for (const char *q = p; q < end; q = line_end(q, end) + 1) total++;
  for (const char *q = p; q < end && lines < CODE_MAX_LINES;) {
    const char *e = line_end(q, end);
    int len = (int)(e - q);
    if (len > 0 && q[len - 1] == '\r') len--;
    if (len == 0) y += lh;
    while (len > 0) {
      int fit = fit_len(q, len, CODE_SCALE, w);
      op_text(x0 + 7, y, CODE_SCALE, COL_CODE_TEXT, false, q, fit);
      q += fit;
      len -= fit;
      y += lh;
    }
    lines++;
    q = e + 1;
  }
  if (total > CODE_MAX_LINES) {
    char more[48];
    snprintf(more, sizeof more, "... %d more lines", total - CODE_MAX_LINES);
    op_text(x0 + 7, y, CODE_SCALE, COL_DIM, false, more, -1);
    y += lh;
  }
  if (bg >= 0) ops[bg].h = y + 5 - cy;
  cy = y + 5;
}

#define MAX_COLS 8

static int split_cells(const char *s, const char *e, const char **cs, const char **ce) {
  s = skip_ws(s, e);
  while (e > s && (e[-1] == ' ' || e[-1] == '\r')) e--;
  if (s < e && *s == '|') s++;
  if (e > s && e[-1] == '|' && (e - 1 == s || e[-2] != '\\')) e--;
  int n = 0;
  const char *c = s;
  for (const char *p = s; p <= e && n < MAX_COLS; p++) {
    if (p == e || (*p == '|' && (p == s || p[-1] != '\\'))) {
      const char *a = skip_ws(c, p), *b = p;
      while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
      cs[n] = a;
      ce[n] = b;
      n++;
      c = p + 1;
    }
  }
  return n;
}

static void table(const char *p, const char *end, float x0, float x1) {
  float scale = SCALE_SMALL, lh = gfx_line_h(scale), pad = 5;
  const char *cs[MAX_COLS], *ce[MAX_COLS];
  float natural[MAX_COLS] = {0};
  int cols = 0;
  for (const char *q = p; q < end; q = line_end(q, end) + 1) {
    const char *e = line_end(q, end);
    if (is_table_sep(q, e)) continue;
    int n = split_cells(q, e, cs, ce);
    if (n > cols) cols = n;
    for (int i = 0; i < n; i++) {
      float w = text_w(cs[i], (int)(ce[i] - cs[i]), scale) + 2 * pad;
      if (w > natural[i]) natural[i] = w;
    }
  }
  if (cols == 0) return;
  float avail = x1 - x0, sum = 0, widths[MAX_COLS];
  for (int i = 0; i < cols; i++) sum += natural[i];
  if (sum <= avail) {
    for (int i = 0; i < cols; i++) widths[i] = natural[i];
  } else {
    // Narrow columns keep their width; wide ones share what is left.
    float fair = avail / (float)cols, spare = avail, wide = 0;
    for (int i = 0; i < cols; i++) {
      if (natural[i] <= fair) spare -= natural[i];
      else wide += natural[i];
    }
    for (int i = 0; i < cols; i++) widths[i] = natural[i] <= fair ? natural[i] : spare * natural[i] / wide;
  }
  float tw = 0;
  for (int i = 0; i < cols; i++) tw += widths[i];
  int frame = op_rrect(x0, cy, tw, 0, 3, COL_LINE);
  int inner = op_rrect(x0 + 1, cy + 1, tw - 2, 0, 3, COL_BG);
  float top = cy;
  bool header = true;
  for (const char *q = p; q < end; q = line_end(q, end) + 1) {
    const char *e = line_end(q, end);
    if (is_table_sep(q, e)) continue;
    int n = split_cells(q, e, cs, ce);
    int head_bg = header ? op_rrect(x0 + 1, cy + 1, tw - 2, 0, 3, COL_PANEL_HI) : -1;
    float row_end = cy + lh, x = x0;
    for (int i = 0; i < cols; i++) {
      if (i < n) {
        int count = parse_inline(cs[i], (int)(ce[i] - cs[i]), header ? ST_BOLD : 0, 0);
        Flow f = flow_at(x + pad, x + widths[i] - pad, scale, COL_TEXT);
        f.y = cy + 3;
        flow_runs(&f, runs, count, x + pad);
        if (f.y + lh > row_end) row_end = f.y + lh;
      }
      x += widths[i];
    }
    row_end += 3;
    if (head_bg >= 0) ops[head_bg].h = row_end - cy - 1;
    cy = row_end;
    if (q + (e - q) < end) op_rect(x0 + 1, cy, tw - 2, 1, COL_LINE);
    header = false;
  }
  if (frame >= 0) ops[frame].h = cy - top + 1;
  if (inner >= 0) ops[inner].h = cy - top - 1;
  // Column dividers, drawn after the rows so they sit on top of the header.
  float x = x0;
  for (int i = 0; i + 1 < cols; i++) {
    x += widths[i];
    op_rect(x, top + 1, 1, cy - top - 1, COL_LINE);
  }
  cy += 1;
}

static void markdown(const char *text, float x0, float x1, float scale);

static void list_item(const char *s, const char *content_end, int level, bool ordered, int marker, float x0,
                      float x1, float scale) {
  float lh = gfx_line_h(scale);
  float bx = x0 + (float)level * 12;
  const char *content = s + marker;
  float tx = bx + 11;
  if (ordered) {
    op_text(bx, cy, scale, COL_MUTED, false, s, marker - 1);
    tx = bx + text_w(s, marker - 1, scale) + 4;
  } else {
    op_dot(bx + 4, cy + lh * 0.5f, 2.2f, COL_MUTED);
  }
  // Task list checkboxes.
  if (content_end - content >= 3 && content[0] == '[' && content[2] == ']' &&
      (content[1] == ' ' || content[1] == 'x' || content[1] == 'X')) {
    bool done = content[1] != ' ';
    op_rrect(tx, cy + lh * 0.5f - 4.5f, 9, 9, 2, done ? COL_ACCENT : COL_DIM);
    if (!done) op_rrect(tx + 1, cy + lh * 0.5f - 3.5f, 7, 7, 2, COL_BG);
    tx += 13;
    content += 3;
  }
  para(content, (int)(content_end - content), tx, x1, scale, COL_TEXT, 0, 0);
}

// Block markdown: headings, paragraphs, lists, quotes, code blocks, tables, rules.
static void markdown(const char *text, float x0, float x1, float scale) {
  const char *p = text, *end = text + strlen(text);
  bool first = true, prev_list = false;
  while (p < end) {
    const char *le = line_end(p, end), *next = le < end ? le + 1 : end;
    const char *s = skip_ws(p, le);
    if (is_blank(s, le)) {
      prev_list = false;
      p = next;
      continue;
    }
    bool ordered = false;
    int marker = list_marker(s, le, &ordered);
    if (!first) cy += marker && prev_list ? 1 : PARA_GAP;
    first = false;
    prev_list = false;
    if (is_fence(s, le)) {
      const char *body = next, *q = next;
      while (q < end) {
        const char *qe = line_end(q, end);
        if (is_fence(skip_ws(q, qe), qe)) break;
        q = qe < end ? qe + 1 : end;
      }
      const char *body_end = q >= end ? end : q > body ? q - 1 : body;
      code_block(body, body_end, x0, x1);
      p = q < end ? line_end(q, end) : end;
      if (p < end) p++;
      continue;
    }
    int level = heading_level(s, le);
    if (level) {
      if (cy > 0) cy += 3;
      float hs = level == 1 ? 0.6f : level == 2 ? 0.56f : scale;
      para(s + level, (int)(le - s - level), x0, x1, hs, COL_TEXT, ST_BOLD, 0);
      cy += 1;
      p = next;
      continue;
    }
    if (is_rule(s, le)) {
      op_rect(x0, cy + 4, x1 - x0, 1, COL_LINE);
      cy += 9;
      p = next;
      continue;
    }
    if (*s == '>') {
      // Strip the quote markers into a copy, then lay it out like any markdown.
      char *buf = malloc((size_t)(end - p) + 1);
      size_t n = 0;
      const char *q = p;
      while (q < end) {
        const char *qe = line_end(q, end), *qs = skip_ws(q, qe);
        if (qs >= qe || *qs != '>') break;
        qs++;
        if (qs < qe && *qs == ' ') qs++;
        memcpy(buf + n, qs, (size_t)(qe - qs));
        n += (size_t)(qe - qs);
        buf[n++] = '\n';
        q = qe < end ? qe + 1 : end;
      }
      buf[n] = '\0';
      float top = cy;
      markdown(buf, x0 + 10, x1, scale);
      op_rect(x0 + 2, top, 2, cy - top, COL_DIM);
      free(buf);
      p = q;
      continue;
    }
    if (*s == '|' && next < end && is_table_sep(next, line_end(next, end))) {
      const char *q = p;
      while (q < end) {
        const char *qe = line_end(q, end), *qs = skip_ws(q, qe);
        if (qs >= qe || (*qs != '|' && !is_table_sep(qs, qe))) break;
        q = qe < end ? qe + 1 : end;
      }
      table(p, q > p && q[-1] == '\n' ? q - 1 : q, x0, x1);
      p = q;
      continue;
    }
    // Paragraphs and list items run until a blank line or the next block.
    const char *content_end = le, *q = next;
    while (q < end) {
      const char *qe = line_end(q, end), *qs = skip_ws(q, qe);
      bool o;
      if (is_blank(qs, qe) || starts_block(qs, qe) || (marker && list_marker(qs, qe, &o))) break;
      content_end = qe;
      q = qe < end ? qe + 1 : end;
    }
    if (marker) {
      int indent = 0;
      for (const char *c = p; c < s; c++) indent += *c == '\t' ? 4 : 1;
      int lvl = indent / 2;
      list_item(s, content_end, lvl > 4 ? 4 : lvl, ordered, marker, x0, x1, scale);
      prev_list = true;
    } else {
      para(s, (int)(content_end - s), x0, x1, scale, COL_TEXT, 0, 0);
    }
    p = q;
  }
}

// ---------------------------------------------------------------- timeline items

static void divider(const char *text) {
  float lh = gfx_line_h(SCALE_SMALL), w = text_w(text, -1, SCALE_SMALL);
  if (w > X1 - X0 - 40) w = X1 - X0 - 40;
  float tx = (X0 + X1 - w) / 2, mid = cy + lh / 2;
  op_rect(X0, mid, tx - 8 - X0, 1, COL_LINE);
  op_text_fit(tx, cy, SCALE_SMALL, COL_DIM, text, -1, w);
  op_rect(tx + w + 8, mid, X1 - (tx + w + 8), 1, COL_LINE);
  cy += lh;
}

static void user_bubble(const ConvItem *it) {
  const float max_w = 300, pad = 8;
  if (turn_count == turn_cap) {
    turn_cap = turn_cap ? turn_cap * 2 : 16;
    turns = realloc(turns, sizeof(Turn) * (size_t)turn_cap);
  }
  turns[turn_count++] = (Turn){cy, intern(it->text, first_line_len(it->text) > 200 ? 200 : first_line_len(it->text))};
  float top = cy, x0 = X1 - max_w + pad;
  int at = op_rrect(X1 - max_w, cy, max_w, 0, 9, COL_BUBBLE);
  cy += pad - 1;
  para_hard_breaks = true;
  markdown(it->text, x0, X1 - pad, SCALE_BODY);
  para_hard_breaks = false;
  cy += pad + 1;
  if (at < 0) return;
  // Laid out at full width, then shrunk to the widest line and moved right.
  float right = x0;
  for (int i = at + 1; i < op_count; i++) {
    const Op *o = &ops[i];
    float r = o->x + (o->kind == OP_TEXT ? text_w(arena + o->str, o->len, o->scale) : o->w);
    if (r > right) right = r;
  }
  float bw = right - x0 + 2 * pad;
  if (bw > max_w) bw = max_w;
  float shift = max_w - bw;
  for (int i = at + 1; i < op_count; i++) ops[i].x += shift;
  ops[at].x = X1 - bw;
  ops[at].w = bw;
  ops[at].h = cy - top;
}

static int card_begin(u32 border, u32 bg) {
  int at = op_rrect(X0, cy, X1 - X0, 0, 6, border);
  op_rrect(X0 + 1, cy + 1, X1 - X0 - 2, 0, 5, bg);
  cy += 7;
  return at;
}

static void card_end(int at, float top) {
  cy += 6;
  if (at >= 0) {
    ops[at].h = cy - top;
    ops[at + 1].h = cy - top - 2;
  }
}

static void card_header(const char *title, const char *note, u32 color) {
  float lh = gfx_line_h(SCALE_SMALL);
  op_text(X0 + 9, cy, SCALE_SMALL, color, true, title, -1);
  if (note) op_text(X0 + 13 + text_w(title, -1, SCALE_SMALL), cy, SCALE_SMALL, COL_DIM, false, note, -1);
  cy += lh + 3;
}

static void work_row(const ConvItem *it, float indent, bool details) {
  float lh = gfx_line_h(SCALE_SMALL), x = X0 + indent, tx = x + 13, w = X1 - tx;
  char buf[520], tail[48];
  // Shapes rather than glyphs: the system font lacks most symbols.
  float mid = cy + lh / 2;
  switch (it->work) {
    case WORK_COMMAND: op_text(x + 2, cy, SCALE_SMALL, it->failed ? COL_ERROR : COL_DIM, false, "$", 1); break;
    case WORK_EDIT: op_rrect(x + 2, mid - 3, 6, 6, 1, it->failed ? COL_ERROR : COL_OK); break;
    case WORK_THOUGHT:
      op_dot(x + 5, mid, 3, COL_DIM);
      op_dot(x + 5, mid, 1.8f, COL_BG);
      break;
    case WORK_AGENT: op_dot(x + 5, mid, 3, COL_ACCENT); break;
    default: op_dot(x + 5, mid, 2, it->failed ? COL_ERROR : COL_MUTED); break;
  }
  const char *label = shorten(it->text, buf, sizeof buf);
  if (it->work == WORK_COMMAND) {
    float tail_w = 0;
    if (it->failed && it->exit_code > 0) {
      snprintf(tail, sizeof tail, "exit %d", it->exit_code);
      tail_w = text_w(tail, -1, SCALE_SMALL) + 8;
      op_text(X1 - tail_w + 8, cy, SCALE_SMALL, COL_ERROR, false, tail, -1);
    }
    op_text_fit(tx, cy, SCALE_SMALL, COL_CODE, label, first_line_len(label), w - tail_w);
  } else if (it->work == WORK_EDIT && (it->additions >= 0 || it->deletions >= 0)) {
    char add[16], del[16];
    snprintf(add, sizeof add, "+%d", it->additions < 0 ? 0 : it->additions);
    snprintf(del, sizeof del, "-%d", it->deletions < 0 ? 0 : it->deletions);
    float aw = text_w(add, -1, SCALE_SMALL), dw = text_w(del, -1, SCALE_SMALL);
    float lw = op_text_fit(tx, cy, SCALE_SMALL, COL_MUTED, label, first_line_len(label), w - aw - dw - 14);
    op_text(tx + lw + 6, cy, SCALE_SMALL, COL_OK, false, add, -1);
    op_text(tx + lw + 10 + aw, cy, SCALE_SMALL, COL_ERROR, false, del, -1);
  } else {
    op_text_fit(tx, cy, SCALE_SMALL, it->running ? COL_TEXT : COL_MUTED, label, first_line_len(label), w);
  }
  cy += lh + 1;
  if (!details || !it->detail) return;
  if (it->work == WORK_THOUGHT || it->work == WORK_AGENT) {
    para(it->detail, (int)strlen(it->detail), tx, X1, SCALE_SMALL, COL_DIM, 0, 2);
    cy += 2;
  } else if (it->work == WORK_COMMAND && it->failed) {
    float clh = gfx_line_h(CODE_SCALE);
    for (const char *q = it->detail; *q;) {
      int len = first_line_len(q);
      op_text_fit(tx, cy, CODE_SCALE, COL_DIM, q, len, w);
      cy += clh;
      q += len;
      if (*q) q++;
    }
    cy += 2;
  }
}

static void todo_card(const ConvItem *it) {
  float top = cy;
  int at = card_begin(COL_LINE, COL_PANEL);
  int done = 0, total = 0;
  for (const char *q = it->detail ? it->detail : ""; *q; q += first_line_len(q) + (q[first_line_len(q)] ? 1 : 0)) {
    total++;
    if (*q == 'c') done++;
  }
  char note[32];
  snprintf(note, sizeof note, "%d of %d done", done, total);
  card_header("Tasks", note, COL_TEXT);
  if (it->text[0]) para(it->text, (int)strlen(it->text), X0 + 9, X1 - 9, SCALE_SMALL, COL_MUTED, 0, 3);
  float lh = gfx_line_h(SCALE_SMALL);
  for (const char *q = it->detail ? it->detail : ""; *q;) {
    int len = first_line_len(q);
    char state = *q;
    float mid = cy + lh / 2;
    if (state == 'c') op_dot(X0 + 14, mid, 3.5f, COL_OK);
    else if (state == 'r') op_dot(X0 + 14, mid, 3.5f, COL_RUNNING);
    else {
      op_dot(X0 + 14, mid, 3.5f, COL_DIM);
      op_dot(X0 + 14, mid, 2.2f, COL_PANEL);
    }
    para(q + 1, len - 1, X0 + 23, X1 - 9, SCALE_SMALL, state == 'c' ? COL_MUTED : COL_TEXT, 0, 2);
    cy += 1;
    q += len;
    if (*q) q++;
  }
  card_end(at, top);
}

static bool is_pending(const ConvItem *it, const T3Thread *t) {
  return t && it->request_id[0] && !strcmp(it->request_id, t->pending_id) && t->pending_blocking;
}

static void render_item(const ConvItem *it, const T3Thread *t, float indent, bool details) {
  float top = cy;
  switch (it->kind) {
    case ITEM_USER: user_bubble(it); break;
    case ITEM_ASSISTANT: markdown(it->text, X0 + indent, X1, SCALE_BODY); break;
    case ITEM_PLAN: {
      int at = card_begin(COL_ACCENT, COL_PANEL);
      card_header("Plan", it->streaming ? "writing..." : NULL, COL_ACCENT);
      markdown(it->text, X0 + 9, X1 - 9, SCALE_BODY);
      card_end(at, top);
      break;
    }
    case ITEM_TODO: todo_card(it); break;
    case ITEM_WORK: work_row(it, indent, details); break;
    case ITEM_NOTICE: divider(it->text); break;
    case ITEM_ERROR: {
      int at = card_begin(COL_ERROR, COL_ERROR_BG);
      para(it->text, (int)strlen(it->text), X0 + 9, X1 - 9, SCALE_SMALL, COL_ERROR_TEXT, 0, 8);
      card_end(at, top);
      break;
    }
    case ITEM_APPROVAL:
      if (is_pending(it, t)) {
        int at = card_begin(COL_ATTENTION, COL_ATTENTION_BG);
        card_header("Needs your approval", GLYPH_A " approve   " GLYPH_Y " decline", COL_ATTENTION);
        if (it->detail) code_block(it->detail, it->detail + strlen(it->detail), X0 + 8, X1 - 8);
        else para(it->text, (int)strlen(it->text), X0 + 9, X1 - 9, SCALE_SMALL, COL_TEXT, 0, 6);
        card_end(at, top);
      } else {
        ConvItem row = *it;
        row.work = WORK_TOOL;
        work_row(&row, indent, false);
      }
      break;
    case ITEM_QUESTION:
      if (is_pending(it, t)) {
        int at = card_begin(COL_ACCENT, COL_PANEL);
        card_header("The agent has a question", GLYPH_X " answer", COL_ACCENT);
        markdown(it->detail ? it->detail : it->text, X0 + 9, X1 - 9, SCALE_SMALL);
        card_end(at, top);
      } else {
        ConvItem row = *it;
        row.work = WORK_TOOL;
        work_row(&row, indent, false);
      }
      break;
  }
}

static bool foldable(const ConvItem *it, const T3Thread *t, int index, int final) {
  if (it->kind == ITEM_WORK) return true;
  if (it->kind == ITEM_ASSISTANT) return index != final;
  if (it->kind == ITEM_APPROVAL || it->kind == ITEM_QUESTION) return !is_pending(it, t);
  return false;
}

static void gap(float h) {
  if (op_count) cy += h;
}

// One run's items after its user message. A finished run folds its work, like the web.
static void layout_run(const ConvItem *items, int a, int b, const T3Thread *t) {
  bool live = t && items[a].run_id[0] && !strcmp(items[a].run_id, t->active_run_id) && thread_is_active(t);
  int final = -1, folded = 0, commands = 0, edits = 0, other = 0;
  int64_t start = 0, finish = 0;
  for (int k = a; k < b; k++) {
    const ConvItem *it = &items[k];
    if (it->kind == ITEM_ASSISTANT) final = k;
    if (it->started_at && (!start || it->started_at < start)) start = it->started_at;
    if (it->ended_at > finish) finish = it->ended_at;
  }
  for (int k = a; k < b; k++) {
    if (!foldable(&items[k], t, k, final)) continue;
    folded++;
    if (items[k].kind != ITEM_WORK) continue;
    if (items[k].work == WORK_COMMAND) commands++;
    else if (items[k].work == WORK_EDIT) edits++;
    else if (items[k].work != WORK_THOUGHT) other++;
  }
  if (live) {
    // The running turn stays open, showing its latest steps.
    int shown_from = a, work = 0;
    for (int k = b - 1; k >= a; k--) {
      if (items[k].kind == ITEM_WORK && ++work > LIVE_ROWS && !folds_open) {
        shown_from = k + 1;
        break;
      }
    }
    if (shown_from > a) {
      char note[48];
      snprintf(note, sizeof note, "%d earlier steps", shown_from - a);
      gap(ITEM_GAP);
      op_text(X0 + 13, cy, SCALE_SMALL, COL_DIM, false, note, -1);
      cy += gfx_line_h(SCALE_SMALL);
    }
    for (int k = shown_from; k < b; k++) {
      gap(items[k].kind == ITEM_WORK && k > shown_from && items[k - 1].kind == ITEM_WORK ? 0 : ITEM_GAP);
      render_item(&items[k], t, 0, true);
    }
    return;
  }
  bool fold_drawn = folded == 0;
  for (int k = a; k < b; k++) {
    const ConvItem *it = &items[k];
    bool in_fold = foldable(it, t, k, final);
    if (in_fold && !fold_drawn) {
      fold_drawn = true;
      fold_count++;
      gap(ITEM_GAP);
      char label[64], dur[24], summary[96] = "";
      if (start && finish > start) {
        fmt_duration(finish - start, dur, sizeof dur);
        snprintf(label, sizeof label, "Worked for %s", dur);
      } else {
        snprintf(label, sizeof label, "Worked");
      }
      size_t o = 0;
      if (commands) o += (size_t)snprintf(summary + o, sizeof summary - o, "%d command%s", commands, commands == 1 ? "" : "s");
      if (edits && o < sizeof summary)
        o += (size_t)snprintf(summary + o, sizeof summary - o, "%s%d edit%s", o ? ", " : "", edits, edits == 1 ? "" : "s");
      if (other && o < sizeof summary)
        snprintf(summary + o, sizeof summary - o, "%s%d other step%s", o ? ", " : "", other, other == 1 ? "" : "s");
      float lh = gfx_line_h(SCALE_SMALL);
      Op *chev = push_op(OP_CHEVRON);
      if (chev) *chev = (Op){OP_CHEVRON, folds_open, 0, 0, X0 + 1, cy + lh / 2 - 3.5f, 7, 7, 0, COL_MUTED};
      float lw = text_w(label, -1, SCALE_SMALL);
      op_text(X0 + 13, cy, SCALE_SMALL, COL_MUTED, false, label, -1);
      if (summary[0]) op_text_fit(X0 + 21 + lw, cy, SCALE_SMALL, COL_DIM, summary, -1, X1 - X0 - 21 - lw);
      cy += lh + 1;
    }
    if (in_fold && !folds_open) continue;
    bool tight = in_fold && it->kind == ITEM_WORK && k > a && items[k - 1].kind == ITEM_WORK;
    gap(tight ? 0 : in_fold ? 3 : ITEM_GAP);
    render_item(it, t, in_fold ? 13 : 0, true);
  }
}

static void layout(Client *c, const T3Thread *t) {
  op_count = 0;
  arena_len = 0;
  turn_count = 0;
  fold_count = 0;
  live_y = -1;
  cy = 6;
  Project *p = t ? client_find_project(c, t->project_id) : NULL;
  root = p ? p->root : NULL;
  root_len = root ? strlen(root) : 0;
  if (c->conv.has_more || c->conv.limited) {
    divider(c->conv.limited ? "History memory limit reached" : "At the top: LEFT loads earlier messages");
    cy += 2;
  }
  const ConvItem *items = c->conv.items;
  int n = c->conv.count;
  for (int i = 0; i < n;) {
    if (items[i].kind == ITEM_USER) {
      gap(i ? 12 : 0);
      render_item(&items[i], t, 0, true);
      i++;
      continue;
    }
    int j = i;
    while (j < n && items[j].kind != ITEM_USER && !strcmp(items[j].run_id, items[i].run_id)) j++;
    layout_run(items, i, j, t);
    i = j;
  }
  if (t && thread_is_active(t)) {
    live_since = 0;
    for (int i = 0; i < n; i++)
      if (!strcmp(items[i].run_id, t->active_run_id) && items[i].started_at &&
          (!live_since || items[i].started_at < live_since))
        live_since = items[i].started_at;
    live_waiting = t->pending_id[0] && t->pending_blocking;
    gap(ITEM_GAP);
    live_y = cy;
    cy += gfx_line_h(SCALE_SMALL);
  }
  height = cy + 10;
}

bool convview_sync(Client *c, const T3Thread *t) {
  bool active = t && thread_is_active(t);
  const char *run = t ? t->active_run_id : "", *pending = t && t->pending_blocking ? t->pending_id : "";
  const char *thread = t ? t->id : "";
  if (built_version == c->conv_version && built_folds == folds_open && built_active == active &&
      !strcmp(built_run, run) && !strcmp(built_pending, pending) && !strcmp(built_thread, thread))
    return false;
  built_version = c->conv_version;
  built_folds = folds_open;
  built_active = active;
  util_strlcpy(built_run, run, sizeof built_run);
  util_strlcpy(built_pending, pending, sizeof built_pending);
  util_strlcpy(built_thread, thread, sizeof built_thread);
  layout(c, t);
  return true;
}

float convview_height(void) { return height; }

void convview_draw(const T3Thread *t, float top, float view_h, float scroll, int64_t now) {
  float off = top - scroll, lo = scroll - 2, hi = scroll + view_h + 2;
  for (int i = 0; i < op_count; i++) {
    const Op *o = &ops[i];
    if (o->y + o->h < lo || o->y > hi) continue;
    float y = o->y + off;
    switch (o->kind) {
      case OP_TEXT:
        gfx_text(o->x, y, o->scale, o->color, arena + o->str, o->len);
        if (o->flag) gfx_text(o->x + 0.6f, y, o->scale, o->color, arena + o->str, o->len);
        break;
      case OP_RECT: gfx_rect(o->x, y, o->w, o->h, o->color); break;
      case OP_RRECT: gfx_rrect(o->x, y, o->w, o->h, o->scale, o->color); break;
      case OP_DOT: gfx_dot(o->x + o->scale, y + o->scale, o->scale, o->color); break;
      case OP_CHEVRON:
        if (o->flag) gfx_tri(o->x, y + 1, o->x + o->w, y + 1, o->x + o->w / 2, y + o->h - 1, o->color);
        else gfx_tri(o->x + 1, y, o->x + 1, y + o->h, o->x + o->w - 1, y + o->h / 2, o->color);
        break;
    }
  }
  if (live_y >= 0 && live_y + 20 > lo && live_y < hi) {
    float y = live_y + off, lh = gfx_line_h(SCALE_SMALL);
    char line[64], dur[24];
    if (live_waiting) {
      snprintf(line, sizeof line, "Waiting for you");
    } else if (live_since && now > live_since) {
      fmt_duration(now - live_since, dur, sizeof dur);
      snprintf(line, sizeof line, "Working for %s", dur);
    } else {
      snprintf(line, sizeof line, "Working...");
    }
    gfx_dot(X0 + 4, y + lh / 2, 3, live_waiting ? COL_ATTENTION : COL_RUNNING);
    gfx_text(X0 + 13, y, SCALE_SMALL, live_waiting ? COL_ATTENTION : COL_MUTED, line, -1);
  }
  (void)t;
}

void convview_toggle_folds(void) { folds_open = !folds_open; }

bool convview_folds_open(void) { return folds_open; }

int convview_fold_count(void) { return fold_count; }

int convview_turn_count(void) { return turn_count; }

float convview_turn_y(int i) { return i >= 0 && i < turn_count ? turns[i].y : 0; }

const char *convview_turn_text(int i) { return i >= 0 && i < turn_count ? arena + turns[i].str : ""; }
