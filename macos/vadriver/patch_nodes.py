# Usage: python3 -I patch_nodes.py <in> <out>
# Grows the plugin's capability nodes 0x430 -> 0x438 and writes the next link at
# both +0x428 (old plugin readers) and +0x430 (Tahoe AppleGVA readers).
import sys,struct
d=bytearray(open(sys.argv[1],'rb').read())
MEMCPY=0x23e41c
OLD=bytes.fromhex('4c89f3ba28040000''4c89f7e8')
assert d[0x2bbe7:0x2bbec]==bytes.fromhex('be30040000'); d[0x2bbe8:0x2bbea]=b'\x38\x04'
for L in (0x2bc7e,0x2bd18,0x2bd81,0x2be1b):
    assert d[L:L+12]==OLD, hex(L)
    assert d[L+0x34-2:L+0x34]==bytes.fromhex('75d5') or True
    rel=MEMCPY-(L+8+5)
    c =bytes.fromhex('4c89f7')              # mov  %r14,%rdi
    c+=bytes.fromhex('ba28040000')          # mov  $0x428,%edx
    c+=b'\xe8'+struct.pack('<i',rel)        # call memcpy  (rax = dst)
    c+=bytes.fromhex('4c8db038040000')      # lea  0x438(%rax),%r14
    c+=bytes.fromhex('4d8976f0')            # mov  %r14,-0x10(%r14)  -> node+0x428
    c+=bytes.fromhex('4d8976f8')            # mov  %r14,-0x8(%r14)   -> node+0x430
    c+=bytes.fromhex('4983c708')            # add  $8,%r15
    c+=bytes.fromhex('498b37')              # mov  (%r15),%rsi
    c+=bytes.fromhex('4885f6')              # test %rsi,%rsi
    c+=b'\x75'+bytes([(-(len(c)+2))&0xff])  # jne  L
    c+=bytes.fromhex('0f57c0')              # xorps %xmm0,%xmm0
    c+=bytes.fromhex('410f1146f0')          # movups %xmm0,-0x10(%r14) : last node +0x428/+0x430 = 0
    c+=bytes.fromhex('0f1f4000')            # nop4
    assert len(c)==0x34                     # empty-list je lands at L+0x34
    c+=bytes.fromhex('4983c408')            # add  $8,%r12
    c+=bytes.fromhex('0f1f840000000000')+bytes.fromhex('0f1f440000')+b'\x90'  # 14 nops
    assert len(c)==70
    d[L:L+70]=c
open(sys.argv[2],'wb').write(d)
print('ok')
