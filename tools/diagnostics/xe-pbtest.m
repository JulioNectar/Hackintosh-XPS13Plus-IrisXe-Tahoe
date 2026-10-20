// xe-pbtest: cria CVPixelBuffers com vários formatos/atributos e mostra quais falham (ex.: -6680 = formato inválido).
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-pbtest ~/xe/tools/xe-pbtest.m -framework CoreVideo -framework Foundation
#import <CoreVideo/CoreVideo.h>
#include <stdio.h>
int main() { @autoreleasepool {
    OSType fmts[] = {'BGRA', '420v', '420f', 'x420', 'xf20', 'RGhA', 'l10r', 'w30r', '2vuy', 'y420'};
    NSArray *nomes = @[@"IOSurface", @"IOSurface+Metal", @"IOSurface+GL", @"IOSurface+Metal+GL"];
    NSArray *attrs = @[
      @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}},
      @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}, (id)kCVPixelBufferMetalCompatibilityKey: @YES},
      @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}, (id)kCVPixelBufferOpenGLCompatibilityKey: @YES},
      @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}, (id)kCVPixelBufferMetalCompatibilityKey: @YES, (id)kCVPixelBufferOpenGLCompatibilityKey: @YES}];
    printf("%-6s", "fmt");
    for (NSString *n in nomes) printf(" %20s", n.UTF8String);
    printf("\n");
    for (int i = 0; i < (int)(sizeof fmts / sizeof *fmts); i++) {
        OSType f = fmts[i];
        printf("%c%c%c%c  ", f>>24, (f>>16)&0xff, (f>>8)&0xff, f&0xff);
        for (NSDictionary *a in attrs) {
            CVPixelBufferRef pb = NULL;
            CVReturn r = CVPixelBufferCreate(NULL, 640, 480, f, (__bridge CFDictionaryRef)a, &pb);
            printf(" %20s", r ? [NSString stringWithFormat:@"ERRO %d", r].UTF8String : "ok");
            if (pb) CVPixelBufferRelease(pb);
        }
        printf("\n");
    }
} return 0; }
