// xe-yuvtest: cada linha de 256 B de uma IOSurface 420v leva o próprio índice; a GPU (blit textura->buffer) lê o
// 1º texel de cada plano -> mostra de qual deslocamento leu. Correto: plano 1 = off1/256 (0x40). Bug antigo: 00.
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-yuvtest ~/xe/tools/xe-yuvtest.m -framework Metal -framework IOSurface -framework Foundation
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <stdio.h>
static int teste(id<MTLDevice> dev, int off1, int alloc) {
    int W = 64, H = 64, ok = 1;
    NSDictionary *p = @{(id)kIOSurfaceWidth:@(W), (id)kIOSurfaceHeight:@(H), (id)kIOSurfacePixelFormat:@('420v'),
        (id)kIOSurfacePlaneInfo:@[
          @{(id)kIOSurfacePlaneWidth:@(W), (id)kIOSurfacePlaneHeight:@(H), (id)kIOSurfacePlaneBytesPerRow:@256, (id)kIOSurfacePlaneBytesPerElement:@1, (id)kIOSurfacePlaneOffset:@0, (id)kIOSurfacePlaneSize:@(256*H)},
          @{(id)kIOSurfacePlaneWidth:@(W/2), (id)kIOSurfacePlaneHeight:@(H/2), (id)kIOSurfacePlaneBytesPerRow:@256, (id)kIOSurfacePlaneBytesPerElement:@2, (id)kIOSurfacePlaneOffset:@(off1), (id)kIOSurfacePlaneSize:@(256*H/2)}],
        (id)kIOSurfaceAllocSize:@(alloc)};
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
    if (!s) { printf("off1=%d: IOSurfaceCreate falhou\n", off1); return 0; }
    IOSurfaceLock(s, 0, NULL);
    uint8_t *b0 = IOSurfaceGetBaseAddress(s); size_t sz = IOSurfaceGetAllocSize(s);
    for (size_t a = 0; a < sz; a++) b0[a] = (uint8_t)((a / 256) & 0xff);
    IOSurfaceUnlock(s, 0, NULL);
    printf("off1=%d alloc=%zu:", off1, sz);
    for (int pl = 0; pl < 2; pl++) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(pl ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm)
                                   width:(pl ? W/2 : W) height:(pl ? H/2 : H) mipmapped:NO];
        d.storageMode = MTLStorageModeManaged;
        id<MTLTexture> t = [dev newTextureWithDescriptor:d iosurface:s plane:pl];
        id<MTLBuffer> b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; memset(b.contents, 0xEE, 64);
        id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(1,2,1)
                  toBuffer:b destinationOffset:0 destinationBytesPerRow:(pl ? 2 : 1) destinationBytesPerImage:(pl ? 4 : 2)];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        uint8_t *o = b.contents; int esp0 = pl ? (off1 / 256) & 0xff : 0, got1 = pl ? o[2] : o[1];
        printf("  plano%d: %02x %02x (esperado %02x %02x)", pl, o[0], got1, esp0, esp0 + 1);
        if (o[0] != esp0 || got1 != esp0 + 1) ok = 0;
    }
    printf("  -> %s\n", ok ? "OK" : "ERRADO"); CFRelease(s); return ok;
}
int main() { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    printf("GPU: %s\n", dev.name.UTF8String);
    int ok = teste(dev, 16384, 65536) & teste(dev, 32768, 65536);
    printf("RESULTADO: %s\n", ok ? "YUV OK" : "YUV COM DEFEITO");
    return ok ? 0 : 1;
} }
