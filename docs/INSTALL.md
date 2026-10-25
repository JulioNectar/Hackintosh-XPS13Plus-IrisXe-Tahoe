# Installation guide

> **Same laptop (XPS 13 Plus 9320)?** The short path is `zsh make-efi.sh` (EFI with your own serials) and, after
> installing macOS, `zsh install.sh`. See the README. This guide is the manual procedure behind those scripts.

Target: Dell XPS 13 Plus 9320, i7-1360P, Iris Xe 8086:A7A0, **macOS Tahoe 26.7.1 (25G241)**. Other Raptor Lake /
Alder Lake laptops may work with their own EFI, but the patches were only tested here. NootedGreen describes
every non-Tiger-Lake GPU as 96 EU (12 subslices × 8 EU, 8 L3 banks, MCR steering to DSS0). On an **80 EU part**
(e.g. i5-1334U, i5-1340P, i5-12500H) add **`ngreen-eu=80 ngreen-l3=6`** to the boot-args: it removes the rendering
artifacts (confirmed on an i5-1340P and an i5-1334U, issue [#3](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/3), and on an Alder Lake-P i5-1240P / 46A6, [#16](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/16)). `ioreg -rc IntelAccelerator -d1 | grep xe-topo`
then shows 20486 (80 EU, 6 L3 banks) instead of 24584. To check another chip, read the fuses from a Linux live USB:
`sseu_topology` (number of DSS × 16 = EU) and register `0x9118` (low 4 bits = L3 bank **pairs** fused off; banks =
2 × (4 − number of set bits)), see [#3](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/3), [#4](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/4).

## 0. What you need

- A USB drive (16 GB+) for OpenCore and the installer, or a second disk.
- Another Mac (or the laptop itself, from Windows/Linux) to create the installer.
- The `sle_Internal/le` and `sle_Internal/sle` folders of [NootedGreen](https://github.com/sgiammori/NootedGreen)
  (Apple's 16.0.32 Tiger Lake driver bundles) at commit `95f81898be65cb9f7b3d370ca78054be4cc8a62d`. `install.sh`
  clones it into `build/deps/NootedGreen` by itself. Keep this repository and NootedGreen on an APFS/HFS disk, not on
  FAT/exFAT (see step 5).

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
zsh build-hookcase.sh                    # -> build/HookCase.kext (installed by step 5)
```

`HookCase.kext` is required: Apple's TGL kexts link against `org.smichaud.HookCase`, and without it kmutil refuses
to register them. It goes to `/Library/Extensions` (step 5), not to the EFI.

Put `build/NootedGreen.kext` in `EFI/OC/Kexts/` and switch back to the accelerated `config.plist` (keep the boot-args
from the repository).

**Own config.plist or reused NVRAM:** OpenCore only writes `NVRAM > Add > boot-args` if the variable is also listed
under `NVRAM > Delete` for the same GUID (`7C436110-AB2A-4BBB-A880-FE41995C9F82`); otherwise the OpenCore log shows
`ignored, exists` and the old boot-args stay. The repository's configs already list it. Without it the Xe boot-args
are missing and the boot stops right after the accelerator starts (`didn't find dpmicro`, no panic), see [#16](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/16).

## 5. User-space side

Keep the repository and the NootedGreen download on an APFS disk (your home folder), not on a FAT/exFAT USB
stick: macOS stores metadata there in `._` files, which break the bundle copies and code signing.

```sh
git clone https://github.com/sgiammori/NootedGreen.git ~/NootedGreen
git -C ~/NootedGreen checkout 95f81898be65cb9f7b3d370ca78054be4cc8a62d
zsh macos/build-all.sh ~/NootedGreen/sle_Internal/sle
sudo zsh macos/install-macos.sh ~/NootedGreen/sle_Internal/sle ~/NootedGreen/sle_Internal/le
```

`zsh install.sh --build-only` does the clone, checkout and builds automatically (into `build/deps/NootedGreen`).

Approve the extension in **System Settings › Privacy & Security** if asked, and reboot.

## 6. After the first accelerated boot

- Build the two command-line tools into `tools/build/`:
  ```sh
  mkdir -p tools/build
  clang -O2 -o tools/build/xe-res tools/xe-res.c -framework CoreGraphics -framework CoreFoundation
  clang -O2 -fobjc-arc -DXECTL_NO_MAIN -o tools/build/xe-brilho tools/xe-brilho.m tools/xectl.c -framework AppKit -framework IOKit
  ```
- Resolution / UI scale: `tools/build/xe-res` (list) and `tools/build/xe-res w1680` (HiDPI 1680 pt wide). Avoid
  **System Settings › Displays**.
- Brightness: build `tools/XeShim` (`zsh tools/build-kext.sh XeShim`), load it, then `tools/build/xe-brilho 70`.
- Touchscreen trail (optional): `zsh tools/build-xe-toque.sh`, then `open tools/xe-toque.app` and allow it in
  Privacy & Security › Input Monitoring. Only touches on the screen draw; the trackpad is ignored.
- Check: `system_profiler SPDisplaysDataType` shows **Metal Support** and the chipset model you set.
- Internal speakers, Wi-Fi, lid/sleep setup and touchpad vibration: see the sections below.

## 7. Moving OpenCore to the internal disk

1. `sudo diskutil mount disk0s1` (check the identifier with `diskutil list`) and copy `EFI/`.
2. In the Dell BIOS (F2) › Boot Configuration › Boot Sequence: **Add Boot Option** → the internal disk →
   `\EFI\BOOT\BOOTx64.efi`, and move it to the top.

## Updating an existing install

Fixes land in the repository as they are confirmed (see [CHANGELOG.md](../CHANGELOG.md)). To update a working setup:

1. **Back up the EFI you boot from** (copy the whole `EFI` folder somewhere, or keep a second bootable USB EFI), so a
   kext that does not boot can be undone.
2. Update and rebuild, from your clone (on an APFS disk):
   ```sh
   cd ~/XPS13Plus-Tahoe-IrisXe   # your clone
   git pull
   zsh install.sh                # rebuilds and reinstalls NootedGreen, the Metal driver, libxedrv and the shader compiler
   ```
   This replaces the user-space files in `/Library/GPUBundles` and `/Library/Extensions` (they take effect for apps
   started afterwards, and for WindowServer after the reboot).
   If you update by hand instead, rebuild the Metal bundle with `zsh macos/build-all.sh <sle/>` (it builds libxedrv
   and runs `patch-mtl17.py`) and reinstall it with `sudo zsh macos/install-macos.sh <sle/> <le/>` (step 5). A
   `git pull` alone changes nothing on the system: the WindowServer memory leak fix (Metal bundle v18,
   [#12](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/12)) and the invisible Dock badge
   digits fix (libxedrv v19, [#10](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/10)) only
   apply after this rebuild and reinstall, and WindowServer picks them up after a reboot.
3. Copy the new `build/NootedGreen.kext` over `EFI/OC/Kexts/NootedGreen.kext` on the EFI you boot from. Only
   compare `config.plist` with the repository's if the CHANGELOG entry says boot-args or kexts changed; keep your
   own serials. Do not run `make-efi.sh` again: it generates new SMBIOS serials (iCloud/iMessage sign-out).
4. Reboot, then check that the new kext is loaded: `kmutil showloaded | grep NootedGreen` and
   `system_profiler SPDisplaysDataType | grep Metal`. Builds from 25 Oct on also publish `xe-rowchk-post` in
   `ioreg -rc IntelAccelerator -d1` (XE-ROWCHK; its low 16 bits should be 0xc000).
5. Optional parts are updated separately with their own scripts: speakers/microphones
   (`zsh tools/audio/install-audio.sh build`, then `kext`, then `hal`), fingerprint (`tools/fingerprint`), etc.

If the new kext does not boot, boot the backup EFI and put the old `NootedGreen.kext` back. A single fix can also be
turned off with its boot-arg (listed in the CHANGELOG entry and in [KERNEL-PATCHES.md](KERNEL-PATCHES.md)).

## Recovery

- `config-install.plist` (NootedGreen off) is only safe **before** the TGL files are installed, or after
  `zsh install.sh --uninstall`. It still spoofs the GPU as 9A49, and once `install-macos.sh` has set the TGL
  accelerator's `IOPCIPrimaryMatch` to `0x9a498086`, Apple's stock TGL driver binds 9A49 without NootedGreen's patches
  and panics or stalls (stock 16.0.32 only accepts 9A40/9A48). For an unaccelerated fallback after installing, also
  remove the `device-id` / `AAPL,ig-platform-id` spoof from that config, or uninstall first.
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
  *Clear drawing* wipes it), **Touch mode** (switches to a 1152-pt-wide HiDPI mode with `xe-res` and back
  to the previous mode). It uses `build/xe-res` next to the app (`tools/build/xe-res`, see step 6); if you moved the app,
  point it at the tool with `defaults write local.xe.toque xeRes /path/to/xe-res`.
- Log: `~/Library/Logs/xe-toque/xe-toque.log`. Diagnostic: `xe-toque.app/Contents/MacOS/xe-toque --probe-events`.

## Internal speakers and microphones

48 kHz stereo output and input. Two pieces: the `AudShim` kext (loaded by OpenCore; it talks to the
audio DSP `8086:51CA`, SoundWire, the RT1316 amplifiers and the RT714 microphone codec) and the `XeSpeakers`
AudioServerPlugIn (the macOS devices). They show up in System Settings › Sound as **"Alto-falantes internos"** (Output)
and **"Microfone interno"** (Input); the names are in `tools/XeSpeakers/XeSpeakers.c`.

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
tools/build/audctl info        # "interface v9"
tools/build/audctl drv test    # three short ~1 kHz tones on both speakers at a low level, then "DRV TEST RESULT: SUCCESS"
tools/build/audctl rec /tmp/mic.wav --seconds=5   # talk or clap: "REC RESULT: OK", RMS well above -100 dBFS
```

**4. Devices.** `zsh tools/audio/install-audio.sh hal` (sudo: copies `XeSpeakers.driver` to
`/Library/Audio/Plug-Ins/HAL` and restarts coreaudiod). Choose the devices in System Settings › Sound › Output and
Input. The volume starts at 30 % the first time; the input level slider is a software gain (0 to +30 dB, default +12 dB).
With an older 0.8.0 kext (interface v8) only the speakers appear.

Notes:
- **Level.** The kext caps the DSP gain at −3 dB (−6 dB before 0.9.1) and ramps every change; the SOF topology has no speaker protection.
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

## Hardware video decode (optional)

H.264 and HEVC decode on the GPU for VideoToolbox clients (QuickTime, Photos, video editors). It needs the boot-arg
**`-ngreengva`** and the patched VA plugin, which is not part of `install.sh` and is installed by hand:

```sh
zsh macos/vadriver/build-vadriver.sh ~/NootedGreen/sle_Internal/sle      # -> macos/out/AppleIntelTGLGraphicsVADriver.bundle (no sudo)
sudo rm -rf /Library/GPUBundles/AppleIntelTGLGraphicsVADriver.bundle
sudo ditto macos/out/AppleIntelTGLGraphicsVADriver.bundle /Library/GPUBundles/AppleIntelTGLGraphicsVADriver.bundle
killall VTDecoderXPCService     # apps that already opened a decoder need a restart
```

(With `install.sh` the `sle/` folder is `build/deps/NootedGreen/sle_Internal/sle`.) Add `-ngreengva` to the boot-args
and reboot. Without the boot-arg the plugin is not used and decode stays in software.

- **FairPlay does not play with `-ngreengva`**: Apple Music (and probably Apple TV+) fails, the VA plugin logs "ME not
  loaded" ([#11](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/11)). Remove the boot-arg if
  you need that content.
- Safari still decodes on the CPU: its WebKit GPU process may not load plugins from `/Library/GPUBundles`.
- Undo: remove `-ngreengva` and delete `/Library/GPUBundles/AppleIntelTGLGraphicsVADriver.bundle`.

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

Keep the default display sleep (NootedGreen XE-FBDPMS, backlight off only): with `-ngreenfbdpms` the panel is really
powered down and does not come back on wake (eDP link not retrained). Experimental S0 deep idle work is described in
[STATUS.md](STATUS.md); it is paused and not part of this install.

## Touchpad vibration

The haptic touchpad clicks by itself; macOS can only change how strong the vibration is (HID feature report 0x09,
0–100). `clang -O2 -o tools/build/xe-haptic tools/xe-haptic.c -framework IOKit -framework CoreFoundation`, then
`xe-haptic list` (read-only dump) or `xe-haptic set 60 100`. The value is lost at reboot. For a persistent setting,
`zsh tools/build-xe-hapticbar.sh` and `open tools/xe-hapticbar.app` (add it to Login Items): a hand icon in the menu bar
with a slider, applied at login and on every change.

## Fingerprint for sudo (optional)

The Goodix sensor in the power button (27C6:63BC, match-on-chip) works through libfprint's `goodixmoc` driver. Only
`sudo` (and other PAM services you add it to) can use it: login, screen unlock, Touch ID settings and
LocalAuthentication need the Secure Enclave and are **not** supported. The sensor stores the prints itself.

```sh
bash tools/fingerprint/build.sh          # ~4 min: downloads and builds libusb, glib, libgusb, patched libfprint -> tools/build/fingerprint
F=tools/build/fingerprint/dist/xefp/xe-fp
$F list                                  # device info and prints on the chip (as yourself, no sudo)
$F enroll right-index                    # 12 touches, lift and re-place each time
$F verify                                # exit 0 = match
sudo bash tools/fingerprint/install-pam.sh                  # copies xe-fp + pam_xefp.so to /usr/local (root-owned)
sudo bash tools/fingerprint/install-pam.sh --enable-sudo    # adds one line to /etc/pam.d/sudo_local (backup kept)
```

Keep that terminal open and test in a new one: `sudo -k; sudo -v` should ask you to touch the sensor, and falls back to
the password after 10 s, on no match, over SSH or when the sensor is busy. Only prints enrolled by xe-fp for the same
user name are accepted; prints from other systems (e.g. Windows Hello) are listed as foreign and never used or deleted.
Our libfprint patch also removes upstream's automatic wipe of the sensor storage when listing fails.
Undo: `sudo bash tools/fingerprint/uninstall-pam.sh` (prints stay on the sensor; `xe-fp delete right-index` removes one).

## Power button locks the screen (optional)

`sh tools/xe-pblock/install.sh` builds `xe-pblock.app`, copies it to `~/Library/Application Support/xe/`, runs two
15-second checks (touching the fingerprint sensor must not count as a press; one press must be seen) and loads the
user LaunchAgent `local.xe.pblock`. Allow **xe-pblock** in Privacy & Security › Input Monitoring when asked. A short
press then locks the screen at once (the same call as Apple menu › Lock Screen); with the screen already locked it does
nothing. No `pmset` change is needed: on this laptop loginwindow ignores short presses. `sh tools/xe-pblock/probe.sh`
shows the events live without locking. Log: `~/Library/Logs/xe-pblock.log`. Undo: `sh tools/xe-pblock/uninstall.sh`
(`--reset-tcc` also removes the permission).
