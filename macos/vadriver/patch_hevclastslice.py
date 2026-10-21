#!/usr/bin/env python3
# patch_hevclastslice.py <plugin binary>  (step 8 of build-vadriver.sh, in place): HEVC decode - set LastSliceOfPic in the slice parameters.
# Tahoe AppleGVA fills the HEVC slice parameter array (DXVA-style, stride 0x244; LongSliceFlags at +0x17c: bit0
# LastSliceOfPic, bit1 dependent_slice_segment, bits 3:2 slice_type ...) but never sets LastSliceOfPic (measured:
# flags 0x1088 for a single I slice). The 16.0.32 plugin takes HCP_SLICE_STATE.LastSliceOfPic (and the
# next-slice position) from that bit (0x1413bf), so the hardware never sees the end of the picture.
# Fix: at the entry of the HEVC slice-programming function 0x13fee0(ctx, ...) - ctx+0x20 = slice count,
# ctx+0x30 = slice array - clear bit0 for every slice and set it for the last one, then run the original prologue.
# Same rule as WhateverXe's TGLVA xe_vt_hevc_slice_flags (runtime patch of the same function).
import sys, struct
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
FN, BACK, CAVE = 0x13fee0, 0x13fef6, 0xf40
PRO = bytes.fromhex('554889e54881ec80020000' '48897df0' '488975e8' '8955e4')   # original prologue (0x16 bytes)
rel = lambda end, dst: struct.pack('<i', dst - end)
if bytes(b[FN:FN+5]) == b'\xe9' + rel(FN + 5, CAVE): print('hevclastslice: already patched'); sys.exit(0)
assert bytes(b[FN:FN+0x16]) == PRO, b[FN:FN+0x16].hex()
assert bytes(b[0x140164:0x14016a]) == bytes.fromhex('8b887c010000'), 'not the HEVC slice function (0x17c read)'
c = bytearray(PRO)
c += bytes.fromhex('8b4f20')                  # mov  0x20(%rdi),%ecx      slice count
c += bytes.fromhex('85c9') + b'\x74\x00'; j1 = len(c) - 1          # test ; jz done
c += bytes.fromhex('488b4730')                # mov  0x30(%rdi),%rax      slice array
c += bytes.fromhex('4885c0') + b'\x74\x00'; j2 = len(c) - 1        # test ; jz done
loop = len(c)
c += bytes.fromhex('83a07c010000fe')          # andl $-2,0x17c(%rax)      LastSliceOfPic = 0
c += bytes.fromhex('ffc9') + b'\x74\x00'; j3 = len(c) - 1          # dec %ecx ; jz last
c += bytes.fromhex('480544020000')            # add  $0x244,%rax
c += b'\xeb' + bytes([(loop - (len(c) + 2)) & 0xff])               # jmp loop
last = len(c)
c += bytes.fromhex('83887c01000001')          # orl  $1,0x17c(%rax)       last slice: LastSliceOfPic = 1
done = len(c)
c += b'\xe9' + rel(CAVE + len(c) + 5, BACK)   # jmp 0x13fef6 (rest of the function)
c[j1] = done - (j1 + 1); c[j2] = done - (j2 + 1); c[j3] = last - (j3 + 1)
assert all(x == 0 for x in b[CAVE:CAVE + len(c) + 16]), 'cave not free'
b[CAVE:CAVE + len(c)] = c
b[FN:FN+0x16] = b'\xe9' + rel(FN + 5, CAVE) + b'\x90' * 0x11
open(p, 'wb').write(b); print('hevclastslice: 0x13fee0 -> cave %#x (%d bytes)' % (CAVE, len(c)))
