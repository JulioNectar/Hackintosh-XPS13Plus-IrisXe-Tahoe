#!/usr/bin/env python3
# patch_nocksum.py <plugin binary>: stop advertising the FrameChecksum function (capability type 2, id 0x1000000, entry at
# __DATA 0x78a410, listed from __DATA_CONST 0x754e08). AppleGVA's low-latency encoder (Branch_Encoder0::createChecksum)
# only builds its checksum stage when it finds this capability; that stage runs a media EU kernel (AVD_FrameChecksum,
# MEDIA_OBJECT_WALKER 45x2 at 1280x720) which hangs the render engine on this GPU. Without the capability createChecksum
# fails, the encoder clears its checksum flag and carries on (Branch_Encoder0 setup 0x81ecc, AppleGVA 26.7.1).
# The id is changed to 0x80000000, which nothing asks for. __DATA is mapped 1:1 to the file (vmaddr = fileoff 0x760000).
import sys, struct
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
E = 0x78a410
if struct.unpack('<IIQ', b[E:E+16]) == (2, 0, 0x80000000): print('already patched'); sys.exit(0)
assert struct.unpack('<IIQ', b[E:E+16]) == (2, 0, 0x1000000), b[E:E+16].hex()
b[E+8:E+16] = struct.pack('<Q', 0x80000000)
open(p, 'wb').write(b); print('patched: FrameChecksum capability no longer advertised')
