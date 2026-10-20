// xe-carender: compõe camadas com o CARenderer (mesmo motor do WindowServer) numa textura Metal BGRA8 e mede a cor
// de cada camada. Reproduz fora do WindowServer o defeito dos ícones (conteúdo f16 estendido no espaço de cor do destino).
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-carender ~/xe/tools/xe-carender.m -framework AppKit -framework QuartzCore -framework Metal
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <dlfcn.h>
static CGImageRef azul(CGColorSpaceRef cs, int f16, int S) {
    CGContextRef c = f16 ? CGBitmapContextCreate(NULL, S, S, 16, S * 8, cs, kCGImageAlphaPremultipliedLast | kCGBitmapFloatComponents | kCGBitmapByteOrder16Little)
                         : CGBitmapContextCreate(NULL, S, S, 8, S * 4, cs, kCGImageAlphaPremultipliedLast);
    CGContextSetFillColorWithColor(c, CGColorCreateGenericRGB(0, 0, 1, 1)); CGContextFillRect(c, CGRectMake(0, 0, S, S));
    return CGBitmapContextCreateImage(c);
}
int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (getenv("ESPIAO")) { char buf[2048]; strlcpy(buf, getenv("ESPIAO"), sizeof buf); for (char *t = strtok(buf, ":"); t; t = strtok(NULL, ":")) { void *h = dlopen(t, RTLD_NOW); if (!h) printf("dlopen: %s\n", dlerror()); } }
    int W = 800, H = 120, S = 80, G = 100;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeManaged;
    id<MTLTexture> alvo = [dev newTextureWithDescriptor:d];
    CGColorSpaceRef tela = [NSScreen mainScreen].colorSpace.CGColorSpace;
    CGColorSpaceRef destino = argc > 1 && !strcmp(argv[1], "srgb") ? CGColorSpaceCreateWithName(kCGColorSpaceSRGB) : tela;
    CARenderer *r = [CARenderer rendererWithMTLTexture:alvo options:@{kCARendererColorSpace: (__bridge id)destino}];
    CALayer *raiz = [CALayer layer]; raiz.frame = CGRectMake(0, 0, W, H); raiz.backgroundColor = CGColorCreateGenericRGB(1, 1, 1, 1);
    const char *nomes[] = {"sRGB 8", "XsRGB f16", "tela 8", "tela-ext f16", "ícone NSImage", "ícone CGImage"};
    for (int i = 0; i < 6; i++) {
        CALayer *l = [CALayer layer]; l.frame = CGRectMake(10 + i * G, 20, S, S);
        if (i == 0) l.contents = (__bridge id)azul(CGColorSpaceCreateWithName(kCGColorSpaceSRGB), 0, S);
        if (i == 1) l.contents = (__bridge id)azul(CGColorSpaceCreateWithName(kCGColorSpaceExtendedSRGB), 1, S);
        if (i == 2) l.contents = (__bridge id)azul(tela, 0, S);
        if (i == 3) l.contents = (__bridge id)azul(CGColorSpaceCreateExtended(tela), 1, S);
        if (i >= 4) {
            NSImage *ic = [[NSWorkspace sharedWorkspace] iconForFile:@"/System/Applications/Calculator.app"]; ic.size = NSMakeSize(S, S);
            if (i == 4) l.contents = [ic layerContentsForContentsScale:1.0];
            else { NSRect rc = NSMakeRect(0, 0, S, S); l.contents = (__bridge id)[ic CGImageForProposedRect:&rc context:nil hints:nil]; }
        }
        [raiz addSublayer:l];
    }
    r.layer = raiz; r.bounds = raiz.frame;
    [CATransaction flush];
    [r beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL]; [r addUpdateRect:r.bounds]; [r render]; [r endFrame];
    id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e synchronizeResource:alvo]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    uint8_t *px = malloc(W * H * 4); [alvo getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    for (int i = 0; i < 6; i++) {
        int cx = 10 + i * G + S / 2, cy = H - (20 + S / 2), lin = 0;
        uint8_t *p = px + (cy * W + cx) * 4;
        for (int y = 20; y < 20 + S; y++) { uint8_t *q = px + ((H - 1 - y) * W + cx) * 4; if (q[0] > 200 && q[1] > 200 && q[2] > 200) lin++; }
        printf("%-16s centro BGRA=(%3d,%3d,%3d,%3d)  linhas brancas no meio da camada=%d\n", nomes[i], p[0], p[1], p[2], p[3], lin);
    }
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:W pixelsHigh:H bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:W * 4 bitsPerPixel:32];
    for (int k = 0; k < W * H; k++) { rep.bitmapData[k*4] = px[k*4+2]; rep.bitmapData[k*4+1] = px[k*4+1]; rep.bitmapData[k*4+2] = px[k*4]; rep.bitmapData[k*4+3] = 255; }
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@"/tmp/xe-carender.png" atomically:YES];
} return 0; }
