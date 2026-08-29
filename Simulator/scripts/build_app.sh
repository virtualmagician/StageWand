#!/usr/bin/env bash
# build_app.sh — release-build AmoledSim and assemble it into dist/AmoledSim.app.
#
# Build artifacts are kept out of Dropbox sync (~/Library/Caches by default); override with
# AMOLEDSIM_BUILD_DIR if you want them elsewhere.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

SCRATCH="${AMOLEDSIM_BUILD_DIR:-$HOME/Library/Caches/AmoledSimBuild}"
mkdir -p "$SCRATCH"

echo "Building AmoledSim (release) — scratch path: $SCRATCH"
swift build -c release --scratch-path "$SCRATCH"

BUILT_BINARY="$SCRATCH/release/AmoledSim"
if [ ! -f "$BUILT_BINARY" ]; then
    echo "error: expected built binary at $BUILT_BINARY, not found" >&2
    exit 1
fi

APP_NAME="AmoledSim.app"
DIST_DIR="$ROOT_DIR/dist"
APP_DIR="$DIST_DIR/$APP_NAME"
CONTENTS_DIR="$APP_DIR/Contents"
MACOS_DIR="$CONTENTS_DIR/MacOS"

echo "Assembling $APP_DIR"
rm -rf "$APP_DIR"
mkdir -p "$MACOS_DIR"

cp "$BUILT_BINARY" "$MACOS_DIR/AmoledSim"
chmod +x "$MACOS_DIR/AmoledSim"

cat > "$CONTENTS_DIR/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>AmoledSim</string>
    <key>CFBundleDisplayName</key>
    <string>AMOLED 1.8 Simulator</string>
    <key>CFBundleIdentifier</key>
    <string>com.marcotempest.amoledsim</string>
    <key>CFBundleShortVersionString</key>
    <string>0.1.0</string>
    <key>CFBundleVersion</key>
    <string>0.1.0</string>
    <key>CFBundleExecutable</key>
    <string>AmoledSim</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
</dict>
</plist>
PLIST

echo "Ad-hoc code-signing $APP_DIR"
codesign --force --sign - "$APP_DIR"

echo "Built $APP_DIR"
