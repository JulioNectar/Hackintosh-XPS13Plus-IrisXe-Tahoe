#!/usr/bin/env python3
# patch_hcpsurface.py <plugin binary>  (step 7 of build-vadriver.sh, in place): HEVC decode - actually emit HCP_SURFACE_STATE.
# The stock 16.0.32 HCP surface-state builder (fn 0x192d20) fills the 5-dword command at -0x38(%rbp) and registers its
# relocations (pitch / Cb offset at the current batch position), but never calls the add-command routine (0x41300).
# The command is missing from the batch and its relocations land on HCP_PIPE_BUF_ADDR_STATE DW1/DW2 (written next at
# the same position). Fix: at the function's exit (0x193543: mov -0x24(%rbp),%eax ; mov %eax,-0x4(%rbp)) jump to a
# cave that calls 0x41300(cmdbuf = -0x18(%rbp), &cmd = -0x38(%rbp), 0x14) and returns its status.
# Same fix as WhateverXe's TGLVA xe_vt_hevc_surface_shim (runtime patch at 0x19351b).
import sys, struct
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
SITE, BACK, CAVE, ADDCMD = 0x193543, 0x193549, 0xf00, 0x41300
rel = lambda end, dst: struct.pack('<i', dst - end)
if bytes(b[SITE:SITE+5]) == b'\xe9' + rel(SITE + 5, CAVE): print('hcpsurface: already patched'); sys.exit(0)
assert bytes(b[SITE:SITE+6]) == bytes.fromhex('8b45dc8945fc'), b[SITE:SITE+6].hex()
assert bytes(b[BACK:BACK+3]) == bytes.fromhex('8b45fc')                      # mov -0x4(%rbp),%eax (common exit)
assert bytes(b[0x192d60:0x192d69]) == bytes.fromhex('488d7dc8e8e7bd0000'), 'not the HCP surface-state builder'
c  = bytes.fromhex('488b7de8')                    # mov  -0x18(%rbp),%rdi   (command buffer)
c += bytes.fromhex('488d75c8')                    # lea  -0x38(%rbp),%rsi   (HCP_SURFACE_STATE, 5 dwords)
c += bytes.fromhex('ba14000000')                  # mov  $0x14,%edx
c += b'\xe8' + rel(CAVE + len(c) + 5, ADDCMD)     # call 0x41300            (add command to the batch)
c += bytes.fromhex('8945fc')                      # mov  %eax,-0x4(%rbp)    (function status)
c += b'\xe9' + rel(CAVE + len(c) + 5, BACK)       # jmp  0x193549           (return)
assert all(x == 0 for x in b[CAVE:CAVE + len(c) + 16]), 'cave not free'
b[CAVE:CAVE + len(c)] = c
b[SITE:SITE+6] = b'\xe9' + rel(SITE + 5, CAVE) + b'\x90'
open(p, 'wb').write(b); print('hcpsurface: 0x193543 -> cave %#x (%d bytes)' % (CAVE, len(c)))
