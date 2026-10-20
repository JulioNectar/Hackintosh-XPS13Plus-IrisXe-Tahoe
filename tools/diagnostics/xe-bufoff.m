// xe-bufoff: textura sobre MTLBuffer com offsets variados; lê por blit e por AMOSTRAGEM num shader (como o CoreAnimation).
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-bufoff ~/xe/tools/xe-bufoff.m -framework Metal -framework Foundation
#import <Metal/Metal.h>
#include <stdio.h>
static const char *SRC =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct VO { float4 p [[position]]; float2 uv; };\n"
"vertex VO vs(uint i [[vertex_id]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; VO o; o.p = float4(p[i],0,1); o.uv = (p[i] + 1) * 0.5; o.uv.y = 1 - o.uv.y; return o; }\n"
"fragment half4 fs(VO v [[stage_in]], texture2d<half> t [[texture(0)]]) { constexpr sampler s(filter::linear); return t.sample(s, v.uv); }\n";
int main() { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *e = nil; id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(SRC) options:nil error:&e]; if (!lib) { printf("%s\n", e.description.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    int W = 80, H = 80; size_t bpr = getenv("BPR") ? atoi(getenv("BPR")) : 512;
    size_t offs[] = {0, 16, 32, 64, 128, 256, 512, 4096};
    printf("alinhamento mínimo de offset informado pelo device: %lu\n", (unsigned long)[dev minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm]);
    for (int k = 0; k < 8; k++) {
        size_t off = offs[k];
        id<MTLBuffer> b = [dev newBufferWithLength:off + bpr * H + 4096 options:MTLResourceStorageModeShared];
        uint32_t *px = (uint32_t *)((uint8_t *)b.contents + off); for (int i = 0; i < W * H; i++) px[i] = 0xff0000ffu; // BGRA azul? (B=ff G=00 R=00 A=ff) em LE: 0xAARRGGBB -> ff0000ff = A=ff R=00 G=00 B=ff
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [b newTextureWithDescriptor:d offset:off bytesPerRow:bpr];
        if (!t) { printf("off=%-5zu textura NIL\n", off); continue; }
        // render: amostra a textura numa render target branca
        MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:16 height:16 mipmapped:NO];
        rd.usage = MTLTextureUsageRenderTarget; rd.storageMode = MTLStorageModeManaged; id<MTLTexture> rt = [dev newTextureWithDescriptor:rd];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 1, 1, 1);
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re setRenderPipelineState:ps]; [re setFragmentTexture:t atIndex:0]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding];
        id<MTLBuffer> o = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; memset(o.contents, 0xEE, 16);
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; [be synchronizeResource:rt];
        [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(40,40,0) sourceSize:MTLSizeMake(1,1,1) toBuffer:o destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
        [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
        uint8_t p[4]; [rt getBytes:p bytesPerRow:64 fromRegion:MTLRegionMake2D(8, 8, 1, 1) mipmapLevel:0];
        uint8_t *bl = o.contents;
        printf("off=%-5zu blit lê BGRA=%02x%02x%02x%02x   shader desenha BGRA=%02x%02x%02x%02x  %s\n", off, bl[0], bl[1], bl[2], bl[3], p[0], p[1], p[2], p[3], (p[0] > 200 && p[2] < 50) ? "OK" : "ERRADO");
    }
} return 0; }
