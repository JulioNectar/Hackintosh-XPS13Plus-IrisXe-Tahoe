#!/usr/bin/env python3
# patch_payloadhdr.py <plugin binary>  (step 4 of build-vadriver.sh, in place)
# Always commit the 4-byte CodecHAL payload header dword of the 0x8200 sideband token.
# The sideband token is {u16 type, u16 len_dw, u32 cmd_dword_offset}, then a payload header dword (bit0 = command
# buffer is in an indirect resource, bits 16:1 = its handle), then 5-dword relocation entries. The stock 16.0.32
# token writer (token start at 0x64945) reserves the header dword but only commits it (call 0x640b0) when an argument
# is non-zero (`je` at 0x64a15); for decode it is zero, so the first relocation entry overwrites the header.
# The kext (IGAccelVideoContext::patch_codechal_command_buffer) then reads entry0.dw0 as "indirect command buffer,
# handle = dw0 >> 1", the resource lookup fails, the stream is marked invalid and IOAcceleratorFamily2 drops it
# ("dropping ... commands on floor"): every decoded frame came back empty. Fix: NOP the `je`.
# (WhateverXe's Tahoe VA bundle makes the same change.)
import sys
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
O = 0x64a15; old = bytes.fromhex('0f8412000000'); new = b'\x90' * 6
cur = bytes(b[O:O+6])
if cur == new: print('already patched'); sys.exit(0)
assert cur == old, cur.hex()
assert b[O-4:O] == bytes.fromhex('837de400'), b[O-4:O].hex()   # cmpl $0, -0x1c(%rbp)
b[O:O+6] = new; open(p, 'wb').write(b); print('patched 0x64a15 je -> nop*6')
