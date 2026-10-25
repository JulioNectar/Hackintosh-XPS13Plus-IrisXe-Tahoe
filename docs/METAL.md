# User-space stack (Metal driver, libxedrv, shader compiler)

Everything here is built by `macos/build-all.sh` from Apple's 16.0.32 bundles plus the sources in `macos/src`, and
installed by `macos/install-macos.sh`.

**Version history.** The Metal driver went through builds v1–v12 and MTL13–MTL17 (18–19 Oct); **MTL18** (20 Oct) is
MTL17 with two bytes restored (see below), and **MTL17 patch v18** (25 Oct, the name `patch-mtl17.py` uses) adds the
command-buffer leak fix at `0x07ed87`. The two "18"s are different changes; the bundle built today contains both.
libxedrv went through v7–v19 and the LLVM bridge through v1–v2. Every build, with its evidence and result, is in
[HISTORY.md](HISTORY.md) and [CATALOG.md](CATALOG.md) §4; abandoned approaches are in [EXPERIMENTS.md](EXPERIMENTS.md).

```
/Library/Extensions/AppleIntelTGLGraphics*.kext, *.bundle      Apple 16.0.32 (accelerator Info.plist edited)
/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle       Metal driver, patched ("MTL17") + libxedrv.dylib
/Library/GPUBundles/AppleIntelGraphicsShared.bundle             Apple 16.0.32 (profiler and runtime files)
/System/Library/Caches/xe.bundle                                 shader compiler stack
/Library/GPUBundles/AppleIntelTGLGraphicsVADriver.bundle        video decode plugin, patched (macos/vadriver/, optional)
```

## 1. Why /Library/GPUBundles and /System/Library/Caches

- Tahoe's Metal looks for third-party GPU bundles in `/Library/GPUBundles`.
- The shader compiler runs inside `MTLCompilerService`. Its sandbox only reads `/System`, and
  `MTLCompilerPluginInterface` only registers plugins whose path starts with `/System/Library/Extensions/`,
  `/System/Library/PrivateFrameworks/` or `/Library/GPUBundles/`. `/System/Library/Caches` is a writable firmlink
  inside `/System`, and the driver refers to the plugin as
  `/System/Library/PrivateFrameworks/../Caches/xe.bundle/...`, which satisfies both rules.
- `amfi_get_out_of_my_way=1` is needed for ad-hoc signed code in these processes.

## 2. Metal driver patches (MTL17)

`macos/patch-mtl17.py` + `mtl17-patches.json` turn the stock binary into the current build: MTL17 (verified byte for byte
against the 19 Oct build, zero differences outside the code signature) minus the two MTL18 bytes, plus the
`0x07ed87` leak fix (patch v18). The table lists what is applied today.

| Offset | Change | Reason |
|---|---|---|
| header | + `LC_LOAD_DYLIB` `.../libxedrv.dylib` | load the adapter in every Metal process |
| `0x0a2e7b` | compiler plugin path → `xe.bundle` | see above |
| `0x065f12` | L3 allocation immediate `0x88000020` → `0xd0000020` (v11) | RPL-P reset value; not the cause of the hang it targeted, kept |
| `0x01c7c0`, `0x01c7cf` | `setne` → NOP in `generateClearRequirements:` (MTL13) | no render-target fast clear (needs the AUX table) |
| `0x049cfe` | `setne` → NOP in the clear `writePSState` (MTL14) | no dual-SIMD8 clear PS |
| `0x018c28` | `mov ecx,[rax+0x18]` → `xor ecx,ecx` (MTL15) | ignore the kernel's IOSurface compression flag |
| `0x067c27` | `je` → NOP in `programClearPipeline` (MTL16) | a clear always emits a disabled VS (it inherited the previous VS → hangs) |
| `0x067c5c` | `jne` → `jmp` (MTL17) | after a clear the VS is marked dirty, so the next draw re-emits its own |
| `0x07ed87` | `jne` → NOP in `-[MTLIGAccelCommandBuffer dealloc]` (patch v18, 25 Oct, [#12](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/12)) | the buffer's 96 KB POSH block was never freed when it was the queue's only active buffer (the usual case), so WindowServer grew by GBs. The queue's shared POSH pointer is never set, so every buffer owns its block and freeing it is safe. Test program: 3000 command buffers leave 0 blocks (before: 1 per buffer) |
| Info.plist | `Development.PoshDisable = 1` | no POSH pipe on RPL |

**MTL18.** v11 originally replaced the same byte at `0x07ff77` and `0x080253` too, but those are not L3 values: in
`IntelState::IntelState()` the byte is the opcode of `movb %al,0x110(%rdi)` / `movb %cl,0x8e0(%rbx)`, which the patch
turned into `rolb`/`rorb`. The repository's `mtl17-patches.json` **excludes those two bytes** (since 20 Oct), so every
bundle `patch-mtl17.py` has built since then is MTL18 plus, from 25 Oct, the `0x07ed87` leak fix. No problem has been
attributed to the change, but no test isolating its effect was recorded either. (The docstring of `patch-mtl17.py` still
lists `0x07ff77`/`0x080253`; `mtl17-patches.json` is what is applied.) Details: [CATALOG.md](CATALOG.md) §6.1.

## 3. libxedrv (macos/src/libxedrv.m)

An ObjC dylib loaded into every Metal client (WindowServer included) by the driver. It:

1. **Descriptor ABI**: converts `MTLRenderPipelineDescriptorPrivate` and color attachment bitfields from Tahoe's
   layout to the 16.0.32 layout before the driver reads them.
2. **Missing methods**: adds the Tahoe-era selectors the old driver lacks and rejects features it can't do
   (dynamic libraries, binary archives).
3. **No compression**: every texture is created with `allowGPUOptimizedContents = NO` (fast clear and compressed
   stencil need the AUX table).
4. **Unaligned zero-copy textures (v15, the "missing icons" fix)**: Core Animation wraps image memory with
   `-[MTLIOAccelDevice newTiledTextureWithBytesNoCopy:…offset:bytesPerRow:]`. For images that are already in the
   destination color space the offset is `0x20` (the image buffer header), and the GPU can't sample a linear texture
   at that offset, so the layer comes out invisible. That hid app icons in Finder, DMG window backgrounds and any
   extended-range content. When the offset is not 256-byte aligned, libxedrv creates a texture of its own and
   **copies** the pixels. The caller's deallocator still runs when the texture is released.
5. **Diagnostics**: per-process log in `/tmp/xedrv-<process>.log` with texture formats, pipeline failures and the
   crash handler. Lines are throttled.
6. **Buffer writes on the 3D blit path (v16, the Chrome fix)**: the driver runs `copyFromBuffer:toBuffer:` and
   `fillBuffer:` on one of two paths, chosen by `IGBL::shouldUse2DBlit` (driver `+0x8bf48`): the **2D** path (blitter
   commands in their own segment of the command buffer) for fills and for copies where
   `(srcOffset - dstOffset) % 64 == 0`, and the **3D** path (shader copy) for the other copies. On this system 3D work
   in a command buffer does **not** wait for earlier 2D writes in the *same* command buffer: a fill or aligned copy
   followed by a misaligned copy or a draw that reads the buffer races (fill→misaligned copy 11–17/20 wrong, aligned
   copy→draw 17/20 wrong; across separate command buffers it is fine). Dawn zero-fills every new buffer with
   `fillBuffer` and then uploads Graphite's static vertex/index data with mostly misaligned copies in the same blit
   encoder, so the zero fill often landed last: the static **index buffer stayed all zero** and every rounded rect,
   path and stroke drew nothing (missing toolbar icons, `border-radius` ignored, the Google logo as a bar). It was a
   per-launch race; with a populated cache Chrome starts faster and lost it almost every time, which is why it looked
   cache-related. v16 never writes buffers on the 2D path: `fillBuffer:range:value:` (and `pattern4:`) becomes a copy
   from a shared pattern buffer with a relative offset of 4, and an aligned buffer→buffer copy becomes two misaligned
   copies through a pooled temporary buffer (`src → tmp[(so+4)%64] → dst`). Everything then runs on the 3D path, which
   is ordered. Tests: `~/xe/build/chromecache/tests` (`fillcopy`, `paths`, `fcoff`, `blitrender`, `bigcopy`).
   The underlying 2D↔3D segment fence is a kernel-side gap (BCS/RCS sync) and still applies to 2D texture blits,
   which have not been seen to fail.

7. **Stencil8 render targets aligned to 64 KB (v17, the lock-screen clock fix)**: the 16.0.32 driver places private
   `Stencil8` textures with 4 KB alignment, although every other tiled render target (and the driver's own
   `heapTextureSizeAndAlignWithDescriptor:`) uses 64 KB. A stencil that starts inside the last 64 KB block of a
   ≥32 bpp colour target used in the same render pass loses its writes on every odd 64-byte line up to the next 64 KB
   boundary (standalone repro: 5/5 failures; a 16 bpp colour target or a stencil in its own block is fine). SwiftUI's
   RenderBox draws the Liquid Glass clock digits exactly that way (R16F + RG16F + Stencil8), so the stencil test failed
   in 4-row bands: streaks above the digits. v17 re-creates a private render-target `Stencil8`/`X32_Stencil8` texture
   that is not 64 KB aligned in its own `MTLHeap` (size rounded up to 64 KB), so it owns whole blocks (log line
   `STENCIL64K`). The likely hardware reason (64 KB units for ≥32 bpp colour targets) is inferred, not documented.

8. **IOSurface textures with ≥32768 bytes per row (v18, the black login wallpaper)**: the kernel never uses an IOSurface
   texture in place; the GPU works on a Y-tiled copy and `IGAccelResource::pageon`/`pageoff` copy between the two with
   `IntelAccelerator::submitBlit`. The 2D blit takes linear pitches up to 0x7ffe only; wider ones go to the 3D blit,
   which does not work on this GPU, so the GPU reads zeros and its writes never come back, with no error. Any plane of
   32768 bytes per row or more is affected: a half-float RGBA frame ≥4096 px wide, which is what macOS renders from a
   10-bit Aerial for the login-screen wallpaper (all black). v18 creates such textures (2D, one mip, one sample, pitch
   and offset aligned to `minimumLinearTextureAlignment`) as a linear texture over a no-copy `MTLBuffer` that aliases
   the IOSurface's own memory, so no copy is needed (log line `PITCH LARGO`). Repro: 18 of 136 pitch cases failed
   before, 3 after (the remaining 3 use a pitch that is not 256-byte aligned and keep the old path).

9. **Mip levels for zero-copy textures (v19, the Dock badge digits, [#10](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe/issues/10))**:
   for layers with `minificationFilter = trilinear` Core Animation creates the zero-copy texture with
   `newTiledTextureWithBytesNoCopy` and `mipmapLevelCount > 1` (108×108 → 7 levels) over image memory that only holds
   level 0. The 16.0.32 driver never fills levels 1+, so the trilinear sample read empty levels and the layer (Dock badge
   numbers, other downscaled images) was invisible. v19 creates its own texture, uploads level 0 and generates the
   others with `generateMipmapsForTexture:` before returning it. Reproduced in-process with `CARenderer`.

Environment switches: `XE_COMPRESSAO=1` keeps compression, `XE_NOCOPY_ZERO=1` disables fix 4, `XE_FILL_NATIVO=1`
disables fix 6, `XE_COPIA_NATIVA=1` keeps aligned buffer copies on the 2D path (fills stay fixed),
`XE_STENCIL_NATIVO=1` disables fix 7, `XE_NOWIDEPITCH=1` disables fix 8, `XE_MIP_NATIVO=1` disables fix 9.

## 4. Shader compiler (xe.bundle)

Tahoe's `MTLCompiler` calls `MTLCompilerBuildRequestWithSerializedBitcode` with LLVM 32023 bitcode. The only Gen12LP
compiler is Apple's 16.0.32 IGC (`libigc.dylib`), whose plugin exports `MTLCompilerBuildRequestWithOptions` and
links LLVM 3902.

| File | Origin |
|---|---|
| `libMTLIGCCompilerPlugin.dylib` | ours, `src/compiler-adapter.cpp`: maps the new entry point (argument layout taken from disassembly) and re-exports the old plugin |
| `libMTLIGCCompilerPluginOld.dylib` | Apple's plugin: `install_name_tool` (LLVM → bridge) + 12 byte patches (resource paths → `xe.bundle`, `isPoshEnabled` → false) |
| `libLLVMbridge.dylib` | ours, `src/llvm-bridge.cpp`: re-exports Tahoe's libLLVM and provides the few LLVM 3902 symbols the old plugin imports (`parseIRFile`, `WriteBitcodeToFile`, …) |
| `libigc.dylib`, `Resources/*` | Apple 16.0.32, re-signed |

`build-shader-compiler.sh` regenerates the libLLVM stub from the running system (`dyld_info -exports`).

## 5. Video decode plugin (macos/vadriver/)

`macos/vadriver/build-vadriver.sh <stock VADriver bundle or sle/>` copies Apple's 16.0.32
`AppleIntelTGLGraphicsVADriver.bundle`, applies the patch scripts in order (each one checks the original bytes first)
and ad-hoc signs the result. AppleGVA only loads it from `/Library/GPUBundles`, and only when the accelerator keeps the
`IOGVA*` keys (`-ngreengva`). Code caves sit in the zero padding after the load commands (r-x `__TEXT`, 0xb00–0x1100).

| Step | Script | Problem on Tahoe / RPL | Fix |
|---|---|---|---|
| 1 | `patch-vadriver.py` | device-id whitelist, accelerator lookup, Tahoe no longer fills the bitstream request (`bwar`), unknown driver parameters, the plugin dlopens the *ICL* VAME | 9A49 accepted, lookup by name, checks skipped, unknown parameters skipped, VAME name ICL→TGL |
| 2 | `patch_nodes.py` | AppleGVA walks capability nodes with the next link at +0x430 (plugin: +0x428) | nodes 0x438 bytes, link at both offsets |
| 3 | `patch_nodelayout.py` | Tahoe nodes have a new u64 at +0x20, so every list moved by 8 → H.264 node had no input format (`-12913`); Tahoe asks driver parameter 14 (capability list), the plugin knows 2–12 | AppleGVA gets a converted deep copy of the list (at accelerator create and for parameter 14); the plugin keeps its own layout |
| 4 | `patch_payloadhdr.py` | CodecHAL sideband payload header never committed for decode (`je` at 0x64a15) → the kernel reads relocation 0 as an indirect-batch handle, the lookup fails and IOAcceleratorFamily2 drops every decode batch ("commands on floor") → empty frames | always commit it |
| 5 | `patch_pitchdelta.py` | SURFACE_STATE pitch relocation delta already holds pitch-1 = -1 → pitch one too small | clear the field before the kernel adds pitch-1 |
| 6 | `patch_spsshift.py` | Tahoe's H.264 SPS structure has 8 extra bytes before `level_idc` → MFX_AVC_IMG_STATE with chroma 4:4:4, MBAFF, wrong frame_num/POC widths → garbage and a VCS0 hang | the three SPS readers use SPS+8 |
| 7 | `patch_hcpsurface.py` | HEVC: the HCP surface-state builder fills the command but never adds it to the batch; its relocations land on HCP_PIPE_BUF_ADDR_STATE | emit it (5 dwords) |
| 8 | `patch_hevclastslice.py` | HEVC: Tahoe AppleGVA never sets LastSliceOfPic in the slice parameters | set it on the last slice, clear it elsewhere |

`--trace` adds an `LC_LOAD_DYLIB` for `tools/diagnostics/xe-vatrace.c` (logs every IOAccelerator/IOKit call of the
plugin, subsystem `xe.vatrace`); off by default. With all steps, H.264 and HEVC (8-bit 4:2:0) decode bit-exact against
the software decoder in tests. HEVC sessions also need the accelerator property `IOGVAHEVCDecode` = "2" (the ICL kext
has it, the TGL one doesn't; without it HEVC fails with `-12906`). Safari can't use the plugin: the
`com.apple.WebKit.GPU` sandbox denies `file-map-executable` for `/Library/GPUBundles`, so Safari decodes on the CPU.

## 6. Known gaps in this layer

- `BGR10_XR` / `w30r` (extended-range 10-bit) is not available on any Intel/AMD Mac: Metal's format validation requires the Apple1 GPU family, CoreAnimation only maps `w30r`/`w40a` for Apple2+ devices, and the 16.0.32 driver/Gen12 hardware have no XR surface format. Forcing CoreAnimation's Apple2 bit makes 10-bit 4:2:2/4:2:0 video use Apple-only multi-plane formats (crash), so it stays unsupported.
- Hardware video decode needs `-ngreengva` and the plugin above (section 5); Safari still decodes on the CPU. Without
  `-ngreengva` (XE-NOGVA) VideoToolbox decodes and converts in software.
- Hardware encode: H.264 low-latency encode through VDENC works in tests, but P frames hang the video engine and the
  VPHAL patches it needs break 1080p pixel transfers in other apps. Research paused on 2026-10-24, nothing installed:
  [macos/vadriver/encode-experimental/README.md](../macos/vadriver/encode-experimental/README.md).
- Chrome's own GPU path (ANGLE/Metal) draws blank windows.
- Chrome (Graphite/Dawn) rendering with a populated GPU cache: fixed in libxedrv v16 (item 6 above).
