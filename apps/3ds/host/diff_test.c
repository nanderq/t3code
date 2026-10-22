#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/diff.h"
#include "../source/launch.h"

static DiffPreview diff;

static bool parse(const char *json) {
  JsonCur c;
  json_init(&c, json, strlen(json));
  return diff_parse_preview(&c, &diff);
}

static void parse_patch(const char *patch, const char *stats, bool truncated) {
  JBuf b;
  jb_init(&b);
  jb_raw(&b, "{\"sources\":[{\"kind\":\"branch-range\",\"diff\":");
  jb_str(&b, patch);
  if (stats) { jb_raw(&b, ",\"files\":"); jb_raw(&b, stats); }
  jb_raw(&b, truncated ? ",\"truncated\":true}]}" : ",\"truncated\":false}]}");
  assert(parse(b.p));
  jb_free(&b);
}

static void sample_preview(void) {
  const char *patch =
    "diff --git a/src/main.c b/src/main.c\n"
    "--- a/src/main.c\n+++ b/src/main.c\n@@ -1,2 +1,2 @@\n context\n-old\n+new\n"
    "diff --git a/old.txt b/new name.txt\n"
    "similarity index 100%\nrename from old.txt\nrename to new name.txt\n"
    "diff --git a/deleted.txt b/deleted.txt\n"
    "deleted file mode 100644\n--- a/deleted.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-gone\n"
    "diff --git \"a/caf\\303\\251.txt\" \"b/caf\\303\\251.txt\"\n"
    "--- \"a/caf\\303\\251.txt\"\n+++ \"b/caf\\303\\251.txt\"\n@@ -1 +1 @@\n-before\n+after\n";
  const char *stats = "[{\"path\":\"src/main.c\",\"previousPath\":null,\"additions\":1,\"deletions\":1},"
    "{\"path\":\"new name.txt\",\"previousPath\":\"old.txt\",\"additions\":0,\"deletions\":0},"
    "{\"path\":\"deleted.txt\",\"previousPath\":null,\"additions\":0,\"deletions\":1},"
    "{\"path\":\"caf\xc3\xa9.txt\",\"previousPath\":null,\"additions\":1,\"deletions\":1},"
    "{\"path\":\"omitted.bin\",\"previousPath\":null,\"additions\":0,\"deletions\":0}]";
  parse_patch(patch, stats, false);
  assert(diff.count == 5 && diff.has_source && !diff.truncated);
  assert(diff.files[0].line_count == 7);
  assert(!strcmp(diff.files[0].text + diff.files[0].lines[6], "+new"));
  assert(diff.files[1].text && !strcmp(diff.files[1].path, "new name.txt"));
  assert(diff.files[2].deletions == 1 && diff.files[2].text);
  assert(diff.files[3].text && !strcmp(diff.files[3].path, "caf\xc3\xa9.txt"));
  assert(!diff.files[4].text);
  puts("sample preview:");
  for (int i = 0; i < diff.count; i++) {
    const DiffFile *f = &diff.files[i];
    printf("  %s +%d -%d: %d lines%s\n", f->path, f->additions, f->deletions, f->line_count,
           f->text ? "" : " (preview omitted)");
  }
  assert(parse("{\"sources\":[{\"kind\":\"working-tree\",\"diff\":\"ignored\"},"
               "{\"diff\":\"\",\"files\":[],\"kind\":\"branch-range\",\"truncated\":false}]}"));
  assert(diff.has_source && !diff.count && !diff.truncated);
  assert(parse("{\"sources\":[]}") && !diff.has_source);
  parse_patch("diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n--- fake-path\n+++ fake-path\n", NULL, false);
  assert(diff.count == 1 && !strcmp(diff.files[0].path, "x"));
  assert(diff.files[0].additions == 1 && diff.files[0].deletions == 1);
  parse_patch("diff --git a/my file b/my file\n--- a/my file\t\n+++ b/my file\t\n@@ -0,0 +1 @@\n+hello\n", NULL, false);
  assert(diff.count == 1 && !strcmp(diff.files[0].path, "my file"));
  assert(!parse("{\"sources\":[{\"kind\":\"branch-range\",\"diff\":\"broken"));
  assert(!diff.count);
}

static void limits(void) {
  JBuf patch;
  jb_init(&patch);
  for (int file = 0; file < 8; file++) {
    char header[160];
    snprintf(header, sizeof header, "diff --git a/f%d b/f%d\n--- a/f%d\n+++ b/f%d\n@@ -0,0 +1 @@\n", file, file, file, file);
    jb_raw(&patch, header);
    for (int i = 0; i < 3000; i++) jb_raw(&patch, "+a moderately long line for exercising the memory budget\n");
  }
  parse_patch(patch.p, NULL, false);
  assert(diff.truncated && diff.bytes <= DIFF_TOTAL_BYTES && diff.line_count <= DIFF_TOTAL_LINES);
  assert(diff.count > 0 && diff.files[0].truncated);
  for (int i = 0; i < diff.count; i++) assert(diff.files[i].line_count <= DIFF_FILE_LINES);
  jb_free(&patch);
  jb_init(&patch);
  jb_raw(&patch, "diff --git a/many b/many\n--- a/many\n+++ b/many\n@@ -0,0 +10000 @@\n");
  for (int i = 0; i < 10000; i++) jb_raw(&patch, "+\n");
  parse_patch(patch.p, NULL, false);
  assert(diff.files[0].line_count == DIFF_FILE_LINES && diff.files[0].truncated);
  jb_free(&patch);
  jb_init(&patch);
  for (int i = 0; i < 6; i++) {
    char header[160];
    snprintf(header, sizeof header, "diff --git a/lines%d b/lines%d\n--- a/lines%d\n+++ b/lines%d\n@@ -0,0 +8000 @@\n", i, i, i, i);
    jb_raw(&patch, header);
    for (int j = 0; j < 8000; j++) jb_raw(&patch, "+\n");
  }
  parse_patch(patch.p, NULL, false);
  assert(diff.line_count == DIFF_TOTAL_LINES && diff.truncated);
  jb_free(&patch);
  jb_init(&patch);
  for (int i = 0; i < DIFF_MAX_FILES + 5; i++) {
    char header[160];
    snprintf(header, sizeof header, "diff --git a/f%d b/f%d\n--- a/f%d\n+++ b/f%d\n@@ -0,0 +1 @@\n+new\n", i, i, i, i);
    jb_raw(&patch, header);
  }
  parse_patch(patch.p, NULL, true);
  assert(diff.count == DIFF_MAX_FILES && diff.truncated);
  jb_free(&patch);
  parse_patch("", "[{\"path\":\"large\",\"additions\":100000,\"deletions\":2}]", true);
  assert(diff.count == 1 && !diff.files[0].text && diff.truncated);
  puts("limits: byte, line, file and omitted-preview cases passed");
}

static void launch_refs(void) {
  const char *json = "{\"nextCursor\":24,\"isRepo\":true,\"refs\":["
    "{\"name\":\"main\",\"current\":false,\"isDefault\":true},"
    "{\"name\":\"origin/main\",\"isRemote\":true,\"current\":false,\"isDefault\":true},"
    "{\"name\":\"feature/a\",\"current\":true,\"isDefault\":false}]}";
  JsonCur c;
  LaunchRefs refs;
  json_init(&c, json, strlen(json));
  launch_parse_refs(&c, &refs);
  assert(!c.err && refs.count == 2 && refs.preferred == 1 && refs.is_repo && refs.has_more);
  json = "{\"nextCursor\":null,\"isRepo\":false,\"refs\":[]}";
  json_init(&c, json, strlen(json));
  launch_parse_refs(&c, &refs);
  assert(!c.err && !refs.count && !refs.has_more);
  char branch[96];
  launch_branch_seed("Fix the broken dialog!", branch, sizeof branch);
  assert(!strcmp(branch, "t3/fix-the-broken-dialog"));
  JBuf b;
  jb_init(&b);
  LaunchWorkspace workspace = {branch, "feature/a"};
  launch_workspace_json(&b, &workspace);
  assert(!strcmp(b.p, "{\"type\":\"worktree\",\"baseRef\":\"feature/a\",\"branch\":\"t3/fix-the-broken-dialog\",\"startFromOrigin\":false}"));
  printf("workspace payload: %s\n", b.p);
  jb_free(&b);
  puts("refs: current branch preferred, remotes excluded, pagination retained");
}

int main(void) {
  sample_preview();
  limits();
  launch_refs();
  diff_clear(&diff);
  puts("parser tests passed");
  return 0;
}
