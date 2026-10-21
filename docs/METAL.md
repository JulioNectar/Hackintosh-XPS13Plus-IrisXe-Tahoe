# User-space stack (Metal driver, libxedrv, shader compiler)

Everything here is built by `macos/build-all.sh` from Apple's 16.0.32 bundles plus the sources in `macos/src`, and
installed by `macos/install-macos.sh`.

**Version history.** The Metal driver went through builds v1–v12 and MTL13–MTL17 (18–19 Oct); **MTL18** (20 Oct, in
test) is MTL17 with two bytes restored — see the L3 row below. libxedrv went through v7–v15 and the LLVM bridge through
v1–v2. Every build, with its evidence and result, is in [HISTORY.md](HISTORY.md) and [CATALOG.md](CATALOG.md) §4;
abandoned approaches are in [EXPERIMENTS.md](EXPERIMENTS.md).

```
/Library/Extensions/AppleIntelTGLGraphics*.kext, *.bundle      Apple 16.0.32 (accelerator Info.plist edited)
/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle       Metal driver, patched ("MTL17") + libxedrv.dylib
/Library/GPUBundles/AppleIntelGraphicsShared.bundle             Apple 16.0.32 (profiler and runtime files)
/System/Library/Caches/xe.bundle                                 shader compiler stack
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

`macos/patch-mtl17.py` + `mtl17-patches.json` turn the stock binary into MTL17 byte for byte (verified: zero
differences outside the code signature).

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
| Info.plist | `Development.PoshDisable = 1` | no POSH pipe on RPL |

**MTL18.** v11 originally replaced the same byte at `0x07ff77` and `0x080253` too, but those are not L3 values: in
`IntelState::IntelState()` the byte is the opcode of `movb %al,0x110(%rdi)` / `movb %cl,0x8e0(%rbx)`, which the patch
turned into `rolb`/`rorb`. The repository's `mtl17-patches.json` now **excludes those two bytes**, so `patch-mtl17.py`
builds MTL18 (MTL17 minus the corruption; being tested). Details: [CATALOG.md](CATALOG.md) §6.1.

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

Environment switches: `XE_COMPRESSAO=1` keeps compression, `XE_NOCOPY_ZERO=1` disables fix 4.

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

## 5. Known gaps in this layer

- `BGR10_XR` / `w30r` (extended-range 10-bit) isn't supported by the 16.0.32 driver.
- Hardware video decode/encode is off (see XE-NOGVA). Decoding with an explicit output pixel format fails in
  VideoToolbox (`-6680`), which is why Safari can't play YouTube. Chrome with `--disable-gpu
  --disable-gpu-compositing` plays it with software decode.
- Chrome's own GPU path (ANGLE/Metal) draws blank windows.
