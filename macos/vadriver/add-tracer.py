#!/usr/bin/env python3
# add-tracer.py <plugin binary> [dylib path]  (optional, build-vadriver.sh --trace; DIAGNOSTICS ONLY)
# Appends an LC_LOAD_DYLIB for the xe-vatrace tracer (tools/diagnostics/xe-vatrace.c), which logs every
# IOAccelerator/IOKit call of the plugin to the unified log (subsystem "xe.vatrace"). The new load command goes into the
# zero padding after the existing ones and must end before the first code cave (0xb00, patch_nodelayout.py).
# Default path: the dylib shipped inside the installed bundle.
import sys, struct
p = sys.argv[1]
path = (sys.argv[2] if len(sys.argv) > 2 else
        '/Library/GPUBundles/AppleIntelTGLGraphicsVADriver.bundle/Contents/MacOS/xe-vatrace.dylib').encode()
d = bytearray(open(p, 'rb').read())
magic, _, _, _, ncmds, sizeofcmds = struct.unpack_from('<IiiIII', d, 0)
assert magic == 0xfeedfacf, 'not a 64-bit Mach-O'
o = 32
for _ in range(ncmds):
    cmd, cs = struct.unpack_from('<II', d, o)
    if cmd == 0xc and d[o + 0x18:o + 0x18 + len(path) + 1] == path + b'\0':
        print('add-tracer: already present'); sys.exit(0)
    o += cs
end = 32 + sizeofcmds
size = (0x18 + len(path) + 1 + 7) & ~7
assert end + size <= 0xb00, 'no room for the load command before the code caves'
assert all(x == 0 for x in d[end:end + size]), 'load-command padding is not empty'
# dylib_command: cmd, cmdsize, name offset, timestamp, current version 1.0.0, compatibility version 1.0.0
d[end:end + size] = (struct.pack('<IIIIII', 0xc, size, 0x18, 2, 0x10000, 0x10000) + path).ljust(size, b'\0')
struct.pack_into('<II', d, 16, ncmds + 1, sizeofcmds + size)
open(p, 'wb').write(d)
print('add-tracer: LC_LOAD_DYLIB', path.decode())
