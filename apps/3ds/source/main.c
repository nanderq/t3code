#include <3ds.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <time.h>

#include "alerts.h"
#include "bg.h"
#include "client.h"
#include "gfx.h"
#include "ui.h"
#include "util.h"

#define SOC_BUFFER_SIZE 0x100000

uint64_t client_ticks_ms(void) { return osGetTime(); }

int main(void) {
  // 804 MHz and the extra cache on New 3DS; a no-op on older models.
  osSetSpeedupEnable(true);
  gfxInitDefault();
  util_seed_random(svcGetSystemTick() ^ (uint64_t)time(NULL));

  u32 *soc_buffer = memalign(0x1000, SOC_BUFFER_SIZE);
  // Without sockets every connection fails and the UI reports it; nothing else to do here.
  bool net_ok = soc_buffer && socInit(soc_buffer, SOC_BUFFER_SIZE) == 0;
  // Started with `3dslink -s`: send stdout to that host for debugging.
  if (net_ok && __3dslink_host.s_addr) link3dsStdio();

  gfx_init();
  bg_init();
  alerts_init();
  ui_init();

  while (aptMainLoop()) {
    hidScanInput();
    u32 down = hidKeysDown(), held = hidKeysHeld();
    if (down & KEY_START) break;
    touchPosition touch;
    hidTouchRead(&touch);
    ui_frame(down, held, &touch);
  }

  ui_shutdown();
  bg_shutdown();
  alerts_shutdown();
  gfx_exit();
  if (net_ok) socExit();
  free(soc_buffer);
  gfxExit();
  return 0;
}
