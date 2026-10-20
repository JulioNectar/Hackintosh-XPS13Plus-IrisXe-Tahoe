// xe-miptest: textura RGBA8 com mipmaps; preenche o nível 0, gera mipmaps na GPU e lê cada nível de volta.
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-miptest ~/xe/tools/xe-miptest.m -framework Metal -framework Foundation
#import <Metal/Metal.h>
#include <stdio.h>
static void teste(id<MTLDevice> dev, MTLPixelFormat pf, const char *nome, int gerar) {
    int S = 256;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:S height:S mipmapped:YES];
    d.storageMode = MTLStorageModeManaged; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    uint8_t *px = malloc(S * S * 4);
    for (int i = 0; i < S * S; i++) { px[i*4] = 200; px[i*4+1] = 100; px[i*4+2] = 50; px[i*4+3] = 255; }
    for (int l = 0; l < (int)t.mipmapLevelCount; l++) {
        int s = S >> l; if (!gerar || l == 0) [t replaceRegion:MTLRegionMake2D(0, 0, s, s) mipmapLevel:l withBytes:px bytesPerRow:s * 4];
    }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    if (gerar) [e generateMipmapsForTexture:t];
    [e synchronizeResource:t]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    printf("%s (%s): ", nome, gerar ? "mips gerados na GPU" : "mips enviados pela CPU");
    for (int l = 0; l < (int)t.mipmapLevelCount && l < 7; l++) {
        uint8_t o[4] = {0xEE, 0xEE, 0xEE, 0xEE};
        [t getBytes:o bytesPerRow:(S >> l) * 4 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:l];
        printf(" L%d=%02x%02x%02x%02x", l, o[0], o[1], o[2], o[3]);
    }
    printf("  status=%ld\n", (long)cb.status); free(px);
}
static void teste_gpu_read(id<MTLDevice> dev) {
    // copia cada nível para um buffer pela GPU (não pela CPU)
    int S = 256;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:S height:S mipmapped:YES];
    d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    id<MTLBuffer> src = [dev newBufferWithLength:S * S * 4 options:MTLResourceStorageModeShared];
    uint8_t *px = src.contents; for (int i = 0; i < S * S; i++) { px[i*4] = 50; px[i*4+1] = 100; px[i*4+2] = 200; px[i*4+3] = 255; }
    id<MTLBuffer> dst = [dev newBufferWithLength:4 * 16 options:MTLResourceStorageModeShared]; memset(dst.contents, 0xEE, 64);
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    for (int l = 0; l < 6; l++) { int s = S >> l;
        [e copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:s * 4 sourceBytesPerImage:s * s * 4 sourceSize:MTLSizeMake(s, s, 1) toTexture:t destinationSlice:0 destinationLevel:l destinationOrigin:MTLOriginMake(0,0,0)]; }
    for (int l = 0; l < 6; l++)
        [e copyFromTexture:t sourceSlice:0 sourceLevel:l sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(1,1,1) toBuffer:dst destinationOffset:l * 4 destinationBytesPerRow:4 destinationBytesPerImage:4];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    uint8_t *o = dst.contents; printf("privada GPU->GPU: ");
    for (int l = 0; l < 6; l++) printf(" L%d=%02x%02x%02x%02x", l, o[l*4], o[l*4+1], o[l*4+2], o[l*4+3]);
    printf("  status=%ld\n", (long)cb.status);
}
int main() { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    teste(dev, MTLPixelFormatRGBA8Unorm, "RGBA8", 0);
    teste(dev, MTLPixelFormatRGBA8Unorm, "RGBA8", 1);
    teste(dev, MTLPixelFormatBGRA8Unorm, "BGRA8", 1);
    teste_gpu_read(dev);
} return 0; }
