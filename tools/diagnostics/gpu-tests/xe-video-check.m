// xe-video-check: confere conversões de vídeo do sistema (VTPixelTransfer) e, com um .mov, a média de cor dos quadros
// decodificados (verde puro = R0 G~77 B0 = YUV zerado).   xe-video-check            xe-video-check arquivo.mov
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
static CVPixelBufferRef mk(OSType f) { CVPixelBufferRef pb; CVPixelBufferCreate(NULL, 1920, 1080, f, (__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}}, &pb); return pb; }
static double avg0(CVPixelBufferRef pb) { CVPixelBufferLockBaseAddress(pb, 1); size_t np = CVPixelBufferGetPlaneCount(pb);
  uint8_t *b = np ? CVPixelBufferGetBaseAddressOfPlane(pb,0) : CVPixelBufferGetBaseAddress(pb); size_t n = np ? CVPixelBufferGetBytesPerRowOfPlane(pb,0)*CVPixelBufferGetHeightOfPlane(pb,0) : CVPixelBufferGetDataSize(pb);
  double s = 0; for (size_t i = 0; i < n; i += 16) s += b[i]; CVPixelBufferUnlockBaseAddress(pb, 1); return s / (n/16); }
static void fill(CVPixelBufferRef pb, int v) { CVPixelBufferLockBaseAddress(pb, 0); size_t np = CVPixelBufferGetPlaneCount(pb);
  for (size_t p = 0; p < (np ? np : 1); p++) memset(np ? CVPixelBufferGetBaseAddressOfPlane(pb,p) : CVPixelBufferGetBaseAddress(pb), v, np ? CVPixelBufferGetBytesPerRowOfPlane(pb,p)*CVPixelBufferGetHeightOfPlane(pb,p) : CVPixelBufferGetDataSize(pb));
  CVPixelBufferUnlockBaseAddress(pb, 0); }
int main(int c, char **v) { @autoreleasepool {
  if (c < 2) {
    OSType fs[] = {'420v', '420f', 'BGRA', '2vuy'}; VTPixelTransferSessionRef ts; VTPixelTransferSessionCreate(NULL, &ts); int bons = 0, tot = 0;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { if (i == j) continue;
      CVPixelBufferRef a = mk(fs[i]), b = mk(fs[j]); fill(a, 120); fill(b, 0);
      OSStatus st = VTPixelTransferSessionTransferImage(ts, a, b); double m = avg0(b); tot++; if (!st && m > 1) bons++;
      printf("%c%c%c%c -> %c%c%c%c: st=%d média=%.0f %s\n", fs[i]>>24, fs[i]>>16, fs[i]>>8, fs[i], fs[j]>>24, fs[j]>>16, fs[j]>>8, fs[j], (int)st, m, m > 1 ? "" : "<<< VAZIO");
      CVPixelBufferRelease(a); CVPixelBufferRelease(b); }
    printf("conversões com conteúdo: %d/%d\n", bons, tot); return 0;
  }
  AVURLAsset *as = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(v[1])] options:nil];
  AVAssetTrack *t = [[as tracksWithMediaType:AVMediaTypeVideo] firstObject];
  AVAssetReader *r = [AVAssetReader assetReaderWithAsset:as error:nil];
  AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA)}];
  [r addOutput:o]; [r startReading]; CMSampleBufferRef sb; int n = 0;
  while ((sb = [o copyNextSampleBuffer]) && n < 3) { CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb); CVPixelBufferLockBaseAddress(pb, 1);
    uint8_t *b = CVPixelBufferGetBaseAddress(pb); size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), bpr = CVPixelBufferGetBytesPerRow(pb); double s[3] = {0}; long k = 0;
    for (size_t y = 0; y < h; y += 8) for (size_t x = 0; x < w; x += 8) { uint8_t *p = b + y*bpr + x*4; s[0] += p[2]; s[1] += p[1]; s[2] += p[0]; k++; }
    printf("quadro %d (%zux%zu): média R=%.0f G=%.0f B=%.0f\n", n, w, h, s[0]/k, s[1]/k, s[2]/k); CVPixelBufferUnlockBaseAddress(pb, 1); CFRelease(sb); n++; }
}}
