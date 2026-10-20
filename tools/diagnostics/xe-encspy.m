// xe-encspy: dylib de depuração (DYLD_INSERT_LIBRARIES) — registra, por draw, o pipeline (função fragment) e as texturas
// de fragment ligadas (formato, tamanho, se vem de buffer/IOSurface). Saída: stderr.
// Compilar: clang -fno-objc-arc -dynamiclib -o ~/xe/build/xe-encspy.dylib ~/xe/tools/xe-encspy.m -framework Metal -framework Foundation -framework IOSurface
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <IOSurface/IOSurface.h>
#include <stdio.h>
static IMP o_src, o_srccb, o_data, o_fnconst, o_fnspec, o_fnspeccb, o_fndesc, o_fnplib;
static IMP o_t1, o_t2, o_t3, o_t4;
static IMP o_rp, o_setps, o_settex, o_draw, o_drawi, o_buftex, o_dix, o_dixi, o_texs, o_texsamp;
static const void *kNome = &kNome;
static __thread id t_ps; static __thread id t_tex[8];
static id w_rp(id s, SEL c, MTLRenderPipelineDescriptor *d, NSError **e) {
    id r = ((id(*)(id, SEL, id, NSError **))o_rp)(s, c, d, e);
    if (r) objc_setAssociatedObject(r, kNome, [NSString stringWithFormat:@"%@/%@", d.vertexFunction.name, d.fragmentFunction.name], OBJC_ASSOCIATION_RETAIN);
    return r;
}
static void w_setps(id s, SEL c, id ps) { t_ps = ps; ((void(*)(id, SEL, id))o_setps)(s, c, ps); }
static void w_settex(id s, SEL c, id t, NSUInteger i) { if (i < 8) t_tex[i] = t; ((void(*)(id, SEL, id, NSUInteger))o_settex)(s, c, t, i); }
static void dump(const char *k, NSUInteger n) {
    NSString *nm = t_ps ? objc_getAssociatedObject(t_ps, kNome) : nil;
    fprintf(stderr, "DRAW %s n=%lu pipe=%s", k, (unsigned long)n, nm ? nm.UTF8String : "?");
    for (int i = 0; i < 4; i++) if (t_tex[i]) {
        id<MTLTexture> t = t_tex[i]; IOSurfaceRef io = t.iosurface;
        fprintf(stderr, " | t%d fmt=%lu %lux%lu buf=%s ios=%s storage=%lu", i, (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height,
                t.buffer ? "sim" : "nao", io ? "sim" : "nao", (unsigned long)t.storageMode);
        if (t.buffer) fprintf(stderr, " bufobj=%p len=%lu off=0x%lx bpr=%lu gpu=0x%llx", t.buffer, (unsigned long)t.buffer.length, (unsigned long)t.bufferOffset, (unsigned long)t.bufferBytesPerRow, [t.buffer respondsToSelector:@selector(gpuAddress)] ? (unsigned long long)t.buffer.gpuAddress : 0ull);
        if (t.buffer && t.buffer.storageMode != MTLStorageModePrivate) {
            int bpp = t.pixelFormat >= 110 ? 8 : 4;
            uint8_t *b = (uint8_t *)t.buffer.contents + t.bufferOffset + (t.height / 2) * t.bufferBytesPerRow + (t.width / 2) * bpp;
            if (bpp == 4) fprintf(stderr, " centro=%02x%02x%02x%02x", b[0], b[1], b[2], b[3]);
            else { __fp16 *h = (__fp16 *)b; fprintf(stderr, " centro=(%.2f,%.2f,%.2f,%.2f)", (float)h[0], (float)h[1], (float)h[2], (float)h[3]); }
        }
    }
    fprintf(stderr, "\n");
}
static void w_draw(id s, SEL c, NSUInteger p, NSUInteger st, NSUInteger n) { dump("draw", n); ((void(*)(id, SEL, NSUInteger, NSUInteger, NSUInteger))o_draw)(s, c, p, st, n); }
static void w_drawi(id s, SEL c, NSUInteger p, NSUInteger st, NSUInteger n, NSUInteger inst) { dump("drawInst", n); ((void(*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, NSUInteger))o_drawi)(s, c, p, st, n, inst); }
static void w_dix(id s, SEL c, NSUInteger p, NSUInteger n, NSUInteger it, id ib, NSUInteger off) { dump("drawIdx", n); ((void(*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger))o_dix)(s, c, p, n, it, ib, off); }
static void w_dixi(id s, SEL c, NSUInteger p, NSUInteger n, NSUInteger it, id ib, NSUInteger off, NSUInteger inst) { dump("drawIdxInst", n); ((void(*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger))o_dixi)(s, c, p, n, it, ib, off, inst); }
static void w_texs(id s, SEL c, const id *ts, NSRange r) { for (NSUInteger i = 0; i < r.length; i++) if (r.location + i < 8) t_tex[r.location + i] = ts[i]; ((void(*)(id, SEL, const id *, NSRange))o_texs)(s, c, ts, r); }
static void w_texsamp(id s, SEL c, id t, NSUInteger ti, id sm, NSUInteger si) { if (ti < 8) t_tex[ti] = t; ((void(*)(id, SEL, id, NSUInteger, id, NSUInteger))o_texsamp)(s, c, t, ti, sm, si); }
static int g_nsrc;
static void salva_src(NSString *src) {
    char p[128]; snprintf(p, sizeof p, "/tmp/ca-shader-%d.metal", g_nsrc++);
    [src writeToFile:@(p) atomically:YES encoding:NSUTF8StringEncoding error:nil];
    fprintf(stderr, "SRC %s (%lu chars)\n", p, (unsigned long)src.length);
}
static id w_src(id s, SEL c, NSString *src, id opt, NSError **e) { salva_src(src); return ((id(*)(id, SEL, id, id, NSError **))o_src)(s, c, src, opt, e); }
static void w_srccb(id s, SEL c, NSString *src, id opt, id h) { salva_src(src); ((void(*)(id, SEL, id, id, id))o_srccb)(s, c, src, opt, h); }
static id w_data(id s, SEL c, id data, NSError **e) { fprintf(stderr, "LIBDATA %lu bytes\n", (unsigned long)[data length]); return ((id(*)(id, SEL, id, NSError **))o_data)(s, c, data, e); }
static void lognome(const char *via, NSString *n, NSString *sp, id cv) {
    fprintf(stderr, "FUNC %s base=%s spec=%s\n", via, n ? n.UTF8String : "-", sp ? sp.UTF8String : "-");
    if (cv && [cv respondsToSelector:@selector(newIndexedConstantArray)]) {
        id a = [cv performSelector:@selector(newIndexedConstantArray)];
        id b = [cv performSelector:@selector(newNamedConstantArray)];
        fprintf(stderr, "   idx=%s\n   nomes=%s\n", [[a description] UTF8String], [[b description] UTF8String]);
    }
}
static id w_fnspec(id s, SEL c, NSString *n, id cv, id cache, NSString *sp, NSError **e) { lognome("spec", n, sp, cv); return ((id(*)(id, SEL, id, id, id, id, NSError **))o_fnspec)(s, c, n, cv, cache, sp, e); }
static void w_fnspeccb(id s, SEL c, NSString *n, id cv, id cache, NSString *sp, id h) { lognome("spec-cb", n, sp, cv); ((void(*)(id, SEL, id, id, id, id, id))o_fnspeccb)(s, c, n, cv, cache, sp, h); }
static id w_fnconst(id s, SEL c, NSString *n, id cv, NSError **e) { lognome("const", n, nil, cv); return ((id(*)(id, SEL, id, id, NSError **))o_fnconst)(s, c, n, cv, e); }
static id w_fndesc(id s, SEL c, MTLFunctionDescriptor *d, NSError **e) { lognome("desc", d.name, d.specializedName, d.constantValues); return ((id(*)(id, SEL, id, NSError **))o_fndesc)(s, c, d, e); }
static id w_fnplib(id s, SEL c, NSString *n, id cv, id pl, NSError **e) { lognome("plib", n, nil, cv); return ((id(*)(id, SEL, id, id, id, NSError **))o_fnplib)(s, c, n, cv, pl, e); }
static id w_t1(id s, SEL c, MTLTextureDescriptor *d, id b, NSUInteger off, NSUInteger bpr) { fprintf(stderr, "TEXAPI dev:buffer:offset off=0x%lx bpr=%lu\n", (unsigned long)off, (unsigned long)bpr); return ((id(*)(id,SEL,id,id,NSUInteger,NSUInteger))o_t1)(s,c,d,b,off,bpr); }
static id w_t2(id s, SEL c, MTLTextureDescriptor *d, NSUInteger off, NSUInteger bpr, NSUInteger bpi) { fprintf(stderr, "TEXAPI buf:newLinear off=0x%lx bpr=%lu\n", (unsigned long)off, (unsigned long)bpr); return ((id(*)(id,SEL,id,NSUInteger,NSUInteger,NSUInteger))o_t2)(s,c,d,off,bpr,bpi); }
static id w_t3(id s, SEL c, void *ptr, NSUInteger len, MTLTextureDescriptor *d, NSUInteger off, NSUInteger bpr) { fprintf(stderr, "TEXAPI dev:tiledNoCopy ptr=%p off=0x%lx bpr=%lu\n", ptr, (unsigned long)off, (unsigned long)bpr); return ((id(*)(id,SEL,void*,NSUInteger,id,NSUInteger,NSUInteger))o_t3)(s,c,ptr,len,d,off,bpr); }
static id w_t4(id s, SEL c, void *ptr, NSUInteger len, id dealloc, MTLTextureDescriptor *d, NSUInteger off, NSUInteger bpr) { fprintf(stderr, "TEXAPI dev:tiledNoCopyDealloc ptr=%p off=0x%lx bpr=%lu\n", ptr, (unsigned long)off, (unsigned long)bpr); return ((id(*)(id,SEL,void*,NSUInteger,id,id,NSUInteger,NSUInteger))o_t4)(s,c,ptr,len,dealloc,d,off,bpr); }
static id w_buftex(id s, SEL c, MTLTextureDescriptor *d, NSUInteger off, NSUInteger bpr) {
    id r = ((id(*)(id, SEL, id, NSUInteger, NSUInteger))o_buftex)(s, c, d, off, bpr);
    fprintf(stderr, "BUFTEX fmt=%lu %lux%lu off=%lu bpr=%lu -> %s\n", (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)off, (unsigned long)bpr, r ? "ok" : "NIL");
    return r;
}
static IMP tr(const char *cls, const char *sel, IMP n) { Method m = class_getInstanceMethod(objc_getClass(cls), sel_registerName(sel)); return m ? method_setImplementation(m, n) : NULL; }
static void *instalar(void *a) {
    for (int i = 0; i < 4000 && !objc_getClass("MTLIGAccelRenderCommandEncoder"); i++) usleep(1000);
    o_rp = tr("_MTLDevice", "newRenderPipelineStateWithDescriptor:error:", (IMP)w_rp);
    o_setps = tr("MTLIGAccelRenderCommandEncoder", "setRenderPipelineState:", (IMP)w_setps);
    o_settex = tr("MTLIGAccelRenderCommandEncoder", "setFragmentTexture:atIndex:", (IMP)w_settex);
    o_draw = tr("MTLIGAccelRenderCommandEncoder", "drawPrimitives:vertexStart:vertexCount:", (IMP)w_draw);
    o_drawi = tr("MTLIGAccelRenderCommandEncoder", "drawPrimitives:vertexStart:vertexCount:instanceCount:", (IMP)w_drawi);
    o_dix = tr("MTLIGAccelRenderCommandEncoder", "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:", (IMP)w_dix);
    o_dixi = tr("MTLIGAccelRenderCommandEncoder", "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:", (IMP)w_dixi);
    o_texs = tr("MTLIGAccelRenderCommandEncoder", "setFragmentTextures:withRange:", (IMP)w_texs);
    o_texsamp = tr("MTLIOAccelRenderCommandEncoder", "setFragmentTexture:atTextureIndex:samplerState:atSamplerIndex:", (IMP)w_texsamp);
    o_src = tr("_MTLDevice", "newLibraryWithSource:options:error:", (IMP)w_src);
    o_srccb = tr("_MTLDevice", "newLibraryWithSource:options:completionHandler:", (IMP)w_srccb);
    o_data = tr("_MTLDevice", "newLibraryWithData:error:", (IMP)w_data);
    o_fnspec = tr("_MTLLibrary", "newFunctionWithName:constantValues:functionCache:specializedName:error:", (IMP)w_fnspec);
    o_fnspeccb = tr("_MTLLibrary", "newFunctionWithName:constantValues:functionCache:specializedName:completionHandler:", (IMP)w_fnspeccb);
    o_fnconst = tr("_MTLLibrary", "newFunctionWithName:constantValues:error:", (IMP)w_fnconst);
    o_fndesc = tr("_MTLLibrary", "newFunctionWithDescriptor:error:", (IMP)w_fndesc);
    o_fnplib = tr("_MTLLibrary", "newFunctionWithName:constantValues:pipelineLibrary:error:", (IMP)w_fnplib);
    o_t1 = tr("MTLIGAccelDevice", "newTextureWithDescriptor:buffer:offset:bytesPerRow:", (IMP)w_t1);
    o_t2 = tr("MTLIOAccelBuffer", "newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:", (IMP)w_t2);
    o_t3 = tr("MTLIOAccelDevice", "newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:", (IMP)w_t3);
    o_t4 = tr("MTLIOAccelDevice", "newTiledTextureWithBytesNoCopy:length:deallocator:descriptor:offset:bytesPerRow:", (IMP)w_t4);
    fprintf(stderr, "encspy texapi: %d %d %d %d\n", !!o_t1, !!o_t2, !!o_t3, !!o_t4);
    o_buftex = tr("MTLIGAccelBuffer", "newTextureWithDescriptor:offset:bytesPerRow:", (IMP)w_buftex);
    fprintf(stderr, "encspy: rp=%d ps=%d tex=%d draw=%d drawi=%d buftex=%d\n", !!o_rp, !!o_setps, !!o_settex, !!o_draw, !!o_drawi, !!o_buftex);
    return NULL;
}
__attribute__((constructor)) static void ini(void) { instalar(NULL); }
