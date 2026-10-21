# Status (October 2026, macOS Tahoe 26.7.1 25G241)

## Works

- Metal desktop: WindowServer, Dock, Finder, Liquid Glass materials, Mission Control, animations at 60 Hz.
- 3840×2400 panel, HiDPI 1280–1920 pt (via `tools/xe-res`).
- GPU at ~1300 MHz under load (XE-GTFREQ requests 1500 MHz = RP0; `GT_PERF_LIMIT_REASONS` shows the PL4/IccMax package limit holding it at ~1300), RC6 when idle.
- HEIC/YUV images, wallpapers (PLANES2). App icons, thumbnails and image layers (libxedrv v15).
- Video: YouTube in **Chrome with GPU acceleration** (software decode on the CPU), including 4K. Screen recording works. Safari works once an audio output exists: the laptop has none on macOS, so install the BlackHole virtual device (`tools/build-blackhole.sh`) or use Bluetooth audio.
- Brightness through `xe-brilho` (XeShim).
- All 16 CPU threads (4 P-cores with HT + 8 E-cores).
- Fast shutdown/restart (XE-IDLEBOUND), trackpad and touchscreen on interrupts, multitouch gestures.

## Known problems

| Problem | Details | Workaround |
|---|---|---|
| WindowServer hang | occasionally before login and on some UI-scale changes: a context left pending by the CSB guard → stuck stamp → watchdog. Fix in testing (csbresync) | reboot |
| Slow shutdown / watchdog panic | fixed 2026-10-21 (XE-IDLEBOUND): shutdown no longer waits on a GPU that never goes idle | — |
| Panic at shutdown (rare) | "corrupt list" in `IOAccelOrphanedMemoryPool` (release path) | none yet |
| System Settings › Displays | can panic; mode switches sometimes go black until reboot | `tools/xe-res` |
| Colour profile changes | selecting another profile (e.g. ITU-709) in Displays caused three page-fault panics in GPU memory management (`newResourceWithClientBuffer`, `orphanIt`/`releaseDescriptors`, `IGHardwarePageTable::commitRange`) | keep the factory profile (`tools/xe-perfil padrao` restores it) |
| Brightness keys | the sun keys work on some boots and not on others: when they fail the EC still raises events but the Dell SMM does not report brightness (no `Notify(LCD)`), so BrightnessKeys gets nothing. Firmware state (e.g. not after entering the BIOS); not graphics-related (OpRegion/ASLE checked) | `xe-brilho daemon` with `~/.xe-brilho-teclas` binding F6/F7 (`down 97 800000` / `up 98 800000`) |
| Trackpad / touchscreen | fixed 2026-10-20: GPIO/APIC interrupts instead of forced polling (the I2C bus used to get stuck, "Timeout waiting for bus not to be busy") | — |
| Hardware video decode | **H.264 works** (2026-10-21) with `-ngreengva` + the patched VA plugin (`macos/vadriver/build-vadriver.sh`, installed in `/Library/GPUBundles`): frames are bit-exact against software decode, for VideoToolbox clients outside the WebKit GPU-process sandbox (verified with `xe-vtdec` and in-process decoder tests). **Safari still decodes on the CPU**: its `com.apple.WebKit.GPU` sandbox denies `file-map-executable` for `/Library/GPUBundles`, so the plugin cannot load there. **HEVC**: the plugin fixes are in the same build (bit-exact in tests, I/P/B frames); the accelerator property `IOGVAHEVCDecode` comes from NootedGreen XE-GVAHEVC; **confirmed on the GPU 2026-10-21** (30/30 frames, no hang). Hardware encode is untested | software decode (Safari) |
| E-cores | fixed 2026-10-21: all 16 threads run (see below), grouped by CpuTopologyRebuild | — |
| Internal audio / camera | SoundWire audio (RT714/RT1316) and the camera have no macOS driver | Bluetooth / USB audio, BlackHole for apps that need an output |
| Extended range 10-bit (`w30r` / BGR10_XR) | not supported by the 16.0.32 Metal driver | — |
| Lock-screen clock | fixed 2026-10-21 (libxedrv v17): the 16.0.32 driver aligned Stencil8 render targets to 4 KB instead of 64 KB; a stencil sharing a 64 KB block with a ≥32 bpp colour target in the same pass lost every odd 64-byte line, giving streaks in the Liquid Glass clock | — |
| Chrome UI broken when it starts with a GPU disk cache | toolbar icons missing, `border-radius` ignored, Google logo drawn as a bar. **Cause found (21/10):** not the cache itself but a race in the 16.0.32 Metal driver's blits: a `fillBuffer`/aligned copy (2D blit path) is not ordered before later 3D work in the same command buffer, so Dawn's zero-fill overwrote Graphite's static index buffer after the upload (it stayed all zero; per-launch race, almost always lost when the cache makes startup faster). **Fix: libxedrv v16** routes every buffer fill/copy through the 3D path ([METAL.md](METAL.md) §3 item 6); test Chrome profile: 12/12 launches correct with v16 vs 2/12 without. Needs installing (sudo) | until v16 is installed: `zsh tools/xe-chrome-limpa.sh` + `chflags uchg` on the cache folders (only lowers the odds) |
| Transition glitch | a brief wrong frame during UI-scale changes (`xe-res`). The login → desktop glitch is gone since libxedrv v16 | cosmetic |
| Boot progress bar | a short glitched segment on the Apple progress bar during boot (before WindowServer) | cosmetic, under investigation |

## E-cores

The i7-1360P is a 4P+8E part. With any E-core enabled the kernel froze right after `EXITBS`, before any verbose text,
even with a single E-core. Cause: on a hybrid CPU OpenCore's `ProvideCurrentCpuInfo` patches the kernel's
cores-per-package value to 128, assuming CPUID leaf 1 reports `logical_per_package = 128` (true for 6P+8E and desktop
parts). The 4P+8E Raptor Lake-P reports **64**, so xnu computes threads-per-core = 64/128 = 0 and divides by it in
`cpu_thread_alloc` (`initTopoParms`). Fix: the existing `Cpuid1Data` spoof also sets EBX[23:16] = 0x80
(`Cpuid1Data 55060A00 00008000 00000000 00000000`, `Cpuid1Mask FFFFFFFF 0000FF00 00000000 00000000`). Result:
`hw.ncpu = 16`. CpuTopologyRebuild 2.0.2 is enabled in its default mode (no HT kernel patch needed): the E-cores
become extra SMT threads of the P-cores (`hw.physicalcpu = 4`, `hw.logicalcpu = 16`), which the author recommends for
performance; System Information shows the real 12 cores. `ctrsmt=off` would expose the real topology but is slower.

How each of these was reached, and what was already tried, is in [HISTORY.md](HISTORY.md) and
[EXPERIMENTS.md](EXPERIMENTS.md).

## Not possible on this laptop

- Internal audio: SoundWire (RT714 codec, RT1316 amplifiers) behind the Intel SOF DSP; macOS has no driver. Use
  USB-C or Bluetooth audio.
- Camera (MIPI/IPU) and fingerprint reader.
