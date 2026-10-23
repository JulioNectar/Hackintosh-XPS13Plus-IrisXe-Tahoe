#!/bin/zsh
# Build a single-file C++ kext without Xcode: build-kext.sh <Name>
# Source: tools/<Name>/*.cpp + Info.plist  ->  tools/build/<Name>.kext
set -e
NAME=$1
SRC=${0:A:h}/$NAME
OUT=${0:A:h}/build/$NAME.kext
SDK=$(xcrun --show-sdk-path)
KH=$SDK/System/Library/Frameworks/Kernel.framework/Headers

rm -rf $OUT
mkdir -p $OUT/Contents/MacOS
cp $SRC/Info.plist $OUT/Contents/Info.plist

BID=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" $SRC/Info.plist)
VER=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" $SRC/Info.plist)
INFO=$(mktemp -d)/kmod_info.c
cat > $INFO <<EOC
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL($BID, "$VER", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
EOC
clang -arch x86_64 -mkernel -nostdinc -fno-builtin -fno-common -mmacosx-version-min=11.0 \
  -DKERNEL -I$KH -isystem $KH -c $INFO -o ${INFO%.c}.o

clang++ -arch x86_64 -std=gnu++17 -O2 -g -Wno-deprecated-declarations \
  -mkernel -nostdlib -nostdinc -nostdinc++ \
  -fno-builtin -fno-exceptions -fno-rtti -fno-common -fapple-kext \
  -mmacosx-version-min=11.0 \
  -D__KERNEL__ -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
  -I$KH -isystem $KH \
  -Xlinker -kext -Xlinker -export_dynamic \
  -lkmod -lkmodc++ -lcc_kext \
  $SRC/*.cpp ${INFO%.c}.o -o $OUT/Contents/MacOS/$NAME

codesign -f -s - $OUT >/dev/null 2>&1 || true
echo "built $OUT"
