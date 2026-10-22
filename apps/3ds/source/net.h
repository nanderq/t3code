#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Endpoint of one environment as the user entered it: host[:port], plain HTTP.
typedef struct {
  char host[128];
  int port;
} NetAddr;

// Parses "host", "host:port", or "http://host:port/..." into addr. Returns
// false for https:// because the 3DS client speaks plain HTTP only.
bool net_parse_addr(const char *text, NetAddr *addr);

// Blocking connect with a timeout. Returns a socket or -1 with err filled.
int net_connect(const NetAddr *addr, int timeout_ms, char *err, size_t errcap);
bool net_send_all(int fd, const void *buf, size_t len, int timeout_ms);
void net_set_nonblocking(int fd, bool on);
void net_close(int fd);
