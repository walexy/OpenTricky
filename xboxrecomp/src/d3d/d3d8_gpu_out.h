/* d3d8_gpu_out.h -- the final pass of the macOS GPU backend: the picture to the window (or any target) through a
 * colour shader, for HDR / wide-gamut output. See d3d8_gpu_out.c. */
#ifndef D3D8_GPU_OUT_H
#define D3D8_GPU_OUT_H
#include <SDL3/SDL.h>

typedef struct {
    float gamut;        /* 0..1: how far the picture is read as Display P3 instead of sRGB (more saturated, 0 = off) */
    float warm;         /* extra saturation of oranges and yellows (0 = none, 0.25 = a quarter more chroma at the peak) */
    float boost;        /* 0..1: how much of the display's HDR headroom colourful highlights may use */
    float headroom;     /* the display's HDR headroom above SDR white (1 = none): from the window; capped by the caller */
} GpuOutParams;

/* The shader source (malloc'd). */
char *gpu_out_source(void);

/* Draws `src` (any 2D sampled texture, gamma encoded like the title's picture) into `dst` (format dst_fmt) inside the
 * rectangle `dest` (the rest of dst is cleared to black), with the colour transform of `p`. No render pass may be open.
 * Returns 1 if drawn. */
int gpu_out_draw(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, SDL_GPUTexture *dst,
                 SDL_GPUTextureFormat dst_fmt, const SDL_Rect *dest, const GpuOutParams *p);

#endif
