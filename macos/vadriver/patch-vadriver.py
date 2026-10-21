#!/usr/bin/env python3
# patch-vadriver.py <plugin binary>  (step 1 of build-vadriver.sh, in place)
# Lets AppleGVA on Tahoe load the Tiger Lake VA (video) plugin on a Raptor Lake iGPU spoofed as 9A49:
# device-id whitelist, service lookup, Tahoe bitstream-buffer request, unknown driver parameters, VAME name.
# Run the whole chain with build-vadriver.sh (it starts from the stock 16.0.32 bundle and signs the result).
# Install in /Library/GPUBundles (AppleGVA only looks in /System/Library/Extensions and /Library/GPUBundles).
import sys
p = sys.argv[1]; d = bytearray(open(p, 'rb').read())
def patch(off, old, new, why):
    old = bytes.fromhex(old); new = bytes.fromhex(new)
    if d[off:off + len(new)] == new: print('already:', why); return
    assert d[off:off + len(old)] == old, f'unexpected bytes at {off:#x} ({why})'
    d[off:off + len(new)] = new; print('patched:', why)
# 1. device-id whitelist (9A40/9A48/...) also accepts 9A49
patch(0x130aa, '3d8680489a', '3d8680499a', 'device-id 0x9A498086 accepted')
# 2. find IntelAccelerator by name (AppleGVA passes an accelerator index the plugin mis-resolves)
patch(0x13172, '742b', 'eb2b', 'service lookup by name')
# 3. bitstream buffer request: Tahoe AppleGVA no longer fills 'bwar' at +0xc and 1 at +0x10
patch(0x15456, '756e', '9090', 'skip bwar fourcc check')
patch(0x1545c, '7568', '9090', 'skip flag==1 check')
# 4. GetDriverParameter: unknown ids (Tahoe asks id 14) are skipped instead of returning error 5
patch(0x1623c, '0f87f5010000', '0f87db010000', 'skip unknown driver parameters')
# 5. the plugin dlopens the ICL VAME (copy-paste from the ICL driver); use the TGL one
a, b = b'AppleIntelICLGraphicsVAME', b'AppleIntelTGLGraphicsVAME'
n = d.count(a); d = bytearray(bytes(d).replace(a, b)); print(f'VAME name ICL->TGL: {n} occurrences')
open(p, 'wb').write(d)
