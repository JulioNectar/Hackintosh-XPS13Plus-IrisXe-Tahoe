#!/usr/bin/env python3
# patch_vpnull.py <plugin binary>: VPHAL renderer init (fn 0x132510) calls Initialize (vtable +0x10) on two sub-renderers at
# renderer+0x910 / +0x918 that are NULL on this platform -> segfault at 0x13282a once the kernel-table fix (patch_vphal.py) lets
# init get that far. Skip each call when the pointer is NULL (same behaviour as the WhateverXe Tahoe VA bundle, caves 0xD80/0xD98).
# Also guard CFRelease(NULL) at 0x63d20 (WEX cave 0xDC0). Caves in the free header padding at 0xa60..0xaa0.
import sys, struct
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
def rel32(src, dst): return struct.pack('<i', dst - src)
def jmp(at, dst): return b'\xe9' + rel32(at + 5, dst)
def je(at, dst): return b'\x0f\x84' + rel32(at + 6, dst)
S = [(0x13282a, bytes.fromhex('488b394889bd48ffffff')), (0x13286c, bytes.fromhex('488b394889bd40ffffff')), (0x63d20, bytes.fromhex('e85ba31d00'))]
CA, CB, CC = 0xa60, 0xa80, 0xaa0
if b[0x13282a] == 0xe9 and b[CA:CA+3] == b'\x48\x85\xc9': print('already patched'); sys.exit(0)
for o, orig in S: assert b[o:o+len(orig)] == orig, hex(o)
assert b[CA:CA+0x50] == bytes(0x50)
def cave(at, skip, back, slot):  # test rcx,rcx; je skip; mov rdi,[rcx]; mov [rbp-slot],rdi; jmp back
    c = bytearray(b'\x48\x85\xc9'); c += je(at + len(c), skip); c += b'\x48\x8b\x39' + b'\x48\x89\xbd' + struct.pack('<i', -slot); c += jmp(at + len(c), back); return c
b[CA:CA+0x1c] = cave(CA, 0x132853, 0x132834, 0xb8).ljust(0x1c, b'\xcc')
b[CB:CB+0x1c] = cave(CB, 0x132895, 0x132876, 0xc0).ljust(0x1c, b'\xcc')
c = bytearray(b'\x48\x85\xff\x74\x05'); c += jmp(CC + len(c), 0x23e080); c += b'\xc3'; b[CC:CC+len(c)] = c   # test rdi,rdi; je +5; jmp CFRelease stub; ret
b[0x13282a:0x132834] = jmp(0x13282a, CA) + b'\x90' * 5
b[0x13286c:0x132876] = jmp(0x13286c, CB) + b'\x90' * 5
b[0x63d20:0x63d25] = b'\xe8' + rel32(0x63d25, CC)
open(p, 'wb').write(b); print('patched VPHAL NULL sub-renderer guards + CFRelease guard')
