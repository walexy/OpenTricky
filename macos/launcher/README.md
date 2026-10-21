# macOS launcher

`bash macos/launcher/make_app.sh` builds `dist/SSX Tricky.app` around the GPU build (`../build/gpu/SSX Tricky`; pass another binary and output folder as arguments).

The app is a thin wrapper (`ssx-launcher.sh`):

- asks for your Xbox disc image with a file dialog the first time, and remembers it in `~/Library/Application Support/OpenTricky/disc-image.txt`;
  change it later with `open -a "SSX Tricky" --args --choose-iso`;
- runs the game from `~/Library/Application Support/OpenTricky`, so `settings.ini`, the save folder `hdd`, and `crash.txt` live there (the game keeps its files next to the program it was started as; the wrapper starts it through a link in that folder);
- keeps the last run's console output in `last-run.log` (`last-run.log.prev` for the run before);
- shows a dialog if Homebrew libraries are missing (`brew install sdl3 sdl2-compat openssl@4`) or the game ends abnormally.

Not covered: the game binary still links the Homebrew copies of SDL3, SDL2-compat and OpenSSL (not bundled, so the app is not self-contained and cannot be given to someone without them); the app has no icon; it is ad hoc signed only (a copy downloaded from elsewhere would need Gatekeeper approval); the file-dialog, missing-library and crash dialogs were not exercised (they need a person at the screen).

## Display settings (widescreen, resolution, full screen, screenshots)

By default the game is told the shape of your desktop (4:3 below 1.5, 16:9 up to 1.95, wider shapes as they are, e.g. 21:9) and the GPU draws at 1080 rows (960 at 4:3); the window starts at that shape.
Change it with a file `~/Library/Application Support/OpenTricky/display.env` (read by the app, or export the same names in Terminal):

    OT_SHAPE=21:9        # auto (default) | 4:3 | 16:9 | 21:9 | 32:9 | a number such as 2.39
    OT_RENDER_H=1440     # rows the GPU draws (480 or more; the width follows the shape)
    OT_FULLSCREEN=1      # start in full screen
    OT_BUTTONS=playstation   # button pictures in menus and HUD: modern | playstation | ps2 (default: the game's own Xbox glyphs)
    OT_HD_TEXTURES=/path/to/pack   # your HD texture pack folder (DDS files, not included); OT_HD_MENUS=1: menus too
    OT_HDR=on            # HDR / wide-gamut output (see below); tune with OT_HDR_GAMUT (0..1), OT_HDR_WARM (0..0.5, default 0.1), OT_HDR_BOOST (0..1, default 0.35)
    OT_HDR_SCENE=0.45    # share of the HDR gamut expansion the 3D picture gets (1 = like the HUD and menus)
    OT_SMAA=high         # smoother edges (SMAA) on the 3D: off (default) | low | medium | high | ultra; HUD and menus stay sharp
    OT_SOFT_SHADOWS=on   # soft shadow edges (default off)
    OT_FPS=120           # frame rate limit: 60 (default) | 120 | 144 | 240 | a number | 0 = no limit | monitor
    OT_VSYNC=vsync       # vsync | adaptive | off (default: vsync at 60, present at once above 60)

In the game window: **F11** or **Alt+Enter** toggles full screen, **F12** saves a PNG of the game picture at the render size to `~/Pictures/OpenTricky/`.
The shape is fixed at start (the game is told its screen shape then), so changing `OT_SHAPE` needs a restart. Menus keep the game's own 16:9 frame on wider screens; movies keep 4:3.

Frame rates above 60: the game keeps its logic at 60 Hz and draws extra frames in between, interpolating the camera and riders (port/src/fps_cap.c). Set `OT_FPS`; extra frames are only visible if your display refreshes faster than 60 Hz (on a 60 Hz display the frames are produced and presented but cannot all be shown; `OT_VSYNC=vsync` then caps the rate to the display). Tearing is possible with `off` or above 60 in full screen.

HDR and wide gamut (`OT_HDR=on`): the game's picture is SDR, made for a TV. On a wide-gamut HDR display the app asks for an HDR (extended linear) window and runs a colour pass (`xboxrecomp/src/d3d/d3d8_gpu_out.c`) that reads the colours as Display P3 instead of sRGB (saturated oranges, yellows and reds reach what the panel can show; greys and white stay put), adds a little more chroma to oranges and yellows (`OT_HDR_WARM`), and lifts bright, saturated highlights (fire, lamps, the game's gold) towards the display's HDR headroom, which macOS reports per window and changes with the display's brightness (1.2 on the development machine, so the lift is small there). The 3D picture gets only a share of the gamut expansion (`OT_HDR_SCENE`, default 0.45: photographic colours look oversaturated at full strength, flat graphics do not): at the frame-end marker the 3D is moved part of the way back before the final pass, so the HUD and menus keep the full expansion. Without an HDR display the option is ignored. `macos/hdr_test/hdr_test.c` checks the colour maths on known colours.

Button pictures (`OT_BUTTONS`): the game's button prompts are redrawn as an Xbox Series pad, a PlayStation pad or the PS2 style, from the CC0 drawings in `port/assets/btnicons/` (copied into the app). HD textures (`OT_HD_TEXTURES`): point it at your copy of the community pack (a folder of `.dds` files, or the folder above its `replacements` folder; not included): the game's textures are looked up by hash in `port/assets/hdtex_index.txt` and replaced when the pack has them. Both are off by default.
