#pragma once

#include <stddef.h>

typedef enum {
  PAIRLINK_OK,
  PAIRLINK_NOT_T3,  // Not a pairing link, such as some other QR code.
  PAIRLINK_HTTPS,   // A pairing link for an HTTPS-only endpoint, which the 3DS can't reach.
} PairLinkResult;

// Reads a pairing link scanned from a QR code: either the host's own
// `http://<host>:<port>/pair#token=<code>` or a hosted-app link that carries the
// endpoint in its `host` parameter. On success `addr` is `host:port`.
PairLinkResult pairlink_parse(const char *link, char *addr, size_t addr_cap, char *code, size_t code_cap);
