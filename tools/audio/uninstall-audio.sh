#!/bin/zsh
# Undo tools/audio/install-audio.sh.
#   zsh tools/audio/uninstall-audio.sh hal    remove XeSpeakers.driver (sudo) and restart coreaudiod. The kext tears the audio
#                                             path down (amps muted + PS3, DMA off, DSP off) as soon as the connection closes.
#   zsh tools/audio/uninstall-audio.sh kext   disable the AudShim entry in the EFI config.plist and remove the kext (EFI mounted
#                                             at $EFI, default /Volumes/EFI), then reboot. Remove the HAL driver first.
#   Emergency: boot with -audshimoff (AudShim does not attach; the HAL driver then publishes no device).
set -e
EFI=${EFI:-/Volumes/EFI}
case "$1" in
hal)
    sudo rm -rf /Library/Audio/Plug-Ins/HAL/XeSpeakers.driver
    sudo killall coreaudiod || true
    echo "XeSpeakers.driver removed, coreaudiod restarted."
    ;;
kext)
    K=$EFI/EFI/OC/Kexts; C=$EFI/EFI/OC/config.plist
    [[ -d $K && -f $C ]] || { echo "EFI not mounted (expected $K)"; exit 1; }
    [[ -d /Library/Audio/Plug-Ins/HAL/XeSpeakers.driver ]] && echo "note: XeSpeakers.driver is still installed (it publishes no device without the kext)"
    N=$(plutil -extract Kernel.Add raw $C)
    for ((i = 0; i < N; i++)); do
        if [[ "$(plutil -extract Kernel.Add.$i.BundlePath raw $C 2>/dev/null)" == AudShim.kext ]]; then
            cp $C $C.before-audshim-removal
            plutil -replace Kernel.Add.$i.Enabled -bool NO $C
            echo "AudShim entry disabled in $C (backup: $C.before-audshim-removal)"
        fi
    done
    rm -rf $K/AudShim.kext
    echo "AudShim.kext removed from $K. Reboot."
    ;;
*) echo "usage: zsh tools/audio/uninstall-audio.sh hal | kext"; exit 2 ;;
esac
