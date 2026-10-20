// xe-xfertest: VTPixelTransferSession 420v -> vários formatos (o passo de conversão que o decodificador usa).
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-xfertest ~/xe/tools/xe-xfertest.m -framework VideoToolbox -framework CoreVideo -framework Foundation
#import <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
static CVPixelBufferRef mk(OSType f, int w, int h) {
    CVPixelBufferRef pb = NULL;
    CVPixelBufferCreate(NULL, w, h, f, (__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}}, &pb);
    return pb;
}
int main() { @autoreleasepool {
    VTPixelTransferSessionRef x = NULL;
    printf("criar sessão: %d\n", (int)VTPixelTransferSessionCreate(NULL, &x));
    CVPixelBufferRef src = mk('420v', 640, 480);
    OSType dst[] = {'420v', '420f', 'BGRA', 'x420', 'RGhA'};
    for (int i = 0; i < 5; i++) {
        CVPixelBufferRef d = mk(dst[i], 640, 480);
        OSStatus st = VTPixelTransferSessionTransferImage(x, src, d);
        OSType f = dst[i];
        printf("420v -> %c%c%c%c  mesmo tamanho: %d", f>>24, (f>>16)&0xff, (f>>8)&0xff, f&0xff, (int)st);
        CVPixelBufferRelease(d);
        d = mk(dst[i], 320, 240);
        printf("   reduzindo: %d\n", (int)VTPixelTransferSessionTransferImage(x, src, d));
        CVPixelBufferRelease(d);
    }
} return 0; }
