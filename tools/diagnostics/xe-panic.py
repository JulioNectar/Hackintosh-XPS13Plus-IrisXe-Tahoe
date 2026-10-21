# xe-panic.py arquivo.panic: decodifica os frames do backtrace que caem no AppleIntelTGLGraphics (16.0.32, nm do
# binário em ~/xe/nooted/stage/LE) e no IOAcceleratorFamily2 (símbolos do KC do sistema, ~/xe/re/iaf2-syms.txt).
import json, re, sys, subprocess, bisect, os
t = open(sys.argv[1]).read(); s = json.loads(t.split('\n', 1)[1]).get('macOSPanicString', '')
print(s[:180].replace('\n', ' '))
m = re.search(r'Panicked task[^\n]*', s); print(m.group(0)[:100] if m else '')
H = os.path.expanduser('~/xe')
def carrega(nm):
    v = sorted((int(l.split()[0], 16), l.split()[-1]) for l in nm if len(l.split()) >= 2 and re.match(r'^[0-9a-f]+$', l.split()[0]))
    return v, [a for a, _ in v]
tgl = carrega(subprocess.run(['nm', '-n', H + '/nooted/stage/LE/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics'], capture_output=True, text=True).stdout.splitlines())
iaf = carrega(open(H + '/re/iaf2-syms.txt').read().splitlines()) if os.path.exists(H + '/re/iaf2-syms.txt') else ([], [])
kb = {k: int(a, 16) for k, a in re.findall(r'(AppleIntelTGLGraphics|IOAcceleratorFamily2)\([^)]*\)\[[^\]]*\]@(0x[0-9a-f]+)', s)}
def dem(n): return subprocess.run(['c++filt', n], capture_output=True, text=True).stdout.strip()[:120]
bt = s[s.find('Backtrace'):s.find('Kernel Extensions')]
for a in re.findall(r': (0x[0-9a-f]+)', bt):
    v = int(a, 16); txt = ''
    for k, base, (syms, addrs), textvm in (('TGL', kb.get('AppleIntelTGLGraphics'), tgl, 0), ('IAF2', kb.get('IOAcceleratorFamily2'), iaf, 0x14b65000)):
        if base and base <= v < base + 0x200000 and addrs:
            x = v - base + textvm; i = bisect.bisect_right(addrs, x) - 1
            if i >= 0: txt = '%s %s+0x%x' % (k, dem(syms[i][1]), x - addrs[i])
    if txt: print('  ', txt)
