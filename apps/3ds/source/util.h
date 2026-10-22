#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int64_t util_days_from_civil(int y, int m, int d);
void util_civil_from_days(int64_t z, int *y, int *m, int *d);
// "2026-10-20T12:34:56.789Z" -> epoch seconds, 0 when unparseable.
int64_t util_parse_iso(const char *s);
// "5m", "3h", "2d" relative to now.
void util_format_ago(int64_t then, int64_t now, char *out, size_t cap);
// "in 2h 10m" style countdown.
void util_format_until(int64_t then, int64_t now, char *out, size_t cap);
void util_format_tokens(double n, char *out, size_t cap);

void util_base64(const uint8_t *in, size_t n, char *out);
// RFC 4122 v4 UUID text.
void util_uuid(char out[37]);
void util_seed_random(uint64_t seed);
uint32_t util_random(void);

void *util_memmem(const void *hay, size_t hn, const void *needle, size_t nn);
// Copies src into dst, truncating on a UTF-8 boundary.
void util_strlcpy(char *dst, const char *src, size_t cap);
size_t util_utf8_decode(const char *s, uint32_t *cp);
