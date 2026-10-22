#include "qrscan.h"

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>

#include "quirc/quirc.h"
#include "util.h"

#define CAM_W 400
#define CAM_H 240
#define TEX_W 512
#define TEX_H 256
#define FRAME_BYTES (CAM_W * CAM_H * 2)

static bool active, have_frame;
static int frame_count;
static u32 transfer_unit;
static Handle error_event;

// The camera overruns its small FIFO unless the next transfer is queued as soon
// as one finishes, so a dedicated thread does only that. Of the three buffers,
// one is being filled, one holds the newest frame, and one is being read.
static u16 *frames[3];
static int fill_idx, ready_idx, read_idx;
static LightLock frames_lock;
static Thread camera;
static bool camera_quit;
static C3D_Tex tex;
static const Tex3DS_SubTexture subtex = {CAM_W, CAM_H, 0.0f, 1.0f, CAM_W / (float)TEX_W, 1.0f - CAM_H / (float)TEX_H};

// The main thread fills quirc's image only while `decoding` is false; the
// decoder thread owns quirc and `result` while it is true.
static struct quirc *qr;
static Thread decoder;
static LightEvent decode_go;
static bool decoder_quit;
static bool decoding;
static bool has_result;
static char result[1024];

static void decoder_main(void *unused) {
  (void)unused;
  for (;;) {
    LightEvent_Wait(&decode_go);
    if (__atomic_load_n(&decoder_quit, __ATOMIC_ACQUIRE)) break;
    quirc_end(qr);
    int count = quirc_count(qr);
    for (int i = 0; i < count; i++) {
      struct quirc_code code;
      struct quirc_data data;
      quirc_extract(qr, i, &code);
      quirc_decode_error_t err = quirc_decode(&code, &data);
      // A code seen in a mirror decodes after flipping.
      if (err == QUIRC_ERROR_DATA_ECC) {
        quirc_flip(&code);
        err = quirc_decode(&code, &data);
      }
      if (err) continue;
      printf("qrscan: decoded %d bytes\n", data.payload_len);
      util_strlcpy(result, (const char *)data.payload, sizeof result);
      has_result = true;
      break;
    }
    __atomic_store_n(&decoding, false, __ATOMIC_RELEASE);
  }
}

// Offset of texel (x, y) in a tiled texture: 8x8 tiles, each in Morton order.
static inline u32 tiled(u32 x, u32 y) {
  return (((y >> 3) * (TEX_W >> 3) + (x >> 3)) << 6) | (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
         ((x & 4) << 2) | ((y & 4) << 3);
}

static void stop_camera(void) {
  if (error_event) {
    svcCloseHandle(error_event);
    error_event = 0;
  }
  CAMU_StopCapture(PORT_CAM1);
  CAMU_Activate(SELECT_NONE);
  camExit();
}

static void camera_main(void *unused) {
  (void)unused;
  Handle recv_event = 0;
  CAMU_SetReceiving(&recv_event, frames[fill_idx], PORT_CAM1, FRAME_BYTES, (s16)transfer_unit);
  while (!__atomic_load_n(&camera_quit, __ATOMIC_ACQUIRE)) {
    Handle events[2] = {error_event, recv_event};
    s32 which = -1;
    // Wakes at least every 100 ms to notice qrscan_stop.
    if (R_FAILED(svcWaitSynchronizationN(&which, events, 2, false, 100000000LL)) || which < 0) continue;
    svcCloseHandle(recv_event);
    recv_event = 0;
    if (which == 0) {
      // Overran anyway, as after the HOME menu; restart the capture.
      printf("qrscan: buffer error after %d frames\n", frame_count);
      CAMU_ClearBuffer(PORT_CAM1);
      CAMU_StartCapture(PORT_CAM1);
    } else {
      LightLock_Lock(&frames_lock);
      ready_idx = fill_idx;
      fill_idx = 3 - ready_idx - read_idx;
      LightLock_Unlock(&frames_lock);
    }
    CAMU_SetReceiving(&recv_event, frames[fill_idx], PORT_CAM1, FRAME_BYTES, (s16)transfer_unit);
  }
  if (recv_event) svcCloseHandle(recv_event);
}

bool qrscan_start(char *err, size_t errcap) {
  if (active) return true;
  for (int i = 0; i < 3; i++)
    if (!frames[i]) frames[i] = linearAlloc(FRAME_BYTES);
  if (!tex.data && !C3D_TexInit(&tex, TEX_W, TEX_H, GPU_RGB565)) {
    snprintf(err, errcap, "Not enough memory for the camera");
    return false;
  }
  if (!qr) {
    qr = quirc_new();
    if (!qr || quirc_resize(qr, CAM_W, CAM_H) < 0) {
      snprintf(err, errcap, "Not enough memory for the QR reader");
      return false;
    }
  }
  if (!frames[0] || !frames[1] || !frames[2]) {
    snprintf(err, errcap, "Not enough memory for the camera");
    return false;
  }
  Result rc = camInit();
  if (R_FAILED(rc)) {
    snprintf(err, errcap, "Camera unavailable (0x%08lX)", rc);
    return false;
  }
  CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A);
  CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
  CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30);
  CAMU_SetNoiseFilter(SELECT_OUT1, true);
  CAMU_SetAutoExposure(SELECT_OUT1, true);
  CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
  CAMU_SetTrimming(PORT_CAM1, false);
  CAMU_GetMaxBytes(&transfer_unit, CAM_W, CAM_H);
  CAMU_SetTransferBytes(PORT_CAM1, transfer_unit, CAM_W, CAM_H);
  rc = CAMU_Activate(SELECT_OUT1);
  if (R_FAILED(rc)) {
    camExit();
    snprintf(err, errcap, "Camera unavailable (0x%08lX)", rc);
    return false;
  }
  CAMU_GetBufferErrorInterruptEvent(&error_event, PORT_CAM1);
  CAMU_ClearBuffer(PORT_CAM1);
  CAMU_StartCapture(PORT_CAM1);

  C3D_TexSetFilter(&tex, GPU_LINEAR, GPU_LINEAR);
  memset(tex.data, 0, tex.size);
  C3D_TexFlush(&tex);
  have_frame = false;
  has_result = false;
  decoding = false;
  decoder_quit = false;
  LightEvent_Init(&decode_go, RESET_ONESHOT);
  s32 prio = 0x30;
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  // Below the UI thread, like the network worker: it runs while the UI waits on vsync.
  decoder = threadCreate(decoder_main, NULL, 64 * 1024, prio + 1, -1, false);
  // Above the UI thread, so a finished transfer is requeued at once.
  LightLock_Init(&frames_lock);
  fill_idx = 0;
  ready_idx = -1;
  read_idx = 2;
  camera_quit = false;
  camera = decoder ? threadCreate(camera_main, NULL, 16 * 1024, prio - 1, -1, false) : NULL;
  if (!camera) {
    if (decoder) {
      __atomic_store_n(&decoder_quit, true, __ATOMIC_RELEASE);
      LightEvent_Signal(&decode_go);
      threadJoin(decoder, U64_MAX);
      threadFree(decoder);
      decoder = NULL;
    }
    stop_camera();
    snprintf(err, errcap, "Could not start the QR reader");
    return false;
  }
  active = true;
  frame_count = 0;
  printf("qrscan: camera started\n");
  return true;
}

void qrscan_stop(void) {
  if (!active) return;
  __atomic_store_n(&decoder_quit, true, __ATOMIC_RELEASE);
  LightEvent_Signal(&decode_go);
  threadJoin(decoder, U64_MAX);
  threadFree(decoder);
  decoder = NULL;
  __atomic_store_n(&camera_quit, true, __ATOMIC_RELEASE);
  threadJoin(camera, U64_MAX);
  threadFree(camera);
  camera = NULL;
  stop_camera();
  active = false;
}

bool qrscan_active(void) { return active; }

bool qrscan_has_frame(void) { return have_frame; }

bool qrscan_update(char *out, size_t cap) {
  if (!active) return false;
  LightLock_Lock(&frames_lock);
  bool fresh = ready_idx >= 0;
  if (fresh) {
    read_idx = ready_idx;
    ready_idx = -1;
  }
  LightLock_Unlock(&frames_lock);
  if (!fresh) return false;
  u16 *frame = frames[read_idx];
  // The camera wrote the buffer by DMA; drop stale cache lines before reading it.
  GSPGPU_InvalidateDataCache(frame, FRAME_BYTES);

  bool idle = !__atomic_load_n(&decoding, __ATOMIC_ACQUIRE);
  bool found = false;
  if (idle && has_result) {
    util_strlcpy(out, result, cap);
    has_result = false;
    found = true;
  }
  uint8_t *luma = idle ? quirc_begin(qr, NULL, NULL) : NULL;
  u16 *texels = tex.data;
  for (u32 y = 0; y < CAM_H; y++) {
    const u16 *row = frame + y * CAM_W;
    for (u32 x = 0; x < CAM_W; x++) {
      u16 p = row[x];
      texels[tiled(x, y)] = p;
      if (luma) {
        u32 r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
        *luma++ = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
      }
    }
  }
  C3D_TexFlush(&tex);
  have_frame = true;
  if (++frame_count % 60 == 1) printf("qrscan: frame %d\n", frame_count);
  if (idle) {
    __atomic_store_n(&decoding, true, __ATOMIC_RELEASE);
    LightEvent_Signal(&decode_go);
  }
  return found;
}

void qrscan_draw(float x, float y) {
  C2D_Image image = {&tex, &subtex};
  C2D_DrawImageAt(image, x, y, 0.5f, NULL, 1.0f, 1.0f);
}
