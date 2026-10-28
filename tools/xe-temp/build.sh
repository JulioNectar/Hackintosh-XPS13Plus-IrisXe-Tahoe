#!/bin/zsh
# Builds tools/build/xe-temp.app (menu-bar temperatures). Run it with: open tools/build/xe-temp.app
set -e
D=${0:A:h}; OUT=$D/../build/xe-temp.app
rm -rf $OUT; mkdir -p $OUT/Contents/MacOS
swiftc -O -parse-as-library -target x86_64-apple-macos26.0 -o $OUT/Contents/MacOS/xe-temp $D/XeTemp.swift
cat > $OUT/Contents/Info.plist <<EOP
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>local.xe.temp</string>
<key>CFBundleName</key><string>xe-temp</string>
<key>CFBundleExecutable</key><string>xe-temp</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>LSMinimumSystemVersion</key><string>26.0</string>
<key>CFBundleIconFile</key><string>AppIcon</string>
<key>LSUIElement</key><true/>
</dict></plist>
EOP
mkdir -p $OUT/Contents/Resources && cp $D/../icons/xe-temp.icns $OUT/Contents/Resources/AppIcon.icns
codesign -f -s - -r='designated => identifier "local.xe.temp"' $OUT >/dev/null 2>&1 || true
echo "built $OUT"
