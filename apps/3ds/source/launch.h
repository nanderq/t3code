#pragma once

#include "json.h"

#define LAUNCH_MAX_REFS 24

typedef struct {
  char name[256];
  bool current, is_default;
} LaunchRef;

typedef struct {
  LaunchRef refs[LAUNCH_MAX_REFS];
  int count, preferred;
  bool is_repo, has_more;
} LaunchRefs;

typedef struct {
  const char *branch;
  const char *base_ref;
} LaunchWorkspace;

void launch_parse_refs(JsonCur *c, LaunchRefs *refs);
void launch_workspace_json(JBuf *b, const LaunchWorkspace *workspace);
void launch_branch_seed(const char *text, char *out, size_t cap);
