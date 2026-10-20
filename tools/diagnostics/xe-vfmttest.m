// xe-vfmttest: desenha um quadrado azul com a posição vinda de um atributo de vértice em vários formatos
// (float2, half2, ushort2 normalizado, UInt1010102Normalized...) e mede se o quadrado aparece.
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-vfmttest ~/xe/tools/xe-vfmttest.m -framework Metal -framework Foundation
#import <Metal/Metal.h>
#include <stdio.h>
static const char *SRC =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct VI { float4 p [[attribute(0)]]; };\n"
"vertex float4 vs(VI i [[stage_in]]) { return float4(i.p.xy * 2.0 - 1.0, 0, 1); }\n"
"fragment half4 fs() { return half4(0, 0, 1, 1); }\n";
static uint32_t p1010102(float x, float y) { return (uint32_t)(x * 1023) | ((uint32_t)(y * 1023) << 10) | (1023u << 20) | (3u << 30); }
int main() { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(SRC) options:nil error:&e]; if (!lib) { printf("%s\n", e.description.UTF8String); return 1; }
    struct { MTLVertexFormat vf; int stride; const char *n; } fs[] = {
        {MTLVertexFormatFloat2, 8, "float2"}, {MTLVertexFormatHalf2, 4, "half2"}, {MTLVertexFormatUShort2Normalized, 4, "ushort2norm"},
        {MTLVertexFormatUChar4Normalized, 4, "uchar4norm"}, {MTLVertexFormatUInt1010102Normalized, 4, "UInt1010102Norm"},
        {MTLVertexFormatInt1010102Normalized, 4, "Int1010102Norm"}};
    float q[6][2] = {{0.25,0.25},{0.75,0.25},{0.25,0.75},{0.75,0.25},{0.75,0.75},{0.25,0.75}};
    int W = 64;
    for (int k = 0; k < 6; k++) {
        uint8_t vb[6 * 8] = {0};
        for (int v = 0; v < 6; v++) {
            float x = q[v][0], y = q[v][1];
            if (k == 0) { float *f = (float *)(vb + v * 8); f[0] = x; f[1] = y; }
            if (k == 1) { __fp16 *h = (__fp16 *)(vb + v * 4); h[0] = x; h[1] = y; }
            if (k == 2) { uint16_t *u = (uint16_t *)(vb + v * 4); u[0] = x * 65535; u[1] = y * 65535; }
            if (k == 3) { uint8_t *u = vb + v * 4; u[0] = x * 255; u[1] = y * 255; u[2] = 0; u[3] = 255; }
            if (k == 4) { *(uint32_t *)(vb + v * 4) = p1010102(x, y); }
            if (k == 5) { *(uint32_t *)(vb + v * 4) = (uint32_t)(x * 511) | ((uint32_t)(y * 511) << 10) | (511u << 20) | (1u << 30); }
        }
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = fs[k].vf; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0; vd.layouts[0].stride = fs[k].stride;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.vertexDescriptor = vd;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!ps) { printf("%-16s pipeline falhou: %s\n", fs[k].n, e.localizedDescription.UTF8String); continue; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:W mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeManaged;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 1, 1, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer];
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re setRenderPipelineState:ps]; [re setVertexBytes:vb length:sizeof vb atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6]; [re endEncoding];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; [be synchronizeResource:t]; [be endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        uint8_t px[4]; [t getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(W / 2, W / 2, 1, 1) mipmapLevel:0];
        int azuis = 0; uint8_t *all = malloc(W * W * 4); [t getBytes:all bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, W) mipmapLevel:0];
        for (int i = 0; i < W * W; i++) if (all[i*4] > 200 && all[i*4+2] < 50) azuis++;
        printf("%-16s centro BGRA=(%3d,%3d,%3d) pixels azuis=%4d (esperado ~%d) %s\n", fs[k].n, px[0], px[1], px[2], azuis, W * W / 4, azuis > W * W / 8 ? "OK" : "ERRADO");
        free(all);
    }
} return 0; }
