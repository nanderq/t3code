#pragma once

#include <stdbool.h>
#include <stddef.h>

// Reads QR codes through the outer camera. The preview is 400x240, the size of
// the top screen.
bool qrscan_start(char *err, size_t errcap);
void qrscan_stop(void);
bool qrscan_active(void);
// Takes the newest camera frame, if one arrived, and hands it to the decoder.
// Returns true with the payload when the decoder read a code since the last call.
bool qrscan_update(char *out, size_t cap);
// Whether a camera frame has arrived yet, so the preview has something to show.
bool qrscan_has_frame(void);
// Draws the preview on the current target.
void qrscan_draw(float x, float y);
