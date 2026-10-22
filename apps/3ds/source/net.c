#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

bool net_parse_addr(const char *text, NetAddr *addr) {
  while (*text == ' ') text++;
  if (strncmp(text, "https://", 8) == 0) return false;
  if (strncmp(text, "http://", 7) == 0) text += 7;
  size_t n = strcspn(text, "/?# ");
  const char *colon = memchr(text, ':', n);
  size_t hostlen = colon ? (size_t)(colon - text) : n;
  if (hostlen == 0 || hostlen >= sizeof addr->host) return false;
  memcpy(addr->host, text, hostlen);
  addr->host[hostlen] = '\0';
  addr->port = colon ? atoi(colon + 1) : 3773;
  return addr->port > 0 && addr->port < 65536;
}

void net_set_nonblocking(int fd, bool on) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

int net_connect(const NetAddr *addr, int timeout_ms, char *err, size_t errcap) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char port[8];
  snprintf(port, sizeof port, "%d", addr->port);
  struct addrinfo *res = NULL;
  if (getaddrinfo(addr->host, port, &hints, &res) != 0 || !res) {
    snprintf(err, errcap, "Could not resolve %s", addr->host);
    return -1;
  }
  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) {
    freeaddrinfo(res);
    snprintf(err, errcap, "socket() failed (%d)", errno);
    return -1;
  }
  net_set_nonblocking(fd, true);
  int rc = connect(fd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (rc != 0 && errno != EINPROGRESS) {
    snprintf(err, errcap, "Connection refused by %s:%d", addr->host, addr->port);
    close(fd);
    return -1;
  }
  if (rc != 0) {
    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0) {
      snprintf(err, errcap, "Timed out reaching %s:%d", addr->host, addr->port);
      close(fd);
      return -1;
    }
    // The 3DS socket service doesn't report the result through SO_ERROR and poll
    // flags the way BSD does; only a connected socket has a peer.
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof peer;
    if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) != 0) {
      printf("net: connect to %s:%d failed, revents=0x%x errno=%d\n", addr->host, addr->port, pfd.revents, errno);
      snprintf(err, errcap, "Could not connect to %s:%d", addr->host, addr->port);
      close(fd);
      return -1;
    }
    int soerr = 0;
    socklen_t len = sizeof soerr;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
    printf("net: connected to %s:%d, revents=0x%x so_error=%d\n", addr->host, addr->port, pfd.revents, soerr);
  }
  net_set_nonblocking(fd, false);
  return fd;
}

bool net_send_all(int fd, const void *buf, size_t len, int timeout_ms) {
  const char *p = buf;
  while (len > 0) {
    ssize_t n = send(fd, p, len, 0);
    if (n > 0) {
      p += n;
      len -= (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      struct pollfd pfd = {.fd = fd, .events = POLLOUT};
      if (poll(&pfd, 1, timeout_ms) <= 0) return false;
      continue;
    }
    return false;
  }
  return true;
}

void net_close(int fd) {
  if (fd >= 0) close(fd);
}
