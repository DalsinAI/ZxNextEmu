/*
 * zxnextemu: desktop front end (SDL2).
 *
 *   zxnextemu [-m model] [-r romdir] [-c sd-folder-or-image] [-p pi-folder] [-z scale] [file]
 *
 * -m next runs the ZX Spectrum Next; give it an SD card image with -c.
 *
 * Keys: PC keyboard maps onto the Spectrum's; Left Shift = CAPS SHIFT,
 * Right Shift / Ctrl = SYMBOL SHIFT, Backspace = DELETE. Arrow keys and
 * Tab are a Kempston joystick. F5 resets, F9 toggles full speed, drop a
 * file on the window to load it, Esc quits.
 *
 * Speed is paced by the audio device, so each model runs at its own exact
 * frame rate (50.08 Hz on a 48K, 50.02 Hz on a 128K, 48.83 Hz on a Pentagon).
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../zx/zx.h"
#include "../zx/tape.h"
#include "media.h"
#include "net_posix.h"
#include "../next/next.h"
#include "../devices/esp8266.h"
#include "../devices/nextpi.h"

typedef struct { SDL_Scancode sc; int row, bit; } keymap;

static const keymap keys[] = {
    { SDL_SCANCODE_LSHIFT, 0, 0 }, { SDL_SCANCODE_Z, 0, 1 }, { SDL_SCANCODE_X, 0, 2 },
    { SDL_SCANCODE_C, 0, 3 }, { SDL_SCANCODE_V, 0, 4 },
    { SDL_SCANCODE_A, 1, 0 }, { SDL_SCANCODE_S, 1, 1 }, { SDL_SCANCODE_D, 1, 2 },
    { SDL_SCANCODE_F, 1, 3 }, { SDL_SCANCODE_G, 1, 4 },
    { SDL_SCANCODE_Q, 2, 0 }, { SDL_SCANCODE_W, 2, 1 }, { SDL_SCANCODE_E, 2, 2 },
    { SDL_SCANCODE_R, 2, 3 }, { SDL_SCANCODE_T, 2, 4 },
    { SDL_SCANCODE_1, 3, 0 }, { SDL_SCANCODE_2, 3, 1 }, { SDL_SCANCODE_3, 3, 2 },
    { SDL_SCANCODE_4, 3, 3 }, { SDL_SCANCODE_5, 3, 4 },
    { SDL_SCANCODE_0, 4, 0 }, { SDL_SCANCODE_9, 4, 1 }, { SDL_SCANCODE_8, 4, 2 },
    { SDL_SCANCODE_7, 4, 3 }, { SDL_SCANCODE_6, 4, 4 },
    { SDL_SCANCODE_P, 5, 0 }, { SDL_SCANCODE_O, 5, 1 }, { SDL_SCANCODE_I, 5, 2 },
    { SDL_SCANCODE_U, 5, 3 }, { SDL_SCANCODE_Y, 5, 4 },
    { SDL_SCANCODE_RETURN, 6, 0 }, { SDL_SCANCODE_L, 6, 1 }, { SDL_SCANCODE_K, 6, 2 },
    { SDL_SCANCODE_J, 6, 3 }, { SDL_SCANCODE_H, 6, 4 },
    { SDL_SCANCODE_SPACE, 7, 0 }, { SDL_SCANCODE_RSHIFT, 7, 1 }, { SDL_SCANCODE_LCTRL, 7, 1 },
    { SDL_SCANCODE_RCTRL, 7, 1 }, { SDL_SCANCODE_M, 7, 2 }, { SDL_SCANCODE_N, 7, 3 },
    { SDL_SCANCODE_B, 7, 4 },
};

static void key_event(zx *m, SDL_Scancode sc, int down)
{
    size_t i;
    for (i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (keys[i].sc == sc)
            zx_key(m, keys[i].row, keys[i].bit, down);
    switch (sc) {
    case SDL_SCANCODE_BACKSPACE: zx_key(m, 0, 0, down); zx_key(m, 4, 0, down); break;
    case SDL_SCANCODE_RIGHT: m->kempston = (uint8_t)(down ? m->kempston | 1 : m->kempston & ~1); break;
    case SDL_SCANCODE_LEFT:  m->kempston = (uint8_t)(down ? m->kempston | 2 : m->kempston & ~2); break;
    case SDL_SCANCODE_DOWN:  m->kempston = (uint8_t)(down ? m->kempston | 4 : m->kempston & ~4); break;
    case SDL_SCANCODE_UP:    m->kempston = (uint8_t)(down ? m->kempston | 8 : m->kempston & ~8); break;
    case SDL_SCANCODE_TAB:   m->kempston = (uint8_t)(down ? m->kempston | 16 : m->kempston & ~16); break;
    default: break;
    }
}

static void load(zx *m, const char *path)
{
    if (media_load(m, path) == 0 && m->tape) {
        /* A tape: reset and start the ROM loading it. */
        struct zx_tape *t = m->tape;
        m->tape = NULL;
        zx_reset(m);
        m->tape = t;
        media_type(m->model == ZX_48K ? "j\"\"\n" : "\n");
        media_type_delay(m->model == ZX_48K ? 120 : 90);
    }
}

int main(int argc, char **argv)
{
    zx_net *net = NULL;
    static zx m;
    zx_model model = ZX_128K;
    const char *romdir = "roms", *file = NULL, *sd = NULL, *pidir = NULL;
    int scale = 3, a, running = 1, turbo = 0;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    SDL_AudioDeviceID dev;
    SDL_AudioSpec want, have;
    static uint32_t pixels[256 * 640];
    uint32_t pal[512];
    char title[64];

    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-m") && a + 1 < argc) {
            if (zx_model_from_name(argv[++a], &model)) {
                fprintf(stderr, "unknown model %s\n", argv[a]);
                return 2;
            }
        } else if (!strcmp(argv[a], "-r") && a + 1 < argc) romdir = argv[++a];
        else if (!strcmp(argv[a], "-z") && a + 1 < argc) scale = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-c") && a + 1 < argc) sd = argv[++a];
        else if (!strcmp(argv[a], "-p") && a + 1 < argc) pidir = argv[++a];
        else if (argv[a][0] != '-') file = argv[a];
        else {
            fprintf(stderr, "usage: %s [-m model] [-r romdir] [-c sd-folder-or-image] [-p pi-folder] [-z scale] [file]\n", argv[0]);
            return 2;
        }
    }
    if (zx_init(&m, model, romdir))
        return 1;
    if (model == ZX_NEXT) {
        nx_attach_serial(&m, 0, zx_esp8266_new(net = zx_net_posix_new()));
        if (pidir)
            nx_attach_serial(&m, 1, zx_nextpi_new(pidir));
    }
    if (sd && zx_attach_sd(&m, 0, sd)) {
        fprintf(stderr, "cannot attach SD card image %s\n", sd);
        return 1;
    }
    if (file)
        load(&m, file);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    snprintf(title, sizeof title, "ZxNextEmu - %s", zx_timings[model].name);
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           ZX_SCREEN_W * scale, ZX_SCREEN_H * scale, SDL_WINDOW_RESIZABLE);
    ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, ZX_SCREEN_W, ZX_SCREEN_H);
    /* The Next's picture is 640 wide (half-width pixels); it still shows as 320x256. */
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                            m.screen_w, m.screen_h);
    for (a = 0; a < 512; a++)
        pal[a] = 0xff000000u | (uint32_t)(((a >> 6) & 7) * 255 / 7) << 16 |
                 (uint32_t)(((a >> 3) & 7) * 255 / 7) << 8 | (uint32_t)((a & 7) * 255 / 7);

    SDL_zero(want);
    want.freq = ZX_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev)
        SDL_PauseAudioDevice(dev, 0);

    while (running) {
        SDL_Event ev;
        int x, y;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (ev.key.repeat)
                    break;
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
                    running = 0;
                else if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F5)
                    zx_reset(&m);
                else if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F9)
                    turbo ^= 1;
                else
                    key_event(&m, ev.key.keysym.scancode, ev.type == SDL_KEYDOWN);
                break;
            case SDL_MOUSEMOTION: {
                uint32_t b = ev.motion.state;
                zx_mouse(&m, ev.motion.xrel, ev.motion.yrel, 0,
                         (b & SDL_BUTTON_LMASK ? 1 : 0) | (b & SDL_BUTTON_RMASK ? 2 : 0) | (b & SDL_BUTTON_MMASK ? 4 : 0));
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEWHEEL: {
                uint32_t b = SDL_GetMouseState(NULL, NULL);
                zx_mouse(&m, 0, 0, ev.type == SDL_MOUSEWHEEL ? -ev.wheel.y : 0,
                         (b & SDL_BUTTON_LMASK ? 1 : 0) | (b & SDL_BUTTON_RMASK ? 2 : 0) | (b & SDL_BUTTON_MMASK ? 4 : 0));
                break;
            }
            case SDL_DROPFILE:
                load(&m, ev.drop.file);
                SDL_free(ev.drop.file);
                break;
            default: break;
            }
        }

        media_type_frame(&m);
        zx_run_frame(&m);

        if (dev && !turbo) {
            SDL_QueueAudio(dev, m.audio, (Uint32)m.audio_samples * 4);
            /* Keep about three frames of audio queued: that paces the emulation. */
            while (SDL_GetQueuedAudioSize(dev) > (Uint32)(ZX_AUDIO_RATE / 50 * 4 * 3))
                SDL_Delay(1);
        } else if (dev) {
            SDL_ClearQueuedAudio(dev);
        }
        if (turbo && (m.frames % 10))
            continue;

        for (y = 0; y < m.screen_h; y++)
            for (x = 0; x < m.screen_w; x++)
                pixels[y * m.screen_w + x] = pal[m.pixels[y * m.pitch + x] & 511];
        SDL_UpdateTexture(tex, NULL, pixels, m.screen_w * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        if (!dev && !turbo)
            SDL_Delay(20);
    }
    SDL_Quit();
    zx_free(&m);
    zx_net_posix_free(net);
    return 0;
}
