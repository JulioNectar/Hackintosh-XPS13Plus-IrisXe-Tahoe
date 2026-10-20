// xe-fbfetch: testa framebuffer fetch (ler a cor do destino no fragment shader, [[color(0)]]).
// Fundo branco; o shader devolve mix(destino, azul, 0.5) -> esperado ~(128,128,255). Sem fetch funcionando, sai errado.
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-fbfetch ~/xe/tools/xe-fbfetch.m -framework Metal -framework Foundation
#import <Metal/Metal.h>
#include <stdio.h>
static const char *SRC =
"#include <metal_stdlib>\nusing namespace metal;\n"
"vertex float4 vs(uint i [[vertex_id]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; return float4(p[i], 0, 1); }\n"
"fragment half4 fs_fetch(half4 dst [[color(0)]]) { return mix(dst, half4(0,0,1,1), half(0.5)); }\n"
"fragment half4 fs_plain() { return half4(0,0,1,1); }\n";
int main() { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(SRC) options:nil error:&e]; if (!lib) { printf("lib: %s\n", e.description.UTF8String); return 1; }
    const char *fns[] = {"fs_plain", "fs_fetch"};
    MTLPixelFormat pfs[] = {MTLPixelFormatBGRA8Unorm, MTLPixelFormatRGBA16Float};
    for (int k = 0; k < 2; k++) for (int f = 0; f < 2; f++) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@(fns[f])];
        pd.colorAttachments[0].pixelFormat = pfs[k];
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!ps) { printf("%s pipeline falhou: %s\n", fns[f], e.localizedDescription.UTF8String); continue; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pfs[k] width:16 height:16 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeManaged;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 1, 1, 1);
        id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer]; id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re setRenderPipelineState:ps]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; [be synchronizeResource:t]; [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (k == 0) { uint8_t p[4]; [t getBytes:p bytesPerRow:64 fromRegion:MTLRegionMake2D(8, 8, 1, 1) mipmapLevel:0]; printf("BGRA8   %-9s -> RGB=(%3d,%3d,%3d)\n", fns[f], p[2], p[1], p[0]); }
        else { __fp16 p[4]; [t getBytes:p bytesPerRow:128 fromRegion:MTLRegionMake2D(8, 8, 1, 1) mipmapLevel:0]; printf("RGBA16F %-9s -> RGB=(%.2f,%.2f,%.2f)\n", fns[f], (float)p[0], (float)p[1], (float)p[2]); }
    }
    printf("esperado: plain=(0,0,255); fetch=(128,128,255) ou (0.5,0.5,1.0)\n");
} return 0; }
