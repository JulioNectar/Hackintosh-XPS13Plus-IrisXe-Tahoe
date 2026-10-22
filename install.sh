#!/bin/zsh
# One-command setup of the Iris Xe acceleration stack on macOS Tahoe 26.7.1 (25G241).
#
#   zsh install.sh            check, build and install (asks for your password once, for the install step)
#   zsh install.sh --check    only run the checks
#   zsh install.sh --build-only   checks + download + build, no install
#   zsh install.sh --uninstall
#
# Before: boot this Mac through the repository's OpenCore EFI (see make-efi.sh / docs/INSTALL.md).
# What it does:
#   1. checks the machine (Intel, macOS build, Command Line Tools, GPU spoof, NootedGreen loaded)
#   2. downloads NootedGreen at the pinned commit, which provides Apple's 16.0.32 Tiger Lake driver bundles
#   3. builds NootedGreen.kext, HookCase.kext (the TGL kexts depend on it), the patched Metal driver, libxedrv and
#      the shader compiler
#   4. installs them (sudo) and tells you what to copy to the EFI
set -e; setopt pipe_fail   # a failed build is not hidden by "| tail -1"
ROOT=${0:A:h}
BUILD_REQ=25G241
NG_REPO=https://github.com/sgiammori/NootedGreen.git
NG_COMMIT=95f81898be65cb9f7b3d370ca78054be4cc8a62d
NG=$ROOT/build/deps/NootedGreen
ok()   { print -P "  %F{green}✓%f $1"; }
warn() { print -P "  %F{yellow}!%f $1"; }
fail() { print -P "  %F{red}✗%f $1"; exit 1; }

if [[ "$1" == "--uninstall" ]]; then
  echo "== removing the acceleration stack (asks for your password)"
  sudo zsh -c '
    rm -rf /Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle /Library/GPUBundles/AppleIntelGraphicsShared.bundle
    rm -rf /System/Library/Caches/xe.bundle
    for k in AppleIntelTGLGraphics.kext AppleIntelTGLGraphicsFramebuffer.kext AppleIntelTGLGraphicsMTLDriver.bundle \
             AppleIntelTGLGraphicsGLDriver.bundle AppleIntelTGLGraphicsVADriver.bundle AppleIntelTGLGraphicsVAME.bundle \
             AppleIntelGraphicsShared.bundle HookCase.kext; do rm -rf /Library/Extensions/$k; done
    kmutil load -z --load-style match-only 2>/dev/null || true'
  echo "done. Also disable NootedGreen.kext in your config.plist (or boot config-install.plist), then reboot."
  exit 0
fi

echo "== 1. checks"
[[ $(uname -m) == x86_64 ]] && ok "Intel Mac (x86_64)" || fail "this only works on Intel (x86_64)"
BUILD=$(sw_vers -buildVersion)
if [[ $BUILD == $BUILD_REQ ]]; then ok "macOS $(sw_vers -productVersion) ($BUILD)"
else warn "macOS build is $BUILD, the patches were made for $BUILD_REQ (Tahoe 26.7.1)."
     [[ "$XE_FORCE" == 1 ]] || fail "refusing to continue (set XE_FORCE=1 to try anyway, at your own risk)"; fi
xcode-select -p >/dev/null 2>&1 && ok "Command Line Tools" || fail "install the Command Line Tools first: xcode-select --install"
for t in git python3 clang codesign; do command -v $t >/dev/null || fail "missing tool: $t"; done
if ioreg -r -c IOPCIDevice -n IGPU -d 1 2>/dev/null | grep -q '"device-id" = <499a0000>'; then ok "GPU presented as Tiger Lake (device-id 9A49)"
else warn "IGPU device-id is not 9A49: is this Mac booting through the repository's EFI?"; fi
if kextstat 2>/dev/null | grep -q NootedGreen; then ok "NootedGreen loaded"
else warn "NootedGreen is not loaded (fine for a first install; the EFI must load it for acceleration)"; fi
[[ "$1" == "--check" ]] && exit 0

echo "== 2. NootedGreen $NG_COMMIT (Apple's 16.0.32 driver bundles)"
if [[ ! -d $NG/.git ]]; then mkdir -p ${NG:h}; git clone -q $NG_REPO $NG; fi
git -C $NG fetch -q origin $NG_COMMIT 2>/dev/null || true
git -C $NG checkout -q $NG_COMMIT
SLE=$NG/sle_Internal/sle; LE=$NG/sle_Internal/le
[[ -d $SLE/AppleIntelTGLGraphicsMTLDriver.bundle && -d $LE/AppleIntelTGLGraphics.kext ]] && ok "driver bundles found" || fail "sle_Internal not found in NootedGreen"

echo "== 3. build"
zsh $ROOT/build-nootedgreen.sh | tail -1
zsh $ROOT/build-hookcase.sh | tail -1
zsh $ROOT/macos/build-all.sh $SLE | tail -1
ok "built build/NootedGreen.kext, build/HookCase.kext and macos/out/"

[[ "$1" == "--build-only" ]] && { echo "build only: nothing installed"; exit 0; }
echo "== 4. install (your password is needed once)"
sudo zsh $ROOT/macos/install-macos.sh $SLE $LE

cat <<EOF

All set. Before rebooting:
  - Copy  $ROOT/build/NootedGreen.kext  into  EFI/OC/Kexts/  (replace the old one), on the EFI you boot from.
    The internal ESP can be mounted with: sudo diskutil mount disk0s1   (check the disk with: diskutil list)
  - Use the accelerated config.plist (with the boot-args from the repository).
  - If macOS asks to allow a system extension, approve it in System Settings > Privacy & Security.
Then reboot. Check with: system_profiler SPDisplaysDataType  (look for "Metal Support")
EOF
