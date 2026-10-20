# mtl-diff.py ORIGINAL PATCHADO: lista as diferenças entre dois Mach-O x86_64 fora da assinatura (LC_CODE_SIGNATURE).
import struct, sys
def sig(b):
    ncmds = struct.unpack_from('<I', b, 16)[0]; p = 32
    for _ in range(ncmds):
        c, s = struct.unpack_from('<II', b, p)
        if c == 0x1d: return struct.unpack_from('<II', b, p + 8)
        p += s
    return (len(b), 0)
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
sa, sb = sig(a), sig(b)
print('assinatura original off=0x%x tam=%d | patchado off=0x%x tam=%d | tamanhos %d %d' % (sa[0], sa[1], sb[0], sb[1], len(a), len(b)))
n = min(sa[0], sb[0]); i = 0; difs = []
while i < n:
    if a[i] != b[i]:
        j = i
        while j < n and (a[j] != b[j] or (j + 1 < n and a[j + 1] != b[j + 1])) and j - i < 4096: j += 1
        difs.append((i, a[i:j], b[i:j])); i = j
    else: i += 1
for o, x, y in difs: print('0x%06x %3d bytes: %s -> %s' % (o, len(x), x[:48].hex(), y[:48].hex()))
print('total', len(difs), 'regiões; extra no fim do patchado antes da assinatura:', sb[0] - sa[0])
