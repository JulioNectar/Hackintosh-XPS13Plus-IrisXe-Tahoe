# Status (October 2026, macOS Tahoe 26.7.1 25G241)

## Works

- Metal desktop: WindowServer, Dock, Finder, Liquid Glass materials, Mission Control, animations at 60 Hz.
- 3840×2400 panel, HiDPI 1280–1920 pt (via `tools/xe-res`).
- GPU at 1100 MHz under load (XE-GTFREQ), RC6 when idle.
- HEIC/YUV images, wallpapers (PLANES2). App icons, thumbnails and image layers (libxedrv v15).
- Video: YouTube in **Chrome launched with `--disable-gpu --disable-gpu-compositing`** (software decode).
- Brightness through `xe-brilho` (XeShim).

## Known problems

| Problem | Details | Workaround |
|---|---|---|
| GPU hang in long sessions | after ~1 h or heavy browser work: gpuRestart → WindowServer watchdog → reboot | save often |
| Panic at shutdown | "corrupt list" in `IOAccelOrphanedMemoryPool` (release path) | none yet |
| Safari video | VideoToolbox decode with an explicit output format → `-6680` | use Chrome (no GPU) |
| Chrome with GPU on | blank windows (ANGLE/Metal) | `--disable-gpu --disable-gpu-compositing` |
| System Settings › Displays | can panic; mode switches sometimes go black until reboot | `tools/xe-res` |
| Colour profile changes | selecting another profile (e.g. ITU-709) in Displays caused three page-fault panics in GPU memory management (`newResourceWithClientBuffer`, `orphanIt`/`releaseDescriptors`, `IGHardwarePageTable::commitRange`) | keep the factory profile (`tools/xe-perfil padrao` restores it) |
| Brightness keys | the TGL framebuffer never writes the PWM duty | `xe-brilho` |
| Trackpad / touchscreen | fixed 2026-10-20: GPIO/APIC interrupts instead of forced polling (the I2C bus used to get stuck, "Timeout waiting for bus not to be busy") | — |
| Hardware video decode/encode | off (XE-NOGVA) | software decode |
| Extended range 10-bit (`w30r` / BGR10_XR) | not supported by the 16.0.32 Metal driver | — |
| Small glitch on the lock-screen clock | cosmetic | — |

How each of these was reached, and what was already tried, is in [HISTORY.md](HISTORY.md) and
[EXPERIMENTS.md](EXPERIMENTS.md).

## Not possible on this laptop

- Internal audio: SoundWire (RT714 codec, RT1316 amplifiers) behind the Intel SOF DSP; macOS has no driver. Use
  USB-C or Bluetooth audio.
- Camera (MIPI/IPU) and fingerprint reader.
