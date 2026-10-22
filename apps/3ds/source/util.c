#include "util.h"

#include <stdio.h>
#include <string.h>

// Howard Hinnant's civil-date algorithms; avoids timegm, which newlib lacks.
int64_t util_days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

void util_civil_from_days(int64_t z, int *y, int *m, int *d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yy = (int64_t)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yy + (*m <= 2));
}

int64_t util_parse_iso(const char *s) {
  int y, mo, d, h = 0, mi = 0, sec = 0;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3) return 0;
  int64_t t = util_days_from_civil(y, mo, d) * 86400LL + h * 3600 + mi * 60 + sec;
  // Honour an explicit offset such as +02:00; Z and missing mean UTC.
  const char *tz = strchr(s + 10, 'T');
  if (tz) {
    const char *off = tz + 1;
    while (*off && *off != 'Z' && *off != '+' && *off != '-') off++;
    int oh, om;
    if ((*off == '+' || *off == '-') && sscanf(off + 1, "%d:%d", &oh, &om) == 2)
      t -= (*off == '+' ? 1 : -1) * (oh * 3600 + om * 60);
  }
  return t;
}

void util_format_ago(int64_t then, int64_t now, char *out, size_t cap) {
  if (then <= 0) {
    out[0] = '\0';
    return;
  }
  int64_t d = now - then;
  if (d < 60) snprintf(out, cap, "now");
  else if (d < 3600) snprintf(out, cap, "%dm", (int)(d / 60));
  else if (d < 86400) snprintf(out, cap, "%dh", (int)(d / 3600));
  else if (d < 86400 * 30) snprintf(out, cap, "%dd", (int)(d / 86400));
  else snprintf(out, cap, "%dmo", (int)(d / (86400 * 30)));
}

void util_format_until(int64_t then, int64_t now, char *out, size_t cap) {
  int64_t d = then - now;
  if (then <= 0) out[0] = '\0';
  else if (d <= 0) snprintf(out, cap, "resetting");
  else if (d < 3600) snprintf(out, cap, "resets in %dm", (int)(d / 60) + 1);
  else if (d < 86400) snprintf(out, cap, "resets in %dh %dm", (int)(d / 3600), (int)(d % 3600 / 60));
  else snprintf(out, cap, "resets in %dd %dh", (int)(d / 86400), (int)(d % 86400 / 3600));
}

void util_format_tokens(double n, char *out, size_t cap) {
  if (n >= 1e9) snprintf(out, cap, "%.1fB", n / 1e9);
  else if (n >= 1e6) snprintf(out, cap, "%.1fM", n / 1e6);
  else if (n >= 1e3) snprintf(out, cap, "%.1fK", n / 1e3);
  else snprintf(out, cap, "%d", (int)n);
}

void util_base64(const uint8_t *in, size_t n, char *out) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    out[o++] = tbl[(v >> 18) & 63];
    out[o++] = tbl[(v >> 12) & 63];
    out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? tbl[v & 63] : '=';
  }
  out[o] = '\0';
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

void util_seed_random(uint64_t seed) { rng_state ^= seed * 0xBF58476D1CE4E5B9ull + 1; }

uint32_t util_random(void) {
  // xorshift64*: ids only need to be unique, not unpredictable.
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}

void util_uuid(char out[37]) {
  uint8_t b[16];
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = util_random();
    memcpy(b + i, &r, 4);
  }
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
           b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void *util_memmem(const void *hay, size_t hn, const void *needle, size_t nn) {
  if (nn == 0) return (void *)hay;
  const char *h = hay;
  for (size_t i = 0; i + nn <= hn; i++)
    if (h[i] == *(const char *)needle && memcmp(h + i, needle, nn) == 0) return (void *)(h + i);
  return NULL;
}

void util_strlcpy(char *dst, const char *src, size_t cap) {
  if (!cap) return;
  size_t n = strlen(src);
  if (n >= cap) {
    n = cap - 1;
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

size_t util_utf8_decode(const char *s, uint32_t *cp) {
  const unsigned char *u = (const unsigned char *)s;
  if (u[0] < 0x80) {
    *cp = u[0];
    return 1;
  }
  if ((u[0] & 0xE0) == 0xC0 && u[1]) {
    *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    return 2;
  }
  if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
    *cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    return 3;
  }
  if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
    *cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) |
          ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
    return 4;
  }
  *cp = 0xFFFD;
  return 1;
}
