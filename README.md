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
| Native panel resolution (3840×2400) and HiDPI scaling (1280–1920 pt wide) | ✅ switch with `tools/xe-res`, not System Settings (a brief glitch during the switch is cosmetic) |
| GPU clock | ✅ ~1300 MHz under load (requests RP0 1500 MHz; the CPU package power limit PL4/IccMax caps it at ~1300), idles in RC6 |
| CPU | ✅ **all 16 threads (4 P-cores with HT + 8 E-cores)** — the E-cores used to hang the boot; fixed with one byte in `Cpuid1Data` (see [STATUS](docs/STATUS.md)). Topology is still flat (16 equal cores); CpuTopologyRebuild tuning is next |
| HEIC / YUV images (wallpapers, photos) | ✅ |
| App icons, Finder icons, image thumbnails | ✅ (layers in the 10-bit extended-range format `w30r`/`BGR10_XR` still don't render) |
| Hardware video decode | ✅ **H.264 on the GPU** (bit-exact vs. software) for apps that use VideoToolbox: QuickTime, Photos, video editors, conversions. Needs `-ngreengva` and the patched VA plugin (`macos/vadriver/build-vadriver.sh`, installed by hand for now) · ✅ **HEVC** too (NootedGreen XE-GVAHEVC) · ⚠️ Safari decodes on the CPU: its WebKit GPU process may not load drivers from `/Library/GPUBundles` |
| YouTube | ✅ Chrome with GPU compositing, up to 4K (VP9/AV1 decoded on the CPU) · ✅ Safari up to 1080p H.264 (needs an audio output: Bluetooth or the BlackHole virtual device, `tools/build-blackhole.sh`) |
| Screen recording | ✅ |
| Touchscreen | ✅ up to 10 fingers via VoodooI2C; `tools/xe-toque` adds one-finger scroll, hold-for-right-click, edge swipes, pen, magnifier and a touch mode |
| Trackpad, keyboard | ✅ trackpad and touchscreen on GPIO/APIC interrupts (no polling freezes) |
| Brightness | ✅ `tools/xe-brilho` (slider works; macOS maximum is ~37 % of the panel until `ngreenblmax=17777` ships) · ⚠️ the sun keys depend on a Dell firmware state that changes from boot to boot; F6/F7 always work through `xe-brilho daemon` |
| Shutdown / restart | ✅ fast (XE-IDLEBOUND; used to stall ~3 min and panic) |
| System Settings › Displays, colour profiles | ⚠️ may panic: change resolution with `tools/xe-res` and keep the factory colour profile |
| WindowServer hang | ⚠️ rare, before login or on a UI-scale change (reboot recovers); fix in testing (csbresync) |

Everything else about the laptop (Wi-Fi via itlwm/HeliPort, Bluetooth, NVMe, battery) is a regular OpenCore setup. Internal audio (SoundWire RT714/RT1316) and the camera do **not** work on macOS.

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
macos/                   user-space stack: Metal driver patcher, libxedrv, shader compiler, VA (video) plugin patcher, installer
tools/                   xe-res (resolution/DPI), xe-brilho (brightness), xe-perfil (color profile), xe-toque (touchscreen gestures),
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
(acceleration off). BIOS: usual hackintosh settings (storage in AHCI/NVMe mode, not RAID/VMD; Secure Boot off). The E-cores can stay enabled.

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
- **WhateverXe** and **OpenCore Legacy Patcher fork** by b00t0x, and jkbuha's WhateverXe work: reference for Gen12 on macOS (including the Tahoe VA-plugin payload-header fix).
- **BlackHole** by Existential Audio (virtual audio output, GPL-3.0), built by `tools/build-blackhole.sh`.
- **IntelIGPU-Tahoe**: reference for running the TGL Metal driver on Tahoe.
- **acidanthera** (OpenCore, Lilu, WhateverGreen, VirtualSMC, MacKernelSDK), **OpenIntelWireless** (itlwm, IntelBluetoothFirmware), **VoodooI2C**, **USBToolBox**.
- Developed on one Dell XPS 13 Plus by its owner, pair-programming with Claude (Anthropic).

## License

[Thou Shalt Not Profit License 1.0](LICENSE), the license of NootedGreen, which this project derives from. Third-party
kexts in `EFI/` keep their own licenses, and no Apple binaries are included: see [NOTICE.md](NOTICE.md).
