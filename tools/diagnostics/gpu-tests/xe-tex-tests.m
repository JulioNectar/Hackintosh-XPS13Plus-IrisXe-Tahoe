// xe-tex-tests: isola qual operação de textura quebra a conversão de vídeo (gravação de tela verde, Safari -6680).
//   xe-tex-tests <teste>   roda UM teste (o script xe-tex-tests.sh roda cada um num processo separado, com tempo limite)
// Cada teste usa um compute kernel mínimo e confere o resultado na CPU. Saída: "OK", "ERRADO ..." ou o processo trava (GPU).
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <string.h>

static NSString *SRC = @"#include <metal_stdlib>\nusing namespace metal;\n"
  // leitura: copia a textura (half) para um buffer de uchar4/uchar
  "kernel void rd4(texture2d<half, access::read> t [[texture(0)]], device uchar4 *o [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {"
  "  if (g.x < t.get_width() && g.y < t.get_height()) o[g.y * t.get_width() + g.x] = uchar4(t.read(g) * 255.0h + 0.5h); }\n"
  "kernel void rds(texture2d<half> t [[texture(0)]], sampler s [[sampler(0)]], device uchar4 *o [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {"
  "  if (g.x < t.get_width() && g.y < t.get_height()) o[g.y * t.get_width() + g.x] = uchar4(t.sample(s, (float2(g) + 0.5) / float2(t.get_width(), t.get_height())) * 255.0h + 0.5h); }\n"
  // escrita float (half4) e escrita de inteiros (uint4), como nos kernels do CoreVideo
  "kernel void wrf(texture2d<half, access::write> t [[texture(0)]], uint2 g [[thread_position_in_grid]]) {"
  "  if (g.x < t.get_width() && g.y < t.get_height()) t.write(half4(0x50, 0x60, 0x70, 0x80) / 255.0h, g); }\n"
  "kernel void wru(texture2d<uint, access::write> t [[texture(0)]], uint2 g [[thread_position_in_grid]]) {"
  "  if (g.x < t.get_width() && g.y < t.get_height()) t.write(uint4(0x80706050u), g); }\n"
  "kernel void wru4(texture2d<uint, access::write> t [[texture(0)]], uint2 g [[thread_position_in_grid]]) {"
  "  if (g.x < t.get_width() && g.y < t.get_height()) t.write(uint4(0x50, 0x60, 0x70, 0x80), g); }\n"
  // controle: escrita de uint num buffer
  "kernel void bufu(device uint *o [[buffer(0)]], uint g [[thread_position_in_grid]]) { o[g] = 0x80706050u; }\n";

static id<MTLDevice> dev; static id<MTLLibrary> lib; static id<MTLCommandQueue> q;

static int g_manual = 0;   // XE_MANUAL=1: superfície criada à mão com IOSurfaceCreate em vez do CVPixelBuffer
static IOSurfaceRef nv12_manual(int w, int h) {
  size_t bpr = (w + 63) & ~63, off0 = getenv("XE_OFF0") ? atoi(getenv("XE_OFF0")) : 0;
  size_t off1 = getenv("XE_OFF1") ? atoi(getenv("XE_OFF1")) : (((off0 + bpr * h) + 4095) & ~4095);
  NSDictionary *p0 = @{(id)kIOSurfacePlaneWidth:@(w), (id)kIOSurfacePlaneHeight:@(h), (id)kIOSurfacePlaneBytesPerElement:@1,
                       (id)kIOSurfacePlaneBytesPerRow:@(bpr), (id)kIOSurfacePlaneOffset:@(off0), (id)kIOSurfacePlaneSize:@(bpr*h)};
  NSDictionary *p1 = @{(id)kIOSurfacePlaneWidth:@(w/2), (id)kIOSurfacePlaneHeight:@(h/2), (id)kIOSurfacePlaneBytesPerElement:@2,
                       (id)kIOSurfacePlaneBytesPerRow:@(bpr), (id)kIOSurfacePlaneOffset:@(off1), (id)kIOSurfacePlaneSize:@(bpr*h/2)};
  return IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@(w), (id)kIOSurfaceHeight:@(h),
    (id)kIOSurfacePixelFormat:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange), (id)kIOSurfacePlaneInfo:@[p0, p1], (id)kIOSurfaceAllocSize:@(off1 + bpr*h/2)});
}
static void dump_surf(IOSurfaceRef s) {
  CFDictionaryRef d = IOSurfaceCopyAllValues(s);
  printf("[alloc=%zu planos=%zu off1=%ld bpr0=%zu bpr1=%zu] ", IOSurfaceGetAllocSize(s), IOSurfaceGetPlaneCount(s),
    (long)((uint8_t*)IOSurfaceGetBaseAddressOfPlane(s,1) - (uint8_t*)IOSurfaceGetBaseAddressOfPlane(s,0)), IOSurfaceGetBytesPerRowOfPlane(s,0), IOSurfaceGetBytesPerRowOfPlane(s,1));
  if (getenv("XE_PROPS")) NSLog(@"%@", (__bridge NSDictionary *)d);
  if (d) CFRelease(d);
}
static IOSurfaceRef nv12(int w, int h) {   // 420v com padrão conhecido: Y = (x+y)&255, UV = (x&255, y&255)
  IOSurfaceRef s;
  if (g_manual) s = nv12_manual(w, h);
  else { CVPixelBufferRef pb; NSDictionary *a = @{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
    CVPixelBufferCreate(NULL, w, h, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, (__bridge CFDictionaryRef)a, &pb);
    s = CVPixelBufferGetIOSurface(pb); CFRetain(s); CVPixelBufferRelease(pb); }
  dump_surf(s);
  IOSurfaceLock(s, 0, NULL);
  uint8_t *y = IOSurfaceGetBaseAddressOfPlane(s, 0); size_t by = IOSurfaceGetBytesPerRowOfPlane(s, 0);
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) y[j*by+i] = (uint8_t)(i + j);
  uint8_t *uv = IOSurfaceGetBaseAddressOfPlane(s, 1); size_t bu = IOSurfaceGetBytesPerRowOfPlane(s, 1);
  for (int j = 0; j < h/2; j++) for (int i = 0; i < w/2; i++) { uv[j*bu+2*i] = (uint8_t)i; uv[j*bu+2*i+1] = (uint8_t)j; }
  IOSurfaceUnlock(s, 0, NULL); return s;
}
static IOSurfaceRef bgra(int w, int h) {
  return IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@(w), (id)kIOSurfaceHeight:@(h), (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfacePixelFormat:@('BGRA')});
}
static id<MTLTexture> tex(MTLPixelFormat f, int w, int h, MTLTextureUsage u, IOSurfaceRef s, int plane) {
  MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
  d.usage = u; d.storageMode = s ? MTLStorageModeManaged : MTLStorageModeShared;
  return s ? [dev newTextureWithDescriptor:d iosurface:s plane:plane] : [dev newTextureWithDescriptor:d];
}
static int roda(const char *k, id<MTLTexture> t, id<MTLBuffer> b, int w, int h) {
  NSError *e; id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@(k)] error:&e];
  if (!ps) { printf("ERRO pipeline %s\n", e.description.UTF8String); return 0; }
  id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
  [ce setComputePipelineState:ps]; if (t) [ce setTexture:t atIndex:0]; if (b) [ce setBuffer:b offset:0 atIndex:0];
  if (!strcmp(k, "rds")) { MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; [ce setSamplerState:[dev newSamplerStateWithDescriptor:sd] atIndex:0]; }
  if (h) [ce dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
  else [ce dispatchThreads:MTLSizeMake(w, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
  [ce endEncoding];
  if (t && !t.iosurface && t.storageMode == MTLStorageModeManaged) { id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; [be synchronizeResource:t]; [be endEncoding]; }
  [cb commit]; [cb waitUntilCompleted];
  if (cb.status != MTLCommandBufferStatusCompleted) { printf("ERRO status=%ld %s\n", (long)cb.status, cb.error.description.UTF8String); return 0; }
  return 1;
}
// confere um buffer de uchar4 contra o esperado
static void confere4(id<MTLBuffer> b, int w, int h, uint32_t (*esp)(int, int)) {
  uint32_t *o = b.contents; long ok = 0; int ex = -1, ey = -1; uint32_t got = 0, want = 0;
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { uint32_t e = esp(i, j); if (o[j*w+i] == e) ok++; else if (ex < 0) { ex = i; ey = j; got = o[j*w+i]; want = e; } }
  if (ok == (long)w*h) printf("OK\n");
  else printf("ERRADO %.1f%% certos; 1º erro (%d,%d) leu %08x esperado %08x\n", 100.0*ok/((long)w*h), ex, ey, got, want);
}
static void confereSurf(IOSurfaceRef s, int plane, int bytesW, int h, uint8_t (*esp)(int, int)) {
  IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
  uint8_t *p = IOSurfaceGetBaseAddressOfPlane(s, plane); size_t bpr = IOSurfaceGetBytesPerRowOfPlane(s, plane);
  if (!p) { p = IOSurfaceGetBaseAddress(s); bpr = IOSurfaceGetBytesPerRow(s); }
  long ok = 0; int ex = -1, ey = -1; uint8_t got = 0, want = 0;
  for (int j = 0; j < h; j++) for (int i = 0; i < bytesW; i++) { uint8_t e = esp(i, j); if (p[j*bpr+i] == e) ok++; else if (ex < 0) { ex = i; ey = j; got = p[j*bpr+i]; want = e; } }
  IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
  if (ok == (long)bytesW*h) printf("OK\n");
  else printf("ERRADO %.1f%% certos; 1º erro (%d,%d) leu %02x esperado %02x\n", 100.0*ok/((long)bytesW*h), ex, ey, got, want);
}
static int W = 1920, H = 1080;
static uint32_t eY4(int i, int j) { uint32_t r = 0; for (int k = 0; k < 4; k++) r |= (uint32_t)(uint8_t)(4*i + k + j) << (8*k); return r; }   // plano Y lido como RGBA8 (4 pixels/texel)
static uint32_t eY1(int i, int j) { return (uint8_t)(i + j) | 0xff000000u; }                                                                // plano Y lido como R8 -> (y,0,0,1)
static uint32_t eUV2(int i, int j) { return (uint8_t)i | ((uint32_t)(uint8_t)j << 8) | 0xff000000u; }                                       // plano UV lido como RG8
static uint32_t eUV4(int i, int j) { return (uint8_t)(2*i) | ((uint32_t)(uint8_t)j << 8) | ((uint32_t)(uint8_t)(2*i+1) << 16) | ((uint32_t)(uint8_t)j << 24); }  // UV como RGBA8
static uint8_t pad(int i, int j) { return (uint8_t)(0x50 + 0x10 * (i & 3)); }

int main(int argc, char **argv) { @autoreleasepool {
  if (argc < 2) { printf("testes: bufu r8 rgba8 uv_rg8 uv_rgba8 amostra wf_plano wu_tex wu4_tex wu_plano wu_ios\n"); return 1; }
  dev = MTLCreateSystemDefaultDevice(); q = [dev newCommandQueue]; NSError *e;
  lib = [dev newLibraryWithSource:SRC options:nil error:&e]; if (!lib) { printf("ERRO lib %s\n", e.description.UTF8String); return 1; }
  const char *t = argv[1]; setvbuf(stdout, NULL, _IONBF, 0); g_manual = getenv("XE_MANUAL") != NULL;
  if (!strcmp(t, "bufu")) {                                    // controle: uint em buffer
    id<MTLBuffer> b = [dev newBufferWithLength:4096*4 options:MTLResourceStorageModeShared];
    if (roda("bufu", nil, b, 4096, 0)) { uint32_t *o = b.contents; long ok = 0; for (int i = 0; i < 4096; i++) ok += o[i] == 0x80706050u; printf(ok == 4096 ? "OK\n" : "ERRADO %ld/4096\n", ok); }
  } else if (!strcmp(t, "r8") || !strcmp(t, "rgba8") || !strcmp(t, "uv_rg8") || !strcmp(t, "uv_rgba8") || !strcmp(t, "amostra")) {
    IOSurfaceRef s = nv12(W, H); int uv = !strncmp(t, "uv", 2), quatro = !strcmp(t, "rgba8") || !strcmp(t, "uv_rgba8");
    int w = uv ? (quatro ? W/4 : W/2) : (quatro ? W/4 : W), h = uv ? H/2 : H;
    MTLPixelFormat f = quatro ? MTLPixelFormatRGBA8Unorm : uv ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm;
    if (!strcmp(t, "amostra")) { f = MTLPixelFormatRGBA8Unorm; w = W/4; }
    id<MTLTexture> x = tex(f, w, h, MTLTextureUsageShaderRead, s, uv);
    id<MTLBuffer> b = [dev newBufferWithLength:(size_t)w*h*4 options:MTLResourceStorageModeShared];
    if (roda(strcmp(t, "amostra") ? "rd4" : "rds", x, b, w, h))
      confere4(b, w, h, !strcmp(t, "r8") ? eY1 : !strcmp(t, "uv_rg8") ? eUV2 : !strcmp(t, "uv_rgba8") ? eUV4 : eY4);
  } else if (!strcmp(t, "wf_plano") || !strcmp(t, "wu_plano")) {   // escrita no plano Y de um 420v, visto como RGBA8 / R32Uint
    IOSurfaceRef s = nv12(W, H); int u = t[1] == 'u';
    id<MTLTexture> x = tex(u ? MTLPixelFormatR32Uint : MTLPixelFormatRGBA8Unorm, W/4, H, MTLTextureUsageShaderWrite, s, 0);
    if (roda(u ? "wru" : "wrf", x, nil, W/4, H)) confereSurf(s, 0, W, H, pad);
  } else if (!strcmp(t, "wu_tex") || !strcmp(t, "wu4_tex")) {       // escrita de inteiros numa textura comum (sem IOSurface)
    int quatro = !strcmp(t, "wu4_tex");
    id<MTLTexture> x = tex(quatro ? MTLPixelFormatRGBA8Uint : MTLPixelFormatR32Uint, 480, 1080, MTLTextureUsageShaderWrite, NULL, 0);
    if (roda(quatro ? "wru4" : "wru", x, nil, 480, 1080)) {
      uint32_t *o = malloc(480*1080*4); [x getBytes:o bytesPerRow:480*4 fromRegion:MTLRegionMake2D(0, 0, 480, 1080) mipmapLevel:0];
      long ok = 0; for (int i = 0; i < 480*1080; i++) ok += o[i] == 0x80706050u; printf(ok == 480*1080 ? "OK\n" : "ERRADO %.1f%% (px0=%08x)\n", 100.0*ok/(480*1080), o[0]); free(o);
    }
  } else if (!strcmp(t, "wu_ios")) {                                // o caso que travou a GPU em 20/10 21:05
    IOSurfaceRef s = bgra(480, 1080);
    id<MTLTexture> x = tex(MTLPixelFormatR32Uint, 480, 1080, MTLTextureUsageShaderWrite, s, 0);
    if (roda("wru", x, nil, 480, 1080)) confereSurf(s, 0, 480*4, 1080, pad);
  } else { printf("teste desconhecido\n"); return 1; }
  return 0;
}}
