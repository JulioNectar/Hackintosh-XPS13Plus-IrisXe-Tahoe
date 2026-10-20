// xe-atlastest: copia blocos (buffer -> textura privada) para posições != 0 e níveis de mipmap > 0, como um atlas,
// em formatos de 4/8/16 bytes por pixel; lê de volta e confere cada bloco.
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-atlastest ~/xe/tools/xe-atlastest.m -framework Metal -framework Foundation
#import <Metal/Metal.h>
#include <stdio.h>
static void teste(id<MTLDevice> dev, MTLPixelFormat pf, int bpp, const char *nome) {
    int W = 1280, H = 128, B = 40, levels = 4;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:W height:H mipmapped:YES];
    d.mipmapLevelCount = levels; d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    int ruins = 0, total = 0;
    struct { int l, x, y; } pos[] = {{0,0,0},{0,40,0},{0,600,40},{0,1200,80},{1,0,0},{1,100,20},{2,50,10},{3,30,0}};
    for (int i = 0; i < 8; i++) {
        int l = pos[i].l, x = pos[i].x, y = pos[i].y, bw = B >> l > 4 ? B >> l : 4;
        if (x + bw > (W >> l) || y + bw > (H >> l)) continue;
        id<MTLBuffer> src = [dev newBufferWithLength:(size_t)bw * bw * bpp options:MTLResourceStorageModeShared];
        uint8_t *p = src.contents; for (int k = 0; k < bw * bw * bpp; k++) p[k] = (uint8_t)(0x11 * (i + 1) + (k % bpp));
        id<MTLBuffer> dst = [dev newBufferWithLength:(size_t)bw * bw * bpp options:MTLResourceStorageModeShared]; memset(dst.contents, 0xEE, (size_t)bw * bw * bpp);
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:bw * bpp sourceBytesPerImage:(size_t)bw * bw * bpp sourceSize:MTLSizeMake(bw, bw, 1)
                toTexture:t destinationSlice:0 destinationLevel:l destinationOrigin:MTLOriginMake(x, y, 0)];
        [e copyFromTexture:t sourceSlice:0 sourceLevel:l sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(bw, bw, 1)
                  toBuffer:dst destinationOffset:0 destinationBytesPerRow:bw * bpp destinationBytesPerImage:(size_t)bw * bw * bpp];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        int ok = memcmp(src.contents, dst.contents, (size_t)bw * bw * bpp) == 0; total++; if (!ok) ruins++;
        if (!ok) { uint8_t *o = dst.contents; printf("  %s nível %d pos (%d,%d) %dx%d: ERRADO (lido %02x%02x%02x%02x, esperado %02x%02x%02x%02x)\n", nome, l, x, y, bw, bw, o[0],o[1],o[2],o[3], p[0],p[1],p[2],p[3]); }
    }
    printf("%-14s blocos errados: %d de %d\n", nome, ruins, total);
}
int main() { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    teste(dev, MTLPixelFormatBGRA8Unorm, 4, "BGRA8");
    teste(dev, MTLPixelFormatRGBA16Unorm, 8, "RGBA16Unorm");
    teste(dev, MTLPixelFormatRGBA16Float, 8, "RGBA16Float");
    teste(dev, MTLPixelFormatRGBA32Float, 16, "RGBA32Float");
} return 0; }
