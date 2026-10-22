// Headless screen check: runs the .3dsx in Azahar's libretro core with the
// software renderer, plays a button script, and writes frames as PPM files
// (top screen above bottom, 400x480). No window or screen capture needed.
//
//   retroshot <azahar_libretro.dylib> <t3code.3dsx> <script>
//
// Script lines, by emulated frame (60 per second):
//   <frame> press <joypad id>   held for 8 frames; A=8 B=0 X=9 Y=1 SELECT=2
//                               START=3 UP=4 DOWN=5 LEFT=6 RIGHT=7 L=10 R=11
//   <frame> hold <id>:<frames>  held for that many frames
//   <frame> shot <path.ppm>
//   <frame> end
//
// The core keeps its SD card in /tmp/lr-save/Azahar/sdmc; put a paired
// 3ds/t3code/environments.json there to start connected.
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "libretro.h"

static unsigned fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static const void *last; static unsigned lw, lh; static size_t lpitch;
static unsigned char *copy; static size_t copy_cap;
static int pressed_id = -1;

static void lr_log(enum retro_log_level level, const char *f, ...) {
  if (level < RETRO_LOG_WARN && !getenv("LR_VERBOSE")) return;
  va_list ap; va_start(ap, f); vfprintf(stderr, f, ap); va_end(ap);
}

static bool env(unsigned cmd, void *data) {
  switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = "/tmp/lr-sys"; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = "/tmp/lr-save"; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: fmt = *(enum retro_pixel_format *)data; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = lr_log; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable *v = data;
      if (!strcmp(v->key, "citra_graphics_api")) { v->value = "Software"; return true; }
      if (!strcmp(v->key, "citra_layout_option")) { v->value = "Default Top-Bottom"; return true; }
      if (!strcmp(v->key, "citra_resolution_factor")) { v->value = "1x (Native)"; return true; }
      if (!strcmp(v->key, "citra_is_new_3ds")) { v->value = getenv("LR_OLD3DS") ? "Old 3DS" : "New 3DS"; return true; }
      return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    default: return false;
  }
}

static void video(const void *d, unsigned w, unsigned h, size_t pitch) {
  if (!d) return;
  size_t need = pitch * h;
  if (need > copy_cap) { copy = realloc(copy, need); copy_cap = need; }
  memcpy(copy, d, need);
  last = copy; lw = w; lh = h; lpitch = pitch;
}

static void save(const char *path) {
  if (!last) { fprintf(stderr, "no frame for %s\n", path); return; }
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6\n%u %u\n255\n", lw, lh);
  for (unsigned y = 0; y < lh; y++)
    for (unsigned x = 0; x < lw; x++) {
      unsigned char px[3];
      if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
        unsigned v = ((const unsigned *)((const char *)last + y * lpitch))[x];
        px[0] = v >> 16; px[1] = v >> 8; px[2] = v;
      } else {
        unsigned short v = ((const unsigned short *)((const char *)last + y * lpitch))[x];
        px[0] = ((v >> 11) & 31) << 3; px[1] = ((v >> 5) & 63) << 2; px[2] = (v & 31) << 3;
      }
      fwrite(px, 1, 3, f);
    }
  fclose(f);
  fprintf(stderr, "saved %s (%ux%u)\n", path, lw, lh);
}

static void audio(int16_t l, int16_t r) {}
static size_t audio_batch(const int16_t *d, size_t n) { return n; }
static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) {
  return port == 0 && dev == RETRO_DEVICE_JOYPAD && (int)id == pressed_id;
}

int main(int argc, char **argv) {
  // usage: front core.dylib content script
  // script lines: <frame> press <id> | <frame> shot <name> | <frame> end
  void *h = dlopen(argv[1], RTLD_NOW);
  if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define SYM(n) typeof(&n) p_##n = dlsym(h, #n)
  SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
  SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
  SYM(retro_init); SYM(retro_load_game); SYM(retro_run); SYM(retro_deinit);
  p_retro_set_environment(env); p_retro_set_video_refresh(video); p_retro_set_audio_sample(audio);
  p_retro_set_audio_sample_batch(audio_batch); p_retro_set_input_poll(poll); p_retro_set_input_state(input);
  p_retro_init();
  struct retro_game_info gi = {.path = argv[2]};
  if (!p_retro_load_game(&gi)) { fprintf(stderr, "load failed\n"); return 1; }
  FILE *s = fopen(argv[3], "r");
  if (!s) { perror(argv[3]); return 1; }
  char line[256]; int frame = 0, release_at = -1;
  while (fgets(line, sizeof line, s)) {
    int at; char op[16], arg[128] = "";
    if (sscanf(line, "%d %15s %127s", &at, op, arg) < 2) continue;
    while (frame < at) {
      if (frame == release_at) pressed_id = -1;
      p_retro_run(); frame++;
    }
    if (!strcmp(op, "press")) { pressed_id = atoi(arg); release_at = frame + 8; }
    else if (!strcmp(op, "hold")) { int id, n; if (sscanf(arg, "%d:%d", &id, &n) == 2) { pressed_id = id; release_at = frame + n; } }
    else if (!strcmp(op, "shot")) save(arg);
    else if (!strcmp(op, "end")) break;
  }
  _exit(0);
}
