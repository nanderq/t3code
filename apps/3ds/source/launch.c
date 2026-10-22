#include "launch.h"

#include <ctype.h>
#include <string.h>

#include "util.h"

void launch_parse_refs(JsonCur *c, LaunchRefs *refs) {
  memset(refs, 0, sizeof *refs);
  if (!json_obj_begin(c)) return;
  JStr key;
  while (json_obj_next(c, &key)) {
    if (jstr_eq(key, "isRepo")) refs->is_repo = json_bool(c);
    else if (jstr_eq(key, "nextCursor")) refs->has_more = !json_is_null(c), json_skip(c);
    else if (jstr_eq(key, "refs") && json_arr_begin(c)) {
      while (json_arr_next(c)) {
        LaunchRef ref = {0};
        bool remote = false;
        if (!json_obj_begin(c)) continue;
        JStr k;
        while (json_obj_next(c, &k)) {
          if (jstr_eq(k, "name")) json_str(c, ref.name, sizeof ref.name);
          else if (jstr_eq(k, "current")) ref.current = json_bool(c);
          else if (jstr_eq(k, "isDefault")) ref.is_default = json_bool(c);
          else if (jstr_eq(k, "isRemote")) remote = json_bool(c);
          else json_skip(c);
        }
        if (!remote && ref.name[0] && refs->count < LAUNCH_MAX_REFS) refs->refs[refs->count++] = ref;
      }
    } else json_skip(c);
  }
  for (int i = 0; i < refs->count; i++) {
    if (refs->refs[i].is_default) refs->preferred = i;
  }
  for (int i = 0; i < refs->count; i++) {
    if (refs->refs[i].current) refs->preferred = i;
  }
}

void launch_workspace_json(JBuf *b, const LaunchWorkspace *workspace) {
  if (!workspace) {
    jb_raw(b, "{\"type\":\"root\"}");
    return;
  }
  jb_raw(b, "{\"type\":\"worktree\"");
  jb_kv_str(b, "baseRef", workspace->base_ref);
  jb_kv_str(b, "branch", workspace->branch);
  jb_raw(b, ",\"startFromOrigin\":false}");
}

void launch_branch_seed(const char *text, char *out, size_t cap) {
  util_strlcpy(out, "t3/", cap);
  size_t n = strlen(out);
  bool dash = false;
  for (const unsigned char *p = (const unsigned char *)text; *p && n + 1 < cap && n < 48; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) {
      if (dash && n > 3 && n + 2 < cap) out[n++] = '-';
      dash = false;
      out[n++] = (char)tolower(*p);
    } else dash = true;
  }
  out[n] = '\0';
  if (n == 3) util_strlcpy(out, "t3/new-thread", cap);
}
