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
