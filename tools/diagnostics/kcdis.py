# kcdis.py KC symlist.txt nome_substr [max_instr]: desmonta funções de um kext dentro do Kernel Collection
import struct,sys,bisect,capstone,subprocess
kc=open(sys.argv[1],'rb').read()
syms=sorted((int(l.split()[0],16),l.split()[1]) for l in open(sys.argv[2]) if not l.startswith('#'))
addrs=[a for a,_ in syms]; names={}
for a,n in syms: names.setdefault(a,n)
segs=[]
ncmds=struct.unpack_from('<I',kc,16)[0]; p=32
for _ in range(ncmds):
    c,s=struct.unpack_from('<II',kc,p)
    if c==0x19:
        vm,vs,fo,fs=struct.unpack_from('<QQQQ',kc,p+24); segs.append((vm,vs,fo))
    p+=s
def off(v):
    for vm,vs,fo in segs:
        if vm<=v<vm+vs: return fo+(v-vm)
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
def dem(n):
    try: return subprocess.run(['c++filt',n],capture_output=True,text=True).stdout.strip()
    except Exception: return n
want=sys.argv[3]; mx=int(sys.argv[4]) if len(sys.argv)>4 else 400
for i,(a,n) in enumerate(syms):
    if want in n and (i==0 or syms[i-1][0]!=a):
        end=addrs[bisect.bisect_right(addrs,a)] if bisect.bisect_right(addrs,a)<len(addrs) else a+0x2000
        o=off(a); code=kc[o:o+min(end-a,0x4000)]
        print('==',dem(n),hex(a))
        for k,ins in enumerate(md.disasm(code,a)):
            if k>=mx: break
            t=''
            if ins.mnemonic in ('call','jmp') and ins.op_str.startswith('0x'):
                x=int(ins.op_str,16); j=bisect.bisect_right(addrs,x)-1
                if j>=0 and addrs[j]==x: t='  ; '+dem(syms[j][1])
            print('%x\t%s %s%s'%(ins.address,ins.mnemonic,ins.op_str,t))
