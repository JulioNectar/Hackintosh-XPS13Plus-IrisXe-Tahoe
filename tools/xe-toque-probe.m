// xe-toque --probe-events [touch_s] [trackpad_s] [out]
// Diagnostic: logs every CGEvent (listen-only tap at the HID level) with all of its integer fields (0..255) plus the
// raw touchscreen HID activity, so we can learn which fields tell the touchscreen's synthetic pointer events
// (VoodooI2CTouchscreenHIDEventDriver) apart from the trackpad's and the mouse's.
//   Glass     -> phase 1 (touch_s, default 25 s): use ONLY the touchscreen
//   Ping      -> phase 2 (trackpad_s, default 15 s): use ONLY the trackpad
//   Submarine -> done; the report is written to `out` (default ~/xe/build/toque/probe-<time>.txt)
// An event is labelled "touch" when it arrives in phase 1 while the touchscreen HID stream is active
// (a value within the last 250 ms; VoodooI2C sends the click ~50 ms after the finger lifts).
#import <Cocoa/Cocoa.h>
#import <IOKit/hid/IOHIDManager.h>
#import <IOKit/IOKitLib.h>

#define NF 256

typedef struct { double t; int phase, touch, contacts; CGEventType type; CGPoint loc; int64_t f[NF]; double d[NF]; } Reg;

static NSMutableData *regs;
static double t0, lastHID = -1;
static int phase = 0, tipsDown = 0, maxTips = 0, touchDowns = 0;
static CFMachPortRef ptap;
static NSMutableDictionary<NSValue *, NSNumber *> *tipOf;  // collection -> tip

static double now(void) { return CACurrentMediaTime(); }

static const char *nomeTipo(CGEventType t) {
    switch ((int)t) {
        case 1: return "LMouseDown"; case 2: return "LMouseUp"; case 3: return "RMouseDown"; case 4: return "RMouseUp";
        case 5: return "MouseMoved"; case 6: return "LMouseDragged"; case 7: return "RMouseDragged";
        case 10: return "KeyDown"; case 11: return "KeyUp"; case 12: return "FlagsChanged";
        case 22: return "ScrollWheel"; case 23: return "TabletPointer"; case 24: return "TabletProximity";
        case 25: return "OtherMouseDown"; case 26: return "OtherMouseUp"; case 27: return "OtherMouseDragged";
        case 29: return "Gesture"; case 30: return "Magnify"; case 31: return "Swipe"; case 18: return "Rotate";
        case 19: return "BeginGesture"; case 20: return "EndGesture"; case 32: return "SmartMagnify";
        case 34: return "Pressure"; case 37: return "DirectTouch"; case 38: return "ChangeMode";
        case kCGEventTapDisabledByTimeout: return "TapDisabledByTimeout";
        case kCGEventTapDisabledByUserInput: return "TapDisabledByUserInput";
        default: { static char b[8][16]; static int k; k = (k + 1) % 8; snprintf(b[k], 16, "type%d", (int)t); return b[k]; }
    }
}

static CGEventRef cbTap(CGEventTapProxy p, CGEventType type, CGEventRef e, void *ctx) {
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) { CGEventTapEnable(ptap, true); return e; }
    if (!phase) return e;
    Reg r; memset(&r, 0, sizeof r);
    r.t = now() - t0; r.phase = phase; r.type = type; r.loc = CGEventGetLocation(e); r.contacts = tipsDown;
    r.touch = phase == 1 && lastHID > 0 && now() - lastHID < 0.25;
    for (int i = 0; i < NF; i++) { r.f[i] = CGEventGetIntegerValueField(e, i); r.d[i] = CGEventGetDoubleValueField(e, i); }
    [regs appendBytes:&r length:sizeof r];
    return e;
}

static void cbHID(void *ctx, IOReturn res, void *snd, IOHIDValueRef v) {
    lastHID = now();
    IOHIDElementRef el = IOHIDValueGetElement(v);
    if (IOHIDElementGetUsagePage(el) != kHIDPage_Digitizer || IOHIDElementGetUsage(el) != kHIDUsage_Dig_TipSwitch) return;
    NSValue *k = [NSValue valueWithPointer:IOHIDElementGetParent(el)];
    int tip = (int)IOHIDValueGetIntegerValue(v), old = tipOf[k].intValue;
    tipOf[k] = @(tip);
    if (tip && !old) { tipsDown++; if (phase == 1) touchDowns++; } else if (!tip && old) tipsDown--;
    if (tipsDown > maxTips && phase == 1) maxTips = tipsDown;
}

static void som(NSString *n) {
    NSSound *s = [[NSSound alloc] initWithContentsOfFile:[NSString stringWithFormat:@"/System/Library/Sounds/%@.aiff", n] byReference:YES];
    [s play];
}

static NSString *ids(void) {
    // registry IDs of the pointer-producing services, so sender-ID-like fields can be recognised
    NSMutableString *o = [NSMutableString string];
    for (NSString *cls in @[@"VoodooI2CTouchscreenHIDEventDriver", @"VoodooI2CPrecisionTouchpadHIDEventDriver",
                            @"VoodooInputSimulatorDevice", @"VoodooI2CHIDDevice", @"ApplePS2Mouse", @"ApplePS2SynapticsTouchPad"]) {
        io_iterator_t it = 0;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(cls.UTF8String), &it)) continue;
        io_object_t s;
        while ((s = IOIteratorNext(it))) {
            uint64_t id = 0; IORegistryEntryGetRegistryEntryID(s, &id);
            io_registry_entry_t par = 0; uint64_t pid = 0;
            if (!IORegistryEntryGetParentEntry(s, kIOServicePlane, &par)) { IORegistryEntryGetRegistryEntryID(par, &pid); IOObjectRelease(par); }
            [o appendFormat:@"  %-42s id=0x%llx (%llu) parent=0x%llx\n", cls.UTF8String, id, id, pid];
            IOObjectRelease(s);
        }
        IOObjectRelease(it);
    }
    return o;
}

static NSString *conj(NSSet *s) {
    NSArray *a = [s.allObjects sortedArrayUsingSelector:@selector(compare:)];
    if (a.count > 8) return [NSString stringWithFormat:@"{%@ ... %lu values ... %@}", a.firstObject, (unsigned long)a.count, a.lastObject];
    return [NSString stringWithFormat:@"{%@}", [a componentsJoinedByString:@","]];
}

static void relatorio(NSString *out) {
    NSMutableString *o = [NSMutableString string];
    Reg *R = regs.mutableBytes; NSUInteger n = regs.length / sizeof(Reg);
    [o appendFormat:@"xe-toque probe %@\nservices:\n%@", [NSDate date], ids()];
    [o appendFormat:@"touch-downs in phase 1: %d, max simultaneous contacts: %d, events logged: %lu\n\n", touchDowns, maxTips, (unsigned long)n];

    // groups: T = phase 1 & touch active, P = phase 2 (trackpad), O = phase 1 without touch activity
    NSMutableDictionary *cnt = [NSMutableDictionary dictionary];
    for (NSUInteger i = 0; i < n; i++) {
        NSString *g = R[i].phase == 2 ? @"P" : R[i].touch ? @"T" : @"O";
        NSString *k = [NSString stringWithFormat:@"%-16s %@", nomeTipo(R[i].type), g];
        cnt[k] = @([cnt[k] intValue] + 1);
    }
    [o appendString:@"== event counts (T = touchscreen active in phase 1, P = phase 2 trackpad, O = phase 1 no touch) ==\n"];
    for (NSString *k in [cnt.allKeys sortedArrayUsingSelector:@selector(compare:)]) [o appendFormat:@"  %@  %@\n", k, cnt[k]];

    // per mouse-ish type and per field: value sets for T vs P; flag the disjoint ones
    [o appendString:@"\n== fields whose values never overlap between T and P (per event type) ==\n"];
    NSMutableSet *tipos = [NSMutableSet set];
    for (NSUInteger i = 0; i < n; i++) [tipos addObject:@(R[i].type)];
    for (NSNumber *tp in [tipos.allObjects sortedArrayUsingSelector:@selector(compare:)]) {
        BOOL cabecalho = NO;
        for (int f = 0; f < NF; f++) {
            NSMutableSet *T = [NSMutableSet set], *P = [NSMutableSet set];
            for (NSUInteger i = 0; i < n; i++) {
                if (R[i].type != tp.intValue) continue;
                if (R[i].phase == 2) [P addObject:@(R[i].f[f])]; else if (R[i].touch) [T addObject:@(R[i].f[f])];
            }
            if (!T.count) continue;
            BOOL disj = P.count && ![T intersectsSet:P];
            BOOL soT = !P.count && T.count == 1;  // no trackpad sample of this type: show constant touch fields
            if (!disj && !soT) continue;
            if (soT && [T containsObject:@0]) continue;
            if (!cabecalho) { [o appendFormat:@" %s:\n", nomeTipo(tp.intValue)]; cabecalho = YES; }
            [o appendFormat:@"   field %3d: T=%@  P=%@%@\n", f, conj(T), conj(P), disj ? @"   <== DISJOINT" : @""];
        }
    }

    // a few full samples of each (type, group)
    [o appendString:@"\n== samples (non-zero integer fields; doubles shown when not equal to the integer) ==\n"];
    NSMutableDictionary *vistos = [NSMutableDictionary dictionary];
    for (NSUInteger i = 0; i < n; i++) {
        NSString *g = R[i].phase == 2 ? @"P" : R[i].touch ? @"T" : @"O";
        NSString *k = [NSString stringWithFormat:@"%d%@", R[i].type, g];
        int c = [vistos[k] intValue]; if (c >= 3) continue; vistos[k] = @(c + 1);
        [o appendFormat:@"[%@] t=%.3f %s at (%.0f,%.0f) tips=%d:", g, R[i].t, nomeTipo(R[i].type), R[i].loc.x, R[i].loc.y, R[i].contacts];
        for (int f = 0; f < NF; f++) {
            if (R[i].f[f]) [o appendFormat:@" %d=%lld", f, R[i].f[f]];
            if (R[i].d[f] != (double)R[i].f[f] && R[i].d[f] != 0) [o appendFormat:@" %d~%.3f", f, R[i].d[f]];
        }
        [o appendString:@"\n"];
    }

    // full timeline (compact): time, group, type, loc, a few interesting fields
    [o appendString:@"\n== timeline (fields 3 click, 7 subtype, 41 pid?, 45 srcstate, 87+, 2 pressure) ==\n"];
    for (NSUInteger i = 0; i < n; i++) {
        NSString *g = R[i].phase == 2 ? @"P" : R[i].touch ? @"T" : @"O";
        [o appendFormat:@"%7.3f %@ tips=%d %-14s (%5.0f,%5.0f) f1=%lld f3=%lld f7=%lld f41=%lld f45=%lld f87=0x%llx f88=%lld f2=%.2f f99=%lld f132=%lld\n",
         R[i].t, g, R[i].contacts, nomeTipo(R[i].type), R[i].loc.x, R[i].loc.y, R[i].f[1], R[i].f[3], R[i].f[7], R[i].f[41],
         R[i].f[45], R[i].f[87], R[i].f[88], R[i].d[2], R[i].f[99], R[i].f[132]];
    }
    [o writeToFile:out atomically:YES encoding:NSUTF8StringEncoding error:nil];
    fprintf(stderr, "report: %s\n", out.UTF8String);
}

int xeProbeEvents(int argc, const char **argv) { @autoreleasepool {
    double durT = argc > 2 ? atof(argv[2]) : 25, durP = argc > 3 ? atof(argv[3]) : 15;
    NSString *dir = [@"~/xe/build/toque" stringByExpandingTildeInPath];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter *df = [NSDateFormatter new]; df.dateFormat = @"yyyyMMdd-HHmmss";
    NSString *out = argc > 4 ? @(argv[4]) : [dir stringByAppendingFormat:@"/probe-%@.txt", [df stringFromDate:[NSDate date]]];
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    regs = [NSMutableData data]; tipOf = [NSMutableDictionary dictionary];

    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(m, (__bridge CFDictionaryRef)@{@kIOHIDDeviceUsagePageKey: @(kHIDPage_Digitizer),
                                                                 @kIOHIDDeviceUsageKey: @(kHIDUsage_Dig_TouchScreen)});
    IOHIDManagerRegisterInputValueCallback(m, cbHID, NULL);
    IOHIDManagerScheduleWithRunLoop(m, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOReturn r = IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    fprintf(stderr, "probe: HID open=0x%x listen=%d post=%d ax=%d\n", r, CGPreflightListenEventAccess(), CGPreflightPostEventAccess(), AXIsProcessTrusted());

    ptap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly, kCGEventMaskForAllEvents, cbTap, NULL);
    if (!ptap) { fprintf(stderr, "probe: CGEventTapCreate failed (Input Monitoring / Accessibility not granted)\n"); return 2; }
    CFRunLoopAddSource(CFRunLoopGetMain(), CFMachPortCreateRunLoopSource(NULL, ptap, 0), kCFRunLoopCommonModes);
    CGEventTapEnable(ptap, true);

    t0 = now(); phase = 1; som(@"Glass");
    fprintf(stderr, "probe: phase 1 (%.0f s) TOUCHSCREEN ONLY\n", durT);
    [NSTimer scheduledTimerWithTimeInterval:durT repeats:NO block:^(NSTimer *t) {
        phase = 2; som(@"Ping"); fprintf(stderr, "probe: phase 2 (%.0f s) TRACKPAD ONLY\n", durP);
        [NSTimer scheduledTimerWithTimeInterval:durP repeats:NO block:^(NSTimer *t2) {
            phase = 0; som(@"Submarine");
            relatorio(out);
            [NSTimer scheduledTimerWithTimeInterval:1.5 repeats:NO block:^(NSTimer *t3) { exit(0); }];  // let the sound finish
        }];
    }];
    [NSApp run];
    return 0;
} }
