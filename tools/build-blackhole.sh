#!/bin/zsh
# Compila o BlackHole (saída de áudio virtual, GPL-3.0, github.com/ExistentialAudio/BlackHole) sem Xcode.
# Motivo: o XPS 13 Plus não tem saída de áudio no macOS (SoundWire), e o Safari não toca vídeo sem uma.
#   zsh tools/build-blackhole.sh   -> build/BlackHole2ch.driver
# Instalar: sudo cp -R build/BlackHole2ch.driver /Library/Audio/Plug-Ins/HAL/ && sudo killall coreaudiod
set -e
ROOT=${0:A:h:h}; SRC=$ROOT/build/deps/BlackHole; OUT=$ROOT/build/BlackHole2ch.driver
[[ -d $SRC ]] || git clone --depth 1 https://github.com/ExistentialAudio/BlackHole.git $SRC; NAME=BlackHole2ch; VER=$(cat $SRC/VERSION)
rm -rf $OUT; mkdir -p $OUT/Contents/MacOS $OUT/Contents/Resources
sed -e "s/\${EXECUTABLE_NAME}/$NAME/g" -e "s/\$(PRODUCT_BUNDLE_IDENTIFIER)/audio.existential.$NAME/g" \
    -e "s/\${PRODUCT_NAME}/$NAME/g" -e "s/\$(MARKETING_VERSION)/$VER/g" $SRC/BlackHole/BlackHole.plist > $OUT/Contents/Info.plist
grep -q '\$[({]' $OUT/Contents/Info.plist && { grep '\$[({]' $OUT/Contents/Info.plist; exit 1; }
cp $SRC/BlackHole/BlackHole.icns $OUT/Contents/Resources/
clang -arch x86_64 -O2 -bundle -mmacosx-version-min=11.0 -DkNumber_Of_Channels=2 \
  -framework CoreAudio -framework CoreFoundation -framework Accelerate \
  $SRC/BlackHole/BlackHole.c -o $OUT/Contents/MacOS/$NAME
codesign -f -s - $OUT
echo "built $OUT ($VER)"
