# kcsyms.py KC bundle-id: símbolos (vmaddr nome) de um kext dentro de um Kernel Collection (fileset)
import struct,sys
kc=open(sys.argv[1],'rb').read(); want=sys.argv[2]
def cmds(off):
    ncmds=struct.unpack_from('<I',kc,off+16)[0]; p=off+32
    for _ in range(ncmds):
        c,s=struct.unpack_from('<II',kc,p); yield c,p,s; p+=s
hdr=None
for c,p,s in cmds(0):
    if c==0x80000035:
        vm,fo,eo=struct.unpack_from('<QQI',kc,p+8); name=kc[p+eo:kc.index(b'\0',p+eo)].decode()
        if name==want: hdr=fo; break
if hdr is None: sys.exit('nao achei')
for c,p,s in cmds(hdr):
    if c==2:
        symoff,nsyms,stroff,strsize=struct.unpack_from('<IIII',kc,p+8)
        for i in range(nsyms):
            n_strx,n_type,n_sect,n_desc,n_value=struct.unpack_from('<IBBhQ',kc,symoff+16*i)
            if n_type&0x0e==0x0e and n_value: print('%x %s'%(n_value,kc[stroff+n_strx:kc.index(b'\0',stroff+n_strx)].decode(errors='replace')))
