// xe-vttest: codifica 8 quadros azuis H.264 (VTCompressionSession) e decodifica (VTDecompressionSession) com
// decodificador de hardware exigido / proibido; mostra status e o pixel do centro (esperado azul).
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-vttest ~/xe/tools/xe-vttest.m -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework Foundation
#import <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
static NSMutableArray *amostras;
static void enc_cb(void *r, void *s, OSStatus st, VTEncodeInfoFlags f, CMSampleBufferRef sb) {
    if (st == noErr && sb) [amostras addObject:(__bridge id)sb];
    else printf("  encode erro %d\n", (int)st);
}
static int ndec, nerr; static OSStatus lastErr;
static void dec_cb(void *r, void *s, OSStatus st, VTDecodeInfoFlags f, CVImageBufferRef img, CMTime pts, CMTime d) {
    if (st != noErr || !img) { nerr++; lastErr = st; return; }
    if (ndec++ == 0) {
        CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
        OSType fmt = CVPixelBufferGetPixelFormatType(img);
        size_t w = CVPixelBufferGetWidth(img), h = CVPixelBufferGetHeight(img);
        printf("  1º quadro: %zux%zu fmt '%c%c%c%c' planos=%zu", w, h, fmt>>24, (fmt>>16)&0xff, (fmt>>8)&0xff, fmt&0xff, CVPixelBufferGetPlaneCount(img));
        if (CVPixelBufferGetPlaneCount(img) == 2) {
            uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(img, 0), *c = CVPixelBufferGetBaseAddressOfPlane(img, 1);
            size_t by = CVPixelBufferGetBytesPerRowOfPlane(img, 0), bc = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
            printf("  Y=%d Cb=%d Cr=%d (azul ~ Y41 Cb240 Cr110)", y[(h/2)*by + w/2], c[(h/4)*bc + (w/4)*2], c[(h/4)*bc + (w/4)*2 + 1]);
        }
        printf("  IOSurface=%s\n", CVPixelBufferGetIOSurface(img) ? "sim" : "nao");
        CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    }
}
static void decodifica2(CMFormatDescriptionRef fd, int hw, NSDictionary *out, const char *nome);
static void decodifica(CMFormatDescriptionRef fd, int hw, int nativo) {
    ndec = nerr = 0; lastErr = 0;
    NSDictionary *spec = @{(id)(hw ? kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder
                                     : kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder): hw ? @YES : @NO};
    NSDictionary *out = nativo ? nil : @{(id)kCVPixelBufferPixelFormatTypeKey: @('420v'), (id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
    VTDecompressionOutputCallbackRecord cb = {dec_cb, NULL};
    VTDecompressionSessionRef ds = NULL;
    OSStatus st = VTDecompressionSessionCreate(NULL, fd, (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)out, &cb, &ds);
    printf("%s%s: criar sessão = %d\n", hw ? "HARDWARE" : "SOFTWARE", nativo ? " (saída nativa)" : " (saída 420v+IOSurface)", (int)st);
    if (st) return;
    for (id sb in amostras) VTDecompressionSessionDecodeFrame(ds, (__bridge CMSampleBufferRef)sb, 0, NULL, NULL);
    VTDecompressionSessionWaitForAsynchronousFrames(ds);
    printf("  decodificados=%d erros=%d (último erro %d)\n", ndec, nerr, (int)lastErr);
    VTDecompressionSessionInvalidate(ds); CFRelease(ds);
}
int main() { @autoreleasepool {
    int W = 640, H = 480; amostras = [NSMutableArray array];
    printf("VTIsHardwareDecodeSupported(H264)=%d HEVC=%d\n", VTIsHardwareDecodeSupported(kCMVideoCodecType_H264), VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC));
    VTCompressionSessionRef cs = NULL;
    NSDictionary *espec = @{(id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @NO};
    OSStatus st = VTCompressionSessionCreate(NULL, W, H, kCMVideoCodecType_H264, (__bridge CFDictionaryRef)espec, NULL, NULL, enc_cb, NULL, &cs);
    if (st) { printf("encoder: %d\n", (int)st); return 1; }
    for (int i = 0; i < 8; i++) {
        CVPixelBufferRef pb; CVPixelBufferCreate(NULL, W, H, '420v', (__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}}, &pb);
        CVPixelBufferLockBaseAddress(pb, 0);
        uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *c = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        for (int r = 0; r < H; r++) memset(y + r * CVPixelBufferGetBytesPerRowOfPlane(pb, 0), 41, W);
        for (int r = 0; r < H / 2; r++) for (int k = 0; k < W / 2; k++) { c[r*CVPixelBufferGetBytesPerRowOfPlane(pb,1)+2*k] = 240; c[r*CVPixelBufferGetBytesPerRowOfPlane(pb,1)+2*k+1] = 110; }
        CVPixelBufferUnlockBaseAddress(pb, 0);
        VTCompressionSessionEncodeFrame(cs, pb, CMTimeMake(i, 30), kCMTimeInvalid, NULL, NULL, NULL);
        CVPixelBufferRelease(pb);
    }
    VTCompressionSessionCompleteFrames(cs, kCMTimeInvalid);
    printf("codificados: %lu quadros\n", (unsigned long)amostras.count);
    if (!amostras.count) return 1;
    CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription((__bridge CMSampleBufferRef)amostras[0]);
    if (getenv("SOFMT")) { decodifica2(fd, 0, @{(id)kCVPixelBufferPixelFormatTypeKey: @('BGRA')}, "só formato BGRA"); return 0; }
    decodifica(fd, 1, 0);
    decodifica(fd, 0, 0);
    decodifica(fd, 0, 1);
    decodifica2(fd, 0, @{(id)kCVPixelBufferPixelFormatTypeKey: @('420v')}, "só formato 420v");
    decodifica2(fd, 0, @{(id)kCVPixelBufferPixelFormatTypeKey: @('BGRA')}, "só formato BGRA");
    decodifica2(fd, 0, @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}}, "só IOSurface");
    decodifica2(fd, 0, @{(id)kCVPixelBufferMetalCompatibilityKey: @YES}, "só Metal");
    decodifica2(fd, 0, @{(id)kCVPixelBufferWidthKey: @640, (id)kCVPixelBufferHeightKey: @480}, "só tamanho");
} return 0; }

static void decodifica2(CMFormatDescriptionRef fd, int hw, NSDictionary *out, const char *nome) {
    ndec = nerr = 0; lastErr = 0;
    VTDecompressionOutputCallbackRecord cb = {dec_cb, NULL};
    VTDecompressionSessionRef ds = NULL;
    OSStatus st = VTDecompressionSessionCreate(NULL, fd, (__bridge CFDictionaryRef)@{(id)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: @NO}, (__bridge CFDictionaryRef)out, &cb, &ds);
    if (st) { printf("[%s] criar sessão = %d\n", nome, (int)st); return; }
    for (id sb in amostras) VTDecompressionSessionDecodeFrame(ds, (__bridge CMSampleBufferRef)sb, 0, NULL, NULL);
    VTDecompressionSessionWaitForAsynchronousFrames(ds);
    printf("[%s] decodificados=%d erros=%d (último %d)\n", nome, ndec, nerr, (int)lastErr);
    VTDecompressionSessionInvalidate(ds); CFRelease(ds);
}
