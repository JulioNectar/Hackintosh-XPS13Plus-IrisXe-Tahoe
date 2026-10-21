// xe-vtdec: onde a decodificação de vídeo por hardware falha? Gera 30 quadros H.264 (e HEVC) com o codificador por
// software, depois tenta decodificar com: (1) hardware obrigatório, (2) padrão, (3) software. Mostra status de cada etapa.
#import <VideoToolbox/VideoToolbox.h>
#import <CoreVideo/CoreVideo.h>
#include <stdio.h>
static NSMutableArray *quadros;
static void codifica(CMVideoCodecType codec) {
  quadros = [NSMutableArray array]; VTCompressionSessionRef cs; int w = 1280, h = 720;
  OSStatus st = VTCompressionSessionCreate(NULL, w, h, codec, (__bridge CFDictionaryRef)@{(id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder:@NO}, NULL, NULL, NULL, NULL, &cs);
  if (st) { printf("  codificador: st=%d\n", (int)st); return; }
  for (int f = 0; f < 30; f++) {
    CVPixelBufferRef pb; CVPixelBufferCreate(NULL, w, h, '420v', (__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}}, &pb);
    CVPixelBufferLockBaseAddress(pb, 0); uint8_t *Y = CVPixelBufferGetBaseAddressOfPlane(pb, 0); size_t by = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) Y[y*by+x] = (uint8_t)((x + y + f*8) ^ (x >> 3));
    memset(CVPixelBufferGetBaseAddressOfPlane(pb, 1), 128, CVPixelBufferGetBytesPerRowOfPlane(pb, 1) * h / 2); CVPixelBufferUnlockBaseAddress(pb, 0);
    VTCompressionSessionEncodeFrameWithOutputHandler(cs, pb, CMTimeMake(f, 30), kCMTimeInvalid, NULL, NULL, ^(OSStatus s, VTEncodeInfoFlags fl, CMSampleBufferRef sb) { if (!s && sb) [quadros addObject:(__bridge id)sb]; });
    CVPixelBufferRelease(pb);
  }
  VTCompressionSessionCompleteFrames(cs, kCMTimeInvalid); VTCompressionSessionInvalidate(cs); CFRelease(cs);
  printf("  %lu quadros codificados\n", (unsigned long)quadros.count);
}
static void decodifica(const char *nome, NSDictionary *spec, NSDictionary *saida) {
  if (!quadros.count) return;
  CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription((__bridge CMSampleBufferRef)quadros[0]); VTDecompressionSessionRef ds = NULL;
  OSStatus st = VTDecompressionSessionCreate(NULL, fd, (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)saida, NULL, &ds);
  if (st) { printf("  %-28s criar sessão: st=%d\n", nome, (int)st); return; }
  CFBooleanRef hw = NULL; VTSessionCopyProperty(ds, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &hw);
  __block int ok = 0, erro = 0; __block OSStatus ult = 0; __block double soma = 0;
  for (id q in quadros) VTDecompressionSessionDecodeFrameWithOutputHandler(ds, (__bridge CMSampleBufferRef)q, 0, NULL, ^(OSStatus s, VTDecodeInfoFlags fl, CVImageBufferRef ib, CMTime p, CMTime d) {
    if (s || !ib) { erro++; ult = s; return; } ok++;
    CVPixelBufferLockBaseAddress(ib, 1); uint8_t *b = CVPixelBufferGetPlaneCount(ib) ? CVPixelBufferGetBaseAddressOfPlane(ib, 0) : CVPixelBufferGetBaseAddress(ib);
    double s2 = 0; for (int i = 0; i < 4096; i++) s2 += b[i * 37]; soma += s2 / 4096; CVPixelBufferUnlockBaseAddress(ib, 1); });
  VTDecompressionSessionWaitForAsynchronousFrames(ds);
  printf("  %-28s hardware=%s quadros ok=%d erro=%d (último st=%d) média=%.0f\n", nome, hw == kCFBooleanTrue ? "SIM" : "não", ok, erro, (int)ult, ok ? soma / ok : 0);
  VTDecompressionSessionInvalidate(ds); CFRelease(ds);
}
int main(void) { @autoreleasepool {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("VTIsHardwareDecodeSupported: H.264=%d HEVC=%d\n", VTIsHardwareDecodeSupported(kCMVideoCodecType_H264), VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC));
  CMVideoCodecType cs[] = {kCMVideoCodecType_H264, kCMVideoCodecType_HEVC}; const char *nm[] = {"H.264", "HEVC"};
  for (int i = 0; i < 2; i++) {
    printf("%s:\n", nm[i]); codifica(cs[i]);
    decodifica("hardware obrigatório", @{(id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder:@YES}, nil);
    decodifica("hardware obrig. + BGRA", @{(id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder:@YES}, @{(id)kCVPixelBufferPixelFormatTypeKey:@('BGRA')});
    decodifica("padrão", @{}, nil);
    decodifica("software", @{(id)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder:@NO}, nil);
  }
}}
