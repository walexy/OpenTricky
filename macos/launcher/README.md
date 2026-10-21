# macOS launcher

`bash macos/launcher/make_app.sh` builds `dist/SSX Tricky.app` around the GPU build (`../build/gpu/SSX Tricky`; pass another binary and output folder as arguments).

The app is a thin wrapper (`ssx-launcher.sh`):

- asks for your Xbox disc image with a file dialog the first time, and remembers it in `~/Library/Application Support/OpenTricky/disc-image.txt`;
  change it later with `open -a "SSX Tricky" --args --choose-iso`;
- runs the game from `~/Library/Application Support/OpenTricky`, so `settings.ini`, the save folder `hdd`, and `crash.txt` live there (the game keeps its files next to the program it was started as; the wrapper starts it through a link in that folder);
- keeps the last run's console output in `last-run.log` (`last-run.log.prev` for the run before);
- shows a dialog if Homebrew libraries are missing (`brew install sdl3 sdl2-compat openssl@4`) or the game ends abnormally.

Not covered: the game binary still links the Homebrew copies of SDL3, SDL2-compat and OpenSSL (not bundled, so the app is not self-contained and cannot be given to someone without them); the app has no icon; it is ad hoc signed only (a copy downloaded from elsewhere would need Gatekeeper approval); the file-dialog, missing-library and crash dialogs were not exercised (they need a person at the screen).
