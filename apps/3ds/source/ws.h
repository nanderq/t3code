#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net.h"

typedef struct {
  int fd;
  uint8_t *rbuf;  // raw bytes not yet parsed into frames
  size_t rlen, rcap;
  char *msg;  // reassembly buffer for fragmented messages
  size_t mlen, mcap;
  bool closed;
  bool binary;
  uint8_t message_opcode;
  bool message_compressed;
  void *inflater;  // z_stream when permessage-deflate was negotiated, else NULL
  bool inflate_reset;  // server_no_context_takeover: every message starts a fresh window
  char *zbuf;  // inflated message
  size_t zcap;
  unsigned close_code;
  char close_reason[124];
  size_t message_limit;
  uint64_t received_bytes;
  int status;  // HTTP status of the upgrade response
  char err[160];
} Ws;

typedef void (*WsMessageFn)(void *ud, char *data, size_t len);

// Blocking TCP connect and upgrade. On success the socket is left
// non-blocking for ws_poll.
bool ws_connect(Ws *ws, const NetAddr *addr, const char *path, const char *headers);
bool ws_ping(Ws *ws);
bool ws_send_text(Ws *ws, const char *data, size_t len);
// Reads whatever is available without blocking and delivers complete text
// messages. `data` is NUL-terminated and only valid during the callback.
// Returns false once the connection is gone.
bool ws_poll(Ws *ws, WsMessageFn fn, void *ud);
bool ws_reserve(Ws *ws, size_t message_limit);
void ws_close(Ws *ws);
