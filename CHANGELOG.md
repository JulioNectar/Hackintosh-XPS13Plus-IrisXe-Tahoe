# Changelog

Every change is tested on the real XPS 13 Plus 9320 before it is marked **confirmed**. Status values:
**confirmed** (works on hardware), **testing** (built and installed, being evaluated), **reverted**.
The full story behind each item is in [docs/HISTORY.md](docs/HISTORY.md).

## Unreleased

| Date | Change | Status |
|---|---|---|
| 2026-10-21 | **XE-IDLEBOUND**: shutdown/restart no longer stalls. During shutdown WindowServer called `IGScheduler5::waitForGpuIdle` (up to 25000 × 100 µs) several times on a GPU with stuck stamps, until the watchdog panicked (~3.5 min). Now, once a wait runs ≥ 2 s, the waits of the next 30 s return at once. Same machine: ~11 s, no panic. `-ngreennoidlebound` disables | confirmed |
| 2026-10-21 | **csbresync** (for the WindowServer hang before login / on UI-scale changes): CSB guard dropped a real active→idle event when a very short context finished before its idle→active bookkeeping, leaving the context pending forever. Before dropping, if the pending slot holds the same context, run the original `csbProcessIdleToActive` first. `-ngreennocsbresync` disables | testing |
| 2026-10-21 | **XE-BLITALIGN v2**: only misaligned large copies go to the copy engine; aligned ones use the 3D blit again. Fixes the missing Chrome toolbar/page icons caused by v1 (confirmed by a boot with the patch off); screen recording and video conversions still pass | confirmed |
| 2026-10-21 | AppleGVA `hwgva-id` is now the real board-id (was iMacPro1,1 from NootedRed, which makes AppleGVA accept only AMD plugins) and XE-GVAID (IOVARendererID 0x1080080 with `-ngreengva`) | testing |
| 2026-10-21 | Hardware decode bring-up (WIP, off by default): `macos/vadriver/` patches the TGL VA plugin (device-id 9A49, VAME name, Tahoe bitstream request, unknown driver parameter, capability-node layout 0x430→0x438); install in `/Library/GPUBundles`. AppleGVA now creates the decoder up to the NV12 output surface. Tracer: `tools/diagnostics/xe-vatrace.c` | in progress |
| 2026-10-20 | **Safari video works**: it needs an audio output device, and this laptop has none on macOS (SoundWire audio unsupported). `tools/build-blackhole.sh` builds the BlackHole virtual output (GPL-3.0) without Xcode; with it (or Bluetooth headphones) YouTube plays in Safari | confirmed |
| 2026-10-20 | **XE-BLITALIGN + XE-PAGESYNC**: video surfaces from CoreVideo now work on the GPU. Screen recording is no longer green, all VTPixelTransfer conversions carry content (12/12, was 3/12), YouTube 4K in Chrome no longer flickers. Root causes: misaligned plane copies went to the broken 3D blit, and render work did not wait for the copy engine. Test tools: `tools/diagnostics/gpu-tests/` | confirmed |
| 2026-10-20 | XE-GTFREQ floor raised to 1500 MHz (RP0); under load the GPU runs at ~1300 MHz, capped by the PL4/IccMax package power limit (`GT_PERF_LIMIT_REASONS` bit 8), not by the driver | confirmed |
| 2026-10-20 | CpuTopologyRebuild 2.0.2 (with E-cores off) stops the boot before the progress bar; E-cores on without it boots the kernel but WindowServer hangs | reverted |
| 2026-10-20 | **Input without polling**: trackpad on a GPIO interrupt, touchscreen on an APIC interrupt; `-vi2c-force-polling` removed. Fixes the intermittent keyboard/trackpad/touchscreen freezes (I2C bus stuck while polling). Three pieces: VoodooGPIO built from current source with Tiger Lake-LP support (`build-voodoogpio.sh`) plus an upstream bug fix (`patches/voodoogpio-tigerlakelp-start.diff`); `SSDT-XPS-I2C` now returns the firmware's own resources (`acpi/SSDT-XPS-I2C.dsl`) | confirmed |
| 2026-10-20 | **XE-FBDPMS**: display sleep only turns the backlight off; the pipe/transcoder/DDI path to the panel is never torn down, so the display wakes again (the TGL framebuffer cannot retrain the eDP link on display 13). Hooks `AppleIntelFramebuffer::setFramebufferPowerState`; `-ngreenfbdpms` disables | confirmed |
| 2026-10-20 | XE-NODPMS (register-write filter for the same problem): broke the framebuffer's own mode sets (spinlock timeout at boot); kept as opt-in `-ngreennodpms` only | reverted |
| 2026-10-20 | `tools/xe-sleep-diag.sh`, `tools/xe-modeset-diag.sh`: display register snapshots around display sleep / mode changes (need XeShim) | — |
| 2026-10-20 | **MTL18**: `mtl17-patches.json` no longer rewrites `0x7ff77`/`0x80253`. The v11 L3 patch had turned two `movb` instructions in `IntelState::IntelState()` into `rolb`/`rorb` | testing |
| 2026-10-20 | `make-efi.sh` (EFI with your own SMBIOS) and `install.sh` (check, fetch, build, install, uninstall) | confirmed (build path) |
| 2026-10-20 | Deep documentation: HISTORY, EXPERIMENTS, CATALOG | — |

## v0.1 — 2026-10-20 (first public snapshot)

| Area | Change | Status |
|---|---|---|
| Kernel | XE-GTFREQ: GPU frequency floor (1100 MHz) — the GPU was stuck at 100 MHz | confirmed |
| Kernel | PLANES2: IOSurface plane count read at `+0x98` (Tahoe) → HEIC/YUV no longer green | confirmed |
| Kernel | XE-NOGVA: hide hardware video decode → VideoToolbox uses software decode | confirmed (Chrome) |
| Kernel | XE-NOORPHAN, XE-RESFREE, XE-NODEPTH: memory/resolve panics | confirmed |
| Metal | libxedrv v15: copy zero-copy textures at unaligned offsets → app icons and image layers visible | confirmed |
| Repo | reproducible MTL17 patcher and shader-compiler build; EFI without serials | confirmed |

See [docs/STATUS.md](docs/STATUS.md) for what works and the known problems.
