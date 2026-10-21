# Iris Xe acceleration on macOS Tahoe — Dell XPS 13 Plus (9320)

Hardware-accelerated graphics (**Metal, Core Animation, Liquid Glass**) for the **Intel Iris Xe of the i7-1360P (Raptor Lake-P, 8086:A7A0)** on **macOS Tahoe 26.7.1**, in an OpenCore hackintosh.

There is no Apple driver for Raptor Lake. This project runs **Apple's own driver for Tiger Lake Iris Xe** (GPUDriversIntel 16.0.32, written for Macs that never shipped) on the newer GPU. Both GPUs share the Xe-LP (Gen12) architecture. A modified **NootedGreen** Lilu plugin bridges the generation gap in the kernel, and a set of user-space patches bridges the gap between that 2020-era driver and Tahoe's Metal stack.

> 🚧 **Work in progress — this repository is a living snapshot.** Development happens day by day on the author's own
> XPS 13 Plus, and every fix is committed here after it is confirmed on real hardware. Expect frequent updates;
> see [CHANGELOG.md](CHANGELOG.md) for what changed and what is still being tested, and the
> [releases](../../releases) for the stable points.

> 🇧🇷 Leia em português: [README.pt-BR.md](README.pt-BR.md)

> ⚠️ **Experimental.** Expect occasional GPU hangs and kernel panics. Keep a second bootable disk (or the USB installer) around. Read [docs/STATUS.md](docs/STATUS.md) before you start.

---

## What works

| Area | Status |
|---|---|
| Desktop, Dock, windows, Liquid Glass blur | ✅ GPU-accelerated through Metal |
| Native panel resolution (3840×2400) and HiDPI scaling (1280–1920 pt wide) | ✅ (switch with `tools/xe-res`, not System Settings) |
| GPU clock | ✅ ~1300 MHz under load (requests RP0 1500 MHz; the CPU package power limit PL4/IccMax caps it at ~1300), idles in RC6 |
| HEIC / YUV images (wallpapers, photos) | ✅ |
| App icons, Finder icons, image thumbnails | ✅ |
| YouTube | ✅ in Chrome (GPU on, up to 4K) · ✅ Safari with an audio output (Bluetooth or the BlackHole virtual device, `tools/build-blackhole.sh`) |
| Screen recording | ✅ |
| Hardware video decode (H.264/HEVC) | 🚧 in progress (off by default; video plays decoded on the CPU) — see [docs/STATUS.md](docs/STATUS.md) |
| Brightness keys | ⚠️ the Dell firmware stopped reporting the sun keys; F6/F7 work through `tools/xe-brilho daemon` |
| System Settings › Displays | ⚠️ may panic, change resolution with `tools/xe-res` |
| Shutdown / restart | ✅ fast (XE-IDLEBOUND; used to stall ~3 min and panic) |
| WindowServer hang | ⚠️ occasionally before login, and sometimes when changing the UI scale (reboot recovers); fix in testing (csbresync) |

Everything else about the laptop (Wi-Fi via itlwm/HeliPort, Bluetooth, keyboard/trackpad/touchscreen via VoodooI2C, NVMe, battery) is a regular OpenCore setup. Internal audio (SoundWire RT714/RT1316) and the camera do **not** work on macOS.

## How it works (one paragraph)

The EFI presents the GPU as Tiger Lake (`device-id 9A49`). Apple's TGL kexts load from `/Library/Extensions`, and **NootedGreen** patches them in memory: display engine 13 quirks, the GuC-less host scheduler, broken resolve paths, IOSurface layout changes in Tahoe, GPU frequency, and more ([docs/KERNEL-PATCHES.md](docs/KERNEL-PATCHES.md)). In user space, the TGL **Metal driver** is binary-patched and gets an adapter library (`libxedrv`) that converts Tahoe's private Metal structures to the old layout and works around hardware limits. The TGL **shader compiler** (IGC gen12lp, LLVM 3902) runs inside Tahoe's compiler service (LLVM 32023) through an API adapter and an LLVM bridge ([docs/METAL.md](docs/METAL.md)).

## Repository layout

```
EFI/                     OpenCore EFI for the XPS 13 Plus 9320 (serials removed — generate your own)
  OC/config.plist        accelerated config
  OC/config-install.plist  safe config for installing macOS (NootedGreen off, verbose)
src/NootedGreen/         the modified NootedGreen source (Lilu plugin)
build-nootedgreen.sh     builds src/NootedGreen with only the Command Line Tools
patches/                 diff of this fork against upstream NootedGreen 95f8189
macos/                   user-space stack: Metal driver patcher, libxedrv, shader compiler, installer
tools/                   xe-res (resolution/DPI), xe-brilho (brightness), xe-perfil (color profile), xe-toque (touch trail),
                         XeShim + xectl (register access), diagnostics/ (the test programs used to find bugs)
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
(acceleration off). BIOS: usual hackintosh settings (storage in AHCI/NVMe mode, not RAID/VMD; Secure Boot off).

**3. On the new macOS**, with the accelerated `config.plist` back in place:

```sh
xcode-select --install          # Command Line Tools, once
zsh install.sh                  # checks, downloads Apple's driver bundles via NootedGreen, builds, installs
```

Copy the freshly built `build/NootedGreen.kext` to `EFI/OC/Kexts/`, approve the extension if asked, reboot.
`zsh install.sh --check` only runs the checks; `zsh install.sh --uninstall` removes everything.

Other hardware (other Raptor Lake / Alder Lake laptops): the user-space part is the same, but you need your own
EFI (ACPI, USB map, input devices). The full manual procedure is in [docs/INSTALL.md](docs/INSTALL.md).

## Credits

- **NootedGreen** by StezzaPilot ([sgiammori/NootedGreen](https://github.com/sgiammori/NootedGreen)): the base of the kernel plugin and the source of the 16.0.32 driver bundles. Licensed under the *Thou Shalt Not Profit License 1.0*, which also covers the fork in `src/`.
- **WhateverXe** and **OpenCore Legacy Patcher fork** by b00t0x: reference for Gen12 on macOS.
- **IntelIGPU-Tahoe**: reference for running the TGL Metal driver on Tahoe.
- **acidanthera** (OpenCore, Lilu, WhateverGreen, VirtualSMC, MacKernelSDK), **OpenIntelWireless** (itlwm, IntelBluetoothFirmware), **VoodooI2C**, **USBToolBox**.
- Developed on one Dell XPS 13 Plus by its owner, pair-programming with Claude (Anthropic).

## License

[Thou Shalt Not Profit License 1.0](LICENSE), the license of NootedGreen, which this project derives from. Third-party
kexts in `EFI/` keep their own licenses, and no Apple binaries are included: see [NOTICE.md](NOTICE.md).
