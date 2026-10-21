# Iris Xe acceleration on macOS Tahoe — Dell XPS 13 Plus (9320)

Hardware-accelerated graphics (**Metal, Core Animation, Liquid Glass**) for the **Intel Iris Xe of the i7-1360P (Raptor Lake-P, 8086:A7A0)** on **macOS Tahoe 26.7.1**, in an OpenCore hackintosh.

There is no Apple driver for Raptor Lake. This project runs **Apple's own driver for Tiger Lake Iris Xe** (GPUDriversIntel 16.0.32, written for Macs that never shipped) on the newer GPU. Both GPUs share the Xe-LP (Gen12) architecture. A modified **NootedGreen** Lilu plugin bridges the generation gap in the kernel, and a set of user-space patches bridges the gap between that 2020-era driver and Tahoe's Metal stack.

> 🇧🇷 Leia em português: [README.pt-BR.md](README.pt-BR.md)

> ⚠️ **Experimental.** Expect occasional GPU hangs and kernel panics. Keep a second bootable disk (or the USB installer) around. Read [docs/STATUS.md](docs/STATUS.md) before you start.

---

## What works

| Area | Status |
|---|---|
| Desktop, Dock, windows, Liquid Glass blur | ✅ GPU-accelerated through Metal |
| Native panel resolution (3840×2400) and HiDPI scaling (1280–1920 pt wide) | ✅ (switch with `tools/xe-res`, not System Settings) |
| GPU clock | ✅ 1100 MHz floor while busy (`ngreengtfreq`), idles in RC6 |
| HEIC / YUV images (wallpapers, photos) | ✅ |
| App icons, Finder icons, image thumbnails | ✅ |
| YouTube | ✅ in Chrome with GPU compositing off (see [docs/STATUS.md](docs/STATUS.md)) · ❌ Safari |
| Hardware video decode (H.264/HEVC) | ❌ disabled on purpose, decoded on the CPU |
| Brightness keys | ❌ use `tools/xe-brilho` |
| System Settings › Displays | ⚠️ may panic, change resolution with `tools/xe-res` |
| Long sessions | ⚠️ occasional GPU hang → WindowServer watchdog reboot |

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
tools/                   xe-res (resolution/DPI), xe-brilho (brightness), xe-perfil (color profile),
                         XeShim + xectl (register access), diagnostics/ (the test programs used to find bugs)
docs/                    install guide, patch catalogs, status, debugging notes
```

Documentation: [INSTALL](docs/INSTALL.md) · [STATUS](docs/STATUS.md) · [KERNEL-PATCHES](docs/KERNEL-PATCHES.md) ·
[METAL](docs/METAL.md) · [DEBUGGING](docs/DEBUGGING.md) · [HISTORY](docs/HISTORY.md) (how it was done, phase by phase) ·
[EXPERIMENTS](docs/EXPERIMENTS.md) (what failed and why) · [CATALOG](docs/CATALOG.md) (full patch and tool reference)

## Installing

The full guide is [docs/INSTALL.md](docs/INSTALL.md). In short:

1. **BIOS:** the usual hackintosh settings (storage in AHCI/NVMe mode rather than RAID/VMD, Secure Boot off). On this machine the E-cores are also disabled, so macOS sees 4 cores / 8 threads.
2. **EFI:** copy `EFI/` to a USB stick or the internal ESP. Generate SMBIOS (MacBookPro16,2) serials with GenSMBIOS. **Boot the macOS installer with `config-install.plist`** (rename it to `config.plist`).
3. **Install macOS Tahoe 26.7.1** (build 25G241 — the user-space patches are build-specific).
4. Install the **Command Line Tools** (`xcode-select --install`).
5. Get Apple's 16.0.32 driver bundles: the `sle_Internal/le` and `sle_Internal/sle` folders of the [NootedGreen](https://github.com/sgiammori/NootedGreen) project.
6. `zsh build-nootedgreen.sh` and put `build/NootedGreen.kext` in `EFI/OC/Kexts`.
7. `zsh macos/build-all.sh <sle>` then `sudo zsh macos/install-macos.sh <sle> <le>`, approve the extension, and reboot with the accelerated `config.plist`.

## Credits

- **NootedGreen** by StezzaPilot ([sgiammori/NootedGreen](https://github.com/sgiammori/NootedGreen)): the base of the kernel plugin and the source of the 16.0.32 driver bundles. Licensed under the *Thou Shalt Not Profit License 1.0*, which also covers the fork in `src/`.
- **WhateverXe** and **OpenCore Legacy Patcher fork** by b00t0x: reference for Gen12 on macOS.
- **IntelIGPU-Tahoe**: reference for running the TGL Metal driver on Tahoe.
- **acidanthera** (OpenCore, Lilu, WhateverGreen, VirtualSMC, MacKernelSDK), **OpenIntelWireless** (itlwm, IntelBluetoothFirmware), **VoodooI2C**, **USBToolBox**.
- Developed on one Dell XPS 13 Plus by its owner, pair-programming with Claude (Anthropic).

## License

- `src/NootedGreen` and `patches/`: Thou Shalt Not Profit License 1.0 (see `src/LICENSE-NootedGreen`).
- Everything else written for this project (`macos/`, `tools/`, `docs/`, scripts): same license, for consistency.
- Third-party kexts in `EFI/` keep their own licenses.
- **No Apple binaries are included.** You need your own copy of the 16.0.32 driver bundles.
