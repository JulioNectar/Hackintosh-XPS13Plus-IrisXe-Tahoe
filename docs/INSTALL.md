# Installation guide

> **Same laptop (XPS 13 Plus 9320)?** The short path is `zsh make-efi.sh` (EFI with your own serials) and, after
> installing macOS, `zsh install.sh`. See the README. This guide is the manual procedure behind those scripts.

Target: Dell XPS 13 Plus 9320, i7-1360P, Iris Xe 8086:A7A0, **macOS Tahoe 26.7.1 (25G241)**. Other Raptor Lake /
Alder Lake laptops may work with their own EFI, but the patches were only tested here.

## 0. What you need

- A USB drive (16 GB+) for OpenCore and the installer, or a second disk.
- Another Mac (or the laptop itself, from Windows/Linux) to create the installer.
- The `sle_Internal/le` and `sle_Internal/sle` folders of [NootedGreen](https://github.com/sgiammori/NootedGreen)
  (Apple's 16.0.32 Tiger Lake driver bundles).

## 1. BIOS

Use the usual hackintosh settings (storage in AHCI/NVMe mode, not RAID/VMD; Secure Boot off). The E-cores can stay
**enabled**: the shipped `Cpuid1Data` includes the 4P+8E fix (see STATUS). Change one setting at a time and keep a way to boot back.

## 2. EFI

1. Copy `EFI/` from this repository.
2. Generate an SMBIOS for **MacBookPro16,2** (e.g. with GenSMBIOS) and fill `PlatformInfo > Generic`
   (SystemSerialNumber, MLB, SystemUUID, ROM) in **both** `config.plist` and `config-install.plist`.
3. `HfsPlus.efi` is not shipped (Apple binary): add it from acidanthera/OcBinaryData if you boot HFS+ installer media.
4. USB ports are mapped for this laptop (USBToolBox + `UTBDefault.kext`), and the trackpad/touchscreen use `SSDT-XPS-I2C` (source in `acpi/`) with the patched
   `VoodooGPIO` inside `VoodooI2C.kext` (rebuild it with `zsh build-voodoogpio.sh`), so they run on interrupts, not polling.

## 3. Install macOS

1. Make a Tahoe 26.7.1 installer: `softwareupdate --fetch-full-installer --full-installer-version 26.7.1`, or
   download `InstallAssistant.pkg` with gibMacOS, then use `createinstallmedia`.
2. Boot OpenCore with **`config-install.plist` renamed to `config.plist`**. It disables NootedGreen and boots verbose.
   The installer runs on the firmware framebuffer, without acceleration.
3. Install, finish Setup Assistant, then install the Command Line Tools: `xcode-select --install`.

## 4. Kernel side

```sh
zsh build-nootedgreen.sh                 # -> build/NootedGreen.kext
```

Put `build/NootedGreen.kext` in `EFI/OC/Kexts/` and switch back to the accelerated `config.plist` (keep the boot-args
from the repository).

## 5. User-space side

```sh
zsh macos/build-all.sh /path/to/sle_Internal/sle
sudo zsh macos/install-macos.sh /path/to/sle_Internal/sle /path/to/sle_Internal/le
```

Approve the extension in **System Settings › Privacy & Security** if asked, and reboot.

## 6. After the first accelerated boot

- Resolution / UI scale: `tools/xe-res` (list) and `tools/xe-res w1680` (HiDPI 1680 pt wide). Avoid
  **System Settings › Displays**.
- Brightness: build `tools/XeShim` (`zsh tools/build-kext.sh XeShim`), load it, then `xe-brilho 70`.
- Touchscreen trail (optional): `zsh tools/build-xe-toque.sh`, then `open tools/xe-toque.app` and allow it in
  Privacy & Security › Input Monitoring. Only touches on the screen draw; the trackpad is ignored.
- Check: `system_profiler SPDisplaysDataType` shows **Metal Support** and the chipset model you set.
- Internal speakers, Wi-Fi, lid/sleep setup and touchpad vibration: see the sections below.

## 7. Moving OpenCore to the internal disk

1. `sudo diskutil mount disk0s1` (check the identifier with `diskutil list`) and copy `EFI/`.
2. In the Dell BIOS (F2) › Boot Configuration › Boot Sequence: **Add Boot Option** → the internal disk →
   `\EFI\BOOT\BOOTx64.efi`, and move it to the top.

## Recovery

- Boot with the USB OpenCore and `config-install.plist` (NootedGreen off) to reach the desktop without acceleration.
- Each patch can be turned off with its boot-arg ([KERNEL-PATCHES.md](KERNEL-PATCHES.md)).
- Remove the user-space pieces: delete `/Library/GPUBundles/AppleIntelTGL*`, `/System/Library/Caches/xe.bundle`
  and the TGL kexts in `/Library/Extensions`, then run `sudo kmutil load -z` again.

## Touchscreen extras (xe-toque)

`zsh tools/build-xe-toque.sh`, copy `tools/xe-toque.app` wherever you keep it (e.g. `~/xe/toque/`), open it and add it
to Login Items. Allow it in Privacy & Security › **Input Monitoring** and › **Accessibility** (Screen Recording only
for the magnifier). The build signs it with a requirement on its bundle id, so later rebuilds keep the grants.
A hand icon in the menu bar holds the toggles (saved in `defaults read local.xe.toque`):

- **One-finger scroll**: drag scrolls with momentum, tap clicks (double-tap works), hold 0.5 s = right click,
  hold then drag = left drag/selection. VoodooI2C's own pointer events for the touch are filtered out (CGEvent field 87 =
  sender registry ID of the touchscreen driver); the trackpad and mouse are never filtered.
- **Window buttons magnet** (on by default; needs One-finger scroll, since it acts on xe-toque's own taps): a tap within
  22 pt of a window's close / minimize / zoom button, inside the window's top 52 pt, presses the nearest of those buttons
  (Accessibility AXPress, falling back to a click at its centre) and flashes a blue ring on it. Not applied to taps that
  land on another control (toolbar button, tab, text field…), to full-screen windows, or to the trackpad/mouse. Each
  redirect is logged (`magnet: tap … -> close of …`). Check a point without clicking:
  `xe-toque.app/Contents/MacOS/xe-toque --test-magnet X Y` (global points, top-left origin).
- **Edge swipes**: in from the right edge = Notification Center (presses the menu-bar clock via Accessibility),
  up from the bottom edge = Mission Control.
- Two-, three- and four-finger gestures on the screen already reach macOS as trackpad gestures (VoodooI2C feeds them to an
  emulated Magic Trackpad), so they follow System Settings › Trackpad. With **xe-toque handles 2–5 fingers** on,
  xe-toque replaces them: 2 = scroll, 3 sideways = switch Space (ctrl+←/→), 5-finger pinch = Apps.
- **Trail** / **Ripple** visual feedback, **Magnifier** (two-finger hold), **Pen mode** (draw on screen; Esc exits,
  *Clear drawing* wipes it), **Touch mode** (switches to a 1152-pt-wide HiDPI mode with `~/xe/build/xe-res` and back
  to the previous mode).
- Log: `~/xe/build/toque/xe-toque.log`. Diagnostic: `xe-toque.app/Contents/MacOS/xe-toque --probe-events`.

## Internal speakers

Output only (no microphones yet), 48 kHz stereo. Two pieces: the `AudShim` kext (loaded by OpenCore; it talks to the
audio DSP `8086:51CA`, SoundWire and the RT1316 amplifiers) and the `XeSpeakers` AudioServerPlugIn (the macOS output
device). The device shows up in System Settings › Sound › Output as **"Alto-falantes internos"** (the name is
`kDeviceName` in `tools/XeSpeakers/XeSpeakers.c`).

**1. Firmware and topology (Intel, not shipped here).** Download the official SOF release tarball (`sof-bin-<version>.tar.gz`,
from github.com/thesofproject/sof-bin; tested with 2026.09.2) and take two files from it:

| File in sof-bin | Use |
|---|---|
| `sof/intel-signed/sof-adl.ri` (`sof-rpl.ri` is a link to it) | the IPC3 SOF 2.2 firmware for Raptor Lake-P, Intel-signed |
| `sof-tplg/sof-rpl-rt1316-l12-rt714-l0.tplg` | the topology for this laptop: RT714 on SoundWire link 0, RT1316 on links 1 and 2 |

The firmware is distributed by Intel under its own licence (`LICENCE.Intel` in the tarball). Put both files in
`~/xe/build/audio/fw/use/` (renaming `sof-adl.ri` to `sof-rpl.ri`), which is `audctl`'s default, or pass the paths.

**2. Generate the kext's data header and build.** `AudShimData.h` holds the firmware payload, the IPC3 messages derived
from the topology and the amplifier init table. It is generated, reproducible byte for byte, and not committed
(`.gitignore`):

```sh
clang -O2 -Wall -Wextra -o tools/build/audctl tools/audctl.c -framework IOKit -framework CoreFoundation
tools/build/audctl tplg PATH/sof-rpl-rt1316-l12-rt714-l0.tplg     # optional: parse the topology offline
tools/build/audctl gen-header tools/AudShim/AudShimData.h --fw=PATH/sof-rpl.ri --tplg=PATH/sof-rpl-rt1316-l12-rt714-l0.tplg
zsh tools/audio/install-audio.sh build     # audctl, AudShim.kext, XeSpeakers.driver -> tools/build/
```

**3. Kext.** Mount the EFI (`sudo diskutil mount disk0s1`), then `zsh tools/audio/install-audio.sh kext`: it copies
`AudShim.kext` to `EFI/OC/Kexts` and appends its `Kernel>Add` entry if missing (config backed up as
`config.plist.before-audshim`). Reboot. At boot the kext only maps the device and enables memory decode; nothing else
is touched until a client asks. Check:

```sh
tools/build/audctl info        # "interface v8"
tools/build/audctl drv test    # three short ~1 kHz tones on both speakers at a low level, then "DRV TEST RESULT: SUCCESS"
```

**4. Output device.** `zsh tools/audio/install-audio.sh hal` (sudo: copies `XeSpeakers.driver` to
`/Library/Audio/Plug-Ins/HAL` and restarts coreaudiod). Choose the device in System Settings › Sound › Output. The
volume starts at 30 % the first time.

Notes:
- **Level.** The kext caps the DSP gain at −6 dB and ramps every change; the SOF topology has no speaker protection.
  Boot-arg `audshimcapdb=N` (0–20) moves the cap, `audshimlingerms=N` the 5 s delay before the amps are switched off
  after playback stops, `-audshimverbose` logs every register access, `-audshimoff` stops AudShim from attaching.
- The hardware is brought up when coreaudiod loads the plug-in (~1–2 s after login) and fully torn down (amps muted
  and off, DMA, links and DSP off) on coreaudiod exit, sleep, shutdown/restart or kext stop.
- `audctl drv state` shows starts, xruns and DMA errors while the device is in use. Logs:
  `log show --last 10m --predicate 'subsystem == "local.xe.XeSpeakers" OR eventMessage CONTAINS "AudShim DRV"'`.
- The step-by-step bring-up commands used during development (`audctl m1`, `m2b`, `m3`, `m4`, `m5`, each with `--dry`
  and `--undo`) still work when the HAL driver is not installed.
- Undo: `zsh tools/audio/uninstall-audio.sh hal`, then `… kext` (disables the entry and removes the kext) and reboot.

## Wi-Fi (AirportItlwm)

The shipped `config.plist` loads **AirportItlwm-Tahoe 2.4.0-alpha** (laobamac fork of itlwm, commit 738d4c6) and keeps
`itlwm.kext` disabled. AirportItlwm uses Tahoe's own IO80211Family/IOSkywalkFamily, so no root patch is needed, and Wi-Fi
appears in the menu bar like on a Mac (HeliPort is not needed; remove its login item if you had one).
- `MinKernel 25.0.0` / `MaxKernel 25.99.99`: it only loads on Tahoe. Never enable `itlwm.kext` and `AirportItlwm.kext`
  together (the boot halts).
- After the switch macOS may keep an old "Ethernet" service for en0: System Settings › Network › remove it, then "+" →
  Wi-Fi. en0 should be the Wi-Fi port (iMessage/FaceTime expect a built-in en0).
- Optional: boot-arg `itlwm_he=1` turns on Wi-Fi 6 (HE) rates.
- Fallback: `config-install.plist` still uses itlwm (connect with HeliPort). Switch back the same way if a macOS update
  breaks AirportItlwm.
- Not available with this driver: AirDrop/AWDL, WPA3-only networks.

## Sleep, lid and display sleep

This laptop has no working system sleep on macOS (Modern Standby firmware, `_S3` hidden, eDP cannot be retrained by
the TGL framebuffer). With macOS defaults, **closing the lid starts a real sleep that does not come back**. Use:

```sh
sudo zsh tools/sleep/10-pmset-safe.sh     # sleep 0, disablesleep 1, hibernatemode 0, display sleep 3 min battery / 10 min AC
zsh tools/sleep/install-xe-lid.sh         # LaunchAgent: lid close -> display off, lid open -> display on (log /tmp/xe-lid.log)
```

The machine stays awake (a few watts) with the lid closed: shut it down for long breaks, and consider
`sudo pmset -b lowpowermode 1` on battery. Undo: `sudo zsh tools/sleep/90-pmset-revert.sh` and
`zsh tools/sleep/install-xe-lid.sh --remove`. On boots where the EC does not report the lid (see STATUS), turn the display
off by hand.

## Touchpad vibration

The haptic touchpad clicks by itself; macOS can only change how strong the vibration is (HID feature report 0x09,
0–100). `clang -O2 -o tools/build/xe-haptic tools/xe-haptic.c -framework IOKit -framework CoreFoundation`, then
`xe-haptic list` (read-only dump) or `xe-haptic set 60 100`. The value is lost at reboot. For a persistent setting,
`zsh tools/build-xe-hapticbar.sh` and `open tools/xe-hapticbar.app` (add it to Login Items): a hand icon in the menu bar
with a slider, applied at login and on every change.
