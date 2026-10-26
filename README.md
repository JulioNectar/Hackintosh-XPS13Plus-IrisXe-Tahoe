# Iris Xe acceleration on macOS Tahoe — Dell XPS 13 Plus (9320)

Hardware-accelerated graphics (**Metal, Core Animation, Liquid Glass**) for the **Intel Iris Xe of the i7-1360P (Raptor Lake-P, 8086:A7A0)** on **macOS Tahoe 26.7.1**, in an OpenCore hackintosh.

There is no Apple driver for Raptor Lake. This project runs **Apple's own driver for Tiger Lake Iris Xe** (GPUDriversIntel 16.0.32, written for Macs that never shipped) on the newer GPU. Both GPUs share the Xe-LP (Gen12) architecture. A modified **NootedGreen** Lilu plugin bridges the generation gap in the kernel, and a set of user-space patches bridges the gap between that 2020-era driver and Tahoe's Metal stack.

> 🚧 **Work in progress — this repository is a living snapshot.** Development happens day by day on the author's own
> XPS 13 Plus, and every fix is committed here after it is confirmed on real hardware. Expect frequent updates;
> see [CHANGELOG.md](CHANGELOG.md) for what changed and what is still being tested, and the
> [releases](../../releases) for the stable points.

## New: full CPU turbo and lower idle power (`-ngreenpm`, 26 Oct)

macOS power management (XCPM) predates Intel's hybrid CPUs, and on 12th/13th-gen laptops it holds the CPU back:

- **P-cores stuck at ~3.8 GHz.** XCPM writes the HWP turbo limit in 100 MHz units, but hybrid P-cores count HWP
  performance in ~78.7 MHz units, so "5.0 GHz" becomes ~3.9 GHz. On top of that, its default EPP (128) clips
  one-core turbo on Alder/Raptor Lake mobile parts (an Intel-documented behaviour), and energy-efficient turbo holds
  the top bins.
- **Deep idle states off.** XCPM's idle table for the spoofed CPU has C8/C10 disabled and uses C3, which these CPUs
  don't have, so cores never sleep deeper than C6.

The NootedGreen **XE-PM** profile fixes all of this, 20 s after boot. It is **on in this repo's `config.plist`**
(boot-arg `-ngreenpm`). Measured on the XPS 13 Plus (i7-1360P):

| | Before | With `-ngreenpm` |
|---|---|---|
| Geekbench 7 single-core | 1269 | **1697 (+34 %)** |
| Geekbench 7 multi-core | 5859 | 6148 (+5 %, limited by the 17.5 W PL1 and the thin chassis) |
| One-thread clock | 3.3–3.8 GHz | 4.3–4.85 GHz |
| Idle package power | ~4.6 W | ~3.3 W |

Other Alder Lake / Raptor Lake hybrid machines on this stack should see the same turbo fix; add `-ngreenpm` to try it
(**keep a backup EFI**; it patches XCPM and only applies on macOS 26.7.1 25G241, where the code was checked byte for
byte). EPP: `ngreenpmepp=N` at boot (default 0, max performance) or `sudo sysctl -w debug.xepm.epp=64` live.
Status: `sysctl debug.xepm`. Details in [CHANGELOG.md](CHANGELOG.md).

## Already installed? Update

If the stack already runs on your machine, update it like this (latest changes in [CHANGELOG.md](CHANGELOG.md)):

1. **Back up the EFI you boot from** (a copy of the `EFI` folder or a second bootable USB EFI).
2. In your clone, on an APFS disk:
   ```sh
   git pull
   zsh install.sh          # rebuilds and reinstalls NootedGreen, the Metal driver, libxedrv and the shader compiler
   ```
3. Copy the new `build/NootedGreen.kext` over `EFI/OC/Kexts/NootedGreen.kext` on the EFI you boot from.
4. Reboot and check: `kmutil showloaded | grep NootedGreen`. Since 25 Oct, `ioreg -rc IntelAccelerator -d1 | grep rowchk`
   also shows `xe-rowchk-post` (the GPU fix for the dash artifacts in Preview/Photos, #9).

Don't run `make-efi.sh` again to update: it generates **new** serial numbers (SMBIOS), which signs you out of
iCloud/iMessage. Keep your EFI and only replace the kext.

If it doesn't boot, use the backup EFI. Single fixes can be turned off with the boot-arg named in their CHANGELOG
entry. Details: [docs/INSTALL.md](docs/INSTALL.md#updating-an-existing-install).

> 🇧🇷 Leia em português: [README.pt-BR.md](README.pt-BR.md)

> ⚠️ **Experimental.** Expect occasional GPU hangs and kernel panics. Keep a second bootable disk (or the USB installer) around. Read [docs/STATUS.md](docs/STATUS.md) before you start.

---

## What works

| Area | Status |
|---|---|
| Desktop, Dock, windows, Liquid Glass blur | ✅ GPU-accelerated through Metal |
| Native panel resolution (3840×2400) and HiDPI scaling (1280–1920 pt wide) | ✅ switch with `tools/xe-res`, not System Settings (a brief glitch during the switch is cosmetic) |
| GPU clock | ✅ ~1300 MHz under load (requests RP0 1500 MHz; the CPU package power limit PL4/IccMax caps it at ~1300), idles in RC6 |
| CPU | ✅ **full turbo (up to ~4.85 GHz on one P-core) with `-ngreenpm`** (XE-PM, see above) · ✅ **all 16 threads (4 P-cores with HT + 8 E-cores)** — the E-cores used to hang the boot; fixed with one byte in `Cpuid1Data` (see [STATUS](docs/STATUS.md)). CpuTopologyRebuild 2.0.2 (default mode) groups them as 4 P-cores with 4 threads each, so heavy work lands on P-cores; System Information shows 12 cores |
| HEIC / YUV images (wallpapers, photos) | ✅ |
| App icons, Finder icons, image thumbnails | ✅ (the 10-bit extended-range layer format `w30r`/`BGR10_XR` is Apple-silicon-only: real Intel Macs don't render it either) |
| Hardware video decode | ✅ **H.264 on the GPU** (bit-exact vs. software) for apps that use VideoToolbox: QuickTime, Photos, video editors, conversions. Needs `-ngreengva` and the patched VA plugin (`macos/vadriver/build-vadriver.sh`, installed by hand for now) · ✅ **HEVC** too (NootedGreen XE-GVAHEVC) · ⚠️ Safari decodes on the CPU: its WebKit GPU process may not load drivers from `/Library/GPUBundles` · ⚠️ with `-ngreengva` FairPlay content (Apple Music, probably Apple TV+) does not play: the VA plugin takes the hardware key-exchange path and fails with "ME not loaded" ([#11](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/11)); remove the boot-arg if you need it |
| YouTube | ✅ Chrome with GPU compositing, up to 4K (VP9/AV1 decoded on the CPU) · ✅ Safari up to 1080p H.264 (needs an audio output: the internal speakers, Bluetooth, or the BlackHole virtual device) |
| Screen recording | ✅ |
| Touchscreen | ✅ up to 10 fingers via VoodooI2C; `tools/xe-toque` adds one-finger scroll, hold-for-right-click, edge swipes, pen, magnifier and a touch mode |
| Trackpad, keyboard | ✅ trackpad and touchscreen on GPIO/APIC interrupts (no polling freezes) |
| Brightness | ✅ `tools/xe-brilho` (the macOS slider now covers the full panel range with `ngreenblmax=17777`) · ⚠️ the sun keys depend on a Dell firmware state that changes from boot to boot; F6/F7 always work through `xe-brilho daemon` |
| Shutdown / restart | ✅ fast (XE-IDLEBOUND; used to stall ~3 min and panic) |
| Boot screen | ✅ sharp Apple logo from the first frame, no garbage flash (OpenCore `Resolution=Max` + `gfxrst=1` + XE-BOOTHOLD) |
| Internal speakers | ✅ our own `AudShim` kext runs Intel's SOF firmware on the audio DSP, SoundWire and both RT1316 amplifiers; `XeSpeakers` makes them a normal macOS output (volume, mute). Capped at −3 dB (AudShim 0.9.1), no speaker protection · AudShim 0.9.2 powers the DSP down after 30 s of silence (`audshimidleoffms=N`, 0 = never); the first sound after that takes ~1–2 s · ✅ internal microphones (RT714) as **"Microfone interno"** |
| Wi-Fi, Bluetooth | ✅ native Wi-Fi menu via AirportItlwm-Tahoe (no HeliPort) · ❌ AirDrop/AWDL · ✅ Bluetooth (IntelBluetoothFirmware) |
| Touchpad haptics | ✅ click-vibration strength with `tools/xe-haptic` / the `xe-hapticbar` menu-bar slider · ❌ no on-demand pulse (Force Touch feedback) |
| Fingerprint reader | ✅ for `sudo` (Goodix 27C6:63BC via libfprint, PAM module in `sudo_local`; `tools/fingerprint`) · ❌ login, screen unlock and Touch ID prompts (no Secure Enclave) |
| Power button | ✅ a short press locks the screen (`tools/xe-pblock`) |
| Sleep | ❌ no system sleep in the default install (Modern Standby only firmware; experimental S0 deep idle enters and wakes by RTC alarm, but lid/power button do not wake it and the panel stays black on wake, so it is paused and not shipped, see [STATUS](docs/STATUS.md)); ✅ display sleep (backlight off; with `-ngreenfbdpms` the panel does not come back), and lid close turns the display off (`tools/sleep`) |

Everything else about the laptop (NVMe, battery, USB) is a regular OpenCore setup. The camera (OV01A10 behind Intel's
IPU6) does **not** work on macOS; the fingerprint reader works for `sudo` only. **Internal speakers** work since
2026-10-22 without any Apple audio driver: `AudShim` boots the Intel cAVS 2.5 audio DSP with Intel's SOF firmware
(taken from the official sof-bin release at install time, not shipped here), starts SoundWire and the RT1316
amplifiers, and the `XeSpeakers` AudioServerPlugIn publishes the output and microphone devices ([docs/INSTALL.md](docs/INSTALL.md#internal-speakers-and-microphones)).

## How it works (one paragraph)

The EFI presents the GPU as Tiger Lake (`device-id 9A49`). Apple's TGL kexts load from `/Library/Extensions`, and **NootedGreen** patches them in memory: display engine 13 quirks, the GuC-less host scheduler, broken resolve paths, IOSurface layout changes in Tahoe, GPU frequency, and more ([docs/KERNEL-PATCHES.md](docs/KERNEL-PATCHES.md)). In user space, the TGL **Metal driver** is binary-patched and gets an adapter library (`libxedrv`) that converts Tahoe's private Metal structures to the old layout and works around hardware limits. The TGL **shader compiler** (IGC gen12lp, LLVM 3902) runs inside Tahoe's compiler service (LLVM 32023) through an API adapter and an LLVM bridge ([docs/METAL.md](docs/METAL.md)).

## Repository layout

```
EFI/                     OpenCore EFI for the XPS 13 Plus 9320 (serials removed — generate your own)
  OC/config.plist        accelerated config
  OC/config-install.plist  safe config for installing macOS (NootedGreen off, verbose)
src/NootedGreen/         the modified NootedGreen source (Lilu plugin)
build-nootedgreen.sh     builds src/NootedGreen with only the Command Line Tools
src/HookCase/            NootedGreen's modified HookCase (MIT) — the TGL kexts depend on it
build-hookcase.sh        builds src/HookCase (required; installed to /Library/Extensions by macos/install-macos.sh)
patches/                 diff of this fork against upstream NootedGreen 95f8189
macos/                   user-space stack: Metal driver patcher, libxedrv, shader compiler, VA (video) plugin patcher, installer
tools/                   xe-res (resolution/DPI), xe-brilho (brightness), xe-perfil (color profile), xe-toque (touchscreen gestures),
                         XeShim + xectl (register access), diagnostics/ (the test programs used to find bugs),
                         AudShim + audctl + XeSpeakers + audio/ (internal speakers), sleep/ (pmset + lid agent),
                         xe-haptic + xe-hapticbar (touchpad vibration), fingerprint/ (fingerprint for sudo: libfprint patch,
                         xe-fp, PAM module), xe-pblock/ (power button locks the screen)
docs/                    install guide, patch catalogs, status, debugging notes
```

Documentation: [INSTALL](docs/INSTALL.md) · [STATUS](docs/STATUS.md) · [KERNEL-PATCHES](docs/KERNEL-PATCHES.md) ·
[METAL](docs/METAL.md) · [DEBUGGING](docs/DEBUGGING.md) · [HISTORY](docs/HISTORY.md) (how it was done, phase by phase) ·
[EXPERIMENTS](docs/EXPERIMENTS.md) (what failed and why) · [CATALOG](docs/CATALOG.md) (full patch and tool reference)

## Quick start (Dell XPS 13 Plus 9320)

**1. Make your EFI** (on any Mac, with git):

```sh
git clone https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe.git && cd Hackintosh-XPS13Plus-IrisXe-Tahoe
zsh make-efi.sh        # -> dist/EFI with YOUR OWN serial numbers (never share them)
```

**2. Install macOS Tahoe 26.7.1 (25G241)** booting with `EFI/OC/config-install.plist` renamed to `config.plist`
(acceleration off). BIOS: usual hackintosh settings (storage in AHCI/NVMe mode, not RAID/VMD; Secure Boot off). The E-cores can stay enabled.

**3. On the new macOS**, with the accelerated `config.plist` back in place:

```sh
xcode-select --install          # Command Line Tools, once
zsh install.sh                  # checks, downloads Apple's driver bundles via NootedGreen, builds, installs
```

Copy the freshly built `build/NootedGreen.kext` to `EFI/OC/Kexts/`, approve the extension if asked, reboot.
`zsh install.sh --check` only runs the checks; `zsh install.sh --uninstall` removes everything.

**Updating:** see [Already installed? Update](#already-installed-update).

Other hardware (other Raptor Lake / Alder Lake laptops): the user-space part is the same, but you need your own
EFI (ACPI, USB map, input devices). The full manual procedure is in [docs/INSTALL.md](docs/INSTALL.md).
NootedGreen describes every non-Tiger-Lake GPU as 96 EU (12 subslices × 8 EU, 8 L3 banks) unless told otherwise.
**80 EU parts** (e.g. i5-1334U, i5-1340P, i5-12500H): add **`ngreen-eu=80 ngreen-l3=6`** to the boot-args. This removes
the rendering artifacts (confirmed on an i5-1340P and an i5-1334U, [#3](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/3), on an i5-1240P / 46A6, [#16](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/16), and on a native Tiger Lake i5-1135G7 with 80 EU); these chips have 6 L3 banks
(fuse register `0x9118`), not 8. Check: `ioreg -rc IntelAccelerator -d1 | grep xe-topo` shows 20486.

## Credits

- **NootedGreen** by StezzaPilot ([sgiammori/NootedGreen](https://github.com/sgiammori/NootedGreen)): the base of the kernel plugin and the source of the 16.0.32 driver bundles. Licensed under the *Thou Shalt Not Profit License 1.0*, which also covers the fork in `src/`.
- **WhateverXe** and **OpenCore Legacy Patcher fork** by b00t0x, and jkbuha's WhateverXe work: reference for Gen12 on macOS (including the Tahoe VA-plugin payload-header fix).
- **BlackHole** by Existential Audio (virtual audio output, GPL-3.0), built by `tools/build-blackhole.sh`; also the reference for the AudioServerPlugIn property set.
- **Sound Open Firmware** (Intel SOF firmware and topologies, sof-bin) and the Linux `sound/soc/sof` + SoundWire drivers: the reference for every AudShim step. The firmware is downloaded by you, not shipped.
- **laobamac/itlwm** (AirportItlwm-Tahoe, GPL-2.0), shipped in `EFI/OC/Kexts`.
- **IntelIGPU-Tahoe**: reference for running the TGL Metal driver on Tahoe.
- **acidanthera** (OpenCore, Lilu, WhateverGreen, VirtualSMC, MacKernelSDK), **OpenIntelWireless** (itlwm, IntelBluetoothFirmware), **VoodooI2C**, **USBToolBox**.
- Developed on one Dell XPS 13 Plus by its owner, pair-programming with Claude (Anthropic).

## License

[Thou Shalt Not Profit License 1.0](LICENSE), the license of NootedGreen, which this project derives from. Third-party
kexts in `EFI/` keep their own licenses, and no Apple binaries are included: see [NOTICE.md](NOTICE.md).
