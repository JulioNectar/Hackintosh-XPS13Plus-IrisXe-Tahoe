# Status (October 2026, macOS Tahoe 26.7.1 25G241)

## Works

- Metal desktop: WindowServer, Dock, Finder, Liquid Glass materials, Mission Control, animations at 60 Hz.
- 3840×2400 panel, HiDPI 1280–1920 pt (via `tools/xe-res`).
- GPU at 1100 MHz under load (XE-GTFREQ), RC6 when idle.
- HEIC/YUV images, wallpapers (PLANES2). App icons, thumbnails and image layers (libxedrv v15).
- Video: YouTube in **Chrome with GPU acceleration** (software decode on the CPU), including 4K. Screen recording works. Safari works once an audio output exists: the laptop has none on macOS, so install the BlackHole virtual device (`tools/build-blackhole.sh`) or use Bluetooth audio.
- Brightness through `xe-brilho` (XeShim).

## Known problems

| Problem | Details | Workaround |
|---|---|---|
| WindowServer hang | occasionally before login and on some UI-scale changes: a context left pending by the CSB guard → stuck stamp → watchdog. Fix in testing (csbresync) | reboot |
| Slow shutdown / watchdog panic | fixed 2026-10-21 (XE-IDLEBOUND): shutdown no longer waits on a GPU that never goes idle | — |
| Panic at shutdown | "corrupt list" in `IOAccelOrphanedMemoryPool` (release path) | none yet |
| Safari video | VideoToolbox decode with an explicit output format → `-6680` | use Chrome (no GPU) |
| Chrome with GPU on | blank windows (ANGLE/Metal) | `--disable-gpu --disable-gpu-compositing` |
| System Settings › Displays | can panic; mode switches sometimes go black until reboot | `tools/xe-res` |
| Colour profile changes | selecting another profile (e.g. ITU-709) in Displays caused three page-fault panics in GPU memory management (`newResourceWithClientBuffer`, `orphanIt`/`releaseDescriptors`, `IGHardwarePageTable::commitRange`) | keep the factory profile (`tools/xe-perfil padrao` restores it) |
| Brightness keys | the TGL framebuffer never writes the PWM duty | `xe-brilho` |
| Trackpad / touchscreen | fixed 2026-10-20: GPIO/APIC interrupts instead of forced polling (the I2C bus used to get stuck, "Timeout waiting for bus not to be busy") | — |
| Hardware video decode/encode | off by default (XE-NOGVA); bring-up in progress with `-ngreengva` + patched VA plugin (`macos/vadriver/`): AppleGVA now loads and initialises the TGL plugin, contexts and bitstream buffers are created; it stops at the NV12 output surface | software decode |
| Extended range 10-bit (`w30r` / BGR10_XR) | not supported by the 16.0.32 Metal driver | — |
| Small glitch on the lock-screen clock | cosmetic | — |

How each of these was reached, and what was already tried, is in [HISTORY.md](HISTORY.md) and
[EXPERIMENTS.md](EXPERIMENTS.md).

## Not possible on this laptop

- Internal audio: SoundWire (RT714 codec, RT1316 amplifiers) behind the Intel SOF DSP; macOS has no driver. Use
  USB-C or Bluetooth audio.
- Camera (MIPI/IPU) and fingerprint reader.
