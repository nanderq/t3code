#include "http.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <zlib.h>

#include "util.h"

#define HTTP_TIMEOUT_MS 15000
// Bounded snapshots are budgeted server-side; anything past this is a bug.
#define HTTP_MAX_BODY (24u * 1024u * 1024u)

int64_t http_parse_date(const char *s) {
  static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0};
  int day, year, hh, mm, ss;
  const char *comma = strchr(s, ',');
  if (comma) s = comma + 1;
  if (sscanf(s, " %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return 0;
  const char *m = strstr(months, mon);
  if (!m) return 0;
  return util_days_from_civil(year, (int)(m - months) / 3 + 1, day) * 86400LL + hh * 3600 + mm * 60 + ss;
}

void http_resp_free(HttpResp *r) {
  free(r->body);
  r->body = NULL;
  r->len = 0;
}

// JSON shrinks about 9x under gzip, and the 3DS radio is the bottleneck, so
// inflating on the console is far cheaper than receiving the plain body.
static char *gunzip(const char *in, size_t in_len, size_t *out_len) {
  z_stream z;
  memset(&z, 0, sizeof z);
  if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return NULL;
  size_t cap = in_len * 8 + 1024, len = 0;
  if (cap > HTTP_MAX_BODY) cap = HTTP_MAX_BODY;
  char *out = malloc(cap + 1);
  z.next_in = (Bytef *)in;
  z.avail_in = (uInt)in_len;
  int rc = Z_OK;
  while (out && rc == Z_OK) {
    if (len == cap) {
      if (cap >= HTTP_MAX_BODY) break;
      cap = cap * 2 > HTTP_MAX_BODY ? HTTP_MAX_BODY : cap * 2;
      char *grown = realloc(out, cap + 1);
      if (!grown) break;
      out = grown;
    }
    z.next_out = (Bytef *)out + len;
    z.avail_out = (uInt)(cap - len);
    rc = inflate(&z, Z_NO_FLUSH);
    len = cap - z.avail_out;
  }
  inflateEnd(&z);
  if (rc != Z_STREAM_END) {
    free(out);
    return NULL;
  }
  out[len] = '\0';
  *out_len = len;
  return out;
}

static size_t dechunk(char *buf, size_t len) {
  size_t in = 0, out = 0;
  while (in < len) {
    char *line_end = util_memmem(buf + in, len - in, "\r\n", 2);
    if (!line_end) break;
    size_t sz = strtoul(buf + in, NULL, 16);
    in = (size_t)(line_end - buf) + 2;
    if (sz == 0 || in + sz > len) break;
    memmove(buf + out, buf + in, sz);
    out += sz;
    in += sz + 2;
  }
  buf[out] = '\0';
  return out;
}

int http_request(const NetAddr *addr, const char *method, const char *path, const char *headers,
                 const char *body, size_t body_len, HttpResp *out) {
  memset(out, 0, sizeof *out);
  int fd = net_connect(addr, 8000, out->err, sizeof out->err);
  if (fd < 0) return -1;

  char head[4096];
  int hn = snprintf(head, sizeof head,
                    "%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: T3Code-3DS\r\nAccept: application/json\r\n"
                    "Accept-Encoding: gzip\r\n"
                    "Connection: close\r\n%sContent-Length: %u\r\n\r\n",
                    method, path, addr->host, addr->port, headers ? headers : "", (unsigned)body_len);
  if (hn <= 0 || hn >= (int)sizeof head || !net_send_all(fd, head, (size_t)hn, HTTP_TIMEOUT_MS) ||
      (body_len && !net_send_all(fd, body, body_len, HTTP_TIMEOUT_MS))) {
    snprintf(out->err, sizeof out->err, "Failed to send request");
    net_close(fd);
    return -1;
  }

  size_t cap = 64 * 1024, len = 0;
  char *buf = malloc(cap + 1);
  for (;;) {
    if (len == cap) {
      if (cap >= HTTP_MAX_BODY) break;
      cap *= 2;
      buf = realloc(buf, cap + 1);
    }
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, HTTP_TIMEOUT_MS) <= 0) {
      snprintf(out->err, sizeof out->err, "Server stopped responding");
      free(buf);
      net_close(fd);
      return -1;
    }
    ssize_t n = recv(fd, buf + len, cap - len, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    len += (size_t)n;
  }
  net_close(fd);
  buf[len] = '\0';

  char *hend = strstr(buf, "\r\n\r\n");
  if (!hend || sscanf(buf, "HTTP/1.%*d %d", &out->status) != 1) {
    snprintf(out->err, sizeof out->err, "Malformed response from server");
    free(buf);
    return -1;
  }
  *hend = '\0';
  bool chunked = false, gzipped = false;
  for (char *line = strstr(buf, "\r\n"); line && line < hend; line = strstr(line + 2, "\r\n")) {
    char *h = line + 2;
    if (strncasecmp(h, "transfer-encoding:", 18) == 0 && strstr(h, "chunked")) chunked = true;
    if (strncasecmp(h, "content-encoding:", 17) == 0 && util_memmem(h, strcspn(h, "\r"), "gzip", 4)) gzipped = true;
    if (strncasecmp(h, "date:", 5) == 0) {
      char date[64];
      size_t dl = strcspn(h + 5, "\r");
      if (dl >= sizeof date) dl = sizeof date - 1;
      memcpy(date, h + 5, dl);
      date[dl] = '\0';
      out->server_time = http_parse_date(date);
    }
  }
  char *b = hend + 4;
  size_t blen = len - (size_t)(b - buf);
  memmove(buf, b, blen);
  buf[blen] = '\0';
  if (chunked) blen = dechunk(buf, blen);
  if (gzipped) {
    char *plain = gunzip(buf, blen, &blen);
    free(buf);
    if (!plain) {
      snprintf(out->err, sizeof out->err, "Could not decompress the response");
      return -1;
    }
    buf = plain;
  }
  out->body = buf;
  out->len = blen;
  return 0;
}
