// xe-nocopyfix: dylib de teste — newTiledTextureWithBytesNoCopy com offset não alinhado (ex. 0x20, cabeçalho das imagens
// do CoreAnimation) vira textura própria com CÓPIA dos pixels (a GPU TGL/RPL não lê textura linear nesse offset).
// O deallocator do chamador é chamado quando a nossa textura for liberada (via objeto associado).
// Compilar: clang -fno-objc-arc -dynamiclib -o ~/xe/build/xe-nocopyfix.dylib ~/xe/tools/xe-nocopyfix.m -framework Metal -framework Foundation
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdio.h>
@interface XeDealloc : NSObject { void (^_b)(void *, NSUInteger); void *_p; NSUInteger _n; } - (id)initWithBlock:(id)b ptr:(void *)p len:(NSUInteger)n; @end
@implementation XeDealloc
- (id)initWithBlock:(id)b ptr:(void *)p len:(NSUInteger)n { if ((self = [super init])) { _b = [b copy]; _p = p; _n = n; } return self; }
- (void)dealloc { if (_b) { _b(_p, _n); [_b release]; } [super dealloc]; }
@end
static IMP o3, o4; static long g_n;
static const void *kDealloc = &kDealloc;
static id copia(id dev, void *ptr, NSUInteger len, MTLTextureDescriptor *d, NSUInteger off, NSUInteger bpr) {
    MTLTextureDescriptor *c = [[d copy] autorelease]; c.storageMode = MTLStorageModeManaged;
    id<MTLTexture> t = [dev newTextureWithDescriptor:c];
    if (!t) return nil;
    [t replaceRegion:MTLRegionMake2D(0, 0, d.width, d.height) mipmapLevel:0 withBytes:(uint8_t *)ptr + off bytesPerRow:bpr];
    if (++g_n <= 4) fprintf(stderr, "nocopyfix: textura %lux%lu fmt=%lu off=0x%lx -> cópia\n", (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)off);
    return t;
}
static BOOL precisa(MTLTextureDescriptor *d, NSUInteger off) { return (off & 255) != 0 && d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1; }
static id w3(id s, SEL c, void *ptr, NSUInteger len, MTLTextureDescriptor *d, NSUInteger off, NSUInteger bpr) {
    if (precisa(d, off)) { id t = copia(s, ptr, len, d, off, bpr); if (t) return t; }
    return ((id(*)(id,SEL,void*,NSUInteger,id,NSUInteger,NSUInteger))o3)(s,c,ptr,len,d,off,bpr);
}
static id w4(id s, SEL c, void *ptr, NSUInteger len, id dealloc, MTLTextureDescriptor *d, NSUInteger off, NSUInteger bpr) {
    if (precisa(d, off)) { id t = copia(s, ptr, len, d, off, bpr);
        if (t) { if (dealloc) { XeDealloc *x = [[XeDealloc alloc] initWithBlock:dealloc ptr:ptr len:len]; objc_setAssociatedObject(t, kDealloc, x, OBJC_ASSOCIATION_RETAIN); [x release]; } return t; } }
    return ((id(*)(id,SEL,void*,NSUInteger,id,id,NSUInteger,NSUInteger))o4)(s,c,ptr,len,dealloc,d,off,bpr);
}
__attribute__((constructor)) static void ini(void) {
    Class k = objc_getClass("MTLIOAccelDevice");
    Method m3 = class_getInstanceMethod(k, sel_registerName("newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:"));
    Method m4 = class_getInstanceMethod(k, sel_registerName("newTiledTextureWithBytesNoCopy:length:deallocator:descriptor:offset:bytesPerRow:"));
    if (m3) o3 = method_setImplementation(m3, (IMP)w3);
    if (m4) o4 = method_setImplementation(m4, (IMP)w4);
    fprintf(stderr, "nocopyfix: %d %d\n", !!o3, !!o4);
}
