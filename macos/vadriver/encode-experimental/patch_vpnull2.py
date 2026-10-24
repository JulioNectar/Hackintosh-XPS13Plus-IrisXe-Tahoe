#!/usr/bin/env python3
# patch_vpnull2.py <plugin binary> (24/10): second NULL sub-renderer site. fn 0x133690 (VPHAL renderer, after the
# kernel-table fix lets VPHAL run) stores renderer+0x2cb0 into [renderer+0x920]+8 and [renderer+0x910]+8; both
# sub-renderers are NULL on this platform (see patch_vpnull.py), so any VPHAL frame with this path segfaulted at 0x1336f3
# (found with a 1920x1080 '420v' buffer: VideoToolbox's preparation queue, even with the software encoder).
# Same behaviour as before, with each store skipped when the pointer is NULL. Cave at 0xac0 in the header padding.
import sys, struct
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
S, END, C = 0x1336d2, 0x1336f7, 0xac0
ORIG = bytes.fromhex('488b55d8488bba2009000048894f084881c2b02c0000488b4dd8488bb91009000048895708')
def rel32(src, dst): return struct.pack('<i', dst - src)
if b[S] == 0xe9 and b[C:C+4] == bytes.fromhex('488b55d8'): print('already patched'); sys.exit(0)
assert b[S:END] == ORIG, b[S:END].hex()
cave = bytearray(bytes.fromhex('488b55d8' '488bba20090000' '4885ff' '7404' '48894f08'
                               '4881c2b02c0000' '488b4dd8' '488bb910090000' '4885ff' '7404' '48895708'))
cave += b'\xe9' + rel32(C + len(cave) + 5, END)
assert b[C:C+len(cave)+8] == bytes(len(cave)+8), 'cave not free'
b[C:C+len(cave)] = cave
b[S:END] = (b'\xe9' + rel32(S + 5, C)).ljust(END - S, b'\x90')
open(p, 'wb').write(b); print('patched VPHAL NULL sub-renderer stores in fn 0x133690')
