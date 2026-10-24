#!/usr/bin/env python3
# patch_noscaler.py <plugin binary> (24/10): stop advertising the VPHAL scaler function (capability type 2, id 0x800;
# entries at __DATA 0x787a30, 0x78cdf0, 0x78fc00). The stock VPHAL cannot create its context on this platform (err 11),
# which made AppleGVA's encoder fail ("scaler: context creation failed") because it creates every advertised scaler.
# Making VPHAL work (patch_vphal/patch_vpnull) instead also turned on GPU pixel transfers for all of VideoToolbox
# (VTPixelTransferSession -> AVFPixelProcess), which crashed on 1080p and runs EU kernels. Without the capability the
# encoder creates no scaler and VideoToolbox keeps converting images in software, as with the stock plugin.
import sys, struct
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
E = [(0x787a30, 0x80000004), (0x78cdf0, 0x80000005), (0x78fc00, 0x80000006)]
if all(struct.unpack('<IIQ', b[e:e+16]) == (2, 0, n) for e, n in E): print('already patched'); sys.exit(0)
for e, n in E:
    assert struct.unpack('<IIQ', b[e:e+16]) == (2, 0, 0x800), (hex(e), b[e:e+16].hex())
    b[e+8:e+16] = struct.pack('<Q', n)
open(p, 'wb').write(b); print('patched: VPHAL scaler capability (0x800) no longer advertised')
