#ifdef __3DS__
#include <3ds.h>
#include <math.h>
#include <string.h>

#include "alerts.h"

#define ALERT_RATE 22050
#define ALERT_SAMPLES (ALERT_RATE / 5)

static bool ready;
static int16_t *samples;
static ndspWaveBuf wave;

void alerts_init(void) {
  if (R_FAILED(ndspInit())) return;
  samples = linearAlloc(ALERT_SAMPLES * sizeof *samples);
  if (!samples) {
    ndspExit();
    return;
  }
  for (int i = 0; i < ALERT_SAMPLES; i++) {
    float envelope = sinf((float)M_PI * i / ALERT_SAMPLES);
    samples[i] = (int16_t)(6000 * envelope * sinf(2 * (float)M_PI * 880 * i / ALERT_RATE));
  }
  DSP_FlushDataCache(samples, ALERT_SAMPLES * sizeof *samples);
  ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
  ndspChnSetRate(0, ALERT_RATE);
  ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
  float mix[12] = {1, 1};
  ndspChnSetMix(0, mix);
  ready = true;
}

void alerts_play(void) {
  if (!ready) return;
  ndspChnWaveBufClear(0);
  memset(&wave, 0, sizeof wave);
  wave.data_vaddr = samples;
  wave.nsamples = ALERT_SAMPLES;
  ndspChnWaveBufAdd(0, &wave);
}

void alerts_shutdown(void) {
  if (!ready) return;
  ndspChnWaveBufClear(0);
  ndspExit();
  linearFree(samples);
  samples = NULL;
  ready = false;
}
#endif
