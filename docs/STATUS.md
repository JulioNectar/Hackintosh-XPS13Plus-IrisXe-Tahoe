# Status (October 2026, macOS Tahoe 26.7.1 25G241)

## Works

- Metal desktop: WindowServer, Dock, Finder, Liquid Glass materials, Mission Control, animations at 60 Hz.
- 3840×2400 panel, HiDPI 1280–1920 pt (via `tools/xe-res`).
- GPU at ~1300 MHz under load (XE-GTFREQ requests 1500 MHz = RP0; `GT_PERF_LIMIT_REASONS` shows the PL4/IccMax package limit holding it at ~1300), RC6 when idle.
- HEIC/YUV images, wallpapers (PLANES2). App icons, thumbnails and image layers (libxedrv v15).
- Video: YouTube in **Chrome with GPU acceleration** (software decode on the CPU), including 4K. Screen recording works. Safari works once an audio output exists: the laptop has none on macOS, so install the BlackHole virtual device (`tools/build-blackhole.sh`) or use Bluetooth audio.
- Brightness through `xe-brilho` (XeShim).
- Fast shutdown/restart (XE-IDLEBOUND), trackpad and touchscreen on interrupts, multitouch gestures.

## Known problems

| Problem | Details | Workaround |
|---|---|---|
| WindowServer hang | occasionally before login and on some UI-scale changes: a context left pending by the CSB guard → stuck stamp → watchdog. Fix in testing (csbresync) | reboot |
| Slow shutdown / watchdog panic | fixed 2026-10-21 (XE-IDLEBOUND): shutdown no longer waits on a GPU that never goes idle | — |
| Panic at shutdown (rare) | "corrupt list" in `IOAccelOrphanedMemoryPool` (release path) | none yet |
| System Settings › Displays | can panic; mode switches sometimes go black until reboot | `tools/xe-res` |
| Colour profile changes | selecting another profile (e.g. ITU-709) in Displays caused three page-fault panics in GPU memory management (`newResourceWithClientBuffer`, `orphanIt`/`releaseDescriptors`, `IGHardwarePageTable::commitRange`) | keep the factory profile (`tools/xe-perfil padrao` restores it) |
| Brightness keys | since 2026-10-20 night the sun icons on the touch row stopped working: the EC raises events but the Dell SMM no longer reports brightness (no `Notify(LCD)`), so BrightnessKeys gets nothing. Not graphics-related (OpRegion/ASLE checked). Possibly firmware state; BIOS "Load defaults" untested | `xe-brilho daemon` with `~/.xe-brilho-teclas` binding F6/F7 (`down 97 800000` / `up 98 800000`) |
| Brightness range | with `ngreenblmax=48000` the macOS maximum is only 37 % of the panel PWM; the right value is `17777` (prepared, not yet shipped) | — |
| Trackpad / touchscreen | fixed 2026-10-20: GPIO/APIC interrupts instead of forced polling (the I2C bus used to get stuck, "Timeout waiting for bus not to be busy") | — |
| Hardware video decode | **H.264 works** (2026-10-21) with `-ngreengva` + the patched VA plugin (`macos/vadriver/build-vadriver.sh`, installed in `/Library/GPUBundles`): frames are bit-exact against software decode, for VideoToolbox clients outside the WebKit GPU-process sandbox (verified with `xe-vtdec` and in-process decoder tests). **Safari still decodes on the CPU**: its `com.apple.WebKit.GPU` sandbox denies `file-map-executable` for `/Library/GPUBundles`, so the plugin cannot load there. **HEVC**: the plugin fixes are in the same build (bit-exact in tests, I/P/B frames); it also needs the accelerator property `IOGVAHEVCDecode` (NootedGreen XE-GVAHEVC, pending a boot test). Hardware encode is untested | software decode (Safari; HEVC until XE-GVAHEVC) |
| E-cores | only the 4 P-cores (8 threads) run; booting with E-cores on hangs. Test plan prepared (CpuTopologyRebuild + HT patch) | E-cores off in BIOS |
| Internal audio / camera | SoundWire audio (RT714/RT1316) and the camera have no macOS driver | Bluetooth / USB audio, BlackHole for apps that need an output |
| Extended range 10-bit (`w30r` / BGR10_XR) | not supported by the 16.0.32 Metal driver | — |
| Small glitch on the lock-screen clock | cosmetic | — |
| Transition glitch | a brief wrong frame when going from the login screen to the desktop and during UI-scale changes (`xe-res`) | cosmetic |

How each of these was reached, and what was already tried, is in [HISTORY.md](HISTORY.md) and
[EXPERIMENTS.md](EXPERIMENTS.md).

## Not possible on this laptop

- Internal audio: SoundWire (RT714 codec, RT1316 amplifiers) behind the Intel SOF DSP; macOS has no driver. Use
  USB-C or Bluetooth audio.
- Camera (MIPI/IPU) and fingerprint reader.
