# User-space stack (Metal driver, libxedrv, shader compiler)

Everything here is built by `macos/build-all.sh` from Apple's 16.0.32 bundles plus the sources in `macos/src`, and
installed by `macos/install-macos.sh`.

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
| `0x065f12`, `0x07ff77`, `0x080253` | L3 allocation `0x20000088` → `0x200000d0` | RPL L3 bank layout |
| `0x01c7c0`, `0x01c7cf`, `0x049cfe`, `0x018c28` | `setne`/`mov` → NOP / `xor` | disable code paths for features RPL lacks |
| `0x067c27` | `je` → NOP | no render-target fast clear (needs the AUX table) |
| `0x067c5c` | `jne` → `jmp` | a clear always disables the VS stage (it inherited the VS state → hangs) |
| Info.plist | `Development.PoshDisable = 1` | no POSH pipe on RPL |

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
