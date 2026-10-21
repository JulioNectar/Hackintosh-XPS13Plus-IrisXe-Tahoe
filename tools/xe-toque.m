// xe-toque: draws a fading trail under your fingers, only for the touchscreen (trackpad and mouse are ignored).
// Reads the digitizer directly with IOHIDManager (usage page 0x0D, usage 0x04 = Touch Screen) and draws in a
// transparent, click-through window above everything. Needs Input Monitoring the first time.
// Build: zsh tools/build-xe-toque.sh  ->  tools/xe-toque.app (up to 10 fingers, one colour each)
#import <Cocoa/Cocoa.h>
#import <IOKit/hid/IOHIDManager.h>

#define MAXF 10
#define FADE 0.7  // seconds until the trail fades out

typedef struct { CGPoint p; double t; } Ponto;
typedef struct { IOHIDElementRef col; int tip, id; double x, y; BOOL temX, temY; } Slot;

static Slot slots[16];
static int nslots;
static NSMutableArray<NSMutableData *> *rastros;  // one per contact identifier
static double xmax = 1, ymax = 1;

static double agora(void) { return CACurrentMediaTime(); }

@interface Tela : NSView
@end
@implementation Tela
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)r {
    double t = agora();
    NSArray *cores = @[NSColor.systemPinkColor, NSColor.systemTealColor, NSColor.systemYellowColor,
                       NSColor.systemGreenColor, NSColor.systemOrangeColor, NSColor.systemPurpleColor,
                       NSColor.systemBlueColor, NSColor.systemRedColor, NSColor.systemMintColor, NSColor.whiteColor];
    for (NSUInteger f = 0; f < rastros.count; f++) {
        NSMutableData *d = rastros[f];
        Ponto *v = d.mutableBytes; NSUInteger n = d.length / sizeof(Ponto), k = 0;
        for (NSUInteger i = 0; i < n; i++) if (t - v[i].t < FADE) v[k++] = v[i];  // drop old points
        d.length = k * sizeof(Ponto);
        NSColor *c = cores[f % cores.count];
        for (NSUInteger i = 1; i < k; i++) {
            if (v[i].p.x < 0 || v[i-1].p.x < 0) continue;  // "finger lifted" marker
            double a = 1.0 - (t - v[i].t) / FADE;
            NSBezierPath *b = [NSBezierPath bezierPath];
            b.lineWidth = 4 + 10 * a; b.lineCapStyle = NSLineCapStyleRound;
            [b moveToPoint:v[i-1].p]; [b lineToPoint:v[i].p];
            [[c colorWithAlphaComponent:a * 0.85] setStroke]; [b stroke];
        }
    }
}
@end

static Tela *tela;

static void fimDeRelatorio(void) {
    NSRect fr = tela.bounds;
    for (int i = 0; i < nslots; i++) {
        Slot *s = &slots[i];
        int f = s->id >= 0 ? s->id % MAXF : i;
        NSMutableData *d = rastros[f];
        Ponto p = { CGPointMake(-1, -1), agora() };
        if (s->tip && s->temX && s->temY)
            p.p = CGPointMake(s->x / xmax * fr.size.width, s->y / ymax * fr.size.height);
        Ponto *ult = d.length ? (Ponto *)d.mutableBytes + d.length / sizeof(Ponto) - 1 : NULL;
        if (p.p.x < 0 && (!ult || ult->p.x < 0)) continue;  // already lifted
        [d appendBytes:&p length:sizeof p];
    }
    tela.needsDisplay = YES;
}

static Slot *slotDe(IOHIDElementRef el) {
    IOHIDElementRef col = IOHIDElementGetParent(el);
    for (int i = 0; i < nslots; i++) if (slots[i].col == col) return &slots[i];
    if (nslots == 16) return NULL;
    slots[nslots] = (Slot){ col, 0, -1, 0, 0, NO, NO };
    return &slots[nslots++];
}

static void valor(void *ctx, IOReturn r, void *snd, IOHIDValueRef v) {
    IOHIDElementRef el = IOHIDValueGetElement(v);
    uint32_t pg = IOHIDElementGetUsagePage(el), us = IOHIDElementGetUsage(el);
    CFIndex iv = IOHIDValueGetIntegerValue(v);
    if (pg == kHIDPage_Digitizer && (us == kHIDUsage_Dig_ContactCount || us == 0x56)) return;  // contact count / scan time (sent only on change / every report)
    Slot *s = slotDe(el); if (!s) return;
    if (pg == kHIDPage_Digitizer && us == kHIDUsage_Dig_TipSwitch) s->tip = (int)iv;
    else if (pg == kHIDPage_Digitizer && us == kHIDUsage_Dig_ContactIdentifier) s->id = (int)iv;
    else if (pg == kHIDPage_GenericDesktop && us == kHIDUsage_GD_X) {
        s->x = iv; s->temX = YES; double m = IOHIDElementGetLogicalMax(el); if (m > 1) xmax = m;
    } else if (pg == kHIDPage_GenericDesktop && us == kHIDUsage_GD_Y) {
        s->y = iv; s->temY = YES; double m = IOHIDElementGetLogicalMax(el); if (m > 1) ymax = m;
    }
}

int main(void) { @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    rastros = [NSMutableArray array];
    for (int i = 0; i < MAXF; i++) [rastros addObject:[NSMutableData data]];

    NSScreen *sc = NSScreen.screens.firstObject;  // the built-in panel (where the touchscreen is)
    NSWindow *w = [[NSWindow alloc] initWithContentRect:sc.frame styleMask:NSWindowStyleMaskBorderless
                                                backing:NSBackingStoreBuffered defer:NO];
    w.opaque = NO; w.backgroundColor = NSColor.clearColor; w.hasShadow = NO;
    w.ignoresMouseEvents = YES; w.level = NSScreenSaverWindowLevel;
    w.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary |
                           NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorIgnoresCycle;
    tela = [[Tela alloc] initWithFrame:(NSRect){NSZeroPoint, sc.frame.size}];
    w.contentView = tela; [w orderFrontRegardless];

    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(m, (__bridge CFDictionaryRef)@{@kIOHIDDeviceUsagePageKey: @(kHIDPage_Digitizer),
                                                                 @kIOHIDDeviceUsageKey: @(kHIDUsage_Dig_TouchScreen)});
    IOHIDManagerRegisterInputValueCallback(m, valor, NULL);
    IOHIDManagerScheduleWithRunLoop(m, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOReturn r = IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    NSSet *devs = CFBridgingRelease(IOHIDManagerCopyDevices(m));
    fprintf(stderr, "xe-toque: %lu touchscreen(s), open=0x%x%s\n", (unsigned long)devs.count, r,
            r ? " (allow it in System Settings > Privacy & Security > Input Monitoring)" : "");

    [NSTimer scheduledTimerWithTimeInterval:1.0/60 repeats:YES block:^(NSTimer *t) { fimDeRelatorio(); }];
    [NSApp run];
} }
