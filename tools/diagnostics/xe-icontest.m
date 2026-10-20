// xe-icontest: pede ícones ao IconServices (NSWorkspace) e conta pixels opacos (0 = ícone vazio).
// Compilar: clang -fobjc-arc -o ~/xe/build/xe-icontest ~/xe/tools/xe-icontest.m -framework AppKit
#import <AppKit/AppKit.h>
#include <stdio.h>
int main() { @autoreleasepool {
    NSArray *apps = @[@"/System/Applications/Calculator.app", @"/System/Applications/Notes.app", @"/System/Applications/App Store.app",
                      @"/Applications/Google Chrome.app", @"/Applications/HeliPort.app", @"/System/Library/CoreServices/Finder.app"];
    for (NSString *p in apps) {
        NSImage *img = [[NSWorkspace sharedWorkspace] iconForFile:p];
        for (NSNumber *sz in @[@32, @128, @512]) {
            int s = sz.intValue;
            NSBitmapImageRep *r = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:s pixelsHigh:s bitsPerSample:8
                samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:s*4 bitsPerPixel:32];
            [NSGraphicsContext saveGraphicsState];
            [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:r]];
            [img drawInRect:NSMakeRect(0, 0, s, s) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1];
            [NSGraphicsContext restoreGraphicsState];
            unsigned char *d = r.bitmapData; long op = 0; double R = 0, G = 0, B = 0;
            for (long i = 0; i < (long)s * s; i++) if (d[i*4+3] > 16) { op++; R += d[i*4]; G += d[i*4+1]; B += d[i*4+2]; }
            printf("%-24s %4dpx opacos=%5.1f%% cor média=(%3.0f,%3.0f,%3.0f)\n", p.lastPathComponent.UTF8String, s, 100.0 * op / (s * s), op ? R/op : 0, op ? G/op : 0, op ? B/op : 0);
            if (s == 512) [[r representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[NSString stringWithFormat:@"/tmp/icone-%@.png", p.lastPathComponent.stringByDeletingPathExtension] atomically:YES];
        }
    }
} return 0; }
