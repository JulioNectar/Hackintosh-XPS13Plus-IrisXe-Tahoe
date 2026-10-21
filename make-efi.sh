#!/bin/zsh
# Builds a ready-to-use OpenCore EFI for the Dell XPS 13 Plus 9320 (i7-1360P) with YOUR OWN serial numbers.
#
#   zsh make-efi.sh                 -> dist/EFI and dist/EFI-XPS13Plus-9320.zip
#
# Every machine needs unique SMBIOS data (serial, MLB, UUID, ROM): sharing one set between machines breaks iCloud /
# iMessage and can get Apple IDs flagged. This script downloads Apple-compatible serial tooling (macserial, from the
# official OpenCore release that matches the EFI) and fills both config files:
#   EFI/OC/config.plist          accelerated (NootedGreen on)
#   EFI/OC/config-install.plist  for installing macOS (NootedGreen off, verbose boot)
# Works on macOS (uses the macOS macserial binary). On Linux, run macserial.linux by hand and use plutil-like edits.
set -e
ROOT=${0:A:h}
OC_VER=1.0.8
DEPS=$ROOT/build/deps; DIST=$ROOT/dist
mkdir -p $DEPS $DIST
MS=$DEPS/OpenCore-$OC_VER/Utilities/macserial/macserial
if [[ ! -x $MS ]]; then
  echo "== baixando OpenCore $OC_VER (macserial)"
  curl -fL -o $DEPS/oc.zip https://github.com/acidanthera/OpenCorePkg/releases/download/$OC_VER/OpenCore-$OC_VER-RELEASE.zip
  mkdir -p $DEPS/OpenCore-$OC_VER && (cd $DEPS/OpenCore-$OC_VER && unzip -q -o ../oc.zip 'Utilities/macserial/*')
  chmod +x $MS
fi

rm -rf $DIST/EFI; ditto $ROOT/EFI $DIST/EFI
line=$($MS -m MacBookPro16,2 -g -n 1)
SERIAL=${${line%% |*}// /}; MLB=${${line##*| }// /}
UUID=$(uuidgen)
ROM=$(openssl rand -base64 6)          # 6 random bytes (plist <data> is base64)
for c in $DIST/EFI/OC/config.plist $DIST/EFI/OC/config-install.plist; do
  plutil -replace PlatformInfo.Generic.SystemProductName -string MacBookPro16,2 $c
  plutil -replace PlatformInfo.Generic.SystemSerialNumber -string $SERIAL $c
  plutil -replace PlatformInfo.Generic.MLB -string $MLB $c
  plutil -replace PlatformInfo.Generic.SystemUUID -string $UUID $c
  plutil -replace PlatformInfo.Generic.ROM -data $ROM $c
  plutil -lint $c >/dev/null
done
rm -f $DIST/EFI-XPS13Plus-9320.zip
(cd $DIST && ditto -c -k --keepParent EFI EFI-XPS13Plus-9320.zip)
cat <<EOF

EFI pronta em: $DIST/EFI   (zip: $DIST/EFI-XPS13Plus-9320.zip)
  Serial: $SERIAL   MLB: $MLB   UUID: $UUID
  Guarde esses dados: são os da SUA máquina. Não publique a pasta dist/.

Próximos passos:
  1. Copie dist/EFI para a partição EFI de um pendrive (FAT32).
  2. Para INSTALAR o macOS: renomeie EFI/OC/config-install.plist para config.plist (guarde o original).
  3. Depois de instalar: volte o config.plist acelerado e rode  zsh install.sh
EOF
