#!/bin/sh
# Builds xe-pblock.app (power button -> lock screen). The app bundle only gives it a stable identity for
# Input Monitoring. It is signed with a requirement on its bundle id, so the grant survives rebuilds.
#   sh tools/xe-pblock/build.sh [OUTDIR]      default: tools/build (git-ignored)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/../build}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
APP=$OUT/xe-pblock.app
mkdir -p "$APP/Contents/MacOS"
clang -fobjc-arc -O2 -Wall -o "$APP/Contents/MacOS/xe-pblock" "$HERE/xe-pblock.m" \
    -framework Cocoa -framework IOKit -framework ApplicationServices
cat > "$APP/Contents/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>local.xe.pblock</string>
<key>CFBundleName</key><string>xe-pblock</string>
<key>CFBundleExecutable</key><string>xe-pblock</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>LSUIElement</key><true/>
</dict></plist>
PL
codesign -f -s - -r='designated => identifier "local.xe.pblock"' "$APP"
echo "built $APP"
