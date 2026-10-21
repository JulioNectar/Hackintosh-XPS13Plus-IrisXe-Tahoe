# Kernel patches (NootedGreen fork)

All patches live in `src/NootedGreen/kern_gen11.cpp` and `kern_green.cpp`. They target Apple's
**AppleIntelTGLGraphics 16.0.32** (kext bundle IDs renamed to `com.xxxxx.*`) and Tahoe's **IOAcceleratorFamily2**
(build 25G241). Byte patches are `LookupPatchPlus` find/replace pairs, so they fail safely ("NAO encontrado" in the
log) on a different driver build.

The fixes below are active with the boot-args shipped in `EFI/OC/config.plist`:

```
IGLogLevel=3 msgbuf=400000 -ngreentglwithgfx -allow3d -disablegfxfirmware ngreen-dmc=adlp
-ngreenv189 -ngreenkeepefi -ngreenxefix -ngreenfixbootmode amfi_get_out_of_my_way=1 ngreenSched=4 ngreenV142=3
ngreenblmax=48000
```

`-ngreenxefix` turns on the Raptor Lake / Tahoe fix set. Lilu truncates boot-args at about 228 characters, so keep the
line short.

**More detail.** This page lists the patches that matter for a working system. The full list — every `XE-*` patch
including diagnostics and opt-in experiments, the upstream V-numbered patches, byte patterns and all boot-args — is in
[CATALOG.md](CATALOG.md). **Version history:** the order in which these patches were found, with the evidence for each,
is in [HISTORY.md](HISTORY.md) (phases 1–2 display, 9–16 scheduler/memory, 17–18 power and video), and the patches that
were tried and dropped are in [EXPERIMENTS.md](EXPERIMENTS.md).

## Display (display engine 13)

| Patch | Problem | Fix | Disable |
|---|---|---|---|
| DPT off / DBUF | Display 13 reads tiled planes through the DPT, and the TGL driver powers off DBUF slice S1 → noise on screen | `CHICKEN_MISC_2` bit 30, keep `DBUF_CTL_S1` power request | — |
| GAMMA | TGL programs 10-bit post-CSC gamma in a layout display 13 reads wrong | `GAMMA_MODE_A = 0` (bypass) | — |
| XE-FMT | `performTransaction` reads the frame format from `IOSurface+0xa4`; Tahoe moved it (now `+0x80`) → plane programmed as YUV 4:2:2 (green screen) | force `'BGRA'` | — |
| fixbootmode | WindowServer aborted (no display mode / timings at start) | inject detailed timings early | `-ngreenfixbootmode` is opt-in |
| XE-BLR | Brightness topped out at ~50 % | keep the hardware PWM period, rescale duty (`ngreenblmax`) | `-ngreennoblr` |

## Command submission (host scheduler, no GuC)

The GuC firmware is disabled (`-disablegfxfirmware`), so the driver runs its host scheduler (`ngreenSched=4`).

| Patch | Problem | Fix | Disable |
|---|---|---|---|
| CSB guard | "EL[0] was invalidated" panics: context-switch events for a context that was already replaced | skip CSB events whose context ID doesn't match | `-ngreennocsbguard` |
| csbresync | WindowServer hang: CSB guard dropped a real active→idle event for a context still in the pending slot (finished before its idle→active was processed) → stuck stamp → watchdog | run the original `csbProcessIdleToActive` for that context, then let the event through | `-ngreennocsbresync` |
| XE-IDLEBOUND | slow shutdown / watchdog panic: `IGScheduler5::waitForGpuIdle` waits up to 2.5 s each time on a GPU that never idles | after one wait ≥ 2 s, skip the waits of the next 30 s | `-ngreennoidlebound` |
| SAMETAIL | the same context resubmitted with the same tail confused the execlist state | mark it as a lite restore | `-ngreennosametail` |
| ELSQ idle wait | submissions while the ELSP was busy | wait up to 5 ms for idle | `-ngreennoelsqidle` |
| XE-NOPOSH | RPL has no POSH pipe | clear the POSH bit in `MI_BATCH_BUFFER_START` | opt-in `-ngreennoposh` |
| QUIET LOG | log flood and a Lilu log deadlock in interrupt context | events only with `-ngreenevlog` | — |

## Memory and resources

| Patch | Problem | Fix | Disable |
|---|---|---|---|
| BLITSCRATCH2 | Blit3D kernels read a zeroed scratch area (Tahoe changed `lockForCPUAccess`) | write the scratch through the GPU-visible mapping | `-ngreennoblitscratch` |
| XE-POOL2 | small command buffer pools | larger buffers for small pools | `-ngreennopoolfix` |
| PAGEON GUARD | GP faults on non-canonical pointers when pages are wired back | validate the pointer first | `-ngreennopageonguard` |
| XE-RESFREE | `kfree` panic in `IGAccelResource::free` (per-plane table with garbage pointers) | neutralize unmapped pointers before freeing | `-ngreennoresfree` |
| XE-NOORPHAN | "corrupt list" panic in `IOAccelSysMemory::unorphanIt` (orphaned memory reuse) | `IOAccelOrphanedMemoryPool::getMemory` returns nothing → always allocate | `-ngreenorphanreuse` |
| **PLANES2** | Tahoe moved `IOSurface` plane count from `+0xb0` to `+0x98`; the old workaround treated every surface as 1-plane → **YUV/HEIC chroma read as zero: green wallpapers, blank video frames** | read `[IOSurface+0x98]` | `-ngreenplanesxor` (old behaviour) |

## Render compression (disabled)

Gen12 render compression needs the AUX translation table, which the TGL driver doesn't set up on RPL.

| Patch | Fix | Disable |
|---|---|---|
| XE-NOCCS | `RenderCompressionMode=0`, `AllowIOSurfaceRenderCompression=0` | — |
| XE-NOAUX | `fillSurfaceStateAuxInfo` always writes AUX = none | `-ngreenccs` |
| XE-NOCCS3 | `IntelAccelerator::submitCCSResolve` returns immediately (wrote into a pool buffer Tahoe doesn't have → page fault) | — |
| XE-NODEPTH | same for `IntelAccelerator::submitDepthResolve` (`resolve_hiz_g7` page fault) | `-ngreendepthresolve` |

## Power and video

| Patch | Problem | Fix | Disable |
|---|---|---|---|
| **XE-GVAID** | with `-ngreengva` the TGL personality advertises IOVARendererID 0x1080040, but the TGL VA plugin identifies as 0x1080080 (the ICL id) and AppleGVA only knows that one | set IOVARendererID = 0x1080080 when the GVA keys are kept | (only with `-ngreengva`) |
| **gvaboard** | NootedGreen inherited NootedRed's iMacPro1,1 `hwgva-id` for AppleGVA; on that board-id AppleGVA only accepts AMD plugins | `hwgva-id` = the real board-id (Mac-5F9802EFE386AA28, MacBookPro16,2) | — |
| **XE-BLITALIGN** | IOSurface planes whose offset is not a multiple of 64 (CoreVideo's 420v uses 32 / 2073632) read as zeros and lost writes: the copy between the IOSurface and the driver's device cache was routed to the 3D blit, which does not work here | `blit2d_common_supported`: address mask `0x3f`→`0x0f` (source and destination); large non-fast linear copies go to `blt2d_source_copy` (copy engine) instead of 3D **only when the source or destination is not 64-byte aligned** (v2; v1 rerouted every large copy and made Chrome icons disappear) | `-ngreennoblitalign` |
| **XE-PAGESYNC** | `pageon`/`pageoff` copies run on the copy engine and the render/compute work did not wait for them (our `barrierSubmission` replacement skips Apple's cross-engine semaphore) → half-copied textures, mixed write-backs, flicker | after `IGAccelResource::pageon`/`pageoff`, CPU-wait the copy's event with `IOAccelEventMachineFast2::finishEvent` | `-ngreennopagesync` |
| **XE-FBDPMS** | display sleep tore down pipe A, TRANS_DDI_FUNC_CTL_A, DDI_BUF_CTL_A and the scaler; wake re-enabled the panel and plane but never the eDP link → display stayed dark | `setFramebufferPowerState` on FB0: Wake→Doze/Sleep only clears `BLC_CTL` bit 31; back to Wake only sets it | `-ngreenfbdpms` |
| **XE-GTFREQ** | without GuC the frequency policy object returns 0 → GPU stuck at 100 MHz (RPn) | `setGTFrequencyMMIO` never requests less than `ngreengtfreq` MHz (default 1500 = RP0; under load the PCU grants ~1300 because of the PL4/IccMax package limit). Idle GPU still enters RC6 | `ngreengtfreq=0` |
| **XE-NOGVA** | the TGL personality advertises hardware video decode (`IOGVACodec`, HEVC caps). On RPL the decoder can't be created and decode sessions that request an output format fail with `-6680` → YouTube "an error occurred" | remove the `IOGVA*` properties → VideoToolbox uses software decode | `-ngreengva` |

## Tahoe facts used by these patches (25G241)

- `IOSurface::getPlaneCount()` → `[rdi+0x98]`, `IOSurface::getPixelFormat()` → `[rdi+0x80]`.
- `IOAccelResource2`: parent resource `+0xe0`, plane index `+0xf8`, plane offset `+0xfc`.
- Symbols of kexts inside the kernel collection can be listed with `tools/diagnostics/kcsyms.py` and disassembled
  with `kcdis.py` (needs `pip install --user capstone`).
