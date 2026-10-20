// xe-nofetch: dylib de teste — faz o driver Metal TGL anunciar SEM framebuffer fetch / programmable blending /
// raster order groups (o CoreAnimation passa a usar mistura por hardware).
// Compilar: clang -fno-objc-arc -dynamiclib -o ~/xe/build/xe-nofetch.dylib ~/xe/tools/xe-nofetch.m -framework Foundation
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <dlfcn.h>
static BOOL nao(id s, SEL c) { return NO; }
__attribute__((constructor)) static void ini(void) {
    Class d = objc_getClass("MTLIGAccelDevice");
    if (!d) { void *h = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW); (void)h; }
    const char *sels[] = {"isFramebufferReadSupported", "supportsProgrammableBlending", "areRasterOrderGroupsSupported"};
    for (int i = 0; i < 3; i++) {
        Class k = objc_getClass("MTLIGAccelDevice");
        Method m = k ? class_getInstanceMethod(k, sel_registerName(sels[i])) : NULL;
        if (m) method_setImplementation(m, (IMP)nao);
        fprintf(stderr, "nofetch: %s %s\n", sels[i], m ? "-> NO" : "(classe/método ausente)");
    }
}
