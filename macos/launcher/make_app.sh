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
swiftc -O -swift-version 5 "$HERE/ot-tool.swift" -o "$APP/Contents/Resources/ot-tool" 2>&1 | grep -E "error" || true
[ -x "$APP/Contents/Resources/ot-tool" ] || echo "(settings window not built: swiftc failed; the app still starts the game)"
# app icon: drawn by make_icon.swift, packed into AppIcon.icns
ICONS="$(mktemp -d)"
if swiftc -O "$HERE/make_icon.swift" -o "$ICONS/make_icon" 2>/dev/null && "$ICONS/make_icon" "$ICONS/icon.png"; then
    mkdir "$ICONS/AppIcon.iconset"
    for sz in 16 32 128 256 512; do
        sips -z $sz $sz "$ICONS/icon.png" --out "$ICONS/AppIcon.iconset/icon_${sz}x${sz}.png" >/dev/null
        sips -z $((sz*2)) $((sz*2)) "$ICONS/icon.png" --out "$ICONS/AppIcon.iconset/icon_${sz}x${sz}@2x.png" >/dev/null
    done
    iconutil -c icns "$ICONS/AppIcon.iconset" -o "$APP/Contents/Resources/AppIcon.icns" || echo "(icon not packed)"
else echo "(icon not drawn: swiftc failed)"; fi
rm -rf "$ICONS"
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
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1.3-dev</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
</dict></plist>
PLIST
codesign --force --deep -s - "$APP" >/dev/null 2>&1 && echo "signed ad hoc" || echo "(codesign failed; the app still runs from a local build)"
# Finder and the Dock cache bundle icons by path: refresh the registration so a rebuilt app shows its current icon
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP" >/dev/null 2>&1 || true
touch "$APP"
echo "built: $APP"
