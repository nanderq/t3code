#pragma once

#include <stdbool.h>

// Runs blocking network work off the UI thread. `work` runs on the worker,
// `done` runs later on the main thread from bg_poll. Jobs run one at a time in
// submission order.
typedef void (*BgFn)(void *arg);

void bg_init(void);
void bg_shutdown(void);
void bg_submit(BgFn work, BgFn done, void *arg);
void bg_poll(void);
bool bg_busy(void);
