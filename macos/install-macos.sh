#!/bin/zsh
# Installs the Iris Xe acceleration stack on a clean macOS Tahoe 26.7.1 (25G241). Run with sudo, then approve the
# extension in System Settings > Privacy & Security if asked, and reboot.
#   sudo zsh macos/install-macos.sh <sle/ folder> <le/ folder>
#   <le/>  : AppleIntelTGLGraphics.kext + AppleIntelTGLGraphicsFramebuffer.kext with com.xxxxx bundle IDs
#   <sle/> : the 16.0.32 GL / VA / VAME / Shared / MTL bundles
# Both folders come from the NootedGreen project (sle_Internal/le and sle_Internal/sle); they are Apple binaries
# and are not redistributed here. Build macos/out first with macos/build-all.sh and build/HookCase.kext with
# build-hookcase.sh (both as your user, no sudo).
set -e
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }
SLE=${1:?sle/}; LE=${2:?le/}
HERE=${0:A:h}; OUT=$HERE/out
[[ -d $OUT/xe.bundle && -d $OUT/AppleIntelTGLGraphicsMTLDriver.bundle ]] || { echo "run macos/build-all.sh first"; exit 1; }
put() { rm -rf "$2"; mkdir -p "$(dirname "$2")"; ditto "$1" "$2"; chown -R root:wheel "$2"; echo "  $2"; }

echo "== kexts and bundles in /Library/Extensions"
put $LE/AppleIntelTGLGraphics.kext /Library/Extensions/AppleIntelTGLGraphics.kext
put $LE/AppleIntelTGLGraphicsFramebuffer.kext /Library/Extensions/AppleIntelTGLGraphicsFramebuffer.kext
K=/Library/Extensions/AppleIntelTGLGraphics.kext/Contents/Info.plist
# the accelerator personality matches the spoofed device-id (9A49) and needs a version above the stock one
plutil -replace IOKitPersonalities.Gen7.IOPCIPrimaryMatch -string 0x9a498086 $K
plutil -replace CFBundleVersion -string 16.0.2 $K
for b in AppleIntelTGLGraphicsMTLDriver AppleIntelTGLGraphicsGLDriver AppleIntelTGLGraphicsVADriver AppleIntelTGLGraphicsVAME AppleIntelGraphicsShared; do
  put $SLE/$b.bundle /Library/Extensions/$b.bundle
done
# the TGL kexts link against org.smichaud.HookCase: build it with build-hookcase.sh (or take it from <le/>)
HC=$HERE/../build/HookCase.kext; [[ -d $LE/HookCase.kext ]] && HC=$LE/HookCase.kext
[[ -d $HC ]] || { echo "HookCase.kext missing: run 'zsh build-hookcase.sh' (as your user) first"; exit 1; }
put $HC /Library/Extensions/HookCase.kext

echo "== Metal driver (MTL17) and shared bundle in /Library/GPUBundles"
put $OUT/AppleIntelTGLGraphicsMTLDriver.bundle /Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle
put $SLE/AppleIntelGraphicsShared.bundle /Library/GPUBundles/AppleIntelGraphicsShared.bundle

echo "== shader compiler in /System/Library/Caches/xe.bundle (writable firmlink inside /System)"
put $OUT/xe.bundle /System/Library/Caches/xe.bundle

echo "== registering kexts in the Auxiliary Kernel Collection"
kexts=(-p /Library/Extensions/AppleIntelTGLGraphics.kext -p /Library/Extensions/AppleIntelTGLGraphicsFramebuffer.kext)
kexts+=(-p /Library/Extensions/HookCase.kext)
/usr/bin/kmutil load -z --load-style match-only $kexts 2>&1 || true
echo "done: approve the extension if asked, then reboot"
