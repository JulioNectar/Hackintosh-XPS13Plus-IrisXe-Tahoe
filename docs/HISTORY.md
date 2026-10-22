# Project history

How the Iris Xe of a Dell XPS 13 Plus 9320 (i7-1360P, Raptor Lake-P, `8086:A7A0`) went from a 1024×768 firmware
framebuffer to a Metal-accelerated macOS Tahoe 26.7.1 desktop, running Apple's Tiger Lake driver (GPUDriversIntel
16.0.32) through a modified NootedGreen plus user-space patches.

This is the long version. For the current state read [STATUS.md](STATUS.md); for the patches as they are now read
[KERNEL-PATCHES.md](KERNEL-PATCHES.md), [METAL.md](METAL.md) and the full reference [CATALOG.md](CATALOG.md); every
abandoned approach is collected in [EXPERIMENTS.md](EXPERIMENTS.md).

The work was done by the owner of the laptop ("the user") pair-programming with Claude (Anthropic) ("the assistant"),
over about a week in October 2026. From 21 Oct the work widened from graphics to the rest of the laptop (Wi-Fi,
boot screen, sleep, touchpad haptics and, from 21–22 Oct, the internal speakers: Phases 22–25).

## Conventions

- **Dates and times** are October 2026, in the local (Pacific) clock of the Tahoe install, which is what macOS logs,
  panic files and the `live-YYYYMMDD-HHMMSS` diagnostic folders use. The working notes used two other clocks (UTC
  session timestamps, and a Sequoia install whose headers ran a few hours ahead); they were converted. Times are
  approximate to a few minutes, and the order of a few entries on 19 Oct is reconstructed from boot logs.
- **Offsets** are file offsets in Apple's 16.0.32 binaries: "tglk" or "HW kext" = `AppleIntelTGLGraphics`, "FB kext" =
  `AppleIntelTGLGraphicsFramebuffer`, "MTL driver" = `AppleIntelTGLGraphicsMTLDriver`. IOAF2 = Tahoe's
  `IOAcceleratorFamily2` (25G241). Kernel patches are applied by pattern search, so the offsets are informational.
- **Names.** Kernel patches written for this project are tagged `XE-*` (or `xe/*` in the source) and most are enabled
  by `-ngreenxefix`. Patches inherited from the NootedGreen lineage are `V1…V509`. Metal driver builds are `v1…v12`,
  then `MTL13…MTL18`. The user-space adapter is `libxedrv` (v7…v17). The audio kext `AudShim` was built in milestones
  `M1…M6` (versions 0.1.0…0.8.0).
- **Uncertain points** are marked *(uncertain)*. Hypotheses that were later disproved are labelled as such.
- Development-tree paths (`build/…`, `nooted/tahoe/…`) are from the working tree the repository was later cut from.
  The repository equivalents are in `src/`, `macos/` and `tools/`.

## Hardware and baseline

| Item | Value |
|---|---|
| Machine | Dell XPS 13 Plus 9320, OpenCore. During most of the work the EFI lived on a USB volume; it was copied to the internal ESP on 20 Oct. |
| CPU | i7-1360P (Raptor Lake-P). E-cores disabled in the BIOS, so macOS sees 4 cores / 8 threads. |
| GPU | Iris Xe, `8086:A7A0`: Xe-LP Gen12 "RPL-P", 96 EU in 6 DSS, display IP version 13 ("display 13"). Spoofed as Tiger Lake `9A49`. |
| Panel | 3840×2400 eDP, 4-lane HBR3 |
| macOS | Sequoia 15.8.1 for the first, self-written stack; Tahoe 26.7.1 (25G241) on an external SSD from 16 Oct; Tahoe clean-installed on the internal SSD on 20 Oct. The Sequoia install was used for most of the work to read Tahoe's logs after each test boot. |
| Other kexts | itlwm + HeliPort (Wi-Fi, Intel AX211 CNVi), IntelBluetoothFirmware, VoodooI2C in polling mode (`-vi2c-force-polling`; replaced by GPIO interrupts on 2026-10-20), WhateverGreen (still loading next to NootedGreen). |
| Constraint | Lilu/the kernel truncate boot-args at about **228 characters**. Debug arguments repeatedly had to be dropped to make room. |

## Timeline at a glance

| # | Dates | Phase | Outcome |
|---|---|---|---|
| 0 | ≤ 15–16 Oct | Own driver stack on Sequoia (XeShim, xectl, Mesa compiler) | Pixel-exact GPU rendering from user space; no way to plug it into macOS |
| 1 | 16–17 Oct | Apple's TGL driver via NootedGreen on Tahoe; display 13 | Stable native-resolution display, accelerator initialises, desktop black |
| 2 | 17 Oct 21:17 → 18 Oct 01:30 | DisplayPipe and boot-mode chain | DisplayPipe created; next wall: "Failed to create MetalDevice" |
| 3 | 18 Oct 01:00 → 02:30 | Making Metal find the driver | MTLDevice created; compiler plugin wrong / AMFI |
| 4 | 18 Oct 02:30 → 04:00 | Compiler sandbox and plugin registration (MTL v2–v5) | Plugin registered via `PrivateFrameworks/../Caches/xe.bundle` |
| 5 | 18 Oct 09:30 → 11:10 | LLVM bridge and compiler adapter | First shaders compiled end to end |
| 6 | 18 Oct 11:15 → 13:30 | Descriptor ABI: MTL v6, v7, libxedrv v7 | Driver generates pipeline state |
| 7 | 18 Oct 12:15 → 16:00 | Panic series; DMA hypothesis refuted | Teardown panics are not caused by GPU execution |
| 8 | 18 Oct 16:15 → 18:05 | libxedrv v8/v9, MTL v10 (POSH), bridge v2 | Every shader compiles; command buffers never complete |
| 9 | 18 Oct 18:50 → 23:10 | Making the GPU execute | Verbose console found corrupting contexts; first GPU work done |
| 10 | 18 Oct 23:10 → 19 Oct 01:35 | Fast clear, flips, formats, pools | Flips reach the screen; blit hangs gone |
| 11 | 19 Oct 01:45 → 08:05 | Where the work goes missing | Fake blit success removed; Apple logo drawn by the GPU |
| 12 | 19 Oct 08:05 → 12:30 | The lost execlist submission | POSH contexts wedge the RCS → XE-POSHCTX; login screen drawn |
| 13 | 19 Oct 12:30 → 14:05 | Clears, compression, VS state (MTL13–17) | First login to the desktop; windows with content |
| 14 | 19 Oct 14:05 → 17:20 | The Blit3D context | Blit3D kernels were never uploaded → XE-BLITSCRATCH2 |
| 15 | 19 Oct 17:20 → 21:20 | Stabilisation | CSB guard, QUIETLOG, PAGEONGUARD, SAMETAIL, one change per boot |
| 16 | 19 Oct 21:20 → 23:30 | c01dbead hangs, brightness, depth resolve | XE-NODEPTH: stable desktop, work moves onto Tahoe itself |
| 17 | 19 Oct 23:20 → 20 Oct 14:30 | Living on Tahoe; clean reinstall | GPU frequency fixed; Tahoe on the internal SSD |
| 18 | 20 Oct 14:30 → 17:30 | Clean-install regressions; repository | PLANES2, NOGVA, libxedrv v15 icons fix, reproducible build |
| 19 | 20 Oct, after the repository | Audit and new problems | MTL v11 byte bug → MTL18 (in test); display sleep; colour-profile panics |
| 20 | 20 Oct evening → 21 Oct | Hardware video decode | H.264 decodes on VDBOX, bit-exact; HEVC plugin fixed |
| 21 | 21 Oct | Stability, touch and E-cores | Fast shutdown, 16 threads, touchscreen gestures |
| 22 | 21 Oct | Chrome blits, lock-screen clock, `w30r` | libxedrv v16 (blit ordering) and v17 (Stencil8 alignment); XR formats are Apple-silicon-only |
| 23 | 21 Oct afternoon | Boot screen and boot speed | `Resolution=Max` + `gfxrst=1` + XE-BOOTHOLD: clean boot, no 20 s stall |
| 24 | 21–22 Oct | Wi-Fi, haptics, sleep, power, side investigations | Native Wi-Fi; vibration strength; no system sleep, lid → display off |
| 25 | 21–22 Oct | Internal speakers (AudShim M1 → M6) | SOF firmware, SoundWire and RT1316 from our own kext; speakers are a macOS output device |

---

## Phase 0 — Own kernel shim and GPU stack on Sequoia (≤ 15–16 Oct)

**Goal.** Drive the Iris Xe from macOS without any Apple GPU driver, in the spirit of Linux UIO or Asahi's
user-space bring-up, and grow that into a desktop compositor.

**Components.**

| Component | Role |
|---|---|
| `XeShim` (kext, `local.xe.XeShim`) | Minimal IOKit user client. Maps BAR0 (MMIO + GGTT) and BAR2 (aperture), allocates physically contiguous DMA buffers, reads PCI config. Never touches GPU registers itself. Only the console user may open it. |
| `xectl` and `xe_bcs.c` / `xe_gfx.h` | User-space driver: execlist submission (a fresh Gen12 context per run with restore inhibited), PPGTT, PAT, per-engine reset, GPU objects, state heap, surface states, binding tables. |
| `cc/` (`xe_cc`) | Mesa's Intel backend (`brw_compile`) built on macOS with two local patches: NIR assembled in C, Gen12 code out. |
| `XeFB` | Own `IOFramebuffer` meant to replace the GOP `IONDRVFramebuffer`. |
| `XeICLFix`, `XeICLBridge`, `XeProbe` | Experiments with matching Apple's Ice Lake driver to the GPU (internals not recorded). |

**Milestones** (all verified pixel-exact against CPU references):

| Milestone | Result |
|---|---|
| M0–M4 | GPU access, registers, GGTT, first command on BCS0, blitter rectangles on screen, 4-level PPGTT |
| D1 | Own scanout on pipe A (1920×1200 scaled 2×) |
| M5a–c | RCS0 commands; first EU kernel (hand-written, `GPGPU_WALKER`); first 3D `RECTLIST` with its own PS (1536/1536 pixels) |
| M6a/b | Alpha blending, texture sampling |
| M7a–c | Mesa compiler working on macOS; VS+PS, TRILIST, hardware viewport |
| M8a/b | Two-layer composite with UBO tint; 5×5 box-blur compute shader (12288/12288, max error 0) |
| M9 | "Frosted glass" compositor: 36864 + 12288 pixels correct, also drawn on the GOP scanout |
| later | Push constants, timeline fences, depth, BO lifecycle, format matrix, GLSL 4.50 via Mesa, 4× MSAA; H.264 decode on VCS0 |

**Pitfalls recorded.** Gen12 does not interlock dependencies in the in-order pipe (`{@N}` / `{$n}` annotations are
needed); EOT payload must be in r112–r127; 3DSTATE pointers are relative to `STATE_BASE_ADDRESS` and MOCS must never
be 0; Mesa returns 0-byte programs unless the `DEBUG_*_SIMD*` bits are set; an aborted test leaves the GGTT mapped
(`xe-gtt-clean` restores the firmware scratch page); several store sends in a row in a PS hung the GPU.

**State on 16 Oct.** The desktop was still the GOP framebuffer at 1024×768 stretched to the panel; there was no
IOAccelerator and no MTLDevice.

**Why it stopped.**
- The `XeFB` takeover crashed in `postOpen`.
- Two assistant answers analysing Apple's IOGraphics internals were cut by an automatic content filter. The project
  chose to close that line (framebuffer swap, ICL driver reuse, IOGraphics analysis) rather than work around it.
- The missing upper half — an IOAccelerator and a Metal driver macOS would use — would have had to be written from
  scratch. From 16 Oct all work moved to Apple's TGL driver, which already has an accelerator, a framebuffer and a
  Metal driver *(the motivation is inferred; no explicit decision message survives)*.
- XeShim and `xectl` stayed in use as diagnostic tools for the rest of the project.

---

## Phase 1 — Apple's Tiger Lake driver on Tahoe; display 13 (16–17 Oct)

**Setup.**
- Apple's TGL kexts from GPUDriversIntel 16.0.32 (`AppleIntelTGLGraphics`, `AppleIntelTGLGraphicsFramebuffer`, the
  GL/MTL/VA bundles and `AppleIntelGraphicsShared.bundle`; the donor is the `sle_Internal` set of the NootedGreen
  project) installed in `/Library/Extensions`, with bundle IDs renamed to `com.xxxxx.*`.
- GPU spoofed to TGL `9A49` (`IOPCIPrimaryMatch 0x9a498086` in the `Gen7` personality). Scripts
  `ativar-acelerador.sh` / `desativar-acelerador.sh` turned the accelerator personality on and off.
- NootedGreen (StezzaPilot's Lilu plugin, carrying the community V-numbered fixes up to V509) forked from upstream
  `95f8189`. The first Tahoe changes: `KernelVersion::Tahoe` in `kern_start.cpp` (the plugin would not load on macOS
  26 otherwise), `-ngreenkeepefi`, and a linear stride computed from the pipe width.
- A root LaunchDaemon `xe-bootdiag` collected, about 150 s after each boot, kernel and WindowServer logs, ioreg,
  kextstat, display and GGTT registers through XeShim, a 128 MB aperture dump and a plane CRC.

**Display problems and fixes** (each proved by plane CRC and register reads):

| Problem | Evidence | Fix |
|---|---|---|
| Upstream writes a register dump of the original author's 2560×1600 2-lane panel into transcoder A | panel black on the 3840×2400 4-lane HBR3 panel | `-ngreenkeepefi`: keep the GOP-programmed timings/M-N/PPS when `PIPECONF_A` bit 31 shows the pipe running |
| Upstream forces a linear plane with `PLANE_STRIDE=0xa0` (2560 px) | bands on screen | stride from `PIPE_SRCSZ`; then `-ngreenxtile` (keep Apple's X-tiling, stride `0x1e`), `ngreenstride=N` and `-ngreenmax2560` experiments |
| Display 13 reads tiled planes through the DPT | noise | `CHICKEN_MISC_2` (`0x42084`) bit 30, as i915 `intel_dpt_configure` |
| The TGL driver powers off DBUF slice S1 | plane fetch turns into noise | keep `DBUF_CTL_S1` (`0x44FE8`) bit 31 |
| TGL enables a 10-bit post-CSC gamma (`GAMMA_MODE_A 0x4A480`) in a layout display 13 reads wrong | degraded text and colours | force 0 (bypass); proved live on 17 Oct ~20:00 |
| DMC firmware choice | — | `ngreen-dmc=adlp` (ADL-P DMC v2.16) kept after `dmcskip`/`dmctgl` test configs |

These were folded into NootedGreen under the new master switch **`-ngreenxefix`**.

**Accelerator.** With the **PSMI** workaround (Wa_1607297627: masked write to `RING_PSMI_CTL` `0x2050`, bits 12|7;
upstream had it as dead code after a `return`), the TGL `IntelAccelerator` initialised without a "timeout waiting for
IntelAccelerator stamp". The desktop stayed black with the accelerator on, attributed to the missing Metal path.

**State on 17 Oct evening.** Two EFI states: "display only" (stable, `-ngreentglfb … -ngreendp0 -ngreenkeepefi
-ngreenxefix`) and "accelerator test" (`-ngreentglwithgfx -allow3d -disablegfxfirmware` instead of `-ngreentglfb`).
The user had chosen 1504×940 HiDPI. Brightness keys were not ported.

---

## Phase 2 — DisplayPipe, WindowServer abort loop and the boot-mode chain (17 Oct 21:17 → 18 Oct ~01:30)

**Goal.** Get WindowServer to start with the accelerator on.

### 2.1 `IOPresentment Interface creation error 0x2005` → remove `-ngreendp0`

- **Symptom.** GPUWrangler registered `8086.9a49` with `IntelAccelerator`; WindowServer opened `IGAccel2DContext`
  but logged `IOPresentment Interface creation error 0x2005` for each of its 3 framebuffers; black screen.
- **Evidence.** ioreg: `IOAccelDisplayPipeCapabilities = {DisplayPipeSupported=0, TransactionsSupported=0}`. Apple's
  TGL and ICL plists both say true. NootedGreen's `-ngreendp0` forces 0.
- **Experiment.** Remove `-ngreendp0`. **Result:** the 0x2005 error disappeared, and WindowServer crash-looped
  (`SIGABRT`, ~28 crashes in 14 min): `__assert_rtn ← CoreDisplay_CreateDisplayForCGXDisplayDevice ←
  UpdateCGXDisplayDevice`. FB1 got mode `0x80001000` (3840×2400); FB2/FB3 asked for mode `0x0`,
  `IOFBGetDisplayModeInformation()` failed, "capabilities with no devices", abort. With dp0, all three used
  `0x80001016` (3008×1880).

### 2.2 `-ngreentglfb` with the accelerator → panic

- To separate the effects, boot-args were switched to `-ngreentglfb -allow3d -disablegfxfirmware`. **Result:** kernel
  panic, then boots stopping at random verbose lines; only `AAPL,PanicInfo` fragments in NVRAM.
- **Cause.** NootedGreen registers its accelerator hooks *only* with `-ngreentglwithgfx`; with `-ngreentglfb` the
  accelerator runs unpatched. **Rule: never combine `-ngreentglfb` with the accelerator.**

### 2.3 Live boot logging

- bootdiag ran only after 150 s, so early deaths left nothing. `xe-bootdiag` was extended to dump `dmesg` every 3 s
  into `live-*/dmesg.txt` with `sync` and a heartbeat.
- Pitfalls: after 150 s dmesg is flooded by `xectl` lines; in zsh `log` is a builtin (use `/usr/bin/log`); a crashed
  WindowServer's assertion text is in ReportCrash's "ASI" field.

### 2.4 Hunting `fCurrentMode = 0xfffffffb`

With `IGLogLevel=8`, `IOAccelDisplayPipe::init_framebuffer_resource` failed with "getPixelInformation for framebuffer 0
failed" while the FB reported `fCurrentMode = 0xfffffffb` (`kIOFBBootProgrammable`).

1. **Route on `AppleIntelFramebuffer::getCurrentDisplayMode`** returning `0x80001000` (new boot-arg
   `-ngreenfixbootmode`). No effect — and the discovery that **boot-args were truncated at ~228 characters**: the kernel
   saw `…-ngreenfix`. `liludump`/`liludbuf` were dropped (207 characters).
2. Still no effect, and no log line. Load-time SYSLOGs were lost because `IGLogLevel=8` floods the msgbuf before logd
   drains it. Technique adopted: keep counters and print them in the late `HANGCHECK` block.
3. Counters: `calls=0`. A route on `getPixelInformation` (commented out upstream) was enabled: also `calls=0`. State
   counters showed the routes **installed but never invoked**. **Lesson: in this kext, Lilu routes on these functions
   do not fire; `LookupPatchPlus` byte patches do.**
4. **Byte patch** of the single `movl $0xfffffffb, 0x4250(%rbx)` at FB `0x53de3` (`c7 83 50 42 00 00 fb ff ff ff` →
   immediate `0x80001000`). Result: `FBMODE patch: tried=1 ok=1`, mode now `0x80001000` — and the error remained. The
   −5 was a **symptom**. (The patch survives as opt-in `-ngreenfixbootpatch`.)

### 2.5 Root cause: no detailed-timings table when the DisplayPipe is created

From the disassembly: `init_framebuffer_resource` → `getPixelInformation(fb0)` → `validateDisplayMode`, which requires
`fDetailedTimings` (`fb+0x4260`, set by `setDetailedTimings` / property `IOFBDetailedTimings`) to be non-NULL and the mode
ID to be ≥ `0x10000`; otherwise `0xe00002c2`. The table arrives about 2 s later. DisplayPipe creation is attempted once;
its failure makes CoreDisplay mark the main display offline and WindowServer abort. dp0 avoided all this by never
creating a DisplayPipe.

Options: (a) supply the table early, (b) patch `validateDisplayMode`, (c) delay the pipe. **(a) was chosen:** the
**`fbTimingsWorker`**, a `thread_call` every 50 ms for up to 5 min, finds FB0 of `AppleIntelFramebufferDiags` (online,
`+0x4260 == NULL`), builds the timing the EFI left in the CRTC with the driver's own
`AppleIntelBaseController::hwCRTCToIODetailedTimingInformation`, sets `IOFBDetailedTimings` and stores the pointer.

- First result (00:41): `built=1 done=1 crtc=3840x2400`, entry matched (pixel clock 888750000), but `Mode=0`: on a match
  the driver copies the entry's **first dword** into `fCurrentMode`, and real EDID tables carry `0x80001000+` there.
- Fix: write `0x80001000` into dword 0. **Result (01:04): the `init_framebuffer_resource` failure disappeared and the
  DisplayPipe is created.**

### 2.6 Next wall: `Failed to create MetalDevice`

CoreDisplay was disassembled from the dyld shared cache by attaching `lldb -b -p <pid> -o 'disassemble -n SYMBOL'` to a
small self-built process that `dlopen`s the framework (the system python3 is protected). ReportCrash confirmed:

```
Assertion failed: (Failed to create MetalDevice (non-null)), function Display, file …/CoreDisplay/Display/Display.cpp, line 3228
```

With a DisplayPipe, CoreDisplay **requires** a MetalDevice for the accelerator. From here on the problem was Metal.

---

## Phase 3 — Making Metal find the driver (18 Oct ~01:00 → 02:30)

- **Metal probe.** `metalprobe` (run by bootdiag) showed `IntelAccelerator` with `MetalPluginName =
  AppleIntelTGLGraphicsMTLDriver`, the bundle loading from `/Library/Extensions` with `NSBundle` (`load=1`), and
  `MTLCopyAllDevices()` returning **0**, with no reason logged. An alternative hypothesis was recorded from the kernel
  log (`waitForStamp: stamp 3, gpu_stamp=2`, `INSTDONE=0xfffffffe`: RPL not executing TGL commands); it was re-examined
  in Phase 7.
- **External reference.** A from-scratch RPL-U driver project (`MyIntelGPU.kext`) was read (documents only; no licence,
  binaries not run). Its desktop runs on `AppleSoftwareRenderer` with no MTLDevice, consistent with the finding above;
  the string `Failed to find bundle for accelerator bundle named: %s errno: %d` suggested a lookup by path.
- **`/Library/GPUBundles`.** The dyld cache knows this (empty, pre-existing) directory. Copying
  `AppleIntelTGLGraphicsMTLDriver.bundle` and `AppleIntelGraphicsShared.bundle` there worked (boot 01:44): WindowServer
  **created the MTLDevice** and reached `CA::OGL::MetalContext`, with the mouse pointer briefly visible, then aborted:
  `Assertion failed: (0 && "Unsupported platform"), function getICBFile, MTLIntelCompiler.cpp:412`.
- **Cause.** The MTL driver hard-codes
  `/System/Library/Extensions/AppleIntelGraphicsShared.bundle/Contents/MacOS/libMTLIGCCompilerPlugin.dylib` — **Tahoe's**
  IGC plugin, which no longer supports TGL.
- **MTL v1** (`mtl-patched`): same-length path `/Library/GPUBundles////////AppleIntelGraphicsShared.bundle/…`, ad-hoc
  re-signed. Result (02:06): `Library Validation failed: Rejecting '…' (Team ID: none, platform: no) for process
  'WindowServer' (platform: yes), reason: mapping process is a platform binary`.
- **AMFI.** After asking the user, `amfi_get_out_of_my_way=1` was added (`msgbuf` dropped to stay at 218 characters).
  Result (02:16): the bundle loads, `MTLCompilerService` compiles the first stage (AIR 2.7 specialization, `Compilation
  SUCCESS`), then `Failed to dlopen compiler plugin`: the TGL-era plugin links
  `GPUCompiler.framework/Versions/3902/Libraries/libLLVM.dylib`; Tahoe only has `32023`, which lacks the old
  `llvm::parseIRFile`. The user saw the mouse for under a second, then black.
- **Decision.** Offered "software desktop + video decode on VDBOX", "continue Metal on the GPU" or "pause and restore",
  the user chose **Metal on the GPU**.

---

## Phase 4 — Compiler location, sandbox and plugin registration (18 Oct ~02:30 → 04:00)

**Hybrid compiler.** Tahoe's plugin `getICBFile` (`~0x4eab`) uses `dladdr`: loaded from a path containing
`/System/Library/Extensions` it accepts only ICL (`0x1c`, `0x1d`) and KBL (`0x13`, `0x14`); elsewhere it uses
bundle-relative `runtime.gpuencode.split`, `runtime.igc64.split` and `libigc.dylib`. Tahoe's `libigc` has no Gen12;
the TGL-era `libigc` has gen12lp and depends only on libSystem/libc++. The "hybrid" bundle = Tahoe plugin + TGL
`libigc` + TGL `.split` files, installed in `/Library/GPUBundles/AppleIntelGraphicsShared.bundle`.

**Sandbox.** Result (02:45): `Sandbox: MTLCompilerService deny file-read-data /Library/GPUBundles/…`. The profile
`com.apple.mtlcompilerservice.sb` allows reads under `/System` and `file-map-executable` under `/System/Library`;
`/Library/GPUBundles` only with `(system-attribute apple-internal)`.
- `csr-active-config` `03 0a 00 00` → `13 0a 00 00` (Apple Internal bit): **no effect** — `csrutil` still reports
  "Apple Internal: disabled"; the bit is discarded at boot.
- **Plan B:** `/System/Library/Caches` is a **firmlink to the writable Data volume** that still sits under
  `/System/Library`, so the sandbox accepts it.

**Plugin registration.** `MTLCompilerPluginInterface::init` in `MTLCompiler.framework` imposes two rules on the plugin
path, discovered one at a time:

| Version | Plugin path in the driver | Result |
|---|---|---|
| **v2** (03:03) | `/System/Library/Caches/xe//AppleIntelGraphicsShared.bundle/…` | Sandbox OK, first stage OK (573 ms); then `Compiler plugin <path> is not registered with the MTLCompilerObject` → WindowServer `MTLRenderPipelineState creation failed for device Intel(R) TGL Unknown` → `OS_REASON_COREANIMATION \| Metal failed to build render pipeline` |
| **v3** (03:18) | same, without the double slash, NUL-padded. Hypothesis: `//` broke a string match (**disproved**) | same error |
| — | disassembly: `validPathPrefixes` = {`/System/Library/Extensions/`, `/System/Library/PrivateFrameworks/`, `/Library/GPUBundles/`}. `/Library/GPUBundles` passes the prefix but fails the sandbox; `/System/Library/Caches` passes the sandbox but fails the prefix | — |
| **v4** (03:36) | `/System/Library/PrivateFrameworks/../Caches/xe/Contents/MacOS/libMTLIGCCompilerPlugin.dylib` — a textually valid prefix whose `..` resolves into the writable Caches firmlink | same error (boot 03:39) |
| — | second rule, `validPluginSuffixes`: the path must also **contain** `.framework` or `.bundle` (`std::string::find`) | — |
| **v5** (03:53) | **`/System/Library/PrivateFrameworks/../Caches/xe.bundle/Contents/MacOS/libMTLIGCCompilerPlugin.dylib`** (98 bytes): valid prefix, contains `.bundle`, does not contain `/System/Library/Extensions` (which would make `getICBFile` treat it as a system plugin) | **Registered** (boot 03:57), no sandbox denials |

This path is unchanged in every later driver build, and the compiler has lived in `/System/Library/Caches/xe.bundle`
ever since. The assistant's tooling refused to run the `..` patch even after the user authorised it, so the user ran
the patch and install scripts in Terminal on their own. Writing the plugin into the sealed system volume (`bless
--create-snapshot`) was considered and rejected as riskier.

**Next wall** (v5 + hybrid): `Assertion failed: (m_pReadBitcodeFn), function openIGCLibrary, file
MTLIntelCompiler.cpp, line 275`. Tahoe's plugin needs `libigc` exports (`readBitcode`, `readBitcodeWithContext`,
`loadSplitBuiltinModuleFromFile`, `deleteContext`, `getTargetTripleOSMajor`) that the TGL `libigc` lacks; it has 13
TGL-only exports instead. **No `libigc` on the machine has both the new API and Gen12.** A forwarding `libigc` shim was
started and dropped as too risky. Measured alternative: the **old** TGL plugin against Tahoe's libLLVM 32023 — of its
32 LLVM imports, 29 resolve; missing are `llvm::parseIRFile(StringRef, SMDiagnostic&, LLVMContext&, bool)` (signature
changed), `llvm::raw_string_ostream::~raw_string_ostream()` (D1) and `llvm::WriteBitcodeToFile(const Module*, …)` (now
`const Module&`, ABI-identical).

---

## Phase 5 — LLVM bridge and compiler adapter (18 Oct ~09:30 → 11:10)

Each iteration: the user installs with sudo in Terminal, reboots, waits 2–3 minutes, and comes back via Safe Mode — the
only way to reach the assistant while the screen was black.

| Step | Change | Result |
|---|---|---|
| 1 | `libLLVMbridge.dylib`: re-exports Tahoe libLLVM 32023 (stub generated from `dyld_info -exports`) and implements the 3 missing symbols. The old plugin's LLVM load command points at the bridge; package `xe-old` | `MTLSpecializeFunctionToArchive` succeeds (3–22 ms): first shader through `libigc`. `MTLBuildFunctions` requests never return; WindowServer aborts in `MetalShader::CopyPipelineState` |
| 2 | Diagnosis: Tahoe's MTLCompiler calls a **new entry point**, `MTLCompilerBuildRequestWithSerializedBitcode` (serialized bitcode), while the old plugin exports only `MTLCompilerBuildRequestWithOptions` (`llvm::Module*`). New **adapter** `libMTLIGCCompilerPlugin.dylib` re-exports the old plugin (renamed `…Old.dylib`), parses the bitcode with the LLVM C API and calls the old function. Argument mapping from disassembly: old `r9` = first new stack argument, remaining old stack = new stack shifted by 8 | `parse bitcode -> bad=0`, then an assert in `getReducedICBBuiltinModule` |
| 3 | The old plugin hard-codes 11 paths under `/System/Library/Extensions/AppleIntel…`; all binary-patched to `/System/Library/Caches/xe.bundle/Contents/…` | next crash |
| 4 | SIGSEGV in new `raw_ostream::write` from `WriteBitcodeToFile`: `raw_ostream` field offsets differ and the old plugin builds `raw_string_ostream` inline. The bridge now writes bitcode via `LLVMWriteBitcodeToMemoryBuffer` and appends it to the old stream's `std::string` at `+0x28`; D1 is a no-op | fragments compile |
| 5 | Fragment shaders: `Compilation SUCCESS (MTLBuildFunctions)` in 13–18 ms — **the first pipeline compile**. Vertex shaders: SIGSEGV in `detectPostTessellatedVertexFunction`, `getNamedMetadata("air.vertex")` null. Cause: `Twine` layout (kinds at `+0x10/+0x11` old, `+0x20/+0x21` new). The bridge interposes `Module::getNamedMetadata(Twine)` and `Value::setName(Twine)` through `std::string` | vertex OK |
| 6 | End to end | `MTLBuildFunctions` SUCCESS for VS and PS. **The wall moved into the Intel MTL driver**: `-[_MTLFunctionInternal layouts]: unrecognized selector` inside `generate3DStateVF` |

(One installer was first run through the assistant's non-TTY shell and failed with "a terminal is required to read the
password"; from then on the user ran every sudo command in their own Terminal.)

---

## Phase 6 — Descriptor ABI: MTL v6, v7 and libxedrv v7 (18 Oct ~11:15 → 13:30)

- **Cause.** On Tahoe `MTLRenderPipelineDescriptorPrivate` gained two 8-byte fields (`shaderValidation`,
  `shaderValidationState`), so everything from `0xa0` moved by `+0x10` (measured on a live descriptor: `sampleCount`
  0xa8→0xb8, `fragmentFunction` 0xf8→0x108, `vertexDescriptor` 0x100→0x110). The driver read the function where the
  vertex descriptor should be.
- **MTL v6** (11:21): 13 displacement patches in `generateSampleMask:`, `generate3DStatePS:`, `generate3DStateWM:`,
  `generate3DStateSBE:`, `generate3DStateVF:posh:` and `generate3DStateClip:`. Result: past VF, then SIGSEGV in
  `generateBlendState` (colour-attachment pixel format decoded wrongly). Separately, old `libigc` crashed in
  `LoadBuiltinModule → PassManager::run` on the POSH (position-only shader) vertex path.
- **MTL v7**: v6 reverted; v5 + an `LC_LOAD_DYLIB` for **`libxedrv.dylib`** (Objective-C, built with `-fno-objc-arc`).
  `isPoshEnabled()` in the old plugin patched to false (`0f 9f c0` → `31 c0 90`).
- **Offline harness.** At the user's request ("can't you get ahead of the work?"), `drvtest/h2…h6.m` loads the driver in
  a normal process, builds real Tahoe descriptors and calls `generate*` directly. It reproduced the WindowServer crash
  and showed: the driver's ObjC **type encodings contain the exact old struct layouts**; `alphaToCoverage`/`alphaToOne`
  grew from 1 to 2 bits; the real colour-attachment bit positions (Tahoe: blend@0, rgbOp@2, aOp@5, srcRGB@8, srcA@13,
  dstRGB@18, dstA@23, writeMask@32, pixelFormat@42; old: blend@0, rgbOp@1, aOp@4, srcRGB@7, srcA@12, dstRGB@17, dstA@22,
  writeMask@27, logicOp@31–35, pixelFormat@36). The first guess (pixelFormat 36→38) was wrong; the harness caught it.
- **Sandbox again.** `deny file-map-executable /System/Library/Caches/xe.bundle/…/libxedrv.dylib`: WindowServer may map
  executables only from `/Library/GPUBundles`. libxedrv moved into the MTL bundle's `Contents/MacOS`.
- **Conversion scope.** Converting in place during the whole `init` broke Metal's own superclass init (`setLabel:` →
  `copyWithZone:` unrecognized selector). New technique: swizzle `_descriptorPrivate` and return an old-layout
  **shadow copy only when the caller is the driver** (`dladdr` on the return address).
- **Cross-check.** IntelIGPU-Tahoe (GPL-3.0, Alder Lake-P, Tahoe 25G83) uses the same TGL 16.0.32 driver (identical
  UUID) and projects `_descriptorPrivate` for driver callers with the same offsets and bits; it also adds selectors the
  old driver lacks. Its Metal.framework differs from 25G241, so it could not be used directly. Ideas ported (own code):
  texture views with `resourceIndex`, `newBufferWithDescriptor:`, global-memory stubs, "not supported" answers for mesh
  shaders, binary archives and ICBs.

---

## Phase 7 — Instrumented compiler, panic series, and the DMA hypothesis (18 Oct ~12:15 → 16:00)

**Compiler instrumentation.** The adapter gained base64 dumps of bitcode/input/reply to the unified log, a 250 ms
sleep so logs reach disk, and `sigsetjmp` containment (a `libigc` crash returns error 1, logged as "FALHA CONTIDA").
An offline replay tool (`lt4/rp`) found two bugs:
- the input struct length is in `rdx` (216 bytes; the first dumps were truncated at 1 KB);
- **uninitialised helper:** the old plugin keeps at `compiler+0x10` a helper that older macOS installed via
  `MTLCompilerSetHelper`. Tahoe never calls it, so the plugin read heap garbage and compiles succeeded or failed at
  random. The adapter now installs a fake helper whose provider returns a null ICB module.

With that, 3 of 4 captured shaders reproduce the service's reply sizes exactly (19286, 19708, 19222 bytes). One
colour-management shader (LUT + colour matrix, 7248 bytes) compiled in the service but not offline; never explained.
The bridge also adopted MTLCompiler's own `getSerializedModule(Module*, 3902)` to emit old-format bitcode (fixed offset
`0x7FB06`; see Phase 8).

**Panic series** — all after a WindowServer or ReportCrash death with the accelerator in use:

| Time | Panic |
|---|---|
| 12:15:46 | `Possible memory corruption: pmap_pv_remove(...): null pv_list` in `task_terminate_internal → vm_map_destroy` |
| 12:34:59 | same mechanism; that boot produced the first real Gen12 binaries (`r=0`, replies 19–23 KB); a third compile crashed in old `libigc`, and Metal crashed in `-[MTLCompiler releaseHashMaps:]` |
| 12:49:20 | null dereference in `kqueue_threadreq_bind` inside ReportCrash |
| 13:09:41 | NMIPI spinlock timeout in `kprintf`/`os_log` inside `IOService::newUserClient` (WindowServer opening the accelerator) — `IGLogLevel=8` flooding. **Fix: `IGLogLevel=3`** |
| 13:14:32 | ReportCrash `vm_map_enter` null |
| 13:26:33 | `pmap_recycle_page: page … is referenced @pmap.c:1511` |
| 14:01:07 | `metadata list corruption in zone kalloc.type5.128` |

Before the 14:01 panic, `releaseHashMaps:` was replaced by a logging no-op: the map was **valid** (5 sane entries), and
WindowServer instead died of heap corruption (`nanov2_guard_corruption_detected`, `objc_release` on a pointer with one
flipped bit, `0x5ff8…` vs `0x7ff8…`). **Hypothesis: the GPU corrupts memory by DMA** through a misconfigured context,
ring or GTT.

**The Reddit RPL-HX kext.** A much larger NootedGreen build (≈940 KB vs 425 KB, about 140 more V-fixes) was found. Its
strings show the same plan done in kernel hooks: V268/V270/V273 plugin and `.split` paths in `/Library/GPUBundles` plus
sandbox/AMFI relaxations, V271 a rebuilt **Big Sur libLLVM 3902**, V274 forcing bitcode version 3902, a user-space shim
for the new entry point, and V500–V509 (CSB, L3ALLOC, CS_CHICKEN1). **A/B test** with that binary (without our display
fixes, which it does not know): froze before user space at `deferred remaining count 2 - IOG flags 0x3 (0x51) -
Generation from SMC report as 2`. Reverted.

**`-ngreenengineinert`** (new): `startGraphicsEngine` is skipped and Apple's writes to RCS/BCS/VCS register ranges are
dropped. Result: inert mode worked (`XE-INERT`, rings `CTL=0`), WindowServer crashed 3 times in 35 s, and the kernel
**still panicked** (`pmap_recycle_page … is referenced`, 14:39) **with the GPU idle**. **The DMA hypothesis was
refuted:** the teardown panics come from how graphics memory is unmapped when a graphics process dies, not from GPU
execution. The assistant counted eight panics that day *(another note says 19, possibly counting differently)*. The
EFI went back to the "display only" state.

**Re-reading HANGCHECK.** The "stamp 3 (gpu_stamp=2)" timeout at 76–80 s **precedes** `startGraphicsEngine` (81.2 s); it
is a boot-ordering artefact that V221 already papers over, not proof of a GPU hang. One real anomaly: RCS LRCA page 1
(`GGTT[0x19]`) held a `00ffffff` fill with no LRI header despite V507's repair. Calling `xeLrcaRepair` again after
engine start ended in a WindowServer watchdog panic with the logs lost.

**Research.** The user rejected stopping or switching to VDBOX, asked for deep research, then asked to keep
going. No public working desktop acceleration on TGL/RPL with NootedGreen was found (sgiammori issues, other forks,
InsanelyMac/AMD-OSX threads, IntelIGPU-Tahoe with unresolved stalls, a Latitude 7320 Tahoe TGL report blocked by
Gen11 vs Gen12 ISA). One fork's README described the same final symptom: WindowServer active, then frozen on the last
frame about 220 ms later.

**Automating boots (`xe-auto`).** To get past ~10 manual boots a day, the assistant wrote a queue-driven agent (arm EFI,
reboot, collect 150 s of logs, next; max 40 reboots, stop after 3 crashes, `STOP` file, restore the stable EFI at the
end). Its first run hung before `launchd` and then looked for the EFI on the wrong disk; it was fixed (mount by label)
but is limited to boots that reach user space. Later (Phase 8) an empty queue made `ls` list `/`, the agent picked
`.VolumeIcon.icns` as an experiment and wrote **empty boot-args** to the EFI; fixed with a null glob and a refusal to
write empty arguments, then disabled by the user.

---

## Phase 8 — libxedrv v8/v9, MTL v10 and bridge v2 (18 Oct ~16:15 → 18:05)

- **libxedrv v8.** Compared against IntelIGPU-Tahoe's adapter, v7 had three bugs: the shift started at `0xa0` instead
  of `0xa8` (the old driver also has `shaderValidation` at `0xa0`, per its type encoding
  `MTLRenderPipelineDescriptorPrivate=@[8Q]QQQQcQQQQQcq(?=QQ)…`); hooks were installed asynchronously by a polling
  thread, so early pipelines missed them; and dynamic libraries were not rejected (Tahoe told Metal the driver
  supports them). v8 fixes all three, verifies the layout before converting (`MTLRenderPipelineDescriptorInternal` is
  0x338 bytes, `_private` at 0x30), adds 12 methods, and has a "lab mode" (`/Library/GPUBundles/xe-lab`: WindowServer
  gets no MTLDevice). **Lab mode failed:** with the accelerator present and `initWithAcceleratorPort` returning nil,
  WindowServer aborts and loops. The `releaseHashMaps` map became valid with v8. `-ngreendp0` was replaced by
  `-ngreenfixbootmode` in the boot-args for good.
- **libxedrv v9 (crash recorder).** Signal handler logging registers, symbolised `rip` and the frame chain. It showed
  `CRASH sinal=11 addr=0x600020000000` (exact page boundary) in `Hash32::addInput ← -[MTLIGRenderPipelineState
  generateSamplerDeclHash:]`, on the **POSH** path at `0x6e88` (`mPoshVS->+8`), because `MTLIGAccelDevice.bPoshDisable
  = 0` — read from the bundle's `Info.plist` `Development.PoshDisable`. POSH was already off in the compiler, so there
  was no POSH output and the driver read past the buffer.
- **MTL v10:** `Development.PoshDisable = 1`. Result (17:38): the POSH crash disappeared; WindowServer created
  pipelines. New error: `libigc` SIGSEGV (contained) on a CoreAnimation framebuffer-fetch pixel shader, then "Metal
  failed to build render pipeline". Clue: the bridge logged "prologo de getSerializedModule nao confere (03 00 eb 08)" —
  the fixed offset `0x7FB06` is wrong inside MTLCompilerService (which loads a versioned
  `MTLCompiler.framework/Versions/<N>/MTLCompiler`), so the bridge had silently fallen back to modern bitcode.
- **Bridge v2:** finds `getSerializedModule` by signature (prologue + `cmp edx,0xf3d` and `cmp edx,0xf3e`, exactly one
  candidate): `Versions/32024 +0x806b7` on Sequoia, `Versions/32023 +0x8cc2a` on Tahoe. **Result (18:04): every shader
  compiled (`r=0`), zero WindowServer crashes** — then the machine froze ~33 s later; CoreAnimation logged "synchronize
  timed out", the kernel `waitForStamp … stamp 3 (gpu_stamp=2)`. **Command buffers never completed. The blocker was now
  the GPU executing work (kernel side), no longer user space** — exactly where NootedGreen issue #20 had stopped.

---

## Phase 9 — Making the GPU execute (18 Oct ~18:50 → 23:10)

**Goal.** Find out why command buffers never complete.

### 9.1 A register sampler that survives freezes

`bootdiag` read registers only after 150 s; the machine froze earlier. **`xe-rcsdump`** was written: one process that,
through XeShim, writes RCS0/BCS0 state (TAIL/HEAD/ACTHD/IPEHR/INSTDONE/EIR/ESR/EXECLIST_STATUS/RING_MODE/CSB_PTR/CCID/
HWSP), fault and error registers once per second with `fsync`. A known-good reference existed: the Phase 0 `xe_bcs.c`.

- First run (18:50): all zeros, including forcewake acks. **GT registers read 0 in RC6 without forcewake.** Fix: take
  forcewake (`FW_GT 0xA188`/ack `0x130044`, `FW_RENDER 0xA278`/ack `0xD84`, private bit 5) around every sample.
- First real reading (20:24): RCS always idle at 1 Hz (`EXECLIST_STATUS lo=0x18001`), but `CSB_PTR` moving every second
  (context switches happen); `ERR_GEN6=0x7b`; `RING_MODE=0x8000` (bit 3 `GFX_DISABLE_LEGACY_MODE`, set by i915 on Gen11+,
  missing — noted as suspicious, never tested); `INSTDONE=0xfffffffe`; FAULT=0. A burst mode (~25 000 reads in 200 ms
  windows) never saw the RCS active.
- **Code review of V507/V509:** they assume a single fixed submission "slot" at `GGTT[0x19]` (page 1 of LRCA `0x18000`),
  copy one context image there and temporarily remap `GGTT[0]` — the first page of the framebuffer. HANGCHECK showed the
  execlist always at `0x18000`: WindowServer's work never ran. This comes from **scheduler 5** (host preemptive),
  NootedGreen's default for RPL.

### 9.2 Scheduler 4 → CCS panic → XE-NOCCS

- **Experiment `ngreenSched=4`** (`debug=0x100` dropped for length). The kernel finally **processed** command buffers
  (`IGAccelCommandQueue::processCommandBuffer` → `IntelMTLRenderFunctions::execute`), then panicked in SecurityAgent:
  page fault in `resolve_ccs(…)+0xc7` ← `IntelAccelerator::submitCCSResolve` ←
  `IGAccelResource::enableRenderCompressionWithAccelTask` (Gen12 CCS render compression).
- *Correction found on 19 Oct (Phase 11):* `-disablegfxfirmware` **forces scheduler 5** (`0x1190 |= 0x2800000`), so
  `ngreenSched=4` is in fact ignored (scheduler 4 is the GuC path). Why this boot behaved differently is unexplained
  *(uncertain)*. The argument is still in the release boot-args, harmlessly.
- **XE-NOCCS:** inject `RenderCompressionMode=0` and `AllowIOSurfaceRenderCompression=0` into the accelerator's
  `Development` dictionary and top level. Later found that `RenderCompressionMode` only selects the type (0="ALL",
  1="128", 2="64") and does **not** disable compression. Result (21:09): new panic `"EL[0] was invalidated!!"
  @tgl/sched5/IGHardwareCommandStreamer.cpp:64` — the GPU rejected a context.

### 9.3 The verbose console was corrupting GPU contexts

- **Evidence.** The "corrupt" context images held `00ffffff`/`00000000` patterns: `slot pg1: 00000000 11001015 00ffffff
  00000000 00ffffff …` — white and black pixels.
- **Hypothesis.** The `-v` kernel console keeps drawing text into the boot framebuffer at the start of the GGTT, the same
  range where the accelerator placed LRCA `0x18000`, slot `GGTT[0x19]` and rings.
- **Experiment.** Boot without `-v`. **Result (21:15): the cursor appeared and the GPU executed work** — the first
  gpuRestart report showed queue 1 with `0x55` of `0x5e` command buffers completed. First proof that RPL executes
  Apple's TGL contexts. **`-v` has been banned from the accelerated config ever since.**
- New hang: the RCS stopped in a PPGTT batch starting at `0x40006fc0` (64 bytes before a page end) and crossing into
  `0x40007000`: `ACTHD=0x40007050`, `IPEHR=0x40259441` (invalid instruction), EIR=ESR=1. PTWALK (a 4-level PPGTT/GGTT walk
  through a borrowed spare GGTT entry) was added to xe-rcsdump. *(This was the first sighting of the batch-runs-off-its-page
  family solved by XE-POOL in Phase 10.)*

### 9.4 XE-NOCCS2 and the draw that never completes

- The `resolve_ccs` panic came back intermittently (21:29). **XE-NOCCS2:** in `enableRenderCompressionWithAccelTask`
  turn `8a 86 91 00 00 00 a8 02 0f 85 37 02 00 00` into `… a8 02 90 e9 37 02 00 00` (jump to the epilogue at
  `0x739db`; no resource compressed).
- **Result (21:37):** no CCS panic; cursor for < 1 s; the GPU completed `0x53` command buffers, then hung **without any
  error** (EIR=0, FAULT=0) on a `PIPE_CONTROL` (`IPEHR=0x7a000004`) for 8 s; `INSTDONE=0xffdfffff` (only CS not done).
  GPU restart, WindowServer watchdog, "userspace watchdog timeout" panic.
- 21:48/21:52: same hang; diagnostics started too late. 22:00/22:01: early **"EL[0] was invalidated!!"** panics
  (`handleSoftwareContextSwitchInterrupt → processContextStatusBuffer → csbProcessActiveToIdle →
  ContextStatusBufferValidate`), attributed to xe-rcsdump's **burst forcewake** confusing `MultiForceWake`; burst made
  opt-in (`XE_RAJADA`).

### 9.5 Ruling things out: PPGTT, L3ALLOC (MTL v11), PSMI, memory writes

| Test | Evidence | Verdict |
|---|---|---|
| PTWALK of the hung batch (22:10) | PPGTT PTE `0x72cc8508b` valid | **not** a mapping problem |
| Batch contents | `PIPELINE_SELECT`, `LRI 0x7008`, **`LRI 0xb134 (L3ALLOC) = 0x88000020`**, `LRI 0xb138`; RPL-P's reset value is `0xd0000020` | suspicious |
| **MTL v11** (22:17): byte `0x88 → 0xd0` at 0x65f12, 0x7ff77, 0x80253 | L3ALLOC now `0xd0000020` in the batch (22:19); hang moved to `PIPE_CONTROL dw1=0x40105002` after two `MI_STORE_DATA_IMM` | **L3 not the cause** (patch kept). *Only 0x65f12 was really the L3 value — see Phase 19.* |
| PSMI re-applied every second from user space (22:34) | `0x2050=0x1090` during the hang; identical hang. `0x2050` falls back to `0x10` several times a second (RC6) | **PSMI not the cause** |
| Read the write destinations (22:55) | post-sync `0x40000010` written (GPU writes reach memory); the next `STORE_DATA_IMM` not yet executed; last executed header = CS-stall `PIPE_CONTROL` right after a `3DPRIMITIVE` (`7b000008 … 3 vertices, 1 instance`) | **the draw never completes**, although INSTDONE shows every unit done |
| Fuses (`0x9118`, `0x9134…`) | `DSS_EN=0x3f`, `EU_DIS=0`: 6 DSS, all EUs and L3 banks, as NootedGreen hard-codes | topology **not** the cause |

`xe-decode.py` was written: a Gen12 batch decoder built on Mesa 24.0 genxml (gen4→gen12 import chain).

---

## Phase 10 — Fast clear, flips, pixel formats and command pools (18 Oct ~23:08 → 19 Oct 01:35)

### 10.1 The stuck draw was a fast clear; NOCCS2 caused it → XE-NOCCS3

- **Decoded (23:08):** the stuck draw is a **render-target fast clear** (`3DSTATE_PS` with "Render Target Fast Clear
  Enable", RECTLIST, 3 vertices), preceded by a `3DSTATE_WM_HZ_OP` clear of a **compressed stencil**
  (`3DSTATE_STENCIL_BUFFER` with Control Surface / Stencil Compression Enable).
- **libxedrv v12** (23:15): every texture-creation path gets a descriptor copy with `allowGPUOptimizedContents = NO`.
  **No effect** (23:17): the TGL driver reads the private descriptor in `fillTextureData`; the property exists only in
  ObjC metadata. Kept as harmless.
- **Realisation.** The fast-clear hang began only after XE-NOCCS2, which removed the AUX mapping work while Metal kept
  compressing and fast-clearing. The original panic was specifically `resolve_ccs` writing into "buffer 2" of a
  `IOAccelCommandBufferPool2` that Tahoe's IOAF2 does not provide.
- **XE-NOCCS3** (replaces NOCCS2): the 24-byte unique prologue of `IntelAccelerator::submitCCSResolve` (`0x2c84c`) gets
  `31 c0 c3` (return 0; callers ignore it). A 17-byte prefix had 18 hits. **Result (23:29): the fast-clear hang
  disappeared**; the RCS executes and idles.

### 10.2 Flips refused → XE-DPCAPS (no effect) → XE-VALD

- **Symptom.** `IOAccelDisplayPipeTransaction2::set_transaction_args returning error 0xe00002c7` several times a second:
  no frame presented, flip stamps never close, GPU restart and watchdog.
- **XE-DPCAPS:** upstream V77 always injected `IOAccelDisplayPipeCapabilities {0, 0}`; now true/true unless `-ngreendp0`.
  Result (23:41): no change.
- **Trace** (Tahoe IOAF2 extracted from `SystemKernelExtensions.kc`): `set_transaction_args` → `pipe->vtable[0x8a0]` =
  `IGAccelDisplayPipe::validateTransaction` → `callPlatformFunction("vald")` = `AppleIntelFramebuffer::ValidateTransaction`
  (FB `0x3ec5a`), which rejects any transaction or plane option with bits `0x500` (set by default on Tahoe) and reads the
  pixel format from `IOSurface+0xa4` (moved on Tahoe; reads 0).
- **XE-VALD** (three FB patches): `test eax,0x500` → `test eax,0` at `0x3ecf1` and `0x3ed1d`; the `+0xa4` read at
  `0x3ed88` NOPed so the default `'BGRA'` stays. (IntelIGPU-Tahoe solved the same by rewriting the framebuffer.)
- The next boot **panicked early**: "divide error" in `hwCRTCToIODetailedTimingInformation+0x499` (FB `0x6c0c7`) called
  by `fbTimingsWorker` — the divisor `p->0x164` was still 0 (race). **Divide guard:** call only when the divisor is
  non-zero, retry in 50 ms. (After the repeated panics the trackpad was wedged even on Sequoia; a full power-off cured it.)
- **MILESTONE (19 Oct 00:05):** the mouse moves over an image — half the screen two-tone green, half glitched stripes.
  **WindowServer's flips reach the display.** xe-rcsdump gained pipe A plane registers.

### 10.3 Green screen → XE-FMT; garbage at the bottom → V99 off

- **Green (00:14):** the console used `PLANE_CTL 0x94000408` (XRGB8888, X-tiled); WindowServer's flip programmed
  `0x90010400` — **format 0 = YUV 4:2:2**. Same `IOSurface+0xa4` read, in the accelerator:
  `IGAccelDisplayPipe::performTransaction` (`0x81361`). **XE-FMT:** `8b 83 a4 00 00 00 89 85 f4 fe ff ff` →
  `b8 41 52 47 42 90 …` (`mov eax,'BGRA'`). Result (00:28): green became **black** with a glitch at the bottom.
- Renaming WindowServer's saved display prefs (3008×1880 HiDPI) to test a size mismatch: no effect.
- **SURFWALK** (GGTT walk of the displayed surface on every `PLANE_SURF` change): the 3840×2400 surface (36.9 MB) had real
  memory only in its first **16 MB**; the rest pointed into stolen memory (contexts, rings) — the moving glitch was
  GPU-internal memory on screen. Cause: NootedGreen's **V99R/V99G** hook on `PLANE_SURF` writes (`0x7019C`), sized for
  2560×1600, redirects the plane to fixed 4000-page slots, and slot 0 overwrites `GGTT[0..3999]` where the context
  lives. **V99 off under `-ngreenxefix`** (`-ngreenv99g` re-enables). Result (00:52): fully black screen, no glitch,
  cursor moving: the plane now shows WindowServer's real buffers, still black because the GPU still hangs.

### 10.4 Batches running into poison → XE-POOL; misaligned blit state → XE-POOLALIGN

- **Boot 01:02:** the hang was a batch running into poison: `BB_START 0x40006e40`, `ACTHD 0x40007004`, **`IPEHR
  0xc01dbead`**. The blit3d batch ends with a `3DPRIMITIVE` at `0x40006ffc`, exactly at the page end, and another batch
  starts at `0x40007000`. `getBufferPtrNoInc` (identical in Big Sur and Tahoe IOAF2) submits whatever is already written
  when `cur + n > limit` — even a half-built batch. TGL asks for up to 64 KB at once (`cmpq $0x10000` in the blit3d
  acquisition); Tahoe creates the pools with **4 KB** buffers (`POOL[1] init count=8 size=0x1000`).
- **XE-POOL** (`kern_green.cpp`): route on `IOAccelCommandBufferPool2::init`, size ≥ `0x20000` (128 KB). Result (01:14):
  **the poison disappeared**; blit3d batches complete with `MI_BATCH_BUFFER_END`. Next hang: the blit's RECTLIST
  (546×244) never ends.
- **AUX-TT check (01:23):** `AUXT_LO (0x4200) = 0x40008000` — the AUX table **is** configured (hypothesis discarded).
  The batch decode showed the blit's surface states and binding table written **0x18 bytes after** where the batch
  points (BT pointer `0x18e0` vs data at `0x18f8`): the GPU read an empty binding table. It happened only for blits that
  start at a non-64-byte-aligned address (`0x40b429b8`, right after the context-init commands).
- **XE-POOLALIGN:** route on `getBufferPtrNoInc`; for reservations ≥ `0x400` dwords, pad with `MI_NOOP` to a 64-byte
  boundary. **Result (01:35): no more gpuRestart reports — the blit hang was gone.** Black screen, smooth mouse; the user
  typed the password, then WindowServer watchdogs: `Display 1141199936 not ready … Surface Use Counts … FB Re[ady]`.
- **Correction.** An earlier "CSB read pointer = 0, lost interrupt" reading was wrong: Apple keeps the read pointer in
  bits 15:12 of `0x23a0` (`0x2002` = write 2 / read 2). The CSB **is** processed.

---

## Phase 11 — Where the work goes missing (19 Oct 01:45 → 08:05)

- **Boots 01:45–01:58** (WindowServer watchdog panics): the ring dump shows a complete submission ending with a notify
  `PIPE_CONTROL` whose post-sync value sits in memory: **the GPU writes the stamp**. So "stamp 56 / gpu 55" is work that
  was **never sent** to the GPU.
- **Static analysis, no boot.** A vtable dumper decoding Kernel Collection chained pointers compared the Big Sur and Tahoe
  IOAF2 classes used by TGL (`IOAccelEventMachineFast2` 121 slots, `IOAccelDisplayPipe` 68, `IOAccelCommandBufferPool2`
  91, `IOAccelChannel2` 76, …): **0 differences**. "Flip called without enabling VBL" was shown to be normal ordering.
  V187/V189 DYLD patches target Sonoma 14.7.1 CoreDisplay bytes and probably do not match on Tahoe.
- **Boot 06:10 + analysis:** `-disablegfxfirmware` **forces scheduler 5** (string at tglk `0x27ac8`), so the
  "empty `IGScheduler4::signalDisplayStampUpdate`" theory was dropped. Pipe A is healthy (`FRMCNT_A` at 60 Hz,
  `FLIPCNT_A` climbing, power wells on); after the first frames index-1 work never reaches the ring.
- **XE-FLIPDIAG / XE-SCHEDDIAG** (counters on the flip path and on `IGScheduler5::push`, `submitExecList`,
  pause/resume). IOLog does not reach Tahoe's unified log, so a collector pulled `XeNG` lines from dmesg into
  `xeng.txt`. Findings: the original pool size is `0x1000`; flips 1–11 complete and later ones wait behind render work.
- **Boot 06:47: WindowServer rendered ~50 frames at 60 Hz**, then stopped right after the first use of the kernel's
  blit3d context. In that ring the last stamp written was 5 while 7 had been **assigned**: blits 6 and 7 never reached
  the ring.
- **Cause: V142/V170** — by default on spoofed hardware `Gen11::submitBlit` **returns without executing the blit**
  (meant to avoid blit hangs that were really the 4 KB pool and the misaligned state). The caller had already reserved a
  stamp and waited forever. **Fix: `ngreenV142=3`** (Apple's original `submitBlit`; also enables the BCS interrupt bits).
- **MILESTONE (06:56): the Apple boot screen with the logo appeared in high resolution, rendered by the GPU**, and froze
  part-way along the progress bar.
- **V193** also dropped every `routeSel=3` (blit3d) blit with a fake success ("TGL shader hangs on RPL" — really the
  shifted state). **Off under `-ngreenxefix`** (`-ngreenv193` re-enables).
- **XE-BLITDIAG** (07:45): all 64 blits go through the original `submitBlit` with `routeSel=0` and succeed — the stuck
  index is another GPU client. An earlier "PIPE_CONTROL on BCS" reading was garbage from a dump bug.
- **XE-RINGDIAG** (07:53) and **proof (08:04):** push #197 (BCS) → execlist #167; push #198 (WindowServer, RCS) and
  push #199 → **no execlist after that**. Scheduler 5 receives RCS work and stops submitting it.

---

## Phase 12 — The lost execlist submission (19 Oct 08:05 → ~12:30)

**Goal.** Find why an RCS submission written by scheduler 5 is never started by the hardware.

- **Reverse engineering.** `IGHardwareCommandStreamer5::schedDispatch` (tglk `0x63192`), `processContextStatusBuffer`
  (6 entries, write pointer from the HWSP if `0x1190` bit 7), `processCSBDetail` (mask `0x3ff8000`), `dequeueExecList →
  verifyThrottling → evalWaitQueue → prepareExecListAndSubmit`.
- **XE-CSSTATE, XE-CSBDIAG, XE-EVALDIAG** (diagnostics; hot paths write to an interrupt-safe ring). Results (08:18–08:35):
  CSB events decoded correctly (idle→active, active→idle `0x18`, preempt, element switch); DPSM awake; throttling always 0.
  **The exact failure:** the last RCS job ends; the scheduler submits the next one immediately from inside the interrupt
  handler; the hardware never produces a CSB event for it.

### 12.1 The forcewake theory (discarded)

| Patch | Idea | Result |
|---|---|---|
| **XE-FWREF** | NootedGreen's i915-style `forceWake` replacement has no reference count: one user's release turns forcewake off under another, so the ELSP write right after RC6 entry is lost. Per-(domain, ctx) refcount under an `IOSimpleLock` | hang unchanged (08:44, 08:52); kept |
| **XE-FWHOLD** | never release forcewake | **worse** (09:00): froze at 68.9 s, CSB entries zeroed, "EL[0] was invalidated" panic. Reverted to opt-in `-ngreenfwhold` |
| **XE-FWSUBMIT** | hold Render+Blitter forcewake around `prepareExecListAndSubmit` | panic 09:35 "hwlock … trying to lock … recursively" — the FWREF lock is also taken in primary interrupt context. Fixed with `IOSimpleLockLockDisableInterrupt` |
| verdict (09:41) | forcewake guaranteed at submission | **same hang**: 155 `submitExecList` writes happen, the hardware does not react. **Forcewake discarded as the cause**; FWREF and FWSUBMIT left installed |

### 12.2 Which submission is lost

- **XE-ELSQ** (09:50): the user noticed that **only the picture freezes** — the cursor changes shape over the password
  field and the login is accepted. Decoded ELSQ (descriptor lo = LRCA | `0x319`, hi >> 5 = context ID): the **first
  submission of a brand-new context, ctx18**, never starts — deterministically, every boot.
- **V509 suspicion** (10:02): logging showed all "ok", no repairs. **Innocent.** ctx18's image looks intact;
  `EXECLIST_STATUS = 00000200:40018098` (the hardware still has ctx16 current).
- **XE-POSH** (10:08–10:20): the only structural difference between ctx16 (runs) and ctx18 (stuck) was `POSH_LRCA`
  (`0x21b0`) in the context image, inherited from the boot's first context through `restoreFromSafeImage`. Zeroing it:
  **still lost**. Hook kept (harmless). New finding: good submissions leave status `0x00003098`; the lost one
  `0x40018098` (bit 30, never decoded).
- **XE-ELSQWAIT** (10:29): wait for bit 30 to clear before submitting. **Never triggered** — the bit appears *after* the
  write. ctx16's batches use `MI_BATCH_BUFFER_START 0x18900101` (bit 20 = POSH), the others `0x18800101`.
- **XE-NOPOSH:** never set bit 20 in `submitBatchBuffer` (`0x43021`). Two boots went **black before the high-res logo**
  (it used to appear) → reverted to opt-in `-ngreennoposh`.
- **Panic 10:38:** "NMIPI for spinlock acquisition timeout, owner == current_thread" from an interrupt. Cause: **IOLog in
  diagnostic hooks called in interrupt context** while the interrupted thread held the log lock, plus
  `wrapWriteRegister32` inside the FWREF lock. Hot hooks now write only to the `xeEv` ring buffer; register writes inside
  the lock use raw MMIO. **Rule adopted: never IOLog/SYSLOG from code that can run at interrupt time.**

### 12.3 EXECLIST_STATUS decoded → ELSQDELAY, ELSQIDLE, ELSQRETRY, NOPREEMPT

The status bits were decoded from Apple's own register-dump code (tglk ~`0x45705`): b0 current pointer, b1 write
pointer, b2 queue full, b3/b4 EL1/EL0 valid, b13:5 last switch reason, b15:14 active element state, b16 arbitration,
b17/b18 EL1/EL0 active; b30 appears only on refusals. Idle reads `0x18001`.

| Before the write → after | Outcome |
|---|---|
| `0x18001` (idle) → `0x3098` | always OK |
| `0x3098` (running) → `0x2098` | OK |
| `0x14198` / `0x18098` (element switch pending, arbitration on) → `0x4001x198` / `0x40018098` | **refused**; waiting 2 ms does not change the state |

| Patch | Result |
|---|---|
| **XE-ELSQDELAY** (fixed 50 µs before each RCS submit) | not enough |
| **XE-ELSQIDLE ("IDLW")**: if a context is current, wait (5 µs steps) for idle before an RCS submit (11:07) | back to the high-res logo, progress bar stops |
| **XE-ELSQRETRY**: a thread_call reloads the ELSQ once the engine is idle (11:29–11:58) | status frozen at `0x40018098` for the full 200 ms: the RCS is **wedged**, not merely refusing. Reboot loops ("WindowServer initialization not complete") |
| **XE-NOPREEMPT**: every refusal had arbitration on + a current context, i.e. the submission triggers a preemption; wait up to `ngreenpreemptwait` (12:08) | refusals with arbitration off were transient; the fatal stall happens **before** the write: after ctx16 ends, the RCS sits at `0x18098` for > 50 ms. Preemption is not the culprit |

### 12.4 XE-POSHCTX — the first GPU-drawn login screen

- **Common factor:** every context that wedged the engine (ctx16, and ctx7 on the NOPOSH boot) had been created as a
  **POSH context** (`ctx+0x6e` bit `0x10`, `CTX_CTRL` bit 4, own POSH buffer, batch bit 20), although the driver tells
  user space POSH is off (`_kIntelSharedExFtrPoshEnable=0`).
- **XE-POSHCTX:** in the `restoreFromSafeImage` wrapper, clear `ctx+0x6e` bit `0x10` *before* the image is built, so
  every context is a plain context end to end (`-ngreenposhctx` keeps POSH).
- **Result (~12:30): "DEU VIDEO!!"** — the login screen was fully visible, drawn by the GPU, and usable for a moment.
  Typing then froze the picture after one character, and two small black rectangles sat where the date and the password
  hint should be.

---

## Phase 13 — Clears, compression and VS state: MTL13 → MTL17 (19 Oct 12:30 → 14:05)

- **Hang location:** ctx5 (WindowServer) at the `PIPE_CONTROL` after a clear RECTLIST with fast clear and `WM_HZ_OP`; no
  fault; AUX-TT configured.
- **MTL13** (12:36): `-[MTLIGAccelTexture generateClearRequirements:]` sets bits 11/12 when `m_Info+0x160/+0x164` (AUX
  info) is non-zero; the two `setne` at `0x1c7c0`/`0x1c7cf` → NOP. **Fast clear gone**, but the RCS still hangs after an
  **ordinary** clear (`IGRenderClearEncoder::writePSState`: SIMD8 + SIMD16 + Dual SIMD8). Photo: login visible with
  black rectangles instead of the small text; the big clock draws.
- **MTL14** (12:48): dual SIMD8 off in the clear PS (`setne` at `0x49cfe` → NOP). Same. The user noticed that the first
  seconds always work (small text, brightness keys) and the freeze begins when a second process starts using the GPU.
- **XE-IDLEANY** (13:03): wait for idle even without arbitration. Same: IDLW waited 50 ms on `0x3098`/ctx5 — the RCS was
  **already** stuck in a ctx5 batch submitted onto an idle engine. **The "double submission" was a consequence, not a
  cause.**
- **RTDUMP** (render-target decode in xe-rcsdump, 13:12): the stuck clear's target is a small BGRA8 Tile-Y surface
  (384×64 or 704×128 — small text layers) with **`AuxiliarySurfaceMode=5` (AUX_CCS_E)**. For IOSurfaces the Metal driver's
  compression flag comes from the kernel's shared info (`+0x18`) in `initWithDevice:descriptor:iosurface:…`.
- **MTL15** (13:19): that read (`mov ecx,[rax+0x18]` at `0x18c28`) → `xor ecx,ecx; nop`. **Result (13:21) — MILESTONE:
  the user logged in to the desktop** (slow mouse, black for a while, then Tahoe's wallpaper with a garbage panel in the
  middle). RTDUMP still showed auxmode 5 and gpuRestart every ~9.5 s.
- **XE-NOAUX** (13:32): the kernel's `IGAccelResource::fillSurfaceStateAuxInfo` (`0x73fd0`) always writes AUX_NONE.
  Applied (auxmode 0) and **the hang continued** (32 pauses). **Compression was not the cause**; MTL15 and NOAUX kept as
  harmless.
- **New clue: `INSTDONE=0xffdffffb`** — bit 2, VS not done. The first clear in a command buffer does not program
  `3DSTATE_VS`; it inherits whatever VS the previous draw left (enabled, pointing at another pipeline's kernel). That
  breaks when the GPU switches between WindowServer and another process — matching "the first seconds work".
- **MTL16** (13:42): `IGAccelRenderCommandEncoder::programClearPipeline` always emits the disabled `3DSTATE_VS` (`je` at
  `0x67c27` → NOPs). Result (13:44): **zero RCS hangs**, but windows were empty outlines — the encoder's state tracker
  (`+0x11b70`) still believed the previous VS was programmed.
- **MTL17** (13:53): `jne` → `jmp` at `0x67c5c`, so after the clear the VS is marked dirty and the next draw re-emits its
  own. Result (13:56): faster login, **windows with real content** (Photos, Terminal text), grey wallpaper, still slow.
  **MTL17 remained the Metal driver patch level for the rest of the project** (until MTL18, Phase 19).

---

## Phase 14 — The Blit3D context and its missing kernels (19 Oct 14:05 → 17:20)

- **Remaining hang** (14:08–14:31): a context with its own ring (ctx11) stuck on `PIPE_CONTROL 0x50105021` with the 3D
  pipe idle. ctx11 is the kernel's **Blit3D** context (`IGAccelDisplayPipe::generateFlip` → `getBlit3DContext`): its first
  batch sets all 3D state, later blits emit only PS/BT/VB/3DPRIMITIVE. Best boot so far (14:31): menu bar, widgets,
  Photos and Maps drawn by the GPU; only 2 real RCS hangs (one a 32×128 R32G32B32A32 Tile-Y copy from a linear source —
  probably the cursor). System Settings showed "Unknown Display".
- **"3D state lost on context switch?"** **LRCSCAN** (scan of 32 pages of the context images) showed SBA, VB, VS, PS
  **saved** in both ctx11 and WindowServer's image. Theory weakened. URB size also ruled out (4 KB/bank × 16 ways).
- **Log hygiene.** `verifyThrottling` runs ~6000 times/s; its event had produced > 860 000 lines; reduced to counters.
  Event and CSB rings made circular.
- **XE-NOTCF** (15:07): the flip's `PIPE_CONTROL` dw1 `0x50105021` includes Tile Cache Flush (bit 28); the rest of the
  driver uses `0x40105021`. Three `movabs` sites patched. Zero pauses for ~5 minutes, then a gpuRestart whose ring still
  held `0x50105021` — from `IGHardwareRingBuffer{Main,Compute}::commitStampCommand`, which shifts a bool to bit 60.
  **XE-NOTCF2** patched that `shl r14,0x3c`. **Result (15:33) — MILESTONE: first complete desktop** (wallpaper, Dock,
  widgets, Terminal, Settings), then a total freeze; the ring no longer had TCF and the hang remained. **TCF was not
  the cause** (both kept). Apple's reports: EUs/WMFE/PSS "not done".
- **XE-BLIT16:** force the SIMD16-only Blit3D path (the SIMD32 PS `IGBlit3DKernel_default_simd32`, KSP `0x540`, was
  suspected of never terminating). Applied (16:06), hang identical. **SIMD32 not the cause** (kept). One unrelated panic
  in `necp_client_action` did not repeat.
- **ROOT CAUSE (16:15).** An instruction-heap dump showed the SBA saved in ctx11 correct (instruction base `0x477a3000`)
  but the memory at `0x477a34c0` **all zero**: the EUs executed zeros. NootedGreen's **V148** replaces
  `IGHardwareBlit3DContext::initialize()` and deliberately **skips `blit3d_initialize_scratch_space`** (it had crashed on
  an earlier macOS); the later V181 "safe copy" never runs.
- **XE-BLITSCRATCH:** call the copy again. First boot: panic "NMIPI … spinlock … owner == current_thread" in
  `Gen11::v71EmrEnforcer+0x190` — right after `lilu_os_log` in the periodic "V74Ep" SYSLOG: a log-lock deadlock in
  interrupt context. Other boots froze silently with a healthy GPU (logs stop at ~2.5 min). **XE-LOGQUIET:** no SYSLOG
  from `v71EmrEnforcer`, forcewake timeouts as `xeEv('FWTO')`, HANGCHECK silenced. The scratch was **still zero**: the
  V181 wrapper needs routed `IOAF2_lock/unlockForCPUAccess`, and their resolution in `kern_green.cpp` was commented out.
- **XE-BLITSCRATCH2:** route `IOAccelSysMemory::lock/unlockForCPUAccess`; for the scratch object only, `lock` returns the
  CPU VA of the memory the GPU really reads. First version: **boot loop** — write fault at `cpu+0xd000`: the CPU mapping
  covers only `0xd000` bytes and Apple writes ~`0x210` bytes of sampler/blend state beyond it (the same crash that
  motivated V69/V148). Final version: Apple's routine writes into an `IOMallocZero(0xe000)` temporary buffer and the
  first `0xd000` bytes are copied. **Result (17:06/17:17): `[0x4c0] 00000000 -> 2420005b`, zero pauses, zero hangs.**
  macOS is navigable (slow, some stalls). The sampler-state tail beyond `0xd000` is lost (known gap).

---

## Phase 15 — Stabilisation (19 Oct 17:20 → ~21:20)

**Goal.** Turn a navigable desktop into a usable one. From here on, the assistant could work on Tahoe directly once
Wi-Fi was available; until then each test still meant a round trip through Sequoia.

### 15.1 Displays pane panics → XE-PLANES; xe-res

- Opening **System Settings › Displays** rebooted the machine: "divide error" in
  `IGAccelResource::calculateIOSurfaceDeviceCacheVRAMBytes` (`0x72bcd`, `divl r15d` = 0). The plane count is read from
  `IOSurface+0xb0`, a Big Sur offset. **XE-PLANES:** replace the read with `xor eax,eax`. (This later turned out to cause
  the green-wallpaper bug; see Phase 18.) A second panic, "EL[0] was invalidated", made **ELSQRETRY off by default**.
- Displays still panicked, now with a GP fault in `IGAccelResource::pageon` (`0x705b1`, `[r12+rbx*8]` on an invalid
  pointer) via `IGAccelSegmentResourceList::prepare` ← `processCommandBuffer`.
- **`xe-res`** (`tools/xe-res.c`): list display modes including HiDPI and switch with
  `CGConfigureDisplayWithDisplayMode`; `xe-res wNNNN` picks the HiDPI mode NNNN points wide.

### 15.2 QUIETLOG and the Wi-Fi detour

- Wi-Fi (itlwm 2.3.0, AX211 `8086:51F1`) stopped working on Tahoe: `SecBoot CPU1 Status 0x7fb7 … could not load firmware
  … attach fail` at ~2.6 s, and later "interface en0 does not exist". The itlwm messages were also drowned by `XeNG`
  output.
- **XE-QUIETLOG:** event/CSB printing only with `-ngreenevlog` (still recorded in the ring); `XeNG FLIP queue` had been
  printed on every flip (~45/s) because of an always-true condition. `-NGreenDebug` removed from the boot-args. **The
  user reported much smoother video and fewer freezes.**
- A bisection of the Wi-Fi problem (old kext in the EFI) was prepared and **declined** by the user to stay focused on
  graphics. Wi-Fi came back later after a BIOS change (Phase 16).

### 15.3 CSB guards

- Mode changes with `xe-res` caused **"EL[0] was invalidated!!"** (`ContextStatusBufferValidate` ←
  `csbProcessActiveToIdle` ← `processContextStatusBuffer`): a "completed" CSB event for an element already retired.
- **XE-CSBGUARD:** drop the event if the EL is null or the context ID (`(e[1]>>5)&0x7ff` vs `ctx+0x7c`) does not match.
  1680 pt worked; 1280 pt then panicked through `csbProcessPreempted` → **CSBGUARD2** covers ActiveToIdle, Preempted and
  ElementSwitch (counter `csbx`, and a dump of the last 40 CSB entries and 120 events on each skip).
- Three boots had 0 pauses and no gpuRestart: the remaining stalls were **not** GPU hangs.

### 15.4 Idle waits, pageon, and a rollback

- **Measured (19:03):** `idlw=1444/5407ms max=50000us to=8` — in ~3 minutes, 1444 pre-submit busy-waits cost 5.4 s of CPU,
  and each of the 8 timeouts was a visible stutter. `ngreenpreemptwait` 50 ms → **2 ms**: artifacts came back
  (`to=861`) → 15 ms.
- **XE-NOPAGE** (pageon/pageoff as no-ops): emptied windows and the menu bar (`pgon=2069` in ~3 min) — `pageon` is part of
  normal texture preparation. Opt-in only.
- **XE-PAGEONGUARD:** before `pageon` walks a level's rectangle list, check with `pmap_find_phys(kernel_pmap, …)` that
  it is mapped; if not, zero that level's count (counter `pgfix`).
- With CSBGUARD2 + IDLW 15 ms + PAGEONGUARD the user saw **new glitches** (bands over the login clock, sometimes no
  keyboard). **Rollback** to the "good version" — the QUIETLOG build (BLITSCRATCH2, PLANES, QUIETLOG, ELSQRETRY off,
  IDLW 50 ms, no guards) — with the plan to **add one change per boot**.
- The good build panicked at every login: the mode-switch scripts had saved a 1504×940 HiDPI mode, WindowServer
  re-applied it, and "EL[0] was invalidated" hit without the guard; it also panicked in normal use
  (`csbProcessElementSwitch`). **The CSB guard is needed in general.** New baseline: good build + CSBGUARD2.

### 15.5 Ideas from WhateverXe: XE-SAMETAIL and XE-RPCS; IDLW off

- **WhateverXe** (b00t0x, 0.3.0) supports Tahoe acceleration on real TGL and ADL-N with the **same** 16.0.32 driver
  (identical offsets); RPL-P and eDP were "Planned".
- **XE-SAMETAIL** (from its commit efa9241): if the RCS re-submits the active item with identical ctx, descriptor, image
  and tail, set Apple's one-shot force-restore flag (`ctx+0xc9`), as Linux avoids a lite restore with the same tail.
- **XE-RPCS** (from 1163642): `R_PWR_CLK_STATE` (`0x20c8`) in the RCS context `0x80000000` → `0x80041000`.
- **RPCS + SAMETAIL (20:42):** 0 panics, `csbx=0`, but **very slow** (4–7 submissions/s instead of ~47; mouse every 20 s).
  **RPCS made opt-in.** SAMETAIL then gave `csbx=0` across three boots: no invalid CSB events at all.
- One boot froze completely at ~3.5 min with IDLW waits summing 15.7 s → **IDLW off by default.** Three new GP-fault
  panics in `pageon` with **non-canonical** pointers (WallpaperImageExtension and the Displays extension) →
  **PAGEONGUARD on by default**, also rejecting non-canonical addresses.
- **Config change that did not boot.** WhateverGreen limited to Sequoia (`MaxKernel=24.99.99`) + CpuTopologyRebuild
  enabled, together with BIOS changes (all cores, Hyper-Threading, hybrid technology) — **no boot**, cause unknown. The
  user recovered from another Mac (an old config was put on the EFI), then the last good config was restored. **Rule:
  one change per boot.** E-cores stayed off.

---

## Phase 16 — c01dbead hangs, brightness, and the depth resolve (19 Oct ~21:20 → 23:30)

- **Side effect of the BIOS change:** from the next boot (21:52) **Wi-Fi worked on Tahoe** again (`firmware alive`, en0
  up). The responsible BIOS option was never identified.
- **New hang (21:52):** after ~170 s of smooth use the mouse froze. ctx21 (a Blit3D context): `ACTHD=40c2a044`,
  `BB_START=40c29f80`, **`IPEHR=c01dbead`** — the batch starts 0x80 bytes before a page end and continues into a page
  whose PTE points at Apple's scratch/poison page.

| Hypothesis | Experiment | Result |
|---|---|---|
| XE-POOL v1 too aggressive: 256 buffers × 128 KB = 32 MB per process exhausts the command-buffer VA | **XE-POOL2**: 128 KB only for small (count ≤ 16) kernel pools, 4 KB → 16 KB for large ones, 64 KB unchanged | same hang (22:01) |
| IDLW: the hang appeared only on the two boots without it | IDLW back on with a **5 ms** cap | still 4 c01dbead PTWALKs → not the cause (kept at 5 ms) |
| Deleting the display-mode prefs triggered it | restore the 1504×940 plists | same hang |
| Pages of the Blit3D pool mapped too late (cf. WhateverXe 239c010, "late mapping invalidation") | not implemented | — |

The freezes stopped after XE-NODEPTH (below); the c01dbead hang itself was **never root-caused**.

- **`xe-bootdiag` made light.** Its +150 s block forced brightness to 60 % through XeShim (the "brightness jump" the user
  saw), dumped 128 MB and ran a full `log show` (the "slow at 2–3 minutes" stalls). Removed.
- **Brightness capped at ~50 % → XE-BLR.** South PWM registers: `0xC8250` (enable), `0xC8254` (period), `0xC8258` (duty).
  Modelled on WhateverGreen's BacklightRegistersFix: keep the hardware period and rescale the duty. Measured (22:48):
  `bl hw=96000 apple=0 fw=0 duty=17741` — Apple **never writes the period** → `ngreenblmax=48000` as the reference.
- **Panic `resolve_hiz_g7`** (iconservicesagent, page fault) ← `IntelAccelerator::submitDepthResolve` (`0x2c656`) ←
  `enableRenderCompressionWithAccelTask`: the same class of bug as NOCCS3 (the HiZ resolve writes into a pool buffer
  Tahoe does not provide). **XE-NODEPTH:** `submitDepthResolve` returns immediately (its prologue occurs twice, so the
  pattern was extended).
- **MILESTONE: with NODEPTH, Tahoe was stable** — no gpuRestart, smooth desktop, Wi-Fi connected — and the assistant's
  session moved onto Tahoe itself.

---

## Phase 17 — Living on Tahoe; clean reinstall (19 Oct ~23:20 → 20 Oct 14:30)

- **Brightness keys.** Apple's driver never writes `0xC8258` after boot (158 identical samples), so the keys never reach
  the hardware. `xectl wr 0xC8258 0x17700` gives 100 %. Workaround **`xe-brilho`** (PWM through XeShim, `+`/`-`, and a
  daemon that follows the keys through a CGEventTap; the daemon did not react to the keys and its LaunchAgent was not
  installed).
- **Panic (Wallpaper):** `kfree: addr 0x300c000040001, size 0 … not found in any zone` at `IGAccelResource::free()+0x182`.
  **XE-RESFREE:** before the original `free`, null unmapped pointers in the per-plane/mip table at `this+0x250` (counter
  `resfix`). The bad pointers were probably an effect of the PLANES bug.
- **DPI:** `xe-res w1680` switched to 1680 pt HiDPI with only a brief glitch (`csbx=4`). The Displays pane was still
  avoided; later `xe-res w1280` went black with a cursor until reboot.
- **GPU stuck at 100 MHz.** `RPSTAT1 0xA01C = 0x03000000` → CAGF ≈ 100 MHz (RPn); RP0 is 1500 MHz. `xectl` gained `rdw`/`wrw`
  (hold forcewake; plain writes were lost in RC6). Writing a 900 MHz request raised CAGF within 20 ms, and Apple reset it
  to 0 within ~40 ms. A 40 s loop holding 1100 MHz: 5287 of 6857 samples at 1100. Cause:
  `handleChangeFrequencyInterrupt` asks a power object (`accel+0xde8`) that depends on GuC/SLPC — disabled by
  `-disablegfxfirmware` — so `setGTFrequencyMMIO` writes 0. **XE-GTFREQ:** never request less than `ngreengtfreq` MHz
  (default 1100). **Confirmed the next morning:** CAGF 1100 MHz, and the IDLW total dropped from 5634 ms to 222 ms.
- **Trackpad:** in some boots `VoodooI2CControllerDriver::pci8086,51e8 Warning: Timeout waiting for bus not to be busy`
  every ~3.3 s; independent of the GPU work; not fixed. One `pmap_recycle_page` panic through USB storage/APFS (the
  external boot SSD) was seen once.
- **Survey:** Metal reports "Intel(R) TGL Unknown"; VideoToolbox advertises H.264 hardware decode; no audio (SoundWire
  RT714 + RT1316 behind the SOF DSP); no camera.
- **Clean reinstall onto the internal SSD.** The user decided to wipe the internal disk (Windows). A safe install config
  (NootedGreen off, `-v`) was prepared. `softwareupdate --fetch-full-installer` was interrupted at ~90 % by a GPU hang
  and WindowServer watchdog (after 1 h 17 min of use), the re-run left a truncated `SharedSupport.dmg` (`createinstallmedia`:
  error 113), and a gibMacOS download was interrupted; the user finally installed Tahoe outside the session.
  `reinstalar-stack.sh` re-applied the stack from the external Tahoe (TGL kexts, MTL17 checked by MD5
  `50bfb18b1366f08dc65465d12f4d37b4`, `xe.bundle` with bridge v2, XeShim, `xe-bootdiag`). **14:30: acceleration OK on the
  clean install** — with new symptoms: **green wallpapers** and missing desktop icons. OpenCore copied to the internal ESP;
  the GPU renamed "Intel Iris Xe Graphics" through DeviceProperties.

---

## Phase 18 — Clean-install regressions and the repository (20 Oct 14:30 → ~17:30)

### 18.1 "corrupt list" → XE-NOORPHAN

- YouTube produced two gpuRestarts, then `panic: corrupt list around element …` (iconservicesagent) inside IOAF2.
  Symbolicating with an older IOAF2 extract gave nonsense (wrong UUID); a new tool, `kcsyms.py`, reads the fileset
  Mach-O of `SystemKernelExtensions.kc` (4171 symbols). Frames: `IOAccelOrphanedMemoryPool::addMemory` ←
  `IOAccelSysMemory::unorphanIt+0x2b` ← `IOAccelOrphanedMemoryPool::getMemory+0x131` ←
  `IOAccelResource2::newResourceWithOptions`.
- **XE-NOORPHAN:** `getMemory` always returns nullptr, so memory is always freshly allocated. **Partial:** a second
  "corrupt list" panic at shutdown went through `IOAccelSysMemory::free → addMemory`, which NOORPHAN does not cover
  (still open).

### 18.2 Green HEIC wallpapers → XE-PLANES2

- A solid-colour PNG wallpaper displayed correctly; HEIC (YUV, biplanar) displayed green; `sips` converting HEIC to PNG
  produced pure (0,255,0) because the conversion itself went through the GPU.
- **`xe-yuvtest`:** a 420v IOSurface filled with Y=41, Cb=240, Cr=110 read back by the GPU: plane 0 correct, **plane 1
  read 0**; with row-stamped planes, plane 1 was read from offset 0 regardless of its real offset.
- Tahoe kernelcache: `IOSurface::getPlaneCount()` = `mov eax,[rdi+0x98]`, `getPixelFormat()` = `mov eax,[rdi+0x80]` (the
  old driver uses `+0xb0` / `+0xa4`). **Root cause: our own XE-PLANES** — forcing the count to 0 made every surface
  single-plane, so the per-plane table never got plane 1 and the chroma read as zero (green).
- **XE-PLANES2:** read `[rbx+0x98]`. **Confirmed (15:40):** "YUV OK", correct HEIC wallpaper and lock screen, Dock icons
  and thumbnails back.

### 18.3 Video: −12913 / −6680 → XE-NOGVA (partial)

- Safari YouTube played avc1 at "0x0" with zero frames, then "An error occurred". `defaults write com.apple.coremedia
  hardwareVideoDecoder disable` changed nothing.
- **`xe-vttest`:** hardware H.264 session creation fails with **−12913**; a software session works with native output
  but fails with **−6680** (`kCVReturnInvalidPixelFormat`) as soon as any output pixel format is requested. CVPixelBuffer
  creation (`xe-pbtest`) and VTPixelTransferSession (`xe-xfertest`) work in isolation.
- **XE-NOGVA:** remove the five `IOGVA*` properties the TGL personality advertises. Now
  `VTIsHardwareDecodeSupported(H264)=0`, and **software decode with an output format still fails with −6680** — Safari
  stays broken.
- **Workaround:** Chrome with GPU rendering (ANGLE/Metal) draws only fragments of its window; launched with
  `--disable-gpu --disable-gpu-compositing` it renders correctly and plays YouTube.

### 18.4 Invisible icons → libxedrv v15 (NOCOPY)

Finder app icons in the new `.icon` format, DMG window backgrounds and Quick Look were missing; old `.icns` icons were
fine; clearing icon caches did nothing.

| Step | Test | Result |
|---|---|---|
| What is not broken | `xe-icontest` (IconServices bitmaps), `xe-miptest`, `xe-f16test`, `xe-atlastest` | icons, mipmaps, RGBA16F uploads and copies all correct |
| Layers | `xe-layertest`: CALayers with IOSurface contents in six formats + CGImage squares | `w30r` (10-bit XR) missing (Metal rejects BGR10_XR); a real icon via `NSImage` missing but its CGImage used directly shown; content converted to the display colour space missing or striped |
| Colour profile | `xe-perfil` set an sRGB display profile | worse; **reverted** to the factory (EDID) profile |
| Instrument WindowServer | libxedrv **v13** (texture logging to `/tmp/xedrv-<proc>.log`), **v14** (pipeline logging) | 11 pipelines, 0 failures: shaders are not the problem |
| Reproduce in-process | `xe-carender` (CARenderer → Metal texture) | display-colour-space layers come out wrong **outside** WindowServer too |
| Buffer textures | `xe-buftex`, `xe-bufoff`, `xe-vfmttest` | correct |
| Framebuffer fetch | `xe-fbfetch` correct; `xe-nofetch` forced the caps off, CoreAnimation switched shaders, output unchanged | **ruled out** |
| Alignment | `xe-align` forced 256-byte texture-buffer alignment | no change |
| Texture-API spy | `xe-encspy` | the failing textures all start at offset **0x20** |

**Root cause.** CoreAnimation wraps image memory zero-copy with
`-[MTLIOAccelDevice newTiledTextureWithBytesNoCopy:length:(deallocator:)descriptor:offset:bytesPerRow:]`; for images
already in the destination colour space the offset is `0x20` (a 32-byte image-buffer header; `0x4c0` and `0xda0` were
also seen). The TGL driver on RPL does not sample a linear texture at such an offset, so the layer is invisible.

**libxedrv v15:** when the offset is not 256-byte aligned (single-level 2D), create a driver-owned texture and **copy**
the pixels; the caller's deallocator runs when our texture is released. Proven first with the test dylib
`xe-nocopyfix`. **Result: Finder icons appeared** (`NOCOPY textura 128x128 fmt=80 off=0x20 bpr=512 -> cópia alinhada`).
DMG backgrounds and Quick Look were expected to benefit but were not re-verified.

### 18.5 Reproducible build and repository

- `tools/diagnostics/mtl-diff.py` diffs two Mach-O files outside the code signature: stock vs MTL17 = **12 content
  regions** (0x10, 0x1080, 0x18c28, 0x1c7c0, 0x1c7cf, 0x49cfe, 0x65f12, 0x67c27, 0x67c5c, 0x7ff77, 0x80253, 0xa2e7b).
  Two codesign-managed regions must be excluded (including them gave "main executable failed strict validation").
- `macos/patch-mtl17.py` + `mtl17-patches.json` rebuild MTL17 from the stock bundle with **zero content differences**;
  `macos/build-shader-compiler.sh` rebuilds `xe.bundle` (stock `libigc` re-signed, the old plugin with its install name
  and 12 regions patched, `libLLVMbridge` against a stub of 21 502 exports generated with `dyld_info -exports`, the
  adapter).
- The repository was assembled and committed locally: sanitised EFI (serials blanked, `HfsPlus.efi` and a
  machine-specific DSDT removed, `config-install.plist`), `src/NootedGreen` + the diff against upstream `95f8189`,
  `build-nootedgreen.sh`, `macos/`, `tools/`, `docs/`. No Apple binaries are included.

---

## Phase 19 — After the repository: audit findings and new problems (20 Oct, latest)

### 19.1 The MTL v11 L3ALLOC patch damaged two instructions → MTL18

While cataloguing every byte of MTL17 against the symbol table and `otool`, the v11 change turned out to be only partly
what it claimed:

- v11 replaced the byte `0x88` with `0xd0` at three offsets, i.e. the little-endian dword `20000088` → `200000d0`
  (`0x88000020` → `0xd0000020`).
- **0x65f12** is a real immediate — `movl $0x88000020, 0x14(%rcx)` in
  `IGAccelRenderCommandEncoder::writeInitializationHWState`. That is the L3 configuration value.
- **0x7ff77 and 0x80253** are not. Inside `IntelState::IntelState()` the bytes `20 00 00 88` are the tail of a
  `movq $0x2000, …` followed by the **opcode byte of the next instruction**. The patch turned `movb %al,0x110(%rdi)`
  (`88 87 10 01 00 00`) into `rolb 0x110(%rdi)` and `movb %cl,0x8e0(%rbx)` into `rorb 0x8e0(%rbx)`. Those two byte fields
  of the driver's state object have been left uninitialised (rotated garbage) since 18 Oct.
- Since v11 was already known not to be the cause of the hang it targeted (Phase 9), the damage went unnoticed through
  MTL12–MTL17. What the two fields control was not established.

**MTL18** = MTL17 with those two bytes restored to `0x88` (only the real L3 immediate keeps `0xd0`). The repository's
`macos/mtl17-patches.json` now excludes the two corrupting entries, so `patch-mtl17.py` produces MTL18. **MTL18 is being
tested**; whether it changes any of the open problems is not known yet.

### 19.2 Display does not wake after display sleep

After the display goes to sleep, it does not come back. Not investigated yet. (Sleep in general had been listed as
untested.)

### 19.3 Colour-profile change → three page-fault panics

Opening **System Settings › Displays** and selecting the **ITU-709** colour profile led to three kernel panics, all page
faults in GPU memory management:

| Panic | Path |
|---|---|
| 1 | `newResourceWithClientBuffer` / `client_address_persistent` |
| 2 | `orphanIt` / `releaseDescriptors` |
| 3 | `IGHardwarePageTable::commitRange` |

Reverting to the factory (EDID-derived) profile restored stability. The earlier sRGB-profile experiment (Phase 18.4) had also
made things worse. Practical rule: **leave the display profile at its factory setting**, and keep
using `tools/xe-res` instead of the Displays pane. The root cause is open. The second panic goes through the same
orphaned-memory code (`orphanIt`) as XE-NOORPHAN and the shutdown "corrupt list" panic.

---

## Phase 20 — Hardware video decode (20 Oct evening → 21 Oct)

**Goal.** Let VideoToolbox use the GPU's video engine (VDBOX) through Apple's TGL VA plugin, run in place by AppleGVA.

- **Loading and creating the decoder.** With `-ngreengva` the accelerator keeps its `IOGVA*` keys; XE-GVAID gives it the
  renderer id the plugin reports, and `hwgva-id` became the real board-id (AppleGVA only accepts AMD plugins on
  iMacPro1,1). `patch-vadriver.py` made the plugin load (device id, VAME name, bitstream request, driver parameters).
  Decoder creation then crashed on an NV12 output pool (driver parameter 14 unanswered) and failed with `-12913`: Tahoe
  reads capability nodes in the ICL layout, with a new field at +0x20 (`patch_nodes.py`, `patch_nodelayout.py`). Found
  with an in-process harness (`VTApplyRestrictions(1)` + a `DYLD_INSERT_LIBRARIES` library that redirects AppleGVA to a
  local plugin build and guards every GPU submission).
- **Empty frames.** Every decode batch was dropped by IOAcceleratorFamily2 ("commands on floor"). A log-only kernel
  hook (VADIAG) on `bind_resource` showed the kernel looking up handle 0x88: the plugin never committed the CodecHAL
  payload-header dword for decode (`je` at 0x64a15), so the first relocation entry was read as an indirect-batch handle
  (`patch_payloadhdr.py`; WhateverXe makes the same change). The pitch relocation was off by one (`patch_pitchdelta.py`).
- **Garbage and VCS0 hangs.** The batch now ran but produced garbage and hung VCS0 in the closing MI_FLUSH_DW.
  MI-only test batches, TLB/row-store experiments and a register survey ruled out coherency and the VDBOX setup;
  patching three dwords of MFX_AVC_IMG_STATE in-process gave a bit-exact frame. Tahoe's H.264 SPS structure has 8 extra
  bytes before `level_idc`, so the plugin read chroma 4:4:4, MBAFF and wrong frame_num/POC widths (`patch_spsshift.py`).
  H.264 then decoded bit-exact with no hang, also with BGRA output, and system-wide after installing the plugin.
- **HEVC.** AppleGVA only offers HEVC when the accelerator has `IOGVAHEVCDecode` (the ICL kext sets "2"; faked
  in-process). The HCP picture state was already right, but the plugin never added HCP_SURFACE_STATE to the batch
  (`patch_hcpsurface.py`) and Tahoe never sets LastSliceOfPic (`patch_hevclastslice.py`); both match WhateverXe's runtime
  shims. With both, 10 HEVC frames (I and B) decode bit-exact (Y and UV), no hang. The property needs a kernel change.
- **Safari** still decodes on the CPU: the `com.apple.WebKit.GPU` sandbox denies `file-map-executable` for
  `/Library/GPUBundles`, so the plugin cannot load in Safari's GPU process.
- **Reproducible.** `macos/vadriver/build-vadriver.sh` rebuilds the plugin from the stock bundle and reproduces the tested
  binaries byte for byte; the call tracer used during bring-up is now opt-in (`--trace`).

---

## Phase 21 — Stability, touch and E-cores (21 Oct)

- **XE-IDLEBOUND**: shutdown waited on `IGScheduler5::waitForGpuIdle` for a GPU that never idles (~3.5 min, then a
  watchdog panic). Bounding the waits brought it to ~11 s.
- **XE-RCPOLICY**: the renderCompression flag flipped on at the first mode set while AUX/resolves are off; forced to 0.
- **Brightness keys**: not a graphics bug. The OpRegion/ASLE theory was ruled out; the Dell SMM simply stops reporting
  the sun keys on some boots. A diagnostic that re-mapped GGTT entries to read physical memory hung the render ring
  once; tools that touch GPU page tables are now off-limits during live sessions.
- **Touchscreen**: `xe-toque` reads the digitizer with IOHIDManager and tells touchscreen events from trackpad events by
  the CGEvent sender ID (field 87); it adds one-finger scroll, right click on hold, edge swipes, pen and magnifier.
- **E-cores**: any enabled E-core froze the kernel right after EXITBS. Diffing the OpenCore logs showed the only change
  was ProvideCurrentCpuInfo's hybrid path setting cores-per-package to 128; this 4P+8E chip reports
  logical_per_package 64, so xnu divided by threads-per-core = 0. Spoofing EBX[23:16] = 0x80 in `Cpuid1Data` fixed
  it: 16 threads.

---

## Phase 22 — Chrome blits, the lock-screen clock and `w30r` (21 Oct)

**Chrome UI breakage (libxedrv v16).** Chrome (Graphite/Dawn) drew without toolbar icons, ignored `border-radius` and
drew the Google logo as a bar, and it looked tied to the GPU disk cache: a fresh cache often helped, `xe-chrome-limpa.sh`
and a locked cache folder lowered the odds. Several explanations were tested and dropped (EXPERIMENTS #70, #71): the
Metal per-app function cache (a test with an empty, redirected cache was still broken), Dawn toggles (results were
state-dependent, not toggle-dependent), XE-RCPOLICY and the newly enabled E-cores. The decisive tool was a capture
library injected into a test Chrome that recorded pipelines, bound buffers and draw arguments, plus a standalone replay:
the static **index buffer** of the rounded-rect draws was **all zero** on the GPU while its vertex buffer was correct.
Dawn zero-fills each new buffer (`fillBuffer`) and then uploads Graphite's static data with copies in the same blit
encoder. Small tests (`fillcopy`, `paths`, `fcoff`, `blitrender`) showed that the 16.0.32 driver runs fills and
64-byte-aligned copies on a 2D blitter path and the other copies on a 3D path, and that 3D work in a command buffer is
**not ordered** after earlier 2D writes in the same command buffer (11–17 of 20 wrong). A warm cache only made Chrome
start faster, so it lost the race more often. **libxedrv v16** never writes buffers on the 2D path (fills become copies
from a pattern buffer, aligned copies go through a misaligned temporary): a stress test went from 9/227 errors to 0, and
a test Chrome profile from 2/12 to 12/12 correct launches. The login → desktop flash disappeared with it.

**Lock-screen clock streaks (libxedrv v17).** SecurityAgent draws the Liquid Glass clock with SwiftUI's RenderBox
(R16F + RG16F colour targets, a Stencil8 stencil). A SwiftUI test app reproduced the streaks without locking the screen,
and a pass-dump library showed whole 4-row bands where the stencil test had failed. A freshly allocated stencil fixed
it; its GPU address showed why: the driver places `Stencil8` with **4 KB** alignment (everything else 64 KB), so the
stencil started inside the last 64 KB block of the RG16F target. A standalone repro failed 5/5 only when the colour
target was ≥ 32 bpp and shared the block. **v17** moves such stencils into their own 64 KB-aligned heap. Details:
[METAL.md](METAL.md) §3 items 6–7.

**`w30r` / BGR10_XR.** The extended-range 10-bit layer format was examined in three layers: Metal's own format
validation accepts the XR formats only on Apple-family GPUs, CoreAnimation maps `w30r` only when the device is Apple2+,
and Gen12 has no XR-bias surface format to sample. A proof of concept that flips the checks in a test process also
switches 10-bit video to Apple-only paths, so nothing was shipped (EXPERIMENTS #72). Real Intel Macs do not composite
`w30r` layers either.

---

## Phase 23 — Boot screen and boot speed (21 Oct afternoon)

**Goal.** Remove three glitches seen while booting: (1) a bright line early on, (2) a stretched low-resolution stage,
and (3) about one second of garbage when the display switched to the native mode.

- **Diagnostics first.** A NootedGreen build with XE-BOOTDIAG (log only) recorded Apple's writes to plane A, FBC, PSR,
  CDCLK and backlight registers with timestamps, and dumped the kernel console description around the framebuffer
  start. The first boot (16:17) killed the FBC theory: frame-buffer compression was never enabled (EXPERIMENTS #73).
- **Glitch 2** was the 1024×768 GOP mode stretched to the panel. OpenCore `UEFI>Output>Resolution=Max` removed it, and
  glitch 1 disappeared in the same boots (a CDCLK reprogramming theory was left untested, #75).
- **Glitch 3** was Apple's first mode set: it re-declares the linear boot buffer as X-tiled and arms it with
  `SURF=0`, then shows WindowServer's buffers before they have been drawn. **XE-BOOTHOLD** went through four versions
  in one afternoon (#74): v1 kept the plane off until the first real surface, v2 until the second, v3 until a surface
  address repeated (the buffer ring had wrapped); the flash remained, because part of it was not plane-A scanout. **v4**
  also keeps the backlight off during the hold and releases it only after the ring wraps and at least 600 ms
  (`xebbms`) have passed. Result: the logo is sharp from the first frame and nothing flashes.
- **The 20 s stall.** With `Resolution=Max` WindowServer's "Creating FB 1 of 3" step took ~22 s instead of 2–6 s.
  CoreDisplay's `needs_refresh = NO` appeared exactly in the Max boots, which pointed at IOGraphics' "keep the boot
  logo" path. A NootedGreen route on `IOFramebuffer::initFB` (XE-MAXRES) **panicked at boot** twice: WhateverGreen
  already routes that function, and Lilu refuses a second route on it (#77). Reading WhateverGreen's source found the
  real cause: its default `gfxrst` mode copies the whole boot console framebuffer with the CPU through an uncached
  mapping on the first `initFB` — 36.9 MB at 3840×2400, ~18 s, versus ~1.6 s for 3.1 MB at 1024×768 (the 11.6× ratio
  matched the byte ratio). **`gfxrst=1`** (reset instead of copy) fixed it with a boot-arg only (#76).
- **Boot speed.** A timeline of one boot showed the remaining fixed costs: OpenCore file logging (~3–4 s; now off),
  the picker timeout (now 1 s), a 6.5 s wait for accelerator stamp 3 inside `IntelAccelerator::start` that always ends
  by timeout (a shortcut was built but not installed, #78), the Aux KC load of the TGL kexts at ~7.5 s, and up to 25 s
  of "reopen windows at login" before login items start.

---

## Phase 24 — Wi-Fi, haptics, sleep, power and side investigations (21–22 Oct)

- **Native Wi-Fi.** Upstream itlwm has no Tahoe build. Two forks existed: laobamac's AirportItlwm-Tahoe (links Tahoe's
  own IO80211Family/Skywalk, EFI-only change) and a legacy build that needs an OCLP-style root patch, breaks the system
  seal and must be re-applied after every update (#79). An open issue reported the same AX211 CNVi failing with the
  laobamac build, so it was tried as a reversible EFI change with itlwm disabled. It worked: native Wi-Fi menu, en0 is the
  Wi-Fi port, HeliPort retired. AirDrop/AWDL is not possible with either fork.
- **Touchpad haptics.** The Elan `04F3:31D1` HID descriptor has one host control, feature report 0x09 (vibration
  intensity plus one more byte), and none of the Microsoft haptic trigger/waveform usages. `xe-haptic` sets it
  (confirmed by feel; GET 0x09 returns nothing, so the value cannot be read back) and `xe-hapticbar` puts it in the menu
  bar. The vendor packages showed that the pad is an Aito haptic module behind the Elan HID endpoint; no command for an
  on-demand pulse was found (#82).
- **Sleep.** The power log had zero sleep attempts in 88 boots. The firmware is Modern Standby only (LPIT, `S0ID`), the
  DSDT hides `_S3`, and the TGL framebuffer cannot retrain eDP after a power cycle (the reason XE-FBDPMS only turns the
  backlight off). With macOS defaults a lid close would have started an untested real sleep. Chosen: no system sleep
  (`10-pmset-safe.sh`) and a small LaunchAgent, `xe-lid`, that turns the display off on lid close. Hibernation-only and
  an injected `_S3` were prepared but not tried (#80).
- **Power.** RAPL through XeShim: ~17 W package at PL1 while playing video, GT in RC6 ~90 % of the time, XCPM and HWP
  active. SMCProcessor does not attach because Lilu does not know CPU model 0xBA (no CPU temperatures in the SMC). The
  biggest idle consumer is the panel; display sleep is now on.
- **Safari decode.** The WebKit GPU-process sandbox never allows executable mappings from `/Library/GPUBundles`, while
  `/System/Library/Extensions` is allowed and is the second place the driver lookup tries. Sandbox overrides, symlinks
  and Safari Technology Preview cannot work; a sealed-volume root patch was prepared and left unapplied (#81).
- **iServices** readiness checked read-only (no identifiers recorded): SMBIOS consistent, APNs connected.
- **Camera** identified (OmniVision OV01A10 behind an Intel VSC on the IPU6); no work started.

---

## Phase 25 — Internal speakers and microphones: AudShim M1 → M7 (21–22 Oct)

**Starting point.** The audio function `8086:51CA` is an Intel cAVS 2.5 DSP in SOF mode. The speakers are two Realtek
RT1316 amplifiers on SoundWire links 1 and 2, the microphones an RT714 codec on link 0. macOS has no driver for any of
it, and AppleHDA is gone on Tahoe. The plan followed Linux's SOF driver step by step, with every hardware write
whitelisted in the kext, logged, and undone by a rollback on failure or client death, and each milestone run by hand
with `audctl` before the next was written.

| Milestone | AudShim | What it did | Result |
|---|---|---|---|
| M1 | 0.1.0 | read-only register dump through a user client (no write method, read-only BAR maps, FIFO windows refused) | config space fine, but PCI memory decode was off: every MMIO read refused |
| M1b | 0.2.0 | one 16-bit config write: Memory Space Enable | HDA registers readable (GCAP ISS 7 / OSS 9, capability chain with the processing pipe); the SoundWire shim reads 0 until the DSP is up |
| M2 | 0.3.0 | DSP core 0 power-up and ROM handshake (`ROM_CONTROL` IPC) | **failed twice**: IPC sent after the core ran → no answer; IPC first (Linux order) → the ROM took the message and the SRAM window went to `0xffffffff` (#84, #85) |
| M2b | 0.4.0 | the Linux prerequisites the ROM needs: controller reset cycle, `PPCTL` (GPROCEN/PIE), `EM2.L1SEN` off, bus mastering, the TGL-family **ICCMAX** capture stream with the LTR guardband, and the **code-loader stream** with the tag named in the IPC | ROM `INIT_DONE` (`0x05000001`, waiting for the DMA) on the first attempt. Before that, a pre-check had rejected this controller's 16 streams (#86); later it turned out the optional SSP write had been silently refused by the whitelist (#87) |
| M3 | 0.5.0 | firmware download over the code-loader DMA: Intel-signed SOF 2.2.0 IPC3 image (`sof-rpl.ri`, 512 KB `$CPD` payload after the extended manifest) | ROM states through CSE validation to `FW_ENTERED`, then **FW_READY**: SOF 2.2.0, ABI 3.22.1 |
| M4 | 0.6.0 | SoundWire link power-up, Cadence master start, enumeration driven from user space | RT714 on link 0, RT1316 on links 1 and 2, IDs as in the DSDT |
| M5 | 0.7.0 | `audctl tplg` parses `sof-rpl-rt1316-l12-rt714-l0.tplg` and builds the same 25 IPC3 topology messages Linux sends; RT1316 init, SoundWire stream on links 1+2, host DMA, `TRIG_START` | M5a (no audio) then M5b: **a 1 kHz tone at −30 dBFS on both speakers**, DMA at exactly 48 kHz, 0 xruns, 43 IPCs all error 0 |
| M6 | 0.8.0 | kext **driver mode** (the whole M3–M5 sequence in the kernel from a generated header, START/STOP with a 5 s deferred stop, −6 dB cap, PM-tree membership for teardown on sleep/shutdown) + the **XeSpeakers** AudioServerPlugIn (shared 64 KiB ring, DMA-position clock) | first install: the plug-in loaded and the hardware came up, but **coreaudiod dropped the device** — it queries several device properties in the input scope too, which the plug-in answered only for output (#88). Fixed: system sounds and music play through System Settings › Sound |
| M7 | 0.9.0 | the **microphones**: RT714 (Linux `rt715-sdca`) attach/hw_params/power sequence, SoundWire link 0 data port 4 → master port 2 (PDI 2, ALH stream 7) with the bank switch on one link, SOF capture pipeline 5 (ALH → EQ IIR high-pass → PGA → host PCM4) on host capture stream SD0, kext ops `CAP_START`/`CAP_STOP` independent of playback; XeSpeakers 0.9.0 adds the input device **"Microfone interno"** (software gain 0 to +30 dB) | first hardware run: 5 s at 48 kHz, 0 xruns, speech and claps recorded on both channels, speakers unchanged |

**Why M2 failed.** In run 2 the ROM reacted to `ROM_CONTROL | PURGE_FW | tag 1` and then its SRAM window stopped decoding:
it went looking for a host DMA stream with that tag, which did not exist. Every working implementation (Linux, and two
earlier community macOS experiments on older cAVS) prepares that stream, enables bus mastering and resets the controller
before the IPC; Linux additionally runs the ICCMAX stream on TGL-family parts. M2b added all of them at once, with
switches to bisect them later; it never needed bisecting.

**Firmware and data.** The firmware and topology are Intel's, from the official sof-bin release; the repository ships
neither. `audctl gen-header` turns them into `AudShimData.h` (firmware payload, IPC messages, SoundWire parameters, the
RT1316 init table), which the kext compiles in.

## Version index

### Metal driver (`AppleIntelTGLGraphicsMTLDriver.bundle`)

| Version | Date | Change | Outcome |
|---|---|---|---|
| stock in `/Library/Extensions` | 18 Oct | — | bundle loads, `MTLCopyAllDevices = 0` |
| stock in `/Library/GPUBundles` | 18 Oct 01:44 | — | MTLDevice created; "Unsupported platform" in `getICBFile` |
| v1 | 18 Oct 02:03 | plugin path → `/Library/GPUBundles////////…` | AMFI rejects; then dlopen fails (LLVM 3902); then sandbox |
| v2 | 03:03 | → `/System/Library/Caches/xe//…` | sandbox OK; "not registered" |
| v3 | 03:18 | clean, NUL-padded | same |
| v4 | 03:36 | → `/System/Library/PrivateFrameworks/../Caches/xe/Contents/MacOS/…` | same (no `.bundle`) |
| v5 | 03:53 | → `/System/Library/PrivateFrameworks/../Caches/xe.bundle/Contents/MacOS/libMTLIGCCompilerPlugin.dylib` | **registered** (kept in all later versions) |
| v6 | 11:21 | 13 descriptor-offset displacement patches | past VF; crash in `generateBlendState`; superseded |
| v7 | 11:57 / 13:30 | v6 reverted; `LC_LOAD_DYLIB` libxedrv | runtime descriptor conversion |
| v8, v9 | 16:31, 17:24 | binary unchanged; libxedrv v8, v9 | ABI fixes; crash recorder |
| v10 | 17:34 | `Info.plist` `Development.PoshDisable = 1` | POSH crash gone |
| v11 | 22:17 | byte `0x88→0xd0` at 0x65f12, 0x7ff77, 0x80253 | L3 not the cause; **two of the three bytes corrupted instructions** (Phase 19) |
| v12 | 23:15 | libxedrv v12 | no effect |
| MTL13 | 19 Oct 12:36 | `generateClearRequirements:` `setne` → NOP (0x1c7c0, 0x1c7cf) | no fast clear |
| MTL14 | 12:48 | `writePSState` `setne` → NOP (0x49cfe) | no dual SIMD8 in clears; no effect |
| MTL15 | 13:19 | IOSurface compression flag zeroed (0x18c28) | first desktop login; not causal |
| MTL16 | 13:42 | `programClearPipeline` always emits disabled VS (0x67c27) | RCS hangs gone; empty windows |
| MTL17 | 13:53 | VS marked dirty after a clear (0x67c5c `jne`→`jmp`) | windows with content; release build 20 Oct |
| MTL18 | 20 Oct | MTL17 with 0x7ff77 and 0x80253 restored to `0x88` | in test |

### libxedrv

| Version | Date | Change |
|---|---|---|
| v7 | 18 Oct 13:30 | descriptor shadowing for driver callers, missing selectors, capability overrides, `releaseHashMaps` leak |
| v8 | 18 Oct 16:30 | shift from `0xa8`, synchronous install, reject dynamic libraries, layout preflight, lab mode (abandoned) |
| v9 | 18 Oct 17:23 | crash recorder |
| v12 | 18 Oct 23:15 | `allowGPUOptimizedContents = NO` on every texture (ineffective, kept) |
| v13 | 20 Oct 16:17 | texture logging to `/tmp/xedrv-<process>.log` |
| v14 | 20 Oct 16:25 | pipeline-creation logging |
| v15 | 20 Oct 16:37 | **NOCOPY**: copy textures whose zero-copy offset is not 256-byte aligned (`XE_NOCOPY_ZERO=1` disables) |
| v16 | 21 Oct | buffer fills and aligned buffer copies moved off the driver's 2D blit path (not ordered against later 3D work in the same command buffer): the Chrome fix (`XE_FILL_NATIVO=1`, `XE_COPIA_NATIVA=1` disable) |
| v17 | 21 Oct | private render-target Stencil8 textures re-created 64 KB-aligned: the lock-screen clock fix (`XE_STENCIL_NATIVO=1` disables) — current |

There were no libxedrv v10/v11 (MTL v10/v11 bundle v9).

### Shader compiler (`/System/Library/Caches/xe.bundle`)

hybrid (Tahoe plugin + TGL `libigc`; `m_pReadBitcodeFn` assert) → `xe-old` (old plugin + bridge v1) → `xe-new` (adapter
+ old plugin with 11 resource paths patched and POSH off + bridge, fake ICB helper, crash containment) → bridge v1 with
`getSerializedModule` at a fixed offset → **bridge v2** (signature scan; every shader compiles).

### Kernel (`XE-*`), in order of introduction

keepefi, pipe-width stride, xtile, stride, max2560, xefix display-13 fix (DPT/DBUF/GAMMA), PSMI, fixbootmode + timings
worker, fixbootpatch, engineinert, LRCA repair after start, NOCCS, NOCCS2, NOCCS3, DPCAPS, VALD, divide guard, FMT,
V99 off, POOL, POOLALIGN, FLIPDIAG, SCHEDDIAG, `ngreenV142=3`, V193 off, BLITDIAG, RINGDIAG, CSSTATE, CSBDIAG,
EVALDIAG, FWREF, FWHOLD, FWSUBMIT, ELSQ, V509 logs, POSH, ELSQWAIT, NOPOSH, ELSQDELAY, ELSQIDLE (IDLW), ELSQRETRY,
NOPREEMPT, POSHCTX, IDLEANY, NOAUX, NOTCF, NOTCF2, BLIT16, BLITSCRATCH, LOGQUIET, BLITSCRATCH2, PLANES, QUIETLOG,
CSBGUARD, CSBGUARD2, NOPAGE, PAGEONGUARD, RPCS, SAMETAIL, POOL2, BLR, NODEPTH, RESFREE, GTFREQ, NOORPHAN, PLANES2,
NOGVA, FBDPMS, BLITALIGN (v1, v2), PAGESYNC, GVAID, GVAHEVC, IDLEBOUND, RCPOLICY, csbresync, BOOTDIAG, BOOTHOLD (v1–v4).
Their current defaults are in [CATALOG.md](CATALOG.md) §1 and §3.2 and [KERNEL-PATCHES.md](KERNEL-PATCHES.md).

### Audio (`tools/AudShim`, `tools/audctl`, `tools/XeSpeakers`)

| AudShim | Milestone | User-client interface | New capability |
|---|---|---|---|
| 0.1.0 | M1 | v1 | read-only config/MMIO reads |
| 0.2.0 | M1b | v2 | PCI Memory Space Enable at start |
| 0.3.0 | M2 | v3 | whitelisted DSP power-up and ROM IPC (failed) |
| 0.4.0 | M2b | v4 | controller reset, PPCTL, L1SEN, bus mastering, ICCMAX + code-loader streams: ROM INIT_DONE |
| 0.5.0 | M3 | v5 | firmware download, FW_READY |
| 0.6.0 | M4 | v6 | SoundWire links, enumeration |
| 0.7.0 | M5 | v7 | IPC3 messages, RT1316 init, playback: test tone |
| 0.8.0 | M6 | v8 | driver mode for the XeSpeakers HAL plug-in (0.8.0): macOS output device |
| 0.9.0 | M7 | v9 | capture: RT714 microphones, SOF pipeline 5, read-only capture ring; XeSpeakers 0.9.0 input device |

---

## Lessons learned

1. **Prove each layer before the next.** The work moved framebuffer → DisplayPipe → MetalDevice → compiler location →
   compiler ABI → driver ABI → GPU execution → flips → command pools → scheduler → individual draw hangs → resources.
   Each "MetalDevice" or "pipeline" error was only the topmost symptom.
2. **Establish where a hang is before guessing why.** PTWALK, SURFWALK, RTDUMP, LRCSCAN and Mesa-genxml batch decoding
   turned theories into facts: the GPU did write the stamps; work was never submitted; the Blit3D kernels were zeros.
3. **Old workarounds in the fork were often the bug.** V99 surface slots, V142/V170 and V193 silent blit drops, V148
   skipping the Blit3D upload, the `-ngreendp0` capability dictionary — each hid an earlier symptom whose real cause was
   later fixed elsewhere. So was one of ours (XE-PLANES → green YUV).
4. **Tahoe moved private offsets the 2020 driver depends on:** IOSurface pixel format (`+0xa4` → `+0x80`) and plane
   count (`+0xb0` → `+0x98`), the Metal pipeline descriptor (+0x10 from `0xa8`), the compiler entry point and the
   never-called `MTLCompilerSetHelper`, command-buffer pool sizes. IOAF2 vtables, by contrast, are identical to Big Sur's.
5. **Metal plugin loading on Tahoe has four gates:** bundle location (`/Library/GPUBundles`), AMFI library validation,
   the per-process sandbox (MTLCompilerService reads `/System`; WindowServer maps executables from `/Library/GPUBundles`),
   and the plugin path rules (prefix list **and** a `.bundle`/`.framework` substring).
6. **Never log from interrupt context**, and take spinlocks with interrupts disabled when an interrupt path can reach
   them. Several NMIPI panics and silent freezes came from diagnostics.
7. **Diagnostics perturb the system:** the verbose console overwrote GPU contexts; burst forcewake desynchronised the
   CSB; a log flooded dmesg with 860 000 lines; the boot-diag script changed brightness and dumped 128 MB.
8. **Read GT registers only with forcewake held**; in RC6 they read 0.
9. **Change one thing per boot**, and keep every step reversible (EFI backups per experiment, install/restore script
   pairs, a stable "display only" state, Safe Mode as the way back).
10. **Mind the boot-args limit (~228 characters)** and check what the kernel actually received.
11. **Gen12 compression features (CCS/AUX, HiZ resolves, fast clear) are hazardous** on this bring-up; several were
    disabled even where they turned out not to be the cause of a particular hang.
12. **Byte patches must be checked as instructions, not as byte strings.** Replacing every occurrence of a dword pattern
    silently corrupted two instructions in MTL v11 (Phase 19).
13. **Leave disproved hypotheses written down:** GPU DMA corruption, forcewake, "3D state lost on context switch",
    POSH_LRCA, Tile Cache Flush, SIMD32, L3ALLOC, PSMI, topology — each was tested and recorded as not the cause.
14. **A correlation is not a cause.** Chrome's breakage followed the GPU cache, and the 20 s boot stall followed
    `needs_refresh = NO`; both were timing side effects of something else (a blit race, a CPU copy in WhateverGreen).
    Capture-and-replay and reading the other kexts' source found the causes.
15. **Check who else hooks a function before routing it.** WhateverGreen already routed `IOFramebuffer::initFB`; a second
    Lilu route panics at boot. A boot-arg of the existing hook (`gfxrst=1`) was the whole fix.
16. **Bring up unknown hardware the way Linux does, step by step, with a rollback for every write.** Each audio milestone
    ran by hand with a dry-run plan, a whitelist in the kext and an automatic undo; the one failure (M2) was diagnosed
    from what the ROM did to its SRAM window, and the missing prerequisites came straight from the Linux SOF driver.
17. **The OS-facing layer matters as much as the hardware path.** The speakers worked from the kext long before macOS
    showed the device: coreaudiod drops a device if any property it asks for, in any scope, is unanswered.
18. **Know when the platform says no.** S3 is hidden by the firmware, AirDrop needs Apple's Wi-Fi stack, and the haptic
    module has no pulse command; documenting the limit and the safe workaround (display-off on lid close) was the result.

## Open items

- System Settings › Displays and colour-profile changes panic in GPU memory management; some mode switches go black
  until reboot (use `xe-res`).
- "corrupt list" panic at shutdown (`IOAccelSysMemory::free → IOAccelOrphanedMemoryPool::addMemory`), rare.
- Occasional WindowServer hang before login / on UI-scale changes (csbresync in testing); the c01dbead Blit3D hang was
  never root-caused.
- Video: Safari cannot load the VA plugin (WebKit GPU-process sandbox) and decodes on the CPU; the root patch that would
  fix it is prepared but not applied. Hardware encode untested.
- Brightness keys and lid events: on some boots the EC/Dell SMM does not report the sun keys or the lid (firmware state);
  `xe-brilho daemon` covers F6/F7, nothing covers the lid on those boots.
- No system sleep (Modern Standby firmware, eDP retraining); hibernation-only is the untested next experiment.
- Audio: no speaker protection (−6 dB cap instead); the DSP stays powered while a device is open; power cost not measured.
- 6.5 s accelerator stamp-3 wait at every boot (shortcut built, not tested); SMCProcessor does not attach (Lilu does not
  know CPU model 0xBA).
- Camera (OV01A10 + Intel VSC + IPU6) and fingerprint reader not started; `w30r`/BGR10_XR stays Apple-silicon-only.
- MTL18 verification; a fixed GPU frequency floor (1500 MHz request) instead of a real governor.
