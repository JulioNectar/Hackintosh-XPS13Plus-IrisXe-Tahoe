#!/usr/bin/env python3
# patch_encsps.py <plugin binary>: H.264 ENCODE counterpart of patch_spsshift.py.
# Tahoe AppleGVA's H.264 SPS structure has 8 extra bytes before level_idc (level @0x24, chroma_format_idc @0x2c,
# log2_max_frame_num-4 @0x404, frame_mbs_only @0x830, mbaff @0x834, direct_8x8 @0x838, scaling-list flags @0x40/lists @0x64),
# while the 16.0.32 VDENC encoder reads the old layout from frameParams+0x410 (old: level @0x1c, chroma @0x24, ...).
# Symptom (guarded dry run, 1280x720): MFX_AVC_IMG_STATE DW4 = 0x1d9e (MBAFF=1, ChromaFormatIdc=3) instead of 0x159c.
# All SPS reads in the VDENC path are at old offsets >= 0x1c, so each load of the SPS pointer gets +8 (lea, flags kept):
#   0x2329d IMG_STATE builder, 0x23564 level, 0x24a7e/0x24ca5 scaling lists, 0x258c2 slice/mbaff, 0x275bf/0x27603/0x27632
#   (SPS passed to 0x906b/0x8b4f/0x9073: log2_max_frame_num @0x3fc). Caves at 0xd20..0xda0 (free header padding).
import sys, struct
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
LEA = {'r8': '4d8d4008', 'rbx': '488d5b08', 'rdx': '488d5208', 'rdi': '488d7f08'}
S = [(0x2329d, '4c8b8610040000', 'r8'), (0x23564, '4c8b8610040000', 'r8'), (0x24a7e, '488b9810040000', 'rbx'),
     (0x24ca5, '4c8b8610040000', 'r8'), (0x258c2, '488b9610040000', 'rdx'), (0x275bf, '488bbb10040000', 'rdi'),
     (0x27603, '488bbb10040000', 'rdi'), (0x27632, '488bbb10040000', 'rdi')]
CAVE = 0xd20
if b[S[0][0]] == 0xe9 and b[CAVE:CAVE+3] == bytes.fromhex('4c8b86'): print('already patched'); sys.exit(0)
assert b[CAVE:CAVE+16*len(S)] == bytes(16*len(S)), 'cave not free'
for i, (o, orig, reg) in enumerate(S):
    orig = bytes.fromhex(orig); assert b[o:o+7] == orig, hex(o)
    c = CAVE + 16*i
    cave = orig + bytes.fromhex(LEA[reg]) + b'\xe9' + struct.pack('<i', (o + 7) - (c + 16))
    assert len(cave) == 16; b[c:c+16] = cave
    b[o:o+7] = b'\xe9' + struct.pack('<i', c - (o + 5)) + b'\x90\x90'
open(p, 'wb').write(b); print('encsps: patched %d SPS loads' % len(S))
