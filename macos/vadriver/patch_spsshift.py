#!/usr/bin/env python3
# patch_spsshift.py <plugin binary>  (step 6 of build-vadriver.sh, in place)
# Tahoe AppleGVA's H.264 SPS structure (frame params +0x420 -> SPS) has 8 extra bytes before level_idc (profile @0x4,
# level @0x24, chroma_format_idc @0x2c, log2_max_frame_num-4 @0x404, poc_type @0x408, log2_max_poc_lsb-4 @0x40c,
# max_num_ref_frames @0x820, frame_mbs_only @0x830, mbaff @0x834, direct_8x8 @0x838), while the 16.0.32 plugin reads
# the old layout (0x24, 0x3fc..0x404, 0x818, 0x828..0x830). Result: MFX_AVC_IMG_STATE with chroma_format_idc = 3 (read
# from the level), MBAFF = 1 and wrong frame_num/POC bit widths -> garbage pictures and a VCS0 hang.
# Fix: the three AVC DDI consumers of the SPS pointer use SPS+8 (all their reads are at offsets >= 0x24):
#   0x228e20 (picture params), 0x229730 (scaling lists), 0x229390 (single mbaff read 0x82c -> 0x834).
# The PPS structure did not change.
import sys
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
P = [
 (0x228e3f, '488b4dd0 488b892804000048 894dc0 488b4dd0 488b8920040000 48894db8',
            '488b4dd0 488b912804000048 8955c0 488b8920040000 4883c108 48894db8'),
 (0x229740, '488b45f0 488b8028040000 488945e0 488b45f0 488b8020040000 488945d8',
            '488b4df0 488b8128040000 488945e0 488b8120040000 4883c008 488945d8'),
 (0x229655, '83b82c08000000', '83b83408000000'),
]
done = 0
for o, old, new in P:
    old = bytes.fromhex(old.replace(' ', '')); new = bytes.fromhex(new.replace(' ', ''))
    assert len(old) == len(new), hex(o)
    cur = bytes(b[o:o+len(old)])
    if cur == new: continue
    assert cur == old, (hex(o), cur.hex())
    b[o:o+len(new)] = new; done += 1
open(p, 'wb').write(b); print('spsshift: patched %d/3 sites' % done)
