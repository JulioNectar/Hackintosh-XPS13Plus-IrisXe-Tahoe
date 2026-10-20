// xe-f16test: textura RGBA16Float (64 bpp) — upload pela CPU, cópia GPU buffer->textura e textura->textura;
// confere TODAS as linhas (uma cópia parcial aparece como linhas erradas).
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-f16test ~/xe/tools/xe-f16test.m -framework Metal -framework Foundation
#import <Metal/Metal.h>
#include <stdio.h>
static int confere(id<MTLTexture> t, int W, int H, uint64_t esperado, const char *nome) {
    id<MTLDevice> dev = t.device;
    id<MTLBuffer> o = [dev newBufferWithLength:(size_t)W * H * 8 options:MTLResourceStorageModeShared]; memset(o.contents, 0xEE, (size_t)W * H * 8);
    id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1) toBuffer:o destinationOffset:0 destinationBytesPerRow:W * 8 destinationBytesPerImage:(size_t)W * H * 8];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    uint64_t *p = o.contents; int ruins = 0, primeira = -1;
    for (int y = 0; y < H; y++) { int ok = 1; for (int x = 0; x < W; x++) if (p[(size_t)y * W + x] != esperado) { ok = 0; break; } if (!ok) { ruins++; if (primeira < 0) primeira = y; } }
    printf("  %-34s %4dx%-4d linhas erradas=%d%s\n", nome, W, H, ruins, ruins ? [NSString stringWithFormat:@" (1ª=%d, valor %016llx)", primeira, p[(size_t)primeira * W]].UTF8String : "");
    return ruins;
}
int main() { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue];
    uint64_t azul = 0x3c003c0000000000ull; // R=0 G=0 B=1.0 A=1.0 em half (LE: R,G,B,A)
    int tam[][2] = {{140,140},{256,256},{512,512},{1024,1024},{100,37}};
    for (int i = 0; i < 5; i++) {
        int W = tam[i][0], H = tam[i][1];
        uint64_t *px = malloc((size_t)W * H * 8); for (size_t k = 0; k < (size_t)W * H; k++) px[k] = azul;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:W height:H mipmapped:NO];
        d.storageMode = MTLStorageModeManaged; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> a = [dev newTextureWithDescriptor:d];
        [a replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 8];
        confere(a, W, H, azul, "CPU replaceRegion");
        d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> b = [dev newTextureWithDescriptor:d];
        id<MTLBuffer> src = [dev newBufferWithBytes:px length:(size_t)W * H * 8 options:MTLResourceStorageModeShared];
        id<MTLTexture> c = [dev newTextureWithDescriptor:d];
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:W * 8 sourceBytesPerImage:(size_t)W * H * 8 sourceSize:MTLSizeMake(W,H,1) toTexture:b destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        [e copyFromTexture:b toTexture:c];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        confere(b, W, H, azul, "GPU buffer->textura (privada)");
        confere(c, W, H, azul, "GPU textura->textura");
        free(px);
    }
} return 0; }
