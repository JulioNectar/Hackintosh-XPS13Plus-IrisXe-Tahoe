#!/bin/zsh
# Builds xe.bundle, the Metal shader compiler stack used by the patched TGL Metal driver on macOS Tahoe.
#
#   zsh build-shader-compiler.sh <stock sle/ folder> <output dir>
#
# <stock sle/ folder> must contain Apple's GPUDriversIntel 16.0.32 bundles AppleIntelGraphicsShared.bundle and
# AppleIntelTGLGraphicsMTLDriver.bundle (they are not redistributed here). Run it on the Tahoe build you will use:
# it reads the exported symbols of Tahoe's own libLLVM (GPUCompiler 32023).
#
# Why it exists: Tahoe's Metal compiler service talks to compiler plugins with a newer API and LLVM 32023, while
# the Tiger Lake compiler (libigc, gen12lp) only exists in the 16.0.32 plugin, built against LLVM 3902. Layers:
#   libMTLIGCCompilerPlugin.dylib     ours (src/compiler-adapter.cpp): maps the Tahoe entry point
#                                      MTLCompilerBuildRequestWithSerializedBitcode to the old ...WithOptions
#   libMTLIGCCompilerPluginOld.dylib  Apple's 16.0.32 plugin, patched: paths -> xe.bundle, LLVM -> our bridge,
#                                      isPoshEnabled -> false
#   libLLVMbridge.dylib               ours (src/llvm-bridge.cpp): re-exports Tahoe's libLLVM and supplies the few
#                                      LLVM 3902 symbols the old plugin imports
#   libigc.dylib + Resources/*        Apple's 16.0.32 files, re-signed
# The bundle must live in /System/Library/Caches/xe.bundle (sandbox and plugin-path rules, see docs/METAL.md).
set -e
SLE=${1:?pasta sle/ com os bundles 16.0.32}; OUT=${2:?pasta de saída}
HERE=${0:A:h}
SH=$SLE/AppleIntelGraphicsShared.bundle/Contents
MTL=$SLE/AppleIntelTGLGraphicsMTLDriver.bundle/Contents
DEST=/System/Library/Caches/xe.bundle/Contents/MacOS
LLVM=/System/Library/PrivateFrameworks/GPUCompiler.framework/Versions/32023/Libraries/libLLVM.dylib
B=$OUT/xe.bundle/Contents; T=$(mktemp -d)
rm -rf $OUT/xe.bundle; mkdir -p $B/MacOS $B/Resources

# 1. Apple files (16.0.32) copied as-is
cp $SH/Info.plist $SH/PkgInfo $SH/version.plist $B/
cp $SH/Resources/* $B/Resources/
cp $MTL/Resources/runtime.gpuencode.split $B/Resources/
for f in libigc.dylib libigcHSW.dylib libIGIL-Metal.dylib libISAProfilerDyn.dylib; do cp $SH/MacOS/$f $B/MacOS/; done

# 2. tbd stub of Tahoe's libLLVM, generated from the running system
{
  print -- "--- !tapi-tbd\ntbd-version: 4\ntargets: [ x86_64-macos ]\ninstall-name: '$LLVM'\ncurrent-version: 1\nexports:\n  - targets: [ x86_64-macos ]\n    symbols: ["
  /usr/bin/dyld_info -exports $LLVM | awk '$1 ~ /^0x/ {print "      '\''" $2 "'\'',"}' | sed '$ s/,$//'
  print -- "    ]\n..."
} > $T/libLLVM32023.tbd

# 3. LLVM bridge
clang++ -arch x86_64 -dynamiclib -std=c++17 -O2 -mmacosx-version-min=15.0 \
  -install_name $DEST/libLLVMbridge.dylib -compatibility_version 1.0.0 -current_version 3902.30.1 \
  $HERE/src/llvm-bridge.cpp -Wl,-reexport_library,$T/libLLVM32023.tbd -o $B/MacOS/libLLVMbridge.dylib

# 4. Apple's old plugin, patched
P=$B/MacOS/libMTLIGCCompilerPluginOld.dylib
cp $SH/MacOS/libMTLIGCCompilerPlugin.dylib $P
codesign --remove-signature $P
install_name_tool -id $DEST/libMTLIGCCompilerPluginOld.dylib \
  -change /System/Library/PrivateFrameworks/GPUCompiler.framework/Versions/3902/Libraries/libLLVM.dylib $DEST/libLLVMbridge.dylib $P
python3 - $P $HERE/plugin-old-patches.json <<'PY'
import json, sys
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
for x in json.load(open(sys.argv[2])):
    o, old, new = x['off'], bytes.fromhex(x['old']), bytes.fromhex(x['new'])
    if b[o:o + len(old)] != old: sys.exit('plugin antigo: bytes em 0x%x não conferem (bundle 16.0.32 errado?)' % o)
    b[o:o + len(new)] = new
open(p, 'wb').write(b)
PY

# 5. adapter (the plugin Tahoe loads), re-exporting the old plugin
clang++ -arch x86_64 -dynamiclib -std=c++17 -O2 -mmacosx-version-min=15.0 \
  -install_name $DEST/libMTLIGCCompilerPlugin.dylib \
  $HERE/src/compiler-adapter.cpp $T/libLLVM32023.tbd -Wl,-reexport_library,$P -o $B/MacOS/libMTLIGCCompilerPlugin.dylib

# 6. ad-hoc signatures
for f in $B/MacOS/*.dylib; do codesign -f -s - $f; done
codesign -f -s - $OUT/xe.bundle 2>/dev/null || true
rm -rf $T
echo "built $OUT/xe.bundle  (install: sudo ditto it to /System/Library/Caches/xe.bundle; chown -R root:wheel)"
