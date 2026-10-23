#!/bin/zsh
# Builds every user-space piece into macos/out/ (no sudo):
#   zsh macos/build-all.sh <path to the sle/ folder of the NootedGreen project>
# <sle/> holds Apple's GPUDriversIntel 16.0.32 bundles (not redistributed here).
#   out/libxedrv.dylib                          Metal driver adapter (src/libxedrv.m)
#   out/AppleIntelTGLGraphicsMTLDriver.bundle   patched Metal driver ("MTL17") + libxedrv
#   out/xe.bundle                               shader compiler stack
# Then: sudo zsh macos/install-macos.sh <sle/> <le/>
set -e
SLE=${1:?usage: build-all.sh <NootedGreen sle_Internal/sle folder> (16.0.32 bundles)}
HERE=${0:A:h}; OUT=$HERE/out
[[ -d $SLE/AppleIntelTGLGraphicsMTLDriver.bundle ]] || { echo "$SLE has no AppleIntelTGLGraphicsMTLDriver.bundle: pass NootedGreen's sle_Internal/sle folder"; exit 1; }
# FAT/exFAT (USB sticks) keep macOS metadata in "._" files: bundles copied there break codesign and the build
for d in $HERE $SLE; do
  fs=$(diskutil info "$(df -P "$d" | awk 'NR==2{print $1}')" 2>/dev/null | awk -F': *' '/Type \(Bundle\)/{print $2}')
  [[ $fs == (apfs|hfs) ]] || { echo "$d is on a '$fs' volume: put the repository and NootedGreen on an APFS disk (e.g. your home folder)"; exit 1; }
done
mkdir -p $OUT
clang -arch x86_64 -mmacosx-version-min=15.0 -dynamiclib -fno-objc-arc -O2 \
  -framework Foundation -framework Metal -framework IOSurface -lobjc \
  -install_name /Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/libxedrv.dylib \
  -o $OUT/libxedrv.dylib $HERE/src/libxedrv.m
codesign -f -s - $OUT/libxedrv.dylib
python3 $HERE/patch-mtl17.py $SLE/AppleIntelTGLGraphicsMTLDriver.bundle $OUT/libxedrv.dylib $OUT
zsh $HERE/build-shader-compiler.sh $SLE $OUT
echo "ok: $OUT"
