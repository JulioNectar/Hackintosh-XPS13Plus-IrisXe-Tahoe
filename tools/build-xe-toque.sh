#!/bin/zsh
# Builds tools/xe-toque.app (touchscreen finger trail). Start it with `open tools/xe-toque.app`, stop with `killall xe-toque`.
set -e
cd "${0:A:h}"
A=xe-toque.app/Contents
mkdir -p $A/MacOS
clang -fobjc-arc -O2 -framework Cocoa -framework IOKit -framework QuartzCore xe-toque.m xe-toque-probe.m -o $A/MacOS/xe-toque
cat > $A/Info.plist <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>local.xe.toque</string>
<key>CFBundleName</key><string>xe-toque</string>
<key>CFBundleExecutable</key><string>xe-toque</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSUIElement</key><true/>
</dict></plist>
PL
codesign -f -s - xe-toque.app
echo "built $PWD/xe-toque.app"
