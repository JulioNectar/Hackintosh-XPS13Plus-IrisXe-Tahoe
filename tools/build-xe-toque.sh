#!/bin/zsh
# Builds tools/xe-toque.app (touchscreen gestures, trail/ripple, pen, magnifier, menu-bar toggles).
# Start it with `open tools/xe-toque.app`, stop with `killall xe-toque`. Diagnostic: xe-toque --probe-events.
set -e
cd "${0:A:h}"
A=xe-toque.app/Contents
mkdir -p $A/MacOS
clang -fobjc-arc -O2 -Wall -Wno-unused-function -framework Cocoa -framework IOKit -framework QuartzCore \
  -framework ApplicationServices -framework ScreenCaptureKit \
  xe-toque.m xe-toque-probe.m -o $A/MacOS/xe-toque
cat > $A/Info.plist <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>local.xe.toque</string>
<key>CFBundleName</key><string>xe-toque</string>
<key>CFBundleExecutable</key><string>xe-toque</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>2.0</string>
<key>CFBundleIconFile</key><string>AppIcon</string>
<key>LSUIElement</key><true/>
</dict></plist>
PL
mkdir -p $A/Resources && cp icons/xe-toque.icns $A/Resources/AppIcon.icns
# Ad-hoc signature with a designated requirement on the bundle identifier only, so the privacy grants
# (Input Monitoring, Accessibility, Screen Recording) survive rebuilds instead of being tied to the cdhash.
codesign -f -s - -r='designated => identifier "local.xe.toque"' xe-toque.app
echo "built $PWD/xe-toque.app"
