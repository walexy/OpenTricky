/* ot_window.c -- see ot_window.h. */
#include "ot_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(OT_HAVE_WINDOW)

#include <pthread.h>
#include <SDL3/SDL.h>

#define GUEST_STACK_BYTES ((size_t)64 * 1024 * 1024)   /* lifted code keeps big frames on the native stack */

/* ---- frame mailbox: written by the presenting thread, read by the window thread ---- */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *s_frame;          /* latest RGBA frame */
static unsigned s_w, s_h;
static unsigned long s_frame_seq; /* bumped on every post */

void ot_window_post_frame(const uint8_t *rgba, unsigned w, unsigned h)
{
    size_t n = (size_t)w * h * 4;
    if (!rgba || !w || !h) return;
    pthread_mutex_lock(&s_lock);
    if (w != s_w || h != s_h || !s_frame) {
        free(s_frame);
        s_frame = (uint8_t *)malloc(n);
        s_w = w; s_h = h;
    }
    if (s_frame) { memcpy(s_frame, rgba, n); s_frame_seq++; }
    pthread_mutex_unlock(&s_lock);
}

/* ---- the guest's thread ---- */
typedef struct { int (*fn)(void *); void *arg; int result; volatile int done; } guest_t;

static void *guest_thread(void *p)
{
    guest_t *g = (guest_t *)p;
    g->result = g->fn(g->arg);
    __atomic_store_n(&g->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int ot_window_wanted(void)
{
    const char *e = getenv("OT_WINDOW"), *h = getenv("XBOX_HEADLESS");
    if (e && e[0] == '0') return 0;
    if (h && h[0] == '1') return 0;
    return 1;
}

static void save_ppm(SDL_Renderer *r, const char *path)
{
    SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
    FILE *f;
    int x, y;
    if (!s) { fprintf(stderr, "[WINDOW] readback failed: %s\n", SDL_GetError()); return; }
    {   SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(s); s = c; }
    if (!s || !(f = fopen(path, "wb"))) { if (s) SDL_DestroySurface(s); return; }
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (y = 0; y < s->h; y++) {
        const uint8_t *row = (const uint8_t *)s->pixels + (size_t)y * s->pitch;
        for (x = 0; x < s->w; x++) fwrite(row + x * 4, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "[WINDOW] saved what the window shows (%dx%d) to %s\n", s->w, s->h, path);
    SDL_DestroySurface(s);
}

int ot_window_run(int (*guest)(void *), void *arg)
{
    guest_t g = { guest, arg, 0, 0 };
    pthread_t th;
    pthread_attr_t attr;
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    unsigned tex_w = 0, tex_h = 0;
    unsigned long shown_seq = 0, frames = 0;
    const char *shot = getenv("OT_WINDOW_SHOT");
    const char *sf = getenv("OT_WINDOW_SHOT_FRAMES");
    unsigned long shot_after = (sf && atoi(sf) > 0) ? (unsigned long)atoi(sf) : 90;
    int shot_done = 0, quit = 0;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[WINDOW] SDL_Init failed (%s): running without a window\n", SDL_GetError());
        return guest(arg);
    }
#if defined(OT_GPU_RENDERER)
    win = SDL_CreateWindow("SSX Tricky (OpenTricky, macOS: 3D in progress)", 1280, 960,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    {
        /* The GPU renderer (xboxrecomp/src/d3d/d3d8_gpu.c) owns the window's swapchain: it draws and presents
         * from the thread that presents. This thread only runs the event loop. */
        extern int d3d8_gpu_attach(void *device, void *window);
        SDL_GPUDevice *gd = win ? SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, getenv("OT_GPU_DEBUG") != NULL, NULL) : NULL;
        if (!gd || !SDL_ClaimWindowForGPUDevice(gd, win)) {
            fprintf(stderr, "[WINDOW] no GPU device (%s): running without a window\n", SDL_GetError());
            if (win) SDL_DestroyWindow(win);
            SDL_Quit();
            return guest(arg);
        }
        SDL_SetGPUSwapchainParameters(gd, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
        d3d8_gpu_attach(gd, win);
        fprintf(stderr, "[WINDOW] GPU renderer on %s\n", SDL_GetGPUDeviceDriver(gd));
        ren = NULL;
    }
    {
#else
    win = SDL_CreateWindow("SSX Tricky (OpenTricky, macOS: no 3D yet)", 1280, 960,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    {
#endif
#if !defined(OT_GPU_RENDERER)
    if (!ren) {
        fprintf(stderr, "[WINDOW] could not open a window (%s): running without one\n", SDL_GetError());
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        return guest(arg);
    }
    fprintf(stderr, "[WINDOW] %s renderer, video driver %s\n", SDL_GetRendererName(ren), SDL_GetCurrentVideoDriver());
    SDL_SetRenderLogicalPresentation(ren, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);   /* the title's 4:3 */
#endif
    }

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, GUEST_STACK_BYTES);
    if (pthread_create(&th, &attr, guest_thread, &g) != 0) {
        fprintf(stderr, "[WINDOW] could not start the game thread\n");
        return 1;
    }
    pthread_attr_destroy(&attr);

    while (!quit && !__atomic_load_n(&g.done, __ATOMIC_ACQUIRE)) {
        SDL_Event ev;
        int have;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = 1;

#if defined(OT_GPU_RENDERER)
        SDL_Delay(4);        /* the GPU renderer presents from the game's thread */
        continue;
#endif
        pthread_mutex_lock(&s_lock);
        have = s_frame && s_frame_seq != shown_seq;
        if (have) {
            if (!tex || tex_w != s_w || tex_h != s_h) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int)s_w, (int)s_h);
                if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
                tex_w = s_w; tex_h = s_h;
            }
            if (tex) SDL_UpdateTexture(tex, NULL, s_frame, (int)s_w * 4);
            shown_seq = s_frame_seq;
        }
        pthread_mutex_unlock(&s_lock);

        if (have && tex) {
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren, tex, NULL, NULL);
            frames++;
            if (shot && !shot_done && frames >= shot_after) { save_ppm(ren, shot); shot_done = 1; }
            SDL_RenderPresent(ren);
        } else {
            SDL_Delay(4);
        }
    }

    if (quit) {
        fprintf(stderr, "[WINDOW] closed: exiting\n");
        fflush(NULL);
        exit(0);        /* the game threads are killed with the process */
    }
    pthread_join(th, NULL);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return g.result;
}

#else /* no window in this build */

int ot_window_wanted(void) { return 0; }
int ot_window_run(int (*guest)(void *), void *arg) { return guest(arg); }
void ot_window_post_frame(const uint8_t *rgba, unsigned w, unsigned h) { (void)rgba; (void)w; (void)h; }

#endif
