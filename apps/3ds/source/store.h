#pragma once

#include <stdbool.h>

#define MAX_ENVS 8

// One paired environment. The bearer token is a 30-day session credential
// minted from a one-time pairing code.
typedef struct {
  char label[64];
  char addr[160];
  char token[2048];
  char environment_id[80];
} EnvConfig;

typedef struct {
  EnvConfig envs[MAX_ENVS];
  int count;
  int last_used;
} Store;

bool store_load(Store *s, const char *path);
bool store_save(const Store *s, const char *path);
void store_remove(Store *s, int index);
// Replaces an entry for the same environment id, otherwise appends.
int store_upsert(Store *s, const EnvConfig *env);
