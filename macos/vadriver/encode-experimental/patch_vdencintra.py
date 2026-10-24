#!/usr/bin/env python3
# patch_vdencintra.py <plugin binary> (24/10, interim): the VDENC inter path of the 16.0.32 VA driver (Gen11 programming)
# hangs VCS0 on Gen12 in every P frame (stuck in MI_FLUSH_DW after the first slice; intra-only VDENC does not hang).
# fn 0x25133 copies one of two prebuilt VDENC_IMG_STATE templates into the batch: +0xec (I) or +0x178 (P), chosen by
# "cmove r13, rcx" at 0x2556e. Always take the I template: VDENC does intra mode decision only, the PAK still writes P
# slices (decodable; tested with a batch patch: 9/9 frames, Y-PSNR 36-40 dB, no hang). Bigger frames than real inter
# prediction, but working hardware encode until the inter path is fixed.
import sys
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
O, OLD, NEW = 0x2556e, bytes.fromhex('4c0f44e9'), bytes.fromhex('4989cd90')   # cmove r13,rcx -> mov r13,rcx; nop
if b[O:O+4] == NEW: print('already patched'); sys.exit(0)
assert b[O:O+4] == OLD, b[O:O+4].hex()
b[O:O+4] = NEW
open(p, 'wb').write(b); print('patched: VDENC always uses the intra VDENC_IMG_STATE template (interim)')
