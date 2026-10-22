#include "diff.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

void diff_clear(DiffPreview *diff) {
  for (int i = 0; i < diff->count; i++) {
    free(diff->files[i].text);
    free(diff->files[i].lines);
  }
  memset(diff, 0, sizeof *diff);
}

static DiffFile *find_file(DiffPreview *diff, const char *path) {
  for (int i = 0; i < diff->count; i++) {
    if (!strcmp(diff->files[i].path, path)) return &diff->files[i];
  }
  if (diff->count == DIFF_MAX_FILES) {
    diff->truncated = true;
    return NULL;
  }
  DiffFile *file = &diff->files[diff->count++];
  util_strlcpy(file->path, path, sizeof file->path);
  return file;
}

// Git quotes paths with C escapes, including octal UTF-8 bytes.
static void git_path(const char *p, size_t n, char *out, size_t cap) {
  size_t used = 0;
  bool quoted = n && *p == '"';
  if (quoted) p++, n--;
  for (size_t i = 0; i < n && used + 1 < cap; i++) {
    unsigned char ch = (unsigned char)p[i];
    if (quoted && ch == '"') break;
    if (quoted && ch == '\\' && i + 1 < n) {
      ch = (unsigned char)p[++i];
      if (ch >= '0' && ch <= '7') {
        unsigned int v = ch - '0';
        for (int j = 0; j < 2 && i + 1 < n && p[i + 1] >= '0' && p[i + 1] <= '7'; j++)
          v = v * 8 + (unsigned int)(p[++i] - '0');
        ch = (unsigned char)v;
      } else if (ch == 't') ch = '\t';
      else if (ch == 'n') ch = '\n';
      else if (ch == 'r') ch = '\r';
    }
    if (ch) out[used++] = (char)ch;
  }
  out[used] = '\0';
  if (!quoted) {
    char *tab = strchr(out, '\t');
    if (tab) *tab = '\0';
  }
  if (used >= 2 && (out[0] == 'a' || out[0] == 'b') && out[1] == '/') memmove(out, out + 2, used - 1);
}

static void patch_path(const char *start, const char *end, char *path, size_t cap) {
  path[0] = '\0';
  for (const char *p = start; p < end;) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    const char *stop = nl ? nl : end;
    if (stop - p >= 3 && !memcmp(p, "@@ ", 3)) break;
    if (stop - p > 4 && (!memcmp(p, "+++ ", 4) || !memcmp(p, "--- ", 4))) {
      if (stop - p != 13 || memcmp(p + 4, "/dev/null", 9)) git_path(p + 4, (size_t)(stop - p - 4), path, cap);
    }
    if (!nl) break;
    p = nl + 1;
  }
  if (path[0]) return;
  const char *nl = memchr(start, '\n', (size_t)(end - start));
  const char *stop = nl ? nl : end;
  const char *split = NULL;
  for (const char *p = start + 11; stop - p >= 4; p++) {
    if (!memcmp(p, " b/", 3) || !memcmp(p, " \"b/", 4)) split = p + 1;
  }
  if (split) git_path(split, (size_t)(stop - split), path, cap);
}

static void keep_patch(DiffPreview *diff, const char *start, size_t n, bool cut) {
  char path[512];
  patch_path(start, start + n, path, sizeof path);
  if (!path[0]) return;
  DiffFile *file = find_file(diff, path);
  if (!file || file->text) return;
  size_t limit = DIFF_TOTAL_BYTES - diff->bytes;
  if (limit > DIFF_FILE_BYTES) limit = DIFF_FILE_BYTES;
  if (n > limit) n = limit, cut = true;
  int max_lines = DIFF_TOTAL_LINES - diff->line_count;
  if (max_lines > DIFF_FILE_LINES) max_lines = DIFF_FILE_LINES;
  if (!n || max_lines <= 0) {
    file->truncated = diff->truncated = true;
    return;
  }
  int lines = 1;
  for (size_t i = 0; i + 1 < n; i++) {
    if (start[i] == '\n' && ++lines > max_lines) {
      n = i + 1;
      lines = max_lines;
      cut = true;
      break;
    }
  }
  file->text = malloc(n + 1);
  if (!file->text) {
    file->truncated = diff->truncated = true;
    return;
  }
  memcpy(file->text, start, n);
  file->text[n] = '\0';
  file->lines = malloc((size_t)lines * sizeof *file->lines);
  if (!file->lines) {
    free(file->text);
    file->text = NULL;
    file->truncated = diff->truncated = true;
    return;
  }
  file->lines[file->line_count++] = 0;
  for (size_t i = 0; i < n; i++) {
    if (file->text[i] == '\n') {
      file->text[i] = '\0';
      if (i + 1 < n) file->lines[file->line_count++] = (uint32_t)(i + 1);
    }
  }
  if (!file->additions && !file->deletions) {
    bool hunk = false;
    for (int i = 0; i < file->line_count; i++) {
      const char *line = file->text + file->lines[i];
      if (!strncmp(line, "@@ ", 3)) hunk = true;
      else if (hunk && line[0] == '+') file->additions++;
      else if (hunk && line[0] == '-') file->deletions++;
    }
  }
  file->truncated = cut;
  diff->bytes += n;
  diff->line_count += file->line_count;
  if (cut) diff->truncated = true;
}

static void parse_source(JsonCur *c, DiffPreview *diff) {
  JStr patch = {0}, stats = {0};
  bool truncated = false;
  if (!json_obj_begin(c)) return;
  JStr key;
  while (json_obj_next(c, &key)) {
    if (jstr_eq(key, "diff")) json_raw(c, &patch);
    else if (jstr_eq(key, "files")) json_raw(c, &stats);
    else if (jstr_eq(key, "truncated")) truncated = json_bool(c);
    else json_skip(c);
  }
  diff->has_source = true;
  diff->truncated = truncated;
  if (stats.p) {
    JsonCur s;
    json_init(&s, stats.p, stats.n);
    if (json_arr_begin(&s)) {
      while (json_arr_next(&s)) {
        char path[512] = "";
        int additions = 0, deletions = 0;
        if (!json_obj_begin(&s)) continue;
        while (json_obj_next(&s, &key)) {
          if (jstr_eq(key, "path")) json_str(&s, path, sizeof path);
          else if (jstr_eq(key, "additions")) additions = (int)json_int(&s);
          else if (jstr_eq(key, "deletions")) deletions = (int)json_int(&s);
          else json_skip(&s);
        }
        DiffFile *file = path[0] ? find_file(diff, path) : NULL;
        if (file) file->additions = additions, file->deletions = deletions;
      }
    }
    if (s.err) c->err = true;
  }
  if (!patch.p) return;
  // Decode once with a hard ceiling; the socket's JSON stays owned by the caller.
  size_t cap = patch.n < DIFF_TOTAL_BYTES ? patch.n + 1 : DIFF_TOTAL_BYTES + 1;
  char *text = malloc(cap);
  if (!text) { diff->truncated = true; return; }
  JsonCur p;
  json_init(&p, patch.p, patch.n);
  json_str(&p, text, cap);
  size_t n = strlen(text);
  bool cut = patch.n > DIFF_TOTAL_BYTES && n + 4 >= cap;
  if (cut) diff->truncated = true;
  const char *end = text + n;
  const char *start = text;
  while (start < end) {
    if (end - start < 11 || memcmp(start, "diff --git ", 11)) {
      const char *next = memchr(start, '\n', (size_t)(end - start));
      if (!next) break;
      start = next + 1;
      continue;
    }
    const char *next = start;
    do {
      next = memchr(next, '\n', (size_t)(end - next));
      if (!next) break;
      next++;
    } while (end - next < 11 || memcmp(next, "diff --git ", 11));
    if (!next) next = end;
    keep_patch(diff, start, (size_t)(next - start), next == end && (cut || truncated));
    start = next;
  }
  if (p.err) c->err = true;
  free(text);
}

bool diff_parse_preview(JsonCur *c, DiffPreview *diff) {
  diff_clear(diff);
  if (!json_obj_begin(c)) return false;
  JStr key;
  while (json_obj_next(c, &key)) {
    if (jstr_eq(key, "sources") && json_arr_begin(c)) {
      while (json_arr_next(c)) {
        JsonCur probe = *c;
        char kind[32] = "";
        if (json_obj_begin(&probe)) {
          while (json_obj_next(&probe, &key)) {
            if (jstr_eq(key, "kind")) json_str(&probe, kind, sizeof kind);
            else json_skip(&probe);
          }
        }
        if (!strcmp(kind, "branch-range") && !diff->has_source) parse_source(c, diff);
        else json_skip(c);
      }
    } else json_skip(c);
  }
  if (c->err) { diff_clear(diff); return false; }
  return true;
}
