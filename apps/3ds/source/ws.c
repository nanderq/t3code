#include "ws.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <zlib.h>

#include "util.h"

#define WS_IO_TIMEOUT_MS 10000
// Bytes read per ws_poll call, so one big snapshot does not stall a frame.
#define WS_READ_BUDGET (512 * 1024)
// Inflated size cap when the caller set no message_limit.
#define WS_MAX_INFLATED (24u * 1024u * 1024u)

static void ensure(uint8_t **buf, size_t *cap, size_t need) {
  if (need <= *cap) return;
  size_t c = *cap ? *cap : 64 * 1024;
  while (c < need) c *= 2;
  *buf = realloc(*buf, c);
  *cap = c;
}

bool ws_connect(Ws *ws, const NetAddr *addr, const char *path, const char *headers) {
  memset(ws, 0, sizeof *ws);
  ws->fd = net_connect(addr, 8000, ws->err, sizeof ws->err);
  if (ws->fd < 0) return false;

  uint8_t key[16];
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = util_random();
    memcpy(key + i, &r, 4);
  }
  char key64[32];
  util_base64(key, sizeof key, key64);

  char req[4096];
  int n = snprintf(req, sizeof req,
                   "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: T3Code-3DS\r\n"
                   // Shell snapshots are megabytes of JSON; deflate cuts what the slow radio carries ~9x.
                   "Sec-WebSocket-Extensions: permessage-deflate\r\n%s\r\n",
                   path, addr->host, addr->port, key64, headers ? headers : "");
  if (n <= 0 || n >= (int)sizeof req || !net_send_all(ws->fd, req, (size_t)n, WS_IO_TIMEOUT_MS)) {
    snprintf(ws->err, sizeof ws->err, "Failed to send upgrade request");
    ws_close(ws);
    return false;
  }

  // Read the response head byte-wise-ish; frames may follow it immediately.
  char head[8192];
  size_t hl = 0;
  char *end = NULL;
  const char *failure = "Incomplete upgrade response";
  while (!end) {
    struct pollfd pfd = {.fd = ws->fd, .events = POLLIN};
    if (hl >= sizeof head - 1) { failure = "Upgrade response headers too large"; break; }
    int ready = poll(&pfd, 1, WS_IO_TIMEOUT_MS);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) {
      failure = ready == 0 ? "Timed out waiting for upgrade response" : "Socket error waiting for upgrade response";
      break;
    }
    ssize_t r = recv(ws->fd, head + hl, sizeof head - 1 - hl, 0);
    if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (r <= 0) {
      failure = r == 0 ? "Server closed before completing upgrade" : "Socket error reading upgrade response";
      break;
    }
    hl += (size_t)r;
    head[hl] = '\0';
    end = strstr(head, "\r\n\r\n");
  }
  int status = 0;
  if (end) sscanf(head, "HTTP/1.%*d %d", &status);
  ws->status = status;
  if (status != 101) {
    if (status == 401) snprintf(ws->err, sizeof ws->err, "Session expired. Pair this 3DS again.");
    else if (status == 426) snprintf(ws->err, sizeof ws->err, "Server needs a newer client protocol.");
    else if (status) snprintf(ws->err, sizeof ws->err, "Upgrade rejected (HTTP %d)", status);
    else snprintf(ws->err, sizeof ws->err, "%s (%u bytes)", failure, (unsigned)hl);
    printf("ws: upgrade failed: status=%d bytes=%u error=%s\n", status, (unsigned)hl, ws->err);
    ws_close(ws);
    return false;
  }
  for (char *line = strstr(head, "\r\n"); line && line < end; line = strstr(line + 2, "\r\n")) {
    char *h = line + 2;
    size_t n = strcspn(h, "\r");
    if (strncasecmp(h, "sec-websocket-extensions:", 25) || !util_memmem(h, n, "permessage-deflate", 18)) continue;
    z_stream *z = calloc(1, sizeof *z);
    if (!z || inflateInit2(z, -MAX_WBITS) != Z_OK) {
      free(z);
      snprintf(ws->err, sizeof ws->err, "Out of memory for compression");
      ws_close(ws);
      return false;
    }
    ws->inflater = z;
    ws->inflate_reset = util_memmem(h, n, "server_no_context_takeover", 26) != NULL;
  }
  size_t used = (size_t)(end + 4 - head);
  if (hl > used) {
    ensure(&ws->rbuf, &ws->rcap, hl - used);
    memcpy(ws->rbuf, head + used, hl - used);
    ws->rlen = hl - used;
  }
  net_set_nonblocking(ws->fd, true);
  return true;
}

static bool send_frame(Ws *ws, uint8_t opcode, const void *data, size_t len) {
  uint8_t hdr[14];
  size_t hn = 0;
  hdr[hn++] = 0x80 | opcode;
  if (len < 126) {
    hdr[hn++] = 0x80 | (uint8_t)len;
  } else if (len < 65536) {
    hdr[hn++] = 0x80 | 126;
    hdr[hn++] = (uint8_t)(len >> 8);
    hdr[hn++] = (uint8_t)len;
  } else {
    hdr[hn++] = 0x80 | 127;
    for (int i = 7; i >= 0; i--) hdr[hn++] = (uint8_t)((uint64_t)len >> (i * 8));
  }
  uint32_t m = util_random();
  uint8_t mask[4];
  memcpy(mask, &m, 4);
  memcpy(hdr + hn, mask, 4);
  hn += 4;
  uint8_t frame[1024];
  const uint8_t *src = data;
  size_t off = 0;
  size_t n = len < sizeof frame - hn ? len : sizeof frame - hn;
  memcpy(frame, hdr, hn);
  for (size_t i = 0; i < n; i++) frame[hn + i] = src[i] ^ mask[i & 3];
  bool ok = net_send_all(ws->fd, frame, hn + n, WS_IO_TIMEOUT_MS);
  off += n;
  while (ok && off < len) {
    n = len - off;
    if (n > sizeof frame) n = sizeof frame;
    for (size_t i = 0; i < n; i++) frame[i] = src[off + i] ^ mask[(off + i) & 3];
    ok = net_send_all(ws->fd, frame, n, WS_IO_TIMEOUT_MS);
    off += n;
  }
  if (!ok) {
    ws->closed = true;
    snprintf(ws->err, sizeof ws->err, "Connection lost while sending");
  }
  return ok;
}

bool ws_ping(Ws *ws) {
  return !ws->closed && ws->fd >= 0 && send_frame(ws, 0x9, NULL, 0);
}

bool ws_send_text(Ws *ws, const char *data, size_t len) {
  if (ws->closed || ws->fd < 0) return false;
  return send_frame(ws, 0x1, data, len);
}

static void append_msg(Ws *ws, const uint8_t *p, size_t n) {
  ensure((uint8_t **)&ws->msg, &ws->mcap, ws->mlen + n + 1);
  memcpy(ws->msg + ws->mlen, p, n);
  ws->mlen += n;
}

// RFC 7692: a compressed message is a raw deflate stream missing its final
// empty block, so the 00 00 FF FF tail is appended back before inflating.
static bool inflate_message(Ws *ws, char **out, size_t *out_len) {
  z_stream *z = ws->inflater;
  ensure((uint8_t **)&ws->msg, &ws->mcap, ws->mlen + 5);
  memcpy(ws->msg + ws->mlen, "\x00\x00\xff\xff", 4);
  size_t limit = ws->message_limit ? ws->message_limit : WS_MAX_INFLATED;
  z->next_in = (Bytef *)ws->msg;
  z->avail_in = (uInt)(ws->mlen + 4);
  size_t len = 0;
  for (;;) {
    if (ws->zcap < len + 64 * 1024 + 1) {
      if (len >= limit) return false;
      ensure((uint8_t **)&ws->zbuf, &ws->zcap, len + 64 * 1024 + 1);
    }
    z->next_out = (Bytef *)ws->zbuf + len;
    z->avail_out = (uInt)(ws->zcap - 1 - len);
    int rc = inflate(z, Z_SYNC_FLUSH);
    len = ws->zcap - 1 - z->avail_out;
    if (rc == Z_STREAM_END) {
      // A final block ends the stream; the next message needs a fresh one.
      inflateReset(z);
      break;
    }
    if (rc != Z_OK && rc != Z_BUF_ERROR) return false;
    if (z->avail_in == 0 && z->avail_out > 0) break;
    if (rc == Z_BUF_ERROR && z->avail_out > 0) return false;
  }
  if (len > limit) return false;
  if (ws->inflate_reset) inflateReset(z);
  *out = ws->zbuf;
  *out_len = len;
  return true;
}

bool ws_reserve(Ws *ws, size_t message_limit) {
  uint8_t *raw = malloc(message_limit + 65536);
  char *msg = malloc(message_limit + 1);
  if (!raw || !msg || ws->rlen > message_limit + 65536 || ws->mlen > message_limit) {
    free(raw);
    free(msg);
    return false;
  }
  if (ws->rlen) memcpy(raw, ws->rbuf, ws->rlen);
  if (ws->mlen) memcpy(msg, ws->msg, ws->mlen);
  free(ws->rbuf);
  free(ws->msg);
  ws->rbuf = raw;
  ws->msg = msg;
  ws->rcap = message_limit + 65536;
  ws->mcap = message_limit + 1;
  ws->message_limit = message_limit;
  return true;
}

bool ws_poll(Ws *ws, WsMessageFn fn, void *ud) {
  if (ws->closed || ws->fd < 0) return false;
  size_t budget = WS_READ_BUDGET;
  while (budget > 0) {
    if (!ws->message_limit) ensure(&ws->rbuf, &ws->rcap, ws->rlen + 64 * 1024);
    if (ws->rlen == ws->rcap) break;
    size_t want = ws->rcap - ws->rlen;
    if (want > budget) want = budget;
    ssize_t n = recv(ws->fd, ws->rbuf + ws->rlen, want, 0);
    if (n > 0) {
      ws->received_bytes += (size_t)n;
      ws->rlen += (size_t)n;
      budget -= (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
    ws->closed = true;
    snprintf(ws->err, sizeof ws->err, "Server closed the connection");
    break;
  }

  size_t off = 0;
  for (;;) {
    uint8_t *p = ws->rbuf + off;
    size_t avail = ws->rlen - off;
    if (avail < 2) break;
    bool fin = p[0] & 0x80;
    uint8_t op = p[0] & 0x0F;
    bool masked = p[1] & 0x80;
    uint64_t plen = p[1] & 0x7F;
    size_t hn = 2;
    if (plen == 126) {
      if (avail < 4) break;
      plen = ((uint64_t)p[2] << 8) | p[3];
      hn = 4;
    } else if (plen == 127) {
      if (avail < 10) break;
      plen = 0;
      for (int i = 0; i < 8; i++) plen = (plen << 8) | p[2 + i];
      hn = 10;
    }
    if (masked) hn += 4;
    if (plen > SIZE_MAX - hn || (ws->message_limit && plen > ws->message_limit)) {
      ws->closed = true;
      util_strlcpy(ws->err, "WebSocket frame too large", sizeof ws->err);
      break;
    }
    if (avail < hn + plen) break;
    uint8_t *payload = p + hn;
    if (masked) {
      for (uint64_t i = 0; i < plen; i++) payload[i] ^= p[hn - 4 + (i & 3)];
    }
    off += hn + (size_t)plen;

    if (op == 0x8) {
      if (plen >= 2) {
        ws->close_code = ((unsigned)payload[0] << 8) | payload[1];
        size_t n = (size_t)plen - 2;
        if (n >= sizeof ws->close_reason) n = sizeof ws->close_reason - 1;
        memcpy(ws->close_reason, payload + 2, n);
        ws->close_reason[n] = '\0';
      }
      ws->closed = true;
      snprintf(ws->err, sizeof ws->err, "Server closed the connection");
      break;
    } else if (op == 0x9) {
      send_frame(ws, 0xA, payload, (size_t)plen);
    } else if (op == 0x1 || op == 0x2 || op == 0x0) {
      if (op != 0x0) {
        ws->mlen = 0;
        ws->message_opcode = op;
        ws->message_compressed = ws->inflater && (p[0] & 0x40);
      }
      if (ws->message_limit && plen > ws->message_limit - ws->mlen) {
        ws->closed = true;
        util_strlcpy(ws->err, "WebSocket message too large", sizeof ws->err);
        break;
      }
      append_msg(ws, payload, (size_t)plen);
      if (fin) {
        char *data = ws->msg;
        size_t dlen = ws->mlen;
        if (ws->message_compressed && !inflate_message(ws, &data, &dlen)) {
          ws->closed = true;
          util_strlcpy(ws->err, "Could not decompress a server message", sizeof ws->err);
          break;
        }
        data[dlen] = '\0';
        ws->binary = ws->message_opcode == 0x2;
        fn(ud, data, dlen);
        ws->mlen = 0;
        // Drop very large reassembly buffers once used.
        if (ws->mcap > 4 * 1024 * 1024) {
          free(ws->msg);
          ws->msg = NULL;
          ws->mcap = 0;
        }
        if (ws->zcap > 4 * 1024 * 1024) {
          free(ws->zbuf);
          ws->zbuf = NULL;
          ws->zcap = 0;
        }
      }
    }
  }
  if (off > 0) {
    memmove(ws->rbuf, ws->rbuf + off, ws->rlen - off);
    ws->rlen -= off;
  }
  return !ws->closed;
}

void ws_close(Ws *ws) {
  if (ws->fd >= 0) {
    if (!ws->closed && ws->status == 101) send_frame(ws, 0x8, NULL, 0);
    net_close(ws->fd);
  }
  ws->fd = -1;
  ws->closed = true;
  free(ws->rbuf);
  free(ws->msg);
  free(ws->zbuf);
  if (ws->inflater) {
    inflateEnd(ws->inflater);
    free(ws->inflater);
  }
  ws->rbuf = NULL;
  ws->msg = NULL;
  ws->zbuf = NULL;
  ws->inflater = NULL;
  ws->rlen = ws->rcap = ws->mlen = ws->mcap = ws->zcap = 0;
}
