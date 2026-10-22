#!/bin/zsh
# Builds HookCase.kext (src/HookCase) without Xcode, using only the Command Line Tools.
#   zsh build-hookcase.sh            -> build/HookCase.kext
# The TGL kexts (AppleIntelTGLGraphics + Framebuffer) link against org.smichaud.HookCase, so kmutil refuses to
# register them without it. macos/install-macos.sh installs build/HookCase.kext into /Library/Extensions.
# src/HookCase is NootedGreen's modified HookCase (Steven Michaud, MIT, src/LICENSE-HookCase) with two fixes:
# probePortMode read through a never-set `callback` (NULL deref), and strnstr uses the SDK signature so the
# framebuffer's _strnstr import resolves. The IOKit personality is dropped so it only loads as a dependency.
set -e
ROOT=${0:A:h}
SRC=$ROOT/src/HookCase
DEPS=$ROOT/build/deps
OUT=$ROOT/build/HookCase.kext
NAME=HookCase; BID=org.smichaud.HookCase; VER=8.0.1
mkdir -p $DEPS
[[ -d $DEPS/MacKernelSDK ]] || git clone --depth 1 https://github.com/acidanthera/MacKernelSDK.git $DEPS/MacKernelSDK
SDK=$DEPS/MacKernelSDK
TMP=$(mktemp -d)

rm -rf $OUT; mkdir -p $OUT/Contents/MacOS
python3 -I - "$SRC/Info.plist" "$OUT/Contents/Info.plist" <<EOF
import plistlib, sys
d = plistlib.load(open(sys.argv[1], "rb"))
sub = {"\$(EXECUTABLE_NAME)": "$NAME", "\$(PRODUCT_BUNDLE_IDENTIFIER)": "$BID",
       "\$(PRODUCT_NAME)": "$NAME", "\$(MODULE_VERSION)": "$VER", "\$(DEVELOPMENT_LANGUAGE)": "en"}
for k, v in list(d.items()):
    if isinstance(v, str): d[k] = sub.get(v, v)
d.pop("IOKitPersonalities", None)
plistlib.dump(d, open(sys.argv[2], "wb"), fmt=plistlib.FMT_XML)
EOF
grep -q '\$(' $OUT/Contents/Info.plist && { echo "unresolved \$(...) in Info.plist"; exit 1; }

FLAGS=(-arch x86_64 -mkernel -nostdinc -fno-builtin -fno-common -fno-stack-protector
       -mmacosx-version-min=13.0 -mmmx -msse
       -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DDEBUG=1 -D__null_terminated= -D__stateful_pure=
       -isystem $SDK/Headers -I$SRC -O0 -g)

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
clang $FLAGS -c $SRC/HookCase.s -o $TMP/HookCase_s.o
clang++ $FLAGS -std=gnu++14 -nostdinc++ -fno-exceptions -fno-rtti -fapple-kext \
  -Wno-deprecated-declarations -Wno-inconsistent-missing-override -Wno-unused-parameter \
  -c $SRC/HookCase.cpp -o $TMP/HookCase.o
clang++ -arch x86_64 -mkernel -nostdlib -static -Xlinker -kext -Xlinker -export_dynamic \
  -mmacosx-version-min=13.0 $TMP/*.o $SDK/Library/x86_64/libkmod.a -lcc_kext \
  -o $OUT/Contents/MacOS/$NAME
nm -gU $OUT/Contents/MacOS/$NAME | grep -q ' _strnstr$' || { echo "_strnstr not exported (C linkage)"; exit 1; }
codesign -f -s - $OUT >/dev/null 2>&1 || true
rm -rf $TMP
echo "built $OUT"
