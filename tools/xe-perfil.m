// xe-perfil: define (ou mostra) o perfil de cor da tela principal via ColorSync, sem abrir Ajustes > Telas.
//   xe-perfil                 mostra o perfil atual
//   xe-perfil /caminho.icc    usa esse perfil     |   xe-perfil padrao   volta ao perfil de fábrica (EDID)
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-perfil ~/xe/tools/xe-perfil.m -framework ColorSync -framework CoreGraphics -framework Foundation
#import <Foundation/Foundation.h>
#import <ColorSync/ColorSync.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
int main(int argc, char **argv) { @autoreleasepool {
    CGDirectDisplayID d = CGMainDisplayID();
    CFUUIDRef u = CGDisplayCreateUUIDFromDisplayID(d);
    NSDictionary *info = CFBridgingRelease(ColorSyncDeviceCopyDeviceInfo(kColorSyncDisplayDeviceClass, u));
    NSDictionary *cust = info[(__bridge id)kColorSyncCustomProfiles];
    printf("perfis personalizados: %s\n", cust ? cust.description.UTF8String : "(nenhum)");
    if (argc < 2) return 0;
    id val = !strcmp(argv[1], "padrao") ? (id)kCFNull : [NSURL fileURLWithPath:@(argv[1])];
    NSDictionary *p = @{(__bridge id)kColorSyncDeviceDefaultProfileID: val};
    bool ok = ColorSyncDeviceSetCustomProfiles(kColorSyncDisplayDeviceClass, u, (__bridge CFDictionaryRef)p);
    printf("definir perfil: %s\n", ok ? "ok" : "FALHOU");
    CFRelease(u);
} return 0; }
