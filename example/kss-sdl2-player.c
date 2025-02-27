#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_audio.h>

#include "../src/kssplay.h"

#define MAX_RATE 384000
#define DEFAULT_RATE 44100
#define MAX_PATH 1024
#define AUDIO_FRAMES_PER_SEC 60

/*
 * Build:
 *   git clone --recursive https://github.com/digital-sound-antiques/libkss.git
 *     (or my fork)
 *   cd libkss
 *   mkdir build
 *   cd build
 *   cmake ..
 *   cmake --build .
 *   cmake --build . --target kss-sdl2-player
 *
 * Features:
 * - play using SDL2 audio
 * - load .MBK when found
 * - configurable silent limit & verbosity, and allow infty play time
 * - print more song info
 * - halt on Ctrl-C (not 100%)
 *
 * Wish:
 * - force VSync frequency (e.g. 50Hz)
 * - show more song/file info, e.g.:
 *   - kss->mode 0:MSX 1:SMS 2:GG
 *   - kss->stereo (Sega Game Gear only... Or also MSX-AUDIO?)
 *
 * Limitations:
 * - stutters when e.g. switching windows
 * - KSS (KSCC/KSSX) file formats do not appear to store song length; must detect end-of-song by silence...
 * - Found no useful first/last song data (either 0-255 or 0-0)
 * - No idea if kss2vgm is intended as program? A main() is missing.
 * - Found no .kss files with loops, MGStext, title, extras or infos; untested
 * - Can segfault (when selecting a non-existent song? There's no protection against that...)
 * - MGStext is not erased
 * - Don't know how to show which chips are used in the song; can detect:
 *   - PSG (SSG emu2149 - MSX PSG AY-3-8910 clone/SMS DCSG emu76489): sn76489
 *   - MSX-MUSIC (OPLL/emu2413): fmpac/fmunit
 *   - MSX-AUDIO (OPL/emu8950): msx_audio
 *  but not:
 *   - SCC (emu2212?!)
 *   - DAC
 *   - sng (??)
 */

#define HLPMSG                                                                                                         \
  "Usage: kssplayer [Options] <filename.kss>\n"                                                                        \
  "Options:\n"                                                                                                         \
  "  -f<fade_time>  Fade-out duration (seconds; default:5)\n"                                                          \
  "  -i<silent_lim> Silent limit before halting (ms; default:2)\n"                                                     \
  "  -l<loop_num>   Number of loops (0 means 256; default:1)\n"                                                        \
  "  -n[1|2]        Number of channels (default:1/MONO)\n"                                                             \
  "  -p<play_time>  Max. play time (seconds; <=0 for song end/infty; default:60)\n"                                    \
  "  -q<quality>    Rendering quality 0:LOW 1:HIGH (default:1/HIGH)\n"                                                 \
  "  -r<play_freq>  Specify the frequency/sample rate (Hz; default:44100)\n"                                           \
  "  -s<song_num>   Song number to play\n"                                                                             \
  "  -v<level>      Verbosity level (0-4;0=quiet;default:3/INFO)\n"                                                    \
  "Note: spaces are not accepted between the option character and its parameter.\n"                                    \
  "Also plays (certain) .MGS .MBM .MPK .BGM .OPX files\n"

int bytesPerSample; // all channels combined!
static KSSPLAY *kssplay;

#define LOG_OFF     0
#define LOG_ERR     1
#define LOG_WARN    2
#define LOG_INFO    3
#define LOG_DEBUG   4

typedef struct {
  int rate;
  int nch;
  int bps;
  int song_num;
  int play_time;
  int fade_time;
  int loop_num;
  int silent_limit_ms;
  int quality;
  char input[MAX_PATH + 4];
  int help;
  int error;
  int verbosity;
} Options;

static Options parse_options(int argc, char **argv) {
  Options options;

  // default settings
  options.rate = DEFAULT_RATE;
  options.nch = 1;
  options.song_num = 0;
  options.play_time = 60;
  options.fade_time = 5;
  options.silent_limit_ms = 2000;
  options.loop_num = 1;
  options.quality = 1;
  options.verbosity = LOG_INFO;

  options.input[0] = '\0';
  options.help = 0;
  options.error = 0;

  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      switch (argv[i][1]) {
        case 'n':
          options.nch = (2 == atoi(argv[i] + 2)) ? 2 : 1;
          break;
        case 'p':
          options.play_time = atoi(argv[i] + 2);
          break;
        case 's':
          options.song_num = atoi(argv[i] + 2);
          break;
        case 'f':
          options.fade_time = atoi(argv[i] + 2);
          break;
        case 'r':
          options.rate = atoi(argv[i] + 2);
          break;
        case 'q':
          options.quality = atoi(argv[i] + 2);
          break;
        case 'v':
          options.verbosity = atoi(argv[i] + 2);
          break;
        case 'i':
          options.silent_limit_ms = atoi(argv[i] + 2);
          break;
        case 'l':
          options.loop_num = atoi(argv[i] + 2);
          if (options.loop_num == 0) {
            options.loop_num = 256;
          }
          break;
        default:
          options.error = 1;
          break;
      }
    } else {
      if (strlen(options.input)!=0) {
        if (options.verbosity>=LOG_ERR) printf("input already set: %s!\n", options.input);
        exit(1);
      }
      strncpy(options.input, argv[i], MAX_PATH);
    }
  }

  if (options.rate > MAX_RATE) {
    options.rate = DEFAULT_RATE;
  }

  return options;
}

// https://stackoverflow.com/questions/61505537/sdl-openaudiodevice-continuous-play-from-real-time-processed-source-buffer
// https://wiki.libsdl.org/SDL2/CategoryAudio
// https://thenumb.at/cpp-course/sdl2/06/06.html
// https://wiki.libsdl.org/SDL2/Tutorials-AudioStream

// will be called AUDIO_FRAMES_PER_SEC
void audioCallback(void *userdata, uint8_t *stream, int len) {
  KSSPLAY_calc(kssplay, (int16_t*)stream, len/bytesPerSample);
}

void initialize_audio(Options *opt) {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
    if (opt->verbosity>=LOG_ERR) printf("SDL fails to initialize audio subsystem!\n>> %s", SDL_GetError());
    exit(1);
  }
  atexit(SDL_Quit);

  SDL_AudioSpec want;

  want.freq = opt->rate;
  want.format = AUDIO_S16;
  want.channels = opt->nch;
  want.samples = (double)opt->rate / (double)AUDIO_FRAMES_PER_SEC;
  want.padding = 0;
  want.callback = audioCallback;
  want.userdata = NULL;

  bytesPerSample = sizeof(int16_t) * opt->nch;

  int device = SDL_OpenAudioDevice(SDL_GetAudioDeviceName(0, 0), 0, &want, NULL, 0);
  SDL_PauseAudioDevice(device, 0); // switches audio on
}

void init_kssplay(KSS *kss, Options *opt) {
  kssplay = KSSPLAY_new(opt->rate, opt->nch, sizeof(int16_t) << 3);
  KSSPLAY_set_data(kssplay, kss);
  KSSPLAY_reset(kssplay, opt->song_num, 0);

  KSSPLAY_set_device_quality(kssplay, EDSC_PSG, opt->quality);
  KSSPLAY_set_device_quality(kssplay, EDSC_SCC, opt->quality);
  KSSPLAY_set_device_quality(kssplay, EDSC_OPLL, opt->quality);

  KSSPLAY_set_silent_limit(kssplay, opt->silent_limit_ms);

  if (opt->nch > 1) { // in stereo!
    KSSPLAY_set_device_pan(kssplay, EDSC_PSG, -32);
    KSSPLAY_set_device_pan(kssplay, EDSC_SCC,  32);
    kssplay->opll_stereo = 1;
    KSSPLAY_set_channel_pan(kssplay, EDSC_OPLL, 0, 1);
    KSSPLAY_set_channel_pan(kssplay, EDSC_OPLL, 1, 2);
    KSSPLAY_set_channel_pan(kssplay, EDSC_OPLL, 2, 1);
    KSSPLAY_set_channel_pan(kssplay, EDSC_OPLL, 3, 2);
    KSSPLAY_set_channel_pan(kssplay, EDSC_OPLL, 4, 1);
    KSSPLAY_set_channel_pan(kssplay, EDSC_OPLL, 5, 2);
  }
}

int main(int argc, char *argv[]) {
  char buf[80];
  buf[0]='\0';

  /* parse options */
  if (argc < 2) {
    printf(HLPMSG);
    exit(0);
  }
  Options opt = parse_options(argc, argv);
  if (opt.error) {
    printf(HLPMSG);
    exit(1);
  }

  /* init KSS & SDL */
  if (strcasestr(opt.input,".mbm")) {
    KSS_autoload_mbk(opt.input, "", NULL);
  }
  KSS *kss;
  if ((kss = KSS_load_file(opt.input)) == NULL) {
    if (opt.verbosity>=LOG_ERR) fprintf(stderr, "FILE READ ERROR!\n");
    exit(1);
  }
  if (opt.song_num<kss->trk_min || (kss->trk_max > 0 && opt.song_num>kss->trk_max)) {
    if (opt.verbosity>=LOG_ERR) fprintf(stderr, "Song out of range: %d-%d\n", kss->trk_min, kss->trk_max);
    exit(1);
  }
  init_kssplay(kss, &opt);
  initialize_audio(&opt);

  /* print song info */
  if (opt.verbosity>=LOG_INFO) {
    printf("Playing a song contained in %s\n", opt.input);
    printf("FORMAT: %s, SONG#: %02d, TITLE: \"%s\", PLAYTIME: %ds, LOOPS: %d, FADETIME: %ds, RATE: %dHz, CHANNELS: %s, QUALITY: %s, FREQ: %s\n",
      kss->idstr, opt.song_num, kss->title, opt.play_time, opt.loop_num, opt.fade_time, opt.rate,
      opt.nch == 1 ? "MONO":"STEREO", opt.quality == 1 ? "HI":"LO",
      kss->pal_mode ? "PAL":"NTSC");
  }

  // untested; no .kss files encountered with _any_ infos
  if (opt.verbosity>=LOG_INFO && opt.song_num<kss->info_num) {
    KSSINFO *info = &kss->info[opt.song_num];
    printf("time: %dms fade: %dms\n", info->time_in_ms, info->fade_in_ms);
    if (strlen(info->title)>0) {
      printf("%s\n", info->title);
    }
  }

  if (kss->extra && opt.verbosity>=LOG_INFO) {
    printf("%s\n", kss->extra);
  }

  /* monitor play, one tick per second */
  for (int t = 0; opt.play_time==0 || t < opt.play_time; t++) {
    SDL_Event event;
    if (SDL_PollEvent(&event) && event.type==SDL_QUIT) {
      if (opt.verbosity>=LOG_INFO) printf("\n");
      exit(1);
    }

    if (opt.verbosity>=LOG_INFO) {
      printf("%03d/%03d secs", t + 1, opt.play_time);

      KSSPLAY_get_MGStext(kssplay, (char*)&buf, sizeof(buf)-1);
      if (strlen(buf)>0 && opt.verbosity>=LOG_INFO) {
        printf(" %s", buf); // should remove this using \x08
      }
      fflush(stdout);
    }
    usleep(1000000);
    if (opt.verbosity>=LOG_INFO) printf("\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08");

    if (opt.verbosity>=LOG_DEBUG) printf("loop: %d\n", KSSPLAY_get_loop_count(kssplay));

    /* If looped, start fadeout */
    if (  (KSSPLAY_get_loop_count(kssplay) >= opt.loop_num || (opt.play_time > 0 && (opt.play_time - opt.fade_time) <= t + 1))
       && KSSPLAY_get_fade_flag(kssplay) == KSSPLAY_FADE_NONE) {
      KSSPLAY_fade_start(kssplay, opt.fade_time * 1000);
    }

    /* If the fade is ended or the play is stopped, break */
    if (KSSPLAY_get_fade_flag(kssplay) == KSSPLAY_FADE_END || KSSPLAY_get_stop_flag(kssplay)) {
      break;
    }
  }

  KSSPLAY_delete(kssplay);
  KSS_delete(kss);

  if (opt.verbosity>=LOG_INFO) printf("\n");
  return 0;
}
