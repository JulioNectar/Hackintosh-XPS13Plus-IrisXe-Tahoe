#!/bin/zsh
# Builds the patched Tiger Lake VA (hardware video decode) plugin from Apple's stock 16.0.32 bundle (no sudo):
#   zsh macos/vadriver/build-vadriver.sh <stock AppleIntelTGLGraphicsVADriver.bundle | sle/ folder> [out dir] [--trace]
# The stock bundle is in the NootedGreen project (sle_Internal/sle, fetched by install.sh into build/deps/NootedGreen);
# it is an Apple binary and is not redistributed here. Output: <out dir>/AppleIntelTGLGraphicsVADriver.bundle
# (default macos/out/), ad-hoc signed. --trace also links tools/diagnostics/xe-vatrace.c (diagnostics only).
# Every patch script checks the original bytes before writing and stops on a mismatch.
# HEVC also needs the accelerator property IOGVAHEVCDecode (NootedGreen XE-GVAHEVC, with -ngreengva).
# Install (needs -ngreengva in boot-args so the accelerator advertises hardware video):
#   sudo rm -rf /Library/GPUBundles/AppleIntelTGLGraphicsVADriver.bundle
#   sudo ditto <out dir>/AppleIntelTGLGraphicsVADriver.bundle /Library/GPUBundles/AppleIntelTGLGraphicsVADriver.bundle
#   killall VTDecoderXPCService     (apps that already opened a decoder need a restart)
set -e
HERE=${0:A:h}; ROOT=${HERE:h:h}
TRACE=0; ARGS=()
for a in "$@"; do [[ $a == --trace ]] && TRACE=1 || ARGS+=$a; done
IN=${ARGS[1]:?usage: build-vadriver.sh <stock VADriver bundle or sle/ folder> [out dir] [--trace]}
OUT=${ARGS[2]:-$ROOT/macos/out}
[[ -d $IN/AppleIntelTGLGraphicsVADriver.bundle ]] && IN=$IN/AppleIntelTGLGraphicsVADriver.bundle
STOCK_SHA=d265f2038135c2c5c3649b621d9aea48cf1cb74a1fb36069bb1b3562b3750129   # GPUDriversIntel 16.0.32 plugin binary
SHA=$(shasum -a 256 $IN/Contents/MacOS/AppleIntelTGLGraphicsVADriver | cut -d' ' -f1)
if [[ $SHA != $STOCK_SHA ]]; then
  echo "input is not the stock 16.0.32 plugin (sha256 $SHA)"; [[ "$XE_FORCE" == 1 ]] || exit 1
fi
mkdir -p $OUT; B=$OUT/AppleIntelTGLGraphicsVADriver.bundle
rm -rf $B; ditto $IN $B
BIN=$B/Contents/MacOS/AppleIntelTGLGraphicsVADriver
python3 -I $HERE/patch-vadriver.py   $BIN   # 1. device-id 9A49, service lookup, bitstream request, driver params, VAME name
python3 -I $HERE/patch_nodes.py      $BIN   # 2. capability nodes 0x430 -> 0x438, next link also at +0x430
python3 -I $HERE/patch_nodelayout.py $BIN   # 3. capability-node content in the Tahoe layout + driver parameter 14
python3 -I $HERE/patch_payloadhdr.py $BIN   # 4. CodecHAL sideband payload header always committed (je at 0x64a15)
python3 -I $HERE/patch_pitchdelta.py $BIN   # 5. SURFACE_STATE pitch relocation delta
python3 -I $HERE/patch_spsshift.py   $BIN   # 6. Tahoe H.264 SPS structure (+8 bytes)
python3 -I $HERE/patch_hcpsurface.py $BIN   # 7. HEVC: emit HCP_SURFACE_STATE (the stock builder never adds it)
python3 -I $HERE/patch_hevclastslice.py $BIN  # 8. HEVC: set LastSliceOfPic (Tahoe AppleGVA leaves it 0)
if [[ $TRACE == 1 ]]; then
  python3 -I $HERE/add-tracer.py $BIN
  clang -arch x86_64 -mmacosx-version-min=15.0 -dynamiclib -O2 -framework IOKit -framework IOSurface -framework CoreFoundation \
    -F/System/Library/PrivateFrameworks -framework IOAccelerator \
    -install_name /Library/GPUBundles/AppleIntelTGLGraphicsVADriver.bundle/Contents/MacOS/xe-vatrace.dylib \
    -o $B/Contents/MacOS/xe-vatrace.dylib $ROOT/tools/diagnostics/xe-vatrace.c
  codesign -f -s - $B/Contents/MacOS/xe-vatrace.dylib
fi
codesign -f -s - $B
echo "ok: $B"
shasum -a 256 $BIN
