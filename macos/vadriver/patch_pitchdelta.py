#!/usr/bin/env python3
# patch_pitchdelta.py <plugin binary>  (step 5 of build-vadriver.sh, in place)
# Fixes the relocation delta of the MFX/HCP SURFACE_STATE pitch entry. The plugin emits a type-1 ("pitch-1")
# relocation with shift 3 whose delta already carries pitch-1 = -1 as a 17-bit field (bits 19:3 all ones, e.g.
# 0x080ffffb for MFX_SURFACE_STATE DW3). The kext adds (pitch-1) << 3, so the result is one too small plus a carry
# into bit 20 (1280 -> pitch 1279). Clear bits 19:3 of the delta in that case (same rule as WhateverXe's cave).
# Hook: relocation-entry writer at 0x6453c -> cave 0xe00 (zero padding after the load commands).
import sys, struct
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
SITE, CAVE, BACK = 0x6453c, 0xe00, 0x64543
orig = bytes.fromhex('8b470c488b7dc8')                    # mov eax,[rdi+0xc] ; mov rdi,[rbp-0x38]
def rel(src_end, dst): return struct.pack('<i', dst - src_end)
if bytes(b[SITE:SITE+5]) == b'\xe9' + rel(SITE+5, CAVE): print('already patched'); sys.exit(0)
assert bytes(b[SITE:SITE+7]) == orig, b[SITE:SITE+7].hex()
assert bytes(b[BACK:BACK+3]) == bytes.fromhex('894708')   # mov [rdi+8],eax
c = bytearray()
c += bytes.fromhex('8b470c')                  # mov eax,[rdi+0xc]        (desc delta)
c += bytes.fromhex('837f3c01') + b'\x75\x00'  # cmp [rdi+0x3c],1 (type 1) ; jne done
j1 = len(c) - 1
c += bytes.fromhex('837f3403') + b'\x75\x00'  # cmp [rdi+0x34],3 (shift 3); jne done
j2 = len(c) - 1
c += bytes.fromhex('837f3800') + b'\x75\x00'  # cmp [rdi+0x38],0 (dir 0)  ; jne done
j3 = len(c) - 1
c += bytes.fromhex('51')                      # push rcx
c += bytes.fromhex('89c1')                    # mov ecx,eax
c += bytes.fromhex('81e1f8ff0f00')            # and ecx,0xffff8
c += bytes.fromhex('81f9f8ff0f00')            # cmp ecx,0xffff8
c += bytes.fromhex('59')                      # pop rcx
c += b'\x75\x00'; j4 = len(c) - 1             # jne done
c += bytes.fromhex('250700f0ff')              # and eax,0xfff00007
done = len(c)
for j in (j1, j2, j3, j4): c[j] = done - (j + 1)
c += bytes.fromhex('488b7dc8')                # mov rdi,[rbp-0x38]
c += b'\xe9' + rel(CAVE + len(c) + 5, BACK)   # jmp 0x64543
assert all(x == 0 for x in b[CAVE:CAVE+len(c)+16]), 'cave not free'
b[CAVE:CAVE+len(c)] = c
b[SITE:SITE+7] = b'\xe9' + rel(SITE+5, CAVE) + b'\x90\x90'
open(p, 'wb').write(b); print('patched 0x6453c -> cave 0x%x (%d bytes)' % (CAVE, len(c)))
