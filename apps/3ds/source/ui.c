#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "bg.h"
#include "client.h"
#include "convview.h"
#include "diffview.h"
#include "launchview.h"
#include "threadops.h"
#include "gfx.h"
#include "pairlink.h"
#include "history.h"
#include "prdetailview.h"
#include "qrscan.h"
#include "store.h"
#include "snooze.h"
#include "threadpanels.h"
#include "util.h"

#define STORE_DIR "sdmc:/3ds/t3code"
#define STORE_PATH STORE_DIR "/environments.json"

#define TAB_H 24
#define ACTION_Y 206
#define ACTION_H 28
#define LIST_Y (TAB_H + 2)
#define LIST_H (ACTION_Y - LIST_Y - 4)
#define ROW_H 35
#define TOP_BAR_H 18
#define THREAD_BAR_H 34
#define MENU_ROW_H 21
#define MENU_Y (LIST_Y + 22)
#define MENU_ROWS 7
#define MAX_THREAD_ACTIONS 20
#define USAGE_REFRESH_MS 60000
#define PR_REFRESH_MS 120000

typedef enum { TAB_THREADS, TAB_USAGE, TAB_PRS, TAB_MACHINES, TAB_TASKS, TAB_COUNT } Tab;
typedef enum {
  MODE_LIST,
  MODE_THREAD,
  MODE_THREAD_MENU,
  MODE_PICK_PROJECT,
  MODE_SCAN,
  MODE_PICK_MODEL,
  MODE_PICK_SNOOZE,
  MODE_PR_DETAIL,
  MODE_PICK_SUBAGENT,
  MODE_PICK_WORKSPACE,
  MODE_DIFF,
} Mode;

typedef struct {
  int sel;
  float scroll;
} ListState;

typedef enum {
  ACT_NONE,
  ACT_OPEN,
  ACT_REPLY,
  ACT_STOP,
  ACT_DONE,
  ACT_APPROVE,
  ACT_DECLINE,
  ACT_BACK,
  ACT_NEW,
  ACT_TOGGLE_DONE,
  ACT_REFRESH,
  ACT_CONNECT,
  ACT_PAIR,
  ACT_TYPE_IN,
  ACT_REMOVE,
  ACT_PICK,
  ACT_TOGGLE_REVIEW,
  ACT_MORE,
  ACT_MENU_PICK,
  ACT_PIN,
  ACT_ARCHIVE,
  ACT_UNREAD,
  ACT_RENAME,
  ACT_DISMISS,
  ACT_PLAN,
  ACT_ACCESS,
  ACT_ALLOW_SESSION,
  ACT_ALLOW_ALWAYS,
  ACT_CANCEL_APPROVAL,
  ACT_MODEL,
  ACT_SNOOZE,
  ACT_ANSWER,
  ACT_QUEUE,
  ACT_PR_OPEN,
  ACT_PR_APPROVE,
  ACT_PR_COMMENT,
  ACT_PR_MERGE,
  ACT_TASK_RUN,
  ACT_TASK_TOGGLE,
  ACT_FORK,
  ACT_DIFF,
  ACT_SUBAGENTS,
  ACT_PARENT,
} ActionId;

typedef struct {
  ActionId id;
  const char *hint;
  const char *label;
  u32 key;
  bool enabled;
  bool primary;
} Action;

static Store store;
static Client client;
static Tab tab;
static Mode mode;
static ListState thread_list, pr_list, env_list, project_list;
static ListState model_list, snooze_list;
static bool model_for_new;
static char picker_project[ID_CAP];
static const char *snooze_labels[] = {"1 hour", "3 hours", "Tomorrow at 9:00"};
static ListState task_list;
static float pr_detail_scroll;
static bool merge_armed;
static bool tasks_opened;

// Subagents of the open thread, as indices into client.threads; rebuilt each frame.
#define MAX_SUBAGENTS 64
static ListState subagent_list;
static int subagent_view[MAX_SUBAGENTS];
static int subagent_count, subagent_running;
static char selected_thread[ID_CAP];
static bool show_done;
static bool review_only;
static int thread_menu_sel, thread_menu_first;
static int remove_armed = -1;
static bool pairing;
static char machines_note[160];

// Filtered views rebuilt each frame; indices into client arrays.
static int *thread_view;
static int thread_view_count, thread_view_cap, hidden_done;
static int *pr_view;
static int pr_view_count, pr_view_cap;

static float conv_scroll;
static bool conv_follow = true;

// The bottom screen's list of your messages, laid out by draw_thread_bottom.
#define NAV_ROW_H 17
static float nav_top;
static int nav_first, nav_rows;


static struct {
  bool active, dragging;
  int start_x, start_y, last_x, last_y;
  float start_scroll;
} touch_state;

// ---------------------------------------------------------------- helpers

static uint64_t now_ms(void) { return client_ticks_ms(); }

static bool connected(void) { return client.state == CONN_READY; }

static T3Thread *current_thread(void) { return selected_thread[0] ? client_find_thread(&client, selected_thread) : NULL; }

static bool prompt_text(const char *hint, const char *initial, char *out, size_t cap, int max_len, bool multiline) {
  SwkbdState kb;
  swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, max_len);
  swkbdSetHintText(&kb, hint);
  if (initial) swkbdSetInitialText(&kb, initial);
  swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancel", false);
  swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, multiline ? "Send" : "OK", true);
  swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
  if (multiline) swkbdSetFeatures(&kb, SWKBD_MULTILINE | SWKBD_PREDICTIVE_INPUT);
  out[0] = '\0';
  return swkbdInputText(&kb, out, cap) == SWKBD_BUTTON_RIGHT && out[0];
}

static void save_store(void) {
  mkdir("sdmc:/3ds", 0777);
  mkdir(STORE_DIR, 0777);
  if (!store_save(&store, STORE_PATH)) client_toast(&client, "Could not save to the SD card");
}

static u32 thread_color(const T3Thread *t) {
  if (thread_needs_user(t)) return COL_ATTENTION;
  if (thread_is_active(t)) return COL_RUNNING;
  if (!strcmp(t->status, "failed")) return COL_ERROR;
  if (!strcmp(t->status, "completed") && !t->settled) return COL_OK;
  return COL_DIM;
}

static const char *thread_label(const T3Thread *t) {
  if (t->snoozed_until > client_now(&client)) return "Snoozed";
  if (t->pending_id[0] && t->pending_blocking)
    return strcmp(t->pending_kind, "user_input") == 0 ? "Question for you" : "Needs approval";
  if (!strcmp(t->status, "running")) return "Working";
  if (!strcmp(t->status, "waiting")) return "Waiting";
  if (!strcmp(t->status, "queued")) return "Queued";
  if (!strcmp(t->status, "preparing") || !strcmp(t->status, "starting")) return "Starting";
  if (!strcmp(t->status, "failed")) return "Failed";
  if (!strcmp(t->status, "interrupted")) return "Stopped";
  if (t->settled) return "Done";
  if (!strcmp(t->status, "completed")) return "Ready for review";
  return "Idle";
}

static bool pending_is_approval(const T3Thread *t) {
  return t->pending_id[0] && strcmp(t->pending_kind, "user_input") != 0 && strcmp(t->pending_kind, "auth_refresh") != 0;
}

static Rect more_rect(void) { return (Rect){BOT_W - 78, LIST_Y + 3, 70, 21}; }

static const char *next_runtime_mode(const T3Thread *t) {
  if (!strcmp(t->runtime_mode, "approval-required")) return "auto-accept-edits";
  if (!strcmp(t->runtime_mode, "auto-accept-edits")) return "auto";
  if (!strcmp(t->runtime_mode, "auto")) return "full-access";
  return "approval-required";
}

static void list_keep_visible(ListState *l, int count) {
  if (count == 0) {
    l->sel = 0;
    l->scroll = 0;
    return;
  }
  if (l->sel >= count) l->sel = count - 1;
  if (l->sel < 0) l->sel = 0;
  float top = l->sel * ROW_H, bottom = top + ROW_H;
  if (top < l->scroll) l->scroll = top;
  if (bottom > l->scroll + LIST_H) l->scroll = bottom - LIST_H;
  float max = count * ROW_H - LIST_H;
  if (max < 0) max = 0;
  if (l->scroll > max) l->scroll = max;
  if (l->scroll < 0) l->scroll = 0;
}

static void list_clamp_scroll(ListState *l, int count) {
  float max = count * ROW_H - LIST_H;
  if (max < 0) max = 0;
  if (l->scroll > max) l->scroll = max;
  if (l->scroll < 0) l->scroll = 0;
}

static void rebuild_thread_view(void) {
  if (thread_view_cap < client.thread_count) {
    thread_view_cap = client.thread_count + 32;
    thread_view = realloc(thread_view, sizeof(int) * (size_t)thread_view_cap);
  }
  thread_view_count = 0;
  hidden_done = 0;
  for (int i = 0; i < client.thread_count; i++) {
    T3Thread *t = &client.threads[i];
    if (t->subagent) continue;
    if (!show_done &&
        (t->archived || (!t->pinned_at && thread_rank(t) == 4) || t->snoozed_until > client_now(&client))) {
      hidden_done++;
      continue;
    }
    thread_view[thread_view_count++] = i;
  }
  // Follow the selected thread when the list re-sorts under it.
  if (selected_thread[0]) {
    for (int i = 0; i < thread_view_count; i++) {
      if (!strcmp(client.threads[thread_view[i]].id, selected_thread)) {
        thread_list.sel = i;
        break;
      }
    }
  }
  list_clamp_scroll(&thread_list, thread_view_count);
  if (thread_list.sel >= thread_view_count) thread_list.sel = thread_view_count ? thread_view_count - 1 : 0;
  if (mode == MODE_LIST) {
    if (thread_view_count)
      util_strlcpy(selected_thread, client.threads[thread_view[thread_list.sel]].id, sizeof selected_thread);
    else selected_thread[0] = '\0';
  }
}

static void rebuild_subagent_view(const T3Thread *parent) {
  subagent_count = subagent_running = 0;
  if (!parent) return;
  for (int i = 0; i < client.thread_count && subagent_count < MAX_SUBAGENTS; i++) {
    const T3Thread *t = &client.threads[i];
    if (!t->subagent || strcmp(t->parent_id, parent->id)) continue;
    subagent_view[subagent_count++] = i;
    if (thread_is_active(t)) subagent_running++;
  }
  if (subagent_list.sel >= subagent_count) subagent_list.sel = subagent_count ? subagent_count - 1 : 0;
}

static T3Thread *parent_thread(const T3Thread *t) {
  return t && t->subagent && t->parent_id[0] ? client_find_thread(&client, t->parent_id) : NULL;
}

static void rebuild_pr_view(void) {
  if (pr_view_cap < client.pr_count) {
    pr_view_cap = client.pr_count + 16;
    pr_view = realloc(pr_view, sizeof(int) * (size_t)pr_view_cap);
  }
  pr_view_count = 0;
  for (int i = 0; i < client.pr_count; i++)
    if (!review_only || client.prs[i].review_requested) pr_view[pr_view_count++] = i;
  list_clamp_scroll(&pr_list, pr_view_count);
  if (pr_list.sel >= pr_view_count) pr_list.sel = pr_view_count ? pr_view_count - 1 : 0;
}

// ---------------------------------------------------------------- conversation layout

static float conv_view_h(void) { return SCREEN_H - THREAD_BAR_H; }

static float conv_max_scroll(void) {
  float m = convview_height() - conv_view_h();
  return m > 0 ? m : 0;
}

static void sync_conversation_layout(void) {
  bool was_following = conv_follow || conv_scroll >= conv_max_scroll() - 4;
  float old_height = convview_height();
  float old_anchor = convview_turn_count() ? convview_turn_y(0) : -1;
  int users = 0;
  for (int i = 0; i < client.history_prepended; i++)
    if (client.conv.items[i].kind == ITEM_USER) users++;
  if (!convview_sync(&client, current_thread())) return;
  if (client.history_prepended) {
    float delta = old_anchor >= 0 && users < convview_turn_count()
                    ? convview_turn_y(users) - old_anchor : convview_height() - old_height;
    conv_scroll += delta;
    client.history_prepended = 0;
    was_following = false;
  }
  if (was_following) conv_scroll = conv_max_scroll();
  if (conv_scroll > conv_max_scroll()) conv_scroll = conv_max_scroll();
  conv_follow = false;
}

// ---------------------------------------------------------------- actions

static int build_thread_menu(Action *a) {
  T3Thread *t = current_thread();
  if (!t) return 0;
  int n = 0;
  unsigned options = client_approval_options(&client, t);
  bool ready = connected();
  if (options & APPROVAL_SESSION)
    a[n++] = (Action){ACT_ALLOW_SESSION, NULL, "Allow for session", 0, ready, true};
  if (options & APPROVAL_ALWAYS)
    a[n++] = (Action){ACT_ALLOW_ALWAYS, NULL, "Always allow", 0, ready, false};
  if (options & APPROVAL_CANCEL)
    a[n++] = (Action){ACT_CANCEL_APPROVAL, NULL, "Cancel approval", 0, ready, false};
  if (t->pending_id[0] && !strcmp(t->pending_kind, "user_input"))
    a[n++] = (Action){ACT_DISMISS, NULL, "Dismiss question", 0, ready && client_can_dismiss_question(&client, t), false};
  // A subagent is driven by its parent; thread housekeeping happens there.
  if (t->subagent) {
    a[n++] = (Action){ACT_PARENT, NULL, "Open parent", 0, parent_thread(t) != NULL, true};
    return n;
  }
  rebuild_subagent_view(t);
  if (subagent_count) {
    static char subagents_label[32];
    if (subagent_running) snprintf(subagents_label, sizeof subagents_label, "Subagents (%d, %d running)", subagent_count, subagent_running);
    else snprintf(subagents_label, sizeof subagents_label, "Subagents (%d)", subagent_count);
    a[n++] = (Action){ACT_SUBAGENTS, NULL, subagents_label, 0, true, true};
  }
  a[n++] = (Action){ACT_PIN, NULL, t->pinned_at ? "Unpin" : "Pin", 0, ready, false};
  a[n++] = (Action){ACT_ARCHIVE, NULL, t->archived ? "Unarchive" : "Archive", 0, ready, false};
  a[n++] = (Action){ACT_UNREAD, NULL, "Mark unread", 0, ready && t->latest_completed_ms > 0, false};
  a[n++] = (Action){ACT_RENAME, NULL, "Rename", 0, ready, false};
  a[n++] = (Action){ACT_PLAN, NULL, !strcmp(t->interaction_mode, "plan") ? "Plan mode: off" : "Plan mode: on",
                    0, ready, false};
  const char *next = next_runtime_mode(t);
  const char *access = !strcmp(next, "approval-required") ? "Access: approval-required"
                       : !strcmp(next, "auto-accept-edits") ? "Access: auto-accept-edits"
                       : !strcmp(next, "auto") ? "Access: auto" : "Access: full-access";
  a[n++] = (Action){ACT_ACCESS, NULL, access, 0, ready, false};
  if (client.conv.queue && client.conv.queue->count) {
    static char queue_label[24];
    snprintf(queue_label, sizeof queue_label, "Queue (%d)", client.conv.queue->count);
    a[n++] = (Action){ACT_QUEUE, NULL, queue_label, 0, true, true};
  }
  a[n++] = (Action){ACT_DIFF, NULL, "Changes", 0, ready, false};
  a[n++] = (Action){ACT_FORK, NULL, "Fork", 0, ready, false};
  a[n++] = (Action){ACT_MODEL, NULL, "Model", 0, ready && client.catalog.model_count > 0, false};
  bool snoozed = t->snoozed_until > client_now(&client);
  a[n++] = (Action){ACT_SNOOZE, NULL, snoozed ? "Unsnooze" : "Snooze", 0, ready && (!t->pending_id[0] || snoozed), false};
  a[n++] = (Action){ACT_DONE, NULL, t->settled ? "Reopen" : "Mark done", 0, ready, false};
  if (thread_menu_sel >= n) thread_menu_sel = n - 1;
  if (thread_menu_sel < 0) thread_menu_sel = 0;
  if (thread_menu_sel < thread_menu_first) thread_menu_first = thread_menu_sel;
  if (thread_menu_sel >= thread_menu_first + MENU_ROWS) thread_menu_first = thread_menu_sel - MENU_ROWS + 1;
  return n;
}

static int build_actions(Action *a) {
  int n = 0;
  T3Thread *t = current_thread();
  switch (tab) {
    case TAB_THREADS:
      if (mode == MODE_THREAD_MENU) {
        a[n++] = (Action){ACT_MENU_PICK, GLYPH_A, "Choose", KEY_A, t && connected(), true};
        a[n++] = (Action){ACT_BACK, GLYPH_B, "Close", KEY_B, true, false};
      } else if (mode == MODE_PICK_SUBAGENT) {
        a[n++] = (Action){ACT_PICK, GLYPH_A, "Open", KEY_A, subagent_count > 0, true};
        a[n++] = (Action){ACT_BACK, GLYPH_B, "Back", KEY_B, true, false};
      } else if (mode == MODE_PICK_MODEL || mode == MODE_PICK_SNOOZE) {
        int count = mode == MODE_PICK_SNOOZE ? 3 : client.catalog.model_count + (model_for_new ? 1 : 0);
        a[n++] = (Action){ACT_PICK, GLYPH_A, "Choose", KEY_A, connected() && count > 0, true};
        a[n++] = (Action){ACT_BACK, GLYPH_B, "Back", KEY_B, true, false};
      } else if (mode == MODE_PICK_PROJECT) {
        a[n++] = (Action){ACT_PICK, GLYPH_A, "Next", KEY_A, client.project_count > 0, true};
        a[n++] = (Action){ACT_BACK, GLYPH_B, "Cancel", KEY_B, true, false};
      } else if (mode == MODE_THREAD && !t) {
        a[n++] = (Action){ACT_BACK, GLYPH_B, "Back", KEY_B, true, false};
      } else if (mode == MODE_THREAD) {
        bool question = t->pending_id[0] && !strcmp(t->pending_kind, "user_input");
        if (pending_is_approval(t) && t->pending_blocking) {
          unsigned options = client_approval_options(&client, t);
          a[n++] = (Action){ACT_APPROVE, GLYPH_A, "Approve", KEY_A, connected() && (options & APPROVAL_ACCEPT), true};
          a[n++] = (Action){ACT_DECLINE, GLYPH_Y, "Decline", KEY_Y, connected() && (options & APPROVAL_DECLINE), false};
          a[n++] = (Action){ACT_MORE, NULL, "More", 0, true, false};
        } else {
          a[n++] = (Action){ACT_STOP, GLYPH_Y, "Stop", KEY_Y, t->active_run_id[0] != 0, false};
          if (question)
            a[n++] = (Action){ACT_ANSWER, GLYPH_A, "Answer", KEY_A, connected() && threadpanels_can_answer(&client, t), true};
          a[n++] = (Action){ACT_MORE, question ? NULL : GLYPH_A, "More", question ? 0 : KEY_A, true, false};
        }
        // Provider-run subagents reject messages; the parent thread takes them.
        a[n++] = (Action){ACT_REPLY, GLYPH_X, "Reply", KEY_X, connected() && !t->subagent, !pending_is_approval(t)};
        a[n++] = (Action){ACT_BACK, GLYPH_B, "Back", KEY_B, true, false};
      } else {
        a[n++] = (Action){ACT_OPEN, GLYPH_A, "Open", KEY_A, t != NULL, true};
        a[n++] = (Action){ACT_REPLY, GLYPH_X, "Reply", KEY_X, t && connected(), false};
        a[n++] = (Action){ACT_STOP, GLYPH_Y, "Stop", KEY_Y, t && t->active_run_id[0], false};
        a[n++] = (Action){ACT_NEW, NULL, "+ New", 0, connected() && client.project_count > 0, false};
      }
      break;
    case TAB_USAGE:
      a[n++] = (Action){ACT_REFRESH, GLYPH_Y, "Refresh", KEY_Y, connected(), false};
      break;
    case TAB_TASKS:
      a[n++] = (Action){ACT_TASK_RUN, GLYPH_A, "Run now", KEY_A, connected() && client.task_count > 0 && !client.task_action_busy, true};
      a[n++] = (Action){ACT_TASK_TOGGLE, GLYPH_X,
                        client.task_count && client.tasks[task_list.sel].enabled ? "Disable" : "Enable", KEY_X,
                        connected() && client.task_count > 0 && !client.task_action_busy, false};
      a[n++] = (Action){ACT_REFRESH, GLYPH_Y, "Refresh", KEY_Y, connected(), false};
      break;
    case TAB_PRS:
      if (mode == MODE_PR_DETAIL) {
        PrDetail *d = &client.pr_detail;
        bool ready = connected() && client.pr_detail_state == LOAD_DONE && !d->action_id;
        a[n++] = (Action){ACT_PR_APPROVE, GLYPH_A, "Approve", KEY_A, ready && d->can_approve, true};
        a[n++] = (Action){ACT_PR_COMMENT, GLYPH_X, "Comment", KEY_X, ready && d->can_comment, false};
        a[n++] = (Action){ACT_PR_MERGE, GLYPH_Y, d->pr.draft ? "Ready" : merge_armed ? "Confirm" : "Merge", KEY_Y,
                          ready && (d->can_ready || prdetail_merge_allowed(&client)), false};
        a[n++] = (Action){ACT_BACK, GLYPH_B, "Back", KEY_B, true, false};
        break;
      }
      a[n++] = (Action){ACT_PR_OPEN, GLYPH_A, "Open", KEY_A, connected() && pr_view_count > 0, true};
      a[n++] = (Action){ACT_REFRESH, GLYPH_Y, "Refresh", KEY_Y, connected(), false};
      a[n++] = (Action){ACT_TOGGLE_REVIEW, NULL, review_only ? "Show all" : "To review", KEY_SELECT, true, false};
      break;
    case TAB_MACHINES:
      if (mode == MODE_SCAN) {
        a[n++] = (Action){ACT_TYPE_IN, GLYPH_X, "Type it in", KEY_X, true, false};
        a[n++] = (Action){ACT_BACK, GLYPH_B, "Cancel", KEY_B, true, false};
        break;
      }
      a[n++] = (Action){ACT_CONNECT, GLYPH_A, "Connect", KEY_A, store.count > 0 && !pairing, true};
      a[n++] = (Action){ACT_PAIR, GLYPH_X, "Pair new", KEY_X, !pairing && store.count < MAX_ENVS, false};
      a[n++] = (Action){ACT_REMOVE, GLYPH_Y, remove_armed == env_list.sel ? "Confirm" : "Remove", KEY_Y,
                        store.count > 0 && !pairing, false};
      break;
    default: break;
  }
  return n;
}

static Rect action_rect(int i, int n) {
  float gap = 6, w = (BOT_W - 12 - gap * (float)(n - 1)) / (float)n;
  return (Rect){6 + (float)i * (w + gap), ACTION_Y, w, ACTION_H};
}

static void on_paired(void *ud, const PairResult *r) {
  (void)ud;
  pairing = false;
  if (!r->ok) {
    util_strlcpy(machines_note, r->error, sizeof machines_note);
    return;
  }
  int idx = store_upsert(&store, &r->env);
  if (idx < 0) {
    util_strlcpy(machines_note, "Machine list is full; remove one first", sizeof machines_note);
    return;
  }
  store.last_used = idx;
  env_list.sel = idx;
  save_store();
  snprintf(machines_note, sizeof machines_note, "Paired with %s", r->env.label);
  client_connect(&client, &store.envs[idx]);
  tab = TAB_THREADS;
  mode = MODE_LIST;
}

static void start_pairing(const char *addr, const char *code) {
  pairing = true;
  snprintf(machines_note, sizeof machines_note, "Pairing with %s...", addr);
  client_pair(addr, code, on_paired, NULL);
}

static void pair_typed(void) {
  char addr[160], code[40];
  const char *initial = env_list.sel < store.count ? store.envs[env_list.sel].addr : NULL;
  if (!prompt_text("Server address, e.g. 192.168.1.20:3773", initial, addr, sizeof addr, 150, false)) return;
  if (!prompt_text("Pairing code from Settings > Connections", NULL, code, sizeof code, 32, false)) return;
  start_pairing(addr, code);
}

static void scan_update(void) {
  char link[1024], addr[160], code[64];
  if (mode != MODE_SCAN || !qrscan_update(link, sizeof link)) return;
  switch (pairlink_parse(link, addr, sizeof addr, code, sizeof code)) {
    case PAIRLINK_OK:
      qrscan_stop();
      mode = MODE_LIST;
      start_pairing(addr, code);
      break;
    case PAIRLINK_HTTPS:
      util_strlcpy(machines_note, "That link is for an HTTPS address. Turn on Network access and use the host's LAN link.",
                   sizeof machines_note);
      break;
    case PAIRLINK_NOT_T3:
      util_strlcpy(machines_note, "That QR code isn't a T3 Code pairing link.", sizeof machines_note);
      break;
  }
}

static void open_thread(T3Thread *t) {
  util_strlcpy(selected_thread, t->id, sizeof selected_thread);
  mode = MODE_THREAD;
  conv_scroll = 0;
  conv_follow = true;
  client_open_conversation(&client, t->id);
  client_mark_read(&client, t);
}

static void run_action(ActionId id) {
  T3Thread *t = current_thread();
  char text[2048];
  switch (id) {
    case ACT_MORE:
      if (t) {
        mode = MODE_THREAD_MENU;
        thread_menu_sel = thread_menu_first = 0;
      }
      break;
    case ACT_MENU_PICK: {
      Action a[MAX_THREAD_ACTIONS];
      int n = build_thread_menu(a);
      if (n && a[thread_menu_sel].enabled) {
        ActionId choice = a[thread_menu_sel].id;
        mode = MODE_THREAD;
        run_action(choice);
      }
      break;
    }
    case ACT_PIN:
      if (t) client_set_pinned(&client, t, !t->pinned_at);
      break;
    case ACT_ARCHIVE:
      if (t) client_set_archived(&client, t, !t->archived);
      break;
    case ACT_UNREAD:
      if (t) client_mark_unread(&client, t);
      break;
    case ACT_RENAME:
      if (t && prompt_text("Thread title", t->title, text, sizeof t->title, (int)sizeof t->title - 1, false))
        client_rename(&client, t, text);
      break;
    case ACT_DISMISS:
      if (t) client_dismiss_question(&client, t);
      break;
    case ACT_PLAN:
      if (t) client_set_interaction_mode(&client, t, !strcmp(t->interaction_mode, "plan") ? "default" : "plan");
      break;
    case ACT_ACCESS:
      if (t) client_set_runtime_mode(&client, t, next_runtime_mode(t));
      break;
    case ACT_ALLOW_SESSION:
      if (t) client_respond(&client, t, "acceptForSession");
      break;
    case ACT_ALLOW_ALWAYS:
      if (t) client_respond(&client, t, "acceptAlways");
      break;
    case ACT_CANCEL_APPROVAL:
      if (t) client_respond(&client, t, "cancel");
      break;
    case ACT_PR_OPEN:
      if (pr_view_count) {
        client_open_pr(&client, &client.prs[pr_view[pr_list.sel]]);
        pr_detail_scroll = 0;
        merge_armed = false;
        mode = MODE_PR_DETAIL;
      }
      break;
    case ACT_PR_APPROVE:
    case ACT_PR_COMMENT:
      merge_armed = false;
      if (prompt_text(id == ACT_PR_APPROVE ? "Approval message" : "Comment on the pull request", NULL,
                      text, sizeof text, 1500, true)) client_pr_review(&client, id == ACT_PR_APPROVE, text);
      break;
    case ACT_PR_MERGE:
      if (!client.pr_detail.pr.draft && !merge_armed) { merge_armed = true; break; }
      merge_armed = false;
      client_pr_action(&client);
      break;
    case ACT_TASK_RUN:
    case ACT_TASK_TOGGLE:
      if (task_list.sel < client.task_count) {
        ScheduledTask *task = &client.tasks[task_list.sel];
        if (id == ACT_TASK_RUN) client_task_run(&client, task);
        else client_task_toggle(&client, task);
      }
      break;
    case ACT_OPEN:
      if (t) open_thread(t);
      break;
    case ACT_ANSWER:
      if (t) threadpanels_answer(&client, t);
      break;
    case ACT_QUEUE:
      threadpanels_queue();
      break;
    case ACT_REPLY:
      if (t && prompt_text("Message the agent", NULL, text, sizeof text, 1500, true)) client_send_message(&client, t, text);
      break;
    case ACT_STOP:
      if (t) client_interrupt(&client, t);
      break;
    case ACT_FORK:
      if (t) {
        char title[160];
        snprintf(title, sizeof title, "%.150s fork", t->title);
        if (prompt_text("Fork title (latest stable point)", title, text, sizeof text, 150, false))
          client_fork_thread(&client, t, text);
      }
      break;
    case ACT_DIFF:
      if (t) {
        diffview_reset();
        client_load_diff(&client, t);
        mode = MODE_DIFF;
      }
      break;
    case ACT_DONE:
      if (t) client_set_settled(&client, t, !t->settled);
      break;
    case ACT_APPROVE:
      if (t) client_respond(&client, t, "accept");
      break;
    case ACT_DECLINE:
      if (t) client_respond(&client, t, "decline");
      break;
    case ACT_MODEL:
      model_for_new = false;
      model_list = (ListState){0};
      if (t) {
        for (int i = 0; i < client.catalog.model_count; i++) {
          CatalogModel *m = &client.catalog.models[i];
          if (!strcmp(m->id, t->model) && !strcmp(client.catalog.providers[m->provider].id, t->provider)) model_list.sel = i;
        }
      }
      list_keep_visible(&model_list, client.catalog.model_count);
      mode = MODE_PICK_MODEL;
      break;
    case ACT_SNOOZE:
      if (!t) break;
      if (t->snoozed_until > client_now(&client)) client_snooze(&client, t, 0);
      else {
        snooze_list = (ListState){0};
        mode = MODE_PICK_SNOOZE;
      }
      break;
    case ACT_SUBAGENTS:
      if (t) {
        subagent_list = (ListState){0};
        rebuild_subagent_view(t);
        mode = MODE_PICK_SUBAGENT;
      }
      break;
    case ACT_PARENT: {
      T3Thread *parent = parent_thread(t);
      if (parent) open_thread(parent);
      break;
    }
    case ACT_BACK:
      merge_armed = false;
      if (mode == MODE_PICK_SUBAGENT) {
        mode = MODE_THREAD;
        break;
      }
      if (mode == MODE_THREAD && parent_thread(t)) {
        open_thread(parent_thread(t));
        break;
      }
      if (mode == MODE_PICK_MODEL) {
        mode = model_for_new ? MODE_PICK_PROJECT : MODE_THREAD;
        break;
      }
      if (mode == MODE_THREAD_MENU || mode == MODE_PICK_SNOOZE) {
        mode = MODE_THREAD;
        break;
      }
      if (mode == MODE_THREAD) client_close_conversation(&client);
      if (mode == MODE_SCAN) qrscan_stop();
      mode = MODE_LIST;
      break;
    case ACT_NEW:
      mode = MODE_PICK_PROJECT;
      if (t) {
        for (int i = 0; i < client.project_count; i++)
          if (!strcmp(client.projects[i].id, t->project_id)) project_list.sel = i;
      }
      list_keep_visible(&project_list, client.project_count);
      break;
    case ACT_PICK: {
      if (mode == MODE_PICK_SUBAGENT) {
        if (subagent_list.sel < subagent_count) open_thread(&client.threads[subagent_view[subagent_list.sel]]);
        break;
      }
      if (mode == MODE_PICK_SNOOZE) {
        int64_t until = snooze_deadline(snooze_list.sel, (int64_t)time(NULL), client.clock_offset);
        if (t && until > client_now(&client)) client_snooze(&client, t, until);
        mode = MODE_THREAD;
        break;
      }
      if (mode == MODE_PICK_PROJECT) {
        if (project_list.sel >= client.project_count) break;
        util_strlcpy(picker_project, client.projects[project_list.sel].id, sizeof picker_project);
        model_for_new = true;
        model_list = (ListState){0};
        mode = MODE_PICK_MODEL;
        break;
      }
      if (mode != MODE_PICK_MODEL) break;
      int index = model_list.sel - (model_for_new ? 1 : 0);
      JBuf selection;
      models_selection(&selection, &client.catalog, index);
      if (!model_for_new) {
        if (t && selection.n) client_set_model(&client, t, selection.p);
        mode = MODE_THREAD;
      } else {
        Project *p = client_find_project(&client, picker_project);
        if (p) {
          launchview_begin(p, selection.n ? selection.p : NULL);
          mode = MODE_PICK_WORKSPACE;
        } else {
          client_toast(&client, "Project no longer available");
          mode = MODE_PICK_PROJECT;
        }
      }
      jb_free(&selection);
      break;
    }
    case ACT_TOGGLE_DONE:
      show_done = !show_done;
      if (show_done) client_load_archived(&client);
      break;
    case ACT_TOGGLE_REVIEW:
      review_only = !review_only;
      pr_list.sel = 0;
      pr_list.scroll = 0;
      break;
    case ACT_REFRESH:
      if (tab == TAB_USAGE) client_refresh_usage(&client);
      else if (tab == TAB_TASKS) client_refresh_tasks(&client);
      else client_refresh_prs(&client);
      break;
    case ACT_CONNECT:
      if (env_list.sel < store.count) {
        store.last_used = env_list.sel;
        save_store();
        client_connect(&client, &store.envs[env_list.sel]);
        machines_note[0] = '\0';
        tab = TAB_THREADS;
        mode = MODE_LIST;
        selected_thread[0] = '\0';
        thread_list = (ListState){0};
      }
      break;
    case ACT_PAIR:
      machines_note[0] = '\0';
      if (qrscan_start(machines_note, sizeof machines_note)) mode = MODE_SCAN;
      else pair_typed();
      break;
    case ACT_TYPE_IN:
      qrscan_stop();
      mode = MODE_LIST;
      machines_note[0] = '\0';
      pair_typed();
      break;
    case ACT_REMOVE:
      if (env_list.sel >= store.count) break;
      if (remove_armed != env_list.sel) {
        remove_armed = env_list.sel;
        break;
      }
      if (!strcmp(store.envs[env_list.sel].environment_id, client.env.environment_id) &&
          !strcmp(store.envs[env_list.sel].addr, client.env.addr))
        client_disconnect(&client);
      store_remove(&store, env_list.sel);
      save_store();
      remove_armed = -1;
      list_keep_visible(&env_list, store.count);
      util_strlcpy(machines_note, "Removed. Revoke it on the host under Settings > Connections.", sizeof machines_note);
      break;
    default: break;
  }
}

// ---------------------------------------------------------------- input

static ListState *active_list(int *count) {
  if (tab == TAB_THREADS && mode == MODE_LIST) {
    *count = thread_view_count;
    return &thread_list;
  }
  if (tab == TAB_THREADS && mode == MODE_PICK_PROJECT) {
    *count = client.project_count;
    return &project_list;
  }
  if (tab == TAB_THREADS && mode == MODE_PICK_MODEL) {
    *count = client.catalog.model_count + (model_for_new ? 1 : 0);
    return &model_list;
  }
  if (tab == TAB_THREADS && mode == MODE_PICK_SNOOZE) {
    *count = 3;
    return &snooze_list;
  }
  if (tab == TAB_THREADS && mode == MODE_PICK_SUBAGENT) {
    *count = subagent_count;
    return &subagent_list;
  }
  if (tab == TAB_TASKS) {
    *count = client.task_count;
    return &task_list;
  }
  if (tab == TAB_PRS && mode == MODE_LIST) {
    *count = pr_view_count;
    return &pr_list;
  }
  if (tab == TAB_MACHINES && mode == MODE_LIST) {
    *count = store.count;
    return &env_list;
  }
  *count = 0;
  return NULL;
}

static void select_row(ListState *l, int count, int row) {
  if (row < 0 || row >= count) return;
  l->sel = row;
  list_keep_visible(l, count);
  if (l == &thread_list) util_strlcpy(selected_thread, client.threads[thread_view[row]].id, sizeof selected_thread);
  if (l == &env_list && remove_armed != row) remove_armed = -1;
}

static void switch_tab(int delta) {
  if (client.conv.thread_id[0]) client_close_conversation(&client);
  if (mode == MODE_SCAN) qrscan_stop();
  mode = MODE_LIST;
  tab = (Tab)(((int)tab + delta + TAB_COUNT) % TAB_COUNT);
  remove_armed = -1;
  merge_armed = false;
  if (tab == TAB_TASKS) {
    tasks_opened = connected();
    client_refresh_tasks(&client);
  }
}

static void handle_input(u32 down, u32 held, const touchPosition *touch) {
  if (threadpanels_active()) {
    threadpanels_input(&client, current_thread(), down, touch, prompt_text);
    return;
  }
  if (mode == MODE_DIFF) {
    DiffInput input = diffview_input(&client.diff, down, held, touch);
    if (input == DIFF_INPUT_BACK) {
      client_close_diff(&client);
      mode = MODE_THREAD;
    } else if (input == DIFF_INPUT_REFRESH && client.diff_state != LOAD_BUSY && current_thread()) {
      diffview_reset();
      client_load_diff(&client, current_thread());
    }
    return;
  }
  if (mode == MODE_PICK_WORKSPACE) {
    if (launchview_input(&client, down, held, touch, prompt_text)) mode = MODE_LIST;
    return;
  }
  if (tab == TAB_THREADS && mode == MODE_THREAD) sync_conversation_layout();
  Action actions[6];
  int na = build_actions(actions);
  if (tab == TAB_THREADS && mode == MODE_THREAD && (down & KEY_SELECT)) convview_toggle_folds();

  if (down & KEY_L) { switch_tab(-1); return; }
  if (down & KEY_R) { switch_tab(1); return; }
  if (tab == TAB_THREADS && mode == MODE_LIST && (down & KEY_SELECT)) run_action(ACT_TOGGLE_DONE);

  if (tab == TAB_THREADS && mode == MODE_THREAD_MENU) {
    Action a[MAX_THREAD_ACTIONS];
    int n = build_thread_menu(a);
    if ((down & KEY_DUP) && thread_menu_sel > 0) thread_menu_sel--;
    if ((down & KEY_DDOWN) && thread_menu_sel + 1 < n) thread_menu_sel++;
    build_thread_menu(a);
  }
  if (tab == TAB_PRS && mode == MODE_PR_DETAIL) {
    if (down & KEY_SELECT) { merge_armed = false; client_refresh_pr_detail(&client); }
    float max = prdetailview_height(&client) - conv_view_h();
    if (max < 0) max = 0;
    if (held & KEY_DUP) pr_detail_scroll -= 6;
    if (held & KEY_DDOWN) pr_detail_scroll += 6;
    if (down & KEY_DLEFT) pr_detail_scroll -= conv_view_h();
    if (down & KEY_DRIGHT) pr_detail_scroll += conv_view_h();
    circlePosition cp;
    hidCircleRead(&cp);
    if (cp.dy > 20 || cp.dy < -20) pr_detail_scroll -= (float)cp.dy / 10;
    if (pr_detail_scroll < 0) pr_detail_scroll = 0;
    if (pr_detail_scroll > max) pr_detail_scroll = max;
  }
  if (tab == TAB_THREADS && mode == MODE_THREAD && conv_scroll <= 1 && (down & KEY_DLEFT) &&
      client.conv.has_more) {
    conv_follow = false;
    client_load_history(&client);
  }

  int count = 0;
  ListState *list = active_list(&count);
  if (list) {
    int step = 0;
    if (down & KEY_DUP) step = -1;
    if (down & KEY_DDOWN) step = 1;
    if (down & KEY_DLEFT) step = -5;
    if (down & KEY_DRIGHT) step = 5;
    if (step) select_row(list, count, list->sel + step < 0 ? 0 : (list->sel + step >= count ? count - 1 : list->sel + step));
    circlePosition cp;
    hidCircleRead(&cp);
    if (cp.dy > 40 || cp.dy < -40) {
      list->scroll -= (float)cp.dy / 16.0f;
      list_clamp_scroll(list, count);
    }
  }
  if (tab == TAB_THREADS && mode == MODE_THREAD) {
    float line = gfx_line_h(SCALE_BODY);
    if (held & KEY_DUP) conv_scroll -= line / 2;
    if (held & KEY_DDOWN) conv_scroll += line / 2;
    if (down & KEY_DLEFT) conv_scroll -= conv_view_h() - line;
    if (down & KEY_DRIGHT) conv_scroll += conv_view_h() - line;
    circlePosition cp;
    hidCircleRead(&cp);
    if (cp.dy > 20 || cp.dy < -20) conv_scroll -= (float)cp.dy / 10.0f;
    if (conv_scroll < 0) conv_scroll = 0;
    if (conv_scroll > conv_max_scroll()) conv_scroll = conv_max_scroll();
  }

  // Touch: tabs, action buttons, list taps, and drag to scroll.
  if (down & KEY_TOUCH) {
    touch_state.active = true;
    touch_state.dragging = false;
    touch_state.start_x = touch_state.last_x = touch->px;
    touch_state.start_y = touch_state.last_y = touch->py;
    touch_state.start_scroll = mode == MODE_THREAD ? conv_scroll
                              : mode == MODE_THREAD_MENU ? (float)thread_menu_first : (list ? list->scroll : 0);
  } else if ((held & KEY_TOUCH) && touch_state.active) {
    touch_state.last_x = touch->px;
    touch_state.last_y = touch->py;
    int dy = touch_state.last_y - touch_state.start_y;
    if (abs(dy) > 6 && touch_state.start_y > TAB_H && touch_state.start_y < ACTION_Y) touch_state.dragging = true;
    if (touch_state.dragging) {
      if (mode == MODE_THREAD) {
        conv_scroll = touch_state.start_scroll - (float)dy * 1.5f;
        if (conv_scroll < 0) conv_scroll = 0;
        if (conv_scroll > conv_max_scroll()) conv_scroll = conv_max_scroll();
      } else if (mode == MODE_THREAD_MENU) {
        Action a[MAX_THREAD_ACTIONS];
        int n = build_thread_menu(a);
        int first = (int)touch_state.start_scroll - dy / MENU_ROW_H;
        int max = n > MENU_ROWS ? n - MENU_ROWS : 0;
        thread_menu_first = first < 0 ? 0 : first > max ? max : first;
        thread_menu_sel = thread_menu_first;
      } else if (list) {
        list->scroll = touch_state.start_scroll - (float)dy;
        list_clamp_scroll(list, count);
      }
    }
  } else if (touch_state.active && !(held & KEY_TOUCH)) {
    touch_state.active = false;
    int x = touch_state.last_x, y = touch_state.last_y;
    if (!touch_state.dragging) {
      if (y < TAB_H) {
        Tab next = (Tab)(x * TAB_COUNT / BOT_W);
        if (next != tab) switch_tab((int)next - (int)tab);
      } else if (y >= ACTION_Y) {
        for (int i = 0; i < na; i++)
          if (actions[i].enabled && rect_hit(action_rect(i, na), x, y)) run_action(actions[i].id);
      } else if (tab == TAB_THREADS && mode == MODE_THREAD_MENU && y >= MENU_Y && y < MENU_Y + MENU_ROWS * MENU_ROW_H) {
        Action a[MAX_THREAD_ACTIONS];
        int n = build_thread_menu(a);
        int row = thread_menu_first + (y - MENU_Y) / MENU_ROW_H;
        if (row < n) {
          thread_menu_sel = row;
          run_action(ACT_MENU_PICK);
        }
      } else if (tab == TAB_THREADS && mode == MODE_THREAD && current_thread() &&
                 pending_is_approval(current_thread()) && current_thread()->pending_blocking && rect_hit(more_rect(), x, y)) {
        run_action(ACT_MORE);
      } else if (tab == TAB_THREADS && mode == MODE_THREAD && y >= nav_top && y < nav_top + nav_rows * NAV_ROW_H) {
        int i = nav_first + (int)((y - nav_top) / NAV_ROW_H);
        conv_scroll = convview_turn_y(i) - 4;
        if (conv_scroll < 0) conv_scroll = 0;
        if (conv_scroll > conv_max_scroll()) conv_scroll = conv_max_scroll();
        conv_follow = false;
      } else if (list && y >= LIST_Y) {
        int row = (int)(((float)(y - LIST_Y) + list->scroll) / ROW_H);
        if (row == list->sel && row < count) {
          // A second tap on the selected row acts like A.
          if (tab == TAB_THREADS && mode == MODE_LIST) run_action(ACT_OPEN);
          else if (tab == TAB_THREADS && (mode == MODE_PICK_PROJECT || mode == MODE_PICK_MODEL || mode == MODE_PICK_SNOOZE ||
                                                 mode == MODE_PICK_SUBAGENT)) run_action(ACT_PICK);
          else if (tab == TAB_MACHINES) run_action(ACT_CONNECT);
          else if (tab == TAB_PRS) run_action(ACT_PR_OPEN);
          else if (tab == TAB_TASKS) run_action(ACT_TASK_RUN);
        } else {
          select_row(list, count, row);
        }
      }
    }
    return;
  }

  for (int i = 0; i < na; i++) {
    if (actions[i].key && (down & actions[i].key) && actions[i].enabled) {
      run_action(actions[i].id);
      break;
    }
  }
}

// ---------------------------------------------------------------- drawing: shared

static void draw_top_bar(const char *title) {
  gfx_rect(0, 0, TOP_W, TOP_BAR_H, COL_PANEL);
  u32 dot = client.state == CONN_READY ? COL_OK
            : client.state == CONN_ERROR ? COL_ERROR
            : client.state == CONN_IDLE  ? COL_DIM
                                         : COL_ATTENTION;
  gfx_dot(9, TOP_BAR_H / 2.0f, 3.5f, dot);
  const char *env = client.env.label[0] ? client.env.label : "No machine";
  float w = gfx_text_fit(17, 2, SCALE_SMALL, COL_MUTED, env, 150);
  if (title) gfx_text_fit(17 + w + 8, 2, SCALE_SMALL, COL_TEXT, title, TOP_W - w - 90);
  u8 wifi = osGetWifiStrength();
  char right[32];
  // The 3DS clock already runs on local time.
  int64_t t = (int64_t)time(NULL);
  snprintf(right, sizeof right, "%02d:%02d   %s", (int)(t % 86400 / 3600), (int)(t % 3600 / 60),
           wifi == 0 ? "no wifi" : wifi == 1 ? "wifi *" : wifi == 2 ? "wifi **" : "wifi ***");
  gfx_text_right(TOP_W - 6, 2, SCALE_SMALL, COL_DIM, right);
}

static void draw_toast(void) {
  if (!client.toast[0]) return;
  float w = gfx_text_w(client.toast, -1, SCALE_BODY) + 24;
  if (w > TOP_W - 20) w = TOP_W - 20;
  float x = (TOP_W - w) / 2, y = SCREEN_H - 30;
  gfx_pill(x, y, w, 22, COL_PANEL_HI);
  gfx_text_fit(x + 12, y + 3, SCALE_BODY, COL_TEXT, client.toast, w - 24);
}

static void draw_center_message(float w, const char *headline, const char *detail) {
  float y = SCREEN_H / 2.0f - 24;
  gfx_text((w - gfx_text_w(headline, -1, SCALE_TITLE)) / 2, y, SCALE_TITLE, COL_TEXT, headline, -1);
  if (detail) {
    WrapLine lines[6];
    int n = gfx_wrap(detail, SCALE_SMALL, w - 40, lines, 6);
    for (int i = 0; i < n; i++)
      gfx_text((w - gfx_text_w(lines[i].start, lines[i].len, SCALE_SMALL)) / 2, y + 26 + (float)i * 14, SCALE_SMALL,
               COL_MUTED, lines[i].start, lines[i].len);
  }
}

// Returns false when there is no usable connection to draw data for.
static bool draw_connection_gate(float w) {
  if (client.state == CONN_READY) return true;
  bool has_data = client.thread_count > 0 || client.project_count > 0;
  if (store.count == 0) {
    draw_center_message(w, "No machines yet", "Open the Hosts tab and pair with a T3 Code server on your network.");
    return false;
  }
  if (client.state == CONN_IDLE) {
    draw_center_message(w, "Not connected", "Choose a machine in the Hosts tab.");
    return false;
  }
  if (has_data) return true;  // Reconnecting: keep showing the last data.
  if (client.state == CONN_ERROR) {
    draw_center_message(w, client.auth_expired ? "Pairing expired" : "Can't reach machine", client.error);
    return false;
  }
  draw_center_message(w, client.state == CONN_SYNCING ? "Loading threads..." : "Connecting...", client.env.addr);
  return false;
}

static void draw_tabs(void) {
  static const char *names[TAB_COUNT] = {"Threads", "Usage", "PRs", "Hosts", "Tasks"};
  gfx_rect(0, 0, BOT_W, TAB_H, COL_PANEL);
  float w = (float)BOT_W / TAB_COUNT;
  for (int i = 0; i < TAB_COUNT; i++) {
    bool on = i == (int)tab;
    float scale = SCALE_BODY;
    float tw = gfx_text_w(names[i], -1, scale);
    if (tw > w - 6) { scale *= (w - 6) / tw; tw = w - 6; }
    gfx_text((float)i * w + (w - tw) / 2, 4, scale, on ? COL_TEXT : COL_MUTED, names[i], -1);
    if (on) gfx_rect((float)i * w + 10, TAB_H - 2, w - 20, 2, COL_ACCENT);
  }
}

static void draw_actions(void) {
  Action a[6];
  int n = build_actions(a);
  for (int i = 0; i < n; i++) gfx_button(action_rect(i, n), n > 4 ? NULL : a[i].hint, a[i].label, a[i].enabled, a[i].primary);
}

static void draw_scrollbar(float x, float y, float h, float scroll, float content) {
  if (content <= h) return;
  float bh = h * h / content;
  if (bh < 12) bh = 12;
  float by = y + (h - bh) * (scroll / (content - h));
  gfx_rect(x, by, 3, bh, COL_LINE);
}

// Clips list rows to the list band by drawing the chrome over overflow.
static void list_frame_end(int count, ListState *l) {
  gfx_rect(0, ACTION_Y - 4, BOT_W, SCREEN_H - ACTION_Y + 4, COL_BG);
  draw_scrollbar(BOT_W - 4, LIST_Y, LIST_H, l->scroll, (float)count * ROW_H);
}

static bool row_visible(ListState *l, int i, float *y) {
  *y = LIST_Y + (float)i * ROW_H - l->scroll;
  return *y > LIST_Y - ROW_H && *y < LIST_Y + LIST_H;
}

// ---------------------------------------------------------------- threads

static void draw_thread_rows(void) {
  int64_t now = client_now(&client);
  for (int i = 0; i < thread_view_count; i++) {
    float y;
    if (!row_visible(&thread_list, i, &y)) continue;
    T3Thread *t = &client.threads[thread_view[i]];
    if (i == thread_list.sel) gfx_rect(0, y, BOT_W, ROW_H - 1, COL_PANEL_HI);
    gfx_dot(10, y + 10, 4, thread_color(t));
    if (t->unread) gfx_dot(10, y + 24, 2.5f, COL_ACCENT);
    char ago[16];
    util_format_ago(t->updated_at, now, ago, sizeof ago);
    float aw = gfx_text_w(ago, -1, SCALE_SMALL);
    float title_x = 20;
    if (t->pinned_at) {
      gfx_rect(22, y + 6, 7, 2, COL_ACCENT);
      gfx_tri(23, y + 8, 28, y + 8, 25.5f, y + 12, COL_ACCENT);
      gfx_rect(25, y + 12, 1, 4, COL_ACCENT);
      title_x += 13;
    }
    gfx_text_fit(title_x, y + 3, SCALE_BODY, COL_TEXT, t->title[0] ? t->title : "Untitled", BOT_W - 14 - title_x - aw);
    gfx_text_right(BOT_W - 8, y + 4, SCALE_SMALL, COL_DIM, ago);
    Project *p = client_find_project(&client, t->project_id);
    char meta[160];
    snprintf(meta, sizeof meta, "%s%s  %s", t->archived ? "Archived / " : "", thread_label(t), p ? p->title : "");
    gfx_text_fit(20, y + 18, SCALE_SMALL, thread_color(t) == COL_DIM ? COL_MUTED : thread_color(t), meta, BOT_W - 30);
    gfx_rect(20, y + ROW_H - 1, BOT_W - 20, 1, COL_LINE);
  }
  if (thread_view_count == 0) {
    gfx_text(16, LIST_Y + 12, SCALE_BODY, COL_MUTED,
             hidden_done ? "Done, archived, and snoozed threads are hidden." : "No threads yet.", -1);
  }
  if (hidden_done || show_done) {
    char foot[64];
    snprintf(foot, sizeof foot, show_done ? "SELECT: hide done / snoozed" : "SELECT: show %d hidden", hidden_done);
    float y = LIST_Y + (float)thread_view_count * ROW_H - thread_list.scroll + 6;
    if (y < LIST_Y + LIST_H - 10) gfx_text(16, y, SCALE_SMALL, COL_DIM, foot, -1);
  }
  const char *foot = show_done ? "SELECT: hide done + archived" : "SELECT: show done + archived";
  float foot_y = LIST_Y + (float)thread_view_count * ROW_H - thread_list.scroll + (thread_view_count ? 6 : 38);
  if (foot_y < LIST_Y + LIST_H - 10) gfx_text(16, foot_y, SCALE_SMALL, COL_DIM, foot, -1);
  list_frame_end(thread_view_count, &thread_list);
}

static float draw_wrapped(float x, float y, float w, float scale, u32 color, const char *s, int max_lines) {
  WrapLine lines[16];
  if (max_lines > 16) max_lines = 16;
  int n = gfx_wrap(s, scale, w, lines, max_lines);
  float h = gfx_line_h(scale);
  for (int i = 0; i < n; i++) {
    bool last = i == max_lines - 1 && lines[i].start + lines[i].len < s + strlen(s);
    if (last) {
      char tmp[256];
      int len = lines[i].len < 250 ? lines[i].len : 250;
      memcpy(tmp, lines[i].start, (size_t)len);
      tmp[len] = '\0';
      gfx_text_fit(x, y, scale, color, tmp, w - gfx_text_w("...", 3, scale));
      gfx_text(x + gfx_text_w(tmp, -1, scale), y, scale, color, "...", 3);
    } else {
      gfx_text(x, y, scale, color, lines[i].start, lines[i].len);
    }
    y += h;
  }
  return y;
}

static void draw_thread_detail_top(T3Thread *t) {
  draw_top_bar(NULL);
  if (!draw_connection_gate(TOP_W)) return;
  if (!t) {
    draw_center_message(TOP_W, "No thread selected", "Threads that need you are listed first.");
    return;
  }
  float x = 14, y = TOP_BAR_H + 10;
  float pill_w = gfx_text_w(thread_label(t), -1, SCALE_SMALL) + 16;
  gfx_pill(x, y, pill_w, 16, thread_color(t));
  gfx_text(x + 8, y + 1, SCALE_SMALL, COL_BG, thread_label(t), -1);
  Project *p = client_find_project(&client, t->project_id);
  gfx_text_fit(x + pill_w + 8, y + 1, SCALE_SMALL, COL_MUTED, p ? p->title : "", TOP_W - pill_w - 40);
  y += 22;
  y = draw_wrapped(x, y, TOP_W - 28, SCALE_TITLE, COL_TEXT, t->title[0] ? t->title : "Untitled", 2) + 4;

  char meta[200];
  snprintf(meta, sizeof meta, "%s%s%s%s%s", t->model, t->branch[0] ? "  on " : "", t->branch, t->pr[0] ? "  PR " : "",
           t->pr);
  gfx_text_fit(x, y, SCALE_SMALL, COL_MUTED, meta, TOP_W - 28);
  y += 14;
  snprintf(meta, sizeof meta, "%s / %s", t->interaction_mode, t->runtime_mode);
  gfx_text_fit(x, y, SCALE_SMALL, COL_DIM, meta, TOP_W - 28);
  y += 18;
  gfx_rect(x, y, TOP_W - 28, 1, COL_LINE);
  y += 8;

  if (t->pending_id[0] && t->pending_blocking) {
    const char *msg = pending_is_approval(t) ? "The agent is waiting for your approval. Open the thread to review it."
                                             : "The agent asked a question. Open the thread to read it, then reply.";
    y = draw_wrapped(x, y, TOP_W - 28, SCALE_BODY, COL_ATTENTION, msg, 3) + 6;
  }
  if (t->last_error[0] && !strcmp(t->status, "failed")) y = draw_wrapped(x, y, TOP_W - 28, SCALE_SMALL, COL_ERROR, t->last_error, 3) + 6;
  if (t->last_text[0]) {
    gfx_text(x, y, SCALE_SMALL, COL_DIM, !strcmp(t->last_role, "user") ? "You said" : "Latest from the agent", -1);
    y += 15;
    int lines = (int)((SCREEN_H - 34 - y) / gfx_line_h(SCALE_BODY));
    if (lines > 0) draw_wrapped(x, y, TOP_W - 28, SCALE_BODY, COL_TEXT, t->last_text, lines);
  }
  gfx_text(x, SCREEN_H - 16, SCALE_SMALL, COL_DIM, GLYPH_A " open   " GLYPH_X " reply   " GLYPH_Y " stop   " GLYPH_DPAD " select", -1);
}

static void draw_thread_header(T3Thread *t) {
  draw_top_bar(t ? t->title : NULL);
  gfx_rect(0, TOP_BAR_H, TOP_W, THREAD_BAR_H - TOP_BAR_H, COL_PANEL);
  if (t) {
    char modes[80];
    snprintf(modes, sizeof modes, "%s / %s%s", t->interaction_mode, t->runtime_mode, t->archived ? " / archived" : "");
    gfx_text_fit(10, TOP_BAR_H, SCALE_SMALL, COL_MUTED, modes, TOP_W - 20);
  }
}

static void draw_conversation_top(T3Thread *t) {
  draw_thread_header(t);
  if (client.conv_state == LOAD_BUSY && client.conv.count == 0) {
    draw_center_message(TOP_W, "Loading conversation...", NULL);
    return;
  }
  if (client.conv_state == LOAD_FAILED && client.conv.count == 0) {
    draw_center_message(TOP_W, "Couldn't load this thread", client.conv_error);
    return;
  }
  sync_conversation_layout();
  convview_draw(t, THREAD_BAR_H, conv_view_h(), conv_scroll, client_now(&client));
  if (conv_scroll <= 1 && (client.conv.has_more || client.conv.limited)) {
    gfx_rect(0, THREAD_BAR_H, TOP_W, 22, COL_PANEL);
    gfx_text_fit(10, THREAD_BAR_H + 3, SCALE_SMALL, COL_ACCENT,
                 client.conv.limited ? "History memory limit reached" : client.history_state == LOAD_BUSY ? "Loading earlier messages..."
                   : "LEFT Load earlier", TOP_W - 20);
  }
  if (client.conv.count == 0) draw_center_message(TOP_W, "No messages yet", NULL);
  // Redraw the bar over lines that scrolled beneath it.
  draw_thread_header(t);
  draw_scrollbar(TOP_W - 4, THREAD_BAR_H, conv_view_h(), conv_scroll, convview_height());
}

static void draw_thread_menu(void) {
  Action a[MAX_THREAD_ACTIONS];
  int n = build_thread_menu(a);
  gfx_text(12, LIST_Y + 3, SCALE_BODY, COL_TEXT, "Thread actions", -1);
  char hint[48];
  snprintf(hint, sizeof hint, "%d/%d  D-pad or drag", n ? thread_menu_sel + 1 : 0, n);
  gfx_text_right(BOT_W - 12, LIST_Y + 5, SCALE_SMALL, COL_DIM, hint);
  for (int row = 0; row < MENU_ROWS && row + thread_menu_first < n; row++) {
    int i = row + thread_menu_first;
    float y = MENU_Y + row * MENU_ROW_H;
    if (i == thread_menu_sel) gfx_rect(6, y, BOT_W - 14, MENU_ROW_H - 1, COL_PANEL_HI);
    gfx_text_fit(14, y + 2, SCALE_BODY, a[i].enabled ? COL_TEXT : COL_DIM, a[i].label, BOT_W - 30);
  }
  draw_scrollbar(BOT_W - 4, MENU_Y, MENU_ROWS * MENU_ROW_H, (float)(thread_menu_first * MENU_ROW_H), (float)(n * MENU_ROW_H));
}

static const ConvItem *pending_item(T3Thread *t) {
  for (int i = client.conv.count - 1; i >= 0; i--) {
    const ConvItem *it = &client.conv.items[i];
    if ((it->kind == ITEM_APPROVAL || it->kind == ITEM_QUESTION) && !strcmp(it->request_id, t->pending_id)) return it;
  }
  return NULL;
}

static void draw_thread_bottom(T3Thread *t) {
  float x = 10, y = LIST_Y + 6;
  if (!t) {
    gfx_text(x, y, SCALE_BODY, COL_MUTED, "This thread is no longer active.", -1);
    return;
  }
  gfx_dot(x + 4, y + 8, 4, thread_color(t));
  gfx_text(x + 14, y + 1, SCALE_BODY, thread_color(t) == COL_DIM ? COL_MUTED : thread_color(t), thread_label(t), -1);
  if (pending_is_approval(t) && t->pending_blocking) {
    gfx_button(more_rect(), NULL, "More", true, false);
  } else {
    char right[48];
    snprintf(right, sizeof right, "%d items", t->item_count);
    gfx_text_right(BOT_W - 10, y + 2, SCALE_SMALL, COL_DIM, right);
  }
  y += 22;
  if (t->pending_id[0] && t->pending_blocking) {
    const ConvItem *it = pending_item(t);
    gfx_rect(6, y - 2, BOT_W - 12, ACTION_Y - y - 6, COL_PANEL);
    const char *text = it ? it->text
                          : (pending_is_approval(t) ? "Waiting for approval." : "The agent asked a question.");
    y = draw_wrapped(x + 2, y + 2, BOT_W - 26, SCALE_SMALL, COL_ATTENTION, text, 7);
    if (!pending_is_approval(t))
      gfx_text(x + 2, ACTION_Y - 22, SCALE_SMALL, COL_MUTED, threadpanels_can_answer(&client, t) ? "A: answer the questions." : "Use another device for the full question form.", -1);
    return;
  }
  Project *p = client_find_project(&client, t->project_id);
  char line[300];
  snprintf(line, sizeof line, "%s  \xC2\xB7  %s", p ? p->title : "", t->model[0] ? t->model : t->provider);
  gfx_text_fit(x, y, SCALE_SMALL, COL_MUTED, line, BOT_W - 20);
  y += 14;
  if (t->branch[0] || t->pr[0]) {
    snprintf(line, sizeof line, "%s%s%s", t->branch, t->branch[0] && t->pr[0] ? "  \xC2\xB7  PR " : t->pr[0] ? "PR " : "", t->pr);
    gfx_text_fit(x, y, SCALE_SMALL, COL_DIM, line, BOT_W - 20);
    y += 14;
  }
  if (t->last_error[0] && !strcmp(t->status, "failed"))
    y = draw_wrapped(x, y, BOT_W - 20, SCALE_SMALL, COL_ERROR, t->last_error, 2);
  if (client.history_loaded) {
    gfx_text(x, y, SCALE_SMALL, COL_DIM, "Earlier history; reopen thread for latest", -1);
    y += 14;
  }
  y += 4;
  gfx_rect(x, y, BOT_W - 20, 1, COL_LINE);
  y += 5;
  gfx_text(x, y, SCALE_SMALL, COL_TEXT, "Your messages", -1);
  if (convview_fold_count())
    gfx_text_right(BOT_W - 10, y, SCALE_SMALL, COL_DIM, convview_folds_open() ? "SELECT hide steps" : "SELECT show steps");
  y += 16;
  nav_top = y;
  int total = convview_turn_count();
  nav_rows = (int)((ACTION_Y - 6 - y) / NAV_ROW_H);
  if (nav_rows > total) nav_rows = total;
  int current = 0;
  for (int i = 0; i < total; i++)
    if (convview_turn_y(i) <= conv_scroll + 40) current = i;
  nav_first = current - nav_rows / 2;
  if (nav_first > total - nav_rows) nav_first = total - nav_rows;
  if (nav_first < 0) nav_first = 0;
  for (int r = 0; r < nav_rows; r++) {
    int i = nav_first + r;
    float ry = y + (float)r * NAV_ROW_H;
    if (i == current) gfx_rrect(4, ry - 1, BOT_W - 8, NAV_ROW_H - 1, 4, COL_PANEL_HI);
    char num[8];
    snprintf(num, sizeof num, "%d", i + 1);
    gfx_text_right(x + 14, ry + 1, SCALE_SMALL, i == current ? COL_ACCENT : COL_DIM, num);
    gfx_text_fit(x + 20, ry + 1, SCALE_SMALL, i == current ? COL_TEXT : COL_MUTED, convview_turn_text(i), BOT_W - 40);
  }
  if (total == 0) gfx_text(x, y, SCALE_SMALL, COL_DIM, "Tap a message here to jump to it.", -1);
}

static void draw_project_picker(void) {
  for (int i = 0; i < client.project_count; i++) {
    float y;
    if (!row_visible(&project_list, i, &y)) continue;
    Project *p = &client.projects[i];
    if (i == project_list.sel) gfx_rect(0, y, BOT_W, ROW_H - 1, COL_PANEL_HI);
    gfx_text_fit(12, y + 3, SCALE_BODY, COL_TEXT, p->title, BOT_W - 24);
    gfx_text_fit(12, y + 18, SCALE_SMALL, COL_DIM, p->root, BOT_W - 24);
  }
  list_frame_end(client.project_count, &project_list);
}

static void draw_model_picker(void) {
  int count = client.catalog.model_count + (model_for_new ? 1 : 0);
  for (int i = 0; i < count; i++) {
    float y;
    if (!row_visible(&model_list, i, &y)) continue;
    if (i == model_list.sel) gfx_rect(0, y, BOT_W, ROW_H - 1, COL_PANEL_HI);
    int index = i - (model_for_new ? 1 : 0);
    const char *label = "Use project default";
    const char *detail = "Reuse newest thread; otherwise project or first model";
    if (index >= 0) {
      CatalogModel *m = &client.catalog.models[index];
      label = m->label;
      detail = client.catalog.providers[m->provider].name;
    }
    gfx_text_fit(12, y + 3, SCALE_BODY, COL_TEXT, label, BOT_W - 24);
    gfx_text_fit(12, y + 18, SCALE_SMALL, COL_DIM, detail, BOT_W - 24);
  }
  if (!count) gfx_text(12, LIST_Y + 10, SCALE_BODY, COL_MUTED, "No models available. Refresh Usage.", -1);
  list_frame_end(count, &model_list);
}

static void draw_subagent_rows(void) {
  int64_t now = client_now(&client);
  for (int i = 0; i < subagent_count; i++) {
    float y;
    if (!row_visible(&subagent_list, i, &y)) continue;
    T3Thread *t = &client.threads[subagent_view[i]];
    if (i == subagent_list.sel) gfx_rect(0, y, BOT_W, ROW_H - 1, COL_PANEL_HI);
    gfx_dot(10, y + 10, 4, thread_color(t));
    char ago[16];
    util_format_ago(t->updated_at, now, ago, sizeof ago);
    float aw = gfx_text_w(ago, -1, SCALE_SMALL);
    gfx_text_fit(20, y + 3, SCALE_BODY, COL_TEXT, t->title[0] ? t->title : "Subagent", BOT_W - 34 - aw);
    gfx_text_right(BOT_W - 8, y + 4, SCALE_SMALL, COL_DIM, ago);
    char meta[160];
    snprintf(meta, sizeof meta, "%s  %s", thread_label(t), t->model);
    gfx_text_fit(20, y + 18, SCALE_SMALL, thread_color(t) == COL_DIM ? COL_MUTED : thread_color(t), meta, BOT_W - 30);
    gfx_rect(20, y + ROW_H - 1, BOT_W - 20, 1, COL_LINE);
  }
  if (subagent_count == 0) gfx_text(16, LIST_Y + 12, SCALE_BODY, COL_MUTED, "No subagents.", -1);
  list_frame_end(subagent_count, &subagent_list);
}

static void draw_snooze_picker(void) {
  for (int i = 0; i < 3; i++) {
    float y;
    if (!row_visible(&snooze_list, i, &y)) continue;
    if (i == snooze_list.sel) gfx_rect(0, y, BOT_W, ROW_H - 1, COL_PANEL_HI);
    gfx_text(12, y + 8, SCALE_BODY, COL_TEXT, snooze_labels[i], -1);
  }
  list_frame_end(3, &snooze_list);
}

// ---------------------------------------------------------------- usage

static void draw_usage_top(void) {
  draw_top_bar("Usage limits");
  if (!draw_connection_gate(TOP_W)) return;
  if (client.provider_count == 0) {
    draw_center_message(TOP_W, client.providers_state == LOAD_BUSY ? "Loading providers..." : "No providers enabled",
                        NULL);
    return;
  }
  int64_t now = client_now(&client);
  float y = TOP_BAR_H + 8;
  for (int i = 0; i < client.provider_count && y < SCREEN_H - 20; i++) {
    ProviderUsage *u = &client.providers[i];
    gfx_text(12, y, SCALE_BODY, COL_TEXT, u->name, -1);
    if (strcmp(u->status, "ready") != 0) gfx_text_right(TOP_W - 12, y + 1, SCALE_SMALL, COL_ATTENTION, u->status);
    y += 18;
    if (u->window_count == 0) {
      gfx_text_fit(20, y, SCALE_SMALL, COL_DIM, u->note[0] ? u->note : "No limit data reported yet", TOP_W - 32);
      y += 18;
    }
    for (int w = 0; w < u->window_count && y < SCREEN_H - 16; w++) {
      UsageWindow *win = &u->windows[w];
      float used = win->used_pct < 0 ? 0 : win->used_pct > 100 ? 100 : win->used_pct;
      float pct = 100 - used;
      u32 col = pct <= 10 ? COL_ERROR : pct <= 30 ? COL_ATTENTION : COL_ACCENT;
      gfx_text_fit(20, y, SCALE_SMALL, COL_MUTED, win->label, 90);
      float bx = 112, bw = 130;
      gfx_pill(bx, y + 3, bw, 8, COL_PANEL_HI);
      if (pct > 0) gfx_pill(bx, y + 3, bw * pct / 100.0f < 8 ? 8 : bw * pct / 100.0f, 8, col);
      char txt[48];
      snprintf(txt, sizeof txt, "%d%% left", (int)(pct + 0.5f));
      gfx_text(bx + bw + 8, y, SCALE_SMALL, COL_TEXT, txt, -1);
      char until[40];
      util_format_until(win->resets_at, now, until, sizeof until);
      gfx_text_right(TOP_W - 12, y, SCALE_SMALL, COL_DIM, until);
      y += 16;
    }
    y += 6;
  }
}

static void draw_usage_bottom(void) {
  if (!connected() && client.usage_state != LOAD_DONE) {
    gfx_text(12, LIST_Y + 10, SCALE_BODY, COL_MUTED, "Connect to a machine to see usage.", -1);
    return;
  }
  UsageSummary *u = &client.usage;
  if (client.usage_state == LOAD_FAILED) {
    draw_wrapped(12, LIST_Y + 10, BOT_W - 24, SCALE_SMALL, COL_ERROR, client.usage_error, 4);
    return;
  }
  if (!u->loaded) {
    gfx_text(12, LIST_Y + 10, SCALE_BODY, COL_MUTED, "Reading local usage logs...", -1);
    return;
  }
  double today = u->day_cost[USAGE_DAYS - 1], week = 0, month = 0, peak = 0;
  for (int i = 0; i < USAGE_DAYS; i++) {
    month += u->day_cost[i];
    if (i >= USAGE_DAYS - 7) week += u->day_cost[i];
    if (u->day_cost[i] > peak) peak = u->day_cost[i];
  }
  const char *labels[3] = {"Today", "7 days", "30 days"};
  double values[3] = {today, week, month};
  for (int i = 0; i < 3; i++) {
    float x = 10 + (float)i * 102;
    char v[24];
    snprintf(v, sizeof v, "$%.2f", values[i]);
    gfx_text(x, LIST_Y + 4, SCALE_SMALL, COL_MUTED, labels[i], -1);
    gfx_text(x, LIST_Y + 16, SCALE_TITLE, COL_TEXT, v, -1);
  }
  // 30-day spend, newest on the right.
  float cx = 10, cy = LIST_Y + 42, cw = BOT_W - 20, ch = 54;
  gfx_rect(cx, cy + ch, cw, 1, COL_LINE);
  float bw = cw / USAGE_DAYS;
  for (int i = 0; i < USAGE_DAYS && peak > 0; i++) {
    float h = (float)(u->day_cost[i] / peak) * ch;
    if (h > 0 && h < 1) h = 1;
    gfx_rect(cx + (float)i * bw + 1, cy + ch - h, bw - 2, h, i == USAGE_DAYS - 1 ? COL_ACCENT : COL_PANEL_HI);
  }
  float y = cy + ch + 6;
  for (int i = 0; i < u->provider_count && y < ACTION_Y - 30; i++) {
    char tok[16], line[96];
    util_format_tokens(u->providers[i].tokens, tok, sizeof tok);
    snprintf(line, sizeof line, "%s", u->providers[i].provider);
    gfx_text(12, y, SCALE_SMALL, COL_TEXT, line, -1);
    snprintf(line, sizeof line, "%s tokens   $%.2f", tok, u->providers[i].cost);
    gfx_text_right(BOT_W - 12, y, SCALE_SMALL, COL_MUTED, line);
    y += 14;
  }
  char foot[120];
  snprintf(foot, sizeof foot, "Top model: %s   %d sessions", u->top_model[0] ? u->top_model : "none", u->sessions);
  gfx_text_fit(12, ACTION_Y - 16, SCALE_SMALL, COL_DIM, foot, BOT_W - 24);
}

// ---------------------------------------------------------------- pull requests

static u32 checks_color(const PullRequest *pr) {
  if (!strcmp(pr->checks, "failing")) return COL_ERROR;
  if (!strcmp(pr->checks, "passing")) return COL_OK;
  if (!strcmp(pr->checks, "pending")) return COL_ATTENTION;
  return COL_DIM;
}

static void draw_pr_rows(void) {
  int64_t now = client_now(&client);
  for (int i = 0; i < pr_view_count; i++) {
    float y;
    if (!row_visible(&pr_list, i, &y)) continue;
    PullRequest *pr = &client.prs[pr_view[i]];
    if (i == pr_list.sel) gfx_rect(0, y, BOT_W, ROW_H - 1, COL_PANEL_HI);
    gfx_dot(10, y + 10, 4, checks_color(pr));
    char num[16], ago[16], meta[200];
    snprintf(num, sizeof num, "#%d", pr->number);
    float nw = gfx_text_w(num, -1, SCALE_SMALL) + 6;
    gfx_text(20, y + 4, SCALE_SMALL, COL_DIM, num, -1);
    util_format_ago(pr->updated_at, now, ago, sizeof ago);
    float aw = gfx_text_w(ago, -1, SCALE_SMALL);
    gfx_text_fit(20 + nw, y + 3, SCALE_BODY, pr->draft ? COL_MUTED : COL_TEXT, pr->title, BOT_W - 36 - nw - aw);
    gfx_text_right(BOT_W - 8, y + 4, SCALE_SMALL, COL_DIM, ago);
    const char *slash = strrchr(pr->repo, '/');
    snprintf(meta, sizeof meta, "%s  @%s%s%s", slash ? slash + 1 : pr->repo, pr->author,
             pr->review_requested ? "  review requested" : "", pr->draft ? "  draft" : "");
    gfx_text_fit(20, y + 18, SCALE_SMALL, pr->review_requested ? COL_ATTENTION : COL_MUTED, meta, BOT_W - 30);
    gfx_rect(20, y + ROW_H - 1, BOT_W - 20, 1, COL_LINE);
  }
  if (pr_view_count == 0) {
    const char *msg = client.prs_state == LOAD_BUSY   ? "Loading pull requests..."
                      : client.prs_state == LOAD_FAILED ? "Couldn't load pull requests."
                      : review_only                      ? "Nothing is waiting on your review."
                                                         : "No open pull requests.";
    gfx_text(16, LIST_Y + 12, SCALE_BODY, COL_MUTED, msg, -1);
    if (client.prs_error[0]) draw_wrapped(16, LIST_Y + 34, BOT_W - 32, SCALE_SMALL, COL_ERROR, client.prs_error, 5);
  }
  list_frame_end(pr_view_count, &pr_list);
}

static void draw_pr_top(void) {
  draw_top_bar("Pull requests");
  if (!draw_connection_gate(TOP_W)) return;
  if (pr_view_count == 0) {
    draw_center_message(TOP_W, "Pull requests", "Open pull requests across this machine's projects appear here.");
    return;
  }
  PullRequest *pr = &client.prs[pr_view[pr_list.sel]];
  float x = 14, y = TOP_BAR_H + 10;
  char line[256];
  snprintf(line, sizeof line, "%s #%d", pr->repo, pr->number);
  gfx_text_fit(x, y, SCALE_SMALL, COL_MUTED, line, TOP_W - 28);
  y += 16;
  y = draw_wrapped(x, y, TOP_W - 28, SCALE_TITLE, COL_TEXT, pr->title, 3) + 6;
  const char *state = pr->draft ? "Draft" : !strcmp(pr->state, "open") ? "Open" : pr->state;
  float pw = gfx_text_w(state, -1, SCALE_SMALL) + 16;
  gfx_pill(x, y, pw, 16, pr->draft ? COL_DIM : COL_OK);
  gfx_text(x + 8, y + 1, SCALE_SMALL, COL_BG, state, -1);
  snprintf(line, sizeof line, "@%s wants to merge %s into %s", pr->author, pr->head, pr->base);
  gfx_text_fit(x + pw + 8, y + 1, SCALE_SMALL, COL_MUTED, line, TOP_W - pw - 40);
  y += 26;

  struct {
    const char *k;
    char v[96];
    u32 c;
  } rows[6];
  int n = 0;
  rows[n].k = "Checks";
  util_strlcpy(rows[n].v, pr->checks[0] ? pr->checks : "none reported", sizeof rows[n].v);
  rows[n++].c = checks_color(pr);
  rows[n].k = "Review";
  util_strlcpy(rows[n].v, pr->review[0] ? pr->review : "no decision", sizeof rows[n].v);
  rows[n++].c = !strcmp(pr->review, "approved") ? COL_OK : !strcmp(pr->review, "changes-requested") ? COL_ERROR : COL_TEXT;
  rows[n].k = "Merge";
  util_strlcpy(rows[n].v, pr->mergeability, sizeof rows[n].v);
  rows[n++].c = !strcmp(pr->mergeability, "conflicting") ? COL_ERROR : COL_TEXT;
  if (pr->additions || pr->deletions) {
    rows[n].k = "Changes";
    snprintf(rows[n].v, sizeof rows[n].v, "+%d  -%d", pr->additions, pr->deletions);
    rows[n++].c = COL_TEXT;
  }
  rows[n].k = "Project";
  util_strlcpy(rows[n].v, pr->project_title, sizeof rows[n].v);
  rows[n++].c = COL_TEXT;
  if (pr->labels[0]) {
    rows[n].k = "Labels";
    util_strlcpy(rows[n].v, pr->labels, sizeof rows[n].v);
    rows[n++].c = COL_MUTED;
  }
  for (int i = 0; i < n && y < SCREEN_H - 16; i++) {
    gfx_text(x, y, SCALE_SMALL, COL_DIM, rows[i].k, -1);
    gfx_text_fit(x + 70, y, SCALE_SMALL, rows[i].c, rows[i].v, TOP_W - x - 84);
    y += 16;
  }
}

// ---------------------------------------------------------------- tasks

static void draw_task_rows(void) {
  for (int i = 0; i < client.task_count; i++) {
    float y;
    if (!row_visible(&task_list, i, &y)) continue;
    ScheduledTask *task = &client.tasks[i];
    if (i == task_list.sel) gfx_rect(0, y, BOT_W, ROW_H - 1, COL_PANEL_HI);
    gfx_dot(10, y + 10, 4, task->enabled ? COL_OK : COL_DIM);
    gfx_text_fit(20, y + 3, SCALE_BODY, COL_TEXT, task->title, BOT_W - 30);
    char meta[160];
    snprintf(meta, sizeof meta, "%s  %s%s%s", task->summary, task->enabled ? "on" : "off",
             task->last_status[0] ? "  " : "", task->last_status);
    gfx_text_fit(20, y + 18, SCALE_SMALL, !strcmp(task->last_status, "failed") ? COL_ERROR : COL_MUTED, meta, BOT_W - 30);
  }
  if (!client.task_count) {
    gfx_text(16, LIST_Y + 12, SCALE_BODY, COL_MUTED,
             client.tasks_state == LOAD_BUSY ? "Loading tasks..." : "No scheduled tasks.", -1);
    if (client.tasks_error[0]) draw_wrapped(16, LIST_Y + 34, BOT_W - 32, SCALE_SMALL, COL_ERROR, client.tasks_error, 5);
  }
  list_frame_end(client.task_count, &task_list);
}

static void draw_tasks_top(void) {
  draw_top_bar("Scheduled tasks");
  if (!draw_connection_gate(TOP_W)) return;
  if (!client.task_count) {
    draw_center_message(TOP_W, "Scheduled tasks", client.tasks_error[0] ? client.tasks_error : "Run or pause tasks created on another device.");
    return;
  }
  ScheduledTask *task = &client.tasks[task_list.sel];
  float y = draw_wrapped(14, TOP_BAR_H + 12, TOP_W - 28, SCALE_TITLE, COL_TEXT, task->title, 3) + 12;
  gfx_text(14, y, SCALE_BODY, task->enabled ? COL_OK : COL_DIM, task->enabled ? "Enabled" : "Disabled", -1);
  y += 24;
  y = draw_wrapped(14, y, TOP_W - 28, SCALE_BODY, COL_MUTED, task->summary, 2) + 12;
  char status[64];
  snprintf(status, sizeof status, "Last run: %s", task->last_status[0] ? task->last_status : "not reported");
  gfx_text(14, y, SCALE_BODY, !strcmp(task->last_status, "failed") ? COL_ERROR : COL_MUTED, status, -1);
  if (client.tasks_error[0]) draw_wrapped(14, y + 26, TOP_W - 28, SCALE_SMALL, COL_ATTENTION, client.tasks_error, 2);
}

// ---------------------------------------------------------------- machines

static void draw_machine_rows(void) {
  for (int i = 0; i < store.count; i++) {
    float y;
    if (!row_visible(&env_list, i, &y)) continue;
    EnvConfig *e = &store.envs[i];
    bool current = !strcmp(e->addr, client.env.addr) && !strcmp(e->token, client.env.token);
    if (i == env_list.sel) gfx_rect(0, y, BOT_W, ROW_H - 1, COL_PANEL_HI);
    u32 dot = !current ? COL_DIM : client.state == CONN_READY ? COL_OK : client.state == CONN_ERROR ? COL_ERROR : COL_ATTENTION;
    gfx_dot(10, y + 10, 4, dot);
    gfx_text_fit(20, y + 3, SCALE_BODY, COL_TEXT, e->label, BOT_W - 30);
    gfx_text_fit(20, y + 18, SCALE_SMALL, remove_armed == i ? COL_ERROR : COL_MUTED,
                 remove_armed == i ? "Press Remove again to forget this machine" : e->addr, BOT_W - 30);
    gfx_rect(20, y + ROW_H - 1, BOT_W - 20, 1, COL_LINE);
  }
  if (store.count == 0) {
    draw_wrapped(16, LIST_Y + 12, BOT_W - 32, SCALE_BODY, COL_MUTED,
                 "No machines paired. Press X and scan a pairing QR code from the host.", 4);
  }
  list_frame_end(store.count, &env_list);
}

static void draw_scan_top(void) {
  if (qrscan_has_frame()) qrscan_draw(0, 0);
  else draw_center_message(TOP_W, "Starting camera...", NULL);
  // Corner marks show where to hold the code.
  float s = 150, x0 = (TOP_W - s) / 2, y0 = (SCREEN_H - s) / 2 - 10, l = 18, t = 3;
  for (int i = 0; i < 4; i++) {
    float cx = i & 1 ? x0 + s : x0, cy = i & 2 ? y0 + s : y0;
    gfx_rect(i & 1 ? cx - l : cx, i & 2 ? cy - t : cy, l, t, COL_ACCENT);
    gfx_rect(i & 1 ? cx - t : cx, i & 2 ? cy - l : cy, t, l, COL_ACCENT);
  }
  const char *msg = machines_note[0] ? machines_note : "Point the outer camera at the pairing QR code";
  float w = gfx_text_w(msg, -1, SCALE_SMALL) + 24;
  if (w > TOP_W - 16) w = TOP_W - 16;
  float x = (TOP_W - w) / 2, y = SCREEN_H - 28;
  gfx_pill(x, y, w, 20, COL_PANEL);
  gfx_text_fit(x + 12, y + 3, SCALE_SMALL, machines_note[0] ? COL_ATTENTION : COL_TEXT, msg, w - 24);
}

static void draw_scan_bottom(void) {
  float x = 16, y = LIST_Y + 12, w = BOT_W - 32;
  gfx_text(x, y, SCALE_BODY, COL_TEXT, "Scan a pairing link", -1);
  y = draw_wrapped(x, y + 22, w, SCALE_SMALL, COL_MUTED,
                   "On the host, open Settings > Connections, turn on Network access, and create a pairing link. "
                   "Hold its QR code in front of the outer camera.",
                   5) + 8;
  draw_wrapped(x, y, w, SCALE_SMALL, COL_DIM, "No QR code? Press X to type the address and code instead.", 3);
}

static void draw_machines_top(void) {
  draw_top_bar("Machines");
  float x = 14, y = TOP_BAR_H + 12;
  if (client.env.addr[0]) {
    const char *state = client.state == CONN_READY        ? "Connected"
                        : client.state == CONN_CONNECTING ? "Connecting"
                        : client.state == CONN_SYNCING    ? "Loading"
                        : client.state == CONN_ERROR      ? (client.auth_expired ? "Pairing expired" : "Reconnecting")
                                                          : "Not connected";
    gfx_text_fit(x, y, SCALE_TITLE, COL_TEXT, client.env.label, TOP_W - 28);
    y += 22;
    char line[200];
    snprintf(line, sizeof line, "%s  %s%s%s", state, client.env.addr, client.server_version[0] ? "  v" : "",
             client.server_version);
    gfx_text_fit(x, y, SCALE_SMALL, client.state == CONN_READY ? COL_OK : COL_ATTENTION, line, TOP_W - 28);
    y += 16;
    if (client.state == CONN_ERROR) y = draw_wrapped(x, y, TOP_W - 28, SCALE_SMALL, COL_ERROR, client.error, 3);
    if (client.state == CONN_READY) {
      int running = 0, waiting = 0;
      int threads = 0;
      for (int i = 0; i < client.thread_count; i++) {
        if (client.threads[i].subagent) continue;
        threads++;
        if (thread_rank(&client.threads[i]) == 0) waiting++;
        else if (thread_rank(&client.threads[i]) == 1) running++;
      }
      snprintf(line, sizeof line, "%d projects, %d threads, %d working, %d need you", client.project_count,
               threads, running, waiting);
      gfx_text_fit(x, y, SCALE_SMALL, COL_MUTED, line, TOP_W - 28);
      y += 16;
    }
    y += 10;
  }
  if (machines_note[0]) y = draw_wrapped(x, y, TOP_W - 28, SCALE_BODY, pairing ? COL_ATTENTION : COL_ACCENT, machines_note, 3) + 8;
  gfx_rect(x, y, TOP_W - 28, 1, COL_LINE);
  y += 10;
  gfx_text(x, y, SCALE_BODY, COL_TEXT, "Pairing a machine", -1);
  y += 20;
  y = draw_wrapped(x, y, TOP_W - 28, SCALE_SMALL, COL_MUTED,
                   "1. On the host, open Settings > Connections, turn on Network access, and create a pairing link. "
                   "Or run `t3 pair` on a command-line host.",
                   4) + 4;
  y = draw_wrapped(x, y, TOP_W - 28, SCALE_SMALL, COL_MUTED,
                   "2. Press X here and point the outer camera at the link's QR code. You can also type the host's "
                   "LAN address and port (for example 192.168.1.20:3773) and the 12-character code from the link.",
                   4) + 4;
  draw_wrapped(x, y, TOP_W - 28, SCALE_SMALL, COL_DIM,
               "The 3DS connects over plain HTTP, so use an address on your local network.", 3);
}

// ---------------------------------------------------------------- frame

static void auto_refresh(void) {
  if (!connected()) return;
  T3Thread *t = current_thread();
  if (client.archives_state == LOAD_NONE && (show_done || (t && t->archived))) client_load_archived(&client);
  uint64_t now = now_ms();
  if (tab == TAB_USAGE && client.usage_state != LOAD_BUSY &&
      (client.usage_state == LOAD_NONE || now - client.usage_loaded_ms > USAGE_REFRESH_MS))
    client_refresh_usage(&client);
  if (tab == TAB_TASKS && (!tasks_opened || client.tasks_state == LOAD_NONE)) {
    tasks_opened = true;
    client_refresh_tasks(&client);
  }
  if (tab != TAB_TASKS) tasks_opened = false;
  if (tab == TAB_PRS && mode == MODE_PR_DETAIL && client.pr_detail_state == LOAD_NONE)
    client_refresh_pr_detail(&client);
  if (tab == TAB_PRS && client.prs_state != LOAD_BUSY &&
      (client.prs_state == LOAD_NONE || now - client.prs_loaded_ms > PR_REFRESH_MS))
    client_refresh_prs(&client);
  if (client.launched_thread_id[0]) {
    T3Thread *t = client_find_thread(&client, client.launched_thread_id);
    if (t) {
      client.launched_thread_id[0] = '\0';
      tab = TAB_THREADS;
      open_thread(t);
    }
  }
}

void ui_init(void) {
  client_init(&client);
  store_load(&store, STORE_PATH);
  env_list.sel = store.last_used;
  if (store.count > 0) {
    client_connect(&client, &store.envs[store.last_used]);
    tab = TAB_THREADS;
  } else {
    tab = TAB_MACHINES;
  }
}

void ui_shutdown(void) {
  qrscan_stop();
  client_disconnect(&client);
  free(thread_view);
  free(pr_view);
}

void ui_frame(u32 down, u32 held, const touchPosition *touch) {
  bg_poll();
  client_update(&client);
  rebuild_thread_view();
  rebuild_pr_view();
  list_keep_visible(&env_list, store.count);
  if (mode == MODE_PICK_MODEL) list_keep_visible(&model_list, client.catalog.model_count + (model_for_new ? 1 : 0));
  if (mode == MODE_PICK_PROJECT) list_keep_visible(&project_list, client.project_count);
  list_clamp_scroll(&task_list, client.task_count);
  if (task_list.sel >= client.task_count) task_list.sel = client.task_count ? client.task_count - 1 : 0;
  auto_refresh();
  scan_update();

  // Input runs before the frame starts: the software keyboard cannot open mid-frame.
  handle_input(down, held, touch);
  rebuild_thread_view();
  T3Thread *t = current_thread();

  gfx_frame_begin();
  gfx_target_top();
  if (threadpanels_active()) threadpanels_draw_top(&client);
  else switch (tab) {
    case TAB_THREADS:
      if (mode == MODE_DIFF) {
        draw_top_bar("Changes");
        diffview_draw_top(&client);
      } else if (mode == MODE_PICK_WORKSPACE) {
        draw_top_bar("New thread");
        launchview_draw_top(&client);
      } else if (mode == MODE_THREAD || mode == MODE_THREAD_MENU) {
        draw_conversation_top(t);
      } else if (mode == MODE_PICK_PROJECT) {
        draw_top_bar("New thread");
        draw_center_message(TOP_W, "Pick a project",
                            "Next, choose a model or reuse the project's newest thread, then the project folder or a new worktree.");
      } else if (mode == MODE_PICK_MODEL) {
        draw_top_bar(model_for_new ? "New thread" : "Model");
        draw_center_message(TOP_W, "Pick a model", "Each model lists the provider instance it will use.");
      } else if (mode == MODE_PICK_SUBAGENT) {
        if (subagent_list.sel < subagent_count) draw_thread_detail_top(&client.threads[subagent_view[subagent_list.sel]]);
        else draw_top_bar("Subagents");
      } else if (mode == MODE_PICK_SNOOZE) {
        draw_top_bar("Snooze");
        draw_center_message(TOP_W, "Wake this thread later", "Snoozed threads are hidden until they wake. SELECT shows them in the thread list.");
      } else {
        draw_thread_detail_top(t);
      }
      break;
    case TAB_USAGE: draw_usage_top(); break;
    case TAB_PRS:
      if (mode == MODE_PR_DETAIL) {
        prdetailview_draw(&client, pr_detail_scroll);
        draw_top_bar("Pull request");
        draw_scrollbar(TOP_W - 4, TOP_BAR_H, conv_view_h(), pr_detail_scroll, prdetailview_height(&client));
      } else draw_pr_top();
      break;
    case TAB_TASKS: draw_tasks_top(); break;
    case TAB_MACHINES:
      if (mode == MODE_SCAN) draw_scan_top();
      else draw_machines_top();
      break;
    default: break;
  }
  draw_toast();

  gfx_target_bottom();
  if (threadpanels_active()) threadpanels_draw_bottom(&client, t);
  else if (tab == TAB_THREADS) {
    if (mode == MODE_THREAD_MENU) draw_thread_menu();
    else if (mode == MODE_DIFF) diffview_draw_bottom(&client);
    else if (mode == MODE_PICK_WORKSPACE) launchview_draw_bottom(&client);
    else if (mode == MODE_THREAD) draw_thread_bottom(t);
    else if (mode == MODE_PICK_PROJECT) draw_project_picker();
    else if (mode == MODE_PICK_MODEL) draw_model_picker();
    else if (mode == MODE_PICK_SNOOZE) draw_snooze_picker();
    else if (mode == MODE_PICK_SUBAGENT) draw_subagent_rows();
    else if (connected() || client.thread_count) draw_thread_rows();
  } else if (tab == TAB_USAGE) {
    draw_usage_bottom();
  } else if (tab == TAB_PRS) {
    if (mode == MODE_PR_DETAIL) prdetailview_bottom(&client, merge_armed);
    else if (connected() || client.pr_count) draw_pr_rows();
  } else if (tab == TAB_TASKS) {
    draw_task_rows();
  } else if (tab == TAB_MACHINES) {
    if (mode == MODE_SCAN) draw_scan_bottom();
    else draw_machine_rows();
  }
  if (!threadpanels_active() && mode != MODE_DIFF && mode != MODE_PICK_WORKSPACE) {
    draw_tabs();
    draw_actions();
  }
  gfx_frame_end();
}
