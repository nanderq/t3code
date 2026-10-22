#include "pairlink.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Finds `name` in an `a=b&c=d` list and stores its URLSearchParams-decoded value.
static bool find_param(const char *s, size_t n, const char *name, char *out, size_t cap) {
  size_t name_len = strlen(name);
  const char *end = s + n;
  while (s < end) {
    const char *amp = memchr(s, '&', (size_t)(end - s));
    const char *pair_end = amp ? amp : end;
    if ((size_t)(pair_end - s) > name_len && !memcmp(s, name, name_len) && s[name_len] == '=') {
      size_t o = 0;
      for (const char *p = s + name_len + 1; p < pair_end && o + 1 < cap; p++) {
        int hi, lo;
        if (*p == '%' && pair_end - p >= 3 && (hi = hex_value(p[1])) >= 0 && (lo = hex_value(p[2])) >= 0) {
          out[o++] = (char)(hi * 16 + lo);
          p += 2;
        } else {
          out[o++] = *p == '+' ? ' ' : *p;
        }
      }
      out[o] = '\0';
      while (o > 0 && isspace((unsigned char)out[o - 1])) out[--o] = '\0';
      return o > 0;
    }
    s = amp ? amp + 1 : end;
  }
  return false;
}

PairLinkResult pairlink_parse(const char *link, char *addr, size_t addr_cap, char *code, size_t code_cap) {
  while (isspace((unsigned char)*link)) link++;
  size_t len = strlen(link);
  while (len > 0 && isspace((unsigned char)link[len - 1])) len--;

  const char *hash = memchr(link, '#', len);
  const char *query = memchr(link, '?', hash ? (size_t)(hash - link) : len);
  size_t query_len = query ? (size_t)((hash ? hash : link + len) - query - 1) : 0;

  bool has_code = (hash && find_param(hash + 1, (size_t)(link + len - hash - 1), "token", code, code_cap)) ||
                  (query && find_param(query + 1, query_len, "token", code, code_cap));
  if (!has_code) return PAIRLINK_NOT_T3;

  // Hosted-app links name the endpoint in `host`; direct links are the endpoint.
  char hosted[256];
  const char *base = link;
  size_t base_len = len;
  if (query && find_param(query + 1, query_len, "host", hosted, sizeof hosted)) {
    base = hosted;
    base_len = strlen(hosted);
  }
  if (base_len >= 8 && !strncasecmp(base, "https://", 8)) return PAIRLINK_HTTPS;
  if (base_len < 7 || strncasecmp(base, "http://", 7)) return PAIRLINK_NOT_T3;
  base += 7;
  base_len -= 7;

  size_t host_len = 0;
  while (host_len < base_len && !strchr("/?#", base[host_len])) host_len++;
  if (host_len == 0 || host_len + 4 > addr_cap) return PAIRLINK_NOT_T3;
  memcpy(addr, base, host_len);
  addr[host_len] = '\0';
  // URL serialization drops the default port, while a bare address means 3773 here.
  if (!memchr(addr, ':', host_len)) memcpy(addr + host_len, ":80", 4);
  return PAIRLINK_OK;
}
