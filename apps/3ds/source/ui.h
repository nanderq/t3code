#pragma once

#include <3ds.h>

void ui_init(void);
void ui_shutdown(void);
// Handles one frame of input and draws both screens.
void ui_frame(u32 down, u32 held, const touchPosition *touch);
