#include "snooze.h"

#include <time.h>

int64_t snooze_deadline(int preset, int64_t local_now, int64_t clock_offset) {
  if (preset == 0) return local_now + clock_offset + 3600;
  if (preset == 1) return local_now + clock_offset + 3 * 3600;
  if (preset != 2) return 0;
  time_t now = (time_t)local_now;
  struct tm tomorrow;
  if (!localtime_r(&now, &tomorrow)) return 0;
  tomorrow.tm_mday++;
  tomorrow.tm_hour = 9;
  tomorrow.tm_min = tomorrow.tm_sec = 0;
  tomorrow.tm_isdst = -1;
  time_t until = mktime(&tomorrow);
  return until == (time_t)-1 ? 0 : (int64_t)until + clock_offset;
}
