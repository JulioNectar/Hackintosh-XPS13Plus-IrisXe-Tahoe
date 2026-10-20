#!/bin/zsh
# Builds NootedGreen.kext (this fork, src/NootedGreen) without Xcode, using only the Command Line Tools.
#   zsh build-nootedgreen.sh            -> build/NootedGreen.kext
# Fetches the build dependencies the first time: acidanthera/MacKernelSDK (git) and the Lilu DEBUG release
# (headers + plugin_start.cpp). Copy build/NootedGreen.kext into EFI/OC/Kexts.
set -e
ROOT=${0:A:h}
SRC=$ROOT/src/NootedGreen
DEPS=$ROOT/build/deps
OUT=$ROOT/build/NootedGreen.kext
LILU_VER=1.7.2
NAME=NootedGreen; BID=com.StezzaPilot.NootedGreen; VER=1.0.0
mkdir -p $DEPS
[[ -d $DEPS/MacKernelSDK ]] || git clone --depth 1 https://github.com/acidanthera/MacKernelSDK.git $DEPS/MacKernelSDK
if [[ ! -d $DEPS/Lilu.kext ]]; then
  curl -fL -o $DEPS/lilu.zip https://github.com/acidanthera/Lilu/releases/download/$LILU_VER/Lilu-$LILU_VER-DEBUG.zip
  (cd $DEPS && unzip -q -o lilu.zip)
fi
SDK=$DEPS/MacKernelSDK
LILU=$DEPS/Lilu.kext/Contents/Resources
TMP=$(mktemp -d)

rm -rf $OUT; mkdir -p $OUT/Contents/MacOS
sed -e "s/\$(PRODUCT_BUNDLE_IDENTIFIER)/$BID/g" -e "s/\$(PRODUCT_NAME:rfc1034identifier)/$NAME/g" \
    -e "s/\$(PRODUCT_NAME)/$NAME/g" -e "s/\$(EXECUTABLE_NAME)/$NAME/g" -e "s/\$(MODULE_VERSION)/$VER/g" \
    -e "s/\$(CURRENT_PROJECT_VERSION)/$VER/g" -e "s/\$(DEVELOPMENT_LANGUAGE)/en/g" -e "s/\$(MODULE_NAME)/$BID/g" \
    $SRC/Info.plist > $OUT/Contents/Info.plist

FLAGS=(-arch x86_64 -mkernel -nostdinc -fno-builtin -fno-common -fno-stack-protector
       -mmacosx-version-min=13.0 -mmmx -msse -msse2
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
       -DDEBUG=1 -DAPPLE_KEXT_ASSERTIONS=1 -DPRODUCT_NAME=$NAME -DMODULE_VERSION=$VER
       -isystem $SDK/Headers -I$LILU -O2 -g)
cat > $TMP/info.c <<EOC
#include <mach/mach_types.h>
extern kern_return_t ${NAME}_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t ${NAME}_kern_stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($BID, "$VER", ${NAME}_kern_start, ${NAME}_kern_stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
EOC
clang $FLAGS -c $TMP/info.c -o $TMP/info.o
for s in DisplayMergeNub DYLDPatches Firmware FirmwareADLP HDMI IntelDPLinkTraining kern_gen11 kern_genx kern_green kern_patcherplus kern_start; do
  clang++ $FLAGS -std=gnu++17 -nostdinc++ -fno-exceptions -fno-rtti -fapple-kext \
    -Wno-deprecated-declarations -Wno-inconsistent-missing-override -Wno-unused-parameter -c $SRC/$s.cpp -o $TMP/$s.o
done
clang++ $FLAGS -std=gnu++17 -nostdinc++ -fno-exceptions -fno-rtti -fapple-kext -c $LILU/Library/plugin_start.cpp -o $TMP/plugin_start.o
clang++ -arch x86_64 -mkernel -nostdlib -static -Xlinker -kext -Xlinker -export_dynamic -mmacosx-version-min=13.0 \
  $TMP/*.o $SDK/Library/x86_64/libkmod.a -lcc_kext -o $OUT/Contents/MacOS/$NAME
codesign -f -s - $OUT >/dev/null 2>&1 || true
rm -rf $TMP
echo "built $OUT"
