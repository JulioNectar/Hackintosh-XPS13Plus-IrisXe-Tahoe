#!/usr/bin/env python3
# patch_nodes.py <in> [<out>]  (step 2 of build-vadriver.sh; <out> defaults to <in>)
# The plugin builds its capability list (decoder/encoder "nodes") from static templates. Tahoe AppleGVA walks the node
# chains with the next link at +0x430 (ICL/Tahoe layout), the 16.0.32 plugin uses +0x428. This grows every node from
# 0x430 to 0x438 bytes (calloc size in AVD_CreateAVDAccelerator) and rewrites the 4 copy loops (0x2bc7e, 0x2bd18,
# 0x2bd81, 0x2be1b) so they store the next link at both +0x428 (the plugin's own readers) and +0x430 (AppleGVA).
# The node CONTENT layout is fixed separately by patch_nodelayout.py (step 3).
import sys, struct
src = sys.argv[1]; dst = sys.argv[2] if len(sys.argv) > 2 else src
d = bytearray(open(src, 'rb').read())
MEMCPY = 0x23e41c                                          # __stubs: memcpy
LOOP = bytes.fromhex('4c89f3ba28040000' '4c89f7e8')        # mov %r14,%rbx ; mov $0x428,%edx ; mov %r14,%rdi ; call ...
# calloc(1, 0x430) -> calloc(1, 0x438)
assert d[0x2bbe7:0x2bbec] == bytes.fromhex('be30040000'), 'node size: unexpected bytes (not stock 16.0.32?)'
d[0x2bbe8:0x2bbea] = b'\x38\x04'
for L in (0x2bc7e, 0x2bd18, 0x2bd81, 0x2be1b):
    assert d[L:L + 12] == LOOP, f'copy loop at {L:#x}: unexpected bytes'
    c  = bytes.fromhex('4c89f7')                           # mov  %r14,%rdi
    c += bytes.fromhex('ba28040000')                       # mov  $0x428,%edx
    c += b'\xe8' + struct.pack('<i', MEMCPY - (L + 8 + 5))  # call memcpy (rax = dst)
    c += bytes.fromhex('4c8db038040000')                   # lea  0x438(%rax),%r14   (next node)
    c += bytes.fromhex('4d8976f0')                         # mov  %r14,-0x10(%r14)   -> node+0x428
    c += bytes.fromhex('4d8976f8')                         # mov  %r14,-0x8(%r14)    -> node+0x430
    c += bytes.fromhex('4983c708')                         # add  $8,%r15
    c += bytes.fromhex('498b37')                           # mov  (%r15),%rsi
    c += bytes.fromhex('4885f6')                           # test %rsi,%rsi
    c += b'\x75' + bytes([(-(len(c) + 2)) & 0xff])         # jne  L
    c += bytes.fromhex('0f57c0')                           # xorps %xmm0,%xmm0
    c += bytes.fromhex('410f1146f0')                       # movups %xmm0,-0x10(%r14): last node +0x428/+0x430 = 0
    c += bytes.fromhex('0f1f4000')                         # nop4
    assert len(c) == 0x34                                  # the original empty-list je lands at L+0x34
    c += bytes.fromhex('4983c408')                         # add  $8,%r12
    c += bytes.fromhex('0f1f840000000000') + bytes.fromhex('0f1f440000') + b'\x90'   # 14 bytes of nops
    assert len(c) == 70
    d[L:L + 70] = c
open(dst, 'wb').write(d)
print('patch_nodes: node size 0x438, next link at +0x428 and +0x430')
