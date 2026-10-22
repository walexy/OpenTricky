# macOS (Apple Silicon) port: proposed upstream pull request

**What it is**: a native macOS build of OpenTricky: SDL3 window, a Metal renderer (SDL GPU API), SDL3 audio, input, a signed self-contained app, a settings window and
iCloud Drive storage for settings and saves. It lives on `walexy/OpenTricky`, branch `macos-headless` (about 60 commits on top of `main`).

**Shared-code changes that matter beyond macOS** (each also builds for Windows in principle; the Windows build of these edits has not been run):
- `kernel_file.c`: ISO directory enumeration fixes (lock leak, buffer bounds), positioned reads/writes, directory-context release on close (POSIX).
- `kernel_sync.c`, `kernel_thread.c`: long relative/absolute waits no longer wrap into short ones.
- `nv2a_pgraph_d3d11.c`: the occlusion span table is indexed by a free slot, not by the backend's query handle (a backend that numbers queries past 255 overran it);
  vertex reads are page-checked; the substitution buffer is only honoured while a substituted draw runs.
- `d3d8_gpu.c` (new Metal backend), `d3d8_gpu_*.c`, `nv2a_psh.c` MSL mode, `mmio_trap.c`, `ot_window.c`: macOS only.

**Not included / open**: notarization (needs the maintainer's Apple credentials), a Windows build of the shared edits, the in-game settings menu entry
(the front end is code-driven; see MACOS_PORT_PLAN.md), netplay (the prototype is localhost-only and its two instances close when a race loads on macOS: not investigated).

**How to try**: `bash macos/launcher/make_app.sh` (see macos/launcher/README.md); tests: `macos/tests/drive.py` (scripted menus and races through a diag console), `hdr_test`, `clip_test`.
