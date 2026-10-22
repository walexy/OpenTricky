
## 2026-10-21: step 3 (smoothness): findings
- Shader/pipeline creation measured during a race (`[GPU]   N shaders/pipelines created` lines): about 1 ms each, at most a few ms per period. A disk cache would
  gain nothing visible; not built. (Metal's own cache covers repeats; a cold first run on another Mac is untested.)
- ProMotion / high refresh: `OT_FPS=monitor` already follows the display (120 on ProMotion) with the game's in-between frames; nothing missing.
- MetalFX: SDL's GPU API does not expose the Metal device or command buffer, so MetalFX cannot be attached without leaving SDL's renderer. Instead the SDR
  final pass now upscales with Catmull-Rom bicubic + an anti-ringing sharpen (OT_UPSCALE=sharp|smooth, OT_SHARPEN 0..1; d3d8_gpu_out.c `up_ps`).

## 2026-10-21: step 6
- Audio: SDL3's default-device stream already follows the system output (AirPods etc.); `OT_AUDIO_DEVICE=<name>` picks a specific output (settings window lists them, CoreAudio names).
  Spatial audio for AirPods applies at the system level to the stereo stream ("Spatialize Stereo"); nothing app-side to add.
- Netplay (port/src/netplay, XBOX_NET): initialises on macOS ("[NET] active") but is a localhost-only prototype; with host + guest both started into a race by
  `macos/tests/drive.py` both instances closed ("[WINDOW] closed: exiting") when the race loaded. Two plain instances side by side run fine, so it is netplay-specific; not investigated further.
- Upstream PR: text in macos/UPSTREAM_PR.md; not opened (it is a public action on someone else's repository).

## 2026-10-21: edge-adaptive upscaling
`OT_UPSCALE=edge` (the default; `sharp` = bicubic, `smooth` = the plain blit) adds an FSR 1 style filter to the SDR final pass (`easu` in d3d8_gpu_out.c, an original
implementation of the idea): a 4x4 patch gives the local gradient, the Lanczos-2 shaped kernel is stretched along the edge, the result is held inside the four nearest texels,
then the same mild unsharp (OT_SHARPEN). Seen against `smooth` on the title menu (480p into a 900p window): cleaner outlines and snowflake detail, no ringing.
