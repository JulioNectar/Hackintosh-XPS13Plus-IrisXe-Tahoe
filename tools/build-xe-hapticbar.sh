#!/bin/zsh
# Builds tools/xe-hapticbar.app: a menu-bar slider (hand icon) for the touchpad click-vibration intensity (0-100).
# It runs the bundled xe-haptic (tools/xe-haptic.c) with `set <N> 100` at launch, 8 s later, and on every change.
# Start it with `open tools/xe-hapticbar.app` (add it to Login Items to keep the setting after a reboot).
set -e
cd "${0:A:h}"
A=xe-hapticbar.app/Contents
mkdir -p $A/MacOS
clang -O2 -Wall -o $A/MacOS/xe-haptic xe-haptic.c -framework IOKit -framework CoreFoundation
clang -fobjc-arc -O2 -Wall -framework Cocoa xe-hapticbar.m -o $A/MacOS/xe-hapticbar
cat > $A/Info.plist <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>local.xe.hapticbar</string>
<key>CFBundleName</key><string>xe-hapticbar</string>
<key>CFBundleExecutable</key><string>xe-hapticbar</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSUIElement</key><true/>
</dict></plist>
PL
codesign -f -s - -r='designated => identifier "local.xe.hapticbar"' xe-hapticbar.app
echo "built $PWD/xe-hapticbar.app"
