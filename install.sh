#!/bin/zsh
# One-command setup (and update) of the Iris Xe acceleration stack on macOS Tahoe 26.7.1 (25G241).
#
#   zsh install.sh                check, build and install (asks for your password once, for the install step)
#   zsh install.sh --check        only run the checks
#   zsh install.sh --build-only   checks + download + build, no install
#   zsh install.sh --uninstall
#
# First install: boot this Mac through the repository's OpenCore EFI first (see make-efi.sh / docs/INSTALL.md).
# Update: git pull, run this again, copy the new build/NootedGreen.kext to your EFI. Never re-run make-efi.sh to
# update (it generates new serial numbers).
# What it does:
#   1. checks the machine (Intel, macOS build, Command Line Tools, GPU spoof, NootedGreen loaded)
#   2. downloads NootedGreen at the pinned commit, which provides Apple's 16.0.32 Tiger Lake driver bundles
#   3. builds NootedGreen.kext, HookCase.kext (the TGL kexts depend on it), the patched Metal driver, libxedrv and
#      the shader compiler (full output in build/install.log)
#   4. installs them (sudo) and tells you what to copy to the EFI
set -e; setopt pipe_fail
ROOT=${0:A:h}
BUILD_REQ=25G241
NG_REPO=https://github.com/sgiammori/NootedGreen.git
NG_COMMIT=95f81898be65cb9f7b3d370ca78054be4cc8a62d
NG=$ROOT/build/deps/NootedGreen
LOG=$ROOT/build/install.log

ok()    { print -P "    %F{green}✓%f $1"; }
warn()  { print -P "    %F{yellow}!%f $1"; }
fail()  { print -P "    %F{red}✗%f $1"; exit 1; }
info()  { print -P "    %F{8}$1%f"; }
step()  { print; print -P "%F{blue}%B[$1/4]%b%f %B$2%b"; }
rule()  { print -P "%F{8}  ──────────────────────────────────────────────────────────────%f"; }
# run a build step quietly; on failure show the end of its output
quiet() { local label=$1; shift; local t0=$SECONDS
  print -n -P "    %F{8}…%f $label"
  if "$@" >>$LOG 2>&1; then print -P "\r    %F{green}✓%f $label %F{8}($((SECONDS - t0)) s)%f"
  else print -P "\r    %F{red}✗%f $label"; print; tail -25 $LOG | sed 's/^/      /'; print
       print -P "    Full log: %B$LOG%b"; exit 1; fi; }

banner() {
  local rev=$(git -C $ROOT log -1 --format='%h · %cd' --date=format:'%Y-%m-%d' 2>/dev/null || echo "?")
  print
  print -P "  %F{blue}%B╭────────────────────────────────────────────────────────────╮%b%f"
  print -P "  %F{blue}%B│%b%f  %BIris Xe acceleration · macOS Tahoe 26.7.1%b                 %F{blue}%B│%b%f"
  print -P "  %F{blue}%B│%b%f  %F{8}XPS13Plus-Tahoe-IrisXe  ·  $rev%f$(printf '%*s' $((31 - ${#rev})) '')%F{blue}%B│%b%f"
  print -P "  %F{blue}%B╰────────────────────────────────────────────────────────────╯%b%f"
}

if [[ "$1" == "--uninstall" ]]; then
  banner
  step 1 "Removing the acceleration stack (asks for your password)"
  sudo zsh -c '
    rm -rf /Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle /Library/GPUBundles/AppleIntelGraphicsShared.bundle
    rm -rf /System/Library/Caches/xe.bundle
    for k in AppleIntelTGLGraphics.kext AppleIntelTGLGraphicsFramebuffer.kext AppleIntelTGLGraphicsMTLDriver.bundle \
             AppleIntelTGLGraphicsGLDriver.bundle AppleIntelTGLGraphicsVADriver.bundle AppleIntelTGLGraphicsVAME.bundle \
             AppleIntelGraphicsShared.bundle HookCase.kext; do rm -rf /Library/Extensions/$k; done
    kmutil load -z --load-style match-only 2>/dev/null || true'
  ok "removed"
  print; print -P "  Also disable %BNootedGreen.kext%b in your config.plist (or boot config-install.plist), then reboot."; print
  exit 0
fi

banner
mkdir -p $ROOT/build; : >$LOG

# An update is a machine that already runs the stack: TGL kexts installed and NootedGreen loaded.
UPDATE=0
# (no "| grep -q" with pipe_fail: grep exits early, the writer gets SIGPIPE and the test reads as false)
NG_LOADED=0; [[ $(kmutil showloaded 2>/dev/null) == *NootedGreen* ]] && NG_LOADED=1
if [[ -d /Library/Extensions/AppleIntelTGLGraphics.kext ]] && (( NG_LOADED )); then UPDATE=1; fi
if (( UPDATE )); then print -P "  %F{cyan}Mode:%f %Bupdate%b %F{8}(the stack is already installed and running)%f"
else print -P "  %F{cyan}Mode:%f %Bfirst install%b"; fi

step 1 "Checks"
[[ $(uname -m) == x86_64 ]] && ok "Intel Mac (x86_64)" || fail "this only works on Intel (x86_64)"
BUILD=$(sw_vers -buildVersion)
if [[ $BUILD == $BUILD_REQ ]]; then ok "macOS $(sw_vers -productVersion) ($BUILD)"
else warn "macOS build is $BUILD, the patches were made for $BUILD_REQ (Tahoe 26.7.1)."
     [[ "$XE_FORCE" == 1 ]] || fail "refusing to continue (set XE_FORCE=1 to try anyway, at your own risk)"; fi
xcode-select -p >/dev/null 2>&1 && ok "Command Line Tools" || fail "install the Command Line Tools first: xcode-select --install"
for t in git python3 clang codesign; do command -v $t >/dev/null || fail "missing tool: $t"; done
# FAT/exFAT (USB sticks) keep macOS metadata in "._" files, which break the bundle copies and codesign
FS=$(diskutil info "$(df -P "$ROOT" | awk 'NR==2{print $1}')" 2>/dev/null | awk -F': *' '/Type \(Bundle\)/{print $2}')
[[ $FS == (apfs|hfs) ]] && ok "repository on an $FS volume" \
  || fail "the repository is on a '$FS' volume: clone it to an APFS disk (e.g. your home folder) and run it from there"
if [[ $(ioreg -r -c IOPCIDevice -n IGPU -d 1 2>/dev/null) == *'"device-id" = <499a0000>'* ]]; then ok "GPU presented as Tiger Lake (device-id 9A49)"
else warn "IGPU device-id is not 9A49: is this Mac booting through the repository's EFI? Without the spoof the framebuffer kext is left out of the Auxiliary Kernel Collection (black screen); boot the spoofed EFI and run this again"; fi
if (( NG_LOADED )); then ok "NootedGreen loaded"
else warn "NootedGreen is not loaded (fine for a first install; the EFI must load it for acceleration)"; fi

if (( UPDATE )) && [[ -f $ROOT/CHANGELOG.md ]]; then
  print; print -P "  %F{cyan}Latest changes%f %F{8}(CHANGELOG.md)%f"
  grep -E '^\| 20[0-9]{2}-' $ROOT/CHANGELOG.md | head -4 | while IFS='|' read -r _ d c s _; do
    t=$(print -r -- "$c" | grep -oE '\*\*[^*]+\*\*' | head -1 | tr -d '*'); [[ -z $t ]] && t=$(print -r -- "${c## }" | cut -c1-60)
    s=$(print -r -- "$s" | sed "s/^ *//;s/ *$//"); (( ${#s} > 28 )) && s="${s:0:28}…"
    print -P "    %F{8}${d// /}%f  $t %F{8}($s)%f"
  done
fi
[[ "$1" == "--check" ]] && { print; exit 0; }

step 2 "Apple's 16.0.32 driver bundles (NootedGreen ${NG_COMMIT:0:7})"
if [[ ! -d $NG/.git ]]; then mkdir -p ${NG:h}; quiet "download NootedGreen" git clone -q $NG_REPO $NG
else ok "NootedGreen already downloaded"; fi
git -C $NG fetch -q origin $NG_COMMIT >>$LOG 2>&1 || true
quiet "checkout ${NG_COMMIT:0:7}" git -C $NG checkout -q $NG_COMMIT
SLE=$NG/sle_Internal/sle; LE=$NG/sle_Internal/le
[[ -d $SLE/AppleIntelTGLGraphicsMTLDriver.bundle && -d $LE/AppleIntelTGLGraphics.kext ]] && ok "driver bundles found" || fail "sle_Internal not found in NootedGreen"

step 3 "Build"
info "full output: build/install.log"
quiet "NootedGreen.kext" zsh $ROOT/build-nootedgreen.sh
quiet "HookCase.kext" zsh $ROOT/build-hookcase.sh
quiet "Metal driver, libxedrv, shader compiler" zsh $ROOT/macos/build-all.sh $SLE
NGMD5=$(md5 -q $ROOT/build/NootedGreen.kext/Contents/MacOS/NootedGreen)
info "NootedGreen.kext md5 $NGMD5"

if [[ "$1" == "--build-only" ]]; then print; print -P "  %F{green}%BBuilt.%b%f Nothing was installed (--build-only)."; print; exit 0; fi

step 4 "Install (your password is needed once)"
sudo -v
quiet "kexts and bundles in /Library" sudo zsh $ROOT/macos/install-macos.sh $SLE $LE

KEXT=$ROOT/build/NootedGreen.kext
print; rule
if (( UPDATE )); then
  print -P "  %F{green}%BUpdated.%b%f One manual step left: put the new NootedGreen.kext on the EFI you boot from."
  print
  print -P "  %B1.%b Mount the EFI (internal disk; check with %Bdiskutil list%b if you boot from another one):"
  print -P "       %F{cyan}sudo diskutil mount disk0s1%f"
  print -P "  %B2.%b Keep a copy of the old kext, then replace it:"
  print -P "       %F{cyan}cp -R /Volumes/EFI/EFI/OC/Kexts/NootedGreen.kext ~/NootedGreen.kext.old%f"
  print -P "       %F{cyan}sudo rm -rf /Volumes/EFI/EFI/OC/Kexts/NootedGreen.kext && sudo cp -R $KEXT /Volumes/EFI/EFI/OC/Kexts/%f"
  print -P "  %B3.%b Reboot, then check: %F{cyan}kmutil showloaded | grep NootedGreen%f"
  print
  print -P "  %F{8}Your config.plist and serial numbers stay as they are. Don't re-run make-efi.sh to update.%f"
  print -P "  %F{8}If the new kext doesn't boot: boot a backup EFI and put ~/NootedGreen.kext.old back.%f"
else
  print -P "  %F{green}%BInstalled.%b%f Before rebooting:"
  print
  print -P "  %B1.%b Copy %B$KEXT%b into %BEFI/OC/Kexts/%b on the EFI you boot from"
  print -P "     %F{8}(internal ESP: sudo diskutil mount disk0s1; check the disk with diskutil list)%f"
  print -P "  %B2.%b Use the accelerated config.plist, with the boot-args from the repository."
  print -P "  %B3.%b If macOS asks to allow a system extension: System Settings › Privacy & Security."
  print -P "  %B4.%b Reboot, then check: %F{cyan}system_profiler SPDisplaysDataType | grep Metal%f"
fi
rule; print
