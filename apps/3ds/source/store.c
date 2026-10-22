#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

bool store_load(Store *s, const char *path) {
  memset(s, 0, sizeof *s);
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > 256 * 1024) {
    fclose(f);
    return false;
  }
  char *buf = malloc((size_t)n);
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  JsonCur c;
  json_init(&c, buf, got);
  if (json_obj_begin(&c)) {
    JStr key;
    while (json_obj_next(&c, &key)) {
      if (jstr_eq(key, "lastUsed")) s->last_used = (int)json_int(&c);
      else if (jstr_eq(key, "environments") && json_arr_begin(&c)) {
        while (json_arr_next(&c)) {
          if (s->count >= MAX_ENVS || !json_obj_begin(&c)) {
            json_skip(&c);
            continue;
          }
          EnvConfig *e = &s->envs[s->count];
          JStr k;
          while (json_obj_next(&c, &k)) {
            if (jstr_eq(k, "label")) json_str(&c, e->label, sizeof e->label);
            else if (jstr_eq(k, "address")) json_str(&c, e->addr, sizeof e->addr);
            else if (jstr_eq(k, "token")) json_str(&c, e->token, sizeof e->token);
            else if (jstr_eq(k, "environmentId")) json_str(&c, e->environment_id, sizeof e->environment_id);
            else json_skip(&c);
          }
          if (e->addr[0]) s->count++;
        }
      } else json_skip(&c);
    }
  }
  free(buf);
  if (s->last_used < 0 || s->last_used >= s->count) s->last_used = 0;
  return true;
}

bool store_save(const Store *s, const char *path) {
  JBuf b;
  jb_init(&b);
  jb_raw(&b, "{");
  jb_key(&b, "lastUsed");
  jb_int(&b, s->last_used);
  jb_key(&b, "environments");
  jb_raw(&b, "[");
  for (int i = 0; i < s->count; i++) {
    if (i) jb_raw(&b, ",");
    jb_raw(&b, "{");
    jb_kv_str(&b, "label", s->envs[i].label);
    jb_kv_str(&b, "address", s->envs[i].addr);
    jb_kv_str(&b, "token", s->envs[i].token);
    jb_kv_str(&b, "environmentId", s->envs[i].environment_id);
    jb_raw(&b, "}");
  }
  jb_raw(&b, "]}\n");
  // Write then rename so a crash mid-write cannot lose every pairing.
  char tmp[512];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  bool ok = f && fwrite(b.p, 1, b.n, f) == b.n;
  if (f) fclose(f);
  jb_free(&b);
  if (!ok) return false;
  remove(path);
  return rename(tmp, path) == 0;
}

void store_remove(Store *s, int index) {
  if (index < 0 || index >= s->count) return;
  memmove(&s->envs[index], &s->envs[index + 1], sizeof(EnvConfig) * (size_t)(s->count - index - 1));
  s->count--;
  memset(&s->envs[s->count], 0, sizeof(EnvConfig));
  if (s->last_used >= s->count) s->last_used = s->count ? s->count - 1 : 0;
}

int store_upsert(Store *s, const EnvConfig *env) {
  for (int i = 0; i < s->count; i++) {
    if (env->environment_id[0] && strcmp(s->envs[i].environment_id, env->environment_id) == 0) {
      s->envs[i] = *env;
      return i;
    }
  }
  if (s->count >= MAX_ENVS) return -1;
  s->envs[s->count] = *env;
  return s->count++;
}
