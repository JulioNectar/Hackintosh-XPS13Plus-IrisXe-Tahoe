# Changelog

Every change is tested on the real XPS 13 Plus 9320 before it is marked **confirmed**. Status values:
**confirmed** (works on hardware), **testing** (built and installed, being evaluated), **reverted**.
The full story behind each item is in [docs/HISTORY.md](docs/HISTORY.md).

## Unreleased

| Date | Change | Status |
|---|---|---|
| 2026-10-21 | **libxedrv v16**: buffer fills and aligned buffer copies no longer use the driver's 2D blitter path, which is not ordered against later 3D work in the same command buffer on this stack. Fixes Chrome (Graphite/Dawn) drawing with a zeroed index buffer (missing icons, logo as a bar), which looked cache-related only because a warm start loses the race; also affects any Metal app (stress test 9/227 → 0 errors). `XE_FILL_NATIVO=1` / `XE_COPIA_NATIVA=1` turn it off | confirmed |
| 2026-10-21 | CpuTopologyRebuild 2.0.2 enabled (default mode, no HT patch): 4 P-cores × 4 threads, System Information shows 12 cores | confirmed |
| 2026-10-21 | `ngreenblmax=17777` in the shipped config: the macOS brightness maximum now drives the panel PWM to ~100 % (was 37 % with 48000) | confirmed |
| 2026-10-21 | HEVC hardware decode on the GPU (NootedGreen XE-GVAHEVC sets `IOGVAHEVCDecode`=2 with `-ngreengva`; plugin HCP fixes); `tools/xe-chrome-limpa.sh` for the stale Chrome GPU-cache UI breakage | confirmed |
| 2026-10-21 | **E-cores work: 16 threads.** Any E-core froze the kernel right after EXITBS: OpenCore ProvideCurrentCpuInfo sets cores-per-package to 128, but the 4P+8E i7-1360P reports logical_per_package 64 → threads-per-core 0 → divide by zero in `cpu_thread_alloc`. `Cpuid1Data`/`Cpuid1Mask` now also force EBX[23:16] = 0x80 | confirmed |
| 2026-10-21 | `tools/xe-toque` 2.0: one-finger scroll with momentum, hold = right click, edge swipes (Notification Center, Mission Control), optional 2–5 finger handling, ripple/trail, magnifier, pen, touch mode; touchscreen events told apart by the CGEvent sender ID (field 87) | testing |
| 2026-10-21 | **H.264 hardware decode works** (VDBOX, bit-exact against software decode; with `-ngreengva`). `macos/vadriver/build-vadriver.sh` now builds the whole plugin from the stock 16.0.32 bundle, checking every patched byte. Four causes after the decoder could be created: the capability-node **content layout** (Tahoe adds a field at +0x20: `patch_nodelayout.py`, also answers driver parameter 14); the CodecHAL sideband **payload header** was never committed (`je` at 0x64a15), so the kernel took the first relocation as an indirect-batch handle and dropped every decode batch (`patch_payloadhdr.py`); the SURFACE_STATE **pitch relocation delta** gave pitch-1 (`patch_pitchdelta.py`); Tahoe's H.264 **SPS structure has 8 extra bytes** before `level_idc`, so MFX_AVC_IMG_STATE got chroma 4:4:4/MBAFF/wrong frame_num widths → garbage and a VCS0 hang (`patch_spsshift.py`). Safari still decodes on the CPU: the `com.apple.WebKit.GPU` sandbox denies `file-map-executable` for `/Library/GPUBundles`. The xe-vatrace tracer is now optional (`--trace`) | confirmed |
| 2026-10-21 | HEVC hardware decode, plugin side: the stock HCP surface-state builder never adds HCP_SURFACE_STATE to the batch (`patch_hcpsurface.py`) and Tahoe AppleGVA never sets LastSliceOfPic (`patch_hevclastslice.py`); 10 frames (I + B) bit-exact in in-process tests. Still needs the accelerator property `IOGVAHEVCDecode` = "2" (XE-GVAHEVC in NootedGreen, prepared) | testing |
| 2026-10-21 | **XE-RCPOLICY**: IntelAccelerator+0x1474 (`renderCompression` display/IOSurface policy) forced to 0 at start and in the `callPlatformFunction("renderCompression")` setter, so flips always take the "render compression off" path (AUX is NONE and the resolves are stubbed here). The flag stays 0 across `xe-res` mode switches; a small cosmetic glitch during the switch. `-ngreennorc` disables. From WhateverXe (jkbuha) | confirmed |
| 2026-10-21 | `tools/xe-toque`: fading finger trail for the touchscreen only (reads the digitizer via IOHIDManager, up to 10 fingers; trackpad ignored) | confirmed |
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
