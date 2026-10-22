#pragma once

#include <stdint.h>

// Presets: one hour, three hours, tomorrow at 09:00 on the console clock.
int64_t snooze_deadline(int preset, int64_t local_now, int64_t clock_offset);
