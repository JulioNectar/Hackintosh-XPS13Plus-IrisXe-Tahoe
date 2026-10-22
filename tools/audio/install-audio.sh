#!/bin/zsh
# Internal speakers (AudShim kext + XeSpeakers HAL driver). Run the stages in this order:
#
#   zsh tools/audio/install-audio.sh build   build audctl, AudShim.kext and XeSpeakers.driver into tools/build/
#                                            (needs tools/AudShim/AudShimData.h: see docs/INSTALL.md, "Internal speakers")
#   zsh tools/audio/install-audio.sh kext    copy AudShim.kext to the EFI and add its Kernel>Add entry if missing
#                                            (EFI mounted at $EFI, default /Volumes/EFI; config.plist is backed up). REBOOT,
#                                            then check: tools/build/audctl info (interface v9) and tools/build/audctl drv test / audctl rec
#   zsh tools/audio/install-audio.sh hal     install XeSpeakers.driver in /Library/Audio/Plug-Ins/HAL (sudo) and restart
#                                            coreaudiod; then pick the speakers in System Settings > Sound > Output
#
# Undo: zsh tools/audio/uninstall-audio.sh hal | kext. Emergency: boot with -audshimoff.
set -e
TOOLS=${0:A:h:h}
B=$TOOLS/build
EFI=${EFI:-/Volumes/EFI}
case "$1" in
build)
    mkdir -p $B
    [[ -f $TOOLS/AudShim/AudShimData.h ]] || {
        clang -O2 -Wall -Wextra -o $B/audctl $TOOLS/audctl.c -framework IOKit -framework CoreFoundation 2>/dev/null || true
        echo "missing tools/AudShim/AudShimData.h. Generate it from Intel's sof-bin files first:"
        echo "  tools/build/audctl gen-header tools/AudShim/AudShimData.h --fw=PATH/sof-rpl.ri --tplg=PATH/sof-rpl-rt1316-l12-rt714-l0.tplg"
        exit 1; }
    clang -O2 -Wall -Wextra -o $B/audctl $TOOLS/audctl.c -framework IOKit -framework CoreFoundation
    zsh $TOOLS/build-kext.sh AudShim
    zsh $TOOLS/XeSpeakers/build.sh
    echo "built: $B/audctl  $B/AudShim.kext  $B/XeSpeakers.driver"
    ;;
kext)
    K=$EFI/EFI/OC/Kexts; C=$EFI/EFI/OC/config.plist
    [[ -d $K && -f $C ]] || { echo "EFI not mounted (expected $K). Mount it first (e.g. sudo diskutil mount disk0s1) or set EFI=..."; exit 1; }
    [[ -d $B/AudShim.kext ]] || { echo "missing $B/AudShim.kext: run the build stage first"; exit 1; }
    codesign -v $B/AudShim.kext || { echo "AudShim.kext signature check failed"; exit 1; }
    if [[ -d $K/AudShim.kext ]]; then
        V=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" $K/AudShim.kext/Contents/Info.plist 2>/dev/null || echo unknown)
        mkdir -p $B/backup && cp -R $K/AudShim.kext $B/backup/AudShim.kext.$V.$(date +%Y%m%d-%H%M%S)
        echo "previous AudShim.kext ($V) saved in $B/backup/"
    fi
    rm -rf $K/AudShim.kext && cp -R $B/AudShim.kext $K/
    if ! plutil -extract Kernel.Add xml1 -o - $C | grep -q '<string>AudShim.kext</string>'; then
        cp $C $C.before-audshim
        N=$(plutil -extract Kernel.Add raw $C)     # number of entries: append at the end
        plutil -insert Kernel.Add.$N -dictionary $C
        plutil -insert Kernel.Add.$N.Arch -string x86_64 $C
        plutil -insert Kernel.Add.$N.BundlePath -string AudShim.kext $C
        plutil -insert Kernel.Add.$N.Comment -string "internal speakers (SOF DSP, SoundWire, RT1316)" $C
        plutil -insert Kernel.Add.$N.Enabled -bool YES $C
        plutil -insert Kernel.Add.$N.ExecutablePath -string Contents/MacOS/AudShim $C
        plutil -insert Kernel.Add.$N.MaxKernel -string "" $C
        plutil -insert Kernel.Add.$N.MinKernel -string "" $C
        plutil -insert Kernel.Add.$N.PlistPath -string Contents/Info.plist $C
        plutil -lint $C
        echo "Kernel>Add entry added (backup: $C.before-audshim)"
    fi
    echo "AudShim copied to $K. Reboot, then: $B/audctl info  (interface v8)  and  $B/audctl drv test  (3 short tones)"
    ;;
hal)
    [[ -d $B/XeSpeakers.driver ]] || { echo "missing $B/XeSpeakers.driver: run the build stage first"; exit 1; }
    I=$(ioreg -r -c AudShim -d 1 2>/dev/null | awk -F'= ' '/"AudShimInterface"/ {print $2}')
    [[ "$I" == "8" || "$I" == "9" ]] || { echo "the loaded AudShim is not interface v8/v9 (got '${I:-none}'): install the kext and reboot first"; exit 1; }
    codesign -v $B/XeSpeakers.driver || { echo "XeSpeakers.driver signature check failed"; exit 1; }
    sudo rm -rf /Library/Audio/Plug-Ins/HAL/XeSpeakers.driver
    sudo cp -R $B/XeSpeakers.driver /Library/Audio/Plug-Ins/HAL/
    sudo chown -R root:wheel /Library/Audio/Plug-Ins/HAL/XeSpeakers.driver
    sudo killall coreaudiod || true
    echo "XeSpeakers.driver installed; coreaudiod restarts by itself. After ~3 s choose the internal speakers in"
    echo "System Settings > Sound > Output (the volume starts at 30 %)."
    echo "logs: log show --last 2m --predicate 'subsystem == \"local.xe.XeSpeakers\" OR eventMessage CONTAINS \"AudShim DRV\"'"
    ;;
*) echo "usage: zsh tools/audio/install-audio.sh build | kext | hal"; exit 2 ;;
esac
