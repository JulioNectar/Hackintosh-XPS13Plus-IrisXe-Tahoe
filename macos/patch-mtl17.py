#!/usr/bin/env python3
"""Build the patched Metal driver bundle ("MTL17") from Apple's stock Tiger Lake bundle.

Usage:
  python3 patch-mtl17.py <stock AppleIntelTGLGraphicsMTLDriver.bundle> <libxedrv.dylib> <output dir>

The stock bundle is GPUDriversIntel 16.0.32 (the `sle/` copy shipped with the NootedGreen project). It is not
redistributed here. The script:
  1. copies the bundle,
  2. applies the byte patches in mtl17-patches.json (each one checks the original bytes first),
  3. sets Info.plist Development.PoshDisable = 1,
  4. copies libxedrv.dylib into Contents/MacOS (the patched header loads it with LC_LOAD_DYLIB),
  5. re-signs everything ad hoc.

What the byte patches do (offsets are file offsets in the x86_64 binary):
  - (the __LINKEDIT size and LC_CODE_SIGNATURE size are left to codesign, which rewrites them)
  - Mach-O header + load commands: one extra LC_LOAD_DYLIB for
    /Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/libxedrv.dylib
  - 0x0a2e7b: shader compiler plugin path -> /System/Library/PrivateFrameworks/../Caches/xe.bundle/... (Tahoe's
    MTLCompilerService only accepts plugins under a few prefixes and its sandbox only reads /System)
  - 0x018c28: `mov ecx,[rax+0x18]` -> `xor ecx,ecx` (v15)
  - 0x01c7c0 / 0x01c7cf: two `setne` -> NOP (v13)
  - 0x049cfe: `setne al` -> NOP (v14)
  - 0x065f12: L3 allocation dword 0x20000088 -> 0x200000d0 (v11; the 0x07ff77 / 0x080253 copies were restored in MTL18)
  - 0x067c27: `je +0x5c` -> NOP (v16, fast clear off)
  - 0x067c5c: `jne` -> `jmp` (v17, clear always disables the VS stage)
  - 0x07ed87: `jne` -> NOP in -[MTLIGAccelCommandBuffer dealloc] (v18, #12): the per-buffer POSH block
    (96 KB) was never freed for the "only active" command buffer. The queue's shared POSH pointer is never set
    (only zeroed at queue init), so every command buffer owns its block and freeing it is safe.
"""
import json, os, shutil, subprocess, sys

def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    stock, xedrv, outdir = sys.argv[1:]
    here = os.path.dirname(os.path.abspath(__file__))
    patches = json.load(open(os.path.join(here, 'mtl17-patches.json')))
    out = os.path.join(outdir, 'AppleIntelTGLGraphicsMTLDriver.bundle')
    if os.path.exists(out):
        subprocess.run(['/bin/rm', '-rf', out], check=True)   # shutil.rmtree trips over vanishing "._" files
    shutil.copytree(stock, out, symlinks=True)
    binp = os.path.join(out, 'Contents/MacOS/AppleIntelTGLGraphicsMTLDriver')
    b = bytearray(open(binp, 'rb').read())
    for p in patches:
        off, old, new = p['off'], bytes.fromhex(p['old']), bytes.fromhex(p['new'])
        if b[off:off + len(old)] != old:
            sys.exit('bytes at 0x%x do not match the stock 16.0.32 driver (wrong input bundle?)' % off)
        b[off:off + len(new)] = new
    open(binp, 'wb').write(b)
    subprocess.run(['/usr/bin/plutil', '-replace', 'Development.PoshDisable', '-integer', '1',
                    os.path.join(out, 'Contents/Info.plist')], check=True)
    shutil.copy2(xedrv, os.path.join(out, 'Contents/MacOS/libxedrv.dylib'))
    subprocess.run(['/usr/bin/codesign', '-f', '-s', '-', os.path.join(out, 'Contents/MacOS/libxedrv.dylib')], check=True)
    subprocess.run(['/usr/bin/codesign', '-f', '-s', '-', binp], check=True)
    subprocess.run(['/usr/bin/codesign', '-f', '-s', '-', out], check=True)
    print('built', out)

if __name__ == '__main__':
    main()
