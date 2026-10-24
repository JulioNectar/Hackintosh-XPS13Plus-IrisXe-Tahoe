# H.264 hardware encode: work in progress (not part of the build)

These patches apply, in this order, to the VA plugin that `../build-vadriver.sh` produces
(sha256 `6291f574…`). They are **not** applied by `install.sh`, because the result is not safe to install yet (see
"Open problems"). Each script checks the original bytes before writing.

| Patch | What it does |
|---|---|
| `patch_vphal.py`, `patch_vpnull.py`, `patch_vpnull2.py` | Let the VPHAL video processor create its context (kernel-table size) and skip the NULL sub-renderers it touches. Needed because AppleGVA's encoder creates a scaler when the session opens |
| `patch_encsps.py` | Tahoe's H.264 SPS layout on the encode side (same shift as `patch_spsshift.py` for decode) |
| `patch_nocksum.py` | Hides the FrameChecksum capability: its EU media kernel hangs the render engine on the first frame |
| `patch_nowe.py` | Hides the WeightEstimate capability: its EU media kernel hangs the render engine from the second P frame |
| `patch_vdencfix.py` | Gen12 VCS status registers (`0x1c08a0…` instead of the Gen9/11 `0x128a0…`; without it every frame is "dropped") and the VDENC surface format (PLANAR_420_8 in the VDENC layout; without it the picture is read as RGBA 10:10:10:2 and comes out stretched 4×) |
| `patch_vdencintra.py` | Interim: VDENC uses its intra template for every frame. The PAK still writes P slices |
| `patch_noscaler.py` | Experiment: hides the VPHAL scaler capability (see below) |

The accelerator must also advertise `IOGVAH264EncodeCapabilities` (`nootedgreen-ngreenenc.diff`, opt-in boot-arg
`-ngreenenc`).

## What works (tested 2026-10-24 on the XPS 13 Plus)

- Low-latency sessions (`kVTVideoEncoderSpecification_EnableLowLatencyRateControl`, as FaceTime and streaming apps
  use), through the system encode service: 1280×720 and 60/60 frames at 4 Mbit/s, Y-PSNR 52.8 dB against the source,
  no GPU hang. VideoToolbox picks the hardware encoder by itself.
- Ordinary sessions (usage 0, e.g. export) fail inside AppleGVA (they need the VME path, whose EU kernels hang this GPU)
  and VideoToolbox falls back to the software encoder.

## Open problems

1. **VPHAL in the whole system.** With the VPHAL patches, `VTPixelTransferSession` (AVFPixelProcess) also starts using
   the GPU video processor for every app, and it crashes on 1920×1080 `420v` buffers. Hiding the scaler capability
   fixes that but then the encoder refuses to open (it needs at least one scaler). The encoder uses the scaler entry at
   `__DATA 0x78fc00`, pixel transfers use `0x787a30`; keeping only the former still crashes in VPHAL (`fn 0x126520`).
   This is what blocks installing.
2. **Inter prediction.** Real P frames hang VCS0 in `MI_FLUSH_DW` right after the first slice. With the VDENC forced to
   intra the same batch does not hang, so the problem is in the VDENC reference path; the VDENC commands are in the
   Gen11 (Ice Lake) layout. Addresses of FwdRef0 / DsFwdRef0 are valid; disabling TLB prefetch, sub-pel search or BME
   does not help.
3. The low-latency rate control drops frames at low bitrates, because intra-only P frames are larger.
4. Each GPU hang is recovered (XE-GPUREC v2 for the render engine, Apple's reset for VCS), but several in a row leave
   the desktop sluggish until a reboot.
