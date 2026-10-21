#!/bin/bash
# SSX Tricky (OpenTricky) macOS launcher: the program an app bundle runs.
#
# - remembers the Xbox disc image (asks once, with a file dialog)
# - runs the game from ~/Library/Application Support/OpenTricky, so settings.ini, the save folder (hdd) and
#   crash reports live there and not inside the app
# - keeps the last run's console output in last-run.log
# - tells you in a dialog if the game could not start or ended abnormally
#
# Change the disc image later:  open -a "SSX Tricky" --args --choose-iso
APP_NAME="SSX Tricky"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/../Resources/ssx-tricky-bin"
SUPPORT="$HOME/Library/Application Support/OpenTricky"
ISO_FILE="$SUPPORT/disc-image.txt"
LOG="$SUPPORT/last-run.log"

say() {   # say <message>: a dialog (the app has no terminal)
    osascript -e 'on run argv' -e 'display dialog (item 1 of argv) with title "SSX Tricky" buttons {"OK"} default button 1 with icon caution' -e 'end run' "$1" >/dev/null 2>&1
}

mkdir -p "$SUPPORT" || { say "Could not create $SUPPORT"; exit 1; }

# the game needs SDL3 and OpenSSL from Homebrew
missing=""
for lib in /opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib /opt/homebrew/opt/openssl@4/lib/libcrypto.4.dylib /opt/homebrew/opt/sdl2-compat/lib/libSDL2-2.0.0.dylib; do
    [ -e "$lib" ] || missing="$missing $(basename "$(dirname "$(dirname "$lib")")")"
done
if [ -n "$missing" ]; then
    say "Missing Homebrew libraries:$missing

Install them in Terminal with:
brew install sdl3 sdl2-compat openssl@4"
    exit 1
fi
[ -x "$BIN" ] || { say "The game program is missing from the app (expected $BIN)."; exit 1; }

choose_iso() {
    osascript -e 'POSIX path of (choose file with prompt "Choose your SSX Tricky (USA) Xbox disc image (.iso)")' 2>/dev/null
}

ISO=""
[ -f "$ISO_FILE" ] && ISO="$(cat "$ISO_FILE")"
for a in "$@"; do [ "$a" = "--choose-iso" ] && ISO=""; done
if [ -z "$ISO" ] || [ ! -f "$ISO" ]; then
    ISO="$(choose_iso)"
    [ -n "$ISO" ] || exit 0                       # cancelled
    printf '%s' "$ISO" > "$ISO_FILE"
fi

# the game keeps its files next to the program it was started as: start it as a link that sits in $SUPPORT
LINK="$SUPPORT/$APP_NAME"
ln -sf "$BIN" "$LINK"

cd "$SUPPORT" || exit 1
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.prev"
"$LINK" --direct "$ISO" 2> "$LOG"
rc=$?
if [ $rc -ne 0 ] && [ $rc -ne 143 ]; then
    say "The game ended unexpectedly (exit code $rc).

Its output is in:
$LOG
$( [ -f "$SUPPORT/crash.txt" ] && echo "and a crash report in $SUPPORT/crash.txt" )"
fi
exit $rc
