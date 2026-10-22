#pragma once

#include "client.h"

float prdetailview_height(const Client *c);
void prdetailview_draw(const Client *c, float scroll);
void prdetailview_bottom(const Client *c, bool armed);
