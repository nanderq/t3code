#pragma once

#ifdef __3DS__
void alerts_init(void);
void alerts_shutdown(void);
void alerts_play(void);
#else
static inline void alerts_init(void) {}
static inline void alerts_shutdown(void) {}
static inline void alerts_play(void) {}
#endif
