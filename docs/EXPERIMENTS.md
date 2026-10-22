# Experiments that failed, were abandoned or were reverted

Everything that was tried and did not survive into the final stack, plus the tests whose value was a negative result.
The story around each item is in [HISTORY.md](HISTORY.md) (phase numbers below refer to it); what survived is in
[KERNEL-PATCHES.md](KERNEL-PATCHES.md), [METAL.md](METAL.md) and [CATALOG.md](CATALOG.md).

Dates are October 2026, local (Pacific) time of the Tahoe install. "Kept" means the change is still installed although
it did not fix the problem it was written for.

## Summary table

| # | What | When | Phase | Verdict |
|---|---|---|---|---|
| 1 | Own driver stack (XeShim + xectl + Mesa) as the macOS driver | ≤ 16 Oct | 0 | abandoned |
| 2 | Own framebuffer `XeFB`, Ice Lake driver reuse | ≤ 16 Oct | 0 | abandoned |
| 3 | `-ngreendp0` (no DisplayPipe) | 17 Oct | 2 | replaced |
| 4 | `-ngreentglfb` with the accelerator | 17 Oct | 2 | panic; forbidden |
| 5 | Lilu routes on framebuffer mode functions | 17–18 Oct | 2 | never called |
| 6 | Byte-patching `fCurrentMode` | 18 Oct | 2 | symptom only; opt-in |
| 7 | Long boot-args | 17–18 Oct | 2 | silently truncated |
| 8 | Metal bundle in `/Library/Extensions` | 18 Oct | 3 | not enumerated |
| 9 | Ad-hoc signed driver with AMFI on | 18 Oct | 3 | rejected |
| 10 | Apple-Internal bit in `csr-active-config` | 18 Oct | 4 | discarded at boot |
| 11 | Compiler in `/Library/GPUBundles`, MTL v1 | 18 Oct | 4 | sandbox |
| 12 | MTL v2/v3 (`/System/Library/Caches/xe`) | 18 Oct | 4 | prefix rule |
| 13 | MTL v4 (`PrivateFrameworks/../Caches/xe`) | 18 Oct | 4 | substring rule |
| 14 | Writing into the sealed system volume | 18 Oct | 4 | not done |
| 15 | Hybrid compiler (Tahoe plugin + TGL libigc) | 18 Oct | 4 | API mismatch |
| 16 | `libigc` forwarding shim | 18 Oct | 4 | not built |
| 17 | MTL v6 static offset patches | 18 Oct | 6 | superseded |
| 18 | In-place descriptor conversion | 18 Oct | 6 | broke Metal's init |
| 19 | libxedrv in `/System/Library/Caches` | 18 Oct | 6 | WindowServer sandbox |
| 20 | `IGLogLevel=8` | 17–18 Oct | 7 | panic; lowered to 3 |
| 21 | "GPU DMA corrupts memory" + `-ngreenengineinert` | 18 Oct | 7 | refuted |
| 22 | A/B with the Reddit RPL-HX kext | 18 Oct | 7 | froze early |
| 23 | LRCA repair after engine start | 18 Oct | 7 | inconclusive |
| 24 | `xe-auto` boot agent | 18 Oct | 7 | disabled |
| 25 | libxedrv "lab mode" | 18 Oct | 8 | WindowServer aborts |
| 26 | Bridge v1 fixed offset for `getSerializedModule` | 18 Oct | 8 | replaced by v2 |
| 27 | `ngreenSched=4` | 18 Oct | 9 | ignored |
| 28 | XE-NOCCS (properties) | 18 Oct | 9 | does not disable compression; kept |
| 29 | Burst forcewake sampling | 18 Oct | 9 | EL[0] panics; opt-in |
| 30 | XE-NOCCS2 | 18 Oct | 9–10 | caused fast-clear hangs; removed |
| 31 | MTL v11 L3ALLOC | 18 Oct | 9, 19 | not causal; two bytes corrupted code → MTL18 |
| 32 | PSMI re-application | 18 Oct | 9 | not causal |
| 33 | libxedrv v12 `allowGPUOptimizedContents=NO` | 18 Oct | 10 | ignored by the driver; kept |
| 34 | XE-DPCAPS alone | 18 Oct | 10 | no effect alone; kept |
| 35 | Renaming WindowServer display prefs | 19 Oct | 10, 16 | no effect |
| 36 | "Lost interrupt" CSB theory | 19 Oct | 10 | misreading |
| 37 | IGScheduler4 empty-stub theory | 19 Oct | 11 | scheduler 5 is forced |
| 38 | Forcewake theory: FWREF / FWHOLD / FWSUBMIT | 19 Oct | 12 | not causal; FWHOLD worse |
| 39 | V509 suspicion | 19 Oct | 12 | innocent |
| 40 | XE-POSH (zero `POSH_LRCA`) | 19 Oct | 12 | not causal; kept |
| 41 | XE-ELSQWAIT | 19 Oct | 12 | never triggers; kept |
| 42 | XE-NOPOSH | 19 Oct | 12 | worse; opt-in |
| 43 | XE-ELSQDELAY / ELSQIDLE / IDLEANY / NOPREEMPT | 19 Oct | 12–13, 15–16 | not the root cause; IDLW now 5 ms |
| 44 | XE-ELSQRETRY | 19 Oct | 12, 15 | engine wedged; opt-in |
| 45 | MTL13 / MTL14 (fast clear, dual SIMD8) | 19 Oct | 13 | not causal; kept |
| 46 | MTL15 / XE-NOAUX (compression) | 19 Oct | 13 | not causal; kept |
| 47 | MTL16 alone | 19 Oct | 13 | empty windows → MTL17 |
| 48 | "3D state lost on context switch" | 19 Oct | 14 | disproved by LRCSCAN |
| 49 | XE-NOTCF / NOTCF2 (Tile Cache Flush) | 19 Oct | 14 | not causal; kept |
| 50 | XE-BLIT16 (SIMD16 only) | 19 Oct | 14 | not causal; kept |
| 51 | XE-BLITSCRATCH v1, BLITSCRATCH2 v1 | 19 Oct | 14 | silent no-op; boot loop |
| 52 | XE-PLANES (`xor eax,eax`) | 19 Oct | 15, 18 | caused green YUV → PLANES2 |
| 53 | XE-NOPAGE | 19 Oct | 15 | empty windows; opt-in |
| 54 | `ngreenpreemptwait` 2 ms / 15 ms / 50 ms | 19 Oct | 15 | stutters or artifacts |
| 55 | Combined kext (CSBGUARD2 + IDLW 15 ms + PAGEONGUARD) and rollback to the "good" build | 19 Oct | 15 | glitches; then EL[0] panics |
| 56 | XE-RPCS on by default | 19 Oct | 15 | very slow; opt-in |
| 57 | WhateverGreen + CpuTopologyRebuild + BIOS cores in one boot | 19 Oct | 15 | no boot |
| 58 | Wi-Fi bisection | 19 Oct | 15 | declined |
| 59 | XE-POOL2 / IDLW / display prefs against c01dbead | 19 Oct | 16 | no effect |
| 60 | `xe-brilho` key daemon | 20 Oct | 17 | did not react to keys |
| 61 | `softwareupdate` / gibMacOS installer download | 20 Oct | 17 | interrupted |
| 62 | `hardwareVideoDecoder=disable` | 20 Oct | 18 | no effect |
| 63 | XE-NOGVA against −6680 | 20 Oct | 18 | partial |
| 64 | sRGB display profile | 20 Oct | 18 | worse; reverted |
| 65 | Icon-cache clearing | 20 Oct | 18 | no effect |
| 66 | XR / EDR format theory for the icons | 20 Oct | 18 | wrong cause |
| 67 | XE-NOORPHAN against all "corrupt list" panics | 20 Oct | 18 | partial |
| 68 | ITU-709 colour profile | 20 Oct | 19 | three panics; reverted |
| 69 | E-cores: CpuTopologyRebuild / HT patch / fewer CPUs | 20–21 Oct | 21 | not the cause; divide by zero |
| 70 | Chrome: "the GPU disk cache is bad" | 21 Oct | 22 | wrong; blit race |
| 71 | Chrome: XE-RCPOLICY or the E-cores | 21 Oct | 22 | wrong |
| 72 | `w30r` / BGR10_XR proof of concept | 21 Oct | 22 | works in a test process; not shipped |
| 73 | FBC theory for the boot-bar glitch (XE-FBCWA) | 21 Oct | 23 | FBC never on; removed |
| 74 | XE-BOOTHOLD v1–v3 | 21 Oct | 23 | not enough → v4 |
| 75 | CDCLK reprogramming as the boot line | 21 Oct | 23 | untested; gone with `Resolution=Max` |
| 76 | `Resolution=Max` without `gfxrst` | 21 Oct | 23 | ~20 s stall |
| 77 | XE-MAXRES (route on `IOFramebuffer::initFB`) | 21 Oct | 23 | panic at boot |
| 78 | Fast stamp-3 wait, OpenCore without picker | 21 Oct | 23 | prepared, not installed |
| 79 | Legacy AirportItlwm + root patch | 21 Oct | 24 | not done |
| 80 | Hibernate-only, injected `_S3`, BIOS `setup_var` | 22 Oct | 24 | prepared / not tried |
| 81 | Safari decode: sandbox overrides, symlinks, SLE root patch | 21 Oct | 24 | rejected; patch not applied |
| 82 | On-demand touchpad pulse | 21–22 Oct | 24 | no command exists |
| 83 | Display audio via AppleGFXHDA (device-id spoof) | 21 Oct | 25 | assessed, not run |
| 84 | M2: ROM handshake, IPC after the core runs | 21 Oct | 25 | ROM did not answer |
| 85 | M2: IPC first (Linux order) | 21 Oct | 25 | SRAM window `0xffffffff` |
| 86 | M2b stream-count pre-check | 21 Oct | 25 | bug: rejected 16 streams |
| 87 | M2b SSP clock-port write | 21 Oct | 25 | silently refused; not needed |
| 88 | M6: first XeSpeakers install | 22 Oct | 25 | device dropped by coreaudiod |

---

## Architecture and approach

**1. Own driver stack as the macOS driver (≤ 16 Oct).** XeShim (a minimal user-client kext) plus a user-space execlist
driver and Mesa's Intel compiler built on macOS reached pixel-exact 3D, compute and a "frosted glass" compositor, and
even H.264 decode on VCS0. *Why it seemed promising:* full control and full knowledge of every command. *What happened:*
the lower half worked; the upper half (an IOAccelerator and a Metal driver that WindowServer would use) would have had
to be written from scratch. *Taught:* Gen12 pitfalls (dependency annotations, EOT registers, MOCS ≠ 0), and left behind
XeShim/xectl, which became the main diagnostic tools.

**2. Own framebuffer `XeFB` and Ice Lake driver reuse (≤ 16 Oct).** `XeFB` crashed in `postOpen`. Analysis of
IOGraphics internals was cut twice by an automatic content filter, and the project closed that line (framebuffer swap,
`XeICLFix`/`XeICLBridge`, IOGraphics analysis) rather than work around it. Tahoe's ICL compiler plugin also accepts only
ICL/KBL, and its `libigc` has no Gen12.

## Display and boot

**3. `-ngreendp0`.** Forces `DisplayPipeSupported=0`. It avoided the WindowServer abort loop but gave `IOPresentment
Interface creation error 0x2005` and no Metal path. Replaced by `-ngreenfixbootmode`. *Taught:* CoreDisplay requires a
MetalDevice only when the DisplayPipe is used.

**4. `-ngreentglfb` with the accelerator enabled.** Kernel panic, then boots stopping at random lines. NootedGreen
registers accelerator hooks only with `-ngreentglwithgfx`. Never combine them.

**5. Lilu routes on `getCurrentDisplayMode` / `getPixelInformation` / `validateDisplayMode`.** Installed (original
pointers valid) but never invoked (`calls=0`). *Taught:* in this kext prefer `LookupPatchPlus` byte patches and worker
threads.

**6. Patching `fCurrentMode` (`0xfffffffb` → `0x80001000`, FB `0x53de3`).** Applied correctly (`tried=1 ok=1`) and changed
nothing: the real cause was the missing `IOFBDetailedTimings` table. Kept as opt-in `-ngreenfixbootpatch`.

**7. Long boot-args.** Truncated at about 228 characters with no warning; flags at the end were silently lost
(`…-ngreenfix`). Debug arguments (`liludump`, `liludbuf`, `msgbuf`, `debug=0x100`, `keepsyms`, `IGLogLevel`) were dropped
at various times to make room.

## Getting Metal and the compiler loaded

**8. Metal bundle in `/Library/Extensions`.** Loads with `NSBundle`, but `MTLCopyAllDevices()` returns 0. Metal enumerates
third-party GPU bundles from `/Library/GPUBundles`.

**9. Ad-hoc signed driver with AMFI on.** `Library Validation failed … mapping process is a platform binary`.
`amfi_get_out_of_my_way=1` is required.

**10. `csr-active-config` with the Apple Internal bit (`0x0a13`).** Meant to open the MTLCompilerService sandbox to
`/Library/GPUBundles`. The bit is discarded at boot ("Apple Internal: disabled"); no effect.

**11–13. Plugin path rules (MTL v1–v4).** v1 (`/Library/GPUBundles`) passes the plugin prefix list but the compiler
sandbox denies it. v2/v3 (`/System/Library/Caches/xe`) pass the sandbox but fail `validPathPrefixes` ("not registered with
the MTLCompilerObject"); the v3 hypothesis that a double slash broke a match was wrong. v4
(`/System/Library/PrivateFrameworks/../Caches/xe/…`) passes the prefix but fails the second rule, a `.bundle` /
`.framework` substring. **v5** (`…/../Caches/xe.bundle/…`) satisfied all of them. *Taught:* the four gates of Metal plugin
loading on Tahoe (location, AMFI, per-process sandbox, path rules).

**14. Writing the compiler into the sealed system volume** (`bless --create-snapshot`). Considered, rejected as riskier
than the firmlink trick.

**15. Hybrid compiler** (Tahoe plugin + TGL `libigc` + TGL `.split` files). `Assertion failed: (m_pReadBitcodeFn)`: the new
plugin needs `readBitcode*` exports the old `libigc` lacks, and Tahoe's `libigc` has no Gen12.

**16. `libigc` forwarding shim** (export lists compared). Judged too risky with the context-based new API; not built. The
old plugin + LLVM bridge + adapter replaced it.

**26. Bridge v1** located MTLCompiler's `getSerializedModule` at a fixed offset (`0x7FB06`). Inside MTLCompilerService a
different MTLCompiler version is loaded, the prologue check failed silently, modern bitcode reached the old `libigc`, and
some CoreAnimation shaders crashed it. Bridge v2 finds the function by signature.

## Driver ABI

**17. MTL v6** — 13 static displacement patches for the +0x10 descriptor shift. It got past `generate3DStateVF` but could
not handle the bitfield changes (blend state crashed). Replaced by runtime conversion in libxedrv.

**18. In-place descriptor conversion** for the whole pipeline `init`. Broke Metal's own superclass code (`setLabel:` →
`copyWithZone:` unrecognized selector). Replaced by shadow copies returned only to callers inside the driver.

**19. libxedrv in `/System/Library/Caches`.** `deny file-map-executable`: WindowServer maps executables only from
`/Library/GPUBundles`. The library lives inside the MTL bundle.

**25. libxedrv "lab mode"** (WindowServer gets no MTLDevice so tests can use the GPU without rebooting). With the
accelerator present and the device returning nil, WindowServer aborts and loops on a black screen.

## Kernel stability, 18 Oct

**20. `IGLogLevel=8`.** Flooded the msgbuf (load-time logs lost) and caused an NMIPI spinlock panic in `kprintf` inside
`IOService::newUserClient`. Lowered to 3.

**21. The DMA-corruption hypothesis.** After seven panics during WindowServer teardown and heap corruption with one
flipped pointer bit, the theory was that the GPU wrote to wrong addresses. `-ngreenengineinert` (engines never started,
engine MMIO writes dropped) reproduced the same `pmap_recycle_page … is referenced` panic **with the GPU idle**. Refuted.
*Taught:* the teardown of a graphics process with live GPU mappings is itself dangerous; batch hypotheses and test
offline.

**22. A/B test with another author's NootedGreen build (Reddit RPL-HX).** Froze before user space at the IOGraphics
display takeover; it lacks this machine's display fixes and only a binary was available. Confirmed the design (same
user-space plan in kernel hooks) without solving anything.

**23. `xeLrcaRepair` after engine start.** Meant to fix a `00ffffff` fill in LRCA page 1. Ended in a WindowServer watchdog
panic with the logs lost; not pursued. The `00ffffff` pattern was later attributed to the verbose console drawing
into the same GGTT range (HISTORY Phase 9.3).

**24. `xe-auto` boot agent.** A queue of experiments with automatic reboot and log collection. First run hung before
`launchd` and looked for the EFI on the wrong disk; later an empty queue made it write empty boot-args to the EFI.
Disabled. *Taught:* an agent cannot recover from hangs before user space.

## Making the GPU execute

**27. `ngreenSched=4`.** Meant to try `IGScheduler4` instead of the host scheduler 5. `-disablegfxfirmware` forces scheduler
5 regardless (scheduler 4 is the GuC path), so the argument is ignored — although it is still in the release boot-args.

**28. XE-NOCCS** (`RenderCompressionMode=0`, `AllowIOSurfaceRenderCompression=0`). `RenderCompressionMode` only selects the
compression type (ALL/128/64); it does not disable compression. Kept.

**29. Burst forcewake sampling in xe-rcsdump** (~25 000 reads in 200 ms windows). Correlated with early "EL[0] was
invalidated" panics; made opt-in (`XE_RAJADA`). Its finding (RCS never active) was nevertheless correct.

**30. XE-NOCCS2** (skip `enableRenderCompressionWithAccelTask` entirely). Stopped the `resolve_ccs` panic but removed the
AUX mapping while Metal kept compressing and fast-clearing — creating the fast-clear hang. Replaced by XE-NOCCS3 (only
the resolve returns). *Taught:* cut the narrowest thing that faults.

**31. MTL v11 (L3ALLOC `0x88000020` → `0xd0000020`).** The hung batch loaded L3ALLOC with a non-RPL value. The value
changed and the hang did not. Kept as harmless — but the patch replaced a byte pattern at three offsets, and only
`0x65f12` was the L3 value; at `0x7ff77` and `0x80253` it turned `movb` instructions in `IntelState::IntelState()` into
`rolb`/`rorb`. Found on 20 Oct; MTL18 restores them (in test). *Taught:* verify byte patches as instructions.

**32. PSMI re-application** (Wa_1607297627 written again every second from user space). Identical hang. Showed that
`0x2050` falls back to its reset value several times a second in RC6 (re-applying it in the kernel was noted, not done).

## Flips, pools and the scheduler

**33. libxedrv v12** (`allowGPUOptimizedContents = NO` on every texture). The TGL driver reads its private descriptor, not
that property. Kept.

**34. XE-DPCAPS.** Advertising the display-pipe capabilities as Apple's plist does was correct but did not unblock the
flips; the real refusal came from `ValidateTransaction` (XE-VALD). Kept.

**35. Renaming WindowServer's saved display prefs** — once for a plane-size theory (19 Oct 00:28) and once, in reverse, for
the c01dbead hang (restoring them). No effect either time.

**36. "Lost interrupt" CSB theory.** The CSB read pointer looked stuck at 0; Apple stores it in bits 15:12 of `0x23a0`. The
CSB was processed. The CSB dump also fired only after the GPU reset (a consequence).

**37. IGScheduler4 empty-stub theory** (flip completion signalled through an empty `signalDisplayStampUpdate`). Irrelevant:
scheduler 5 is in use.

**38. The forcewake theory of lost ELSP writes.** XE-FWREF (correct reference counting) and XE-FWSUBMIT (hold forcewake
around submission) changed nothing and are kept; XE-FWHOLD (never release) made things worse (zeroed CSB entries, "EL[0]
was invalidated" panic) and is opt-in. A recursive-lock panic from interrupt context had to be fixed on the way.

**39. V509 suspicion.** It writes a minimal context image when page 1 looks empty; logging showed it never repaired
anything on the lost context.

**40. XE-POSH (zero `POSH_LRCA`).** The only structural difference between a context that ran and one that did not. Zeroing
it did not help. Kept (harmless). The real fix (XE-POSHCTX) removes POSH from contexts altogether.

**41. XE-ELSQWAIT** (wait for `EXECLIST_STATUS` bit 30 before submitting). Bit 30 appears only after a refused write; the
wait never triggers. Kept.

**42. XE-NOPOSH** (clear the POSH bit in `MI_BATCH_BUFFER_START`). Black screen before the high-resolution boot logo.
Opt-in `-ngreennoposh`.

**43. Waiting before RCS submissions** — XE-ELSQDELAY (fixed 50 µs), XE-ELSQIDLE ("IDLW", wait for idle when a context is
current), XE-IDLEANY (wait even without arbitration), XE-NOPREEMPT (wait while arbitration is on). None fixed the root:
the RCS was already stuck inside a batch, and the refused "double submission" was a consequence. IDLW waits also cost
seconds of CPU (`idlw=1444/5407ms … to=8` in ~3 minutes) and caused stutters. It was turned off, then back on with a 5 ms
cap when the c01dbead hang appeared on two boots without it (item 59 showed that was a coincidence).

**44. XE-ELSQRETRY** (reload the ELSQ once idle). The status stayed frozen at `0x40018098` for 200 ms — the engine was
wedged. Later suspected in an "EL[0] was invalidated" panic; opt-in.

## Draw hangs

**45. MTL13 (no fast clear) and MTL14 (no dual SIMD8 in clears).** The hang moved to ordinary clears. Kept.

**46. MTL15 and XE-NOAUX (no render compression).** MTL15 coincided with the first desktop login, but RTDUMP still showed
compressed targets; with NOAUX (AUX_NONE everywhere) the hang was identical. Compression was not the cause. Both kept.

**47. MTL16 alone.** Emitting a disabled VS in clears removed the hangs but left the encoder's state tracker believing the
old VS was still programmed: empty windows. MTL17 marks the VS dirty.

**48. "3D state is lost on context switch."** LRCSCAN found SBA/VB/VS/PS saved in the context images. The real cause of the
Blit3D hang was empty kernels (BLITSCRATCH2).

**49. XE-NOTCF / XE-NOTCF2** (no Tile Cache Flush in flip and stamp `PIPE_CONTROL`s). Applied; the hang remained. Kept.

**50. XE-BLIT16** (SIMD16-only Blit3D). Applied; the hang remained. Kept.

**51. XE-BLITSCRATCH v1 and BLITSCRATCH2 v1.** v1 called the upload through NootedGreen's V181 wrapper, which silently
cancels because its lock/unlock routes are commented out. BLITSCRATCH2 v1 copied straight into the CPU mapping and
faulted at `cpu+0xd000` (boot loop) — the mapping covers only `0xd000` bytes. The final version copies through a
temporary buffer.

## Resources and stability

**52. XE-PLANES** (`xor eax,eax` for the IOSurface plane count). Fixed the Displays divide-error panic but made every
surface single-plane: green HEIC wallpapers, zero chroma, probably the bad pointers behind XE-RESFREE. Replaced by
XE-PLANES2 (read the real Tahoe offset `+0x98`); `-ngreenplanesxor` restores v1.

**53. XE-NOPAGE** (`pageon`/`pageoff` as no-ops). `pageon` is normal texture preparation; windows and the menu bar emptied.
Opt-in `-ngreennopaging`; replaced by XE-PAGEONGUARD.

**54. `ngreenpreemptwait` tuning.** 50 ms: stutters; 2 ms: artifacts (`to=861`); 15 ms: compromise; then back to 50 ms with
the "good" build, and finally 5 ms.

**55. The combined kext and the rollback.** CSBGUARD2 + IDLW 15 ms + PAGEONGUARD at once brought new glitches (bands over
the login clock, missing keyboard). Rolling back to the "good" QUIETLOG build then panicked at every login ("EL[0] was
invalidated" after WindowServer re-applied a saved HiDPI mode) and in normal use: the CSB guard is needed. *Taught:* one
change per boot from a known-good base.

**56. XE-RPCS on by default** (`R_PWR_CLK_STATE` like Linux, from WhateverXe). Submissions dropped from ~47/s to 4–7/s.
Opt-in `-ngreenrpcs`.

**57. Two configuration changes in one boot** (WhateverGreen limited to Sequoia + CpuTopologyRebuild, plus BIOS core
settings). No boot, cause unknown; recovered from another Mac. As a side effect Wi-Fi started working on Tahoe after the
BIOS change.

**58. Wi-Fi bisection** (old kext in the EFI to find which NootedGreen change broke itlwm's firmware load). Declined by
the user to stay focused on graphics; Wi-Fi came back after the BIOS change.

**59. Against the c01dbead Blit3D hang:** XE-POOL2 (smaller pools for user processes), IDLW back on, restoring display
prefs. None changed it; the freezes stopped after XE-NODEPTH, and the hang was never root-caused. The idea of making
Blit3D pool pages resident before submission (WhateverXe 239c010) was not implemented.

## Platform, video and colour

**60. `xe-brilho` daemon following the brightness keys** (CGEventTap). Did not react to the keys; its LaunchAgent was not
installed. `xe-brilho NN` by hand works.

**61. Installer download on the accelerated system.** `softwareupdate --fetch-full-installer` hung at ~90 % with a GPU hang
and WindowServer watchdog; the next run left a truncated `SharedSupport.dmg` (`createinstallmedia` error 113); a gibMacOS
download was interrupted. Tahoe was installed outside the session. *Taught:* long heavy sessions still hit the GPU hang.

**62. `defaults write com.apple.coremedia hardwareVideoDecoder disable`.** No effect on Safari/YouTube; reverted.

**63. XE-NOGVA against −6680.** Removing the `IOGVA*` properties stops VideoToolbox from trying hardware decode (−12913 →
not advertised), but software decode with an explicit output pixel format still fails with −6680. Kept; Safari video
remains broken. Chrome with `--disable-gpu --disable-gpu-compositing` is the workaround.

**64. sRGB display profile** (`tools/xe-perfil`) while chasing the invisible icons. Made the test squares worse; reverted
to the factory profile (`xe-perfil padrao`).

**65. Clearing the icon caches.** No effect: the icons were correct; their textures were not sampled.

**66. The "unsupported XR/EDR format" theory for the icons.** `w30r` / BGR10_XR is indeed unsupported by the 16.0.32
driver, but the invisible icons had a different cause: zero-copy textures at offset `0x20` (fixed by libxedrv v15).

**67. XE-NOORPHAN against "corrupt list".** Disabling orphan-pool reuse fixed the `getMemory` path; the shutdown path
(`IOAccelSysMemory::free → addMemory`) still panics.

**68. ITU-709 colour profile in System Settings › Displays.** Three page-fault panics in GPU memory management
(`newResourceWithClientBuffer`/`client_address_persistent`, `orphanIt`/`releaseDescriptors`,
`IGHardwarePageTable::commitRange`). Reverting to the factory profile restored stability. Do not change the display
profile.

## Stability, Chrome and formats, 20–21 Oct

**69. E-cores: CpuTopologyRebuild, the HT patch and fewer CPUs.** With any E-core enabled the kernel froze right after
EXITBS. CpuTopologyRebuild 2.0.2 (first without its required HT patch), the HT patch itself, and boots with only one,
six or eight E-cores all froze the same way, and E-cores off with CTR stopped before the progress bar. A diff of the
OpenCore logs showed the only difference was ProvideCurrentCpuInfo setting cores-per-package to 128 on a hybrid CPU;
this 4P+8E part reports 64 logical per package, so xnu divided by zero in `cpu_thread_alloc` before any console
output. The `Cpuid1Data` spoof fixed it; CTR then works in its default mode.

**70. "Chrome breaks when its GPU disk cache is populated."** Clearing the cache, locking the cache folders
(`chflags uchg`) and redirecting Metal's per-app function cache to an empty folder were tried. A fresh profile with an
empty Metal cache was still broken, and Dawn toggles only seemed to help because each new toggle forced new
compiles. The cache only changed timing: the real cause was a fill/copy ordering race in the driver's 2D blit path
(fixed by libxedrv v16). `tools/xe-chrome-limpa.sh` remains as a harmless cleanup.

**71. XE-RCPOLICY and the E-cores as the Chrome trigger.** Both arrived on the same day the breakage became frequent.
Neither was involved: the captured index buffer was zero with both unchanged, and the blit tests reproduce the race in
a small Metal program.

**72. `w30r` / BGR10_XR.** A test library made Metal accept the XR pixel formats (answering the Apple-family query only
for those formats) and made CoreAnimation map `w30r`. Gen12 has no XR-bias sampling format, so the values would be
wrong anyway (XR black would show as ~37 % grey), and flipping CoreAnimation's check also moves 10-bit video onto
Apple-only formats, which would crash WindowServer. Not shipped; real Intel Macs behave the same.

## Boot screen and boot speed, 21 Oct

**73. Frame-buffer compression behind the boot-bar glitch.** NootedGreen enables the FBC platform flag and Apple's
`hwFBCSet` lacks two display-13 workaround bits, so a CPU-drawn progress bar on a compressed plane looked plausible. An
XE-FBCWA write filter was built together with BOOTDIAG logging. The first log showed FBC disabled for the whole boot;
FBCWA was removed.

**74. XE-BOOTHOLD v1–v3.** v1 kept plane A disabled until the first non-zero surface address, v2 until the second, v3
until an address repeated (WindowServer's ring had wrapped). Each moved the hold to the right moment, but the flash
stayed: the plane disable latched in the same frame as Apple's new stride and size, and part of what was seen was not
plane-A scanout. v4 adds the backlight to the hold and a minimum time (`xebbms`); that removed it.

**75. CDCLK reprogramming as the bright line early in boot.** NootedGreen always disables and re-programs the display
clock inside the framebuffer start while the pipe scans out, which could cause one underrun line. Before a fix was
written, the line disappeared together with the 1024×768 stretch stage once OpenCore used `Resolution=Max`. Not
tested further.

**76. `Resolution=Max` alone.** The native-resolution GOP removed the stretched boot stage but added a ~20 s black stall
at WindowServer's first framebuffer open (22–24 s instead of 2–6 s). Cause: WhateverGreen's default framebuffer-copy
mode reads the whole boot console framebuffer with the CPU through an uncached mapping (36.9 MB at 3840×2400).
`gfxrst=1` keeps Max and removes the stall.

**77. XE-MAXRES.** A NootedGreen route on `IOFramebuffer::initFB`, forcing the "do not keep the boot logo" path, panicked
at boot twice ("previous plugin had short jump type on a multiroute function"): WhateverGreen routes the same function
first, and Lilu does not allow a second route there. Never install that build.

**78. Faster accelerator start and no picker.** A NootedGreen variant that ends the 6.5 s wait for accelerator stamp 3
after 250 ms (the wait always ends by timeout today), and an OpenCore config without the picker, were built but not
installed. OpenCore file logging off and a 1 s picker timeout were applied instead.

## Platform, 21–22 Oct

**79. Legacy AirportItlwm with a root patch.** A second Tahoe build of AirportItlwm restores the Ventura wireless stack
through an OCLP-derived root patch: it brings AWDL, AirPlay and Handoff but still not reliable AirDrop, breaks the
system-volume seal and must be re-applied after every macOS update, during which there is no network. The EFI-only
native build worked, so this route was not taken.

**80. Real sleep.** Three ways to get any sleep were prepared and not tried: hibernation only (`hibernatemode 25` with
OpenCore resuming the image, so the panel is lit by GOP again), an SSDT that exposes `_S3` (the BIOS may have no S3
resume code; same-generation Dell reports crash on wake), and BIOS `setup_var` changes (offsets unknown; a wrong write can
leave the machine unbootable). The TGL framebuffer cannot retrain eDP after a power cycle in any of them.

**81. Safari GPU-process decode.** WebKit's GPU-process sandbox is compiled in and has no entitlement or default that
allows executable mappings from `/Library/GPUBundles`; sandbox defaults keys, symlinks (resolved before matching) and
Safari Technology Preview (same profile) cannot help. Installing the VA plugin in `/System/Library/Extensions` through
a sealed-volume root patch would work and is reversible (`bless --last-sealed-snapshot`); the script was prepared and
not applied.

**82. An on-demand touchpad pulse.** The Elan HID endpoint exposes only the vibration intensity (feature 0x09); the
Microsoft haptic trigger and waveform usages are absent, and VoodooInput's Force Touch actuator requests have nowhere to
go. The vendor firmware packages show an Aito haptic module behind the Elan endpoint, reachable only through
undocumented vendor reports that also carry its bootloader. No pulse command is known; blind probing was rejected as
unsafe for the module's flash and calibration.

## Internal speakers, 21–22 Oct

**83. Display audio through AppleGFXHDA.** Spoofing the audio function as an Ice Lake controller (`device-id 0x34C8`,
`hda-gfx`) might give HDMI/DP audio through Apple's driver. It conflicts with AudShim (both match the same function),
depends on the TGL framebuffer enabling the display-audio power well, and needs an external monitor. Assessed only.

**84. M2: ROM handshake with the IPC sent after the core runs.** Power, reset and stall of DSP core 0 behaved exactly as
on Linux and the ROM initialised its SRAM window, but it stayed in `INIT` and ignored the late `ROM_CONTROL` IPC
(BUSY never consumed). Rolled back cleanly.

**85. M2: IPC first, in Linux's order.** This time the ROM picked up `ROM_CONTROL | PURGE_FW | tag 1` and then its SRAM
window read `0xffffffff`: it acted on the message and died or powered the memory down, because the host DMA stream with
that tag did not exist. Fixed in M2b by preparing what Linux prepares first (controller reset, PPCTL, L1SEN off, bus
mastering, the ICCMAX stream and the code-loader stream).

**86. M2b stream-count pre-check.** The kext refused to arm when input + output streams exceeded 15; this controller has
ISS 7 + OSS 9 = 16 (HDA allows 30). Caught on the dry run, before any write.

**87. M2b SSP clock-port write.** Linux sets the SSP clock-port bits before the ROM IPC. AudShim's BAR4 write whitelist
did not include those registers, so the write was refused and logged, yet M2b succeeded. Noticed while building M3;
the write stayed off by default (`--ssp` opt-in) because the SSPs are unused on this SoundWire-only laptop.

**88. M6: XeSpeakers device dropped.** On the first install coreaudiod loaded the plug-in and the kext brought the
hardware up, but the device never appeared: coreaudiod logged an unknown-property error (`'who?'`) and "Caught exception
trying to add device". While adding a device the HAL reads latency, safety offset, default-device eligibility and
preferred channel properties in every scope, including input, and the plug-in answered them only for output; it also
lacked the empty box and clock-device lists. With those answered (input scope reports 0/none) the device appears and
plays.

---

## Negative results worth keeping

Tests that ruled something out are as useful as fixes. Each of these was measured, not assumed.

| Ruled out | Test | Result |
|---|---|---|
| Bad Metal descriptor ABI in IOAF2 | vtable diff of 9+ IOAF2 classes, Big Sur vs Tahoe | 0 differing slots |
| Wrong GPU topology in NootedGreen | fuse registers `0x9118`, `0x9134…` | 6 DSS, all EUs and L3 banks, as hard-coded |
| PPGTT mis-mapping of the hung batch | PTWALK | valid PTE |
| GPU writes not reaching memory | read post-sync destinations | written correctly |
| GPU never executing anything | stamps in memory, ring dumps | the GPU wrote every stamp it was given |
| AUX table not configured | `AUXT_LO (0x4200)` | `0x40008000`, configured |
| URB size | arithmetic on Gen12 (4 KB/bank × 16 ways) | adequate |
| Throttling / DPSM blocking submission | XE-EVALDIAG, CSSTATE | throttling always 0; DPSM awake |
| CSB events mis-decoded | XE-CSBDIAG | all decoded correctly |
| GPU DMA corrupting memory | `-ngreenengineinert` | same panic with the GPU idle |
| "stamp 3 (gpu_stamp=2)" = GPU hang | HANGCHECK timing | it precedes engine start; boot-ordering artefact |
| Shader compilation failing in WindowServer | libxedrv v14 pipeline log | 11 pipelines, 0 failures |
| Texture uploads, copies, atlases | `xe-f16test`, `xe-atlastest` | correct (64/128 bpp) |
| Mipmap generation | `xe-miptest` | CPU and GPU mips correct |
| Linear textures over buffers | `xe-buftex`, `xe-bufoff` | correct at offsets 0 and 4096 |
| Vertex formats | `xe-vfmttest` | all correct, including UInt1010102Norm |
| Framebuffer fetch | `xe-fbfetch`, `xe-nofetch` | correct; forcing it off changed nothing |
| Texture-buffer alignment reporting | `xe-align` (force 256) | no change |
| IconServices output | `xe-icontest` | correct bitmaps |
| CVPixelBuffer / pixel transfer | `xe-pbtest`, `xe-xfertest` | work in isolation |
| L3ALLOC, PSMI, Tile Cache Flush, SIMD32 Blit3D, render compression, POSH_LRCA, forcewake | the experiments above | not the causes of the hangs they were aimed at |
| Metal function cache corrupting Chrome's shaders | redirected empty cache; cache-hit vs fresh-compile pipeline tests | identical output; not the cause |
| Draw parameters (baseVertex/baseInstance, strips, restart) | `baseinst`, `multidraw`, `rastdraw`, `restart` tests | all correct |
| Frame-buffer compression during boot | BOOTDIAG register log | FBC never enabled |
| Stencil corruption without a shared 64 KB block | standalone stencil test variants | 16 bpp colour target or stencil in its own block: always correct |
| HDA register reads having side effects | HDA spec / Linux register review before M1 | no read-to-clear registers; SoundWire FIFO window kept unreadable |
| DSP power or reset sequencing (M2) | ADSPCS readback | identical to Linux; the missing piece was the DMA stream |
