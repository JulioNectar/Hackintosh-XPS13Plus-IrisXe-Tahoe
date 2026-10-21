#!/usr/bin/env python3
# patch_nodelayout.py <plugin binary>  (step 3 of build-vadriver.sh, in place)
#
# Tahoe AppleGVA reads capability nodes in the ICL/Tahoe layout. Compared with the 16.0.32 TGL plugin, a new u64 sits
# at +0x20 (on ICL a copy of +0x18), so everything from the old +0x20 moves by 8: input list count+pairs 0x20/0x24 ->
# 0x28/0x2c, output list 0x224/0x228 -> 0x22c/0x230, next link 0x428 -> 0x430. With the old layout AppleGVA saw e.g.
# the H.264 decode node (type 1, 0x200) with input list {count=1, fourcc=0} instead of {count=1, 'rawb'}; its engine
# check fails ("GVA error: ctx_info.error = c" -> VTDecompressionSession error -12913).
#
# The plugin itself keeps reading the old layout, so its own lists stay untouched. conv() (cave at 0xb00) builds ONE
# deep copy in the Tahoe layout (header, engine table, every node chain), cached in a free __DATA qword, and only
# AppleGVA gets it: info+8 of AVD_CreateAVDAccelerator (hook 0x2bb2a -> cave 0xd00) and GetDriverParameter id 14
# (the `ja` at 0x1623c -> cave 0x1100; the stock plugin only knows ids 2..12, Tahoe asks 14 for the capability
# list, and the shipping ICL plugin answers it with accel+0x68). All lists come from the same static templates, so
# one cached copy serves every accelerator and never dangles when the plugin frees its own lists.
# The caves live in the zero padding after the load commands (r-x __TEXT). Needs patch-vadriver.py + patch_nodes.py.
import sys, struct

CONV, CREATE, P14 = 0xb00, 0xd00, 0x1100      # caves in the zero padding after the load commands (r-x __TEXT)
CALLOC, MEMCPY = 0x23e368, 0x23e41c           # __stubs (otool -Iv)
LG = 0x7f3ff0                                 # free qword: __DATA ends 0x7f4000, __common ends 0x7f324d
SKIP = 0x1641d                                # GetDriverParameter: next entry
JA = 0x1623c                                  # GetDriverParameter: ja (id-2 > 10)
CRT = 0x2bb2a                                 # AVD_CreateAVDAccelerator: info+8 = table[0]->+0x30 (12 bytes)

def asm(org, items):
    """items: bytes | ('lab', name) | ('rel', opcode_bytes, target) (rel32 to label or int)."""
    labs = {}
    for _ in range(2):
        pc, out = org, b''
        for it in items:
            if isinstance(it, bytes): out += it; pc += len(it); continue
            if it[0] == 'lab': labs[it[1]] = pc; continue
            op, tgt = it[1], it[2]
            t = labs.get(tgt, 0) if isinstance(tgt, str) else tgt
            ins = op + struct.pack('<i', t - (pc + len(op) + 4)); out += ins; pc += len(ins)
    return out

H = bytes.fromhex
conv = asm(CONV, [
    ('rel', H('488b05'), LG),          # mov  LG(%rip),%rax
    H('4885c0'),                       # test %rax,%rax
    ('rel', H('0f85'), 'ret'),         # jnz  ret            (already converted)
    H('55'), H('4889e5'),              # push %rbp; mov %rsp,%rbp
    H('53'), H('4154'), H('4155'), H('4156'), H('4157'),  # push rbx r12 r13 r14 r15
    H('4883ec08'),                     # sub  $8,%rsp        (16-byte aligned for calls)
    H('4889fb'),                       # mov  %rdi,%rbx      (old list)
    H('4889f8'),                       # mov  %rdi,%rax
    H('4885db'), ('rel', H('0f84'), 'out'),   # NULL list -> return it
    # header: calloc(1,0x20), copy 0x20 bytes
    H('bf01000000'), H('be20000000'), ('rel', H('e8'), CALLOC),
    H('4885c0'), ('rel', H('0f84'), 'fail'),
    H('4989c4'),                       # mov  %rax,%r12      (new header)
    H('0f1003'), H('410f110424'),      # movups (%rbx),%xmm0 ; movups %xmm0,(%r12)
    H('0f104310'), H('410f11442410'),  # movups 0x10(%rbx),%xmm0 ; movups %xmm0,0x10(%r12)
    # engine table: n=[rbx] ptrs at [rbx+8] -> calloc(n, 0x98): n ptrs, then n 0x90-byte engines
    H('8b3b'),                         # mov  (%rbx),%edi
    H('85ff'), ('rel', H('0f84'), 'nodes'),
    H('be98000000'), ('rel', H('e8'), CALLOC),
    H('4885c0'), ('rel', H('0f84'), 'fail'),
    H('4989c5'),                       # mov  %rax,%r13      (new ptr array)
    H('49894424' '08'),                # mov  %rax,0x8(%r12)
    H('4531f6'),                       # xor  %r14d,%r14d
    ('lab', 'eloop'),
    H('443b33'), ('rel', H('0f83'), 'nodes'),   # cmp (%rbx),%r14d ; jae nodes
    H('488b4308'),                     # mov  0x8(%rbx),%rax
    H('4a8b34f0'),                     # mov  (%rax,%r14,8),%rsi
    H('4885f6'), ('rel', H('0f84'), 'enext'),   # NULL engine: leave NULL
    H('8b03'),                         # mov  (%rbx),%eax    (n)
    H('498d7cc500'),                   # lea  0x0(%r13,%rax,8),%rdi   (engines area)
    H('4969c690000000'),               # imul $0x90,%r14,%rax
    H('4801c7'),                       # add  %rax,%rdi
    H('4b897cf500'),                   # mov  %rdi,0x0(%r13,%r14,8)
    H('ba90000000'), ('rel', H('e8'), MEMCPY),
    ('lab', 'enext'),
    H('41ffc6'), ('rel', H('e9'), 'eloop'),     # inc %r14d ; jmp eloop
    # node chains: arr = calloc(count+1, 8)
    ('lab', 'nodes'),
    H('8b7b10'), H('ffc7'),            # mov 0x10(%rbx),%edi ; inc %edi
    H('be08000000'), ('rel', H('e8'), CALLOC),
    H('4885c0'), ('rel', H('0f84'), 'fail'),
    H('4989c5'),                       # mov  %rax,%r13
    H('49894424' '18'),                # mov  %rax,0x18(%r12)
    H('4531f6'),                       # xor  %r14d,%r14d
    ('lab', 'outer'),
    H('443b7310'), ('rel', H('0f83'), 'done'),  # cmp 0x10(%rbx),%r14d ; jae done
    H('488b4318'),                     # mov  0x18(%rbx),%rax
    H('4e8b3cf0'),                     # mov  (%rax,%r14,8),%r15   (old chain head)
    H('4b8d44f500'),                   # lea  0x0(%r13,%r14,8),%rax
    H('48890424'),                     # mov  %rax,(%rsp)          (where to store next new node)
    ('lab', 'inner'),
    H('4d85ff'), ('rel', H('0f84'), 'onext'),
    H('bf01000000'), H('be38040000'), ('rel', H('e8'), CALLOC),
    H('4885c0'), ('rel', H('0f84'), 'fail'),
    H('488b0c24'), H('488901'),        # mov (%rsp),%rcx ; mov %rax,(%rcx)
    H('488d8830040000'), H('48890c24'),  # lea 0x430(%rax),%rcx ; mov %rcx,(%rsp)
    H('410f1007'), H('0f1100'),        # movups (%r15),%xmm0 ; movups %xmm0,(%rax)        +0x00..0x10
    H('410f104710'), H('0f114010'),    # movups 0x10(%r15),%xmm0 ; movups %xmm0,0x10(%rax) +0x10..0x20
    H('498b4f18'), H('48894820'),      # mov 0x18(%r15),%rcx ; mov %rcx,0x20(%rax)    new +0x20 = old +0x18
    H('488d7828'), H('498d7720'),      # lea 0x28(%rax),%rdi ; lea 0x20(%r15),%rsi
    H('ba08040000'), ('rel', H('e8'), MEMCPY),  # old 0x20..0x428 -> new 0x28..0x430
    H('4d8bbf30040000'),               # mov 0x430(%r15),%r15  (old chain link, written by patch_nodes)
    ('rel', H('e9'), 'inner'),
    ('lab', 'onext'),
    H('41ffc6'), ('rel', H('e9'), 'outer'),
    ('lab', 'done'),
    ('rel', H('4c8925'), LG),          # mov %r12,LG(%rip)
    H('4c89e0'), ('rel', H('e9'), 'out'),
    ('lab', 'fail'),
    H('4889d8'),                       # out of memory: hand out the original list
    ('lab', 'out'),
    H('4883c408'), H('415f'), H('415e'), H('415d'), H('415c'), H('5b'), H('5d'),
    ('lab', 'ret'),
    H('c3'),
])
create = asm(CREATE, [                 # called from AVD_CreateAVDAccelerator @0x2bb2a, rdi = info
    H('57'),                           # push %rdi
    H('498b0424'), H('488b7830'),      # mov (%r12),%rax ; mov 0x30(%rax),%rdi
    ('rel', H('e8'), CONV),
    H('5f'), H('48894708'),            # pop %rdi ; mov %rax,0x8(%rdi)
    H('c3'),
])
p14 = asm(P14, [                       # GetDriverParameter: ja lands here, esi = id-2
    H('83fe0c'), ('rel', H('0f85'), SKIP),     # id != 14 -> skip (as patch-vadriver #4)
    H('488bbda0feffff'),               # mov -0x160(%rbp),%rdi  (accel)
    H('488b7f68'),                     # mov 0x68(%rdi),%rdi    (its capability list)
    ('rel', H('e8'), CONV),
    H('498b4f08'),                     # mov 0x8(%r15),%rcx     (entries)
    H('4a890431'),                     # mov %rax,(%rcx,%r14)   (entry.ptr = converted list)
    ('rel', H('e9'), SKIP),
])
assert len(conv) <= CREATE - CONV and len(create) <= P14 - CREATE and CONV + len(conv) <= CREATE

p = sys.argv[1]; d = bytearray(open(p, 'rb').read())
def put(a, b, allow=()):
    cur = bytes(d[a:a + len(b)])
    assert cur == b or cur == bytes(len(b)) or cur in allow, f'{a:#x} not empty/expected: {cur.hex()}'
    d[a:a + len(b)] = b
ja = lambda t: b'\x0f\x87' + struct.pack('<i', t - (JA + 6))
assert bytes(d[JA:JA + 6]) in (ja(0x16437), ja(SKIP), ja(P14)), 'unexpected ja'
assert d[0x2bbe8:0x2bbea] == b'\x38\x04', 'patch_nodes.py not applied'
assert struct.unpack_from('<Q', d, LG)[0] == 0
# old patch_param14 cave (29 bytes) may be there: clear it first
old14 = bytes(d[P14:P14 + 0x40])
if old14[:3] == H('83fe0c'): d[P14:P14 + 0x40] = bytes(0x40)
put(CONV, conv); put(CREATE, create); put(P14, p14)
put(CRT, H('e8') + struct.pack('<i', CREATE - (CRT + 5)) + H('0f1f00') + H('0f1f00') + H('90'),
    allow=(H('498b0424488b4030488947 08'.replace(' ', '')),))
d[JA:JA + 6] = ja(P14)
open(p, 'wb').write(d)
print(f'ok: conv {CONV:#x}+{len(conv)}, create hook {CREATE:#x}+{len(create)}, param14 {P14:#x}+{len(p14)}')
