#!/bin/zsh
# Builds VoodooGPIO.kext with working Tiger Lake-LP / Alder Lake-P GPIO (INTC1055) without Xcode.
#   zsh build-voodoogpio.sh             -> build/VoodooGPIO.kext
# Clones VoodooI2C/VoodooGPIO at 717ab2a and applies patches/voodoogpio-tigerlakelp-start.diff (upstream's
# VoodooGPIOTigerLakeLP::start skips VoodooGPIOIntel::start, so the controller never maps or registers).
# Keeps the bundle id org.coolstar.VoodooGPIO (VoodooI2C 2.9.1 links against it, compatible version 1.1).
# Copy build/VoodooGPIO.kext into EFI/OC/Kexts/VoodooI2C.kext/Contents/PlugIns.
set -e
ROOT=${0:A:h}
DEPS=$ROOT/build/deps
OUT=$ROOT/build/VoodooGPIO.kext
REV=717ab2a
NAME=VoodooGPIO; BID=org.coolstar.VoodooGPIO; VER=1.2
mkdir -p $DEPS
[[ -d $DEPS/MacKernelSDK ]] || git clone --depth 1 https://github.com/acidanthera/MacKernelSDK.git $DEPS/MacKernelSDK
if [[ ! -d $DEPS/VoodooGPIO ]]; then
  git clone https://github.com/VoodooI2C/VoodooGPIO.git $DEPS/VoodooGPIO
  git -C $DEPS/VoodooGPIO checkout -q $REV
  git -C $DEPS/VoodooGPIO apply $ROOT/patches/voodoogpio-tigerlakelp-start.diff
fi
SRC=$DEPS/VoodooGPIO/VoodooGPIO
SDK=$DEPS/MacKernelSDK
TMP=$(mktemp -d)
rm -rf $OUT; mkdir -p $OUT/Contents/MacOS
sed -e "s/\$(PRODUCT_BUNDLE_IDENTIFIER)/$BID/g" -e "s/\$(PRODUCT_NAME:rfc1034identifier)/$NAME/g" \
    -e "s/\$(PRODUCT_NAME)/$NAME/g" -e "s/\$(EXECUTABLE_NAME)/$NAME/g" -e "s/\$(DEVELOPMENT_LANGUAGE)/en/g" \
    -e "s/\$(MODULE_VERSION)/$VER/g" -e "s/\$(CURRENT_PROJECT_VERSION)/$VER/g" -e "s/\$(MARKETING_VERSION)/$VER/g" \
    $SRC/Info.plist > $OUT/Contents/Info.plist
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VER" -c "Set :CFBundleShortVersionString $VER" $OUT/Contents/Info.plist
/usr/libexec/PlistBuddy -c "Add :OSBundleCompatibleVersion string 1.1" $OUT/Contents/Info.plist 2>/dev/null || /usr/libexec/PlistBuddy -c "Set :OSBundleCompatibleVersion 1.1" $OUT/Contents/Info.plist
grep -q '\$(' $OUT/Contents/Info.plist && { echo "unresolved variable in Info.plist"; grep '\$(' $OUT/Contents/Info.plist; exit 1; }
FLAGS=(-arch x86_64 -mkernel -nostdinc -fno-builtin -fno-common -fno-stack-protector -mmacosx-version-min=11.0
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -isystem $SDK/Headers -O2 -g)
cat > $TMP/info.c <<EOC
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($BID, "$VER", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
EOC
clang $FLAGS -c $TMP/info.c -o $TMP/info.o
for f in $SRC/*.cpp $SRC/*/*.cpp; do
  clang++ $FLAGS -std=gnu++17 -nostdinc++ -fno-exceptions -fno-rtti -fapple-kext -I$SRC \
    -Wno-deprecated-declarations -Wno-inconsistent-missing-override -c $f -o $TMP/$(basename ${f%.cpp}).o
done
clang++ -arch x86_64 -mkernel -nostdlib -static -Xlinker -kext -Xlinker -export_dynamic -mmacosx-version-min=11.0 \
  $TMP/*.o $SDK/Library/x86_64/libkmod.a -lcc_kext -o $OUT/Contents/MacOS/$NAME
codesign -f -s - $OUT >/dev/null 2>&1 || true
rm -rf $TMP; echo "built $OUT"
