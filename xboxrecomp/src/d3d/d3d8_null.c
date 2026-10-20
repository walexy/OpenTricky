/* d3d8_null.c -- headless backend for the D3D8 / PGRAPH seam (OT_HEADLESS).
 *
 * Stands in for d3d8_*.c and nv2a_pgraph_d3d11.c. Nothing is drawn. What it keeps
 * is everything the *guest* can observe or wait for:
 *
 *   - frames: a surface flip (NV097_SET_SURFACE_COLOR_OFFSET changing) or, before
 *     the title has flipped at all, a CLEAR_SURFACE after draws, counts as one
 *     present, exactly where the Direct3D 11 translator presents. d3d8_PresentSeq()
 *     is that counter;
 *   - occlusion reports: NV097_GET_REPORT is completed on the spot -- the title
 *     spins on the 16-byte report's status word until the GPU writes it;
 *   - the guest framebuffer the movie player writes through LockRect, which can be
 *     dumped as PPM (XBOX_DUMP_FB=<dir>), the headless way to look at the picture.
 *
 * Everything else the translator and the host D3D layer expose (the OpenTricky
 * display options, the window, screenshots, shaders) is accepted and ignored.
 * nv2a_live_pb.c, the push-buffer ring walker, is the same file as on Windows and
 * drives this through the same calls.
 *
 * The entry points are the d3d8_* and pgraph_d3d11_* functions the rest of the
 * runtime and the port call; the list is in macos/d3d_seam.txt.
 */
#include "platform/xbox_winnt.h"
#include "d3d8_xbox.h"
#include "../nv2a/nv2a_pgraph_d3d11.h"
#include "../nv2a/nv2a_regs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The title's NV097_GET_REPORT reports live in this address range's low 24 bits. */
#define NULL_REPORT_ZPASS 1u    /* report type: zpass pixel count */

/* ======================================================================= */
/* state                                                                   */
/* ======================================================================= */

/* referenced from nv2a_pb_replay.c / nv2a_live_pb.c (real definitions live in the D3D layer) */
volatile int g_suppress_present;
uint32_t     g_guest_fb_alt_va;

/* Draw fence: the last NOP 0x5C000000|n the stream passed. The port's pumplag.c (XBOX_FIX_CONSTWAIT)
 * rewrites a constant block only once this has reached the stamp it left there. */
volatile uint32_t pgraph_d3d11_fork_fence;

static uint8_t  *g_mem;                 /* guest RAM window, from set_mem_base */
static uint32_t  g_mem_size;
static int       g_initialized;

static uint32_t  g_surface_offset;      /* current NV097 colour surface offset */
static int       g_surface_seen;
static int       g_flip_mode;           /* the title flips surfaces */
static uint64_t  g_last_flip_ms;
static unsigned  g_draws_since_present;

static volatile unsigned g_present_seq;
static uint64_t  g_last_present_ms;
static unsigned  g_draw_total, g_clear_total, g_report_total;
static PgraphD3D11Stats g_stats;

/* guest framebuffer registered by the movie player's LockRect (recomp_manual.c) */
static const void *g_fb_src, *g_fb_alt;
static unsigned    g_fb_pitch, g_fb_w, g_fb_h;
static unsigned    g_fb_lock_seq, g_fb_locks;
#define GUEST_FB_IDLE_PRESENTS 30u

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t report_value(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("XBOX_NULL_ZPASS"); v = e ? atoi(e) : 256; }
    return (uint32_t)v;
}

/* ======================================================================= */
/* guest framebuffer dump                                                  */
/* ======================================================================= */

static void dump_guest_fb(void)
{
    static const char *dir; static int checked, max_frames = 60, dumped;
    static unsigned last_locks = ~0u;
    if (!checked) {
        const char *m = getenv("XBOX_DUMP_FB_MAX");
        dir = getenv("XBOX_DUMP_FB");
        if (m && *m) max_frames = atoi(m);
        checked = 1;
    }
    if (!dir || !*dir || dumped >= max_frames) return;
    if (!g_fb_src || !g_fb_w || !g_fb_h || g_fb_locks == last_locks) return;   /* a new decoded frame only */
    last_locks = g_fb_locks;

    char path[1024];
    snprintf(path, sizeof path, "%s/fb_%05u.ppm", dir, (unsigned)dumped);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", g_fb_w, g_fb_h);
    for (unsigned y = 0; y < g_fb_h; y++) {
        const uint8_t *row = (const uint8_t *)g_fb_src + (size_t)y * g_fb_pitch;
        for (unsigned x = 0; x < g_fb_w; x++) {          /* X8R8G8B8, little endian: B G R X */
            uint8_t px[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0] };
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    dumped++;
}

/* ======================================================================= */
/* d3d8_* : present, window, display options                               */
/* ======================================================================= */

/* One frame has been presented (called from the flip / clear paths below and,
 * as the video fallback, from nv2a_live_pb.c). */
void d3d8_PresentFrame(void)
{
    g_present_seq++;
    g_last_present_ms = now_ms();
    dump_guest_fb();
}

unsigned d3d8_PresentSeq(void)   { return g_present_seq; }
DWORD    d3d8_MsSincePresent(void) { return (DWORD)(now_ms() - g_last_present_ms); }
void     d3d8_PumpMessages(void) {}
int      d3d8_VideoPresentReady(void) { return 1; }

void d3d8_SetGuestFramebuffer(const void *src, unsigned pitch, unsigned w, unsigned h)
{ g_fb_src = src; g_fb_pitch = pitch; g_fb_w = w; g_fb_h = h; }
void d3d8_SetGuestFramebufferAlt(const void *alt) { g_fb_alt = alt; }
void d3d8_NoteGuestFramebufferLock(void) { g_fb_lock_seq = g_present_seq; g_fb_locks++; }
int  d3d8_HasGuestFramebuffer(void)      { return g_fb_src != NULL; }
unsigned d3d8_GuestFramebufferLockCount(void) { return g_fb_locks; }
int  d3d8_GuestFramebufferActive(void)
{
    return g_fb_src && (g_present_seq - g_fb_lock_seq) < GUEST_FB_IDLE_PRESENTS;
}

void d3d8_RequestScreenshot(const wchar_t *path) { (void)path; }
int  d3d8_ScreenshotPending(void)                { return 0; }

void d3d8_SetDacPalette(const unsigned char lut[256][3]) { (void)lut; }
void d3d8_SetShowFps(int on)                       { (void)on; }
void d3d8_SetPresentBoxShape(double shape)         { (void)shape; }
void d3d8_SetPointZoom(float zoom)                 { (void)zoom; }
void d3d8_SetMsaa(int samples)                     { (void)samples; }
void d3d8_SetHostUiHooks(const D3D8HostUiHooks *h) { (void)h; }
void d3d8_SetHostDisplay(unsigned w, unsigned h, int widescreen, int fullscreen)
{ (void)w; (void)h; (void)widescreen; (void)fullscreen; }
void d3d8_SetHostAspect(double aspect)             { (void)aspect; }
void d3d8_SetAnisotropy(int n)                     { (void)n; }
void d3d8_PassPhaseChanged(int o, int n, unsigned tag) { (void)o; (void)n; (void)tag; }
void d3d8_HostSetFullscreen(int on)                { (void)on; }
int  d3d8_HostIsFullscreen(void)                   { return 0; }
void d3d8_HostExit(void)                           { exit(0); }
HWND d3d8_GetHostWindow(void)                      { return NULL; }
int  d3d8_monitor_hz(HWND hwnd)                    { (void)hwnd; return 60; }
void d3d8_sync_set_pace(int fps)                   { (void)fps; }
int  d3d8_post_split_wanted(void)                  { return 0; }
void gpuprof_init(void)                            {}
int  d3d8_pump_alt_off(void)                       { return 0; }

/* occlusion: reports are completed at GET_REPORT, so nothing is ever outstanding */
void  d3d8_OcclusionFlush(void) {}
int   d3d8_OcclusionPoll(int h, unsigned long long *samples) { (void)h; if (samples) *samples = 0; return 1; }
float d3d8_OcclusionScale(void) { return 1.0f; }

/* The host device the translator would draw into. The title never calls
 * Direct3DCreate8 (its D3D8 is linked into the game); nv2a_live_pb.c creates one.
 * Here the "device" is a handle that is never dereferenced by anything but
 * CreateDevice below. */
static IDirect3DDevice8 *g_null_dev = (IDirect3DDevice8 *)(uintptr_t)0x1;

static HRESULT __stdcall null_CreateDevice(IDirect3D8 *self, UINT adapter, DWORD type, HWND wnd, DWORD flags,
                                           D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **out)
{
    (void)self; (void)adapter; (void)type; (void)wnd; (void)flags; (void)pp;
    *out = g_null_dev;
    return 0;   /* S_OK */
}

IDirect3D8 *xbox_Direct3DCreate8(UINT sdk)
{
    static IDirect3D8Vtbl vtbl;
    static IDirect3D8 obj;
    (void)sdk;
    memset(&vtbl, 0, sizeof vtbl);
    vtbl.CreateDevice = null_CreateDevice;
    obj.lpVtbl = &vtbl;
    return &obj;
}
IDirect3DDevice8 *xbox_GetD3DDevice(void) { return g_null_dev; }

HRESULT d3d8_CreateTextureFromLevels(UINT w, UINT h, UINT levels, const void *const *bits,
                                     IDirect3DTexture8 **ppTex)
{ (void)w; (void)h; (void)levels; (void)bits; if (ppTex) *ppTex = NULL; return (HRESULT)0x8876086C; /* D3DERR_INVALIDCALL */ }

/* ======================================================================= */
/* pgraph_d3d11_* : the NV2A method stream                                 */
/* ======================================================================= */

void pgraph_d3d11_init(void)     { g_initialized = 1; }
void pgraph_d3d11_shutdown(void) { g_initialized = 0; }
void pgraph_d3d11_set_mem_base(void *base, uint32_t size) { g_mem = (uint8_t *)base; g_mem_size = size; }
void pgraph_d3d11_tick_begin(void) {}
void pgraph_d3d11_poll_reports(void) {}
int  pgraph_d3d11_reports_pending(void) { return 0; }
unsigned pgraph_d3d11_draws_pending(void) { return g_draws_since_present; }
uint32_t pgraph_d3d11_draw_count(void) { return g_draw_total; }
int  pgraph_d3d11_take_frame_complete(void) { return 0; }

int pgraph_d3d11_flipping(void)
{
    return g_flip_mode && now_ms() - g_last_flip_ms < 250 && !d3d8_GuestFramebufferActive();
}

void pgraph_d3d11_flush(void) {}
void pgraph_d3d11_present(uint32_t crtc_start) { (void)crtc_start; d3d8_PresentFrame(); }
void pgraph_d3d11_present_guest_fb(uint32_t va, uint32_t pitch, uint32_t w, uint32_t h)
{ (void)va; (void)pitch; (void)w; (void)h; }
void pgraph_d3d11_test_quad(void) {}
void pgraph_d3d11_report_nops(void) {}
void pgraph_d3d11_report_occlusion(void) {}
void pgraph_d3d11_perf_counts(unsigned *draws, unsigned *methods, unsigned *verts)
{ if (draws) *draws = g_stats.draw_calls; if (methods) *methods = g_stats.methods_handled; if (verts) *verts = 0; }
void pgraph_d3d11_get_stats(PgraphD3D11Stats *out) { if (out) *out = g_stats; }

/* The title-side hooks and display options of the OpenTricky renderer: nothing draws. */
typedef int (*pgraph_plag_fn)(uint32_t q, uint32_t *cb, const uint8_t **snap);   /* declared in the port's pumplag.c */
void pgraph_d3d11_set_chyron_scroll(uint32_t frame) { (void)frame; }
void pgraph_d3d11_set_hud_scale(float kx, float ky) { (void)kx; (void)ky; }
void pgraph_d3d11_set_box(float k)                  { (void)k; }
void pgraph_d3d11_set_box_race2d(float k)           { (void)k; }
void pgraph_d3d11_set_box_frame(float k)            { (void)k; }
void pgraph_d3d11_set_plag_check(pgraph_plag_fn fn) { (void)fn; }
void pgraph_d3d11_set_pass_phase_callback(pgraph_pass_phase_fn fn) { (void)fn; }
void pgraph_d3d11_set_draw_subst(pgraph_draw_subst_fn fn)          { (void)fn; }
int  pgraph_d3d11_pass_phase(void) { return 0; }
void pgraph_d3d11_tex_state(int stage, uint32_t *format, uint32_t *filter, uint32_t *address,
                            uint32_t *control0, int *enabled)
{
    (void)stage;
    if (format) *format = 0;   if (filter) *filter = 0;
    if (address) *address = 0; if (control0) *control0 = 0;
    if (enabled) *enabled = 0;
}
void pgraph_d3d11_tex_binding(int stage, uint32_t *offset, uint32_t *format)
{ (void)stage; if (offset) *offset = 0; if (format) *format = 0; }
uint32_t pgraph_d3d11_tex_address(int stage) { (void)stage; return 0; }

/* diagnostics wired from the port's main.c */
void nv2a_skipprog_set(uint32_t h) { (void)h; }
void nv2a_ignored_dump(void)       {}
void nv2a_drawlog_arm(int frames)  { (void)frames; }

/* Reached through nv2a_live_pb.c (kept) and nv2a_core.c. Accept every method;
 * track only what the guest can see. */
int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    (void)subchannel;
    g_stats.methods_handled++;

    switch (method) {
    case NV097_NO_OPERATION:
        if ((param >> 24) == 0x5Cu) pgraph_d3d11_fork_fence = param & 0xFFFFFFu;   /* the guest's draw fence */
        return 1;

    case NV097_SET_BEGIN_END:
        if (param != 0) { g_draws_since_present++; g_draw_total++; g_stats.draw_calls++; }
        return 1;

    case NV097_GET_REPORT:
        /* The GPU would write {timestamp, count, status = 0} at the report's
         * address; the title zeroed nothing but set status to 0xFFFFFFFF and
         * spins until it reads 0. Write the count, then (release) the status. */
        if ((param >> 24) == NULL_REPORT_ZPASS) {
            uint32_t addr = param & 0x00FFFFFFu;
            if (g_mem && addr + 16 <= g_mem_size) {
                uint8_t *p = g_mem + addr;
                uint64_t ts = now_ms();
                memcpy(p, &ts, 8);
                *(volatile uint32_t *)(p + 8) = report_value();
                __atomic_store_n((volatile uint32_t *)(p + 12), 0u, __ATOMIC_RELEASE);
                g_report_total++;
            }
        }
        return 1;

    case NV097_SET_SURFACE_COLOR_OFFSET:
        /* The title double-buffers by pointing the render surface at the other
         * buffer: the only frame boundary it gives. Present at the flip, and only
         * if something was drawn, as the translator does. */
        if (g_surface_seen && param != g_surface_offset) {
            g_flip_mode = 1;
            g_last_flip_ms = now_ms();
            if (g_draws_since_present > 0) {
                g_draws_since_present = 0;
                d3d8_PresentFrame();
            }
        }
        g_surface_offset = param;
        g_surface_seen = 1;
        return 1;

    case NV097_CLEAR_SURFACE:
        g_clear_total++; g_stats.clears++;
        /* A clear starts a frame: before the title has flipped (no flip seen
         * recently) whatever was drawn is a finished frame. */
        if (g_draws_since_present > 0 && !pgraph_d3d11_flipping()) {
            g_draws_since_present = 0;
            d3d8_PresentFrame();
        }
        return 1;

    default:
        return 1;
    }
}
