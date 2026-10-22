#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A zero-allocation pull reader. Snapshots can be several megabytes, so the
// client walks them in place and copies out only the fields it keeps instead
// of building a tree.
typedef struct {
  const char *p;
  const char *end;
  bool err;
} JsonCur;

typedef struct {
  const char *p;
  size_t n;
} JStr;

typedef enum { J_NONE, J_OBJ, J_ARR, J_STR, J_NUM, J_TRUE, J_FALSE, J_NULL } JType;

void json_init(JsonCur *c, const char *data, size_t len);
JType json_peek(JsonCur *c);

// Object iteration: call json_obj_begin, then loop while json_obj_next returns
// true. The caller must consume (read or skip) each value before the next call.
bool json_obj_begin(JsonCur *c);
bool json_obj_next(JsonCur *c, JStr *key);
bool json_arr_begin(JsonCur *c);
bool json_arr_next(JsonCur *c);

void json_skip(JsonCur *c);
// Copies the next value's exact source text, for passing objects through.
bool json_raw(JsonCur *c, JStr *out);
// Decodes a string (escapes included) into out, truncating on a UTF-8
// boundary. Non-string values are skipped and produce "".
bool json_str(JsonCur *c, char *out, size_t cap);
// Like json_str but heap-allocates the full value. Caller frees.
char *json_str_dup(JsonCur *c);
double json_num(JsonCur *c);
int64_t json_int(JsonCur *c);
bool json_bool(JsonCur *c);
bool json_is_null(JsonCur *c);

bool jstr_eq(JStr s, const char *lit);

// Growable output buffer used to build request payloads.
typedef struct {
  char *p;
  size_t n;
  size_t cap;
} JBuf;

void jb_init(JBuf *b);
void jb_free(JBuf *b);
void jb_raw(JBuf *b, const char *s);
void jb_rawn(JBuf *b, const char *s, size_t n);
void jb_str(JBuf *b, const char *s);
void jb_strn(JBuf *b, const char *s, size_t n);
void jb_int(JBuf *b, int64_t v);
// "key": prefixed by a comma unless it follows an opening brace.
void jb_key(JBuf *b, const char *key);
void jb_kv_str(JBuf *b, const char *key, const char *val);
