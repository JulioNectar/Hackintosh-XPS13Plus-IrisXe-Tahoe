# Debugging notes

Most bugs were found by reproducing them **outside** WindowServer with small programs in `tools/diagnostics`
(build each with the `clang` line in its header comment).

| Tool | What it checks | Bug it found |
|---|---|---|
| `xe-yuvtest` | GPU reads both planes of a 420v IOSurface (each 256-byte row carries its index) | PLANES2: chroma plane read as zero |
| `xe-vttest` | VideoToolbox H.264 encode → decode, HW and SW, with/without output pixel format | XE-NOGVA, the `-6680` VideoToolbox failure |
| `xe-carender` | composites layers with **CARenderer**, the same engine as WindowServer, into a Metal texture and measures each layer | the invisible-icon bug, reproduced in a normal process |
| `xe-encspy.dylib` | injected (`ESPIAO=a.dylib:b.dylib xe-carender`): logs per draw the pipeline (base function + specialization constants) and the bound textures (format, buffer, offset, center pixel) | showed the failing textures all start at offset `0x20` |
| `xe-nocopyfix.dylib` | the copy-on-unaligned-offset fix as an injectable test | proved the fix before it went into libxedrv |
| `xe-layertest` | window with CALayers backed by IOSurfaces of different formats (`VER=300` keeps it open) | `w30r` is not composited |
| `xe-f16test`, `xe-atlastest`, `xe-miptest`, `xe-bufoff`, `xe-fbfetch`, `xe-vfmttest` | uploads/copies of 64/128 bpp textures, atlases, mipmaps, buffer textures, framebuffer fetch, vertex formats | ruled out (all correct) |
| `xe-icontest` | IconServices icons rendered on the CPU | icons themselves are fine |
| `mtl-diff.py` | diff of two Mach-O files outside the code signature | built the reproducible MTL17 patcher |
| `kcsyms.py`, `kcdis.py` | symbols and disassembly of a kext inside a Kernel Collection | symbolicating Tahoe panics |

## Getting logs

- Kernel: the `XeNG CS ...` status line (every 250 ms) with counters `csbx`, `idlw`, `pgfix`, `stail`, `resfix`,
  `orf`, `gtf`, `bl ...`. `log show` needs admin rights on Tahoe; a root LaunchDaemon that dumps `dmesg` is handy.
- User space: `/tmp/xedrv-<process>.log` from libxedrv (textures, pipeline failures, crashes).
- Panics: `/Library/Logs/DiagnosticReports/*.panic`. Map frames to `AppleIntelTGLGraphics` offsets with `nm -n` on
  the 16.0.32 binary, and frames in IOAcceleratorFamily2 with `kcsyms.py SystemKernelExtensions.kc
  com.apple.iokit.IOAcceleratorFamily2`.
- GPU registers: `tools/xectl` through XeShim (`xectl rdw` and `wrw` hold forcewake). Example: GPU frequency is
  `RPSTAT1 (0xA01C) >> 23` × 16.67 MHz.
