
## 2026-10-21: step 3 (smoothness): findings
- Shader/pipeline creation measured during a race (`[GPU]   N shaders/pipelines created` lines): about 1 ms each, at most a few ms per period. A disk cache would
  gain nothing visible; not built. (Metal's own cache covers repeats; a cold first run on another Mac is untested.)
- ProMotion / high refresh: `OT_FPS=monitor` already follows the display (120 on ProMotion) with the game's in-between frames; nothing missing.
- MetalFX: SDL's GPU API does not expose the Metal device or command buffer, so MetalFX cannot be attached without leaving SDL's renderer. Instead the SDR
  final pass now upscales with Catmull-Rom bicubic + an anti-ringing sharpen (OT_UPSCALE=sharp|smooth, OT_SHARPEN 0..1; d3d8_gpu_out.c `up_ps`).
