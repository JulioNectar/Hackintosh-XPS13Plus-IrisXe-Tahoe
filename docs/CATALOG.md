# Catalog

Complete reference of what this project changed, taken from the source code and the build artefacts. For the story
behind each item see [HISTORY.md](HISTORY.md); abandoned approaches are collected in [EXPERIMENTS.md](EXPERIMENTS.md).

Scope: the forked NootedGreen kernel plugin (`src/NootedGreen`), compared against upstream NootedGreen commit `95f8189` ("V212 add"), plus the user-space Metal-driver patches, libxedrv, the LLVM bridge, install scripts and diagnostic tools.

Target hardware: Intel Raptor Lake-P iGPU `8086:A7A0`, driven through Apple's Tiger Lake driver `AppleIntelTGLGraphics` 16.0.32 on macOS Tahoe 26.7.1. The plugin detects real TGL by CPU model `0x8C`/`0x8D` (`isRealTGL`). Everything else runs the "spoofed / non-real-TGL" paths.

Sections 4.4 and 5 describe the original development tree (`build/…`, `nooted/tahoe/…`, `tools/…`) that this
repository was cut from. Only part of it ships here: see `macos/`, `tools/` and `tools/diagnostics/`.

Contents:
1. Fork-only kernel features (`xe/*`, `XE-*`)
2. Upstream V-numbered patches, routes, byte patches and boot-args (kern_gen11.cpp, kern_green.cpp, DYLDPatches.cpp, other modules)
3. Consolidated boot-arg reference
4. User-space history: Metal driver patches v1..v18, libxedrv, LLVM bridge, scripts
5. Diagnostic and test tools
6. Findings from the code audit (comments vs. code, dead paths, latent bugs)

---

## 1. Fork-only kernel features (`xe/*` and `XE-*`)

Everything in this section is **absent from upstream commit 95f8189**. It was found by diffing the fork against `git show HEAD:NootedGreen/<file>`.

| File | Upstream lines | Fork lines | Change |
|---|---|---|---|
| `kern_gen11.cpp` | 9938 | 11202 | about 1260 lines added: all `xe/*` blocks below |
| `kern_gen11.hpp` | 2039 | 2050 | declarations for `getCurrentDisplayMode`, `IGAccelResourcefree`, `wrapSetGTFrequencyMMIO`, `xeLrcaRepair`, and a public `xeForceWakePub()` |
| `kern_green.cpp` | 634 | 787 | IOAcceleratorFamily2 hooks: command-buffer pool, blit scratch lock, orphan pool, display-pipe counters |
| `kern_start.cpp` | 70 | 70 | maximum kernel version changed from `KernelVersion::Sequoia` to `KernelVersion::Tahoe`; without it the plugin does not load on macOS 26 |
| all other files | – | – | identical to upstream |

The upstream checkout's working tree also has two small local edits, kept as `nooted/tahoe/nootedgreen-tahoe-keepefi.patch` (2026-10-16). These were the first Tahoe patches, and the fork contains both:
- `-ngreenkeepefi`;
- the pipe-width linear stride in `paramsSurfCompare`.

Source comments write dates as `dd/10`, meaning October 2026.

"HW kext" means `AppleIntelTGLGraphics` and "FB kext" means `AppleIntelTGLGraphicsFramebuffer`, both 16.0.32. Offsets are file offsets in those binaries, taken from the comments. They are informational only: every patch is applied by pattern search (`LookupPatchPlus`).

Snapshot binaries in the build directory narrow most undated items to 18-20/10.

### 1.0 The master switch `-ngreenxefix`

**Installed or active only with `-ngreenxefix`:**
- the display-13 register filter (`xeFixFilterWrite`/`xeFixEnsure`) and the RCS PSMI workaround;
- forcewake ref-counting;
- turning off V99R/V99G and V193;
- quieting the V74Ep and HANGCHECK logs;
- BLITSCRATCH and BLITSCRATCH2;
- the command-buffer pool size fix and POOLALIGN, and NOORPHAN;
- the IOAF2 flip counters;
- **all IGHardware\* scheduler hooks**. These are the `flipDiag` and `ringDiag` route arrays in the TGL HW block. They carry CSBGUARD, PAGEONGUARD, ELSQWAIT/ELSQIDLE/ELSQDELAY, FWSUBMIT, SAMETAIL, the POSH fixes and the scheduler diagnostics.

A feature marked "default ON" inside those hooks is therefore ON only when `-ngreenxefix` is present.

**Independent of `-ngreenxefix`.** These are gated only on the spoofed (non-real-TGL) path, on their own boot-arg, or on nothing:
- XE-VALD, XE-FMT, XE-NOCCS3, XE-NODEPTH, XE-NOTCF/NOTCF2, XE-BLIT16, XE-PLANES2, XE-NOAUX;
- the XE-NOCCS properties, XE-NOGVA, XE-DPCAPS;
- XE-RESFREE, XE-GTFREQ;
- the bootmode route installation, keepefi, stride/xtile/max2560 and engine-inert.

The release `config.plist` (`EFI/OC/config.plist`) uses these boot-args:
`-vi2c-force-polling IGLogLevel=3 msgbuf=400000 -ngreentglwithgfx -allow3d -disablegfxfirmware ngreen-dmc=adlp -ngreenv189 -ngreenkeepefi -ngreenxefix -ngreenfixbootmode amfi_get_out_of_my_way=1 ngreenSched=4 ngreenV142=3 ngreenblmax=48000`.

### 1.1 Display / framebuffer

| ID (source tag) | Date | Where | What it does and why | Default / gating |
|---|---|---|---|---|
| **keepefi** (`xe/nooted`) | 16/10 | `Gen11::hwInitializeCState` | Upstream always writes a register dump of the original author's 2560x1600 2-lane eDP panel into transcoder A: `TRANS_DDI_FUNC_CTL_A 0x60400=0x8A000106`; the H/V timings `0x60000..0x60028`; DATA/LINK M/N `0x60030..0x60044`; PPS `0xC7204=0x67` and `0xC7208=0x07D00001`. On a 3840x2400, 4-lane HBR3 panel this overwrites the firmware-trained mode and the panel goes black. The fork skips the writes when the boot-arg is set and `PIPECONF_A` (`0x70008`) bit 31 shows the pipe already running from UEFI/GOP. | opt-in `-ngreenkeepefi` (used in the release) |
| **linear stride from pipe width** (`xe/nooted`) | 16/10 | `Gen11::paramsSurfCompare` | Upstream forces plane 2 to linear with `PLANE_STRIDE=0xa0`, which only fits a 2560-px-wide panel. The fork computes the stride from the `PIPE_SRCSZ` width as `srcW*4/64`, which gives `0xf0` for 3840 px. | spoofed path, only when xefix is OFF |
| **xtile** (`xe/xtile`) | 16-17/10 | `paramsSurfCompare` | Keeps Apple's native X-tiled `PLANE_CTL` (bits[12:10]=001) and uses the X-tile stride `srcW*4/512` (`0x14` at 2560 px, `0x1e` at 3840 px). This experiment was superseded by the xefix DPT fix. | opt-in `-ngreenxtile` |
| **stride override** (`xe/stride`) | 17/10 | `paramsSurfCompare`, and `raWriteRegister32` on MMIO `0x70188` (PLANE_STRIDE pipe A, plane 1) | Overrides the stride with N and logs `XeNG STRIDE[n] in/out/ov` for the first 20 writes. Used to measure how the stride changes the width of the bands on screen. | opt-in `ngreenstride=N` |
| **max2560** (`xe/max2560`) | 17/10 | route `AppleIntelFramebuffer::validateDisplayMode` (commented out upstream, active in the fork) | Always calls the original first: skipping it caused a page fault in `getTimingInfoForDisplayMode` (panic at 17/10 10:54). Then it rejects modes wider than 2560 px. Upstream's wrapper body, which forced `pixelClock=785400000`, was removed. | behaviour opt-in `-ngreenmax2560`; route always installed |
| **xefix display-13 fix** (`xe/xefix`) | 17/10 | `xeFixFilterWrite()` inside `FastWriteRegister32` and `raWriteRegister32` (only the BLR subset in `wrapWriteRegister32`). `xeFixEnsure()` runs on every write to `0x7019C` (PLANE_SURF A/1). | Proven by plane CRC. (1) **`CHICKEN_MISC_2` `0x42084` gets bit 30 set**, which disables the DPT as i915 `intel_dpt_configure` does. The X-tiled plane then reads through the GGTT, as on TGL. (2) **`DBUF_CTL_S1` `0x44FE8` gets bit 31 set**: the TGL driver powers DBUF slice S1 off, and without it plane reads become noise. `xeFixEnsure` also ORs in `0x40c000`. (3) **`GAMMA_MODE_A` `0x4A480` is forced to 0** (bypass). TGL enables a 10-bit post-CSC gamma whose table format is wrong for display 13, which degraded text and colours. The bypass was proven live on 17/10 at 20h. | opt-in `-ngreenxefix` (release) |
| **V99R/V99G off under xefix** (`xe`) | 19/10 | PLANE_SURF redirect block in `raWriteRegister32` | The fixed 4000-page GGTT slots were sized for 2560x1600. At 3840x2400 they covered only 16 MB of the 36.9 MB needed, so the rest of the screen read stolen memory (glitch at the bottom). Slot 0 (GGTT[0..3999]) also overwrote the context LRCA at GGTT `0x18000`. With xefix the plane reads the surface that Apple programmed (log `XeNG V99 off: SURF ... direto`). The upstream block in the same function that forced CTL linear and STRIDE=0xa0 is commented out in the fork. | V99 off when `-ngreenxefix`; `-ngreenv99g` turns it back on |
| V99G logging (`xe/diag`) | 17/10 | same block | Adds the `firstSkip/lastSkip` of unmapped source PTEs, plus an `IOLog` copy of the message, because SYSLOG output does not reach the unified log on Tahoe. | diagnostic |
| **fixbootpatch** (`xe/bootmode`) | 17/10 | LookupPatchPlus in the FB kext (`applyFixBootMode()`) | The TGL FB starts with `fCurrentMode = kIOFBBootProgrammable (0xfffffffb)` and no detailed timings. `IOAccelDisplayPipe::init_framebuffer_resource` -> `getPixelInformation` then fails with `0xe00002c2`, CoreDisplay marks the main display offline, and WindowServer aborts in a loop. This patch changes the immediate of `movl $0xfffffffb,0x4250(%rbx)` to `0x80001000` (FB `0x53de3`): find `c7 83 50 42 00 00 fb ff ff ff`, replace `c7 83 50 42 00 00 00 10 00 80`. | opt-in `-ngreenfixbootpatch` (not in the release) |
| **fixbootmode** (`xe/bootmode`) | 17/10 | new route `AppleIntelFramebuffer::getCurrentDisplayMode(int*,int*)`; re-enabled route `getPixelInformation` (commented out upstream); worker `fbTimingsWorker` (thread_call every 50 ms, up to 6000 tries = 5 min) | (a) `getCurrentDisplayMode` replaces a returned `0xfffffffb` with `0x80001000`, depth 1. (b) A call to `getPixelInformation(0xfffffffb,…)` is re-issued for `0x80001000`. (c) WindowServer opens the display pipe about 2 s before the FB receives its timing table. The worker therefore builds that table early. It finds FB0 of `AppleIntelFramebufferDiags` (fb+0x1dc==0, online at fb+0x1e0) while `fDetailedTimings` (fb+0x4260) is still empty. It builds the timing from the CRTC through the solved symbol `AppleIntelBaseController::hwCRTCToIODetailedTimingInformation` and writes ID `0x80001000` into dword 0. It then does what `setDetailedTimings` does: `setProperty("IOFBDetailedTimings")` and stores the pointer at fb+0x4260. | behaviour opt-in `-ngreenfixbootmode` (release); routes always installed |
| **divguard** (`xe`) | 19/10 | `fbTimingsWorker` | `hwCRTCToIODetailedTimingInformation` divides by `p->0x164`, with `p = *(fb + (fb->0x3fd8 != -1 ? 0x4a08 : 0x49f0))`. Early in boot that value is 0, which caused a "divide error" kernel panic (boot at 23:56). The worker now calls the function only after the divisor is non-zero. | with fixbootmode |
| FBMODE counters | 17/10 | HANGCHECK dump | Prints the `gFbMode*`, `gFbt*`, `gTglFbBlockRuns` and related counters in the HANGCHECK dump. Load-time SYSLOGs were being overwritten in msgbuf by IGFB log output. | diagnostic (silenced under xefix together with HANGCHECK) |
| **XE-VALD** (`xe/vald`) | 19/10 | 3 LookupPatchPlus in the FB kext, in `AppleIntelFramebuffer::ValidateTransaction` (`callPlatformFunction "vald"`) | On Tahoe, every WindowServer flip was rejected with `0xe00002c7`. Three patches fix this. (1) The check `getTransactionOptions() & 0x500`: `a9 00 05 00 00 74 17` becomes `a9 00 00 00 00 74 17` (`0x3ecf1`). (2) The check `getPlaneTransactionOptions(p) & 0x500`: `a9 00 05 00 00 74 1e` becomes `a9 00 00 00 00 74 1e` (`0x3ed1d`). Tahoe's IOAF2 sets these bits by default; on Big Sur they meant something else. (3) The pixel format is read directly from `IOSurface+0xa4`, but the IOSurface layout changed on Tahoe and the read returns 0. `8b 80 a4 00 00 00 89 45 b4` is replaced with 9 NOPs, so the function's default `'BGRA'` is kept (`0x3ed88`). | always (logs only if a pattern is not found) |
| **XE-FMT** (`xe/fmt`) | 19/10 | LookupPatchPlus in the HW kext, `IGAccelDisplayPipe::performTransaction` (`0x81361`) | Same `IOSurface+0xa4` problem. Format 0 made the FB program the plane as YUV 4:2:2 (`PLANE_CTL 0x90010400`): a two-tone green screen with garbage on half of it. `8b 83 a4 00 00 00 89 85 f4 fe ff ff` becomes `b8 41 52 47 42 90 89 85 f4 fe ff ff` (`mov eax,'BGRA'`). | always (spoofed) |
| **XE-DPCAPS** (`xe/dpcaps`) | 19/10 | `injectAcceleratorPersonality` | Upstream V77 sets `IOAccelDisplayPipeCapabilities` to {DisplayPipeSupported=0, TransactionsSupported=0}. With the native display pipe in use, every `IOAccelDisplayPipeTransaction2::set_transaction_args` then returned `0xe00002c7`: no frame was presented and the flip stamps never completed, which ended in GPU restarts. The fork advertises true/true, as Apple's TGL Info.plist does, unless the display pipe is force-disabled. | always; `-ngreendp0` / `ngreendp0=1` restores 0 |
| **XE-PLANES / PLANES2** (`xe/planes`, `xe/planes2`) | 19/10, 20/10 | LookupPatchPlus in the HW kext, `IGAccelResource::calculateIOSurfaceDeviceCacheVRAMBytes` (`divl r15d` at `0x72bcd`) | Opening Settings > Displays caused a "divide error" panic: the plane count is read from `IOSurface+0xb0`, an offset that moved on Tahoe. Version 1 replaced the read with `xor eax,eax` (`8b 83 b0 00 00 00 85 c0 ba 01 00 00 00` became `31 c0 90 90 90 90 …`). That made every surface single-plane: the chroma plane of YUV surfaces was missing, giving a green wallpaper from HEIC images and empty video frames. Version 2 reads the Tahoe offset that `IOSurface::getPlaneCount()` uses (kernelcache 25G241): `8b 83 98 00 00 00 85 c0 ba 01 00 00 00`. | always (v2); `-ngreenplanesxor` selects v1; `-ngreennoplanesfix` turns it off |
| **XE-BLR** brightness | 20/10 | `xeFixFilterWrite` on the south PWM registers `0xC8254` (period) and `0xC8258` (duty); also applied in `wrapWriteRegister32` | Brightness only reached about 50%. Like WhateverGreen's BacklightRegistersFix, it keeps the hardware period (read before Apple's first write) and rescales the duty by `hw_period / apple_period`, with a clamp. If Apple never writes a period, `ngreenblmax=N` gives the duty value that Apple treats as 100%. Counters appear on the csstate line. | on with `-ngreenxefix`; `-ngreennoblr` turns it off; the release uses `ngreenblmax=48000` |

### 1.2 Scheduler / execlists / CSB (hooks installed only with `-ngreenxefix`)

| ID | Date | Hooked symbol / site | What it does and why | Default / gating |
|---|---|---|---|---|
| **RCS PSMI workaround** (`xe/xefix`) | 17/10 | `applyPreStartEngineWorkarounds`, `applyPreStopEngineWorkarounds` | Wa_1607297627 (i915 `rcs_engine_wa_init`, tgl/adl-p): masked write `0x10801080` to `RING_PSMI_CTL(RCS)` `0x2050`. This sets bit 12 (RC_SEMA_IDLE_MSG_DISABLE) and bit 7 (WAIT_FOR_EVENT_POWER_DOWN_DISABLE). Upstream had this workaround in dead code after a `return`. Without it the RCS sleeps waiting for an event, and the accelerator stalls with "timeout waiting for IntelAccelerator stamp". | `-ngreenxefix` |
| **fwref** (`xe/fwref`) | 19/10 | `Gen11::forceWake` | Without reference counting, one user releasing forcewake turned it off under another user. The RCS execlist write that followed RC6 entry was then lost. The fix keeps a reference count per (domain, ctx) in `fwRef[8][4]`, like Apple's SafeForceWakeMultithreaded, and writes the MMIO request only on 0->1 and 1->0 transitions. Two locking details: (1) The count is protected by an `IOSimpleLock` taken with interrupts disabled. forceWake also runs in primary interrupt context, and the plain lock caused a "hwlock … recursively" panic. (2) The register is written directly, because recursion through `wrapWriteRegister32` caused a "spinlock acquisition timeout, owner: current_thread" panic. | `-ngreenxefix`; `-ngreennofwref` turns it off |
| **fwhold** (`xe/fwhold`) | 19/10 | `forceWake` | When a reference count reaches 0, forcewake is **not** released, so the engine never drops into RC6. Tested on 19/10: it made things worse (zeroed CSB entries, "EL[0] was invalidated" panic). | **OFF**; opt-in `-ngreenfwhold` (`fwRelease` defaults to 1). The comment's `-ngreenfwrelease` is stale. |
| forcewake log silencing | 19/10 | `forceWake` | The timeout SYSLOGs became the event `xeEv('FWTO',…)`, because SYSLOG from interrupt context risks a deadlock on the log lock. The "ForceWake OK" flood was removed. | always |
| **fwsubmit** (`xe/fwsubmit`) | 19/10 | `IGHardwareCommandStreamer5::prepareExecListAndSubmit(uint,uint,const uint*)` | Holds Render+Blitter forcewake (`dom 0b101`, private ctx 3) around every submission. | ON; `-ngreennofwsubmit` |
| **elsqwait** (`xe/elsqwait`) | 19/10 | same | A submission written right after a context "finished" was lost while `EXECLIST_STATUS` (`eb+0x234`) still had bit 30 set (switch or save in progress): RPL drops ELSQ writes in that window. The hook waits for the bit to clear in 5 µs steps, for at most 2 ms, and logs the event `WAIT`. | ON; `-ngreennoelsqwait` |
| **elsqidle / XE-NOPREEMPT / XE-IDLEANY** (`xe/elsqidle`) | 19-20/10 | same (RCS only, `eb=0x2000`) | Good submissions found status lo `0x…01` (idle); the lost one found `0x00018098` with the previous context still current (hi = ID). The hook waits until the low byte is `0x01` or hi becomes 0, in 5 µs steps, up to `ngreenpreemptwait` µs. The default is 5000 µs (5 ms); earlier values were 50 → 2 → 15 → 50 → 5 ms. Logs `IDLW`, counters `gIdlw*`. **XE-NOPREEMPT:** the rejections happen while arbitration is on (bit 16) and a context is current, so a new submission triggers a preemption that wedges the RPL RCS. **XE-IDLEANY:** the hook also waits when bit 16 is clear; the login hang began on `0x3098` with a ctx5 lite restore. On 20/10 the wait was turned off, because it cost 8-16 s of stalled CPU in 3 minutes. It was turned back on the same day, because without it the Blit3D hang that reads scratch page `0xc01dbead` returned. | ON; `-ngreennoelsqidle` turns it off; `-ngreenidlearb` restores the old arbitration-only condition; `ngreenpreemptwait=N` (0..200000) |
| **elsqdelay** (`xe/elsqdelay`) | 19/10 | same | Adds a fixed delay before each RCS submission when no idle wait took place. | default 50 µs, applied only when the idle wait did not run; `ngreenelsqdelay=N` (0..5000, 0 = off) |
| **elsq log** (`xe/elsq`) | 19/10 | `IGHardwareCommandStreamer5::submitExecList(uint)` | After each submission, records into the event ring: ELSQ `0x510..0x51c`, EL_CTRL `0x550`, status `0x234`/`0x238`, CSB pointer `0x3a0`, and `0x30`/`0x34`. Tags `ELR0..2` (RCS) and `ELB0..2` (BCS). | diagnostic |
| **elsqretry** (`xe/elsqretry`) | 19/10 | same, plus thread_call `xeRetryWorker` | If status bit 30 is set after the write, saves the ELSQ contents. A worker then waits up to 200 ms for idle and re-issues `EL_CTRL_LOAD` (`0x2550 = 1`) if the ELSQ still holds the same descriptors (event `RTRY`). Suspected of causing the "EL[0] was invalidated!!" panic. | **OFF**; opt-in `-ngreenelsqretry`. The header comment that mentions `-ngreennoelsqretry` is stale. |
| **XE-SAMETAIL** (`xe/sametail`, from WhateverXe efa9241) | 20/10 | `submitExecList` → `xeSameTail()` | Linux avoids a lite restore with an identical TAIL. If the RCS re-submits the active item with the same ctx, descriptor (ctx+0x89), image (buf+0x38, buffer size 81920) and tail (image+0x101c), the hook sets Apple's one-shot force-restore flag at `ctx+0xc9`. Apple's code at `0x3a7b6` turns that flag into FORCE_RESTORE in the descriptor. Counter `stail`. | ON; `-ngreennosametail` |
| **XE-CSBGUARD** (`xe/csbguard`) | 19/10 | `IGHardwareCommandStreamer5::csbProcessActiveToIdle`, `csbProcessPreempted`, `csbProcessElementSwitch` (argument `const SGfxContextStatusBufferEntry*`) | Panic "EL[0] was invalidated!!" when the display mode changed. An active->idle event arrived late or twice, with EL[0] already empty, and `ContextStatusBufferValidate` panicked. The guard drops the event if any of these hold: the EL (`cs+0x30+idx*16`) is null; the event's ctx ID `(e[1]>>5)&0x7ff` differs from `ctx+0x7c`; or, for element-switch only, EL[1] is null. Events `CSBX`/`CSBP`/`CSBS`, counter `gCsbSkipN`. Each new skip triggers a dump of the last 40 CSB entries and 120 events (at most 6 per boot). | ON; `-ngreennocsbguard` |
| **POSH LRCA fix** (`xe/posh`) | 19/10 | `IGHardwareContext::restoreFromSafeImage()`; uses the solved symbol `IGSharedMappedBuffer::getVirtualAddress` | Contexts without POSH (ctx+0x6e bit 4 = 0) inherited `POSH_LRCA` (LRI register `0x21b0`) from the first context of the boot through the safe-image copy. As a result, ctx18's first submission never started. The fix zeroes the value after `0x21b0` in page 1 of the context image. | ON; `-ngreennoposhfix` |
| **POSHCTX** (`xe/poshctx`) | 20/10 | same hook, before the original | The RCS wedged in "element switch pending" (`0x18098`) after a POSH context finished. The hook clears bit `0x10` of `ctx+0x6e` at context creation, so no context is POSH. The driver already reports PoshEnable=0 to user space. | ON; `-ngreenposhctx` keeps POSH |
| **NOPOSH** (`xe/noposh`) | 19/10 | LookupPatchPlus in the HW kext, `IGHardwareRingBuffer::submitBatchBuffer` (`0x43021`) | Removes the POSH bit 20 from MI_BATCH_BUFFER_START: `31 c9 83 e0 10 48 c1 e0 10` becomes `31 c9 83 e0 00 48 c1 e0 10`. This gave a black screen. | **OFF**; opt-in `-ngreennoposh` (a comment naming `-ngreenposh` is stale) |
| **XE-LRCA** (`xe/lrca`) | ~18/10 (snapshot `antes-lrca`) | `Gen11::xeLrcaRepair(tag)`, called from `populateResetRegisterList` ("resetRegList") and after `startGraphicsEngine` ("post-start") | Refactor of upstream V507. It re-copies the 96-dword ring LRI from page 1 of the RCS context into the execlist slot GGTT[0x19] when the slot's DW1 is not an LRI header (`DW1 & 0x1F801000 != 0x11001000`). Now it also runs after engine start, and it logs the PTEs and a readback. | spoofed, always |
| V509 logs | 19/10 | V509 init / withOptions | `IOLog` lines `XeNG V509 init/withOptions …` (REPARA / COPIA->slot19). | diagnostic |
| **XE-INERT** (`xe/inert`) | 18/10 | `startGraphicsEngine` and the register-write filters (`FastWriteRegister32`, `raWriteRegister32(b)`, `wrapWriteRegister32`) | Tests the hypothesis that GPU DMA was corrupting memory (WindowServer and kernel heaps; pmap/zalloc panics). `startGraphicsEngine` returns success without starting any ring. Apple's writes to RCS `0x2000-0x2FFF`, BCS `0x22000-0x22FFF` and VDBOX/VEBOX `0x1C0000-0x1CFFFF` are dropped. Display keeps working; Metal waits forever on stamps. | diagnostic, opt-in `-ngreenengineinert` |

### 1.3 Memory / resources / IOAcceleratorFamily2 (kern_green.cpp unless noted)

| ID | Date | Hook | What it does and why | Default / gating |
|---|---|---|---|---|
| **XE-POOL** (`xe/pool`) | 19-20/10 | `IOAccelCommandBufferPool2::init(IOGraphicsAccelerator2*, IOAccelChannel2*, IOAccelTask*, int, int, uint, uint, uint, uint)` | The blit3d batch ran past the end of the pool buffer. Tahoe's `getBufferPtrNoInc` submits a half-full buffer when it runs out of space, and the GPU read the next page, which still held the poison value `0xc01dbead`. TGL requests up to `0x3ffe` dwords at once. Version 1 (19/10) made every buffer at least 128 KB. That gave 256 × 128 KB = 32 MB per pool, one pool per process, and exhausted the command-buffer address space: the batch continued into a scratch page at `40c2a000` and hung. Version 2 (20/10): pools with count ≤ 16 (such as the kernel blit3d pool, count = 8) get at least `0x20000`; larger pools go from 4 KB to `0x4000`; 64 KB pools are left unchanged. The first 40 pool inits are logged as `XeNG POOL[n]`. | route and logging always; size change with `-ngreenxefix`; `-ngreennopoolfix` turns it off |
| **XE-POOLALIGN** | 19/10 | `IOAccelCommandBufferPool2::getBufferPtrNoInc(uint)` | TGL's blit3d computes its state offsets (surface states, binding table) assuming the command starts 64-byte aligned. On Tahoe the blit follows context init at `0x…9b8`, so the state ended up `0x18` bytes off: the GPU read an empty binding table and the draw never finished. For reservations of at least `0x400` dwords, the hook pads with zeros (MI_NOOP) to the next 64-byte boundary. Pool fields used: `+0x1842` current index, `+0x40+24*i` CPU base of buffer i, `+0x1850` limit, `+0x1858` current pointer. | same as XE-POOL |
| **XE-BLITSCRATCH / BLITSCRATCH2** | 19/10 | in kern_gen11.cpp, the `blit3d_init_ctx` wrapper; routes `IOAccelSysMemory::lockForCPUAccess(task*,uint)` and `unlockForCPUAccess(task*)` | Upstream skips step 2 (`blit3d_initialize_scratch_space`), which left the blit3d kernels' scratch **zeroed** (instrBase `0x477a3000`, `+0x4c0` = 0). Every kernel-side 3D blit (display-pipe flip, IOSurface copies) then executed zeros on the EUs: the RCS stuck in PIPE_CONTROL after 3DPRIMITIVE, followed by a cascade of gpuRestart. The fix calls the original function while lock/unlock is redirected for one object (`gXeBlitMem`) to a 0xe000-byte temporary buffer (`gXeBlitCpu`). It then copies `[0,0xd000)` into the IGSharedMappedBuffer's CPU VA. That VA maps only 0xd000 bytes: a write fault at cpu+0xd000 was the same crash as V69. The sampler and blend states at `0xd000..0xd210` are lost. Expected value at +0x4c0: `2420005b`. | `-ngreenxefix`; `-ngreennoblitscratch` turns it off |
| **XE-NOORPHAN** (`xe/noorphan`) | 20/10 | `IOAccelOrphanedMemoryPool::getMemory(IOAccelShared2*, IOAccelResource2*, ulonglong, void*)` → `nullptr` | Panic "corrupt list" in iconservicesagent, in `IOAccelSysMemory::unorphanIt` <- `getMemory` <- `IOAccelResource2::newResourceWithOptions`. With the TGL driver the orphan list gets corrupted, and reused memory may still be in use by the GPU. The hook returns `nullptr`, so fresh memory is always allocated. Counter `orf`. | `-ngreenxefix`; `-ngreenorphanreuse` turns it off |
| **XE-RESFREE** (kern_gen11.cpp) | 20/10 | route `IGAccelResource::free()` | Panic "kfree: addr 0x300c000040001 … not found in any zone" (pid Wallpaper) at `IGAccelResource::free+0x182`. The per-plane/mip table at `this+0x250` holds invalid pointers at entry+0x20. Its size is at `+0x258`; entries are 40 bytes; the index is `i*[0xad]+j` with i < [0xac] and [0xae] ≤ j < [0xae]+[0xad]. The hook nulls the table pointer if it is unmapped and zeroes unmapped entry pointers, leaking a few bytes instead of panicking. If the indices run past the table, it sets [0xac] = 0. Mapping is checked with `pmap_find_phys(kernel_pmap, va)`. | ON (route always installed); `-ngreennoresfree` |
| **XE-NOPAGE** (`xe/nopage`) | 19/10 | `IGAccelResource::pageoff(IOAccelEvent*,bool,bool*,ulonglong)` / `pageon(IOAccelEvent*,bool,ulonglong)` | Panic in `pageon`, from Settings > Displays or a DPI change: WallpaperImageExtension read one level's rect list past its end, into an unmapped page (count `0x79a8`). The fix made both functions no-ops. It was reverted to opt-in because windows then rendered empty (pgoff=771, pgon=2069 in about 3 minutes): pageon is part of normal texture preparation. | **OFF**; opt-in `-ngreennopaging` (the comment's `-ngreenpaging` is stale) |
| **XE-PAGEONGUARD** (`xe/pageonguard`) | 19-20/10 | `pageon` | Before calling the original, checks every page of each level's rect array through `pmap_find_phys`. Layout: entries at resource+0x250, (faces `+0xac`) × (levels `+0xad`), 40 bytes each; u16 count at +0x18; array of u32 pairs at +0x20. If any page is unmapped, that level's count is set to 0. It started as opt-in and has been ON by default since 20/10 01h20, after the last 3 panics were GP faults there with non-canonical pointers. Counter `pgfix`. | ON; `-ngreennopageonguard` |

### 1.4 Compression / resolves / 3D blit

| ID | Date | Site (HW kext) | Patch and reason | Default |
|---|---|---|---|---|
| **XE-NOCCS** (`xe/noccs`) | 18-19/10 | accelerator service properties (non-real-TGL branch) | Sets `RenderCompressionMode=0` and `AllowIOSurfaceRenderCompression=0` both in the `Development` dictionary and at top level; `populateAccelConfig` reads both via `utilGetProperty`. Reason: with `ngreenSched=4`, SecurityAgent panicked in `resolve_ccs` <- `submitCCSResolve` <- `enableRenderCompressionWithAccelTask`, because the AUX-TT is not set up on RPL. | always (spoofed) |
| XE-NOCCS2 (`xe/noccs2`) | 18-19/10 | `enableRenderCompressionWithAccelTask` (`0x7379e`) | Turned the `jne` into a `jmp` so that nothing is ever compressed: `8a 86 91 00 00 00 a8 02 0f 85 37 02 00 00` became `… a8 02 90 e9 37 02 00 00`. **Removed:** the Metal driver still used fast clear and compressed stencil without an AUX mapping, and the RCS hung in PIPE_CONTROL. It survives only in `kern_gen11.cpp.antes-noccs3` and the `noccs2.bin` snapshot. | abandoned |
| **XE-NOCCS3** (`xe/noccs3`) | 19/10 | prologue of `IntelAccelerator::submitCCSResolve` (`0x2c84c`) | Compression and AUX stay enabled; only the resolve returns immediately. `resolve_ccs` writes into buffer 2 of IOAccelCommandBufferPool2, which differs on Tahoe and page-faulted. The caller ignores the return value. Find `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 48 48 89 75 c0 f6 87 78`; the first 3 bytes become `31 c0 c3` (`xor eax,eax; ret`). | always (spoofed) |
| **XE-NODEPTH** (`xe/nodepth`) | 20/10 | prologue of `IntelAccelerator::submitDepthResolve` (`0x2c656`; callers at `0x744d7` and `0x788e6` ignore the return value) | Panic in iconservicesagent in `resolve_hiz_g7`, which also writes into the Tahoe pool. Find `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 38 f6 87 78 0c 00 00 02 0f 84`; the first 3 bytes become `31 c0 c3`. | ON; `-ngreendepthresolve` turns it off |
| **XE-NOAUX** (`xe/noaux`) | 19/10 | `IGAccelResource::fillSurfaceStateAuxInfo` (`0x73fd0`) | The RCS hangs (without a fault) in PIPE_CONTROL after drawing to small render targets with RENDER_SURFACE_STATE AuxiliarySurfaceMode=5 (CCS_E): the AUX-TT does not cover them on RPL. The patch forces aux = NONE. In `f6 86 91 00 00 00 02 8b 42 18 75 0b 83 e0 f8 89 42 18`, the `75 0b` becomes `90 90`. | ON; `-ngreenccs` |
| **XE-NOTCF** (`xe/notcf`) | 19/10 | `IGAccelDisplayPipe::generateFlip`, 3 sites | The first GPU restart of every boot was the display-pipe context stuck in the flip's PIPE_CONTROL, whose dw1 is `0x50105021` (DC/RT/depth flush + CS stall + post-sync + Tile Cache Flush, bit 28). The rest of the driver uses `0x40105021` and does not hang. Patch on the movabs: `48 be 04 00 00 7a 21 50 10 50` becomes `… 10 40`, 3 times. | ON; `-ngreentcf` |
| **XE-NOTCF2** (`xe/notcf2`) | 19/10 | `IGHardwareRingBuffer{Main,Compute}::commitStampCommand(stamp,bool)`, 2 sites | The PIPE_CONTROL that really hangs is the flush before the stamp, with `dw1 = 0x40105021 \| (bool<<28)`. `4c 89 75 a8 49 c1 e6 3c 49 09 de` becomes `4c 89 75 a8 45 31 f6 90 49 09 de`: `shl r14,0x3c` is replaced by `xor r14d,r14d; nop`. | ON; `-ngreentcf` |
| **XE-BLIT16** (`xe/blit16`) | 19/10 | `blit3d_submit_commands`, 3 sites | With the flush fixed, the Blit3D context still hung: EUs/WMFE/PSS reported "not done". The SIMD32 pixel shader (`IGBlit3DKernel_default_simd32`, KSP1=0x540) never terminates on RPL. The patch forces the SIMD16-only path (KSP0, dw7=0x60000), normally taken when params+0x55==0x10, by turning `cmp cl,0x10` into `cmp cl,cl`. (a) `31 f6 80 f9 10 40 0f 95 c6` becomes `31 f6 38 c9 90 40 0f 95 c6` (×1). (b) `80 f9 10 48 be 00 00 00 00 06 00 00 00` becomes `38 c9 90 48 be …` (×2). | ON; `-ngreenblit32` |
| **XE-RPCS** (`xe/rpcs`, from WhateverXe 1163642) | 20/10 | RCS context constructor (`0x7c313`) | Writes `R_PWR_CLK_STATE` (`0x20c8`) in the context as `0x80041000` (ENABLE \| S_CNT_ENABLE \| 1 slice, as Linux does) instead of `0x80000000`. `41 c7 87 0c 11 00 00 00 00 00 80` becomes `41 c7 87 0c 11 00 00 00 10 04 80`. Made opt-in after a boot with RPCS+SAMETAIL submitted very slowly. | **OFF**; opt-in `-ngreenrpcs` |
| **V193 off under xefix** (`xe`) | 19/10 | `Gen11::submitBlit` | Upstream V193 blocks blits with routeSel==3 ("TGL EU shader → RCS hang"). The real cause was the state written 0x18 bytes off, which POOLALIGN fixes. Blocking without executing left the reserved stamp (index 9) pending, and WindowServer waited forever. | off with `-ngreenxefix`; `-ngreenv193` turns it back on |
| blitdiag (`xe/blitdiag`) | 19/10 | `submitBlit` | Every return path goes through `xeBlitRet()`, which logs `XeNG BLIT #n routeSel task exit=L<line> ret` (the first 48 calls, then every 64th). | always (diagnostic) |

### 1.5 Power / frequency

| ID | Date | Hook | What it does and why | Default |
|---|---|---|---|---|
| **XE-GTFREQ** | 20/10 | route `IGHardwareCommandStreamer5::setGTFrequencyMMIO(uint)` | `handleChangeFrequencyInterrupt` asks the power object at `accel+0xde8` for a frequency. That object depends on GuC/SLPC, which `-disablegfxfirmware` turns off, so it answers 0. RPNSWREQ (`0xA008`) is then written as 0 and the GPU stays at RPn: 100 MHz, while RP0 on an i7-1360P is 1500 MHz. Proven live: a 900 MHz request under forcewake raised CAGF at once, and Apple reset it to 0 within about 20 ms. The hook decodes the ratio the way Apple does (`(val&0xf) ? (val>>14)&0x1ff : val>>23`) and never lets it fall below `floor = MHz*3/50` (16.67 MHz units). Below the floor it writes `floor<<23`. When idle the GPU still enters RC6. | ON with a 1100 MHz floor; `ngreengtfreq=N` (capped at 1500; 0 turns it off) |

### 1.6 Video / personality

| ID | Date | Where | What it does and why | Default |
|---|---|---|---|---|
| **XE-NOGVA** (`xe/nogva`) | 20/10 | accelerator service properties (non-real-TGL) | Removes `IOGVACodec`, `IOGVAScaler`, `IOGVABGRAEnc`, `IOGVAHEVCDecodeCapabilities` and `IOGVAHEVCEncodeCapabilities`. On RPL the hardware `VTDecompressionSession` fails with -12913. Any software session that asks for an output format then dies with -6680, because the conversion goes through the GVA scaler. As a result YouTube showed "An error occurred". Without these keys VideoToolbox decodes and converts in software. Proven with the `xe-vttest` tool. | ON; `-ngreengva` keeps the keys |

### 1.7 Fork diagnostics / logging

| ID | Date | Mechanism | Notes |
|---|---|---|---|
| **flipdiag (TGL side)** | 19/10 | routes `IGAccelDisplayPipe::submitTransaction`, `isTransactionComplete`, `signalTransactionComplete(IOAccelEvent*)`, `IntelAccelerator::signalDisplayStampUpdate(int,uint)` | Event tags `FSUB`/`FISC`/`FSIG`/`FDST`. Also captures the `IntelAccelerator` object for csstate. Written to investigate WindowServer stopping flips with "Display not ready". `-ngreenxefix` only. |
| **flipdiag (IOAF2 side)** | 19/10 | `IOAccelDisplayPipe::transaction_interrupt_gated`, `transaction_timeout_timer_fired(IOTimerEventSource*)`, `completeTransaction`, `transaction_queue_gated` | Counter `IOLog`s: the first 24 calls, then every 4096th/64th/1024th. `-ngreenxefix`. |
| **scheddiag** | 19/10 | `IGScheduler5::push(IGHardwareContext*,uint,uint,bool,bool)`, `notify(IGHwCsType)`, `pause()`, `resume()`; `IGHardwareCommandStreamer5::submitExecList` | Written because the work for stamp 56 never reached the GPU. Tags `PUSH`, `EXEC`; pause/resume are IOLogged (first 40). |
| **ringdiag** | 19/10 | `IGHardwareRingBuffer::submitToRing()`, `writeStamp(uint,bool)`, `insertSemaphoreWaitPoll(uint,int)` | Per-stamp-index submit counts and the last stamp; tags `RING` and `SEMW` (first 60). |
| **csbdiag** | 19/10 | `IGHardwareCommandStreamer5::processCSBDetail(SGfxContextStatusBufferEntry*, uint&)` | Interrupt-safe circular buffer of 4096 entries. Each entry holds the raw CSB lo/hi, the decoded type, the current port and an occupancy bitmap. The raw words are read from `accel+0x1240` + `c28+0x29` when `accel[0x1190]` bit 7 is set, otherwise from `cs+0x68` + `0x40`. |
| **evaldiag / event ring** | 19/10 | `xeEv(tag,a,b,c)`; hooks on `evalWaitQueue(const bool*,bool,bool)` (now a pass-through) and `IGScheduler5::verifyThrottling(bool*)` (counters `thrt`/`thrtSim` only; it is called about 6000 times per second) | Circular buffer of 16384 events with timestamps. |
| **csstate** | 19/10 | thread_call `xeCsStateWorker`, every 250 ms for about 10 minutes | Every 8th tick it logs, per command streamer, `cur`, port occupancy and the flags `0x1c00`/`0x1c38`/`0x1c3c`. It also logs DPSM `IntelAccelerator+0x1458`, cfg `+0x1190`, and all fork counters (push/exec/thrt/csbx/idlw/pgoff/pgon/pgfix/stail/resfix/orf/gtf/bl…). The event and CSB buffers are flushed to the log only with `-ngreenevlog`. The dumps triggered by a CSB skip are always on. |
| quiet logs | 19/10 | V74Ep SYSLOG, HANGCHECK dump, ForceWake OK/timeout | Silenced under `-ngreenxefix`. A "spinlock timeout, owner=current_thread" panic stopped inside the V74Ep SYSLOG. |

### 1.8 Experiment snapshot history (build directory)

A kext binary was saved before or after almost every step, named after its `xe/*` tag. In chronological order, by file modification time (18-20/10):
`antes-noccs, noccs1, noccs2, noccs3, dpcaps, vald, divguard, fmt, v99off, pool1, poolalign, flipdiag, scheddiag, v193off, blitdiag, ringdiag1/2, csstate, csbdiag, evaldiag, fwref1/2, fwhold, fwsubmit1/2, elsq, v509log, posh, elsqwait, noposh1/2, elsqdelay, elsqidle, elsqretry, nopreempt, antes-idleany, antes-noaux, antes-evring, antes-notcf, antes-notcf2, antes-blit16, antes-blitscratch, antes-logquiet, antes-blitscratch2, antes-planes, antes-quietlog, antes-csbguard(2), antes-nopage, antes-pageonguard, ATUAL-pageonguard15ms, BOA-quietlog, BOA+csbguard, antes-wex, antes-noidlw, antes-pageonguard2, antes-pool2, antes-idlw5, antes-blr, antes-nodepth, antes-resfree, antes-gtfreq, antes-noorphan, antes-planes2, antes-nogva`.

How to read the names:
- `antes-X` means "before X".
- `BOA` means "good".
- `ATUAL` means "current".
- `antes-wex` and `antes-evring` match no tag in the current source (purpose unclear).

Other snapshots:
- Source snapshots `kern_gen11.cpp.antes-{inert,lrca,noccs,noccs3}` sit next to the source.
- Earlier display experiments (16-17/10) are `NootedGreen-xtile.kext` and the EFI test configs `config.teste-{tahoe,xtile,stride,max2560,dmcskip,dmctgl}.plist`. The `nooted/tahoe/aplicar-*-efi.sh` scripts install them.

---

## 2. Upstream NootedGreen features (V-numbered patches, routes, byte patches, boot-args)

These features exist in upstream commit 95f8189 unless marked as fork code. The section is organised by source range: 2.A covers kern_gen11.cpp lines 1-3800, 2.B lines 3800-7600 and 2.C the rest of the plugin. Where a fork `xe/*` change gates or alters a V-feature, the entry says so. "Spoofed-only" means the code is gated on `!isRealTGL`.

### 2.A kern_gen11.cpp lines 1-3800: V-tags, routes, byte patches, boot-args

Scope: `src/NootedGreen/kern_gen11.cpp` L1-3800. Fork code (xe/*, XE-*, XeNG) is listed only where it gates a V-feature or for completeness of the route/patch tables (marked **fork**). "Spoofed-only" = guarded by `!NGreen::callback->isRealTGL` (RPL-P 8086:A7A0 path).

#### 1. V-tags

| Tag | Area | Where | What it does / problem addressed | Status | Lines |
|---|---|---|---|---|---|
| V35 | display/framebuffer | TGL FB route list (comment) | A ComboPHY hook was removed because it caused an MCE on RPL/ADL; the firmware calibration is enough. | comment only (hook removed) | 1155 |
| V36 | scheduler/interrupts | route `IntelAccelerator::readAndClearInterrupts` (ICL HW and TGL HW lists) | Was meant to initialize Gen11 multi-engine GT interrupts (RCS/BCS user interrupts, ctx-switch notifications) so IOAccelF2 sees stamp completions. | commented-out (V37) | 1615, 1743 |
| V37 | scheduler/interrupts | same | Disabled V36: it caused a boot hang on the TGL path (symbol possibly absent, or interrupt reprogramming too early). It was disabled on ICL too as a precaution. | comment only | 1617, 1746 |
| V44 | diagnostics/logging | route `__ZN16IntelAccelerator5startEP9IOService` -> `start` (TGL HW) | Hooks IntelAccelerator::start so Gen11::start can log. Not fatal if the symbol is missing. | always (TGL HW) | 1876-1885 |
| V46 | blit/3D (device start) | byte patch `f_devstart` (masked) in TGL HW | NOPs the `je` after `call *0x970(%rax); test al,al` in IGAccelDevice::deviceStart. The BCS CS5::init fails (BCS CTL=0, ring not running), which made deviceStart abort, so IOServiceOpen failed and MTLDevice was nil and WindowServer hung. | spoofed-only (in V52 RPL group, PANIC on miss) | 1993-2022, 2140 |
| V52 | other (patch grouping) / blit | TGL HW patch block | Splits patches into an always group (SKU/device-ID bypass f3) and an RPL-only group (topology hardcodes plus BCS bypass), because real TGL reads topology from fuses. The optional long-form deviceStart bypass inside V52 is commented out. | always (split); RPL group spoofed-only | 2123-2455 |
| V59 | memory/MMIO | comment at the ReadRegister32 routes | Experiment that showed ReadRegister32 hooks cause a 0-children regression (forcewake/power-well cycling loop). | comment only | 1183 |
| V60 | memory/MMIO + diagnostics | (a) commented routes `RegisterAccessManager::ReadRegister32` (Em / EPVvm); (b) helper `isV60MonitorEnabled()` | (a) ReadRegister32 hooks disabled because of V59. (b) Monitor enable switch: `ngreenv60off`/`-ngreenv60off` forces off; `ngreenv60`/`-ngreenv60` forces on; otherwise follows `ngreenexp`. Consumers are outside the range. | (a) commented-out; (b) opt-in (default off) | 118-140, 1183-1186 |
| V60rw (aggressive) | diagnostics | helper `isV60AggressiveMonitorEnabled()` | Enable switch `ngreenv60rw`/`-ngreenv60rw`. Consumer at L8348, outside the range. | opt-in, default off | 250-257 |
| V65 | scheduler/engines | helper `getV65Tier1WantBits(isRealTGL)` | Engine "want bits". Real TGL gets RCS0\|BCS. Spoofed gets RCS0 only, unless V142 mode 3 or `-ngreenbcsirq` is set, which gives RCS0\|BCS: when the original submitBlit runs and BCS is missing, RING_CTRL is never enabled and the ring stalls (gpuRestart sig 801). Consumers are outside the range. | always (value depends on platform/args) | 263-279 |
| V69 | blit/3D | route `IGHardwareBlit3DContext::initialize` -> `IGHardwareBlit3DContextinitialize` | Replacement that skips the original's 68-byte memcpy at +0x4c into an unmapped GPU VA (page 0xD of the context buffer, e.g. 0xfffffff034136000) and logs the mapping. | always (TGL HW) | 1759-1762 |
| V72F / V72Fp | diagnostics (was scheduler/EMR) | `Gen11::FastWriteRegister32` | V72F used to force RCS/BCS RING_EMR (0x20b4/0x220b4) writes to 0xFFFFFFFF (mask all engine errors). It was removed; V72Fp now only logs the passthrough (first 6). | V72F removed; V72Fp diagnostic-only | 3527-3540 |
| V72R | scheduler/EMR | `raWriteRegister32` (comment) | The same blanket RING_EMR=0xFFFFFFFF mask on the raWriteRegister32 path. Removed. | comment only (removed) | 3528, 3687-3693 |
| V72W | scheduler/EMR | mentioned as living in the `wrapWriteRegister32` helper | Third member of the EMR-mask trio. Not implemented in this range. | comment only in range | 3528, 3691 |
| V74 | scheduler/EMR | mentioned (50 ms "permanent enforcer", `v71EmrEnforcer`) | Polling thread that re-forces EMR through writeReg32. The comment says its EMR portion is stripped separately. Not implemented in this range. | comment only in range | 3529-3532, 3692 |
| V77 | display (DisplayPipe clients) | helper `getV77KillDelayIterations()` | `ngreenV77DelayKill` is clamped to 0..60. The default of 60 means "do not kill DisplayPipe clients unless explicitly requested". Consumer at L8455, outside the range. | always (default 60) | 281-293 |
| V78A | display/framebuffer | helper `isDisplayPipeForceDisabled()` | Picks native DisplayPipeSupported. `ngreendp1`/`-ngreendp1` gives native ON; `ngreendp0`/`-ngreendp0` forces DisplayPipe disabled ("dp0 mode"); the default is native ON ("V78A (capped)"). It drives Path B/Path C in range and other consumers outside it. | default native ON; dp0 opt-in | 142-180 |
| V80 / V80L | display (plane linearization) | comment in `isDisplayPipeForceDisabled`; helper `isV80LEnforcerEnabled()` | V80 plane-linearization writes are limited to the first 3 ticks (≤150 ms). Coupling V80 to DisplayPipe gave DisplayPipeSupported=0 and a permanent black screen, so the two are deliberately decoupled. V80L enforcer switch is `-ngreenv80l` (consumer at L9340). | V80: comment in range; V80L opt-in, default off | 143-146, 259-261 |
| V88 | display (scanout fill) | helper `isV88ScanoutFillEnabled()` | Switch `ngreenv88`/`-ngreenv88`. Consumer at L9073, outside the range. | opt-in, default off | 241-248 |
| V90L4 | display/link | `Gen11::computeLaneCount` (route `AppleIntelPort::computeLaneCount`) | After the original runs on spoofed hardware, reads DDI_BUF_CTL_A 0x64000 PORT_WIDTH bits[3:1] (≥3 gives 4 lanes, ≥1 gives 2, else 1). Raises `*laneCount` to the HW-trained (UEFI/GOP) lane count when Apple's DPCD-based count is lower. Logs 10. | spoofed-only | 3104-3134 |
| V90L5 | display/link | `Gen11::setupOptimalLaneCount` | Same DDI_BUF_CTL_A read. Raises AppleIntelPort+0x148 (fOptimalLaneCount, the cached value capped to DPCD MAX_LANE_COUNT=2) to the HW lane count, so SetupParams builds TRANS_DDI_FUNC_CTL with the trained width. | spoofed-only | 3136-3171 |
| V93 (+V93PlaneSurfState) | display/framebuffer | `raWriteRegister32`; helper `isV93PlaneGuardEnabled()` | Plane SURF zero-write guard. For regs 0x60000-0xBFFFF ending in 0x19C, a write of 0 while PLANE_CTL (SURF-0x1C) bit31 is set is replaced with the last nonzero SURF (8-slot table) or the current HW SURF. Otherwise the plane is disabled first. | opt-in `ngreenv93`/`-ngreenv93` (default off: black-screen regressions) | 182-189, 3618-3685 |
| V96 / V96p | display/framebuffer | route `AppleIntelFramebuffer::getOnlineInfo` -> `getOnlineInfo` | V96 used to force `*online=1` for fb0, because WEG's getDisplayStatus FOD hook fails ("err 2") on the TGL kext. It was removed as redundant (Apple reports orig=1). The wrapper is now a passthrough that logs (V96p, first 12, fbId at +0x1DC). | V96 removed; V96p diagnostic-only | 1160-1162, 3257-3272 |
| V97 (readAUX) / V97AUX | diagnostics/display link | route `AppleIntelPort::readAUX` -> `wrapICLReadAUX` | Logs the first 40 AUX reads (addr, len, ret, two bytes) to diagnose eDP link training on RPL. The same wrapper hosts V98/V98T/V99. It also always zeroes maxLinkRate when DPCD revision < 0x03, on any platform. | always (logging) | 1158-1159, 3173-3188 |
| V97 (TRANS_DDI bit16) / V97Rp | display/link | `raWriteRegister32` @ 0x60400 | V97 used to clear bit16 (PORT_SYNC_MODE_MASTER_SELECT[0]) of TRANS_DDI_FUNC_CTL_A on spoofed hardware (suspected cause of "InterLane Alignment is lost" ~10 s after boot). Removed as symptom-hiding. V97Rp logs the passthrough (6). | V97 removed; V97Rp diagnostic-only (spoofed) | 3739-3754 |
| V97P | display/link | comments only | Referred to as a "bit16-only" correction / 4-lane DDI_FUNC_CTL write that, combined with lane capping, gave a black screen. Not implemented in this range. | comment only | 3192-3194, 3239 |
| V98 | display/link | `wrapICLReadAUX` (DPCD caps @0x0000/0x2200) | Stopped capping maxLaneCount/maxLinkRate (capping to 2 lanes plus the V97P 4-lane write gave a black screen). UEFI trains HBR3×4, so caps now pass through. Logs 5. | spoofed-only (passthrough + log) | 3236-3248 |
| V98T | display/link | `wrapICLReadAUX` (DPCD 0x0100) | The clamp of LINK_BW_SET/LANE_COUNT_SET to HBR2×2 was removed. Now only logs the passthrough (5). | removed; diagnostic-only | 3190-3207 |
| V99 | display/link | `wrapICLReadAUX` (read at 0x0202, len≥3) | Clears bit7 (LINK_STATUS_UPDATED) of DPCD 0x204. HDCP probing (DPCD 0x6921d) makes the eDP panel assert IRQ_HPD; Apple's checkLinkStatus then sees INTERLANE_ALIGN_DONE=0 and tears down the display ~10 s after boot. Logs 10. | spoofed-only | 3209-3227 |
| V99D | diagnostics | `FastWriteRegister32` | Logs the first 30 FastWrite (addr, val) calls. | always, diagnostic-only | 3516-3525 |
| V99F[S] / V99F[Sp] | display (stride) | `FastWriteRegister32` @ 0x70188 | Was PLANE_STRIDE *= 8 (X-tile units to 64 B units). Removed; Sp logs (3). | removed; diagnostic-only | 3542-3553 |
| V99F[C] / V99F[Cp] | display (tiling) | `FastWriteRegister32` @ 0x70180 | Was a PLANE_CTL X-tiled(001) to Y-legacy(100) rewrite. Removed; Cp logs (3). | removed; diagnostic-only | 3554-3565 |
| V99F[SURF] / V99F[SURFp] | display | `FastWriteRegister32` @ 0x7019C | Was forcing STRIDE=0xa0 at SURF arm. Removed; SURFp logs SURF and HW stride (3). | removed; diagnostic-only | 3588-3600 |
| V99f | display (stride) | comment | A "fast-write variant" stride rewrite mentioned as still existing elsewhere. Not in this range. | comment only | 3702 |
| V99R[S] / V99R[Sp] | display (stride) | `raWriteRegister32` @ 0x70188 | Was PLANE_STRIDE *= 8 (0x14 to 0xa0). Removed. The address shell now carries the fork `ngreenstride` override plus the V99R[Sp] log. | removed; diagnostic-only (+fork override) | 3695-3719 |
| V99R[C] / V99R[Cdp] / V99R[Cp] | display (tiling) | `raWriteRegister32` @ 0x70180 | Were PLANE_CTL rewrites (X to Y-legacy by default; linear under dp0). Removed; Cp logs. | removed; diagnostic-only | 3720-3737 |
| V99R / V99G (core triad) | display + memory/GGTT | `raWriteRegister32` SURF-arm block (continues after L3800) | Comment: on dp0, SURF writes ≥0x10000000 (non-aperture) are redirected to 0, and V99G copies the migrated surface's GGTT PTEs down to GGTT[0..3999] on every flip, so SURF=0 scans the pages WS writes. Linear CTL/STRIDE (0xa0) is forced too. The implementation and gating (off with `-ngreenxefix`, back on with `-ngreenv99g`) are after L3800. | spoofed-only; body outside range | 3282, 3784-3800 |
| V99S | display/framebuffer | `raWriteRegister32` @ 0x7019C (PLANE_SURF) | At SURF arm, reads HW STRIDE 0x70188 and CTL 0x70180 and logs the first 8 with aperture pixel samples. Per the comment, it forces STRIDE/CTL through writeReg32 before the SURF latch, because raWriteRegister32 is cache-only. The rest of the body is after L3800. | always (logging); forcing outside range | 3784-3800 |
| V99Z | display (diagnostic) | comment in Path B | A dirty-rect wipe test, removed (WS rewrites the whole buffer every frame). | comment only | 3295-3301 |
| V103 / V103p | power (DC states) | `raWriteRegister32` @ 0x45504 | V103 used to block nonzero DC_STATE_EN writes with the ADL-P DMC (`dmcIsAdlp`) to avoid DC3/5/6 (eDP lanes dropped ~70 s after). Removed; V103p logs (10). | removed; diagnostic-only | 3756-3771 |
| V103F / V103Fp | power (DC states) | `FastWriteRegister32` @ 0x45504 | The FastWrite twin of V103. Removed; Fp logs (6). | removed; diagnostic-only | 3566-3575 |
| V103P | power | comment | The third V103 site (named as passthrough). Not in this range. | comment only | 3567 |
| V105 | power/PSR | comment in Path B | V105 wrote the wrong register (0x64800 instead of PSR1 0x60800). | comment only | 3288-3290 |
| V111 | blit/3D (device start) | route `IGAccelDevice::deviceStart` -> `deviceStart` | Symbol hook that forces deviceStart to succeed regardless of BCS ring state (backup for the V46 byte pattern). Not fatal. | spoofed-only | 1896-1909 |
| V112 | blit/3D | routes `blit3d_initialize_scratch_space(IGAccelSysMemory*)`, `blit3d_init_ctx(IGHardwareBlit3DContext*)` | Wraps the Blit3D helpers (NootedBlue-derived) so the fallback path initializes scratch/context state. | always (TGL HW) | 1755-1758 |
| V117 | blit/3D | route `IGAccelTask::getBlit2DContext(bool)` | Null-guard only. It never substitutes other context types, because submitBlit reads [ctx+0xb8] assuming the Blit2D layout. | always (TGL HW) | 1820-1823 |
| V119 | blit/3D | route `IGAccelTask::getBlit3DContext(bool)` | Guard checks ctx+0xb8 before Apple stores the context at that+0x298 (otherwise submitBlit+0x28e crashes). | always (TGL HW) | 1750-1753 |
| V120 | blit/3D | route `IntelAccelerator::submitBlit(...)` -> `submitBlit` | Protects against a null context (blitCopy passes the getter result unchecked; +0x28e dereferences param_5+0xb8). Also hosts the V142 modes (body outside the range). | always (TGL HW) | 1787-1792 |
| V121 | blit/3D | route `IGAccelSegmentResourceList::initBlitUsage` | Guards the [member+0xb8] dereference at +0x17 (reached via coalesceSegment, prepare, initBlitUsage). | always (TGL HW) | 1794-1798 |
| V122 | blit/3D | route `IGAccelSegmentResourceList::markBlitUsage` | The same +0x17 dereference guard (called from prepare+0x2a). | always (TGL HW) | 1800-1802 |
| V123b | blit/3D | route `IGAccelSegmentResourceList::prepare` | Routes the existing early-return-0 prepare wrapper, which had been implemented but never routed. | always (TGL HW) | 1804-1806 |
| V124 | blit/3D | route `IGAccelCommandQueue::beginCoalescedSegment` | Guards the [member+0xb8] dereference at +0x2f. This is the lowest safe level; processCommandBuffer is not hooked. | always (TGL HW) | 1808-1814 |
| V126b | blit/3D | route `barrierSubmission(IGAccelCommandQueue&,...)` | Keeps the original contract; routed only to scope a temporary getter fallback. | always (TGL HW) | 1816-1818 |
| V132 | blit/3D | routes `IntelAccelerator::createUserGPUTask`, `IGAccelTask::withOptions` | Task producers hooked so submitBlit never sees a null IGAccelTask on spoofed RPL. | always (TGL HW) | 1739-1741 |
| V139 | blit/3D | byte patches `f_v139_movaps_{10,30,50,00,20,40}` | movaps to movups (0F 29 to 0F 11) in blit3d_submit_rectlist, because command pointers are only 8-byte aligned and aligned SSE stores GP-fault. 14 apply calls (Store00 with skip 0-5, Store20 with skip 0-3); the log says "/12". | spoofed-only, non-fatal | 2045-2084, 2285-2441 |
| V140 | blit/3D | byte patches `f_v140_{default,case4}_store{00,20}` | Contextual movaps to movups for the switch cases that store at [r9]/[r9+0x20], not covered by V139. | spoofed-only, non-fatal | 2092-2121, 2445-2450 |
| V141 | blit/3D | byte patch `f_v141_movapd_00` | movapd [r9] (66 41 0F 29 09) to movupd. | spoofed-only, non-fatal | 2085-2090, 2442-2444 |
| V142 (+diag2d) | blit/3D | helpers `getV142SubmitBlitMode()`, `isV142Diag2DBypassEnabled()` | Submit-blit mode: `ngreenV142=N` or `-ngreenV142orig` (3, original submission), `-ngreenV142pass` (2), `-ngreenV142ok` (1), `-ngreenV142hardunsupported` (0), `-ngreenV142unsupported` (1). Default is 1. The diag2d switch is `ngreenV142diag2d`. Modes are consumed in submitBlit and at L7342, outside the range. | default mode 1; others opt-in | 206-239, 270-276 |
| V143 | blit/3D | route `IGHardwareBlit3DContext::operator new` (`nwEm`) | Registers operator new so getBlit3DContext can allocate. Otherwise ctx stays 0, which floods submitBlit with null tasks and stalls the BCS ring (WAIT_ON_SCANLINE). | always (TGL HW) | 1767-1771 |
| V144 | blit/3D | solve `IGHardwareBlit3DContext::ExtendedCtxParams`; route `IGHardwareExtendedContext::initWithOptions` | Resolves the Blit3D extended-ctx param struct and hooks initWithOptions so init runs with it. Otherwise param_2=0, ok=0, ctx+0xb8 is NULL and submitBlit+0x28e crashes. | always (TGL HW; solve non-fatal) | 1694-1704, 1781-1784 |
| V163 | scheduler / power (context) | route `IntelAccelerator::startGraphicsEngine` (+ `stopGraphicsEngine`) | Clears PERCTX_PREEMPT_CTRL (FF_SLICE_CS_CHICKEN1 0x20E0 bit14; TGL writes 0x40004000) right after the TGL kext enables it. On RPL the snapshot of this bit in the context image freezes the EU dispatcher on the first context switch. | always route (body outside range) | 1713-1720 |
| V164 | scheduler | route `IntelAccelerator::populateResetRegisterList` | Clears 0x20E0 bit14 before the original snapshots live MMIO into the per-context MI_LRI replay list (otherwise 0x4000 is replayed on every switch). | always route | 1730-1736 |
| V165 | diagnostics/logging | SYSLOGs in the TGL HW block | Progress markers: "setRMMIO done", "routing N HW symbols", "routed OK". | always, diagnostic-only | 1692, 1830-1832 |
| V170 | blit/3D | comment and default in `getV142SubmitBlitMode` | Defaults V142 to mode 1 (submitBlit returns 0, no-op). RPL cannot run the TGL-format 3D blit: the RCS EU stalls (INSTDONE=0xfffffffe, bit0 stuck), leading to a hangcheck/reset loop that kills WindowServer within 120 s. The comment says real TGL uses mode 3, but this helper returns 1 regardless of platform. | always (default) | 223-229 |
| V183 | power/forcewake (display power wells) | route `AppleIntelPowerWell::hwSetPowerWellStatePGE(bool,uint)` (+Aux/DDI routes) | On spoofed hardware, a write-only handler for HSW_PWR_WELL_CTL1 0x45400: enable gives `(ctl1 & 0xFFFFFF55) \| (mask & 0xAA)`, disable gives `ctl1 & ~(mask & 0xAA)`, with no STATE polling (ADL-P ACKs never arrive within TGL's 20-iteration poll). Previously wrote CTL2 0x45404 by mistake. Real TGL runs the original with a ccont fixup. Aux/DDI: ccont fixup plus original. | spoofed-only (PGE) | 1177-1181, 3397-3439 |
| V195 / V195p | power (power wells) | `raWriteRegister32` @ 0x45400 | V195 used to OR bits 14 and 12 into CTL1 (when `uefiCtl1 != 0`). Removed; V195p logs (6). | removed; diagnostic-only (spoofed) | 3773-3782 |
| V195F / V195Fp | power | `FastWriteRegister32` @ 0x45400 | The FastWrite site of V195. Removed; Fp logs. | removed; diagnostic-only | 3577-3586 |
| V195W | power | comment | Named as the "same OR-in bits 14,12" site in another write helper. Not in this range. | comment only | 3773 |
| V201 | diagnostics | dbg-kext route `AppleIntelBaseController::hwSetupMemory` -> `wrapHwSetupMemory` | Reads the scanout buffer right after hwSetupMemory to see what fills it (zeros vs wallpaper). Body outside the range. (V201B pixel sampling is after L3800.) | debug-kext only (`!isprod`) | 1265-1267 |
| V204 | display/framebuffer | routes `AppleIntelScaler::init(IGScalerID)`, `AppleIntelPlane::init(IGPlaneID)`; bodies `AppleIntelScalerinit`/`AppleIntelPlaneinit` | Re-enables the init hooks so ccont (write accessor) is set at construction: Scaler fWriteAccessor=ccont and fController=ccont2; Plane +0x90=ccont (not +0x88 fRegCache). Per-method ccont fixups are also kept. | always (TGL FB) | 1111-1115, 2473-2487, 2535 |
| V205 (+PSR1/PSR2) | power/PSR + diagnostics | `wrapIsApertureMemoryRequired` (Path B route) | On every call (all platforms; only checks that NGreen::callback is non-null) clears PSR1 0x60800 bit0 and PSR2 0x60A10 bit0 when set, because PSR makes the panel ignore new SURF arms (frozen frame). Sampled snapshots at calls 1/2/5/10/20/30/40/44 (V205[c]). | always (behavioral) + diagnostic | 3288-3358 |
| V208 / V209 / V210 | display/framebuffer | commented routes `AppleIntelFramebuffer::start`, `::enableController`, `AppleIntelDisplayPath::getFreeJoinablePathCount` | Framebuffer-count-reduction hooks. Disabled at the 3/3/3 baseline because they cause a NULL dereference at IOFramebuffer::getAttributeExt+0x25 when fb1/fb2 start is refused. | commented-out | 1163-1169 |
| V211 | diagnostics | Path B sample | Also probes Pipe A planes 2/3 (0x711xx/0x721xx CTL/SURF/LIVE) and the cursor (0x70080/84/88) for the "frozen overlay" symptom. | diagnostic-only | 3336-3357 |
| V212 | scheduler | routes `IGScheduler5::isGpuIdle() const`, `IGScheduler4::isGpuIdle() const` | Return true when INSTDONE == 0xfffffffe (RPL-P idle with bit0 stuck). Otherwise the watchdog declares a hang and loops on reset and startGraphicsEngine. | always route (body outside range) | 1722-1728 |
| V300 | display (DSC) | comment in V402 | "V300's CRTCParams write-back" (DSC). Not in this range. | comment only | 2692 |
| V400 | display (scaler) | route `AppleIntelScaler::setupPipeScaler` -> `setupPipeScaler` | On spoofed hardware, before the original, zeroes the gate bytes fController+0x1E3 and +0x1E5 (the seam-joining scaler gate; +0x1E5 is used when [ctrl+0x3FD8]==0xFFFFFFFF, i.e. no device tree). This stops Apple from enabling seam joining/bigjoiner on a single-pipe eDP (Linux: bigjoiner no). It then logs PIPE_SEAM_EXCESS, PS_WIN_SZ/POS, HPHASE, SRCSZ and timings (12). The route comment calls it "read-only", but the body writes. | spoofed-only | 1124-1127, 2508-2587 |
| V401 | display (plane) | route `AppleIntelBaseController::paramsSurfCompare` | On spoofed hardware, if the original returns false while PLANE_SURF changed, force true (full reprogram; the SURF-only fast path never writes PLANE_SURF under the spoof). Zeroes p2 PIPE_SEAM_EXCESS and PS_PS_WIN_SZ after the call, because a nonzero DSB entry for PS_WIN_SZ_2_A 0x68274 (0x3fff04f) aborts the flip. Logs CTL tiling (bits[12:10]), STRIDE and SURF for old/new (20). | spoofed-only | 1128-1131, 2589-2687 |
| V402 | display (DSC) | route `AppleIntelBaseController::setupDSCEngineParams` | Read-only pre/post log of DSC_ENGINE_SEL (enabled if & 0xF0000000), DSC_JOINER_CTL, PPS_0/16 (6). Linux reports DSC off. | spoofed-only, diagnostic | 1132-1134, 2689-2723 |
| V403 / V403-zero | display (CRTC) | route `AppleIntelBaseController::SetupParams` | After the call, on spoofed hardware, zeroes PIPE_SEAM_EXCESS, PS_PS_WIN_SZ and PS_HPHASE if any is nonzero (seam-join causation test, active). Logs CLK_SEL, DDI_FUNC_CTL/2, MSA_MISC, H/V timings, SRCSZ, TRANS_CONF, DSC (6). | spoofed-only | 1135-1138, 2725-2769 |
| V404 | display (watermarks) | route `AppleIntelBaseController::setupPipeWatermarks` | Read-only pre/post log that checks whether this function sets PIPE_SEAM_EXCESS (4). | spoofed-only, diagnostic | 1139-1141, 2771-2804 |
| V405 | display (color) | route `AppleIntelPlane::configureColorPipeLine(FlipTransactionArgs*,bool)` | On spoofed hardware, logs GAMMA_MODE 0x4A480 and PIPE_MISC 0x70030 (bpc bits[5:3]) before and after, plus the selector byte at flipArgs+0x1C. Also (DIAG[BM]) clears bit10 of the color-pipeline bitmask at FB(+0x68)+0x4248 on every call (configurePipeHDRMode candidate behind PIPE_MISC bit23). | spoofed-only (behavioral bit10 clear + log 20) | 1142-1146, 2806-2868 |
| V406 (+V406_* macros, V406-final) | display (tiling) | route `AppleIntelPlane::configurePlane(FlipTransactionArgs*)` | Compile-time `V406_APPROACH_TILING_PATCH` with `V406_TILING_VALUE 0`: on spoofed hardware sets FlipTransactionArgs+0x3c (TilingEnum) to 0 (X-tiled: ORs 0x400 into PLANE_CTL) before the original and restores it after. Logs 20. The alternative approaches (POST_SHADOW, STRIDE_DIVISOR, DUAL, DIAGNOSTIC) are compiled out; `V406_STRIDE_FORCE_ENABLE 0`. The route comment says "enum 2 (linear)", but the code uses 0. | spoofed-only (approach 1); alternatives dead | 1147-1150, 2870-3078 |
| V408 | display (tiling/stride) | `paramsSurfCompare` | The original V408 block (pl2 PLANE_CTL bits[12:10]=0, STRIDE=0xa0) is commented out. The live successor (L2615-2631) runs when `!isRealTGL && pl2 && !xeFixEnabled()`: it forces pl2 to linear with stride srcW*4/64 (fork xe/nooted change), or with **fork** `-ngreenxtile` keeps X-tile with stride srcW*4/512. **Fork** `ngreenstride=N` overrides. | original commented-out; live form spoofed-only unless `-ngreenxefix` | 2596-2631 |
| V410 | display (sleep/wake) | routes `AppleIntelFramebuffer::prepareToExitWake/EnterWake/ExitSleep/EnterSleep` | Intercepts panel state tracking on sleep/wake (Apple's versions corrupt WS state on RPL/ADL resume). Bodies are outside the range. | always (TGL FB) | 1189-1194 |
| V508 | diagnostics (context/LRCA) | route `IGHardwareContext::withOptions(...)` | Inspects ctx+0xb8 (ring/FIFO allocation) and dumps LRCA page1 through wbinvd and the aperture after initWithOptions, to tell whether RING_START=0 is real or an LLC artifact. | always route, diagnostic | 1772-1775 |
| V509 | scheduler/execlists (context image) | route `IGHardwareContext::initWithOptions(...)` | Repairs LRCA page1 when restoreFromSafeImage skipped the g_cInitGfxRingContextRCS memcpy (DW1=0x00ffffff), by writing the Gen12 ring-context MI_LRI block so execlist context restore works on RPL. | always route (body outside range) | 1776-1779 |

#### 2. RouteRequestPlus / SolveRequestPlus entries

Kext blocks: **ICL FB** = kextG11FB (loaded only with `-ngreenicl`, skipped if the TGL FB already loaded); **TGL FB** = kextG11FBT/FBTA (`-ngreentglfb` or `-ngreentglwithgfx`); **ICL HW** = kextG11HW (`-ngreenicl`, skipped if the TGL HW loaded); **TGL HW** = kextG11HWT/HWTA (`-ngreentglwithgfx` or `-ngreentglgfx`). `isprod` = the TGL FB lacks `__ZN24AppleIntelBaseController5startEP9IOService`, so the production kext uses the `31AppleIntelFramebufferController` names; otherwise it is the debug kext with `24AppleIntelBaseController` names. Unless noted, routes are PANIC on failure.

##### 2a. Solve requests
| Symbol | -> target | Block | Status / gating | Lines |
|---|---|---|---|---|
| `__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb` | ohwConfigureCustomAUX | ICL FB | active (both wegCoexist branches) | 942, 952 |
| `__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv` | orgProbeCDClockFrequency | ICL FB | active only if !wegCoexist | 953 |
| disableCDClock / setCDClockFrequencyEy / IntelFBClientControl::doAttribute / start / ReadRegister32Em (AIFC) | org* | ICL FB | commented | 937-941, 947-951 |
| `__ZN31AppleIntelFramebufferController19setCDClockFrequencyEy` | orgSetCDClockFrequency | TGL FB | isprod | 1079 |
| `__ZN24AppleIntelBaseController19setCDClockFrequencyEy` | orgSetCDClockFrequency | TGL FB | dbg | 1087 |
| `_gPlatformInformationList` | gPlatformInformationList | TGL FB | both prod/dbg | 1080, 1088 |
| `__ZN24AppleIntelBaseController35hwCRTCToIODetailedTimingInformationEP21AppleIntelFramebufferP29IODetailedTimingInformationV2` | gHwCrtcToTimingAddr | TGL FB | **fork** `-ngreenfixbootmode`, non-fatal | 1214-1226 |
| `__ZN23IGHardwareBlit3DContext17ExtendedCtxParamsE` | Blit3DExtendedCtxParams | TGL HW | always, non-fatal (V144) | 1699-1703 |
| `__ZNK20IGSharedMappedBuffer17getVirtualAddressEv` | gSmbGetVA | TGL HW | **fork** `-ngreenxefix`, non-fatal | 1861 |
| kernel `_pmap_find_phys`, `_kernel_pmap` (solveSymbol) | gPmapFindPhys, gKernelPmap | kernel | **fork** `-ngreenxefix` | 1862-1865 |

##### 2b. Route requests — ICL FB
| Mangled symbol | Wrapper | Status / gating | Lines |
|---|---|---|---|
| `__ZN31AppleIntelFramebufferController18hwInitializeCStateEv` | hwInitializeCState | active (both branches) | 973, 993 |
| `__ZN31AppleIntelFramebufferController11initCDClockEv` | initCDClock | active only !wegCoexist (`ngwegcoex` off) | 996 |
| AIFC::ReadRegister32Em -> wrapReadRegister32; AppleIntelFramebuffer::SaveNVRAMMode -> handleLinkIntegrityCheck; prepareToEnterWake/ExitWake/ExitSleep/EnterSleep -> dovoid; IntelFBClientControl::doAttribute -> wrapFBClientDoAttribute; AppleIntelFramebuffer::init(AIFC*,j) -> AppleIntelFramebufferinit; AIFC::hwShutdown -> handleLinkIntegrityCheck; AIFC::hwConfigureCustomAUX -> hwConfigureCustomAUX; AIFC::probeCDClockFrequency -> wrapProbeCDClockFrequency | — | commented (both branches) | 960-975, 980-995 |

##### 2c. Route requests — TGL FB (common, always when TGL FB loads)
| Mangled symbol | Wrapper | Tag / note | Lines |
|---|---|---|---|
| `__ZN16AppleIntelScaler4initE10IGScalerID` | AppleIntelScalerinit | V204 | 1114 |
| `__ZN15AppleIntelPlane4initE9IGPlaneID` | AppleIntelPlaneinit | V204 | 1115 |
| `__ZN16AppleIntelScaler13disableScalerEb` | disableScaler | ccont fixup | 1121 |
| `__ZN15AppleIntelPlane11enablePlaneEb` | enablePlane | ccont fixup | 1122 |
| `__ZN16AppleIntelScaler17programPipeScalerEP21AppleIntelDisplayPath` | programPipeScaler | ccont fixup | 1123 |
| `__ZN16AppleIntelScaler15setupPipeScalerEP21AppleIntelDisplayPathP10CRTCParams` | setupPipeScaler | V400 | 1127 |
| `__ZN24AppleIntelBaseController17paramsSurfCompareEP10CRTCParamsS1_PN15AppleIntelPlane11PLANEPARAMSES4_` | paramsSurfCompare | V401/V408 | 1131 |
| `__ZN24AppleIntelBaseController20setupDSCEngineParamsEP21AppleIntelFramebufferP10CRTCParamsP21AppleIntelDisplayPathP29IODetailedTimingInformationV2` | setupDSCEngineParams | V402 | 1134 |
| `__ZN24AppleIntelBaseController11SetupParamsEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2` | setupParams | V403 | 1138 |
| `__ZN24AppleIntelBaseController19setupPipeWatermarksEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParams` | setupPipeWatermarks | V404 | 1141 |
| `__ZN15AppleIntelPlane22configureColorPipeLineEP19FlipTransactionArgsb` | configureColorPipeLine | V405 | 1146 |
| `__ZN15AppleIntelPlane14configurePlaneEP19FlipTransactionArgs` | configurePlane | V406 | 1150 |
| `__ZN15AppleIntelPlane19updateRegisterCacheEv` | AppleIntelPlaneupdateRegisterCache | ccont fixup | 1151 |
| `__ZN16AppleIntelScaler19updateRegisterCacheEv` | AppleIntelScalerupdateRegisterCache | ccont fixup | 1152 |
| `__ZN19AppleIntelPowerWell20disableDisplayEngineEv` | disableDisplayEngine | ccont @+0x78 | 1153 |
| `__ZN19AppleIntelPowerWell19enableDisplayEngineEv` | enableDisplayEngine | ccont @+0x78 | 1154 |
| `__ZN14AppleIntelPort16computeLaneCountEPK29IODetailedTimingInformationV2jjPj` | computeLaneCount | V90L4 | 1156 |
| `__ZN14AppleIntelPort21setupOptimalLaneCountEPK29IODetailedTimingInformationV2j` | setupOptimalLaneCount | V90L5 | 1157 |
| `__ZN14AppleIntelPort7readAUXEjPvj` | wrapICLReadAUX | V97/V98/V98T/V99 | 1159 |
| `__ZN21AppleIntelFramebuffer13getOnlineInfoEP21AppleIntelDisplayPathPhS2_` | getOnlineInfo | V96/V96p | 1162 |
| `__ZN21AppleIntelFramebuffer24isApertureMemoryRequiredEv` | wrapIsApertureMemoryRequired | Path B, V205/V211 | 1173 |
| `__ZN21AppleIntelFramebuffer12setAttributeEjm` | wrapSetAttribute | Path C | 1176 |
| `__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj` | hwSetPowerWellStatePGE | V183 | 1179 |
| `__ZN19AppleIntelPowerWell22hwSetPowerWellStateAuxEbj` | hwSetPowerWellStateAux | ccont fixup | 1180 |
| `__ZN19AppleIntelPowerWell22hwSetPowerWellStateDDIEbj` | hwSetPowerWellStateDDI | ccont fixup | 1181 |
| `__ZN31AppleIntelRegisterAccessManager19FastWriteRegister32Emj` | FastWriteRegister32 | V72Fp/V99D/V99F/V103Fp/V195Fp + fork xefix/inert | 1182 |
| `__ZN31AppleIntelRegisterAccessManager15WriteRegister32Emj` | raWriteRegister32 | V93/V97Rp/V99R/V99S/V103p/V195p + fork | 1187 |
| `__ZN31AppleIntelRegisterAccessManager15WriteRegister32EPVvmj` | raWriteRegister32b (no org) | forwards base+offset | 1188 |
| `__ZN21AppleIntelFramebuffer17prepareToExitWakeEv` / `18prepareToEnterWakeEv` / `18prepareToExitSleepEv` / `19prepareToEnterSleepEv` | prepareToExitWake / prepareToEnterWake / prepareToExitSleep / prepareToEnterSleep | V410 | 1191-1194 |
| `__ZN24AppleIntelBaseController15enableVDDForAuxEP14AppleIntelPort` | releaseDoorbell (no org; stub) | purpose not stated in range | 1195 |
| `__ZN21AppleIntelFramebuffer19validateDisplayModeEiPPKNS_15ModeDescriptionEPPK29IODetailedTimingInformationV2` | validateDisplayMode | — | 1201 |
| `__ZN21AppleIntelFramebuffer21getCurrentDisplayModeEPiS0_` | getCurrentDisplayMode | — | 1202 |
| `__ZN21AppleIntelFramebuffer19getPixelInformationEiiiP18IOPixelInformation` | getPixelInformation | — | 1207 |
| Commented: `AppleIntelBaseController::registerWithAICPM` -> alwaysReturnSuccess; `hwSetPowerWellStatePGEbj.cold.1..12` and `hwSetPowerWellStateAuxEbj.cold.1` -> releaseDoorbell; `AppleIntelFramebuffer::start` / `::enableController` / `AppleIntelDisplayPath::getFreeJoinablePathCount` (V208/V209/V210); `RAM::ReadRegister32Em` / `EPVvm` (V60); `SetupDPSSTTimings`, `SetupTimings`, `validateDetailedTiming`, `setupDisplayTiming`, `maxSupportedDepths`, `validateModeDepth`, `IntelFBClientControl::doAttribute`, `vendor_doDeviceAttribute`, `enableController` -> isPanelPowerOn | — | commented-out | 1096-1110, 1163-1169, 1183-1186, 1196-1210 |

##### 2d. Route requests — TGL FB, prod (isprod) vs dbg
| prod symbol (AIFC = `31AppleIntelFramebufferController`) | dbg symbol (`24AppleIntelBaseController`) | Wrapper | Lines |
|---|---|---|---|
| `__ZN21AppleIntelFramebuffer4initEP31AppleIntelFramebufferControllerj` | `__ZN21AppleIntelFramebuffer4initEP24AppleIntelBaseControllerj` | AppleIntelFramebufferinit | 1233 / 1252 |
| `...23initPlatformWorkaroundsEv` | same on BaseController | initPlatformWorkarounds | 1234 / 1253 |
| `...16getOSInformationEv` | same | getOSInformation | 1235 / 1254 |
| `...10hwShutdownEP21AppleIntelFramebuffer` | same | handleLinkIntegrityCheck (no org) | 1236 / 1255 |
| `...18hwInitializeCStateEv` | same | hwInitializeCState | 1237 / 1256 |
| `...20hwConfigureCustomAUXEb` | same | hwConfigureCustomAUX | 1238 / 1257 |
| `__ZN19AppleIntelPowerWell4initEP31AppleIntelFramebufferController` | `...4initEP24AppleIntelBaseController` | AppleIntelPowerWellinit | 1239 / 1258 |
| `...5startEP9IOService` | same | AppleIntelBaseControllerstart | 1240 / 1259 |
| `...21probeCDClockFrequencyEv` | same | wrapProbeCDClockFrequency | 1241 / 1260 |
| `...11initCDClockEv` | same | initCDClock | 1242 / 1261 |
| `...28setCDClockFrequencyOnHotplugEv` | same | setCDClockFrequencyOnHotplug | 1243 / 1262 |
| `...14disableCDClockEv` | same | disableCDClock | 1244 / 1263 |
| `...16hwRegsNeedUpdateEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2PN16AppleIntelScaler12SCALERPARAMSE` | same | hwRegsNeedUpdate | 1245 / 1264 |
| — | `__ZN24AppleIntelBaseController13hwSetupMemoryEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsb` | wrapHwSetupMemory (V201, dbg only) | 1267 |

##### 2e. Route requests — ICL HW
| Mangled symbol | Wrapper | Status / gating | Lines |
|---|---|---|---|
| `__ZN13IGHardwareGuC13loadGuCBinaryEv` | loadGuCBinary | always (WEG FW path is Mojave-gated) | 1594-1597 |
| `__ZN16IntelAccelerator19PAVPCommandCallbackE22PAVPSessionCommandID_tjPjb` | wrapPavpSessionCallback | !wegCoexist | 1603 |
| `__ZN16IntelAccelerator10getGPUInfoEv` | getGPUInfoICL | !wegCoexist (ICL offsets) | 1625 |
| Commented: `IGScheduler5::resume`, `IGHardwareRingBuffer::resetGraphicsEngine` (symbol absent), `IGHardwareGuC::checkWOPCMSettings`, `IGScheduler::canLoadFirmware`, `IntelAccelerator::readAndClearInterrupts` (V36/V37); initHardwareCaps deliberately not routed (ICL SKU at 0x1150 vs TGL 0x1120) | — | commented | 1604-1618 |

##### 2f. Route requests — TGL HW
| Mangled symbol | Wrapper | Tag / gating | Lines |
|---|---|---|---|
| `__ZN16IntelAccelerator20_PAVPCommandCallbackEP8OSObject22PAVPSessionCommandID_tjPj` | wrapPavpSessionCallback | always | 1711 |
| `__ZN16IntelAccelerator19startGraphicsEngineEv` | startGraphicsEngine | V163, always | 1719 |
| `__ZN16IntelAccelerator18stopGraphicsEngineEv` | stopGraphicsEngine | V163 block, always | 1720 |
| `__ZNK12IGScheduler59isGpuIdleEv` | wrapIGScheduler5IsGpuIdle | V212 | 1727 |
| `__ZNK12IGScheduler49isGpuIdleEv` | wrapIGScheduler4IsGpuIdle | V212 | 1728 |
| `__ZN16IntelAccelerator25populateResetRegisterListEv` | populateResetRegisterList | V164 | 1736 |
| `__ZN16IntelAccelerator17createUserGPUTaskEv` | createUserGPUTask | V132 | 1740 |
| `__ZN11IGAccelTask11withOptionsEP16IntelAccelerator` | igAccelTaskWithOptions | V132 | 1741 |
| `__ZN11IGAccelTask16getBlit3DContextEb` | getBlit3DContext | V119 | 1753 |
| `__Z31blit3d_initialize_scratch_spaceP16IGAccelSysMemory` | blit3d_initialize_scratch_space | V112 | 1757 |
| `__Z15blit3d_init_ctxP23IGHardwareBlit3DContext` | blit3d_init_ctx | V112 | 1758 |
| `__ZN23IGHardwareBlit3DContext10initializeEv` | IGHardwareBlit3DContextinitialize | V69 | 1762 |
| `__ZN15IGAccelResource4freeEv` | IGAccelResourcefree | **fork** XE-RESFREE | 1764 |
| `__ZN26IGHardwareCommandStreamer518setGTFrequencyMMIOEj` | wrapSetGTFrequencyMMIO | **fork** XE-GTFREQ | 1766 |
| `__ZN23IGHardwareBlit3DContextnwEm` | IGHardwareBlit3DContextoperatornew | V143 | 1771 |
| `__ZN17IGHardwareContext11withOptionsEP11IGAccelTaskRK23IGHardwareContextParamsh` | IGHardwareContextwithOptions | V508 | 1775 |
| `__ZN17IGHardwareContext15initWithOptionsEP11IGAccelTaskRK23IGHardwareContextParamsh` | IGHardwareContextinitWithOptions | V509 | 1779 |
| `__ZN25IGHardwareExtendedContext15initWithOptionsEP11IGAccelTaskRK31IGHardwareExtendedContextParams` | IGHardwareExtendedContextinitWithOptions | V144 | 1784 |
| `__ZNK14IGMappedBuffer9getMemoryEv` | IGMappedBuffergetMemory | no tag | 1785 |
| `__ZN16IntelAccelerator10submitBlitEP15blit3d_params_tRK8IGVectorI11rect_pair_t25IGIOMallocAllocatorPolicyEP11IGAccelTaskb` | submitBlit | V120 (V142 modes) | 1792 |
| `__ZN26IGAccelSegmentResourceList13initBlitUsageEv` | initBlitUsage | V121 | 1798 |
| `__ZN26IGAccelSegmentResourceList13markBlitUsageEv` | markBlitUsage | V122 | 1802 |
| `__ZN26IGAccelSegmentResourceList7prepareEv` | IGAccelSegmentResourceListprepare | V123b | 1806 |
| `__ZN19IGAccelCommandQueue21beginCoalescedSegmentEv` | beginCoalescedSegment | V124 | 1814 |
| `__Z17barrierSubmissionR19IGAccelCommandQueueR16IntelAcceleratorR24IGAccelCommandDescriptorR12IOAccelEventtPKt` | barrierSubmission | V126b | 1818 |
| `__ZN11IGAccelTask16getBlit2DContextEb` | getBlit2DContext | V117 | 1823 |
| `__ZN11IGAccelTask22getDepthResolveContextEb` | getDepthResolveContext | no tag | 1826 |
| `__ZN11IGAccelTask22getColorResolveContextEb` | getColorResolveContext | no tag | 1827 |
| `IntelAccelerator::readAndClearInterrupts(void*)` | readAndClearInterrupts | V36/V37 — commented | 1748 |
| **fork** flipDiag (non-fatal, `-ngreenxefix`): `IGAccelDisplayPipe::submitTransaction`, `::isTransactionComplete`, `::signalTransactionComplete`, `IntelAccelerator::signalDisplayStampUpdate`, `IGScheduler5::push`, `::notify`, `::pause`, `::resume`, `IGHardwareCommandStreamer5::submitExecList` | xeDpSubmit … xeS5Resume | fork diagnostics | 1835-1845 |
| **fork** ringDiag (non-fatal, `-ngreenxefix`): `IGHardwareRingBuffer::submitToRing`, `::writeStamp`, `::insertSemaphoreWaitPoll`; `IGHardwareCommandStreamer5::processCSBDetail`, `::csbProcessActiveToIdle`, `::csbProcessPreempted`, `::csbProcessElementSwitch`, `::evalWaitQueue`, `::prepareExecListAndSubmit`; `IGAccelResource::pageoff`, `::pageon`; `IGScheduler5::verifyThrottling`; `IGHardwareContext::restoreFromSafeImage` | xeRbSubmit … xeRestoreSafe | fork (contains the default-on CSB guard, ELSQ waits, POSH fixes, pageon guard) | 1846-1860 |
| `__ZN16IntelAccelerator5startEP9IOService` | start | V44, non-fatal | 1878 |
| `__ZN13IGHardwareGuC13loadGuCBinaryEv` | loadGuCBinary | always | 1891 |
| `__ZN13IGAccelDevice11deviceStartEv` | deviceStart | V111, spoofed-only, non-fatal | 1902 |
| `__ZN16IntelAccelerator26SafeForceWakeMultithreadedEbjj` | forceWake | !wegCoexist \|\| `ngreenfullmtlcore` (i915-ported forcewake: 50 ms + reserve-bit fallback; fixes Apple's d<<1 domain mapping, 90 ms timeouts) | 1916 |
| `__ZN16IntelAccelerator10getGPUInfoEv` | getGPUInfo | !wegCoexist | 1926 |

#### 3. LookupPatchPlus byte patches (count = max replacements; "skip" = occurrences skipped)

##### 3a. ICL FB (`minPatches`, kextG11FB, PANIC on failure, always when ICL FB loads)
| Arrays | Find -> Replace | Count | Purpose | Lines |
|---|---|---|---|---|
| kPatchPlatformRemapMovEaxFind0/Replace0 | `B8 00 00 5C 8A` -> `B8 00 00 49 9A` | 1 | Platform-ID remap in constructor entries: 0x8A5C0000 -> 0x9A490000 | 1036-1037, 1051 |
| ...Find1/Replace1 | `B8 01 00 5C 8A` -> `B8 01 00 49 9A` | 1 | same (index 1) | 1038-1039, 1052 |
| ...Find2/Replace2 | `B8 02 00 5C 8A` -> `B8 02 00 49 9A` | 1 | same (index 2) | 1040-1041, 1053 |
| kPatchPlatformRemapC705Find2/Replace2 | `C7 05 E9 9B 05 00 02 00 5C 8A` -> `C7 05 E9 9B 05 00 02 00 49 9A` | 1 | same, at the exact `C7 05` store site | 1042-1043, 1054 |
| kPatchHwRegsNeedUpdateBypassFind/Replace | `E8 E2 CC FF FF 84 C0 74 62` -> `... 84 C0 EB 62` | 1 | hwSetMode: ignore the hwRegsNeedUpdate result and always reprogram (unique at 0x94055 in ICL-LP) | 1047-1048, 1055 |
| f2/r2, f2b/r2b, f2c/r2c (Ventura/Sonoma/Sequoia hwSetMode `74 3D`/`74 5C` -> `EB`) | `E8 31 E5 FF FF 84 C0 74 3D`->`EB 3D`; `E8 54 EA FF FF 84 C0 74 5C`->`EB 5C`; `E8 74 EA FF FF 84 C0 74 5C`->`EB 5C` | 1 | Same bypass for older OSes; the application block is commented out (dead). f15/r15 are commented out entirely. | 1001-1030 |

##### 3b. TGL FB (activeKext = FBT or FBTA)
| Arrays | Find -> Replace | prod (`patchesp`) | dbg (`patches`) | Purpose | Lines |
|---|---|---|---|---|---|
| f1p/r1p | `e8 66 b0 fe ff 89 45 c8 3d ff ff 00 00 74 45` -> `...eb 45` | commented | — | power-well JZ->JMP (not in NootedBlue) | 1277-1278, 1456 |
| f1/r1 | `e8 99 9f fd ff 89 45 c8 3d ff ff 00 00 74 78` -> `...eb 78` | — | commented | same, debug kext | 1274-1275, 1502 |
| f2p/r2p | `c7 05 57 e5 0b 00 01 03 09 03` -> `c7 05 57 e5 0b 00 01 04 03 02` | **1 (active)** | — | "osinfo" dword store 0x03090301 -> 0x02030401 (comment lists fInfoHasLid/PipeCount/PortCount/FramebufferCount; exact byte-to-field mapping not stated) | 1289-1290, 1457 |
| f2/r2 | `c7 05 07 81 10 00 01 03 09 03 b8 00 00 00 04` -> `...01 04 03 02 b8...` | — | not applied (comment: counts now set by the getOSInformation hook) | debug osinfo | 1286-1287, 1503 |
| f2b/r2b, f2c/r2c | `49 be 00 00 00 00 02 00 00 00 4c 89 35 17 81 10 00 b8 08 00 00 00` -> `...01 00 00 00...`; `48 89 1d c0 81 10 00 4c 89 35 c1 81 10 00 48 b8 08 00 00 00 01 00 00 00` -> `48 b8 05 ...` | — | — | defined, never applied (dead); purpose unclear | 1292-1296 |
| f2dp/r2dp | `b8 00 00 10 00 ba 00 00 80 00 0f 44 d0 0f 94 c1 48 01 0d a2 d0 09 00` -> `...ba 00 00 40 00...` | **1 (active)** | — | "mem": immediate 0x800000 -> 0x400000 (8 MB -> 4 MB) | 1303-1304, 1458 |
| f2d/r2d | `0f 94 c0 b9 00 00 10 00 ba 00 00 80 00` -> `...ba 00 00 40 00` | — | not applied | debug "mem" twin | 1299-1300 |
| f2e/r2e | `48 c7 83 60 43 00 00 00 2d 31 01 48 c7 83 68 43 00 00 00 54 ea 2a c6 83 b4 45 00 00 00` -> `48 c7 83 60 4a 00 00 00 a3 02 00 48 c7 83 68 4a 00 00 00 f6 09 00 c6 83 b4 45 00 00 00` | — | — | "cdclock" constants; defined, never applied (dead) | 1307-1308 |
| f3/r3 | 216-byte connector table (9 entries) -> 3 entries (2nd entry flag 0x400 -> 0x800, rest zeroed) | commented | commented | "conn" table rewrite (not in NootedBlue) | 1313-1333, 1460, 1505 |
| f4/f4a/f4b/f4c | `00 72 0c 00`/`04 72 0c 00`/`08 72 0c 00`/`0c 72 0c 00` -> `00 12 06 00`/`04 12 06 00`/`08 12 06 00`/`0c 12 06 00` | commented (11/11/2/2) | commented (12/11/2/2) | "lcd power reg" (offsets 0xC72xx -> 0x612xx); suspected cause of fragmentation on Display 13 | 1337-1347, 1465-1468, 1510-1513 |
| f6a/r6a | `be 04 00 00 00 48 89 da 31 c9 e8 8c ac 04 00` -> call NOPed (5×90) | — | commented | "jalavoui" | 1351-1352, 1514 |
| f7p/r7p | `83 c0 fc 48 39 f0 76 11 48 8b 47 50 48 ff 05 84 40 08 00` -> `83 c0 f8 ...` | **1 (active)** | — | ReadRegister64 bounds check `add eax,-4` -> `-8` | 1358-1359, 1469 |
| f7/r7 | `83 c0 fc ... 48 ff 05 ca f5 0c 00` -> `83 c0 f8 ...` | — | commented | debug twin | 1355-1356, 1515 |
| f10/r10 | `e8 af e2 ff ff 84 c0 74 5b` -> `...eb 5b` | — | **1 (active)** | "hwreg" CALL+JZ -> JMP (hwRegsNeedUpdate-style bypass). The comment above it says "commented out", but the entry is active. | 1363-1364, 1517 |
| f10p/r10p | `e8 9e f3 ff ff 84 c0 74 3d` -> `...eb 3d` | commented | — | prod twin | 1366-1367, 1471 |
| f13/r13 | `ff 91 90 01 00 00 83 f8 02 0f 84 ec 00 00 00` -> 15×`90` | — | **1 (active)** | probe-port-mode bypass ("mandatory: without it the spoofed TGL path freezes during boot") | 1373-1374, 1519 |
| f13b/r13b | `ff 90 90 01 00 00 49 8b 0e 4c 89 f7 89 c6 ff 91 38 01 00 00` -> `c7 c0 01 00 00 00 ...` | — | commented (comment still says "mandatory": probePortState pure-virtual panic) | vcall replaced by `mov eax,1` | 1370-1371, 1520-1522 |
| f13p/r13p | `ff 91 78 01 00 00 83 f8 02 74 64` -> 11×`90` | **1 (active)** | — | prod probe-port bypass | 1376-1377, 1477 |
| f13pb/r13pb | `ff 90 78 01 00 00 49 8b 0e 4c 89 f7 89 c6 ff 91 38 01 00 00` -> `c7 c0 01 00 00 00 ...` | **1 (active)** | — | prod: vcall +0x178 -> `mov eax,1` | 1379-1380, 1478 |
| f15/r15 | `74 36 48 ff 05 7e 51 08 00 44 89 3c 24 48 8d 15 4d 88 03 00 4c 8d 05 28 8a 03 00` -> `eb 36 ...` | — | **1 (active)** | suppresses the getPathByPipe "pipe = 0" log flood at IGLogLevel=8 (cosmetic) | 1384-1385, 1528 |
| f16/r16, f16p/r16p | `...f6 40 14 08 75 0d` -> `90 90`; `48 8b 80 48 05 00 00 f6 40 14 08 75 0a` -> `...90 90` | commented | commented | getBuiltInPort | 1388-1392, 1475, 1529 |
| f19/r19 | `be 70 44 04 00` -> `be a0 38 16 00` | **1 (active)** | **1 (active)** | getHPDState: `mov esi,0x44470` -> `0x1638a0` (HPD register/constant remap; exact semantics not stated) | 1395-1396, 1479, 1530 |
| f20p/r20p | `ff 90 f8 09 00 00 41 89 c6 48 85 db 74 17` -> 9×`90` + `48 85 db 74 17` | **1 (active)** | — | savenvram: NOP vcall +0x9f8 | 1402-1403, 1480 |
| f20/r20 | identical bytes | — | commented | debug twin | 1399-1400, 1531, 1536 |
| f21/r21, f21p/r21p | `0f 84 96 00 00 00 ... be 44 00 13 00 4c 89 f7` -> `48 e9 96...`; `74 3c 48 ff 05 7a 73 09 00 be 44 00 13 00 4c 89 f7 e8 97 80 01 00` -> `eb 3c...` | — | f21 commented; f21p never referenced | SafeForceWake skip | 1406-1410, 1532 |
| f22/r22 | `48 69 c2 50 c3 00 00 49 89 47 28 ... 84 c0 74 3a` -> `48 c7 c0 c0 40 d0 2e ... 84 c0 90 90` | — | commented | "pixel hwcrt": imul by 50000 replaced with constant 0x2ed040c0 | 1413-1415, 1533 |
| f6nb/r6nb | `74 2a 83 f8 01 74 43 85 c0 75 60` -> `90 90 83 f8 01 90 90 85 c0 90 90` | **1 (active)** | commented | force eDP panel detection regardless of pipe (pinfo puts eDP on pipe 0) — from NootedBlue | 1421-1422, 1476, 1534 |
| f24bp/r24bp | `83 78 08 00 75 0c` -> `83 78 08 00 eb 0c` | **14 (active)** | — | always use pipe-0 register offsets | 1425-1426, 1481 |
| f24cp/r24cp | `00 4c 89 ea 75 12` -> `00 4c 89 ea eb 12` | **1 (active)** | — | same | 1427-1428, 1482 |
| f24dp/r24dp | `83 78 08 00 75 0d` -> `83 78 08 00 eb 0d` | **4 (active)** | — | same | 1429-1430, 1483 |
| f24b/r24b | `83 78 08 00 75 0c` -> `83 78 08 00 eb 0c` | — | **11 (active)** | same (debug) | 1431-1432, 1537 |
| f24c/r24c, f24d/r24d | `48 8b 55 d0 75 13` -> `eb 13`; `83 78 08 00 75 0d` -> `eb 0d` | — | commented (1 / 6) | same | 1433-1436, 1538-1539 |
| f25/r25 | `77 77 00 00` -> `33 00 00 00` | **6 (active)** | commented | link-training speed constant fix | 1438-1439, 1484, 1540 |
| f_tcon_camellia / f_tcon_banksia | `3d 11 0a 84 41` -> `3d 14 1e c4 c1`; `3d 12 14 c4 41` -> `3d 14 1e c4 c1` | 1 each | 1 each | Path E: rewrite CamelliaTcon2/BanksiaTcon DPCD-ID compares to this panel's 0xc1c41e14. Gated by `-ngreentglwithgfx`, not fatal ("DO NOT enable FB-only: AGDC hang"). | 1441-1449, 1489-1496, 1545-1552 |
| fbm/rbm (**fork** applyFixBootMode) | `c7 83 50 42 00 00 fb ff ff ff` -> `c7 83 50 42 00 00 00 10 00 80` | 1 | 1 | fCurrentMode initial 0xfffffffb -> 0x80001000 (offset 0x53de3 in 16.0.32). Opt-in `-ngreenfixbootpatch`, not fatal. | 302-310, 1488, 1544 |
| f_vopt / f_vpopt / f_vfmt (**fork** XE-VALD) | `a9 00 05 00 00 74 17` -> `a9 00 00 00 00 74 17`; `a9 00 05 00 00 74 1e` -> `a9 00 00 00 00 74 1e`; `8b 80 a4 00 00 00 89 45 b4` -> 9×`90` | 1 each | 1 each | ValidateTransaction: drop the &0x500 option checks and the IOSurface+0xa4 format read (offsets 0x3ecf1/0x3ed1d/0x3ed88). Always, not fatal. | 1555-1576 |

prod PANICs on failure of `patchesp`; dbg PANICs on failure of `patches`.

##### 3c. ICL HW (kextG11HW, each applied individually with PANIC, always)
| Arrays | Find -> Replace | Count | Purpose | Lines |
|---|---|---|---|---|
| fSKUGates12/rSKUGates12 | `83 F9 01 0F 85 0B 01 00 00 FF C8 83 F8 07 0F 87 00 01 00 00 48 8D 0D 77 02 00 00 48` -> `83 F9 01 90×6 31 C0 83 F8 07 90×6 48 8D 0D 77 02 00 00 48` | 1 | SKU gates 1+2: NOP JNZ/JA, `dec eax` -> `xor eax,eax` (Sonoma ICL) | 1631-1646, 1669 |
| fSKUGate3/rSKUGate3 | `83 F8 08 0F 85 C2 00 00 00 C7` -> `83 F8 08 90×6 C7` | 1 | SKU gate 3 NOP JNZ | 1649-1654, 1670 |
| fSkuBypassLong/rSkuBypassLong | `48 85 C0 74 72 48 0F BC C0 ... 48 6B C9 79` -> `48 85 C0 EB 72 ...` | 1 | SKU bypass JZ->JMP (KC 0x14152bcd) | 1657-1666, 1671 |
| patchesc (Catalina) | — | — | commented-out | 1674-1677 |

##### 3d. TGL HW (activeKext = HWT or HWTA)
| Arrays | Find -> Replace | Count | Gating | Purpose | Lines |
|---|---|---|---|---|---|
| f3/r3 | `8b 3e 81 ff ee be af de 7f 15 81 ff 86 80 40 9a 74 2d` -> `8b 3e 81 ff ee be af de 90 90 81 ff 86 80 40 9a eb 2d` | 1 | always (PANIC) | SKU/device-ID panic bypass at 0x23c1d: spoofed 0x9A498086 is not in the whitelist (0x9A408086/0x9A488086); NOP the sentinel `jg`, last `je` -> `jmp` | 1931-1940, 2129 |
| f3a/r3a | `41 c7 86 20 11 00 00 02 00 00 00 e9 da fc ff ff` -> `...01 00 00 00...` | — | never applied | GT1 override at IGAccelDevice+0x1120 (disabled: 0x9A49 is GT2) | 1941-1948 |
| f3b/r3b | `74 23 83 f9 02 0f 85 89 01 00 00 83 fe 01 75 59 83 fa 0c 75 54 41 c7 87 64 11 00 00 08 00 00 00` -> NOP the 4 conditional jumps | 1 | spoofed-only (V52, PANIC) | always store L3BankCount=8 at +0x1164 (at 0x28776) | 1950-1958, 2137 |
| f3bb/r3bb | `be 08 00 00 00 29 de 41 89 b7 6c 11 00 00 41 8b 8f 58 11 00 00` -> `be 08 00 00 00 90 90 ...` | 1 | spoofed-only (V52) | MaxEUPerSubSlice=8 at +0x116c (drop `sub esi,ebx` of fuse popcount; at 0x28692) | 1960-1970, 2138 |
| f3bbb/r3bbb | `8b 5d d0 f3 0f b8 f3 01 f6 41 89 b7 58 11 00 00` -> `8b 5d d0 be 0c 00 00 00 90 41 89 b7 58 11 00 00` | 1 | spoofed-only (V52) | NumSubSlices=12 (6 DSS × 2) at +0x1158 (at 0x28654) | 1972-1982, 2139 |
| f4/r4 | `c7 83 48 11 00 00 06 ... be 06 00 00 00` -> `...05...90 90 ba 01...be 05...` | — | never applied | CAPS: MaxSlices 6->5, SARation 2->1, MaxEU/SS 6->5 (disabled) | 1984-1991 |
| f_devstart (masked m_devstart / rm_devstart) | `ff 90 70 09 00 00 84 c0 74 ?? 48 8d ??` -> `... 84 c0 90 90 48 8d ??` | 1 | spoofed-only (V52, PANIC) | V46 deviceStart BCS readiness bypass | 2007-2022, 2140 |
| f_devstart_long (masked) | `ff 90 70 09 00 00 84 c0 0f 84 ?? ?? ?? ??` -> `...84 c0 90×6` | 1 | commented-out (V52 optional) | long-form variant | 2028-2043, 2269-2282 |
| f_v139_movaps_10/30/50/00/20/40 | `41 0f 29 51 10 0f 28 d4`; `66 0f 3a 21 d4 23 41 0f 29 51 30`; `0f 57 d2 0f 16 d4 41 0f 29 51 50`; `41 0f 29 09`; `41 0f 29 49 20`; `41 0f 29 59 40` — each `0f 29` -> `0f 11` | 1 each; _00 applied 6× (skip 0..5), _20 applied 4× (skip 0..3) | spoofed-only, non-fatal | V139 movaps -> movups | 2049-2084, 2285-2441 |
| f_v141_movapd_00 | `66 41 0f 29 09` -> `66 41 0f 11 09` | 1 | spoofed-only, non-fatal | V141 | 2085-2090, 2391-2397, 2443 |
| f_v140_default_store00 / _store20 / case4_store00 / case4_store20 | `0f 28 cb 66 0f 15 cc 41 0f 29 09 0f 28 cb 0f c6 cc cc`; `0f c6 cc cc 41 0f 29 49 20 0f 16 dc`; `0f 28 cc f2 0f 12 cb 66 41 0f 29 09 0f 28 cb 0f c6 cc c6`; `0f c6 cc c6 41 0f 29 49 20 0f c6 dc 4e` — `0f 29` -> `0f 11` | 1 each | spoofed-only, non-fatal | V140 | 2094-2121, 2398-2425, 2446-2449 |
| **fork** f_fmt | `8b 83 a4 00 00 00 89 85 f4 fe ff ff` -> `b8 41 52 47 42 90 89 85 f4 fe ff ff` | 1 | spoofed-only, always | XE-FMT: performTransaction pixel format = 'BGRA' (0x81361) | 2158-2162 |
| **fork** f_noposh | `31 c9 83 e0 10 48 c1 e0 10` -> `31 c9 83 e0 00 48 c1 e0 10` | 1 | spoofed + opt-in `-ngreennoposh` | no POSH bit20 in MI_BATCH_BUFFER_START (0x43021) | 2167-2173 |
| **fork** f_noccs3 | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 48 48 89 75 c0 f6 87 78` -> `31 c0 c3 e5 ...` | 1 | spoofed, always | submitCCSResolve returns immediately (0x2c84c) | 2151-2154, 2174-2176 |
| **fork** f_nodepth | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 38 f6 87 78 0c 00 00 02 0f 84` -> `31 c0 c3 ...` | 1 | spoofed, default on (`-ngreendepthresolve` disables) | submitDepthResolve returns immediately (0x2c656) | 2182-2190 |
| **fork** f_notcf / f_notcf2 | `48 be 04 00 00 7a 21 50 10 50` -> `...10 40`; `4c 89 75 a8 49 c1 e6 3c 49 09 de` -> `4c 89 75 a8 45 31 f6 90 49 09 de` | 3 / 2 | spoofed, default on (`-ngreentcf` disables) | drop Tile Cache Flush bit28 from the PIPE_CONTROL dw1 in generateFlip and commitStampCommand | 2203-2217 |
| **fork** f_b16a / f_b16b | `31 f6 80 f9 10 40 0f 95 c6` -> `31 f6 38 c9 90 40 0f 95 c6`; `80 f9 10 48 be 00 00 00 00 06 00 00 00` -> `38 c9 90 48 be ...` | 1 / 2 | spoofed, default on (`-ngreenblit32` disables) | force the blit3d PS SIMD16 path | 2222-2225, 2254-2261 |
| **fork** f_planes | `8b 83 b0 00 00 00 85 c0 ba 01 00 00 00` -> `8b 83 98 ...` (or `31 c0 90 90 90 90 ...` with `-ngreenplanesxor`) | 1 | spoofed, default on (`-ngreennoplanesfix` disables) | IOSurface plane-count offset 0xb0 -> 0x98 (Tahoe) (0x72bcd) | 2234-2242 |
| **fork** f_rpcs | `41 c7 87 0c 11 00 00 00 00 00 80` -> `41 c7 87 0c 11 00 00 00 10 04 80` | 1 | spoofed + opt-in `-ngreenrpcs` | R_PWR_CLK_STATE 0x20c8 = 0x80041000 (0x7c313) | 2246-2253 |
| **fork** f_noaux | `f6 86 91 00 00 00 02 8b 42 18 75 0b 83 e0 f8 89 42 18` -> `...90 90...` | 1 | spoofed, default on (`-ngreenccs` disables) | AuxiliarySurfaceMode always NONE (0x73fd0) | 2195-2198, 2262-2266 |

#### 4. Boot-args checked in L1-3800

| Boot-arg | Meaning | Default |
|---|---|---|
| `-ngreentglfb` | Hook the TGL FB kexts (FBT and FBTA); the ICL FB is skipped | off (no Gen11 FB hook) |
| `-ngreentglwithgfx` | TGL FB plus TGL HW; also enables the Path E TCON ID patches | off |
| `-ngreentglgfx` | TGL HW only | off |
| `-ngreenicl` | ICL FB plus ICL HW fallback (only if no TGL arg is set) | off |
| `ngwegcoex=N` / `-ngwegcoex` | WhateverGreen coexist mode: skips the CDCLK routes on ICL FB, PAVP/getGPUInfo, and forceWake (unless fullmtl) | off |
| `ngreenexp=N` / `-ngreenexp` | Experimental monitor; also the default source for V60 | off |
| `ngreenv60off=N` / `-ngreenv60off` | Force the V60 monitor off | — |
| `ngreenv60=N` / `-ngreenv60` | Force the V60 monitor on | off (follows ngreenexp) |
| `ngreenv60rw=N` / `-ngreenv60rw` | V60 aggressive (read/write) monitor | off |
| `ngreendp1=N` / `-ngreendp1` | Native DisplayPipeSupported on (V78A) | native on |
| `ngreendp0=N` / `-ngreendp0` | Force DisplayPipe disabled ("dp0"): Path B (aperture forced) and Path C (wsrv 1->3) | off |
| `ngreenv93=N` / `-ngreenv93` | V93 plane SURF zero-write guard | off |
| `ngreenfullmtlcore=N` / `-ngreenfullmtlcore` (legacy `ngreenfullmtl` / `-ngreenfullmtl`) | Force the full-Metal path; routes SafeForceWakeMultithreaded even under WEG coexist | off |
| `ngreenV142=N`, `-ngreenV142orig` (3), `-ngreenV142pass` (2), `-ngreenV142ok` (1), `-ngreenV142hardunsupported` (0), `-ngreenV142unsupported` (1) | submitBlit mode (V142/V170) | 1 |
| `ngreenV142diag2d=N` / `-ngreenV142diag2d` | V142 diag 2D bypass | off |
| `ngreenv88=N` / `-ngreenv88` | V88 scanout fill | off |
| `-ngreenv80l` | V80L enforcer | off |
| `-ngreenbcsirq` | V65: include BCS in tier-1 want bits on spoofed hardware | off |
| `ngreenV77DelayKill=N` | V77 DisplayPipe client kill delay, clamped 0..60 | 60 (no kill) |
| **fork** `-ngreenfixbootpatch` | fCurrentMode initial-value byte patch | off |
| **fork** `-ngreenfixbootmode` | Early IOFBDetailedTimings from the EFI CRTC (fbTimingsWorker) | off |
| **fork** `-ngreenevlog` | Print the event/CSB ring | off |
| **fork** `-ngreennosametail` | Disable SAMETAIL | SAMETAIL on |
| **fork** `-ngreenelsqretry` | Enable the ELSQ retry (EXECLIST_CONTROL 0x2550=1) | off |
| **fork** `-ngreennopaging` | pageoff/pageon become no-ops | off |
| **fork** `-ngreennopageonguard` | Disable the pageon guard | guard on |
| **fork** `-ngreennocsbguard` | Disable the CSB invalid-event guard | guard on |
| **fork** `-ngreennofwsubmit` | Disable the forcewake hold around submission | on |
| **fork** `-ngreennoelsqwait` | Disable the EXECLIST_STATUS bit30 wait | on |
| **fork** `ngreenelsqdelay=us` | Delay before each RCS submit (0..5000) | 50 |
| **fork** `-ngreennoelsqidle` | Disable the idle wait before submit | on |
| **fork** `ngreenpreemptwait=us` | Maximum preempt-avoid wait (0..200000) | 5000 |
| **fork** `-ngreenidlearb` | Wait only when arbitration bit16 is set | off |
| **fork** `-ngreenposhctx` | Keep POSH contexts | off (POSH cleared) |
| **fork** `-ngreennoposhfix` | Disable the POSH_LRCA 0x21b0 zeroing | fix on |
| **fork** `-ngreenxefix` | xefix display fixes (DPT off, DBUF S1, GAMMA 0, backlight) plus diag hooks; disables the V408 linear forcing and V99R/V99G | off |
| **fork** `-ngreenxtile` | Keep X-tile PLANE_CTL, stride srcW*4/512 (in place of the V408 linear forcing) | off |
| **fork** `ngreenstride=N` | Override PLANE_STRIDE (struct and MMIO 0x70188) | 0 (none) |
| **fork** `-ngreennoblr`, `ngreenblmax=N` | Backlight rescale off / reference duty | rescale on / 0 |
| **fork** `-ngreenengineinert` | Drop Apple writes to the engine MMIO blocks | off |
| **fork** `-ngreennoposh` | NOPOSH byte patch | off |
| **fork** `-ngreendepthresolve` | Keep the original submitDepthResolve | NODEPTH on |
| **fork** `-ngreentcf` | Keep Tile Cache Flush | NOTCF on |
| **fork** `-ngreenplanesxor`, `-ngreennoplanesfix` | Planes patch variant / disable it | 0x98 fix on |
| **fork** `-ngreenrpcs` | RPCS patch | off |
| **fork** `-ngreenblit32` | Keep SIMD32 blit | BLIT16 on |
| **fork** `-ngreenccs` | Keep AUX/CCS | NOAUX on |

(`-ngreenv99g` is referenced in a comment at L3885 and parsed after L3800.)
<!-- FINAL-END -->



### 2.B kern_gen11.cpp lines 3800-7600: V-tags, hook bodies, boot-args, properties

Source: src/NootedGreen/kern_gen11.cpp, lines 3800-7600 (a few blocks that start just before 3800 are included where they straddle the boundary). "Spoofed-only" = gated on `!isRealTGL`. Fork (xe/XE/XeNG) code is only mentioned where it gates a V-feature.

#### 1. V-tag table

| Tag | Area | Where | What it does + problem addressed | Status | Lines |
|---|---|---|---|---|---|
| V99S | Display scanout (diag + trigger) | `raWriteRegister32` hook (Apple `AppleIntelRegisterAccessManager::WriteRegister32` cache path), on writes to PLANE_SURF_A 0x7019C; reads PLANE_STRIDE 0x70188 / PLANE_CTL 0x70180 and samples 4 aperture pixels (0, 0x7D2800, 0x9C7800, 0x7D1A00) | Logs the first 8 SURF arms with HW stride/ctl and framebuffer pixel samples at fixed 2560x1600 BGRA offsets to diagnose blank/duplicated scanout. The comment's "force STRIDE/CTL" behaviour is no longer present (the linear force block below is commented out); only logging remains. | diagnostic-only (always runs, first 8 SURF writes) | 3775-3820 |
| V201B | Display scanout diag | inside V99S block | Adds top-left/center/loading-bar/mid-left aperture pixel samples (via `setApertureIfNecessary`) to the V99S log to see whether the logo/loading bar exists in the scanned buffer. | diagnostic-only | 3802-3819 |
| V203 | Display scanout diag | inside V99S, on the 8th SURF arm | One-shot dump of plane A (0x70180-0x701CC), pipe A (0x6001C, 0x70008, 0x70030, 0x7002C, 0x70024), transcoder A timings (0x60000-0x60028), TRANS_DDI_FUNC_CTL/2, MSA_MISC, CLK_SEL 0x46140, DP M/N (0x60030-0x6004C), PS1 scaler (0x68170-0x68194), DSC A (0x6B200-0x6B220), DDI_BUF A/B, DP_TP_CTL/STATUS A, DBUF (0x44300/4, 0x70B80/4). Hunts an off-by-2 in stride/src size/DSC bpp/bpc/M-N behind the duplicated-content symptom. | diagnostic-only | 3821-3859 |
| V99R (V99R[P]) | Display scanout | `raWriteRegister32`, PLANE_SURF_A 0x7019C | If Apple writes a non-aperture SURF (>= 0x10000000, after `setupScanoutMemory` migrated the surface when WindowServer goes active), rewrites the SURF value to an aperture slot address (slot*0xFA0000) so the plane scans GGTT pages that V99G remaps. Addresses blank/invisible scanout on spoofed RPL/ADL-P. | spoofed-only (!isRealTGL); ON by default, but OFF when `-ngreenxefix` is set unless `-ngreenv99g` (fork gate, line 3889-3891) | 3861-3967 |
| V99G | Display scanout / GGTT | same block; reads/writes GGTT PTEs `GGTT_PTE_LO/HI(page)` (GEN8_GGTT_PTE_BASE + page*8), flushes GGTT TLB via write 0x1 to 0x101008 | Per-flip GGTT remap: assigns each distinct source SURF page to one of `ngreen-buf` slots (default 2, LRU-evict), copies 4000 valid PTEs from srcPage.. to GGTT[slot*4000..+3999] so the redirected SURF fetches the same physical pages WindowServer renders into. Fixed 4000-page slots only cover 2560x1600 (fork note: on 3840x2400 the rest reads stolen memory and slot 0 overlaps the LRCA at GGTT 0x18000, hence the xefix gate). | spoofed-only; default on; disabled by `-ngreenxefix` unless `-ngreenv99g` | 3876-3967 |
| V99 (linear CTL/STRIDE forces) | Display scanout | `raWriteRegister32` SURF block; PLANE_CTL 0x70180 tiling bits[12:10]→0, PLANE_STRIDE 0x70188→0xa0 | Forced linear tiling and stride 0xa0 (2560 px) so the CPU compositor's linear writes scan correctly; comment says paramsSurfCompare now forces linear with the real-mode stride. | commented-out | 3968-3985 |
| V401 | Display (reference only) | comment in SURF block | Referenced as the paramsSurfCompare logging that showed Apple wants X-tiled CTL bits[12:10]=001 and STRIDE=0x14; no code in range. | referenced only (code elsewhere) | 3969 |
| V97P / V97C | Display (reference only) | comment | Mentioned as the existing partial CRTCParams interception at hwSetupMemory/paramsSurfCompare/hwRegsNeedUpdate (code outside this block; see V97C at hwRegsNeedUpdate below). | referenced only | 3870 |
| V212 | Display workarounds (Display 13) | `initPlatformWorkarounds` hook, after callthrough. Regs: GEN9_CLKGATE_DIS_5 0x46540 bit17 set; GEN8_CHICKEN_DCPR_1 0x46430 bit7 clear; PIPE_CHICKEN A 0x70038 bits 30,15,7 set; XELPD_DISPLAY_ERR_FATAL_MASK 0x4421C=0xFFFFFFFF | Ports Linux i915 ADL-P display workarounds (Wa_22011091694 DPCE gating disable, Bspec 49189 DDI clock reg access clear, underrun-recovery disable, per-pixel-alpha bypass WA#1153, pixel rounding WA#1605353570, mask fatal display errors) that Apple's Display-12 TGL kext never applies; their absence may cause underrun/error-recovery loops. | spoofed-only (!isRealTGL), always | 4028-4058 |
| V50 | Build marker | `hwInitializeCState` | Logs "NB-BUILD-V50-ALLOW-METAL" build banner; no functional effect. | diagnostic-only | 4119 |
| V101T | Display power wells (TGL DMC path) | `hwInitializeCState`, `ngreen-dmc=tgl` branch; HSW_PWR_WELL_CTL1 0x45400 = UEFI value \| 0x401, CTL2 0x45404 = 0xC03 | TGL-DMC variant of V101: preserves UEFI CTL1 bits while asserting PG1 request/state, CTL2 hardcoded. (Also writes CTL3=0x40000000, CTL4=0x401, AUX1/2, DDI1/2 = 3.) | opt-in boot-arg `ngreen-dmc=tgl` | 4147-4160 |
| V104T | DMC context regs (TGL path) | `hwInitializeCState` tgl branch, after original callthrough; 0x8F074=0x6FC0, 0x8F004=0x00A40088, 0x8F034=0xC003B400 | Re-applies TGL DMC/display-engine context registers that the original's ICL CSR blob load overwrote. | opt-in `ngreen-dmc=tgl` | 4190-4194 |
| V102T | Display power wells (TGL path) | `hwInitializeCState` tgl branch, after callthrough; 0x45400 | Restores CTL1 = UEFI \| 0x401 if the ICL DMC save/restore table rewrote it to 0x401 (clearing UEFI bits). | opt-in `ngreen-dmc=tgl` | 4195-4203 |
| V101 | Display power wells (ADL-P DMC path) | `hwInitializeCState` adlp branch; 0x45400/0x45404 | Writes CTL1 = UEFI \| 0x401 and CTL2 = 0xC03 (not OR'd with UEFI's TC bits 0xfc00) after loading the ADL-P DMC v2.16 payload to 0x80000. | opt-in `ngreen-dmc=adlp` | 4232-4239 |
| V100 | Display power wells | comment within V101 block | Rationale for V101: preserve UEFI CTL1 bits 12/14 because writing hardcoded 0x401 broke vsync interrupt delivery (WindowServer "Display not ready" crash ~60s); don't OR TC bits into CTL2 (blocked WSA un-blank). | opt-in `ngreen-dmc=adlp` (same code as V101) | 4225-4231 |
| V104 | DMC context regs (ADL-P path) | `hwInitializeCState` adlp branch after callthrough; 0x8F074/034/004/038/008/03C/00C | Re-applies ADL-P display-engine context MMIO pairs (from the v2.16 blob header) after the original loads the ICL CSR blob, since the ADL-P DMC itself drops eDP lanes at ~10s and the ICL DMC is run instead. | opt-in `ngreen-dmc=adlp` | 4372-4380 |
| V102 | Display power wells (ADL-P path) | after callthrough, 0x45400 | Restores CTL1 = UEFI \| 0x401 if the ICL DMC save/restore table cleared the UEFI bits. | opt-in `ngreen-dmc=adlp` | 4381-4389 |
| V105 | PSR | `hwInitializeCState` adlp branch; writes 0 to EDP_PSR2_CTL TGL 0x60A10, EDP_PSR_CTL TGL 0x60800, PSR2 legacy 0x60900, PSR1 legacy 0x64800 | Disables PSR1/PSR2 enabled by the ICL DMC initializer: on ADL-P the PSR2 selective-update path does not work, the panel stays in self-refresh on the first (black) frame. Address fix: PSR1 on TGL is 0x60800 (earlier version wrote only 0x64800). | opt-in `ngreen-dmc=adlp` only | 4390-4414 |
| V76 / V88 / V205 | (reference only) | comments in V105 | Cited earlier probes: V76 found PSR2_CTL=0x4811, V88 direct physical writes to SURF were invisible, V205 caught PSR1 still enabled at 0x60800. No code for these in this block. | referenced only | 4395-4402 |
| V60 | (reference only) | comment `uefiCtl1 ... save for V60 re-enforcement` | `uefiCtl1` is saved for the V60 CTL1 re-enforcement logic located elsewhere. | referenced only | 4349 |
| V103 / V104P | (reference only) | comment in tgl branch | `dmcIsAdlp=true` also set on TGL-DMC path so V103/V104P protections apply (V103 itself is removed, see 3765-3779). | referenced only | 4170 |
| V25 | Display init ordering | `AppleIntelBaseControllerstart` | Display workarounds applied BEFORE Apple's start (no ForceWake needed for 0x4xxxx+), GT workarounds moved after start (need ForceWake for 0x0-0x7FFF). Starts with DC_STATE_EN=0, Wa_14011294188 (SOUTH_DSPCLK_GATE_D PCH_DPMGUNIT_CLOCK_GATE_DISABLE), PCH reset handshake. | always | 4606- |
| V97P | Display modeset (CRTCParams) | `hwRegsNeedUpdate` hook (AppleIntelFramebufferController::hwRegsNeedUpdate), pending `CRTCParams.TRANS_DDI_FUNC_CTL` | Clears bit16 in the pending TRANS_DDI_FUNC_CTL that Apple's SetupParams sets (UEFI/HW never does); otherwise the FUNC_CTL compare triggers a full modeset that disrupts the already-trained eDP link (black screen). Also calls `adlpDcExit("hwRNU")` first when `dmcIsAdlp`. | spoofed-only (!isRealTGL), always | 4782-4803 |
| V97C | Display modeset (CRTCParams) | `hwRegsNeedUpdate`; pending `CRTCParams.TRANS_CONF` vs live PIPE_CONF_A 0x70008 | Replaces pending TRANS_CONF with live HW value so paramsFbCompare sees no change (Apple wanted 0xc0000000->0xc0000024); writing bits 5/2 on an active pipe caused DPCD InterLane Alignment Lost (0x202=0x80) and checkLinkStatus teardown ~10 s later. Also an unconditional "hwRegsNeedUpdate called" log counter; original result returned. | spoofed-only (!isRealTGL), always | 4805-4846 |
| V300 | Display DSC | `hwRegsNeedUpdate` (comment) | Was a hack zeroing CRTCParams +0xE8/+0xEC (DSC engine select). Reverted: DSC to be controlled at getDPCDInfo / FeatureControl (Info.plist DSCSupport=0). | dead (reverted, comment only) | 4826-4835 |
| V201 | Display scanout diag | `wrapHwSetupMemory` hook (hwSetupMemory), after callthrough; reads fb+0x4330 (SURF), +0x4334 (size), +0x4288 (fb idx), +0x4A18 (Y-tile flag) | For the first 8 calls, samples 4 pixels in the aperture at the SURF offset (2560x1600 layout) to see if the newly set-up buffer contains anything. | diagnostic-only | 4853-4887 |
| V201P | Display scanout diag | same; GGTT_PTE_LO/HI(surf>>12) | Reads the GGTT PTE for the surface page and logs present/LLC bits to confirm the display engine can access it. | diagnostic-only | 4875-4886 |
| V44 | GPU scheduler selection | `Gen11::start` hook (accelerator start), sets property `GraphicsSchedulerSelect` on the accelerator service | Selects scheduler type 3 (IGGuC firmware), 4 (IGScheduler4) or 5 (IGScheduler5 host preemptive): boot-arg `ngreenSched=N` (3..5) > provider property `SchedulerType` (3..5, from NootedGreen Info.plist) > default; injects as `GraphicsSchedulerSelect` read by populateAccelConfig. | always | 4899-4937 |
| V52 | GPU scheduler / ForceWake | `Gen11::start` | Platform-dependent defaults: real TGL default scheduler 3 (GuC), RPL default 5 (host); MultiForceWakeSelect=1 injection (and the fork's XE-NOCCS / XE-NOGVA blocks nested in the same branch) only on !isRealTGL, real TGL logs a skip. | always (branch by isRealTGL) | 4903-5002 |
| (no tag) MultiForceWakeSelect | ForceWake | `Gen11::start`, `Development` dict | Injects `Development/MultiForceWakeSelect=1` so the accelerator uses SafeForceWakeMultithreaded (hooked) instead of the FB SafeForceWake that fails on RPL-P (ACK=0). | spoofed-only | 4939-4969 |
| V29 | GT bring-up diag | `Gen11::start` (accelerator start hook): ForceWake render acquire (FORCEWAKE_RENDER_GEN9 = 0x10001, poll ACK 50 ms) before start; dumps GGTT[0..3], GGTT[0x100..0x103], RCS CTX_SIZE/CCID/CTX_CTRL/MI_MODE/RING_MODE pre- and post-start; post-start acquires render+blitter ForceWake and dumps RCS0/BCS ring state, EIR/ESR/EMR, RING_FAULT, ERROR_GEN6, FAULT_TLB_DATA0/1, GT_INTR_DW0/1, GGTT PTEs for HWS and ring start, CSB[0..5] | Applies GT workarounds under ForceWake (V25 ordering) and logs extensive pre/post-start GT state for diagnosing ring/context bring-up on RPL. The ForceWake acquire/release around workarounds is functional; the rest is logging. | always (diagnostic dumps); ForceWake functional | 5004-5045, 5258-5424 |
| (no tag) GT workarounds | GT | `Gen11::start` pre-start: GEN7_MISCCPCTL clear GEN12_DOP_CLOCK_GATE_RENDER_ENABLE; VDBOX_CGCTL3F10 (RCS/BCS/VEBOX bases) IECPUNIT_CLKGATE_DIS; GEN10_DFR_RATIO_EN_AND_CHICKEN DFR_DISABLE; GEN11_COMMON_SLICE_CHICKEN3 GEN12_DISABLE_CPS_AWARE_COLOR_PIPE; GEN8_CS_CHICKEN1 preempt level thread-group; GEN8_MCR_SELECTOR slice0/subslice0 | Linux-i915-style Gen12 GT workarounds applied before Apple's start. | always | 5103-5118 |
| V55 | GT error state | `Gen11::start` pre-start, if ERROR_GEN6 != 0: TLB invalidate via GEN12_GUC_TLB_INV_CR 0xcee8 (=1 RCS, =4 BCS), 5x W1C of ERROR_GEN6, clear RCS/BCS RING_EIR, read status 0xceec | Clears sticky TLB-fault bits in ERROR_GEN6 (simple W1C left 0x37) before Apple's start so getBlit3DContext sees a clean ERROR_GEN6. | spoofed-only (!isRealTGL), only when ERROR_GEN6 nonzero | 5049-5100 |
| V55G | GT reset | same block, first step: GEN6_GDRST = GEN6_GRDOM_RENDER, poll up to 2000 reads; re-acquire render ForceWake | Render-domain reset to evict the stale EFI/firmware context (CCID=0x80) that keeps re-asserting ERROR_GEN6; logs CCID before/after. | spoofed-only, only when ERROR_GEN6 nonzero | 5057-5074 |
| V162 | GT workaround | `Gen11::start`; GEN7_FF_SLICE_CS_CHICKEN1 GEN9_FFSC_PERCTX_PREEMPT_CTRL (bit14, masked) | Enables the TGL per-context preempt control only on real TGL; on RPL the bit (baked into the execlist context image) froze the EU thread dispatcher on the first Metal context-image restore. | real-TGL-only (i.e. skipped on spoofed) | 5119-5126 |
| V42 | Hangcheck support / IRQ diag | `Gen11::start`: stores `callback->accelInstance = that`; post-start logs pending GT interrupts without clearing | Saves the accelerator instance for child enumeration by the hangcheck code; later logs pending GT IRQs (no W1C, so tier-2 IIR is not lost). | always | 5133-5134, 5437- |
| V54 | GT interrupts | `Gen11::start` pre-start: GEN11_GFX_MSTR_IRQ = GEN11_MASTER_IRQ (bit31); arms `v54IrqWatchdog` thread_call 2 s | Pre-enables the master GT IRQ, which the TGL driver never enables on RPL (completion IRQs never reached the CPU and start() hung ~15 s); watchdog re-enables it every 2 s during start. | spoofed-only | 5136-5145, 5167-5178 |
| V53 / V64 | (reference only) | comments | V53 proved TGL driver never sets GFX_MSTR_IRQ bit31 on RPL; V64 proved GEN11_RENDER_COPY_INTR_ENABLE had RCS0 bit0 disabled. Motivation for V54/V65. | referenced only | 5137, 5148 |
| V65 | GT interrupts | `Gen11::start` pre- and post-start: GEN11_RENDER_COPY_INTR_ENABLE \|= `getV65Tier1WantBits(isRealTGL)`; GEN11_RCS0_RSVD_INTR_MASK unmask GT_CONTEXT_SWITCH_INTERRUPT\|GT_RENDER_USER_INTERRUPT; ERROR_GEN6 cleared; post-start also re-sets GFX_MSTR_IRQ | Enables RCS0/BCS tier-1 interrupt routing and tier-2 unmask before ring init and again immediately after start(), so the scheduler gets completion interrupts (otherwise timeout -> dead engine). | spoofed-only | 5147-5164, 5224-5243 |
| V116 | GGTT / MCE guard | `Gen11::start` pre-start: allocates a zeroed, physically contiguous 4 KB page below 4 GB, writes GGTT[0] PTE = phys \| 0x9 (present+LLC), flushes GGTT TLB (0x101008=1) | Redirects GGTT page 0 (which pointed at stolen-memory base) to a dummy page so stray GPU accesses to VA 0 (ring at 0, PLANE_SURF=0) no longer read stolen memory and trigger package-wide MCE (banks 9/11/15, addr 0x3e800000). Note: V99G slot 0 later overwrites GGTT[0..3999] when active. | spoofed-only | 5180-5216 |
| V221 | Sync flag | `Gen11::start` after callthrough | Sets `Gen11::gGfxAccelStartDone = true` so the waitForStamp hook knows the GFX interrupt handler is installed. | always | 5220-5222 |
| V507 | Execlists | `Gen11::start` post-start; RING_MODE(RCS) = 0x80008000 (masked write of GFX_RUN_LIST_ENABLE bit15) | Apple's init does not set GFX_RUN_LIST_ENABLE on RPL-P (snapshots 0 into the MI_LRI replay list), leaving execlist mode off; this sets it in the live register before any submission. | spoofed-only | 5244-5255 |
| V43 | Scheduler diag | `Gen11::start` post-start: accel field +0x1190 bits 23-25 (sched type), +0x1120, encodeFailureStack +0x1c30..+0x1c4c count +0x1c50, scheduler ptr +0xe00 | Logs which scheduler type the accelerator actually configured, failure codes and whether a scheduler object exists. | diagnostic-only | 5265-5286 |
| V44 (post-start diag) | Accel registration diag | `Gen11::start`: +0x128c fbRegistered, +0xde8 callback ptr; properties MetalPluginName, IOGLBundleName, IODVDBundleName, IOVARendererID | Logs FB registration status and whether TGL Metal/GL/VA bundle names are present on the accelerator service. | diagnostic-only | 5288-5313 |
| V36 | IRQ diag | `Gen11::start` post-start: GFX_MSTR_IRQ, DISPLAY_INT_CTL, RENDER_COPY_INTR_EN, VCS_VECS_INTR_EN, RCS0/BCS RSVD masks | Logs interrupt enable/mask state. | diagnostic-only | 5426-5435 |
| V42 (child enum) | Accel diag | `Gen11::start` post-start: reads 0x190018 (GT_INTR_DW0), enumerates accelerator clients | Logs pending GT_INTR_DW0 (left for the handler so the stamp interrupt V221 relies on is serviced) and lists accelerator children to check IOAccelDevice creation. | diagnostic-only | 5437-5462 |
| V45 | Accel service visibility | `Gen11::start` post-start: getState, provider + siblings, property presence; explicit `registerService(kIOServiceAsynchronous)` on the accelerator | Diagnoses whether the accelerator is registered/matched/published and has MetalPluginName/IOGLBundleName/IOCFPlugInTypes/IOVARendererID; if MetalPluginName is missing, injects TGL (when `tglHWLoaded`) or ICL bundle names, IOGVACodec/IOGVAScaler/IOGVABGRAEnc="Gen10", IOSourceVersion, IOVARendererID=17301568, IOCFPlugInTypes {ACCF0000-0000-0000-0000-000a2789904e: IOAccelerator2D.plugin}. Then forces registerService so IOKit matching sees it. | always (property injection only if MetalPluginName missing) | 5464-5593 |
| V44 (bundle log scheduling) | Accel diag | `v44ScheduleBundleLog(that, 3000)` | Schedules a delayed log of the bundle properties at T+3 s (500 ms variant commented out). | diagnostic-only | 5515-5516 |
| V77 | Display pipe caps | `Gen11::start`, only in the "MetalPluginName missing" fallback: `IOAccelDisplayPipeCapabilities` = {DisplayPipeSupported=0, TransactionsSupported=0} | Disables GPU display-pipe compositing because WindowServer crash-looped in CoreDisplay::DisplayPipe::RunFullDisplayPipe while the RCS ring was idle; forces CPU compositing via the FB aperture. (The V77 "DisplayPipe killer" in the V60 monitor is separate, outside range.) | conditional: only when MetalPluginName absent | 5536-5549 |
| V78 | Display pipe caps | `Gen11::start`: if `isDisplayPipeForceDisabled()` set `IOAccelDisplayPipeCapabilities` = {DisplayPipeSupported=0, TransactionsSupported=0} | Forces CPU compositing so WindowServer never waits for the "FB Ready" flip signal that the V187 AccessComplete stub suppresses (watchdogd kill -> KP). Despite the comment saying "always" for spoofed HW, the helper (lines 142-174, V78A) returns true only with `-ngreendp0` or `ngreendp0=<nonzero>`; `-ngreendp1`/`ngreendp1=<nonzero>` wins and default is native DisplayPipe ON. The else-branch log text "(real TGL)" is misleading. | opt-in boot-arg `-ngreendp0` / `ngreendp0=N` (default-off) | 5559-5583 |
| V187 | (reference only) | comment in V78 | AccessComplete is stubbed to return 0 on spoofed HW to avoid a crash at +1928 (null MTL device hash lookup); code elsewhere. | referenced only | 5560-5563 |
| V110 | Policy | `Gen11::start` | Makes V59 delayed child checks and the V74 EMR enforcer unconditional on !isRealTGL, while the V60 health monitor stays opt-in; logs how to enable V60. | spoofed-only | 5595-5600, 5647 |
| V59 | Accel device rescue | `Gen11::start`: `v45ScheduleDelayedCheck(that, ...)` at T+3,10,15,20,30,60,120 s | Delayed checks that rescue IGAccelDevice children stuck at state 0x0; without them WindowServer never opens IOAccelDisplayPipeUserClient2 and the display pipe never activates. (Checker body outside range.) | spoofed-only | 5602-5610 |
| V74 | GT error interrupts | `Gen11::start`: arms `v71EmrEnforcer` thread_call (50 ms, permanent, retains service); log "maskWrites=1 v116Reenforce=1" | Permanent enforcer that keeps RING_EMR masked so Apple cannot re-enable error interrupts (otherwise all-black screen) and per config also re-enforces V116. (Enforcer body is V71, outside range.) | spoofed-only | 5611-5628 |
| V71 | (reference only) | `v71EmrEnforcer` callback name | The EMR enforcer function armed by V74; body outside range. | referenced only | 5618 |
| V60 | GPU health monitor | `Gen11::start`: `isV60MonitorEnabled()` -> arms `v60GpuHealthMonitor` thread_call every 2 s, 60 iterations | Opt-in health monitor that contains the V77 DisplayPipe terminator (kills IOAccelDisplayPipeUserClient2 after V59 opens it). Gating helper (lines 118-140): disabled by `ngreenv60off=N!=0` or `-ngreenv60off`; enabled by `ngreenv60=N!=0`, `-ngreenv60`, or the experimental flag (`-ngreenexp` per log text). | opt-in (`-ngreenv60` / `ngreenv60` / `-ngreenexp`), default-off | 5630-5648 |
| V47 | GT interrupts | `Gen11::start` end: GEN11_GFX_MSTR_IRQ (0x190010) bit31 set if clear; logs RCS/BCS RING_TIMESTAMP (+0x358) | Enables the master interrupt (as i915 gen11_master_intr_enable) so the scheduler gets completion notifications. | always (only writes if bit31 clear) | 5652-5672 |
| V50 | Diag | `Gen11::start` | Logs Metal readiness / plugin track (TGL from /Library/Extensions, ICL fallback) policy text; mentions `-ngreenNoMetal`. | diagnostic-only | 5674-5678 |
| V51 | GT error cleanup | `Gen11::start` end: ERROR_GEN6 W1C loop (3x), RCS/BCS RING_EIR/ESR W1C, GEN12_RING_FAULT_REG=0, FAULT_TLB_DATA0/1=0, GT_INTR_DW0/1 W1C | Clears stale GPU errors so the Metal plugin does not reject the device at MTLDevice creation. | always | 5680-5734, 5868 |
| V51 (BCS reset) | Blitter engine | `Gen11::start`: if BCS RING_CTL == 0: RING_MI_MODE(BCS) STOP_RING masked set then clear, RING_HEAD/TAIL(BCS)=0 | Clears dead BCS ring state (CTL=0, EXECLIST_STATUS=1) so the scheduler can retry on the first Metal blit. | spoofed-only (V52 skip on real TGL), only if BCS CTL==0 | 5736-5770 |
| V55 (post-start) | GT TLB / errors | `Gen11::start`: 0xcee8 = 1 then 4, ERROR_GEN6 5x W1C, read 0xceec | Post-start TLB invalidation plus repeated clear, because ERROR_GEN6=0x7b (TLB_MISS+PAGE_TABLE_ERROR) re-generates from stale TLB entries. | spoofed-only | 5772-5797 |
| V56 | (reference only) | comment | Findings: ERROR_GEN6 is R/W on RPL-P (writing 0xFFFFFFFF set reserved bits), GDRST killed the ring, RING_RESET_CTL non-functional. | referenced only | 5800-5803 |
| V57 | GT error masking | `Gen11::start`: ERROR_GEN6 = 0 (then W1C fallback), RING_EMR(RCS)=RING_EMR(BCS)=0xFFFFFFFF, RCS EIR W1C, logs ring alive/IPEIR/IPEHR/INSTDONE | Clears ERROR_GEN6 by plain write (R/W on RPL-P) and masks all per-ring error interrupts without destructive engine resets. | spoofed-only | 5799-5866 |
| V111 | Accel device | `deviceStart` hook (IGAccelDevice::deviceStart) | If the original returns false on spoofed hardware (encodeFailureStack[1] set because BCS ring init fails on RPL), forces true so CoreDisplay does not see a null accelerator device. | spoofed-only | 5877-5892 |
| (no tag) PAVP | PAVP | `wrapPavpSessionCallback` | Session command 4 returns kIOReturnSuccess without calling the original; all others pass through. | always | 5894-5905 |
| (no tag) GPU topology | GPU info | `getGPUInfoICL` (ICL binary: 1 slice, 8 SS, 8 EU/SS = 64 EU, L3 banks 8 at +0x1190/+0x1188/+0x11a0/+0x1154/+0x12cc/+0x12d0/+0x1198) and `getGPUInfo` (TGL binary: 1 slice, 12 SS, 8 EU/SS = 96 EU, L3 8, freq max 1000/min 450 MHz at +0x115c/+0x1158/+0x116c/+0x1124/+0xdd8/+0xddc/+0x1164/+0x1150) | Overrides topology reported by Apple's getGPUInfo with RPL i7-13700H values (6 DSS x2). | always | 5907-5987 |
| V164 | Context image | `populateResetRegisterList` hook (IntelAccelerator::populateResetRegisterList): masked clear of GEN7_FF_SLICE_CS_CHICKEN1 0x20E0 bit14 (write 0x40000000) BEFORE original | startGraphicsEngine writes 0x40004000 to 0x20E0 and populateResetRegisterList snapshots it into the per-context MI_LRI replay list, which re-applies PERCTX_PREEMPT_CTRL on every context switch (V162/V163 clears come too late); clearing first makes the snapshot 0. | spoofed-only | 5989-5998, 6050-6059 |
| V163 | (reference only) | comment | Earlier post-hoc MMIO clear of PERCTX_PREEMPT_CTRL, superseded by V164 (code elsewhere/removed). | referenced only | 5995 |
| V507 (LRCA repair label) / V508 | Context / execlist | `xeLrcaRepair` (fork function, tagged xe/lrca) | Comment labels it "V507 LRCA repair": re-copies the first 96 dwords of the saved RCS context page 1 to GGTT slot 0x19 when slot DW1 lacks the MI_LRI header; V508 is referenced as the withOptions-time repair. Fork addition, catalogued elsewhere. | spoofed-only (fork) | 5999-6048 |
| V503 | Context image diag | `populateResetRegisterList` after original, once: reads 0x2080, 0x2134, 0x20c0, 0x7000, 0x7004, 0x209c, 0x2090, 0x4090, 0x20a0, 0x20e4, 0x9430, 0x7010, 0x0d08, 0xe194, 0xb004, 0x20ec, 0x2580, 0x20e0, 0x229c | Logs the 19 live register values Apple just baked into the context reset register list (set decoded from Ghidra). | diagnostic-only (spoofed, once) | 6062-6088 |
| V504 | Context image | `populateResetRegisterList` after original: walks `IntelAccelerator::fResetRegData` (entries 0x24 bytes {addr, value, name[0x1c]}, count `fResetRegCount`) | Patches the reset register list before the context image bakes: CS_CHICKEN1 0x2580 value -> 0x00000002. | spoofed-only | 6089-6113 |
| V504-TEST | Context image | same loop, CS_DEBUG_MODE1 0x20ec -> 0 | Test of clearing Apple's 0x11 in CS_DEBUG_MODE1. | commented-out | 6104-6109 |
| V506 | Context image / execlist | same loop, entry 0x229c (RCS GFX_MODE / RING_MODE) value \|= bit15 | The snapshot reads RING_MODE before GFX_RUN_LIST_ENABLE is set (baked 0); on first lite-restore the MI_LRI replays 0 and disables execlist mode so the context never becomes ACTIVE. Forces bit15 in the image. | spoofed-only | 6114-6124 |
| V507 (post-resetRegList) | Execlist | `populateResetRegisterList` end: RING_MODE(RCS)=0x80008000 | Re-arms GFX_RUN_LIST_ENABLE in the live register after every reset/recovery (Apple's recovery may clear it). Not gated by isRealTGL here. | always (incl. real TGL) | 6127-6135 |
| V137 | Task diag | `createUserGPUTask` / `igAccelTaskWithOptions` hooks | Logs created task, vtable, ctx (+0xb8), kernel task (+0x150) and cached task (first 24). | diagnostic-only, opt-in via experimental monitor flag (`isExperimentalMonitorEnabled`, -ngreenexp) | 6174-6187, 6246-6255 |
| V138 | Task context | `ensureTaskContext` lambda in createUserGPUTask / igAccelTaskWithOptions | If task+0xb8 (context) is NULL on spoofed HW, fills it from task+0x298 (where Apple's getBlit3DContext(task,true) stores the context); does not call getBlit3DContext itself (doing so too early hung boot). Only the log is gated by the experimental flag. | spoofed-only (functional always; log opt-in) | 6142-6171, 6214-6243 |
| V132 | Task fallback | `createUserGPUTask`: if original returns NULL on spoofed HW, return accelerator kernel task (+0x150) or `v132CachedTask`; `igAccelTaskWithOptions`: caches every created task, returns cached one if original returns NULL | Prevents NULL task returns on RPL by falling back to the kernel/cached task. | spoofed-only | 6188-6210, 6256-6273 |
| V148 (getBlit3DContext) | Blit3D context | `getBlit3DContext` hook: before original, sets GEN11_RENDER_COPY_INTR_ENABLE tier-1 bits (`getV65Tier1WantBits(false)`) if missing; after: accepts ctx even with ctx+0xb8 NULL on spoofed HW; fallbacks cached ctx -> `getDepthResolveContext` -> `getColorResolveContext` | Ensures context-creation completion interrupt can fire (ring init disables tier-1), and returns a usable Blit3D context on RPL where the original may return NULL/invalid; logs ERROR_GEN6/tier-1 when original fails. | spoofed-only (real TGL calls original unchanged); cached-hit log opt-in experimental | 6287-6366 |
| V165 | Blit3D context | `getBlit3DContext` | On RPL ctx+0xb8 is NULL (base initWithOptions doesn't set it); accept any non-null ctx, including depth/color resolve fallbacks. Lists the guarded callers V121/V122/V124/V130. | spoofed-only | 6287-6290, 6349-6362 |
| V194 | Blit3D context / GPU restart | `getBlit3DContext`: `callback->v131CachedBlit3DCtx` | Always calls Apple's original first; if it returns a different non-null ctx than the cache (gpuRestart reallocated contexts) the cache is updated; cache only used when original returns NULL. | spoofed-only | 6292-6331 |
| V131 | (reference) | `v131CachedBlit3DCtx` field | Cache variable for the Blit3D context (named after V131, defined elsewhere). | referenced only | 6323-6346 |
| V502 | Blit3D diag | `getBlit3DContext`: logs param_1 and task+0x298 (first 12) | Tells whether the call will create a context (param_1=true) or read the cache. | diagnostic-only | 6311-6319 |
| V121 / V122 / V124 / V130 | (reference) | comment in getBlit3DContext | Names the guarded callers initBlitUsage (V121), markBlitUsage (V122), beginCoalescedSegment (V124), barrierSubmission (V130) whose ctx+0xb8 dereferences early-return on !isRealTGL. | referenced only here (see their own rows) | 6288-6289 |
| V128 | Blit3D init | `blit3d_init_ctx` hook | Rollback: plain passthrough to the original Blit3D context init on all paths (previous V128 suppression removed). | always (passthrough) | 6368-6372 |
| V181 | Blit3D scratch | `blit3d_initialize_scratch_space` hook | On spoofed paths runs the original scratch init only if the IOAF2 `lockForCPUAccess`/`unlockForCPUAccess` symbols were resolved (scratch shaders are needed for RCS routeSel=3 blits; otherwise GPU stalls on null shader code); suppresses it if unresolved. Fork note (6594) says the lock/unlock resolution is commented out in kern_green, so this wrapper aborts. | spoofed-only | 6374-6402 |
| V69 | Blit3D init | `IGHardwareBlit3DContextinitialize` hook (IGHardwareBlit3DContext::initialize) | Replaces the original on spoofed HW because it faults at +0x4c writing 0x44 bytes to scratch buffer+0xD000 (unmapped); logs mapped buffer (ctx+0xd8) internals and ctx fields 0xb0-0x128 for the first 6 calls. | spoofed-only (real TGL calls original) | 6458-6546 |
| V73 | Blit3D init | same | Fixed double-dereference getMember bug and NULL `this` guard (log + return). | always | 6463-6470 |
| V106 | Blit3D init | same; boot-args `-ngreenV69AllowOriginal`, `-ngreenV69SkipOriginal` | Skip Apple's original initializer on spoofed paths by default; `-ngreenV69AllowOriginal` requests it only if mappedBuf GPU VA (+0x38) nonzero and size (+0x20) >= 0xD040. | spoofed-only, default skip | 6498-6538 |
| V111B | Blit3D init | same; `-ngreenV69ForceOriginalUnsafe`; `shouldForceFullMetalPath()` used only for logs | The original runs only if AllowOriginal && !SkipOriginal && safe && (isRealTGL \|\| ForceOriginalUnsafe); otherwise logs why it is blocked. | opt-in (`-ngreenV69AllowOriginal` + `-ngreenV69ForceOriginalUnsafe`), default-off | 6517-6534 |
| V148 (initialize replica) | Blit3D init | same: zeroes ctx+0xe8..+0x110, then calls `oblit3d_init_ctx(that)` if ctx+0xD8 non-null | Re-implements initialize() minus `blit3d_initialize_scratch_space` (whose memcpy of blit shader kernels hits a read-only/unmapped page on RPL), then emits the 3D state init commands via blit3d_init_ctx. Fork BLITSCRATCH2 (gated `-ngreenxefix` && !`-ngreennoblitscratch`) re-adds the scratch step via a temp buffer, because skipping it left zeroed blit kernels and caused gpuRestart cascades. | spoofed-only | 6548-6628 |
| V163 | Context image | `startGraphicsEngine` hook: after original, masked clear of GEN7_FF_SLICE_CS_CHICKEN1 bit14 (write 0x40000000) | startGraphicsEngine writes 0x40004000 to 0x20E0 enabling PERCTX_PREEMPT_CTRL; clear it right after so the first context snapshot captures 0. Also calls `applyPreStartEngineWorkarounds` before the original. Fork: XE-INERT (`xeEngineInertOn`) skips the whole function; xeLrcaRepair afterwards. | spoofed-only | 6631-6677 |
| V71S | GT errors (engine start) | `applyPreStartEngineWorkarounds`: ERROR_GEN6=0 if set; RING_EMR(RCS)=RING_EMR(BCS)=0xFFFFFFFF | Minimal pre-start safety: clear ERROR_GEN6 and mask ring errors. GT chicken/clock-gate WAs are deliberately NOT applied pre-start (they left RING_CTL=0). Fork xeFixApplyRcsPsmi (Wa_1607297627, gated by xefix) runs first. | spoofed-only (called only on !isRealTGL) | 6679-6707 |
| (no tag) pre-stop GT WAs | GT | `applyPreStopEngineWorkarounds`: Wa_14011060649 (VDBOX_CGCTL3F10 IECPUNIT_CLKGATE_DIS on RCS/BCS/VEBOX), Wa_14011059788 (DFR_DISABLE), Wa_14015795083 (GEN7_MISCCPCTL DOP clock gate), Wa_1409142259 (DISABLE_CPS_AWARE_COLOR_PIPE), WaDisableGPGPUMidThreadPreemption, MCR selector 0/0, rcs_engine_wa_init FF_SLICE_CS_CHICKEN1 PERCTX_PREEMPT_CTRL masked enable | Linux gen12 GT/RCS workarounds applied before every stop so they persist into the context image. Note: it ENABLES PERCTX_PREEMPT_CTRL, which V162/V163/V164 then clear. | spoofed-only (called from stopGraphicsEngine on !isRealTGL) | 6709-6735 |
| V71 (PRE/POST) | GT errors | `applyPreStopEngineWorkarounds` (PRE) and `stopGraphicsEngine` after original (POST): ERROR_GEN6=0 if set, RING_EMR(RCS/BCS)=0xFFFFFFFF; POST also drains CSB (RING_CONTEXT_STATUS_PTR read ptr := write ptr) | Clears errors and masks ring error interrupts around the stop; POST drains stale CSB entries. | spoofed-only | 6737-6746, 6861-6881 |
| V160 | GT CSB diag | `stopGraphicsEngine` POST log | Logs CSB read pointer advance, EXECLIST_STATUS and FF_SLICE_CS_CHICKEN1 (part of the V71 POST drain). | spoofed-only (log of functional drain) | 6877-6880 |
| V152 | Blitter engine | `applyPreStopEngineWorkarounds`: if BCS CTL != 0 or HEAD != TAIL: RING_TAIL(BCS) := HEAD, RING_CTL(BCS) := 0 | Drains and disables the BCS ring before stop so Apple sees a clean BCS state. | spoofed-only | 6748-6762 |
| V63 | (counter) | `stopGraphicsEngine` static `v63ResetCount` | Reset counter used for log indices; no other behavior. | spoofed-only | 6770-6776 |
| V155 | RCS ring | `stopGraphicsEngine`: saves RING_CTL(RCS)\|1 before stop, writes it back after original | Restores the RCS ring-enable after stop so Apple's startGraphicsEngine can take over. | spoofed-only | 6780-6785, 6857-6859 |
| V53 / V161 | GT reset diag | `stopGraphicsEngine` PRE/POST: RCS CTL/HEAD/TAIL/START/EXECLIST_STATUS; INSTDONE_1 0x206c, 0x2090, GEN8_CS_CHICKEN1, FF_SLICE_CS_CHICKEN1 | Snapshots ring and EU state before/after stop. | diagnostic-only (spoofed) | 6787-6801, 6845-6851 |
| V505 | Ring diag | `stopGraphicsEngine` PRE (first 3): ring GGTT PTE -> physical, `IOMappedRead32` of 32 dwords | Dumps the ring preamble contents while the ring still runs. | diagnostic-only (spoofed) | 6803-6820 |
| V164 (GDRST) | GT reset | `stopGraphicsEngine` PRE: if EXECLIST_STATUS bit0 && HEAD==TAIL, GEN6_GDRST = GEN6_GRDOM_RENDER, poll <=1000 (max 8 times) | Fires a render-domain reset when a "ghost" context is active on an empty ring before Apple's stop. | spoofed-only | 6822-6839 |
| V162 (post-stop clear) | Context image | `stopGraphicsEngine` after original: FF_SLICE_CS_CHICKEN1 masked clear bit14 (0x40000000) | Clears PERCTX_PREEMPT_CTRL that the TGL context image sets (RPL hangs with it). | spoofed-only | 6853-6855 |
| V143 | Blit3D alloc | `IGHardwareBlit3DContextoperatornew` hook | If the original operator new symbol was never resolved, returns NULL (logs once, under experimental flag): identified as root cause of permanently NULL context allocations, NULL-task submitBlit flood and BCS stall. Otherwise passthrough. | always (log opt-in experimental) | 6886-6902 |
| V509 (initWithOptions) | Execlist LRCA | `IGHardwareContextinitWithOptions` hook (IGHardwareContext::initWithOptions), RCS only (this+0x6c==0); LRCA VA at this+0x89; temporarily points GGTT[0] at LRCA page1 via aperture, wbinvd, TLB flush 0x101008 | On RPL restoreFromSafeImage returns true and the g_cInitGfxRingContextRCS memcpy is skipped, leaving DW1/DW8 = 0x00ffffff GEM fill (MI_NOOP) so register restore is skipped and the context goes IDLE. If DW1==0x00ffffff, zeroes DW0-79 and writes MI_LRI 0x11001005 {0x2090=0xffff0001, 0x2034=0, 0x2030=0}, MI_LRI 0x11001003 {0x2038=live RING_START (fallback 0x402eb000), 0x2040=RING_CTL\|1}, DW80=0x05000001; restores GGTT[0]. | spoofed-only | 6904-6997 |
| V508 | Execlist diag | `IGHardwareContextwithOptions` hook (first 4 calls): ctx+0xb8 fifo fields, ring buffer, ctx/slot page1 DW0-7 | Logs FIFO/ring-buffer fields and raw page1 content of context buffer and submission slot. | diagnostic-only (spoofed) | 6999-7035, 7073-7086 |
| V509 (withOptions) | Execlist LRCA | `IGHardwareContextwithOptions`, RCS only: stores `lastRCSCtx`; reads 96 dwords of ctx page1 (GGTT[lrca/4K+1]) and, if submission slot GGTT[0x19] DW1 is not an MI_LRI header (mask 0x1F801000 != 0x11001000) but ctx DW1 is, copies them to the slot via aperture (GGTT[0] temporarily remapped) | The execlist submission slot page hardware restores from stays GEM-filled because Apple never copies the valid LRI block; copying fixes context restore. The saved ctx enables re-runs from populateResetRegisterList/startGraphicsEngine (xeLrcaRepair). | spoofed-only | 7037-7108 |
| V136 | Extended context | `IGHardwareExtendedContextinitWithOptions` hook | If the original symbol is unresolved, logs once and returns 0. | always | 7110-7119 |
| V502 (ExtendedContext) | Extended context diag | `IGHardwareExtendedContextinitWithOptions`: logs ExtCtxParams +0x10 (buffer size) and +0x18 (0 = simple alloc, nonzero = GPU-command path via vtable+0x118, "hang source on RPL") once | Diagnostic. | diagnostic-only | 7121-7131 |
| V115 | Extended context | comment in `IGHardwareExtendedContextinitWithOptions` | Removed: used to suppress initWithOptions to avoid the GGTT[0]->stolen-memory MCE; caused getBlit3DContext to return NULL and submitBlit+0x28e to fault. V116 makes it unnecessary; result now passed through. | dead (removed) | 7134-7137 |
| V501 | Extended context diag | same: logs ret and ctx+0xb8 before/after (first 8) | Diagnostic. | diagnostic-only | 7138-7143 |
| (no tag) per-engine ctx WAs | Extended context | code after `return ret;` in `IGHardwareExtendedContextinitWithOptions`: RCS MOCS CMD_CCTL, Wa_1606700617, Wa_1606931601, Wa_1407928979, Wa_1406941453, Wa_1409804808, Wa_14010229206, Wa_1607297627 (RING_PSMI_CTL), PERCTX_PREEMPT_CTRL, whitelist PS_INVOCATION_COUNT/GEN7_COMMON_SLICE_CHICKEN1/HIZ_CHICKEN/GEN11_COMMON_SLICE_CHICKEN3, Wa_1409142259, WaDisableGPGPUMidThreadPreemption, Wa_16011163337 FF_MODE2; CCS MOCS; BCS BLIT_CCTL MOCS | Linux-style per-engine workarounds that are unreachable (after an unconditional return); the fork re-adds Wa_1607297627 via xeFixApplyRcsPsmi. | dead (unreachable) | 7146-7303 |
| V121 | Blit usage guard | `initBlitUsage` hook | On spoofed HW skips the original when ctx+0xb8 is NULL (original reads [ctx+0xb8]+0x178 without null check, early init). | spoofed-only | 7306-7318 |
| V186 | Blit submit | `submitBlit` hook (IGAccelBlit::submitBlit), first check | On spoofed HW, if `getV142SubmitBlitMode()` != 3, returns immediately before any task/context logic (mode 2 -> 1, mode 1 -> 0, else kIOReturnUnsupported), because the V149/V171 path writes task+0x298 and could poison task lifetime (later crash in IGAccelTask::release from GC interrupt path). IMPORTANT: helper default is mode 1 (V170, outside range) so by default on spoofed HW every submitBlit is a no-op returning 0 and the code below (V149/V135/V120/V142/V193) is only reached with `ngreenV142=3` or `-ngreenV142orig`. | spoofed-only, active by default | 7330-7363 |
| V186D | Blit submit diag | same, when `isV142Diag2DBypassEnabled()` | Logs routeSel (blit[0xA2]>>3 & 3) and whether 3D flags are set (blit[0x3C]&1, blit[0x84]&1, blit+0x20, blit+0x68) for bypassed blits (first 64). | opt-in `ngreenV142diag2d=N` / `-ngreenV142diag2d`, diagnostic-only | 7342-7356 |
| V142 | Blit submit | `submitBlit`, after task validation; `getV142SubmitBlitMode()` | Same mode switch as V186 applied later (bypass Apple's submitBlit that wedges BCS): 0 = kIOReturnUnsupported, 1 = return 0, 2 = return 1, 3 = original. Boot-args `ngreenV142=N`, `-ngreenV142orig`(3), `-ngreenV142pass`(2), `-ngreenV142ok`(1), `-ngreenV142hardunsupported`(0), `-ngreenV142unsupported`(1); default 1 (V170). Effectively unreachable with mode != 3 because V186 returns first. | spoofed-only; redundant behind V186 | 7493-7519 |
| V170 | (reference, helper outside range) | `getV142SubmitBlitMode` default | Defaults spoofed submitBlit to mode 1 because the TGL-format 3D blit commands stall the RPL EU (INSTDONE=0xfffffffe) -> hangcheck/reset loop that kills WindowServer within 120 s. | default (spoofed) | helper above range |
| V149 | Blit submit / task ctx | `submitBlit` `ensureTaskContext` lambda: task+0x298 | The Blit3D context is cached at task+0x298 (previous code wrongly used +0xb8). If NULL on spoofed HW, calls `callback->getBlit3DContext(task, true)` and stores result at task+0x298. Logs task entries (experimental flag). | spoofed-only, reachable only when V142 mode = 3 | 7365-7408 |
| V171 | Blit submit / task ctx | same lambda | Explicitly writes task+0x298 because the hooked getBlit3DContext returns the global cached ctx without storing it per task, so the invalid-task check passes. | spoofed-only (mode 3 only) | 7375-7383 |
| V135 | Blit submit guard | `submitBlit`: validate task vtable (+0x0) and ctx (+0x298) | Treats non-null tasks with NULL vtable or ctx as invalid (prevents RIP=0 indirect calls in Apple code); log uses "V149" prefix. Invalid tasks are swapped for `v132CachedTask` if that is valid (V138.swap log). | always when reached (mode 3 / real TGL) | 7409-7441 |
| V120 | Blit submit (NULL task) | `submitBlit` invalid-task branch | Returns without calling Apple: mode from `ngreenV120=N` / `-ngreenV120pass`(2) / `-ngreenV120ok`(0) / `-ngreenV120fail`(1); default 1 (kIOReturnUnsupported) on spoofed, 0 on real TGL. 0 = return 0, 1 = unsupported, 2 = return 1. | always when task invalid (spoofed only reachable with V142 mode 3) | 7443-7491 |
| V193 | Blit submit (3D route) | `submitBlit`: blit[0xA2] routeSel == 3 | Blocks routeSel=3 blits (blit3d_submit_commands running TGL-compiled EU shaders that stall RCS on RPL -> BCS MI_SEMAPHORE_WAIT deadlock -> Sig 803 reset loop), returning 1; cursor sprite becomes invisible. Fork: disabled when `-ngreenxefix` unless `-ngreenv193` (the hang was actually blit3d state written 0x18 bytes off, fixed by XE-POOLALIGN; blocking leaves stamp 9 pending). | spoofed-only, default on, off with `-ngreenxefix` (re-enable `-ngreenv193`); reachable only with V142 mode 3 | 7521-7544 |
| V134 | Blit submit guard | `submitBlit`: `callback->osubmitBlit` NULL | Prevents a NULL call-through when the route capture failed: returns 0 on spoofed, kIOReturnUnsupported on real TGL. | always (only if symbol unresolved) | 7546-7556 |
| V122 | Blit usage guard | `markBlitUsage` hook | On spoofed HW skips the original when ctx+0xb8 is NULL (same reasoning as V121). | spoofed-only | 7561-7572 |
| V123c | Segment prepare | `IGAccelSegmentResourceListprepare` hook | Passthrough: earlier short-circuit removed because V121/V122 guard the inner calls and skipping prepare left command-queue state incomplete (stalls). | always (passthrough) | 7582-7588 |
| V124 | Segment DTrace guard | `beginCoalescedSegment` hook | Writes queue+0x830 = 0xFFFFFFFF; on spoofed HW, if cached Blit3D ctx or its +0xb8 is NULL, returns 1 without calling the original (which is pure DTrace and would dereference ctx+0xb8); otherwise calls original. | spoofed-only guard | 7590-7620 |

#### 2. RouteRequestPlus / SolveRequestPlus / LookupPatchPlus in range

None. Lines 3800-7600 contain only hook bodies (wrappers that call `callback->o*` originals); no `RouteRequestPlus`, `SolveRequestPlus`, `LookupPatchPlus` or byte-pattern patch tables are defined in this range. Hooked functions whose bodies are in range (their route registrations are elsewhere): raWriteRegister32 (PLANE_SURF path), AppleIntelFramebuffer::init, initPlatformWorkarounds, getOSInformation, handleLinkIntegrityCheck (stub returning 0), hwInitializeCState, AppleIntelPowerWell::init, prepareToEnter/ExitWake/Sleep, AppleIntelBaseController::start, probeCDClockFrequency (wrap), initCDClock / setCDClockFrequencyOnHotplug / disableCDClock (passthrough), hwRegsNeedUpdate, hwSetupMemory, IntelAccelerator::start, IGAccelDevice::deviceStart, PAVP session callback, getGPUInfo / getGPUInfoICL, populateResetRegisterList, createUserGPUTask, IGAccelTask::withOptions, getBlit3DContext, blit3d_init_ctx, blit3d_initialize_scratch_space, SetGTFrequencyMMIO (fork), IGAccelResource::free (fork), IGHardwareBlit3DContext::initialize, startGraphicsEngine, stopGraphicsEngine, IGHardwareBlit3DContext::operator new, IGHardwareContext::initWithOptions, IGHardwareContext::withOptions, IGHardwareExtendedContext::initWithOptions, initBlitUsage, submitBlit, markBlitUsage, IGMappedBuffer::getMemory (passthrough), IGAccelSegmentResourceList::prepare, beginCoalescedSegment. V143's log hints that a missing RouteRequest for `IGHardwareBlit3DContext::operator new` was the root cause of NULL context allocation.

#### 3. Boot-args and properties in range

##### Boot-args (upstream NootedGreen)

| Boot-arg | Where (lines) | Meaning / default |
|---|---|---|
| `ngreen-buf=N` | 3917-3921 (V99G) | Number of GGTT scanout slots for the V99R/V99G SURF redirect: 1 single, 2 double (default), 3 triple; out-of-range -> 2. Only matters when V99G is active. |
| `ngreen-dmc=<str>` | 4132-4437 (hwInitializeCState) | First char decides: `t`/`T` = TGL DMC v2.12 blob written to 0x80000 + power wells (V101T) + 8Fxxx ctx regs, DC_STATE_DEBUG=2, `dmcIsAdlp=true`, then original (ICL CSR) + V104T + V102T; `a`/`A` = ADL-P DMC v2.16 payload + V100/V101 power wells + 8Fxxx/5Fxxx DKL regs + transcoder A timing/M-N/PPS values (author's 2560x1600 2-lane panel) + DC_STATE_DEBUG=2, `dmcIsAdlp=true`, then original + V104 + V102 + V105 PSR off; `i`/`I` = original + combo-PHY setSignalLevels with zero swing on PHY 0/1; unset/anything else (default, "skip") = original only. `hwConfigureCustomAUX(that,true)` always runs afterwards. `dmcIsAdlp` also enables `adlpDcExit` at hwRegsNeedUpdate. |
| `ngreenSched=N` | 4910 (V44) | Scheduler type 3 (GuC), 4 (IGScheduler4), 5 (host preemptive); only 3..5 accepted. Highest priority; default real TGL 3, spoofed 5. |
| `-ngreentglwithgfx` | 4074 | getOSInformation: `pinfo[1].cameliav = 2` (CamelliaTcon2, needs the GFX kext) instead of 0. |
| `-ngreendp0` / `ngreendp0=N` / `-ngreendp1` / `ngreendp1=N` | 5570 via `isDisplayPipeForceDisabled()` (helper at 142) | V78: dp0 forces `DisplayPipeSupported=0`; dp1 (checked first) keeps native; default native ON. |
| `-ngreenv60` / `ngreenv60=N` / `-ngreenv60off` / `ngreenv60off=N` / `-ngreenexp` / `ngreenexp=N` | 5633 via `isV60MonitorEnabled()` (helper at 118) | V60 GPU health monitor; off by default; `*off` wins. |
| `-ngreenexp` / `ngreenexp=N` | via `isExperimentalMonitorEnabled()` (helper at 109) | Enables experimental logs V137/V138/V143/V148/V149 and the V60 monitor. Default off. |
| `-ngreenV69AllowOriginal`, `-ngreenV69SkipOriginal`, `-ngreenV69ForceOriginalUnsafe` | 6502-6514 (V106/V111B) | Allow / forbid Apple's original IGHardwareBlit3DContext::initialize; on spoofed HW it runs only with Allow + ForceOriginalUnsafe + safe buffer. Default: skipped. |
| `ngreenV142=N`, `-ngreenV142orig`, `-ngreenV142pass`, `-ngreenV142ok`, `-ngreenV142hardunsupported`, `-ngreenV142unsupported` | 7337, 7500 via `getV142SubmitBlitMode()` | submitBlit mode on spoofed HW: 0 unsupported, 1 return 0 (default, V170), 2 return 1, 3 original. |
| `ngreenV142diag2d=N` / `-ngreenV142diag2d` | 7342 | V186D bypass logging. Default off. |
| `ngreenV120=N`, `-ngreenV120pass`(2), `-ngreenV120ok`(0), `-ngreenV120fail`(1) | 7452-7462 | V120 NULL-task return for submitBlit; default 1 (unsupported) on spoofed, 0 on real TGL. |
| `-ngreenNoMetal` | 5676 | Only mentioned in a V50 log string ("display-only"); not parsed in this range. |

##### Boot-args (fork, gating V-features or catalogued elsewhere)

| Boot-arg | Lines | Effect |
|---|---|---|
| `-ngreenxefix` | 3890, 6592, 7532 | Turns OFF V99R/V99G and V193; turns ON BLITSCRATCH2 in V148 initialize replica. |
| `-ngreenv99g` | 3890 | Re-enables V99R/V99G under `-ngreenxefix`. |
| `-ngreenv193` | 7532 | Re-enables V193 under `-ngreenxefix`. |
| `-ngreennoblitscratch` | 6592 | Disables BLITSCRATCH2. |
| `-ngreenkeepefi` | 4308 | ngreen-dmc=adlp: keep UEFI transcoder A timings/PPS if the transcoder is running. |
| `-ngreengva` | 4994 | Keep IOGVA* hardware-video properties (XE-NOGVA removes them by default on spoofed). |
| `ngreengtfreq=MHz` | 6419 | XE-GTFREQ minimum GT frequency request (default 1100, cap 1500, 0 = off). |
| `-ngreennoresfree` | 6435 | Disables XE-RESFREE sanitizing in IGAccelResource::free. |
| (xeEngineInertOn, `-ngreenengineinert` per comment) | 6651 | XE-INERT: skip startGraphicsEngine entirely. |

##### IORegistry properties read / injected

| Key | Direction | Lines | Notes |
|---|---|---|---|
| `SchedulerType` (OSNumber, 3..5) | read from provider (`param_1`, NootedGreen Info.plist personality) | 4917 | V44 second priority. |
| `GraphicsSchedulerSelect` | injected on accelerator | 4931-4935 | V44 result, read by Apple's populateAccelConfig. |
| `Development` / `MultiForceWakeSelect=1` | injected (dict copied or created) | 4939-4969 | Spoofed only; selects SafeForceWakeMultithreaded. |
| `RenderCompressionMode=0`, `AllowIOSurfaceRenderCompression=0` (in Development and top-level) | injected | 4970-4988 | Fork XE-NOCCS (spoofed only). |
| `IOGVACodec`, `IOGVAScaler`, `IOGVABGRAEnc`, `IOGVAHEVCDecodeCapabilities`, `IOGVAHEVCEncodeCapabilities` | removed | 4994-4999 | Fork XE-NOGVA unless `-ngreengva`. |
| `MetalPluginName`, `IOGLBundleName`, `IODVDBundleName`, `IOVARendererID`, `IOCFPlugInTypes` | read (diag) / injected if MetalPluginName missing | 5298-5313, 5508-5556 | V44/V45; TGL or ICL names chosen by `tglHWLoaded`; also IOGVACodec/IOGVAScaler/IOGVABGRAEnc="Gen10", IOSourceVersion="0.0.0.0.0", IOVARendererID=17301568, IOCFPlugInTypes {ACCF0000-0000-0000-0000-000a2789904e: IOAccelerator2D.plugin}. |
| `IOAccelDisplayPipeCapabilities` {DisplayPipeSupported=0, TransactionsSupported=0} | injected | 5540-5548 (V77, fallback only), 5571-5580 (V78, -ngreendp0) | Forces CPU compositing. |
| `AAPL,LCD-PowerState-ON` (bool) | injected on framebuffer | 4547, 4563, 4578, 4600 | Sleep/wake hooks (no V tag); hooks also force fb+0x49e0 state = 4 after Apple's code. |

##### Other untagged behavior in range (for completeness)
- getOSInformation (4061-4110): patches `gPlatformInformationList[1]` flags (DISABLE_PIPE_SCRAMBLE, FBC, ALLOW_CONNECTOR_RECOVER, FORCE_POWER_ALWAYS_CONNECTED, AVOID_FAST_LINK_TRAINING), mobile, 3 pipes/ports/FBs, 1 slice/8 EU/10 subslices, connectors: 0 LVDS (eDP, DDI-A, pipe 0, flags 0x18), 1 DP (pipe 2, flags 0x401), 2-3 dummy.
- initPlatformWorkarounds (4009-4026): flags_ig = BOOST_PIXEL_FREQUENCY_LIMIT; fInfoFlags2 = ALT_PWM_INCREMENT1/2, ENABLE_SLICE_FEATURES, FORCE_POWER_ALWAYS_CONNECTED, AVOID_FAST_LINK_TRAINING (always).
- AppleIntelFramebuffer::init (3993-4005): stamps `ccont` at fb+0x4a40 and fb+0xc40 before and after original.
- PowerWell::init (4481-4514): forces `fAlwaysOn=1` and fills NULL `fMMIO` on spoofed HW.
- adlpDcExit (4443-4479): when `dmcIsAdlp` and DC_STATE_EN != 0, disables DC states and restores power wells, 8Fxxx context regs, PP_CONTROL=0x67 and PSR off; called from hwRegsNeedUpdate.
- AppleIntelBaseController::start (4604-4652): DC_STATE_EN=0, Wa_14011294188, PCH reset handshake, Wa_14011508470, WA#1185 (Wa_14010480278), Wa_14013723622 before start; `registerService()` after successful start to trigger accelerator matching.
- probeCDClockFrequency wrap (4654-4722): sets BXT_DE_PLL_ENABLE bit31 if clear and always runs sanitizeCDClockFrequency (disable CDCLK + set 648/652.8 MHz by ref clock) to avoid the PCU mailbox 0x6 verify failure (0x9b9b9b9b) on ADL-P that otherwise skips boot-display setup. Old initCDClock reimplementation commented out.
- getGPUInfo/getGPUInfoICL topology overrides, PAVP command 4 short-circuit (see table).


### 2.C kern_gen11.cpp lines 7600-11202, kern_green.cpp, DYLDPatches.cpp and other modules
Scope: kern_gen11.cpp lines 7600-11202; kern_green.cpp/.hpp, kern_start.cpp, DYLDPatches.cpp/.hpp, kern_genx.cpp, kern_patcherplus.cpp, Firmware.cpp/FirmwareADLP.cpp, HDMI.cpp, IntelDPLinkTraining.cpp, DisplayMergeNub.cpp, kern_netdbg.cpp, kern_model.hpp, Info.plist.
Conventions: "spoofed-only" = runs only when NGreen::isRealTGL == false (CPUID model is not 0x8C/0x8D, e.g. RPL-P 0xBA). "V60 monitor gate" = isV60MonitorEnabled(): on with `ngreenv60=1`/`-ngreenv60` or `ngreenexp=1`/`-ngreenexp`, forced off with `ngreenv60off`/`-ngreenv60off`; default OFF. Line numbers are first-last occurrence of the tag in that file.

#### 1. V-tag table

| Tag | Area | Where (file:function, hooked symbol, registers) | What + problem addressed | Status | File:lines |
|---|---|---|---|---|---|
| V12 | display | gen11:Gen11::hwConfigureCustomAUX (`__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb` / `__ZN24AppleIntelBaseController20hwConfigureCustomAUXEb`, routed gen11:1238/1257) | V12 had added 0x863xx combo-PHY writes in Custom-AUX enable; they broke EDID reads (56283 us failure vs 3663 us native). Now pure passthrough + log. | always (passthrough); V12 writes dead | kern_gen11.cpp:10643-10652 |
| V39 | other (IOAF2 patch) | kern_green:processKext (IOAcceleratorFamily2) | Diagnostic run that found the Sonoma prologue `41 54 53 50` and two `test edx,imm32; je` hits (+0x38B0, +0x102E6) used by V40. | historical (comment) | kern_green.cpp:508-509 |
| V40 | other (IOAF2 capability check) | kern_green:processKext, LookupPatchPlus "f2" on IOAcceleratorFamily2 | `41 54 53 50 F7 C2 ?? ?? ?? ?? 74 ??` -> byte10 `EB` (je->jmp), count 2: bypasses capability checks; "without it hang on boot". | always | kern_green.cpp:508-521 |
| V41 | other (IOAF2 device-ID check) | kern_green:processKext, LookupPatchPlus "f1" | `75 ?? 49 8B 87 ?? ?? ?? ?? 4C 8B 80 ?? ?? ?? ?? 45 8B 8F` -> `90 90` (NOP jne of a device-ID compare so 0x9A49 falls through). | commented-out (disabled by V206) | kern_green.cpp:523-533 |
| V42 | diagnostics | gen11:Gen11::forceWake HANGCHECK | Enumerates accelerator children + user clients, logs IRQ enable/mask/master regs at hang time. | diagnostic, default-on one-shot (15th forceWake call; needs forceWake route; off with -ngreenxefix) | kern_gen11.cpp:9973-10015 |
| V44 | personality/properties | gen11:injectAcceleratorPersonality; v44DelayedBundleLog/v44ScheduleBundleLog (scheduled gen11:5516) | Injected IntelAccelerator personality carries MetalPluginName/IOGLBundleName/IODVDBundleName (AppleIntelTGLGraphics{MTL,GL,VA}Driver), IOGVACodec/Scaler/BGRAEnc "Gen10", IOVARendererID 0x1084000, IOSourceVersion, IOCFPlugInTypes IOAccelerator2D.plugin — "required by IOAcceleratorFamily2 and WindowServer"; logger prints these 3 s after start. | personality: always (when HW accel kext loads); logger: diagnostic | kern_gen11.cpp:9500-9550, 10558-10593 |
| V45 | diagnostics (+ force-start via V55) | gen11:v45DelayedChildCheck / v45ScheduleDelayedCheck; HANGCHECK V45 | Delayed (3..120 s) dumps of accelerator state, children, siblings; HANGCHECK logs IOService state/isOpen. Timers are scheduled unconditionally on spoofed path (gen11:5601-5609) but the callback returns immediately unless V60 monitor gate is on. | default-off (V60 monitor gate); HANGCHECK part default-on | kern_gen11.cpp:8219-8312, 9991-10009 |
| V47 | diagnostics / power | gen11:forceWake HANGCHECK | RCS/BCS RING_TIMESTAMP (base+0x358) to see if engines ever ran; re-checks GEN11_GFX_MSTR_IRQ and re-enables master IRQ if cleared. | default-on one-shot (same as HANGCHECK) | kern_gen11.cpp:9917-9942 |
| V50 (DYLD sandbox) | user-space/DYLD | DYLDPatches::wrapCsValidatePage, libsystem_sandbox gpu_bundle_find_trusted | "/Library/GPUBundles" -> "/Library/Extensions" so Metal finds the user-installed TGL MTL bundle. | commented-out | DYLDPatches.cpp:91-129 |
| V50 (DYLD ICL) | user-space/DYLD | DYLDPatches::wrapCsValidatePage, ICL Metal driver in shared cache | Masked patch: jne->jmp after `cmp edi,0x8A5C8086 / 0x8A5D8086; call hwcap; test al,al` so the HW-cap check always succeeds (fallback if TGL driver not used). | always (DYLD on, Ventura+) | DYLDPatches.cpp:131-174 |
| V52 | personality / firmware/GuC | kern_green:processPatcher (CPUID detection); gen11:Gen11::loadGuCBinary (`__ZN13IGHardwareGuC13loadGuCBinaryEv`, always routed gen11:1595/1891) | isRealTGL = CPUID model 0x8C/0x8D; master switch for all spoofed-only code. loadGuCBinary: real TGL calls original; RPL returns 1 without loading GuC (cannot authenticate GuC) -> host (execlist) scheduling. | detection always; GuC stub spoofed-only | kern_green.cpp:251-265; kern_gen11.cpp:7965-7975 |
| V54 / V54W | power/IRQ | gen11:Gen11::v54IrqWatchdog (thread_call armed gen11:5168 on spoofed path) | Every 2 s x5 (10 s): if GEN11_GFX_MSTR_IRQ master bit cleared during driver init, re-enable it; logs RCS CTL/HEAD/TAIL/ERROR_GEN6. | spoofed-only, always | kern_gen11.cpp:9427-9497 |
| V55 | other (IOKit service start) | gen11:v45DelayedChildCheck (>=3000 ms) | Calls registerService(kIOServiceAsynchronous) on IGAccelDevice / IGAccelSharedUserClient / IGAccelCommandQueue children stuck at state 0; never on IOAccelDisplayPipeUserClient2 (starting it caused stamp-9 timeouts every ~5.4 s -> GPURestartSignaled). | default-off (inside V45, V60 monitor gate) — despite V110 comment "unconditionally" | kern_gen11.cpp:8242-8272 |
| V57 | scheduler/CSB (error clear) | comments in V60 monitor / raReadRegister32 | Timer-based R/W clear of ERROR_GEN6 ("V57-proven") — referenced approach. | dead (only in comments / commented-out code) | kern_gen11.cpp:8330-10284 |
| V59 | diagnostics | comments (V60 monitor, raReadRegister32) | Earlier ReadRegister32 intercept; caused 0-children regression (display driver loops in power-well cycling). Abandoned. (V59 is also the label of the delayed child checks scheduled at gen11:5602.) | dead (comment) | kern_gen11.cpp:8331-10282 |
| V60 / V60M | diagnostics / scheduler | gen11:Gen11::v60GpuHealthMonitor (armed gen11:5636; re-armed 9273, 60 x 2 s) | Periodic GPU health log: ERROR_GEN6, RCS HEAD/TAIL/CTL/EMR, EXECLIST_STATUS, CSB ptr, GT_INTR_DW0, RENDER_COPY_INTR_ENABLE. Write path (clear ERROR_GEN6, re-mask EMR; `-ngreenv60rw`) commented out. In raReadRegister32 "V60" marks removal of ReadRegister32 hooks (routes commented gen11:1183-1186). | default-off (V60 monitor gate); RR32 hook removal: dead code | kern_gen11.cpp:8314-9295, 10282-10289 |
| V60 (DYLD) | user-space/DYLD | DYLDPatches: CoreDisplay RunFullDisplayPipe+2103 | `[device isRemovable]` objc_msgSend replaced by `mov al,1` (receiver is an __NSCFNumber on spoofed HW -> NSInvalidArgumentException, WS crash loop); returning 1 skips CoreDisplay::Assert. | spoofed-only and not forceFullMTL | DYLDPatches.cpp:187-205, 349-352 |
| V61 | diagnostics | gen11:Gen11::forceWake | Per-call forcewake logging silenced (flooded Lilu buffer, 384+ lines/boot, evicting V60M logs). | always | kern_gen11.cpp:9667, 10103 |
| V63 / V63W | diagnostics (scheduler) | gen11:wrapWriteRegister32 helper (V63W); V60 monitor (V63 ACTHD/CCID/GEN12_RING_FAULT) | V63W logs first 100 writes to RCS MMIO 0x2000-0x2FFF (no modification). V63 adds ACTHD/CCID/RING_FAULT to monitor (logged as V66M). | V63W: spoofed-only diagnostic on the helper path; V63: default-off | kern_gen11.cpp:8199-8214, 8366-8369 |
| V64 | scheduler/CSB | gen11:v60GpuHealthMonitor ticks 1 and 3 | Tick 1: dump 6 RCS CSB entries + RC_INTR_EN/RCS mask/GT_DW0/MSTR/INTR_IDENTITY. Tick 3: if CSB write ptr != read ptr and `-ngreenv60rw`, advance software read pointer in RING_CONTEXT_STATUS_PTR (unstick scheduler waiting on CSB). | default-off; write needs ngreenv60rw | kern_gen11.cpp:8556-8574, 8692-8710 |
| V65 / V65M | power/IRQ | gen11:v60GpuHealthMonitor (two commented copies); helper getV65Tier1WantBits (gen11:263) | Per-tick enforce tier-1 RCS (and BCS if V142 mode 3 or `-ngreenbcsirq`) bits in GEN11_RENDER_COPY_INTR_ENABLE and unmask CTX_SWITCH/USER in GEN11_RCS0_RSVD_INTR_MASK (Apple leaves RCS0 disabled; V64's fix at T+10 s was too late). | monitor copies commented-out; logic lives in V65W + "V65 pre-start" (gen11:~5150, outside range) | kern_gen11.cpp:8738-8790 |
| V65W | power/IRQ / scheduler | gen11:v54IrqWatchdog | During first 10 s: set tier-1 bits in RENDER_COPY_INTR_ENABLE, unmask RCS ctx-switch/user IRQ, clear ERROR_GEN6 (so error handler doesn't kill the engine). Ring activates at T+8-10 s; must be set before. | spoofed-only, always | kern_gen11.cpp:9448-9478 |
| V66M | diagnostics | v60GpuHealthMonitor log line | Logs ACTHD/CCID/FAULT/GT_DW0/RC_EN each tick. | default-off | kern_gen11.cpp:8547 |
| V67 | diagnostics (scheduler) | v60GpuHealthMonitor tick 1 | Dumps accel+0xE00 scheduler struct (128 QWORDs), objects at accel+0xE08..0xE38 with class names, IOWorkLoop and its event-source chain. | default-off | kern_gen11.cpp:8576-9190 |
| V68 | scheduler/CSB (+diagnostics) | v60GpuHealthMonitor ticks 1/5 | Tick 1: TLB_DATA0/1, GUC_STATUS 0xC000, RCS EIR; "early sched clear": if sched+0x0C (scheduler error e.g. 0x7B blocking all submissions) != 0, zero sched+0x0C and sched+0x00 (active write, not gated by ngreenv60rw). Tick 5: deep dump of e08obj+0x100..0x300, fence regs 0x100000.., FAULT/TLB, encodeFailureStack (accel+0x1C50). HW ERROR_GEN6=0x7B recurs every ~10 s. | default-off (V60 monitor gate) | kern_gen11.cpp:8549-9240 |
| V68B | diagnostics | v60GpuHealthMonitor tick 10 | Re-dump e08obj and scheduler. | default-off | kern_gen11.cpp:9249-9260 |
| V68F | diagnostics | v60GpuHealthMonitor | On ERROR_GEN6 != 0 log GEN8_FAULT_TLB_DATA0/1. | default-off | kern_gen11.cpp:8553 |
| V70 | memory/GGTT (diagnostics) | v60GpuHealthMonitor tick 1; commented transition tracker | GGTT PTE dump from 0x800000 (pages 0-15, 0x7FFE-0x8001 around 128 MiB stolen boundary) + BAR length; commented-out child-transition / "IGAccelDevice LOST" dump. | default-off; transition part commented-out | kern_gen11.cpp:8399-8710 |
| V72R / V72W / V72Wp / V72F | power/IRQ (error mask) | V72R raWriteRegister32, V72F FastWriteRegister32 (outside range); V72W/V72Wp gen11:wrapWriteRegister32 | V72x forced RCS/BCS RING_EMR (0x20B4/0x220B4) writes to 0xFFFFFFFF (blanket error mask). All removed; V72Wp only logs EMR writes (6 lines). | V72W/R/F dead (removed); V72Wp diagnostic | kern_gen11.cpp:8186-8198, 9314 |
| V74 (V71 enforcer) | memory/GGTT + scheduler | gen11:Gen11::v71EmrEnforcer, 50 ms permanent thread_call armed gen11:5611-5626 on spoofed path | Originally forced RING_EMR(RCS/BCS)=0xFFFFFFFF and cleared ERROR_GEN6 every 50 ms (Apple rewrites EMR via direct MMIO, unhookable). EMR part REMOVED; timer now read-only for EMR and hosts V116. | timer: spoofed-only always; EMR writes dead | kern_gen11.cpp:9290-9411 |
| V74E | diagnostics | v71EmrEnforcer | Stop timer and release retain when MMIO invalid (GPU teardown). | spoofed-only | kern_gen11.cpp:9399 |
| V74Ep | diagnostics | v71EmrEnforcer | Log EMR_RCS/EMR_BCS/ERROR_GEN6 (first 3 ticks, or while abnormal up to tick 50). Suppressed by fork `-ngreenxefix` (spinlock-timeout panic inside this SYSLOG). | spoofed-only diagnostic | kern_gen11.cpp:9324-9331 |
| V75 | display (diagnostics) | v60GpuHealthMonitor ticks 3/30 | Dumps PIPE_CONF 0x70008, PIPE_SRCSZ 0x6001C, PLANE_CTL/STRIDE/SURF/SIZE/POS/OFFSET 0x70180-0x70194, TRANS_CONF/HTOTAL/VTOTAL, DDI_FUNC_CTL 0x60400/0x60100, BLC 0xC8250/0xC8254, PWR_WELL 0x45400, DC_STATE_EN 0x45504 (black-screen diagnosis). Then calls adlpDcExit("V60") if ADL-P DMC loaded and DC_STATE != 0. | default-off | kern_gen11.cpp:8870-8967 |
| V76 | display/memory (diagnostics) | v60GpuHealthMonitor tick 5 | PLANE_SURF -> GGTT PTE/phys, watermarks 0x70240..0x70278, PSR 0x64800/0x64840/0x60900, gamma samples (index writes only with ngreenv60rw), reads first FB page pixels (hypothesis: SURF points at zeroed memory). | default-off | kern_gen11.cpp:8966-9053 |
| V77 | display / personality | gen11:injectAcceleratorPersonality (IOAccelDisplayPipeCapabilities); commented DisplayPipe terminator in monitor (`ngreenV77DelayKill`) | Original V77 forced DisplayPipeSupported/TransactionsSupported = 0 (prevent WS GPU compositing crash) and periodically terminated IOAccelDisplayPipeUserClient2. Terminator disabled (blocked WS main thread -> watchdog T+40/80 s). Capabilities now true by default; 0 only with `ngreendp0`/`-ngreendp0` (fork xe/dpcaps rewrote the gate). | terminator commented-out; caps value: fork-modified (default true) | kern_gen11.cpp:8448-8482, 10597-10610 |
| V78 | display | comment in monitor (with V77); V78A boot-arg logs in isDisplayPipeForceDisabled (gen11:150-178) | V78/V77 DisplayPipe terminator header; V78A = ngreendp0/ngreendp1 parsing ("default native DisplayPipeSupported ON"). | terminator dead; V78A always (log) | kern_gen11.cpp:8448 (+150-178) |
| V79 / V79RW | display | v60GpuHealthMonitor (commented-out blocks) | Plane linearization (clear PLANE_CTL tiling, scale stride, re-arm SURF) for brief Apple-logo; later read-only monitor; V79RW experimental enforce STRIDE=0xA0 + Y-tiled + SURF re-arm. Plane writes caused WS crash loops/hangs. | commented-out | kern_gen11.cpp:8496-8854 |
| V80 / V80L | display | v71EmrEnforcer (commented-out) | Legacy one-shot plane linearization in first 3 enforcer ticks (<=150 ms), continuous with `-ngreenv80l`; continuing after WS owns the plane -> crash loop -> watchdog KP at T+120 s. | commented-out (boot-arg parsed but unused) | kern_gen11.cpp:9333-9367 |
| V82 / V84 / V87 | display (diagnostics) | comments above V88 | V82: display engine reads PLANE_SURF via GGTT (magenta visible); V84: page-by-page FB fill without GGTT change; V87: SURF==SURFLIVE, only plane 1 active, pipe B off. | historical comments | kern_gen11.cpp:9057-9062 |
| V83 | memory/GGTT | comment | FB fill variant that redirected SURF to GGTT page 0 -> MCE at stolen base 0x3E800000 (MC bank 11, all CPUs). "Never touch stolen base." | dead (comment) | kern_gen11.cpp:9036-9059 |
| V85 | diagnostics | V88 + statics Gen11::v85PersistMap/v85SurfAddr | Persistent cache-inhibited mapping of first FB page during V88. | default-off | kern_gen11.cpp:9149-9153, 9415-9417 |
| V88 | display (diagnostics, destructive) | v60GpuHealthMonitor ticks 1-30 | Transcoder/DDI/PIPE_STATUS/DSB/COLOR_CTL/key dumps; GGTT TLB flush (0x101008); fills 4000 FB pages with R/G/B/W bands; re-arms SURF; tick 5 toggles PLANE_CTL bit31. Harmful in dp0 mode. | default-off: needs ngreenexp AND `ngreenv88`/`-ngreenv88` AND V60 monitor | kern_gen11.cpp:9061-9195 |
| V99S | display | mention only | Elsewhere: ensures PLANE_STRIDE=0xA0 + Y-tiled CTL before every SURF arm / redirects setupScanoutMemory's 0x412BE000 write. | defined outside range | kern_gen11.cpp:8821, 9071 |
| V103 / V103F / V103P | power/forcewake (DC states) | V103 raWriteRegister32, V103F FastWriteRegister32 (outside range); V103P V60 monitor | Trio that forced DC_STATE_EN to 0 when ADL-P DMC active. All removed ("no DC_STATE_EN block" experiment); V60 tick now calls NGreen::adlpDcExit("V60"). | dead (removed); adlpDcExit default-off | kern_gen11.cpp:8916-8920 |
| V104P | diagnostics | V60 monitor | Former "V195 miss?" CTL1 log. | dead (removed) | kern_gen11.cpp:8960 |
| V105 | display (gamma) | v60GpuHealthMonitor (ticks 3/30 block) | If PLANE_SURF != 0 and PAL_PREC LUT (0x4A400 index / 0x4A404 data) entries 0 and 128 equal or g128==0 (zero LUT -> black; IGFB blanking pedestal 0x8020080 -> grey), write linear 8->10-bit LUT with auto-increment. (A separate active V105 PSR-disable exists at gen11:~4412, outside range.) | default-off (monitor) | kern_gen11.cpp:8899-8958 |
| V105B | display (gamma) | commented V79 block | Same gamma check after V79. | commented-out | kern_gen11.cpp:8854-8864 |
| V115 | blit/3D | mention only | With V116 "prevents the Blit3D MCE that caused GPU rendering failure" (implementation outside range). | defined outside range | kern_gen11.cpp:8449-8452 |
| V116 / V116E | memory/GGTT | v71EmrEnforcer (every 50 ms); dummy page Gen11::v116DummyBuf/Phys allocated gen11:~5194 | If GGTT_PTE(0) != dummyPhys|0x9, rewrite GGTT[0] lo/hi to a wired zero page and flush GGTT TLB. Apple's TGL FB maps GGTT[0] to stolen base; any GPU VA-0 access then causes a package-wide MCE. | spoofed-only, always | kern_gen11.cpp:9369-9387, 9419-9421 |
| V124 | scheduler/CSB (DTrace) | gen11:Gen11::beginCoalescedSegment (`__ZN19IGAccelCommandQueue21beginCoalescedSegmentEv`, gen11:1814) | Writes queue+0x830 = 0xFFFFFFFF; on spoofed path returns 1 without calling original while cached Blit3D ctx or ctx+0xB8 is null (original is DTrace-only and derefs [ctx+0xB8] in early init). | spoofed-only (route always) | kern_gen11.cpp:7590-7619 |
| V127 | blit/3D | gen11:Gen11::getBlit2DContext (`__ZN11IGAccelTask16getBlit2DContextEb`, gen11:1823) | Calls original; if null / ctx+0xB8 null, fall back only to cached valid Blit2D ctx, never depth/color/3D contexts (submitBlit uses [ctx+0xB8] as Blit2D FIFO/ring state; wrong layout -> black output / wrong ring). | spoofed-only | kern_gen11.cpp:7740-7793 |
| V130 | scheduler/CSB (blit barriers) | gen11:Gen11::barrierSubmission (`__Z17barrierSubmissionR19IGAccelCommandQueueR16IntelAcceleratorR24IGAccelCommandDescriptorR12IOAccelEventtPKt`, gen11:1818) | Guard for deterministic crash at barrierSubmission+0x198 (original submits Blit2D/3D barriers via BCS). Modes: 0 bypass ret0, 1 bypass ret1 (DEFAULT; ret0 makes scheduler spin -> freeze), 2 original, 3 hybrid (bypass first `ngreenV130warmup` calls, default 12, then original). | spoofed-only, default mode 1 | kern_gen11.cpp:7621-7740 |
| V131 | blit/3D | getBlit2DContext; callback->v131CachedBlit2DCtx / v131CachedBlit3DCtx | Caches last valid Blit2D (and Blit3D) context for NULL fallback (used by V127, V124). | spoofed-only | kern_gen11.cpp:7790-7794 (+7609) |
| V136 | blit/3D | getBlit2DContext / getDepthResolveContext / getColorResolveContext | Log once when the original trampoline is null; return cached/nullptr. | always (defensive) | kern_gen11.cpp:7761-7832 |
| V137 | diagnostics | same three getters | Log ctx and ctx+0xB8 (24/32 lines). | opt-in `ngreenexp`/`-ngreenexp` | kern_gen11.cpp:7774-7842 |
| V142 | scheduler/IRQ | mention (getV65Tier1WantBits) | submitBlit mode selector (ngreenV142 / -ngreenV142*); mode 3 (original submitBlit -> BCS) also enables BCS IRQ bit. | defined outside range | kern_gen11.cpp:8773 |
| V150 | scheduler/CSB | barrierSubmission init | `ngreenV130=2`/`-ngreenV130orig` on spoofed RPL is forced to mode 1 (avoid BCS barrier stall) unless `-ngreenV130forceorig`. | spoofed-only | kern_gen11.cpp:7655 |
| V151 | diagnostics | barrierSubmission, getBlit2DContext | Call-count / INIT-vs-RENDER phase logging (rate limited). | spoofed-only diagnostic | kern_gen11.cpp:7679-7745 |
| V181 | blit/3D | kern_green:processKext IOAF2 | SolveRequestPlus `__ZN16IOAccelSysMemory16lockForCPUAccessEP4taskj` / `__ZN16IOAccelSysMemory18unlockForCPUAccessEP4task` into Gen11 for blit3d scratch init. | commented-out (V206) | kern_green.cpp:536-564 |
| V187 | user-space/DYLD | DYLDPatches: CoreDisplay::DisplaySurface::AccessComplete | Whole-function stub (`31 C0 C3` + NOPs over 32-byte prologue) — WS recycle-loop crash in __hash_table::find+0x4 <- AccessComplete+1928 (fault at 0x80). Never signals completion (logo freeze). | spoofed-only, !forceFullMTL, default unless -ngreenv189 | DYLDPatches.cpp:220-238, 374-380 |
| V188 | user-space/DYLD | DYLDPatches: std::__hash_table<id<MTLRenderPipelineState>>::find | Return-null stub for the crashing hash find. Caused black-screen regression. | opt-in `-ngreenV188htfind` (spoofed, !forceFullMTL), debug only | DYLDPatches.cpp:240-258, 387-393 |
| V189 | user-space/DYLD | DYLDPatches: AccessComplete +0x2F | Replace `mov rdi,[rdi+0x178]; test; jz` with `jmp` to minimal completion block (lock dec [backing+0x170] wakes kernel waiter; clear [r15+0x178]); avoids CG calls that deadlocked. | opt-in `-ngreenv189` (replaces V187) | DYLDPatches.cpp:260-380 |
| V195 / V195F / V195W | power/forcewake (power wells) | V195 raWriteRegister32, V195F FastWriteRegister32 (outside range), V195W wrapWriteRegister32 | OR'd UEFI HSW_PWR_WELL_CTL1 bits 14,12 into every 0x45400 write (belief: ICL DMC save/restore clears them, stalling WS vsync). All three removed. | dead (removed) | kern_gen11.cpp:8167-8181, 8961 |
| V195Wp | diagnostics | wrapWriteRegister32 helper | Logs 0x45400 writes when uefiCtl1 != 0 (6 lines). | spoofed-only diagnostic | kern_gen11.cpp:8177-8185 |
| V201 | diagnostics / memory | kern_green: NGreen::setApertureIfNecessary | Maps BAR2 aperture (aperturePtr/apertureLen) for GTT-translated reads (no raw-PA MCE risk); used by HANGCHECK V204-V206. | helper, used by default-on HANGCHECK | kern_green.cpp:343-345; kern_green.hpp:54 |
| V202 | memory/GGTT (diagnostics) | forceWake HANGCHECK | GGTT[0] PTE (stolen base) and PLANE_SURF page PTE present/LLC bits. | default-on one-shot | kern_gen11.cpp:9946-9965 |
| V203 | scheduler/CSB (diagnostics) | forceWake HANGCHECK | 64-bit EXECLIST_STATUS (lo + base+0x238) -> LRCA GGTT = full64 & ~0xFFF (lo-only gave wrong 0x18000). | default-on one-shot | kern_gen11.cpp:9720-9735 |
| V204 | scheduler/CSB (diagnostics) | forceWake HANGCHECK | CSB ptrs + 6 decoded entries; HWS and RING_START PTEs; dumps preamble ring DWORDs via temporary GGTT[0] remap through aperture. | default-on one-shot (transient GGTT[0] writes) | kern_gen11.cpp:9737-9797 |
| V204b | display (backlight) | kern_green:processKext AppleBacklight branch | Route `__ZN15AppleIntelPanel10setDisplayEP9IODisplay` (inject ApplePanels F14T..F19T/F24T tables) + "F%uT%04x"->"F%uTxxxx". | dead: kextBacklight onKextLoadForce commented out, branch never matches | kern_green.cpp:658-669 |
| V205 | scheduler/CSB (diagnostics) | forceWake HANGCHECK | RING_MODE bit15 GFX_RUN_LIST_ENABLE; dumps LRCA page0 (PPHWSP) and page1 (context image) via GGTT[0] remap. | default-on one-shot | kern_gen11.cpp:9726-9845 |
| V206 (gen11) | scheduler/CSB (diagnostics) | forceWake HANGCHECK | Reads preamble stamp targets GGTT[0x40000] (PIPE_CONTROL #1) and GGTT[0x40001]+0x200 (PC2 stamp=2) after wbinvd; invalid PTE would make fwWaitForHardwareRegisterValue poll forever. | default-on one-shot | kern_gen11.cpp:9853-9898 |
| V206 (kern_green) | other | kern_green:processKext IOAF2 | Disabled IOAF2 f1 patch (V41), IGAccelDevice::deviceStart hook and V181 resolves to match "Visual Ehrmanntraut's working config". | always (the disabling) | kern_green.cpp:536 |
| V208 | display | gen11:wrapAppleIntelFramebufferStart (`__ZN21AppleIntelFramebuffer5startEP9IOService`) | Refuse start() for fb != 0 so CoreDisplay never sees phantom FB1/FB2 (assert in CoreDisplay_CreateDisplayForCGXDisplayDevice). | commented-out route (gen11:1167; caused getAttributeExt+0x25 NULL deref at 3/3/3) | kern_gen11.cpp:10500-10516 |
| V209 | display | gen11:wrapEnableController (`__ZN21AppleIntelFramebuffer16enableControllerEv`) | Short-circuit enableController for fb != 0 (partially-initialized phantom FBs -> getAttributeExt+0x25 NULL deref). | commented-out route (gen11:1168) | kern_gen11.cpp:10482-10498 |
| V210 | display | gen11:wrapGetFreeJoinablePathCount (`__ZN21AppleIntelDisplayPath24getFreeJoinablePathCountEv`) | Return 0 (with 1/1/1 pinfo getPathByPipe(1/2) NULL deref at +0x3644 in setupBootDisplay chain). | commented-out route (gen11:1169) | kern_gen11.cpp:10468-10480 |
| V212 | scheduler/CSB (watchdog) | gen11:wrapIGScheduler5IsGpuIdle / wrapIGScheduler4IsGpuIdle (`__ZNK12IGScheduler59isGpuIdleEv`, `__ZNK12IGScheduler49isGpuIdleEv`, gen11:1727-1728) | If original says busy and RCS RING_INSTDONE (ignoring bit0) == 0xFFFFFFFE, report idle. RPL-P INSTDONE bit0 is stuck 0 -> watchdog declares hang -> reset -> startGraphicsEngine retry loop. | always routed (TGL accel); effect only on that INSTDONE pattern | kern_gen11.cpp:10975-11001 |
| V221 | scheduler (stamps) / IRQ | kern_green:wrapWaitForStamp (`__ZN20IOAccelEventMachine212waitForStampEijPj`, IOAF2); flag Gen11::gGfxAccelStartDone (set gen11:5221) | If waitForStamp fails before IntelAccelerator::start() returns, fake success (*outStamp=stamp): the ring executed but the GFX IRQ handler wasn't installed, so stamp 3 never advanced and CoreDisplay asserted; blocking would deadlock matching. | always routed (not isRealTGL-gated) | kern_green.cpp:350-577; kern_gen11.cpp:9423-9425 |
| V500 | other (IOAF2 surface) | kern_green:wrapSetIdMode (`__ZN20IOAccelLegacySurface11set_id_modeEjj`) | Strip mode bits 0xff8073c0 ("TGL-only preemption/mode bits" rejected with kIOReturnUnsupported) on spoofed path; logs 32 calls. Route comment calls it a "diagnostic logger" but it modifies the argument. | spoofed-only (route always) | kern_green.cpp:479-503, 630-640 |
| V93M / V93M64 | memory/GGTT (display safety) | kern_green.hpp: NGreen::writeReg32 / writeReg64 | Writing 0 to an enabled plane's PLANE_SURF (0x60000-0xBFFFF, &0xFFF==0x19C) keeps current SURF, or disables the plane first if current is 0 (SURF=0 -> HW fetch from GGTT[0]/stolen base -> MCE). | always (every NootedGreen MMIO write) | kern_green.hpp:107-159 |
| V507 | scheduler/context (LRCA) | kern_green.hpp field lastRCSCtx | Stores last RCS context seen by IGHardwareContext::withOptions so V507 can re-run LRCA slot repair in populateResetRegisterList. | defined outside this part (comment only here) | kern_green.hpp:304 |

Untagged-but-notable in range (not V-numbered): HANGCHECK block (default-on, one-shot), i915-ported forceWake (`__ZN16IntelAccelerator26SafeForceWakeMultithreadedEbjj`, routed when !wegCoexist || forceFullMTL), releaseDoorbell stub used for enableVDDForAux, injectAcceleratorPersonality, allocateDisplayResources XE_LPD display-core init (no live route found), hwSetMode TRANS_DDI_FUNC_CTL_A pre-write (no route found), readAndClearInterrupts/beginReset/endReset/IGScheduler5resume/raWriteRegister64/wrapPmNotifyWrapper/patchRCSCheck/IntelFBClientControldoAttribute (no live route -> dead), setAsyncSliceCount (MMIO 0xA204), initHardwareCaps (not routed), checkWOPCMSettings (route commented).

#### 2. Route / Solve / LookupPatch / DYLD patch inventory (files in this part)
##### kern_green.cpp (NGreen::processKext / processPatcher)
| Kind | Target binary | Symbol / bytes | Purpose | Gating |
|---|---|---|---|---|
| routeVirtual | iGPU IOPCIDevice | ConfigRead16 / ConfigRead32 -> configRead16/32 | Return "device-id" property instead of real PCI device ID for services named IGPU* | always |
| LookupPatchPlus f2 (V40) | IOAcceleratorFamily2 | `41 54 53 50 F7 C2 ?? ?? ?? ?? 74 ??` -> byte 10 `EB`, mask, count 2 | bypass capability checks (boot hang otherwise) | always |
| LookupPatchPlus f1 (V41) | IOAcceleratorFamily2 | `75 ?? 49 8B 87 .. 4C 8B 80 .. 45 8B 8F` -> `90 90` | device-ID compare bypass | commented-out (V206) |
| RouteRequestPlus | IOAF2 | `__ZN13IGAccelDevice11deviceStartEv` -> wrapIGAccelDeviceStart (force success on spoof) | | commented-out (V206) |
| SolveRequestPlus (V181) | IOAF2 | IOAccelSysMemory lock/unlockForCPUAccess | blit3d scratch | commented-out |
| RouteRequestPlus (V221) | IOAF2 | `__ZN20IOAccelEventMachine212waitForStampEijPj` -> wrapWaitForStamp | fake stamp success before GFX start | always |
| RouteRequestPlus (V500) | IOAF2 | `__ZN20IOAccelLegacySurface11set_id_modeEjj` -> wrapSetIdMode | strip 0xff8073c0 mode bits | always routed; acts spoofed-only |
| RouteRequestPlus (fork) | IOAF2 | IOAccelSysMemory lock/unlock, IOAccelOrphanedMemoryPool::getMemory, IOAccelCommandBufferPool2::init/getBufferPtrNoInc, IOAccelDisplayPipe transaction_* | fork xe/blitscratch2, xe/noorphan, xe/pool, xe/flipdiag | fork (-ngreenxefix etc.; pool always) — catalogued elsewhere |
| LookupPatchPlus | AppleGraphicsDevicePolicy | "board-id" -> "applehax" | neutralise AGDP per-board config | always |
| LookupPatchPlus | AGDP | `02 00 00 83 F8 02`->`..01` / Ventura `41 83 BE 14 02 00 00 02`->`..01` (FB count >=2 -> >=1) | | commented-out |
| KernelPatcher route (V204b) | AppleBacklight | `__ZN15AppleIntelPanel10setDisplayEP9IODisplay` + "F%uT%04x"->"F%uTxxxx" | panel brightness tables | dead (kext not registered) |
| route | kernel | `__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass` -> wrapSafeMetaCast | metaclass aliasing | commented-out |
##### DYLDPatches.cpp (kernel route `_cs_validate_page`, gated by `-nbdyldoff` off, Ventura+)
See notes "DYLDPatches.cpp / .hpp" above for exact bytes: (1) CoreLSKD/CoreLSKDMSE CPUID->Haswell `C7 C0 01 00 00 00 0F A2`->`C7 C0 C3 06 03 00 66 90` (dead: path-check bug); (2) VideoToolbox "MacPro5,1\0MacPro6,1\0IOService" -> current model id; (3) AppleGVA "board-id\0hw.model"->"hwgva-id\0hw.model"; (4) AppleGVAHEVCEncoder "vendor8bit\0IOService\0board-id"->"...hwgva-id" (+ IODT root hwgva-id = Mac-7BA5B2D9E42DDD94); (5) V50 sandbox GPUBundles path (commented); (6) V50 ICL Metal device-ID jne->jmp (always); (7) CoreDisplay assert NOP and (8) RunFullDisplayPipe NULL vcall guard (commented 2026-05-09); (9) GetMTLTexture and (10) GetMTLCommandQueue return NULL (commented 2026-05-16); (11) V60 isRemovable `mov al,1` (spoofed, !forceFullMTL); (12) V187 AccessComplete stub (default spoofed); (13) V189 AccessComplete jump (-ngreenv189); (14) V188 hash-find stub (-ngreenV188htfind). All CoreDisplay patterns are hard-coded for "Sonoma 14.7.1" bytes; matching on Tahoe is not verified by code (miss = silent no-op).
##### kern_genx.cpp (only when renamed "com.xxxxx.driver.AppleIntelICLLPGraphicsFramebuffer" in /Library/Extensions loads)
Solve: disableCDClock, setCDClockFrequency. Routes: probeCDClockFrequency (CDCLK sanitizer), FramebufferController::ReadRegister32 (passthrough), hwSaveNVRAM (ret 0), prepareToEnter/ExitWake, prepareToEnter/ExitSleep (no-ops), IntelFBClientControl::doAttribute (0x923 -> unsupported). LookupPatchPlus: f2 `74 62 48 FF 05 13 71 10 00 BF 04 00 00 00 BE 10 00 00 00` je->jmp ("hwreg"), f3 `74 36 48 FF 05 52 DC 09 00 ...` je->jmp ("getPathByPipe log"), f9 `02 00 5C 8A` -> `00 00 49 9A` (platform-id); many more commented. Dynamic FBMemMgr_Init patch: NOP `shl $0x11` + `and $0xFE000000` -> `mov $stolen_size` (DVMT fix).
##### HDMI.cpp
HDMI::processKext returns false at its first line and HDMI::init does not register any kext; also agfxhda.init()/processKext calls are commented out in kern_green.cpp -> all AppleGFXHDA/AppleHDA/AppleHDAController patches (probe id 0xAB381002, CreateAppleHDA* widgets, `83 7E 0C 00 75`->`90 90 90 90 EB`) are dead (inherited from NootedRed/AMD).
##### kern_patcherplus.cpp
Helper library (ChefKiss): SolveRequestPlus (symbol then optional pattern), RouteRequestPlus (routeMultiple then pattern + routeFunction), LookupPatchPlus (applyLookupPatch or masked findAndReplaceWithMask with count/skip). No patches of its own.

#### 3. Boot-args (relevant to this part)
| Boot-arg | Meaning | Default |
|---|---|---|
| -nbdyldoff | Disable all DYLD/user-space patching (_cs_validate_page hook, hwgva-id) | off (patches on) |
| ngreenforceprops=1 / -ngreenforceprops | Seed IGPU props (ig-platform-id 0x9A490000, device-id 0x9A49, built-in, slot-name, hda-gfx onboard-1, model, framebuffer-unifiedmem 1536 MB, saved-config) early and on the iGPU | off |
| ngwegcoex=1 / -ngwegcoex | WhateverGreen coexistence: skips forceWake (SafeForceWakeMultithreaded) and getGPUInfo routes | off |
| ngreenfullmtlcore / -ngreenfullmtlcore (fallback ngreenfullmtl / -ngreenfullmtl) | Kernel: force full Metal path (route forceWake even in coexist mode, etc.) | off |
| ngreenfullmtldyld / -ngreenfullmtldyld (fallback ngreenfullmtl / -ngreenfullmtl) | DYLD: treat spoofed HW as full-Metal -> skip V60(DYLD)/V187/V189/V188 CoreDisplay patches | off |
| -ngreenv189 | Use V189 AccessComplete jump instead of V187 stub | off |
| -ngreenV188htfind | Apply V188 hash-find stub (debug; black-screen regression) | off |
| ngreenV130=<0..3>, -ngreenV130fail/-pass/-orig/-hybrid, -ngreenV130forceorig, ngreenV130warmup=<0..200> | barrierSubmission spoof mode (V130/V150) | mode 1 (bypass, return 1); warmup 12 |
| ngreenexp=1 / -ngreenexp | Experimental monitors: V137 logs, enables V60 monitor gate, required for V88 | off |
| ngreenv60=1 / -ngreenv60 ; ngreenv60off / -ngreenv60off | Enable/force-disable V60 health monitor (also V45/V55 delayed checks) | off |
| ngreenv60rw=1 / -ngreenv60rw | Allow monitor register writes (V64 CSB read-ptr advance, V76 gamma index reads) | off |
| ngreenv88=1 / -ngreenv88 | V88 scanout color-band fill (needs ngreenexp + V60) | off |
| -ngreenv80l | V80L continuous plane linearization — code commented out | off (no effect) |
| ngreenV77DelayKill=<0..60> | V77 terminator delay — code commented out | 0 (no effect) |
| ngreendp0 / -ngreendp0 ; ngreendp1 / -ngreendp1 | Force display pipe capabilities off (dp0) or native on (dp1) (V77/V78A) | native ON |
| -ngreenbcsirq | Include BCS in V65 tier-1 IRQ enable on spoofed path | off (RCS only unless V142 mode 3) |
| ngreen-dmc=tgl/adlp/icl | hwInitializeCState DMC choice (TGL v2.12 / ADL-P v2.16 blobs from Firmware*.cpp; icl = passthrough + combo-PHY signal levels) — code outside range | skip (passthrough, no blob) |
| netdbg=IP:port | kern_netdbg network logger target | unused (module not compiled in/called) |
| -NGreenDebug | Lilu debug logging for the plugin (DBGLOG) | off |
| fork: -ngreenxefix, -ngreennofwref, -ngreenfwhold, -ngreenmax2560, -ngreenfixbootmode, -ngreennopoolfix, -ngreennoblitscratch, -ngreenorphanreuse | Fork additions (seen in range; -ngreenxefix also silences V74Ep and HANGCHECK) | catalogued elsewhere |

#### 4. Non-Gen11 modules and spoof IDs
- kern_genx.cpp (Genx): ICL-LP framebuffer helper for a renamed copy of AppleIntelICLLPGraphicsFramebuffer in /Library/Extensions (bundle com.xxxxx...). CDCLK sanitizer, NVRAM/sleep-wake no-ops, doAttribute 0x923 block, 3 byte patches (incl. platform-id 0x8A5C0002 -> 0x9A490000), DVMT stolen-size patch in FBMemMgr_Init. Not used by the TGL framebuffer path unless that kext is installed.
- HDMI.cpp: AppleGFXHDA/AppleHDA patch set inherited from AMD NootedRed; fully disabled (early return, no kext registration, init commented).
- IntelDPLinkTraining.cpp: i915 port of icl_combo_phy_set_signal_levels for TGL (tables tgl_combo_phy_trans_dp_hbr/hbr2) and ADL-P (adlp_combo_phy_trans_*), PHY A/B only (no DKL/TC). Only live caller: hwInitializeCState with `ngreen-dmc=icl` (gen11:4426-4427, outside range); TGL/ADL-P callers removed (gen11:4173-4183, 4351-4362).
- DisplayMergeNub.cpp: AnV Software IOService that always fails probe but merges IOProviderMergeProperties into a matching AppleDisplay; Info.plist personality "DisplayAppleIDInjection" matches DisplayVendorID 17119999 (0x1053AFF) / DisplayProductID 18841 (0x4999) and sets IOClass AppleBacklightDisplay (built-in panel treated as Apple backlight display).
- kern_netdbg.cpp: TCP log sender (netdbg=IP:port); its header include is commented out and NETLOG is unused -> dead (and its retry loop never breaks on success).
- Firmware.cpp / FirmwareADLP.cpp: raw DMC payload blobs tgl_dmc_ver2_12_bin and adlp_dmc_ver2_16_bin (CSS/package header stripped, pipe-A payload from file offset 0x310 for ADL-P) used by hwInitializeCState when `ngreen-dmc=tgl`/`adlp`. (IGGucBinary.h / IGHucBinary.h exist but are not included by any .cpp.)
- kern_start.cpp: Lilu plugin entry (Ventura..Tahoe; -NGreenDebug).
- kern_model.hpp: branding table incl. TGL 0x9A49 and RPL 0xA7A0 "Intel Iris Xe Graphics"; getBranding() is never called.
- Spoofed identities: device-id 0x9A49 (TGL GT2) via configRead16/32 hook + "device-id" property (seeded only with ngreenforceprops, otherwise expected from OpenCore DeviceProperties); AAPL,ig-platform-id 0x9A490000 (seed) — shouldForceRPLBringupPlatform() treats missing id or 0x8A5C0002 as "force RPL bring-up" (helper is not called anywhere in kern_green.cpp); real HW 8086:A7A0 detected only as "not real TGL" by CPUID model (V52). Board/model spoofs: hwgva-id Mac-7BA5B2D9E42DDD94 (iMacPro1,1) for AppleGVA/HEVC; AGDP board-id -> applehax. Accelerator personality uses placeholder bundle ids com.xxxxx.driver.AppleIntelTGLGraphics(/Framebuffer) (also in Info.plist NootedGreenDriverProfiles, which no code reads).


---

## 3. Consolidated boot-arg reference

This table lists every boot-arg the plugin parses. They are read with `checkKernelArgument` (flag) or `PE_parse_boot_argn` (value). The "Release" column marks the boot-args used by `EFI/OC/config.plist`. Section numbers point to the detailed entries.

### 3.1 Upstream boot-args

| Boot-arg | Effect | Default | Release | See |
|---|---|---|---|---|
| `-ngreentglfb` | Hook the TGL framebuffer kexts (com.xxxxx and com.apple identities) and skip the ICL FB | off | – | 2.A |
| `-ngreentglwithgfx` | TGL FB + TGL HW (accelerator) kexts. Also enables the Path E TCON ID patches and `cameliav=2` | off | yes | 2.A, 2.B |
| `-ngreentglgfx` | TGL HW kext only | off | – | 2.A |
| `-ngreenicl` | ICL FB + ICL HW fallback | off | – | 2.A |
| `-NGreenDebug` | Lilu DBGLOG output for the plugin | off | – | 2.C |
| `ngwegcoex=N` / `-ngwegcoex` | WhateverGreen coexistence: skips the CDCLK, PAVP/getGPUInfo and forceWake routes | off | – | 2.A, 2.C |
| `ngreenfullmtlcore` / `ngreenfullmtldyld` (both fall back to `ngreenfullmtl`) | Force the full-Metal path in the kernel / in the DYLD (CoreDisplay) patches | off | – | 2.A, 2.C |
| `ngreenforceprops=N` / `-ngreenforceprops` | Seed IGPU properties early (ig-platform-id `0x9A490000`, device-id `0x9A49`, …) | off | – | 2.C |
| `-nbdyldoff` | Disable all user-space (DYLD, `_cs_validate_page`) patches | patches on | – | 2.C |
| `-ngreenv189` | CoreDisplay: use the V189 AccessComplete jump instead of the V187 stub | off | yes | 2.C |
| `-ngreenV188htfind` | CoreDisplay V188 hash-find stub (debug; caused a black-screen regression) | off | – | 2.C |
| `ngreenSched=N` | Scheduler type: 3 = GuC, 4 = IGScheduler4, 5 = host-preemptive (IGScheduler5). Takes priority over the Info.plist `SchedulerType` | real TGL 3, spoofed 5 | **4** | 2.B (V44) |
| `ngreen-dmc=t\|a\|i` | DMC firmware choice in `hwInitializeCState`: `t` = TGL DMC v2.12, `a` = ADL-P DMC v2.16 plus the V100-V105 display power/PSR fixes, `i` = ICL passthrough with combo-PHY levels | skip (no blob) | **adlp** | 2.B |
| `ngreen-buf=N` | Number of V99G GGTT scanout slots (1-3) | 2 | – | 2.B |
| `ngreenV142=N`, `-ngreenV142{orig,pass,ok,hardunsupported,unsupported}` | submitBlit mode on spoofed hardware: 0 = unsupported, 1 = return 0 / no-op (V170 default), 2 = return 1, 3 = original Apple path | 1 | **3** | 2.A, 2.B |
| `ngreenV142diag2d` | Logs for the V186D 2D bypass | off | – | 2.B |
| `ngreenV120=N`, `-ngreenV120{pass,ok,fail}` | Return value of submitBlit when the task is NULL | 1 on spoofed, 0 on real TGL | – | 2.B |
| `ngreenV130=N`, `-ngreenV130{fail,pass,orig,hybrid,forceorig}`, `ngreenV130warmup=N` | barrierSubmission spoof mode (V130/V150) | mode 1, warmup 12 | – | 2.B, 2.C |
| `-ngreenV69{AllowOriginal,SkipOriginal,ForceOriginalUnsafe}` | Whether Apple's original `IGHardwareBlit3DContext::initialize` may run | original skipped on spoofed | – | 2.B |
| `ngreenexp` / `-ngreenexp` | Experimental monitors and logs (V137/V138/V143/V148/V149); also turns on V60 | off | – | 2.A, 2.C |
| `ngreenv60` / `ngreenv60off` (and the flag forms) | Turn the V60 GPU health monitor (and V45/V55 delayed checks) on or force it off | off | – | 2.C |
| `ngreenv60rw` | Allow the V60 monitor to write registers (V64, V76) | off | – | 2.C |
| `ngreenv88` | V88 scanout colour-band fill (needs exp + V60) | off | – | 2.C |
| `ngreenv93` | V93 guard against writing zero to PLANE_SURF | off | – | 2.A |
| `-ngreenv80l` | V80L plane-linearization enforcer (body commented out) | no effect | – | 2.A, 2.C |
| `ngreenV77DelayKill=N` | V77 delay before killing DisplayPipe clients (code commented out) | – | – | 2.A, 2.C |
| `ngreendp0` / `ngreendp1` (and the flag forms) | Force the display pipe off (dp0) or keep it native (dp1). The fork also uses dp0 for XE-DPCAPS | native on | – | 2.A, 2.B, 1.1 |
| `-ngreenbcsirq` | Include BCS in the V65 tier-1 IRQ enable bits | off (RCS only unless V142=3) | – | 2.A |
| `netdbg=IP` | kern_netdbg UDP logger target (module not called) | unused | – | 2.C |

### 3.2 Fork boot-args (`xe/*`)

| Boot-arg | Effect | Default | Release | See |
|---|---|---|---|---|
| `-ngreenkeepefi` | Keep the transcoder A timing/PPS that UEFI/GOP programmed | off | yes | 1.1 |
| `-ngreenxefix` | Master switch (see 1.0) | off | yes | 1.0 |
| `-ngreenfixbootmode` | Boot-programmable mode substitution and early IOFBDetailedTimings | off | yes | 1.1 |
| `-ngreenfixbootpatch` | Byte patch for the initial fCurrentMode | off | – | 1.1 |
| `-ngreenmax2560` | Reject modes wider than 2560 px | off | – | 1.1 |
| `-ngreenxtile` | Keep the X-tiled plane with the X-tile stride | off | – | 1.1 |
| `ngreenstride=N` | Override PLANE_STRIDE | none | – | 1.1 |
| `-ngreenv99g` | Re-enable V99R/V99G under xefix | off | – | 1.1 |
| `-ngreenv193` | Re-enable V193 under xefix | off | – | 1.4 |
| `-ngreennoblr`, `ngreenblmax=N` | Turn off backlight rescaling / set the duty value Apple treats as 100% | rescale on, N=0 (use Apple's period) | blmax=48000 | 1.1 |
| `ngreengtfreq=N` | GT frequency floor in MHz (0 = off, max 1500) | 1100 | – | 1.5 |
| `-ngreenengineinert` | Do not start the engines; drop engine MMIO writes | off | – | 1.2 |
| `-ngreennofwref`, `-ngreenfwhold` | Turn off forcewake ref-counting / never release forcewake at refcount 0 (hold) | ref on, hold off | – | 1.2 |
| `-ngreennofwsubmit` | No forcewake around submit | on | – | 1.2 |
| `-ngreennoelsqwait` | No bit-30 wait | on | – | 1.2 |
| `-ngreennoelsqidle`, `-ngreenidlearb`, `ngreenpreemptwait=N` | Idle wait before an RCS submit: off / only when arbitration is on / maximum µs | on, 5000 µs | – | 1.2 |
| `ngreenelsqdelay=N` | Fixed delay before an RCS submit (µs) | 50 | – | 1.2 |
| `-ngreenelsqretry` | ELSQ reload retry | off | – | 1.2 |
| `-ngreennosametail` | Turn off SAMETAIL force-restore | on | – | 1.2 |
| `-ngreennocsbguard` | Turn off the CSB event guard | on | – | 1.2 |
| `-ngreennoposhfix`, `-ngreenposhctx`, `-ngreennoposh` | POSH_LRCA zeroing off / keep POSH contexts / NOPOSH byte patch on | fix on, POSH cleared, NOPOSH off | – | 1.2 |
| `-ngreenevlog` | Flush the event/CSB rings to the kernel log | off | – | 1.7 |
| `-ngreennopoolfix` | Turn off the pool size and alignment fixes | on (with xefix) | – | 1.3 |
| `-ngreennoblitscratch` | Turn off the blit3d scratch initialization | on (with xefix) | – | 1.3 |
| `-ngreenorphanreuse` | Keep orphan-pool reuse | reuse off (with xefix) | – | 1.3 |
| `-ngreennoresfree` | Turn off the IGAccelResource::free guard | on | – | 1.3 |
| `-ngreennopaging`, `-ngreennopageonguard` | pageoff/pageon become no-ops / turn off the pageon guard | off / guard on | – | 1.3 |
| `-ngreendepthresolve` | Keep the original depth resolve | NODEPTH on | – | 1.4 |
| `-ngreenccs` | Keep AUX/CCS in surface state | NOAUX on | – | 1.4 |
| `-ngreentcf` | Keep Tile Cache Flush | NOTCF on | – | 1.4 |
| `-ngreenblit32` | Keep the SIMD32 blit PS | BLIT16 on | – | 1.4 |
| `-ngreenrpcs` | R_PWR_CLK_STATE patch | off | – | 1.4 |
| `-ngreenplanesxor`, `-ngreennoplanesfix` | Old xor variant / no planes patch | 0x98 fix on | – | 1.1 |
| `-ngreengva` | Keep the IOGVA* video keys | keys removed | – | 1.6 |

Non-plugin boot-args in the release config: `-allow3d` (purpose unclear from this source tree, not parsed by NootedGreen), `-disablegfxfirmware` (Apple: no GuC/HuC loading, which is why XE-GTFREQ is needed), `IGLogLevel=3` (Apple driver logging), `-vi2c-force-polling`, `msgbuf=400000`, `amfi_get_out_of_my_way=1`.

**Comments that do not match the code.** In each case the code is authoritative:
- the elsqretry comment says `-ngreennoelsqretry` (the code uses opt-in `-ngreenelsqretry`);
- the NOPAGE comment says `-ngreenpaging` (the code uses opt-in `-ngreennopaging`);
- the fwhold comment says `-ngreenfwrelease` (the code makes hold opt-in with `-ngreenfwhold`);
- the NOPOSH comment says `-ngreenposh` (the code uses opt-in `-ngreennoposh`);
- the PAGEONGUARD comment says `-ngreenpageonguard` (the code defaults on and uses `-ngreennopageonguard`);
- a V50 log string mentions `-ngreenNoMetal`, which is never parsed.

---

## 4. User-space history: Metal driver, libxedrv, LLVM bridge, scripts

Scope: everything outside the kernel (NootedGreen) that was needed to get Apple's Tiger Lake Metal driver
(`AppleIntelTGLGraphicsMTLDriver.bundle`, from Apple's Big Sur-era GPUDriversIntel 16.0.32 `AppleIntelTGLGraphics` stack) working on an Intel
Raptor Lake-P Iris Xe (8086:A7A0) under macOS Tahoe 26.7.1.

Sources: the build directories `build/mtl-patched*`, `build/xedrv*`, `build/llvm-bridge*`, `build/xe-new`, the
scripts in `nooted/tahoe/`, and binary diffs made for this document (script `mtldiff.py`: compares two x86_64 Mach-O
files up to the LC_CODE_SIGNATURE `dataoff`, so the re-signed signature blob is ignored; none of the binaries are fat).
Dates are file modification times (2026, local time) unless a comment says otherwise.

Notation: all offsets below are file offsets in the x86_64 Mach-O `Contents/MacOS/AppleIntelTGLGraphicsMTLDriver`. In
this binary the `__TEXT` segment starts at file offset 0, so file offset == unslid vmaddr. Symbols come from the
binary's own symbol table (`nm`).



### 4.1 Metal driver bundle patches (MTL v1 … v18)

Base: Apple's stock bundle (backed up as `backup-gpubundles/AppleIntelTGLGraphicsMTLDriver.bundle.original`,
5,311,296 bytes; MTL17 is 5,324,208 bytes — only because the ad-hoc signature is larger). Every version is installed
to `/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle` and ad-hoc re-signed (`codesign -f -s -`). The re-sign
always changes two codesign-managed header words, at 0x0a90 (`__LINKEDIT` filesize) and 0x107c (LC_CODE_SIGNATURE
datasize); these are not patches and are omitted below.

Each `build/mtl-patchedN` directory contains only the finished bundle (no README/notes); the "why" comes from the
`patch-mtl-v*.sh` / `instalar-mtl-*.sh` headers (v1–v7) and the project's session/memory notes (v8–v17), and every
byte change was confirmed by diffing consecutive builds.

| Ver. | Dir / date | What changed (vs previous) | Why / result |
|---|---|---|---|
| v1 | `mtl-patched`, 18 Oct 02:03 | Compiler-plugin path string at 0xa2e6b (103 bytes): `/System/Library/Extensions/AppleIntelGraphicsShared.bundle/Contents/MacOS/libMTLIGCCompilerPlugin.dylib` → `/Library/GPUBundles////////AppleIntelGraphicsShared.bundle/...` (same length, padded with `/`). Script `instalar-mtl-patch.sh`. | Tahoe ships no TGL-era `AppleIntelGraphicsShared.bundle`; point the driver at a copy of the old compiler bundle in `/Library/GPUBundles`. Result: AMFI library validation rejected the ad-hoc bundle; with AMFI off the old plugin failed to dlopen (needs libLLVM 3902); with the hybrid compiler, sandbox deny in MTLCompilerService. |
| v2 | `mtl-patched2`, 03:03 | Path → `/System/Library/Caches/xe//AppleIntelGraphicsShared.bundle/...` (bytes 0xa2e6c–0xa2e84). | `MTLCompilerService`'s sandbox only maps executables under `/System`; `/System/Library/Caches` is a writable firmlink it accepts. Sandbox OK, but "Compiler plugin … is not registered with the MTLCompilerObject". |
| v3 | `mtl-patched3`, 03:18 | Same path without the double slash, NUL-padded (102 of 103 bytes). Script `instalar-mtl-v3.sh`. | Hypothesis that `//` broke a string match — refuted; same "not registered" error. |
| v4 | `mtl-patched4`, 03:36 | Path → `/System/Library/PrivateFrameworks/../Caches/xe/Contents/MacOS/libMTLIGCCompilerPlugin.dylib` (`patch-mtl-v4.sh`, base v3). | `MTLCompilerPluginInterface::init` requires the path to *start with* `/System/Library/Extensions/`, `/System/Library/PrivateFrameworks/` or `/Library/GPUBundles/` (validPathPrefixes); `..` makes it resolve into the writable Caches dir. Must not contain `/System/Library/Extensions` (Tahoe's `getICBFile` would then treat it as a system plugin, ICL/KBL only). Still "not registered". |
| v5 | `mtl-patched5`, 03:53 | Path → `/System/Library/PrivateFrameworks/../Caches/xe.bundle/Contents/MacOS/libMTLIGCCompilerPlugin.dylib` (98 bytes + 5 NUL) (`patch-mtl-v5.sh`, base v3). | Second rule found in disassembly: the path must also *contain* `.framework` or `.bundle` (validPluginSuffixes). **Plugin registered.** Compiler moved to `/System/Library/Caches/xe.bundle/Contents`. This path string is unchanged in all later versions. |
| v6 | `mtl-patched6`, 11:21 | 13 displacement patches in `MTLIGRenderPipelineState` (`patch-mtl-v6.sh`, base v5): `generateSampleMask:` 0x7e30 `a8→b8` (sampleCount), 0x7ebc `b8→c8` (sampleCoverage), 0x7f11 `b0→c0` (sampleMask); `generate3DStatePS:` 0x8025 `f800→0801` (fragmentFunction 0xf8→0x108), 0x80f4 `a8→b8`, 0x850d `d0→e0` (misc bits); `generate3DStateWM:` 0x887d `d0→e0`, 0x8892 `f800→0801`, 0x88ae `d0→e0`; `generate3DStateSBE:` 0x9952 `d0→e0`; `generate3DStateVF:posh:` 0xac37/0xac62 `00→10` (vertexDescriptor 0x100→0x110); `generate3DStateClip:` 0xbd4e `d0→e0`. (The script's own labels attribute 0x8022–0x850b to generateSampleMask; the symbol table puts them in `generate3DStatePS:`.) | Tahoe's private `MTLRenderPipelineDescriptorPrivate` gained `shaderValidation`/`shaderValidationState`, shifting fields ≥0xa0 by +0x10; the old driver read stale offsets ("`-[_MTLFunctionInternal layouts]: unrecognized selector`"). Superseded by runtime conversion in libxedrv (v7). |
| v7 | `mtl-patched7`, script 11:57, binary 13:30 | v6 patches **reverted** (back to v5 code). New `LC_LOAD_DYLIB` (cmd 0xC, cmdsize 0x70, compat/current 1.0.0) appended in free header space at 0x1080 for `/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/libxedrv.dylib`; header ncmds 26→27, sizeofcmds +0x70 (bytes 0x10–0x14). `libxedrv.dylib` (v7, from `build/xe-new`) copied into the bundle. | Structure conversion done at run time by libxedrv (see 4.2). The lib must live inside the GPU bundle because WindowServer's sandbox only maps executables from `/Library/GPUBundles`. Driver loaded; WindowServer still crashed/froze. |
| v8 | `mtl-patched8`, 16:31 | Driver binary identical to v7; only `libxedrv.dylib` replaced by libxedrv v8. | See 4.2 (layout fix 0xa8, synchronous install, lab mode). |
| v9 | `mtl-patched9`, 17:24 | Binary identical; libxedrv v9. | Crash recorder in WindowServer; found the POSH crash. |
| v10 | `mtl-patched10`, 17:34 | Binary and lib identical to v9; `Contents/Info.plist` `Development.PoshDisable` 0 → 1. | WindowServer SIGSEGV in `Hash32::addInput` ← `-[MTLIGRenderPipelineState generateSamplerDeclHash:]` on the POSH (position-only shader) path: the device's `bPoshDisable` comes from this plist key, but the compiler plugin runs with POSH off, so the driver read past the shader output. Crash gone after v10. |
| v11 | `mtl-patched11`, 22:17 | Byte `0x88 → 0xd0` at 0x65f12, 0x7ff77, 0x80253 (intended: L3 allocation register value `0x88000020 → 0xd0000020`, the RPL-P reset default). | Hunting a render-engine hang whose batch had `LRI 0xb134 (L3ALLOC) = 0x88000020`. Result: hang unchanged — L3 config not the cause. **Caveat found while writing this:** only 0x65f12 is really an immediate (`movl $0x88000020, 0x14(%rcx)` at 0x65f0c in `IGAccelRenderCommandEncoder::writeInitializationHWState`). At 0x7ff77 and 0x80253 (in `IntelState::IntelState()`) the 4 bytes `20 00 00 88` are the tail of `movq $0x2000, …` followed by the opcode of the next instruction, so the patch turned `movb %al, 0x110(%rdi)` into `rolb 0x110(%rdi)` and `movb %cl, 0x8e0(%rbx)` into `rorb 0x8e0(%rbx)` (those byte fields are no longer initialized). These two edits persisted through MTL17; MTL18 reverts them (see 6.1). |
| v12 | `mtl-patched12`, 23:15 | Binary identical to v11; libxedrv v12. | Attempt to disable render compression from user space; no effect (see 4.2). |
| v13 | `mtl-patched13`, 19 Oct 12:36 | `-[MTLIGAccelTexture generateClearRequirements:]`: 0x1c7c0 `0f 95 c2` (setne %dl) → `90 90 90`; 0x1c7cf `40 0f 95 c6` (setne %sil) → `90 90 90 90`. edx/esi are pre-zeroed, so bits 11 and 12 of the clear-requirements word (set when `m_Info+0x160`/`+0x164`, the AUX/CCS info, is non-zero) are always 0. | Disable render-target **fast clear**. The hang moved to an ordinary clear. |
| v14 | `mtl-patched14`, 12:48 | `IGRenderClearEncoder::writePSState(bool,bool,bool,bool)`: 0x49cfe `0f 95 c0` (setne %al) → NOPs; eax pre-zeroed, so bit 5 of the PS state qword is always clear. | Disable dual-SIMD8 dispatch for the clear PS. No effect. |
| v15 | `mtl-patched15`, 13:19 | `-[MTLIGAccelTexture initWithDevice:descriptor:iosurface:plane:field:args:argsSize:]`: 0x18c28 `8b 48 18` (`movl 0x18(%rax), %ecx`) → `31 c9 90` (`xorl %ecx,%ecx; nop`), so the texture field at +0x160 is written as 0 instead of the kernel-supplied IOSurface compression value. | Ignore kernel IOSurface compression for IOSurface-backed textures. First login to the desktop; compression later shown not to be the hang's cause, patch kept as harmless. |
| v16 | `mtl-patched16`, 13:42 | `IGAccelRenderCommandEncoder::programClearPipeline(IGRenderClearEncoder&, sClearPass&)`: 0x67c27 `74 5c` (je 0x67c85) → `90 90`. | The `je` after `testb $0x60, 0x30(%rax)` skipped emitting the 36-byte VS-disabled `3DSTATE_VS` (`g_cInitGfx3dStateVs`). Clear passes inherited the previous draw's VS (seen as INSTDONE VS-not-done), which hung the RCS when another process had drawn last. Result: zero RCS hangs, but windows empty. |
| v17 | `mtl-patched17`, 13:53 | Same function: 0x67c5c `75 16` (jne 0x67c74) → `eb 16` (jmp). | Always take the branch that ORs bit 0x20 into the VS state-tracker byte (`[*(+0x11b70)]+0x30`) and its mask into the dirty word, so the next real draw re-emits its own VS. Windows show content. **This is the installed/release build** (MD5 `50bfb18b1366f08dc65465d12f4d37b4` per the session notes; libxedrv later swapped for v13–v15 in place). |
| v18 | 20 Oct (after the first commit) | MTL17 with 0x7ff77 and 0x80253 restored to `0x88`; only the real L3 immediate at 0x65f12 keeps `0xd0`. The repository's `macos/mtl17-patches.json` no longer contains those two entries, so `macos/patch-mtl17.py` now produces MTL18 (the script and file keep the MTL17 name). | Undo the instruction damage found in the audit (6.1). **Being tested**; effect on the open problems not yet known. |

Cumulative MTL17 content changes vs stock (12 regions): 0x10 (ncmds/sizeofcmds), 0x1080 (LC_LOAD_DYLIB libxedrv),
0x18c28, 0x1c7c0, 0x1c7cf, 0x49cfe, 0x65f12, 0x67c27, 0x67c5c, 0x7ff77, 0x80253, 0xa2e7b (plugin path); plus
`Info.plist` `Development.PoshDisable=1` and the bundled `libxedrv.dylib`. MTL18 = the same minus 0x7ff77 and 0x80253
(10 regions), which is what `macos/mtl17-patches.json` now contains.

Bundled libxedrv per bundle (by SHA-1 prefix): v7 `e2aabb85` (=`build/xe-new`), v8 `05d44dc3`, v9–v11 `1daa3bc5`,
v12–v17 `94ad1491` (= libxedrv v12). libxedrv v13/v14/v15 were installed into the live bundle by their own scripts
(`build/xedrv1N/instalar-xedrv1N.sh`), not as new `mtl-patched` builds.

### 4.2 libxedrv (runtime adapter injected into the Metal driver)

**What it is.** An Objective-C dylib (`libxedrv.dylib`, x86_64) that adapts Apple's TGL Metal driver 16.0.32 — built
against the Big Sur–era private Metal ABI — to Tahoe's Metal.framework (25G241). It never touches the GPU directly; it
patches the Objective-C runtime of the process that loads the driver (WindowServer, SecurityAgent, every Metal app).

**How it is injected.** MTL v7+ adds an `LC_LOAD_DYLIB` for
`/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/libxedrv.dylib` to the driver binary (see
4.1). The dylib sits inside the GPU bundle because WindowServer's sandbox only maps executables from
`/Library/GPUBundles`. Its `__attribute__((constructor))` runs when Metal loads the driver bundle. Since v13 the lib is
swapped in place with `build/xedrv1N/instalar-xedrv1N.sh` (copy, `chown root:wheel`, ad-hoc sign the dylib, then
re-sign the bundle, print MD5; argument `desfazer` restores the saved v12 lib `libxedrv.v12-instalada.dylib`; reboot
needed because WindowServer only loads it at boot/login). The release copy `macos/src/libxedrv.m` is byte-identical to
`xedrv15.m`.

Core mechanisms (present from v7/v8 on):
- **Descriptor shadowing.** Swizzles `-_descriptorPrivate` on `MTLRenderPipelineDescriptorInternal` and
  `MTLRenderPipelineColorAttachmentDescriptorInternal`. When the caller's return address lies in the TGL driver
  (`dladdr`), it returns a *copy* in the old layout from a per-thread ring of 256 buffers; Metal itself keeps seeing
  the real struct. Pipeline struct (0x2e0 bytes): v8 copies 0x00–0xa8 unchanged, old 0xa8–0x108 ← new 0xb8–0x118
  (skipping `shaderValidationState`/`textureWriteRoundingMode`), old 0x108 ← new 0x198 (0x20 bytes: vertex/fragment
  buffers, driverCompilerOptions, gpuCompilerSPIOptions), old 0x128 ← new 0x1c0 (0x20 bytes). The "misc" bitfield
  (0xe0 new / 0xd0 old) is remapped field by field via a 21-entry {old pos, old width, new pos, new width} table; the
  color-attachment bitfield is remapped (v7: raw bit moves 2..27 → 1..26, writeMask 32..35 → 27..30, pixelFormat
  42.. → 36..; v8+: rebuilt from the public properties — blendingEnabled bit 0, rgb/alpha blend op at 1/4, source
  rgb/alpha factor at 7/12, destination rgb/alpha factor at 17/22, writeMask at 27, new bits 33..37 → 31..35,
  pixelFormat at 36).
- **API gaps.** `class_addMethod` for selectors Tahoe's Metal calls but the old driver lacks: the `…resourceIndex:`
  variants of texture-view creation on `MTLIGAccelTexture` (forwarded to the legacy selectors when the index is
  `_MTLInvalidResourceIndex`, else rejected), `getGlobalMemoryBuffer(…)`, `getGlobalMemoryBufferHeader`,
  `initializeComputeIndirectCommandBuffers` (stubs that log and return nil), `newBufferWithDescriptor:` (mapped to
  `newBufferWithBytes/Length:options:` for plain options, otherwise nil), `generateIndirectionIndex:resourceIndex:`.
  Capability getters forced to NO: `supportsDynamicAttributeStride`, `supportsCMPIndirectCommandBuffers`,
  `supportsMeshShaders(InICB)`, `supportsBinaryArchives`, three `supportsAIRNTBinaryArchive…`; and
  `newBinaryArchiveWithDescriptor:error:` returns an NSFeatureUnsupportedError.
- **`-[MTLCompiler releaseHashMaps:]` workaround.** It crashed on an invalid value in the hash→library map; the hook logs
  the nodes (reading them safely with `mach_vm_read_overwrite`) and does not free them (deliberate leak).

| Ver. | Source / date | What changed | Why / result |
|---|---|---|---|
| v7 | `build/llvm-bridge/xedrv.m` (binary in `build/xe-new/Contents/MacOS`), 18 Oct 13:30 | First version: descriptor shadowing (shift applied from 0xa0), API gaps, capability overrides, releaseHashMaps leak. Hooks installed from background threads with polling. Log prefix `xedrv:`; env `XEDRV_TEST` treats every caller as the driver. | Replaces the static MTL v6 offset patches with run-time conversion that also covers the bitfields. Driver loaded; WindowServer still crashed. |
| v8 | `build/xedrv8/xedrv8.m`, 16:30 | (1) shift starts at **0xa8** (the old driver also has `shaderValidation` at 0xa0; v7 put `textureWriteRoundingMode` there) — old layout derived from the driver's own type encoding (0x160 bytes); (2) **synchronous** install in the constructor (late-install thread only as fallback); (3) rejects dynamic libraries (`supportsDynamicLibraries` was inherited as YES); (4) layout preflight (class size, `_private` ivar offset, encoding names, dynamic probe of rasterSampleCount at +0xb8 and vertexDescriptor at +0x110) — conversion disabled if it fails; (5) **lab mode**: if `/Library/GPUBundles/xe-lab` exists, WindowServer gets no MTLDevice (software rendering) so test tools can use the GPU without rebooting. Log `xedrv8:`. | Modelled on a third-party IntelIGPU-Tahoe adapter. releaseHashMaps map became valid; lab mode unusable (WindowServer aborts when the accelerator exists but returns no device). |
| v9 | `build/xedrv9/xedrv9.m`, 17:23 | v8 + crash recorder: SIGSEGV/SIGBUS/SIGILL/SIGABRT handler logs fault address, registers, symbolized rip and the rbp-chain backtrace (image+offset+symbol), then restores default handling. Log `xedrv9:`. | Found the POSH crash (`Hash32::addInput` ← `generateSamplerDeclHash:`), fixed by MTL v10. |
| v10, v11 | — | No libxedrv v10/v11 exist; MTL v10/v11 still bundle lib v9. | — |
| v12 | `build/xedrv12/xedrv12.m`, 23:15 (header says 19/10) | Swizzles `MTLIGAccelDevice newTextureWithDescriptor:`, `newTextureWithDescriptor:iosurface:plane:`, `heapTextureSizeAndAlignWithDescriptor:` and `MTLIGAccelHeap newTextureWithDescriptor:(atOffset:)` to pass a descriptor copy with `allowGPUOptimizedContents = NO`. Env `XE_COMPRESSAO` disables. Log `xedrv12:`. | Avoid lossless compression (fast clear / compressed stencil needing AUX-TT). Result: no effect — the TGL driver reads the descriptor via its private struct, not this property. Kept (harmless) in later versions. |
| v13 | `build/xedrv13/xedrv13.m`, 20 Oct 16:17 | Logging also to `/tmp/xedrv-<process>.log` (unprivileged `log show` did not work); texture-creation statistics per (path, Metal format, IOSurface format, ok/nil), first occurrences of each combination and every nil. Late-install retry loop also waits for the texture hooks. New `LOG` → `xe_log()`. | Instrumentation for the "missing icons" investigation. |
| v14 | `build/xedrv14/xedrv14.m`, 16:25 | Pipeline-creation logger on `_MTLDevice`: `newRenderPipelineStateWithDescriptor:error:`, `…:options:reflection:error:`, `…:completionHandler:`, `…:options:completionHandler:`, `newComputePipelineStateWithFunction:error:` — all failures with compiler error, sample of successes. Build flag `SO_PIPELOG` installs only this logger. | Showed WindowServer created 11 pipelines with 0 failures → shader compilation not the culprit. |
| v15 | `build/xedrv15/xedrv15.m`, 16:37 | **NOCOPY fix**: hooks `MTLIOAccelDevice newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:` and `…length:deallocator:descriptor:offset:bytesPerRow:`. When `offset & 255 != 0` (single-level 2D), creates a driver-owned managed texture and copies the pixels; the caller's deallocator is attached via an associated object (`XeDeallocV15`) and runs when our texture is freed. Env `XE_NOCOPY_ZERO=1` disables. | CoreAnimation wraps image memory zero-copy at offset 0x20 (image header; also 0x4c0, 0xda0); the TGL driver on RPL does not sample linear textures at such offsets → invisible layers (empty Finder icons, black DMG backgrounds). Finder icons appeared. **Current release version.** |

Binary sizes: v7 42,112 B; v8 47,376; v9 47,648; v12 52,336; v13 52,944; v14 58,016; v15 59,280.

### 4.3 Shader-compiler path: LLVM bridge (`build/llvm-bridge`, `build/llvm-bridge2`)

**Problem.** Tahoe's `MTLCompilerService` loads the vendor compiler plugin named by the driver (MTL v5 path:
`/System/Library/Caches/xe.bundle/Contents/MacOS/libMTLIGCCompilerPlugin.dylib`). The TGL-era plugin
(`libMTLIGCCompilerPlugin` + `libigc`) was linked against Apple's LLVM "3902" and exports only
`MTLCompilerBuildRequestWithOptions(compiler, input, rdx, flags, llvm::Module*, …)`, while Tahoe ships libLLVM 32023
(`GPUCompiler.framework/Versions/32023/Libraries/libLLVM.dylib`) and calls
`MTLCompilerBuildRequestWithSerializedBitcode(compiler, input, rdx, flags, bitcode, size, …)`. Tahoe's own hybrid
plugin failed with an `m_pReadBitcodeFn` assert.

**Resulting `xe.bundle/Contents/MacOS` layout** (assembled in `build/xe-new`, 18 Oct; installed by
`instalar-ponte-llvm2.sh`, later by `instalar-mtl-v7.sh`):
- `libMTLIGCCompilerPlugin.dylib` — the **adapter** (`llvm-bridge/adapter.cpp`, 18 Oct 13:21): exports the Tahoe
  entry point, parses the serialized bitcode into a module with libLLVM 32023, and calls the old plugin's
  `BuildRequestWithOptions` with remapped arguments (old r9 = new stack[0x10]; old stack[0x10..0x38] = new
  [0x18..0x40], confirmed in both disassemblies). It contains libigc crashes (a fatal signal during compilation is
  turned into error 1 with a logged backtrace — "FALHA CONTIDA"), can dump bitcode to syslog (`xe-dump`), wraps
  `MTLCompilerCreate` and installs a helper object via `MTLCompilerSetHelper` (Tahoe never calls it, leaving
  compiler+0x10 as heap garbage; the helper's provider returns a null ICB builtin module — correct for shaders that do
  not use ICBs). It LC_REEXPORTs the old plugin. Logs `xe-adapter:`.
- `libMTLIGCCompilerPluginOld.dylib` — the stock TGL plugin with its install name changed and its libLLVM dependency
  pointed at `libLLVMbridge.dylib` (compat 1.0.0, current 3902.30.1).
- `libLLVMbridge.dylib` — the **bridge** (`bridge.cpp`): `LC_REEXPORT_DYLIB` of Tahoe's libLLVM 32023 (linked through
  the generated stub `libLLVM32023.tbd`, from `exports.txt`), plus implementations of the 3902-ABI symbols the old
  plugin imports but 32023 no longer provides or changed:
  `llvm::WriteBitcodeToFile(const Module*, raw_ostream&, …)` (writes bitcode straight into the old
  `raw_string_ostream`'s `std::string*` at +0x28; prefers calling MTLCompiler's internal `getSerializedModule(Module*,
  unsigned)` to emit "MTL bitcode version 3902" that LLVM-7-era IGC can read, else falls back to the LLVM C API),
  `raw_string_ostream::~raw_string_ostream()` D1 (no-op for the old layout),
  `parseIRFile(StringRef, SMDiagnostic&, LLVMContext&, bool)` (forwards to the new signature with a null
  data-layout callback), and `Module::getNamedMetadata(const Twine&)` / `Value::setName(const Twine&)` (the old
  inline Twine layout — LHS@0, RHS@8, kinds@0x10/0x11 — is flattened to a std::string and passed as a new
  StdString Twine). Logs `xe-bridge:`.
- `libigc.dylib`, `libigcHSW.dylib`, `libIGIL-Metal.dylib`, `libISAProfilerDyn.dylib` — stock TGL-era libraries, re-signed.
- `libxedrv.dylib` — v7 copy (see 4.2).

**bridge v1 vs v2.** v1 (`llvm-bridge/libLLVMbridge.dylib`, 18 Oct 13:43) located `getSerializedModule` at the fixed
offset 0x7FB06 in the loaded `…/MTLCompiler` image (validated by the `55 48 89 e5` prologue). Inside
MTLCompilerService the prologue check failed (`03 00 eb 08`), so every module went through the C-API fallback
(modern bitcode) and some CoreAnimation shaders crashed libigc. **v2** (`llvm-bridge2/bridge.cpp`, 18 Oct 17:55)
scans the `__text` of every loaded `…/MTLCompiler` for the function signature (`push rbp; mov rbp,rsp; …
cmp edx,0xf3d (81 fa 3d 0f 00 00) … cmp edx,0xf3e (81 fa 3e 0f 00 00)`) and accepts it only if exactly one candidate
exists; otherwise falls back to the C API. Result: every shader compiled.

**Probe programs in `build/llvm-bridge`** (18 Oct, all small one-off checks): `chk.c` (are the 3 new-ABI symbols in
libLLVM 32023?), `holder.c`/`holder2.c` (dlopen libLLVM and print raw_ostream / getNamedMetadata addresses, then sleep
so the process can be inspected), `ls.c` (list loaded libLLVM images), `t.c` (dlsym arbitrary symbols), `teste.c`
(dlopen a plugin and find `MTLCompilerCreate`), `twtest.c` (Twine/metadata ABI test through the C API), `mc.c`
(dlsym `MTLCompilerPluginInterface::compilerBuildRequest` in MTLCompiler), `mh.m` (list `MTLCompiler` methods such as
`releaseHashMaps:`), `mlist.m` (method lists of `_MTLFunctionInternal` and friends), `ivl.m` (dump ivar offsets/types of
given classes), `pd.m` (dump a real `_descriptorPrivate` to measure the Tahoe layout), `xedrv.m` (libxedrv v7 source),
`adapter.cpp.bak1` (earlier adapter draft).

### 4.4 Scripts in `nooted/tahoe/` (one line each)

All are zsh unless noted; most require `sudo`. "EFI" = the OpenCore partition mounted at `/Volumes/OPENCORE`.

**Metal driver / compiler (user space)**
- `instalar-compilador-hibrido.sh` (18 Oct 02:33) — builds a "hybrid" `AppleIntelGraphicsShared.bundle` in `/Library/GPUBundles`: Tahoe's compiler plugin + TGL-era `libigc`/`runtime.igc64.split` + TGL `runtime.gpuencode.split` (from `build/shared-hybrid`), backing up the original.
- `restaurar-compilador.sh` (02:33) — restores the original `AppleIntelGraphicsShared.bundle` in `/Library/GPUBundles`.
- `instalar-mtl-patch.sh` (02:03) — installs MTL v1 (plugin path → `/Library/GPUBundles////////…`), backing up the stock bundle first.
- `restaurar-mtl.sh` (02:03) — reinstalls the stock (unpatched) Metal bundle from the backup.
- `instalar-compilador-caches.sh` (03:03) — copies the hybrid compiler into `/System/Library/Caches/xe` (writable firmlink the MTLCompilerService sandbox accepts) and installs MTL v2.
- `restaurar-compilador-caches.sh` (03:03) — removes `/System/Library/Caches/xe` and reinstalls MTL v1.
- `instalar-mtl-v3.sh` (03:18) — installs MTL v3 (clean NUL-padded Caches path).
- `patch-mtl-v4.sh` / `instalar-mtl-v4.sh` (03:35) — builds MTL v4 (`PrivateFrameworks/../Caches/xe/Contents/…` path) and installs it plus a compiler copy in `/System/Library/Caches/xe/Contents`.
- `patch-mtl-v5.sh` / `instalar-mtl-v5.sh` (03:53) — builds/installs MTL v5 (`…/Caches/xe.bundle/…`, satisfies both the prefix and the `.bundle` rules) with the hybrid compiler in `/System/Library/Caches/xe.bundle`; also the generic "undo" target of later scripts.
- `patch-mtl-v6.sh` / `instalar-mtl-v6.sh` (11:21) — builds/installs MTL v6 (13 struct-offset displacement patches).
- `instalar-ponte-llvm.sh` (18 Oct 09:30) — installs `build/xe-old/Contents` into `xe.bundle`: old TGL plugin + old libigc + `libLLVMbridge` (bridge v1, no adapter).
- `instalar-ponte-llvm2.sh` (10:16) — installs `build/xe-new/Contents`: adapter plugin + `libMTLIGCCompilerPluginOld` + bridge.
- `patch-mtl-v7.sh` (11:57) — builds MTL v7: v5 + `LC_LOAD_DYLIB` for `libxedrv.dylib` (copied into the bundle).
- `instalar-mtl-v7.sh` (11:36) — installs `xe-new` (compiler + bridge + libxedrv) into `xe.bundle` and MTL v7.
- `sair-do-lab.sh` (16:32) — leaves "lab mode": restores the MTL v7 driver binary/libxedrv/CodeResources, deletes `/Library/GPUBundles/xe-lab`, and puts the EFI in the stable display-only state; works from Tahoe or from the Sequoia host (via the external volume). (It refers to a `preparar-lab.sh` that is not in this directory.)

**Accelerator kext on/off**
- `ativar-acelerador.sh` (17 Oct 20:09) — enables `AppleIntelTGLGraphics.kext` by setting personality `Gen7.IOPCIPrimaryMatch` to `0x9a498086` (the TGL ID NootedGreen spoofs) and bumping `CFBundleVersion` to 16.0.2 so kmutil re-registers it in the Auxiliary KC.
- `desativar-acelerador.sh` (20:09) — neutralizes it (match `0x12348086`, version 16.0.3). Its header comment is a copy of the enable script's.

**Whole-stack install / removal**
- `preparar-tahoe.sh` (16 Oct 19:30) — first Tahoe test: copies the TGL 16.0.32 kexts/bundles + HookCase from `nooted/stage/LE` to `/Library/Extensions`, neutralizes the accelerator (`0x12348086`, 16.0.1), installs the boot-diagnostic LaunchDaemon, registers in the Auxiliary KC.
- `remover-tahoe.sh` (19:30) — undoes it: removes the TGL items and bootdiag daemon, and restores the EFI from `BACKUP-EFI-ANTES-NG-TAHOE` (works in Safe Mode).
- `reinstalar-stack.sh` (20 Oct 14:09) — re-applies the full working stack on a clean Tahoe 26.7.1 from the external Tahoe volume: TGL kexts (accelerator enabled), MTL17 + hybrid shared bundle in `/Library/GPUBundles`, `xe.bundle` (old plugin + bridge v2 + TGL libigc), XeShim, `xe-bootdiag`/`xe-kext` + LaunchDaemon; checks MTL17's MD5 (`50bfb18b…`) and registers the Auxiliary KC.

**EFI / OpenCore variants** (each copies a NootedGreen build into `EFI/OC/Kexts` + a `config.teste-*.plist`, after checking the EFI backup exists)
- `aplicar-efi.sh` (16 Oct 19:31) — base Tahoe test EFI (`build/NootedGreen.kext` + `config.teste-tahoe.plist`).
- `aplicar-xtile-efi.sh` (17 Oct 00:52) — `NootedGreen-xtile.kext` + `-ngreenxtile` config.
- `aplicar-v99g-efi.sh` (01:07) — V99R/V99G kext build with the normal test config.
- `aplicar-dmcskip-efi.sh` (09:31) — V99G + `ngreen-dmc=skip` (no DMC firmware loaded).
- `aplicar-dmctgl-efi.sh` (09:54) — V99G + `ngreen-dmc=tgl` (TGL DMC v2.12 instead of ADL-P v2.16).
- `aplicar-max2560-efi.sh` (10:43) — kext with the mode hook `-ngreenmax2560`.
- `aplicar-stride-efi.sh` (11:38) — `-ngreenstride=0x1e` (plane stride/band-width test).
- `estado-estavel-so-tela.sh` (18 Oct 14:44) — EFI to the stable "display only" state (kext `antes-bootmode-2259` + `config.plist.soTela-2131` from the EFI backups).
- `estado-experimento-acelerador.sh` (15:24) — EFI to the accelerator experiment (`NootedGreen-xefix.kext`, `IGLogLevel=3`, boot-args incl. `-ngreentglwithgfx -allow3d -disablegfxfirmware ngreen-dmc=adlp -ngreenv189 -ngreenkeepefi -ngreenxefix -ngreenfixbootmode amfi_get_out_of_my_way=1`).
- `restaurar-efi-antes-inert.sh` (14:23) — restores kext+config from before the `-ngreenengineinert` test.
- `restaurar-efi-nosso-kext.sh` (14:07) — restores our kext+config after an A/B test with another author's (Reddit) NootedGreen build.
- `config.teste-{tahoe,xtile,dmcskip,dmctgl,max2560,stride}.plist` — the OpenCore configs used by the scripts above.
- `make_test_config.py` (16 Oct 19:31, Python) — builds the Tahoe test config from the current config plus the iGPU DeviceProperties and NootedGreen entry of an older test config, sets debug boot-args and `PollAppleHotKeys`.

**Diagnostics**
- `bootdiag-tahoe.sh` (17 Oct 10:12; installed as `/usr/local/sbin/xe-bootdiag`, run by LaunchDaemon `local.xe.bootdiag`) — 150 s after boot saves kernel log, kext list, ioreg of framebuffer/accelerator/IGPU/XeShim, and display/GGTT/PSR/underrun/DBUF/CDCLK/DPLL/eDP-link/scaler registers via XeShim (`xectl`); can blink the backlight as a signal and capture the first 128 MB of the aperture. `xe-bootdiag.antes-*` (17–18 Oct) are earlier snapshots of the installed copy (before the buddy, wide-dump, fences, interactive, live and metalprobe changes).
- `testar-tiling.sh` (16 Oct 23:32) — live test of plane 1A tiling: writes PLANE_CTL (0x70180) tiling bits 12:10 and PLANE_STRIDE (0x70188) for linear/X/Y via `xectl`, phase by phase, then restores.

**Kernel patch**
- `nootedgreen-tahoe-keepefi.patch` (16 Oct 23:06) — NootedGreen source patch: (1) linear PLANE_STRIDE computed from the real pipe width (`PIPE_SRCSZ`) instead of the hard-coded 0xa0 (2560 px; 3840 px needs 0xf0); (2) `-ngreenkeepefi`: when transcoder A is already enabled (0x70008 bit 31), keep the GOP-programmed eDP timings/M-N/PPS instead of writing the original author's 2560×1600 2-lane panel register dump; (3) adds `KernelVersion::Tahoe` to the supported kernels in `kern_start.cpp`.


---

## 5. Diagnostic and test tools


### 5.1 `tools/`

**Build scripts (no Xcode, Command Line Tools only)**
- `build-kext.sh` (15 Oct) — builds a single-file C++ kext `<Name>` from `<Name>/<Name>.cpp` + Info.plist into `build/<Name>.kext`.
- `build-lilu-plugin.sh` (15 Oct) — builds a Lilu plugin kext `<Name>` against MacKernelSDK + Lilu.
- `build-nootedgreen.sh` (15 Oct) — builds NootedGreen.kext from `ext/NootedGreen`, mirroring the Xcode Debug settings.
- `build-nootedgreen-bigsur.sh` (16 Oct) — same, Big Sur variant (Lilu min kernel BigSur, minos 11.0).
- `build-xtile.sh` (17 Oct) — builds the `-ngreenxtile` variant from `ext/xtile` → `build/NootedGreen-xtile.kext`.
- `build-xefix.sh` (17 Oct) — builds the `-ngreenxefix` variant from `ext/ng-xefix` → `build/NootedGreen-xefix.kext` (the line of kernel work that produced the accelerator fixes).
- `build-hookcase.sh` (15 Oct) — builds NootedGreen's modified HookCase.kext (symbol provider for the TGL kexts) with a probePortMode NULL-callback fix and without its IOKit personality.
- `build-gentool.sh` (15 Oct) — builds Mesa's standalone Intel EU assembler/disassembler without meson.
- `bison/` — a local bison/yacc install (bin, lib, share), presumably for building Mesa's GLSL front-end; purpose not documented.

**Boot / system plumbing**
- `setup-root.sh` (15 Oct) — one-time: installs `xe-kext` to `/usr/local/sbin` and a sudoers rule allowing it without password.
- `xe-kext` (15 Oct) — root helper: load/unload/reload only `local.xe.<Name>` kexts staged in `/Library/Xe`.
- `xe-boot-arm.sh` + `local.xe.arm.plist` (15 Oct) — early-boot LaunchDaemon (pre-Tahoe-accelerator era): loads XeShim and XeFB and hands the display to XeFB (`xectl fb 1`).
- `instalar-mountefi.sh` + `local.xe.mountefi.plist` (20 Oct) — LaunchDaemon that mounts the internal OpenCore EFI partition by partition UUID at every boot.
- `handoff-sequoia.sh` (17 Oct) — copies the assistant session transcript + a memory note to the Sequoia volume so work can resume there.
- `abrir-claude.command` (19 Oct) / `abrir-claude.command.antes` (18 Oct) — login item that resumes the coding-assistant session (current version uses accept-edits permission mode; the older one skipped permissions).
- `xe-teste.command` (17 Oct) — interactive display test at login: for each configuration blinks the backlight and asks y/p/n (good/partial/bad), logging to `logs/teste-interativo-*`.

**Reverse-engineering / analysis helpers**
- `ghserve.py` (15 Oct) — imports a binary into a persistent Ghidra project and serves it to GhidraMCP on a fixed port.
- `hwcmd_dump.py` (15 Oct) — summarizes Intel media-driver `*_hwcmd_*.h` structs (DW labels, bit ranges, opcodes).
- `xe-decode.py` (18 Oct) — decodes a Gen12 batch (`batch.hex` from xe-rcsdump, or dwords) using Mesa genxml (gen12 + imports); options `--de/--ate VA`, `--so CMD,…`.
- `xe-panic.py` (20 Oct) — symbolizes panic-report backtrace frames that fall in AppleIntelTGLGraphics 16.0.32 and IOAcceleratorFamily2.
- `mtl-diff.py` (20 Oct) — lists differences between two x86_64 Mach-O files outside LC_CODE_SIGNATURE (used to derive the MTL17 patch list).
- `dispregs.txt` (17 Oct) — list of 1,043 display MMIO register offsets (from 0x60000) to dump.
- `igc-shim/exp_tahoe.txt`, `exp_tgl.txt` (18 Oct) — exported C++ RTTI/exception symbols of Tahoe's vs TGL's IGC libraries, compared while trying to run the old libigc on Tahoe.

**Display / framebuffer diagnostics**
- `raw2png.py` (16 Oct) — converts a raw 32-bit framebuffer dump (8888 or 2101010, linear or X-tiled) to a downscaled PNG.
- `raw-xtile.py` (17 Oct) — decodes a raw (unfenced) X-tiled framebuffer dump to PNG.
- `xe-crc-teste.sh` (17 Oct) — automatic plane-1A CRC test (PIPE_CRC_CTL_A 0x60050, result 0x60064) on a deterministic pattern: measure on Tahoe, reference on Sequoia/GOP.
- `xe-res.c` (19 Oct) — lists display modes (including scaled HiDPI) and switches/saves a mode from the command line.
- `xe-perfil.m` (20 Oct) — shows/sets the main display's ColorSync profile (`padrao` = factory/EDID).

**GPU engine monitoring**
- `xe-rcsdump.c` (19 Oct) — read-only sampler of GPU engine state through XeShim once per second in a single process, fsync per line; on hangs dumps batch pages, ring, PTWALKs of the PPGTT, LRC scan, fuses; optional PSMI re-apply and burst mode via env vars.

**Metal bring-up and conformance tests (user space, Tahoe)**
- `metalprobe.m` (18 Oct) — tries to create the Metal device outside WindowServer and prints what the framework reports.
- `metalcompile.m` (18 Oct) — compiles a small compute + vertex/fragment library from source to exercise the shader compiler path.
- `xe-mtltest.m` (18 Oct) — staged Metal test outside WindowServer (1 device, 2 compile, 3 compute, 4 render) with result checks; on a 5 s GPU timeout it stays alive instead of exiting (exiting with pending GPU work could panic on teardown).
- `xe-miptest.m` — RGBA8 mipmaps: fill level 0, generate on GPU, read every level back.
- `xe-f16test.m` — RGBA16Float upload and buffer→texture / texture→texture copies, checking every row.
- `xe-atlastest.m` — atlas-style block copies to non-zero positions and mip levels in 4/8/16 B/px formats.
- `xe-buftex.m` — linear texture over an MTLBuffer (`newTextureWithDescriptor:offset:bytesPerRow:`), GPU copy back and verify.
- `xe-bufoff.m` — buffer-backed textures at various offsets, read by blit and by sampling in a shader.
- `xe-vfmttest.m` — draws a quad whose position comes from vertex attributes in many formats (float2, half2, ushort2 norm, UInt1010102Normalized…).
- `xe-fbfetch.m` — framebuffer-fetch test (`[[color(0)]]` read in the fragment shader).
- `xe-xrtest.m` — does the driver accept BGR10_XR/BGRA10_XR, and does the GPU read a `w30r` IOSurface correctly?
- `xe-yuvtest.m` — checks from which offset the GPU reads each plane of a 420v IOSurface (plane-1 offset bug check).
- `xe-pbtest.m` — creates CVPixelBuffers in many formats/attribute sets and reports which fail.
- `xe-xfertest.m` — VTPixelTransferSession 420v → several formats.
- `xe-vttest.m` (+ `.bak`, earlier version) — H.264 encode 8 frames and decode with hardware decoder required/forbidden; prints status and centre pixel.
- `xe-icontest.m` — asks IconServices for app icons and counts opaque pixels (0 = empty icon).
- `xe-layertest.m` — window with CALayers backed by IOSurfaces of different formats; captures the composited window and measures each square.
- `xe-carender.m` — composites layers with CARenderer into a Metal texture; reproduced the missing-icon defect outside WindowServer.
- `xe-encspy.m` — DYLD_INSERT_LIBRARIES spy: logs per draw the fragment function and bound fragment textures (format, size, buffer/IOSurface origin).
- `xe-nofetch.m` — test dylib making the driver report no framebuffer fetch / programmable blending / raster order groups.
- `xe-align.m` — test dylib forcing `minimumTextureBufferAlignmentForPixelFormat:` to 256.
- `xe-nocopyfix.m` — test dylib prototype of the libxedrv v15 NOCOPY fix (copy unaligned `newTiledTextureWithBytesNoCopy` textures).

(All `xe-*.m` tests above without a date are from 20 Oct.)

### 5.2 `xectl/` — direct-hardware bring-up through the XeShim kext (15–16 Oct, before the Apple-driver approach)

**Core**
- `xectl.c` (updated 20 Oct) — CLI over XeShim: `info`, `regs`, `gt`, `rd`/`wr` (MMIO), `rdw`/`wrw` (with forcewake held), `gtt` (GGTT PTEs), `px` (scanout pixel via BAR2), `arm` (toggle the xe-icl-accel marker), `fb 0|1` (XeFB takes/gives back the display).
- `xe.h` — shared helpers for the user-space tools (open XeShim, map BAR0).
- `xe_bcs.c` / `xe_bcs.h` — minimal execlist submission (port of the i915 minimum) for BCS0, also used for RCS0; fresh context per run.
- `xe_gfx.h` — minimal driver layer on top of xe_bcs for Gen12: PPGTT buffer objects, one state heap, surface state / binding table / sampler generation.
- `xe_vdec.c` / `xe_vdec.h` — H.264 decode on the video engine (VCS0, MFX long format, 4:2:0 8-bit, CAVLC/CABAC).
- `h264.c` / `h264.h` — minimal H.264 Annex-B parser (NAL, SPS/PPS, slice header) for xe_vdec.
- `k_ps.inc`, `k_ps_blend.inc`, `k_ps_cc.inc`, `k_ps_tex.inc`, `k_store.inc` — assembled Gen12 EU kernels as C byte arrays (pixel shaders and an A64 store kernel).

**Display / aperture**
- `xe-disp.c` — display prototype: scans out a 1920×1200 XRGB surface on pipe A, upscaled 2× by the pipe scaler; `show`/`restore`.
- `xe-aper.c` — checksums the XeFB scanout buffer to see whether WindowServer draws into it.
- `xe-dump.c` (16 Oct) — copies a range of the BAR2 aperture to a file.
- `xe-open.c` — opens an IOFramebuffer user client on XeFB the way WindowServer does.
- `xe-gtt-clean.c` — restores the GGTT window used by xe_bcs (0xF0000000…) to the firmware scratch PTE after an aborted run.
- `cg-mode.c` — prints what WindowServer/CoreGraphics thinks the main display mode is.

**Milestone programs (incremental GPU bring-up)**
- `xe-m3.c` — first command executed by the GPU from macOS (BCS0 MI_STORE_DATA_IMM via execlists).
- `xe-m4.c` — first accelerated drawing: XY_COLOR_BLT rectangles into the boot framebuffer.
- `xe-m5.c` — first RCS0 commands (MI_STORE_DWORD_IMM, PIPE_CONTROL post-sync).
- `xe-m5b.c` — first EU kernel (GPGPU_WALKER, SIMD8 A64 store).
- `xe-m5c.c` — first 3D draw (RECTLIST into a 64×64 linear RT with an assembled PS).
- `xe-m6a.c` — alpha blending; `xe-m6b.c` — texture fetch (sampler ld) in the PS.
- `xe-m7a.c` — PS compiled by Mesa brw (NIR → Gen12); `xe-m7b.c` — interpolated texcoords, state derived from prog_data; `xe-m7c.c` — full VS+PS pipeline, TRILIST, hardware viewport.
- `xe-m8a.c` — first compositor scene via xe_gfx (UBO tint, blending); `xe-m8b.c` — compute box blur (SSBOs, GPGPU_WALKER).
- `xe-m9.c` — "frosted glass" compositor demo (compute blur + draws).
- `xe-m10a.c` — push constants (3DSTATE_CONSTANT_PS); `xe-m10b.c` — async submission + timeline fences; `xe-m10c.c` — depth testing (D32_FLOAT Tile-Y); `xe-m10d.c` — BO lifecycle (150 create/destroy cycles, VA recycling).
- `xe-m11a.c` — texture/render-target format matrix vs CPU reference; `xe-m11b.c` — GLSL 4.50 fragment shader via Mesa's GLSL front-end → NIR → brw.
- `xe-m12a.c` — 4× MSAA rasterization into a Tile-Y multisampled surface (no MCS).
- `xe-v1.c` — first VCS0 commands (MI_STORE_DWORD_IMM, MI_FLUSH_DW); `xe-v2.c` — H.264 decode on VCS0 compared to a reference I420 file.



---

## 6. Findings from the code audit (comments vs. code, dead paths, latent bugs)

### 6.1 Latent bug in the Metal driver patch

**MTL v11 broke two instructions, and the damage was carried into the release build MTL17. Verified with otool.**

v11 changed byte `0x88` to `0xd0` at three offsets. The goal was to change L3ALLOC `0x88000020` to `0xd0000020`.
- **0x65f12 is a real immediate**, so the change there is correct.
- **0x7ff77 and 0x80253 are not immediates.** They sit inside `IntelState::IntelState()`, where `20 00 00 88` is the end of `movq $0x2000, …` followed by the opcode of the next instruction.
  - At 0x7ff77 the next instruction is `88 87 10 01 00 00` = `movb %al,0x110(%rdi)`. It became `d0 87 …` = `rolb 0x110(%rdi)`.
  - At 0x80253 the next instruction, `movb %cl,0x8e0(%rbx)`, became `rorb 0x8e0(%rbx)`.
  - As a result, those two byte fields are no longer initialized.
- **Fix:** both bytes go back to `0x88`. This is **MTL18**: the two entries were removed from `macos/mtl17-patches.json`. MTL18 is being tested.

### 6.2 Default behaviour that differs from the comments

1. **submitBlit is a no-op by default on spoofed hardware.** `getV142SubmitBlitMode()` returns 1 by default, and the V170 comment's claim that "real TGL uses mode 3" is not what the code does. The early V186 return then makes every spoofed submitBlit a no-op. V149, V171, V135, V120, V193 and the later V142 check run only with `ngreenV142=3` or `-ngreenV142orig`; the release config sets `ngreenV142=3`.
2. **V78 / XE-DPCAPS.** The display pipe is natively ON unless `ngreendp0` is set. The fork made V77's capability dictionary follow the same rule.
3. **V45/V55 delayed checks do nothing by default.** They are scheduled on the spoofed path, but they return immediately unless the V60 monitor or `ngreenexp` is on.
4. **V500 is not only a logger.** The comment calls `IOAccelLegacySurface::set_id_mode` a "diagnostic logger", but on spoofed hardware it strips mode bits `0xff8073c0`.
5. **Several hooks described as "read-only" write state:**
   - V400 zeroes the seam gates at +0x1E3/+0x1E5.
   - V401 forces a full reprogram and zeroes SEAM/WIN_SZ.
   - V403 zeroes the seam fields.
   - V405 clears bit 10 of FB+0x4248.
6. **V406 tiling enum.** The route comment says linear (2); the compiled path sets 0 (X-tiled).
7. **V205 runs on real TGL too.** Its PSR1/PSR2 clearing at `0x60800`/`0x60A10` is not limited to spoofed hardware.

### 6.3 Code that never takes effect

1. **Dead or unrouted code:**
   - the per-engine workaround block after an unconditional `return` in `IGHardwareExtendedContextinitWithOptions`;
   - the upstream Wa_1607297627 RCS PSMI write (the fork re-implements it as XE-PSMI);
   - the V204b AppleBacklight route (`kextBacklight` is not registered);
   - the CoreLSKD CPUID DYLD patch (its path test can never pass);
   - the HDMI module;
   - `kern_netdbg`;
   - `allocateDisplayResources`, `hwSetMode`, `beginReset`/`endReset`, `readAndClearInterrupts`, `IGScheduler5resume` (upstream wrapper), `raWriteRegister64`, `wrapPmNotifyWrapper` and `patchRCSCheck`, none of which has a live route;
   - V208-V210, whose routes are commented out.
2. **`Gen11::framecont` is never assigned.** The `wrapWriteRegister32` and `wrapReadRegister32` helpers therefore always use direct MMIO.
3. **The CoreDisplay DYLD byte patches (V187/V189 etc.) may not apply on Tahoe.** They are hard-coded Sonoma 14.7.1 patterns and a miss is silent. The code does not show whether they match on Tahoe.
4. **The upstream HANGCHECK dump is on by default.** It runs once, on the 15th forceWake, and briefly remaps GGTT[0]. The fork silences it under `-ngreenxefix`.
