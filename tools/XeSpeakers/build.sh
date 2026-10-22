#!/bin/zsh
# Build the XeSpeakers AudioServerPlugIn (HAL driver) without Xcode: zsh tools/XeSpeakers/build.sh
#   -> tools/build/XeSpeakers.driver (x86_64, ad-hoc signed)
# Install (user, sudo): sudo cp -R tools/build/XeSpeakers.driver /Library/Audio/Plug-Ins/HAL/ && sudo killall coreaudiod
# Needs AudShim 0.8.0 (interface v8) loaded; without it the plug-in publishes no device.
set -e
SRC=${0:A:h}
OUT=${0:A:h:h}/build/XeSpeakers.driver
rm -rf $OUT
mkdir -p $OUT/Contents/MacOS $OUT/Contents/Resources
cp $SRC/Info.plist $OUT/Contents/Info.plist
plutil -lint $OUT/Contents/Info.plist >/dev/null
clang -arch x86_64 -O2 -Wall -Wextra -bundle -mmacosx-version-min=11.0 \
  -framework CoreAudio -framework CoreFoundation -framework IOKit \
  $SRC/XeSpeakers.c -o $OUT/Contents/MacOS/XeSpeakers
codesign -f -s - $OUT
echo "built $OUT"
