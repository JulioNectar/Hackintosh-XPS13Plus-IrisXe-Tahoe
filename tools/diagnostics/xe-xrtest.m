// xe-xrtest: o driver Metal aceita BGR10_XR / BGRA10_XR? a GPU lê uma IOSurface 'w30r' certo?
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-xrtest ~/xe/tools/xe-xrtest.m -framework Metal -framework IOSurface -framework Foundation
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <stdio.h>
int main() { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    struct { MTLPixelFormat pf; const char *n; } pfs[] = {{MTLPixelFormatBGR10_XR, "BGR10_XR"}, {MTLPixelFormatBGR10_XR_sRGB, "BGR10_XR_sRGB"},
        {MTLPixelFormatBGRA10_XR, "BGRA10_XR"}, {MTLPixelFormatBGR10A2Unorm, "BGR10A2"}, {MTLPixelFormatRGB10A2Unorm, "RGB10A2"}};
    printf("família Apple1=%d Mac2=%d\n", [dev supportsFamily:MTLGPUFamilyApple1], [dev supportsFamily:MTLGPUFamilyMac2]);
    for (int i = 0; i < 5; i++) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pfs[i].pf width:8 height:8 mipmapped:NO];
        d.storageMode = MTLStorageModeManaged;
        id<MTLTexture> t = [dev newTextureWithDescriptor:d];
        printf("%-14s (%4lu): textura %s\n", pfs[i].n, (unsigned long)pfs[i].pf, t ? "criada" : "NIL");
    }
    // IOSurface w30r -> textura BGR10_XR -> GPU copia para buffer
    int S = 16;
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@(S), (id)kIOSurfaceHeight:@(S), (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@('w30r')});
    IOSurfaceLock(s, 0, NULL); uint32_t *b = IOSurfaceGetBaseAddress(s);
    for (size_t i = 0; i < IOSurfaceGetAllocSize(s) / 4; i++) b[i] = 0x12345678u;
    IOSurfaceUnlock(s, 0, NULL);
    for (int k = 0; k < 2; k++) {
        MTLPixelFormat pf = k ? MTLPixelFormatBGR10A2Unorm : MTLPixelFormatBGR10_XR;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:S height:S mipmapped:NO];
        d.storageMode = MTLStorageModeManaged;
        id<MTLTexture> t = [dev newTextureWithDescriptor:d iosurface:s plane:0];
        if (!t) { printf("w30r como %s: textura NIL\n", k ? "BGR10A2" : "BGR10_XR"); continue; }
        id<MTLBuffer> o = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; memset(o.contents, 0xEE, 64);
        id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(2,1,1) toBuffer:o destinationOffset:0 destinationBytesPerRow:8 destinationBytesPerImage:8];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        printf("w30r como %s: GPU leu %08x %08x (esperado 12345678) status=%ld\n", k ? "BGR10A2" : "BGR10_XR", ((uint32_t *)o.contents)[0], ((uint32_t *)o.contents)[1], (long)cb.status);
    }
} return 0; }
