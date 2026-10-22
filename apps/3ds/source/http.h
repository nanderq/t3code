#pragma once

#include <stddef.h>
#include <stdint.h>

#include "net.h"

typedef struct {
  int status;  // 0 when the request never got a response
  char *body;  // NUL-terminated, caller frees with http_resp_free
  size_t len;
  int64_t server_time;  // epoch seconds from the Date header, 0 if absent
  char err[160];
} HttpResp;

// One blocking HTTP/1.1 request with Connection: close. `headers` holds extra
// header lines, each ending in \r\n, or NULL.
int http_request(const NetAddr *addr, const char *method, const char *path, const char *headers,
                 const char *body, size_t body_len, HttpResp *out);
void http_resp_free(HttpResp *r);

// Parses an RFC 7231 date ("Tue, 20 Oct 2026 12:00:00 GMT") to epoch seconds.
int64_t http_parse_date(const char *s);
