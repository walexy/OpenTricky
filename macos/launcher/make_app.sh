#!/bin/bash
# Build "SSX Tricky.app" around a built game binary.
#   bash macos/launcher/make_app.sh [path/to/"SSX Tricky" binary] [output folder]
# Defaults: the GPU build in ../../../build/gpu and OpenTricky/dist.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="${1:-$ROOT/../build/gpu/SSX Tricky}"
OUT="${2:-$ROOT/dist}"
APP="$OUT/SSX Tricky.app"
[ -x "$BIN" ] || { echo "no game binary at: $BIN (build it first: bash build.sh <iso>)"; exit 1; }
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/Resources/ssx-tricky-bin"
python3 "$HERE/make_resources.py" >/dev/null
cp -R "$ROOT/port/assets" "$APP/Contents/Resources/assets"
cp "$HERE/ssx-launcher.sh" "$APP/Contents/MacOS/SSX Tricky"
chmod +x "$APP/Contents/MacOS/SSX Tricky" "$APP/Contents/Resources/ssx-tricky-bin"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>SSX Tricky</string>
  <key>CFBundleDisplayName</key><string>SSX Tricky</string>
  <key>CFBundleIdentifier</key><string>local.opentricky.macos</string>
  <key>CFBundleExecutable</key><string>SSX Tricky</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1.3-dev</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
</dict></plist>
PLIST
codesign --force --deep -s - "$APP" >/dev/null 2>&1 && echo "signed ad hoc" || echo "(codesign failed; the app still runs from a local build)"
echo "built: $APP"
