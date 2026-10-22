#pragma once

#include "json.h"

#define DIFF_MAX_FILES 128
#define DIFF_TOTAL_BYTES (256 * 1024)
#define DIFF_FILE_BYTES (64 * 1024)
#define DIFF_FILE_LINES 8192
#define DIFF_TOTAL_LINES 32768

typedef struct {
  char path[512];
  int additions, deletions;
  char *text;
  uint32_t *lines;
  int line_count;
  bool truncated;
} DiffFile;

typedef struct {
  DiffFile files[DIFF_MAX_FILES];
  int count;
  size_t bytes;
  int line_count;
  bool truncated, has_source;
} DiffPreview;

void diff_clear(DiffPreview *diff);
bool diff_parse_preview(JsonCur *c, DiffPreview *diff);
