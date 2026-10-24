#!/usr/bin/env python3
# patch_nowe.py <plugin binary> (24/10): stop advertising the WeightEstimate function (capability type 2, id 0x400000,
# entry at __DATA 0x789bb0). From the second P frame on, AppleGVA's low-latency encoder (Branch_Encoder0::createWeightEstimate)
# runs a weighted-prediction estimation media kernel on the EUs (AVD_WeightEstimateContext) that hangs the render engine on
# this GPU. Without the capability the encoder clears its flag and encodes without weighted prediction, the same as
# "defaults write com.apple.GVAEncoder WeightedPrediction -bool NO". Id changed to 0x80000002, which nothing asks for.
import sys, struct
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
E = 0x789bb0
if struct.unpack('<IIQ', b[E:E+16]) == (2, 0, 0x80000002): print('already patched'); sys.exit(0)
assert struct.unpack('<IIQ', b[E:E+16]) == (2, 0, 0x400000), b[E:E+16].hex()
b[E+8:E+16] = struct.pack('<Q', 0x80000002)
open(p, 'wb').write(b); print('patched: WeightEstimate capability no longer advertised')
