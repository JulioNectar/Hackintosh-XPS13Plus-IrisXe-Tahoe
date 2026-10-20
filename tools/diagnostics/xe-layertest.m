// xe-layertest: janela com CALayers cujo contents é uma IOSurface de formatos diferentes (azul puro); captura a
// janela (como o WindowServer compôs) e mede a cor de cada quadrado. Vazio/preto = formato não composto.
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-layertest ~/xe/tools/xe-layertest.m -framework AppKit -framework QuartzCore -framework IOSurface
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>
typedef struct { OSType f; int bpe; const char *n; } F;
static uint16_t h(float v) { // float -> half (só 0..1 positivo)
    if (v <= 0) return 0; union { float f; uint32_t u; } x = {v}; int e = ((x.u >> 23) & 0xff) - 127 + 15; return (uint16_t)((e << 10) | ((x.u >> 13) & 0x3ff));
}
static IOSurfaceRef mk(F f, int S) {
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@(S), (id)kIOSurfaceHeight:@(S),
        (id)kIOSurfaceBytesPerElement:@(f.bpe), (id)kIOSurfacePixelFormat:@(f.f)});
    IOSurfaceLock(s, 0, NULL); uint8_t *b = IOSurfaceGetBaseAddress(s); size_t bpr = IOSurfaceGetBytesPerRow(s);
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        uint8_t *p = b + y * bpr + x * f.bpe;
        if (f.f == 'BGRA') { p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 255; }
        else if (f.f == 'RGBA') { p[0] = 0; p[1] = 0; p[2] = 255; p[3] = 255; }
        else if (f.f == 'RGhA') { uint16_t *q = (uint16_t *)p; q[0] = 0; q[1] = 0; q[2] = h(1); q[3] = h(1); }
        else if (f.f == 'RGfA') { float *q = (float *)p; q[0] = 0; q[1] = 0; q[2] = 1; q[3] = 1; }
        else if (f.f == 'l10r' || f.f == 'w30r') { *(uint32_t *)p = (3u << 30) | 0x3ff; }   // ARGB2101010 LE: B nos bits baixos
        else if (f.f == 'b64a') { uint16_t *q = (uint16_t *)p; q[0] = 0xffff; q[1] = 0; q[2] = 0; q[3] = 0xffff; } // ARGB16 BE
    }
    IOSurfaceUnlock(s, 0, NULL); return s;
}
int main() { @autoreleasepool {
    [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    F fs[] = {{'BGRA',4,"BGRA 8-bit"},{'RGBA',4,"RGBA 8-bit"},{'RGhA',8,"RGhA half 16"},{'RGfA',16,"RGfA float 32"},{'l10r',4,"l10r 10-bit"},{'w30r',4,"w30r 10-bit XR"}};
    int n = 6, S = 70, G = 85;
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(10, 300, 85 * (n + 8) + 20, G + 30) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    w.title = @"xe-layertest"; NSView *v = w.contentView; v.wantsLayer = YES; v.layer.backgroundColor = CGColorCreateGenericRGB(1, 1, 1, 1);
    for (int i = 0; i < n; i++) {
        CALayer *l = [CALayer layer]; l.frame = CGRectMake(10 + i * G, 10, S, S); l.contents = (__bridge id)mk(fs[i], S);
        [v.layer addSublayer:l];
        NSTextField *t = [NSTextField labelWithString:[NSString stringWithFormat:@"%d: %s", i + 1, fs[i].n]];
        t.frame = NSMakeRect(5 + i * G, G + 5, G, 18); t.font = [NSFont systemFontOfSize:10]; [v addSubview:t];
    }
    // 7: CGImage half-float Extended sRGB (como os ícones do Tahoe); 8: o mesmo em sRGB 8-bit; 9: ícone real
    NSArray *extras = @[@"7: CGImage XsRGB f16", @"8: CGImage sRGB 8", @"9: ícone (NSImage)", @"10: CGImage do ícone", @"11: cópia do ícone", @"12: tela 8-bit", @"13: tela f16 estendido", @"14: ícone p/ cor da tela"];
    CGImageRef iconCG = NULL;
    {
        NSImage *ic = [[NSWorkspace sharedWorkspace] iconForFile:@"/System/Applications/Calculator.app"];
        id icon = [ic.representations.lastObject valueForKey:@"_icon"];
        id desc = ((id(*)(id,SEL,CGSize,CGFloat))objc_msgSend)([objc_getClass("ISImageDescriptor") alloc], @selector(initWithSize:scale:), CGSizeMake(80,80), 2.0);
        id img = ((id(*)(id,SEL,id))objc_msgSend)(icon, @selector(imageForDescriptor:), desc);
        iconCG = (__bridge CGImageRef)((id(*)(id,SEL))objc_msgSend)(img, @selector(CGImage)); CGImageRetain(iconCG);
    }
    for (int k = 0; k < 8; k++) {
        CALayer *l = [CALayer layer]; l.frame = CGRectMake(10 + (n + k) * G, 10, S, S);
        CGColorSpaceRef tela = [NSScreen mainScreen].colorSpace.CGColorSpace;
        if (k == 5 || k == 6) {
            CGColorSpaceRef cs = k == 5 ? tela : CGColorSpaceCreateExtended(tela);
            CGContextRef c = k == 5 ? CGBitmapContextCreate(NULL, S, S, 8, S * 4, cs, kCGImageAlphaPremultipliedLast)
                                    : CGBitmapContextCreate(NULL, S, S, 16, S * 8, cs, kCGImageAlphaPremultipliedLast | kCGBitmapFloatComponents | kCGBitmapByteOrder16Little);
            CGContextSetFillColorWithColor(c, CGColorCreateGenericRGB(0, 0, 1, 1)); CGContextFillRect(c, CGRectMake(0, 0, S, S));
            l.contents = (__bridge id)CGBitmapContextCreateImage(c);
        } else if (k == 7) {
            NSImage *ic = [[NSWorkspace sharedWorkspace] iconForFile:@"/System/Applications/Calculator.app"]; ic.size = NSMakeSize(S, S);
            NSRect rc = NSMakeRect(0, 0, S, S);
            CGImageRef cg = [ic CGImageForProposedRect:&rc context:nil hints:@{NSImageHintCTM: [NSAffineTransform transform]}];
            CGColorSpaceRef cs = CGColorSpaceCreateExtended(tela);
            CGContextRef c = CGBitmapContextCreate(NULL, CGImageGetWidth(cg), CGImageGetHeight(cg), 16, CGImageGetWidth(cg) * 8, cs, kCGImageAlphaPremultipliedLast | kCGBitmapFloatComponents | kCGBitmapByteOrder16Little);
            CGContextDrawImage(c, CGRectMake(0, 0, CGImageGetWidth(cg), CGImageGetHeight(cg)), cg);
            l.contents = (__bridge id)CGBitmapContextCreateImage(c);
        } else
        if (k == 3) { l.contents = (__bridge id)iconCG; }
        else if (k == 4) {
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedSRGB);
            CGContextRef c = CGBitmapContextCreate(NULL, CGImageGetWidth(iconCG), CGImageGetHeight(iconCG), 16, CGImageGetWidth(iconCG) * 8, cs, kCGImageAlphaPremultipliedLast | kCGBitmapFloatComponents | kCGBitmapByteOrder16Little);
            CGContextDrawImage(c, CGRectMake(0, 0, CGImageGetWidth(iconCG), CGImageGetHeight(iconCG)), iconCG);
            l.contents = (__bridge id)CGBitmapContextCreateImage(c);
        }
        else if (k < 2) {
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(k == 0 ? kCGColorSpaceExtendedSRGB : kCGColorSpaceSRGB);
            CGContextRef c = k == 0 ? CGBitmapContextCreate(NULL, S, S, 16, S * 8, cs, kCGImageAlphaPremultipliedLast | kCGBitmapFloatComponents | kCGBitmapByteOrder16Little)
                                    : CGBitmapContextCreate(NULL, S, S, 8, S * 4, cs, kCGImageAlphaPremultipliedLast);
            CGContextSetRGBFillColor(c, 0, 0, 1, 1); CGContextFillRect(c, CGRectMake(0, 0, S, S));
            CGImageRef im = CGBitmapContextCreateImage(c); l.contents = (__bridge id)im;
        } else {
            NSImage *ic = [[NSWorkspace sharedWorkspace] iconForFile:@"/System/Applications/Calculator.app"]; ic.size = NSMakeSize(S, S);
            l.contents = [ic layerContentsForContentsScale:2.0]; l.contentsScale = 2.0;
        }
        [v.layer addSublayer:l];
        NSTextField *t = [NSTextField labelWithString:extras[k]];
        t.frame = NSMakeRect(5 + (n + k) * G, G + 5, G, 18); t.font = [NSFont systemFontOfSize:10]; [v addSubview:t];
    }
    [w makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    if (getenv("VER")) { [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:atoi(getenv("VER"))]]; return 0; }
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:2.0]];
    char cmd[256]; snprintf(cmd, sizeof cmd, "/usr/sbin/screencapture -x -o -l %ld /tmp/xe-layertest-cap.png", (long)w.windowNumber);
    if (system(cmd) != 0) { printf("screencapture falhou\n"); return 1; }
    NSBitmapImageRep *r = (NSBitmapImageRep *)[NSBitmapImageRep imageRepWithContentsOfFile:@"/tmp/xe-layertest-cap.png"];
    if (!r) { printf("captura vazia (permissão de Gravação de Tela?)\n"); return 1; }
    NSRect cr = [w contentRectForFrameRect:w.frame]; double titleH = w.frame.size.height - cr.size.height;
    (void)titleH;
    double sc = r.pixelsWide / (double)(G * n + 20);
    for (int i = 0; i < n; i++) {
        NSColor *c = [[r colorAtX:(NSInteger)((10 + i * G + S / 2) * sc) y:(NSInteger)(r.pixelsHigh - (10 + S / 2) * sc)] colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        printf("%-16s cor composta = (%3.0f,%3.0f,%3.0f)  %s\n", fs[i].n, c.redComponent * 255, c.greenComponent * 255, c.blueComponent * 255,
               c.blueComponent > 0.6 && c.redComponent < 0.4 ? "OK azul" : "ERRADO");
    }
    [[r representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@"/tmp/xe-layertest.png" atomically:YES];
} return 0; }
