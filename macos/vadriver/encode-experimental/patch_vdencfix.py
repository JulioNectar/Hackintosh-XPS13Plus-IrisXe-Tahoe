#!/usr/bin/env python3
# patch_vdencfix.py <plugin binary>: two Gen11-isms in the H.264 VDENC encode path of the 16.0.32 VA driver (24/10).
# 1. Status registers. The encoder object keeps the MFC status registers as dword offsets (reg / 4), loaded at init
#    (0x26d72) from a 16-byte constant at 0x24ae50 (+0xa10..+0xa1c) and a movabs at 0x26d82 (+0xa20/+0xa24). They are
#    the legacy VCS addresses 0x128b4/0x128b8/0x12400/0x128a0/0x128a4/0x128d0; on Gen12 the VCS0 MMIO base is
#    0x1c0000, so MI_STORE_REGISTER_MEM read nothing useful, the bitstream byte count came back 0 and AppleGVA reported
#    "Frame dropped". Moved to 0x1c08b4/0x1c08b8/0x1c0400/0x1c08a0/0x1c08a4/0x1c08d0.
# 2. VDENC surface format. fn 0x24753 builds the VDENC_SRC/REF/DS surface-state word that the kernel ORs with the
#    pitch (token +0xc38/+0xc3c) with the MFX encoding 0x48000003 (format 4 << 28 | interleave << 27). In
#    VDENC_SURFACE_STATE the format is bits 27..31, so that read as 9 = RGBA_10_10_10_2 and the picture came out
#    stretched 4x horizontally. PLANAR_420_8 there is 4 << 27 = 0x20000000; the mask that keeps bits 30/27..20 (0x4ff00007)
#    is widened to 0xfff00007 so bit 29 survives.
import sys, struct
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
OLD = [0x4a2d, 0x4a2e, 0x4900, 0x4a28]; NEW = [(r * 4 + 0x1ae000) // 4 for r in OLD]
S = [(0x24ae50, struct.pack('<4I', *OLD), struct.pack('<4I', *NEW)),
     (0x26d82, bytes.fromhex('48b8294a0000344a0000'), b'\x48\xb8' + struct.pack('<II', (0x4a29 * 4 + 0x1ae000) // 4, (0x4a34 * 4 + 0x1ae000) // 4)),
     (0x24773, bytes.fromhex('b803000048'), bytes.fromhex('b803000020')),
     (0x247b0, bytes.fromhex('41be0700f04f'), bytes.fromhex('41be0700f0ff')),
     (0x2480d, bytes.fromhex('41b803000048'), bytes.fromhex('41b803000020'))]
if all(b[o:o+len(n)] == n for o, _, n in S): print('already patched'); sys.exit(0)
for o, old, new in S:
    assert b[o:o+len(old)] == old, hex(o); b[o:o+len(new)] = new
open(p, 'wb').write(b); print('patched VDENC status registers (Gen12 VCS0 base) + VDENC surface format PLANAR_420_8')
