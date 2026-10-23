// xe-toque: touchscreen UX for the XPS 13 Plus (VoodooI2C touchscreen). Trackpad and mouse are never touched.
//  - reads the digitizer directly with IOHIDManager (usage page 0x0D, usage 0x04 = Touch Screen)
//  - draws trail / ripples / pen strokes / loupe in a transparent, click-through window above everything
//  - an active CGEventTap drops the pointer events VoodooI2C synthesizes for the touchscreen (recognised by CGEvent
//    field 87 = sender registry ID) whenever xe-toque's own gesture handling owns the touch, and posts its own events:
//    1 finger: drag = scroll with momentum, tap = click, hold 0.5 s = right click, hold then drag = left drag
//    window-buttons magnet: a tap near close/minimize/zoom presses the nearest one
//    edges: swipe in from the right = Notification Center, swipe up from the bottom = Mission Control
//    optional own multi-finger: 2 = scroll, 3 horizontal = switch Space, 5 pinch = Apps; 2-finger hold = magnifier
//  - menu-bar item with the toggles (stored in NSUserDefaults, domain local.xe.toque)
// Needs Input Monitoring + Accessibility (Screen Recording only for the magnifier).
// Build: zsh tools/build-xe-toque.sh  ->  tools/xe-toque.app.  Diagnostic: xe-toque --probe-events (see xe-toque-probe.m)
#import <Cocoa/Cocoa.h>
#import <IOKit/hid/IOHIDManager.h>
#import <IOKit/IOKitLib.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#define MAXF 10
#define FADE 0.7           // seconds until the trail fades out
#define MAGIC 0x78655430   // kCGEventSourceUserData of the events we post ("xeT0")
#define F_SENDER 87        // undocumented CGEvent field: registry ID of the IOHIDEventService that sent the event
#define MOVE 10.0          // points before a touch counts as a drag
#define HOLD_T 0.5         // seconds for press-and-hold
#define EDGE 24.0          // edge zone (points)
#define EDGE_DIST 70.0     // distance an edge swipe must travel
#define LUPA_R 90.0        // loupe radius (points)
#define LUPA_ZOOM 2.0

typedef struct { CGPoint p; double t; } Ponto;
typedef struct { IOHIDElementRef col; uint32_t cookie; int tip, id; double x, y; BOOL temX, temY; } Slot;
typedef struct { int id; CGPoint p; } Contato;
typedef struct { CGPoint p; double t; int tipo; } Onda;  // ripple: 0 touch-down, 1 hold / action

static Slot slots[16];
static int nslots, ccHID = -1;
static double xmax = 1, ymax = 1;
static NSMutableArray<NSMutableData *> *rastros;  // one per contact identifier
static NSMutableData *ondas;
static CGRect B;                 // built-in panel, global CG coordinates (top-left origin, points)
static CGDirectDisplayID gDisp;
static NSWindow *janela;
static NSUserDefaults *ud;
static CGEventSourceRef src;

// shared with the event-tap thread
static volatile double gLastHID = -10, gClaimUntil = 0;
static volatile int gClaim, gDropMoves, gPen, gOwnMulti, gTapOK;
static volatile uint64_t gTS;           // touchscreen driver registry ID
static volatile uint64_t gTSDesc[64];   // its registry descendants (emulated multitouch device etc.)
static volatile int gNTSDesc;
static CFMachPortRef tapRef;

static double agora(void) { return CACurrentMediaTime(); }
static BOOL opt(NSString *k) { return [ud boolForKey:k]; }
static double dist(CGPoint a, CGPoint b) { return hypot(a.x - b.x, a.y - b.y); }

static void xlog(NSString *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap]; va_end(ap);
    NSString *path = [@"~/Library/Logs/xe-toque/xe-toque.log" stringByExpandingTildeInPath];
    NSString *linha = [NSString stringWithFormat:@"%@ %@\n", [NSDate date], s];
    fputs(linha.UTF8String, stderr);
    NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:path];
    if (!h) { [[NSFileManager defaultManager] createFileAtPath:path contents:nil attributes:nil]; h = [NSFileHandle fileHandleForWritingAtPath:path]; }
    [h seekToEndOfFile]; [h writeData:[linha dataUsingEncoding:NSUTF8StringEncoding]]; [h closeFile];
}

// ---------------------------------------------------------------- posting events

static void marca(CGEventRef e) { CGEventSetIntegerValueField(e, kCGEventSourceUserData, MAGIC); }
static void posta(CGEventRef e) { if (!e) return; marca(e); CGEventPost(kCGHIDEventTap, e); CFRelease(e); }

static void mouse(CGEventType ty, CGPoint p, CGMouseButton b, int cs) {
    CGEventRef e = CGEventCreateMouseEvent(src, ty, p, b);
    CGEventSetIntegerValueField(e, kCGMouseEventClickState, cs);
    posta(e);
}

static double accX, accY;
// phase: kCGScrollPhaseBegan 1, Changed 2, Ended 4; mphase: kCGMomentumScrollPhaseBegin 1, Continue 2, End 3
static void rolar(double dx, double dy, int phase, int mphase) {
    accX += dx; accY += dy;
    int ix = (int)accX, iy = (int)accY; accX -= ix; accY -= iy;
    CGEventRef e = CGEventCreateScrollWheelEvent2(src, kCGScrollEventUnitPixel, 2, iy, ix, 0);
    if (!e) return;
    CGEventSetIntegerValueField(e, kCGScrollWheelEventIsContinuous, 1);
    CGEventSetIntegerValueField(e, kCGScrollWheelEventPointDeltaAxis1, iy);
    CGEventSetIntegerValueField(e, kCGScrollWheelEventPointDeltaAxis2, ix);
    CGEventSetIntegerValueField(e, kCGScrollWheelEventScrollPhase, phase);
    CGEventSetIntegerValueField(e, kCGScrollWheelEventMomentumPhase, mphase);
    posta(e);
}

static void tecla(CGKeyCode k, CGEventFlags f) {
    for (int d = 1; d >= 0; d--) { CGEventRef e = CGEventCreateKeyboardEvent(src, k, d); CGEventSetFlags(e, f); posta(e); }
}

static void abrirApp(NSArray<NSString *> *paths) {
    for (NSString *p in paths) if ([NSFileManager.defaultManager fileExistsAtPath:p]) {
        [NSWorkspace.sharedWorkspace openApplicationAtURL:[NSURL fileURLWithPath:p]
                                            configuration:[NSWorkspaceOpenConfiguration configuration] completionHandler:nil];
        return;
    }
}

static void centralNotificacoes(void) {
    // click the menu-bar clock through Accessibility (opens Notification Center on Tahoe); identifier is not localized
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        NSRunningApplication *a = [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.controlcenter"].firstObject;
        if (!a) { xlog(@"NC: ControlCenter not running"); return; }
        AXUIElementRef app = AXUIElementCreateApplication(a.processIdentifier);
        CFTypeRef bar = NULL, kids = NULL;
        AXUIElementCopyAttributeValue(app, CFSTR("AXExtrasMenuBar"), &bar);
        if (bar) AXUIElementCopyAttributeValue(bar, kAXChildrenAttribute, &kids);
        BOOL ok = NO;
        for (id k in (__bridge NSArray *)kids) {
            CFTypeRef ident = NULL;
            AXUIElementCopyAttributeValue((__bridge AXUIElementRef)k, CFSTR("AXIdentifier"), &ident);
            if ([(__bridge id)ident isEqual:@"com.apple.menuextra.clock"])
                ok = AXUIElementPerformAction((__bridge AXUIElementRef)k, kAXPressAction) == kAXErrorSuccess;
            if (ident) CFRelease(ident);
            if (ok) break;
        }
        if (kids) CFRelease(kids);
        if (bar) CFRelease(bar);
        CFRelease(app);
        if (!ok) xlog(@"NC: could not press the menu-bar clock (Accessibility granted? %d)", AXIsProcessTrusted());
    });
}
static void missionControl(void) { abrirApp(@[@"/System/Applications/Mission Control.app"]); }
static void appsLaunchpad(void) { abrirApp(@[@"/System/Applications/Apps.app", @"/System/Applications/Launchpad.app"]); }
static void trocarEspaco(int dir) {  // +1 = Space to the right (ctrl+→), -1 = left
    tecla(dir > 0 ? 124 : 123, kCGEventFlagMaskControl | kCGEventFlagMaskSecondaryFn | kCGEventFlagMaskNumericPad);
}

// ---------------------------------------------------------------- overlay

static NSMutableArray<NSBezierPath *> *tracos;            // pen strokes
static NSMutableDictionary<NSNumber *, NSBezierPath *> *tracoAtual;
static BOOL lupaOn; static CGPoint lupaC; static CGImageRef lupaImg;

static CGPoint local(CGPoint g) { return CGPointMake(g.x - B.origin.x, g.y - B.origin.y); }
static void onda(CGPoint g, int tipo) {
    if (!opt(@"ripple")) return;
    Onda o = { local(g), agora(), tipo }; [ondas appendBytes:&o length:sizeof o];
}

@interface Tela : NSView
@end
@implementation Tela
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)r {
    double t = agora();
    NSArray *cores = @[NSColor.systemPinkColor, NSColor.systemTealColor, NSColor.systemYellowColor,
                       NSColor.systemGreenColor, NSColor.systemOrangeColor, NSColor.systemPurpleColor,
                       NSColor.systemBlueColor, NSColor.systemRedColor, NSColor.systemMintColor, NSColor.whiteColor];
    // trail
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
    // ripples
    Onda *o = ondas.mutableBytes; NSUInteger n = ondas.length / sizeof(Onda), k = 0;
    for (NSUInteger i = 0; i < n; i++) {
        double dur = o[i].tipo == 2 ? 0.55 : o[i].tipo ? 0.45 : 0.35, a = (t - o[i].t) / dur;
        if (a >= 1) continue;
        o[k++] = o[i];
        if (o[i].tipo == 2) {  // magnet: ring closing in on the pressed window button
            double rr2 = 22 - 12 * a;
            NSBezierPath *b = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(o[i].p.x - rr2, o[i].p.y - rr2, 2 * rr2, 2 * rr2)];
            b.lineWidth = 3; [[NSColor.systemBlueColor colorWithAlphaComponent:0.9 * (1 - a)] setStroke]; [b stroke];
            continue;
        }
        double raio = o[i].tipo ? 44 - 20 * a : 8 + 28 * a;
        NSRect rr = NSMakeRect(o[i].p.x - raio, o[i].p.y - raio, 2 * raio, 2 * raio);
        NSBezierPath *b = [NSBezierPath bezierPathWithOvalInRect:rr];
        NSColor *c = o[i].tipo ? NSColor.systemOrangeColor : NSColor.whiteColor;
        if (!o[i].tipo) { [[NSColor.systemTealColor colorWithAlphaComponent:0.25 * (1 - a)] setFill]; [b fill]; }
        b.lineWidth = o[i].tipo ? 4 : 2.5;
        [[c colorWithAlphaComponent:0.8 * (1 - a)] setStroke]; [b stroke];
    }
    ondas.length = k * sizeof(Onda);
    // pen
    if (gPen || tracos.count) {
        [[NSColor.systemRedColor colorWithAlphaComponent:0.9] setStroke];
        for (NSBezierPath *b in tracos) [b stroke];
        for (NSBezierPath *b in tracoAtual.allValues) [b stroke];
    }
    if (gPen) {
        NSString *s = @"  Pen mode — draw with your finger · Esc exits · menu-bar ▸ Clear  ";
        NSDictionary *at = @{NSFontAttributeName: [NSFont boldSystemFontOfSize:15], NSForegroundColorAttributeName: NSColor.whiteColor};
        NSSize sz = [s sizeWithAttributes:at];
        NSRect rr = NSMakeRect((self.bounds.size.width - sz.width) / 2, 40, sz.width, sz.height + 8);
        [[NSColor colorWithWhite:0 alpha:0.6] setFill];
        [[NSBezierPath bezierPathWithRoundedRect:rr xRadius:8 yRadius:8] fill];
        [s drawAtPoint:NSMakePoint(rr.origin.x, rr.origin.y + 4) withAttributes:at];
    }
    // magnifier
    if (lupaOn && lupaImg) {
        CGPoint c = local(lupaC);
        double cy = c.y - LUPA_R - 70;
        if (cy < LUPA_R + 4) cy = c.y + LUPA_R + 70;  // near the top: show it below the fingers
        double cx = MIN(MAX(c.x, LUPA_R + 4), self.bounds.size.width - LUPA_R - 4);
        NSRect rr = NSMakeRect(cx - LUPA_R, cy - LUPA_R, 2 * LUPA_R, 2 * LUPA_R);
        NSBezierPath *b = [NSBezierPath bezierPathWithOvalInRect:rr];
        [NSGraphicsContext saveGraphicsState];
        [b addClip];
        NSImage *im = [[NSImage alloc] initWithCGImage:lupaImg size:NSZeroSize];
        [im drawInRect:rr fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1 respectFlipped:YES hints:nil];
        [NSGraphicsContext restoreGraphicsState];
        b.lineWidth = 3; [[NSColor colorWithWhite:0.2 alpha:0.9] setStroke]; [b stroke];
    }
}
@end

static Tela *tela;

static void penLimpar(void) { [tracos removeAllObjects]; [tracoAtual removeAllObjects]; tela.needsDisplay = YES; }

// ---------------------------------------------------------------- magnifier (ScreenCaptureKit)

static SCContentFilter *lupaFiltro; static BOOL lupaPedindo;

static void lupaPreparar(void) {
    if (!CGPreflightScreenCaptureAccess()) return;
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent *c, NSError *err) {
        if (!c) { xlog(@"magnifier: no shareable content (%@)", err.localizedDescription); return; }
        SCDisplay *d = nil;
        for (SCDisplay *x in c.displays) if (x.displayID == gDisp) d = x;
        NSMutableArray *ex = [NSMutableArray array];
        for (SCWindow *w in c.windows) if (w.owningApplication.processID == getpid()) [ex addObject:w];
        if (!d) return;
        SCContentFilter *f = [[SCContentFilter alloc] initWithDisplay:d excludingWindows:ex];
        dispatch_async(dispatch_get_main_queue(), ^{ lupaFiltro = f; });
    }];
}

static void lupaPedir(void) {
    if (!lupaFiltro || lupaPedindo) return;
    lupaPedindo = YES;
    SCStreamConfiguration *cfg = [SCStreamConfiguration new];
    double r = LUPA_R / LUPA_ZOOM;
    CGPoint c = local(lupaC);
    cfg.sourceRect = CGRectMake(c.x - r, c.y - r, 2 * r, 2 * r);
    cfg.width = cfg.height = (size_t)(4 * LUPA_R);
    cfg.showsCursor = NO;
    [SCScreenshotManager captureImageWithFilter:lupaFiltro configuration:cfg completionHandler:^(CGImageRef img, NSError *e) {
        if (img) CGImageRetain(img);
        dispatch_async(dispatch_get_main_queue(), ^{
            lupaPedindo = NO;
            if (!img) return;
            if (lupaImg) CGImageRelease(lupaImg);
            lupaImg = img; tela.needsDisplay = YES;
        });
    }];
}
static void lupaMostrar(CGPoint c) { lupaOn = YES; lupaC = c; if (!lupaFiltro) lupaPreparar(); lupaPedir(); }
static void lupaEsconder(void) {
    if (!lupaOn) return;
    lupaOn = NO; if (lupaImg) { CGImageRelease(lupaImg); lupaImg = NULL; }
    tela.needsDisplay = YES;
}

// ---------------------------------------------------------------- gesture engine

enum { IDLE, PEND, SCROLL, HOLD, DRAG, EDGER, EDGEB, NATIVE, MULTI, DONE };
static int est = IDLE, maxN;
static CGPoint p0, pUlt;
static double t0g;
static BOOL claimed;
static Ponto amostras[8]; static int nam;           // recent centroid samples for the fling velocity
static BOOL momOn; static double momVX, momVY;
static double ultClickT; static CGPoint ultClickP; static int nClick;
// multi-finger
static int mN; static CGPoint mC0; static double mS0, mT0; static BOOL mScroll, mFired;

static void setClaim(BOOL c) { claimed = c; gClaim = c; }

static void amostra(CGPoint c, double t) {
    if (nam == 8) { memmove(amostras, amostras + 1, 7 * sizeof(Ponto)); nam = 7; }
    amostras[nam++] = (Ponto){ c, t };
}
static CGPoint velocidade(double t) {  // points / s over the last ~100 ms
    if (nam < 2) return CGPointZero;
    Ponto ult = amostras[nam - 1];
    if (t - ult.t > 0.1) return CGPointZero;
    int i = 0; while (i < nam - 2 && ult.t - amostras[i].t > 0.1) i++;
    double dt = ult.t - amostras[i].t;
    if (dt < 0.01) return CGPointZero;
    return CGPointMake((ult.p.x - amostras[i].p.x) / dt, (ult.p.y - amostras[i].p.y) / dt);
}

static void pararMomentum(void) { if (momOn) { rolar(0, 0, 0, 3); momOn = NO; } }

static void scrollFim(BOOL comMomentum, double t) {
    rolar(0, 0, 4, 0);  // phase ended
    CGPoint v = velocidade(t);
    nam = 0;
    if (!comMomentum || hypot(v.x, v.y) < 150) return;
    double lim = 6000, s = hypot(v.x, v.y);
    if (s > lim) { v.x *= lim / s; v.y *= lim / s; }
    momVX = v.x; momVY = v.y; momOn = YES;
    rolar(momVX / 60, momVY / 60, 0, 1);
}

static void passoMomentum(void) {
    if (!momOn) return;
    momVX *= 0.955; momVY *= 0.955;
    if (hypot(momVX, momVY) < 25) { rolar(0, 0, 0, 3); momOn = NO; return; }
    rolar(momVX / 60, momVY / 60, 0, 2);
}

static void clique(CGPoint p) {
    double t = agora();
    if (t - ultClickT < 0.45 && dist(p, ultClickP) < 24) { nClick++; p = ultClickP; } else nClick = 1;
    ultClickT = t; ultClickP = p;
    mouse(kCGEventLeftMouseDown, p, kCGMouseButtonLeft, nClick);
    mouse(kCGEventLeftMouseUp, p, kCGMouseButtonLeft, nClick);
}
static void cliqueDireito(CGPoint p) {
    mouse(kCGEventRightMouseDown, p, kCGMouseButtonRight, 1);
    mouse(kCGEventRightMouseUp, p, kCGMouseButtonRight, 1);
}

// ---------------------------------------------------------------- window-buttons magnet
// A touchscreen tap within IMA_R pt of a window's close / minimize / zoom button (and inside the window's top IMA_BAND pt)
// presses the nearest of those buttons (AXPress, fallback: click at its centre). Lookup runs off the main thread.

#define IMA_R 22.0
#define IMA_BAND 52.0

typedef struct { BOOL ok; CGPoint centro; CGRect frame; AXUIElementRef botao; NSString *nome, *janela, *motivo, *hit; pid_t pid; } Ima;

static CFTypeRef axAttr(AXUIElementRef e, CFStringRef a) { CFTypeRef v = NULL; AXUIElementCopyAttributeValue(e, a, &v); return v; }
static BOOL axFrame(AXUIElementRef e, CGRect *r) {
    CFTypeRef p = axAttr(e, kAXPositionAttribute), s = axAttr(e, kAXSizeAttribute);
    BOOL ok = p && s && AXValueGetValue(p, kAXValueCGPointType, &r->origin) && AXValueGetValue(s, kAXValueCGSizeType, &r->size);
    if (p) CFRelease(p); if (s) CFRelease(s);
    return ok;
}
static NSString *axStr(AXUIElementRef e, CFStringRef a) {
    CFTypeRef v = axAttr(e, a);
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) return CFBridgingRelease(v);
    if (v) CFRelease(v);
    return nil;
}

// Decides which traffic-light button (if any) a tap at p should press. Caller releases r.botao.
static Ima imaProcurar(CGPoint p) {
    Ima r = { NO };
    // 1. topmost on-screen window under the point (cheap; avoids AX for taps far from any title bar)
    NSArray *ws = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
    NSDictionary *alvo = nil; CGRect wb = CGRectZero;
    CGDirectDisplayID dd = 0; uint32_t ndd = 0;
    CGRect tela0 = CGGetDisplaysWithPoint(p, 1, &dd, &ndd) == kCGErrorSuccess && ndd ? CGDisplayBounds(dd) : CGRectInfinite;
    for (NSDictionary *w in ws) {
        CGRect b;
        if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)w[(id)kCGWindowBounds], &b)) continue;
        if (!CGRectContainsPoint(b, p)) continue;
        if ([w[(id)kCGWindowOwnerPID] intValue] == getpid() || [w[(id)kCGWindowAlpha] doubleValue] <= 0.01) continue;
        int layer = [w[(id)kCGWindowLayer] intValue];
        if (layer != 0 && b.size.width >= tela0.size.width && b.size.height >= tela0.size.height - 40) continue;  // e.g. Dock's full-screen window
        if (layer != 0) { r.motivo = [NSString stringWithFormat:@"covered by layer-%d window of %@", layer, w[(id)kCGWindowOwnerName]]; return r; }
        alvo = w; wb = b; break;
    }
    if (!alvo) { r.motivo = @"no window under the point"; return r; }
    r.pid = [alvo[(id)kCGWindowOwnerPID] intValue];
    r.janela = [NSString stringWithFormat:@"%@ \"%@\" (%.0f,%.0f %.0fx%.0f)", alvo[(id)kCGWindowOwnerName], alvo[(id)kCGWindowName] ?: @"",
                wb.origin.x, wb.origin.y, wb.size.width, wb.size.height];
    if (p.y - wb.origin.y > IMA_BAND) { r.motivo = @"below the title-bar band"; return r; }
    // 2. its AX window (same frame) and buttons
    AXUIElementRef app = AXUIElementCreateApplication(r.pid);
    AXUIElementSetMessagingTimeout(app, 0.25);
    CFTypeRef lista = axAttr(app, kAXWindowsAttribute);
    AXUIElementRef win = NULL;
    if (lista && CFGetTypeID(lista) == CFArrayGetTypeID())
        for (id w in (__bridge NSArray *)lista) {
            CGRect f;
            if (axFrame((__bridge AXUIElementRef)w, &f) && fabs(f.origin.x - wb.origin.x) < 2 && fabs(f.origin.y - wb.origin.y) < 2 &&
                fabs(f.size.width - wb.size.width) < 2 && fabs(f.size.height - wb.size.height) < 2) { win = (AXUIElementRef)CFRetain((__bridge CFTypeRef)w); break; }
        }
    if (lista) CFRelease(lista);
    if (!win) { r.motivo = @"no matching AX window"; CFRelease(app); return r; }
    CFTypeRef fs = axAttr(win, CFSTR("AXFullScreen"));
    if (fs && CFBooleanGetValue(fs)) r.motivo = @"window is full screen";
    if (fs) CFRelease(fs);
    CFStringRef attrs[3] = { kAXCloseButtonAttribute, kAXMinimizeButtonAttribute, kAXZoomButtonAttribute };
    NSString *nomes[3] = { @"close", @"minimize", @"zoom" };
    AXUIElementRef bts[3] = { NULL }; double melhor = IMA_R; int iBest = -1;
    for (int i = 0; i < 3 && !r.motivo; i++) {
        CFTypeRef b = axAttr(win, attrs[i]);
        if (!b || CFGetTypeID(b) != AXUIElementGetTypeID()) { if (b) CFRelease(b); continue; }
        bts[i] = (AXUIElementRef)b;
        CGRect f; if (!axFrame(bts[i], &f) || f.size.width < 1 || !CGRectContainsRect(CGRectInset(wb, -1, -1), f)) continue;
        CFTypeRef en = axAttr(bts[i], kAXEnabledAttribute);
        BOOL ligado = !en || CFBooleanGetValue(en); if (en) CFRelease(en);
        if (!ligado) continue;
        double d = dist(p, CGPointMake(CGRectGetMidX(f), CGRectGetMidY(f)));
        if (d <= melhor) { melhor = d; iBest = i; r.frame = f; }
    }
    if (!r.motivo && iBest < 0) r.motivo = [NSString stringWithFormat:@"no enabled button within %.0f pt", IMA_R];
    // 3. a tap that lands on some other control (e.g. a toolbar button next to the lights) is left alone
    if (!r.motivo) {
        AXUIElementRef el = NULL;
        if (AXUIElementCopyElementAtPosition(app, p.x, p.y, &el) == kAXErrorSuccess && el) {
            BOOL ehBotao = NO;
            for (int i = 0; i < 3; i++) if (bts[i] && CFEqual(el, bts[i])) ehBotao = YES;
            NSString *role = axStr(el, kAXRoleAttribute);
            r.hit = [NSString stringWithFormat:@"%@/%@", role, axStr(el, kAXSubroleAttribute)];
            NSSet *controles = [NSSet setWithArray:@[@"AXButton", @"AXCheckBox", @"AXRadioButton", @"AXPopUpButton", @"AXMenuButton",
                @"AXTextField", @"AXTextArea", @"AXComboBox", @"AXSlider", @"AXSegmentedControl", @"AXTabGroup", @"AXLink",
                @"AXIncrementor", @"AXDisclosureTriangle", @"AXSearchField", @"AXMenuBarItem"]];
            if (!ehBotao && role && [controles containsObject:role])
                r.motivo = [NSString stringWithFormat:@"tap is on another control (%@ \"%@\")", role,
                            axStr(el, kAXDescriptionAttribute) ?: axStr(el, kAXTitleAttribute) ?: @""];
            CFRelease(el);
        }
    }
    if (!r.motivo) {
        r.ok = YES; r.botao = (AXUIElementRef)CFRetain(bts[iBest]); r.nome = nomes[iBest];
        r.centro = CGPointMake(CGRectGetMidX(r.frame), CGRectGetMidY(r.frame));
        r.motivo = [NSString stringWithFormat:@"%.1f pt from the %@ button", melhor, r.nome];
    }
    for (int i = 0; i < 3; i++) if (bts[i]) CFRelease(bts[i]);
    CFRelease(win); CFRelease(app);
    return r;
}

static dispatch_queue_t filaIma;

// tap = click, unless the magnet redirects it to a traffic-light button. Order of taps is kept (serial queue -> main).
static void toque1(CGPoint p) {
    if (!opt(@"magnet") || !AXIsProcessTrusted()) { clique(p); return; }
    dispatch_async(filaIma, ^{
        double t = agora();
        Ima r = imaProcurar(p);
        double ms = (agora() - t) * 1000;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!r.ok) { clique(p); return; }
            AXError e = AXUIElementPerformAction(r.botao, kAXPressAction);
            if (e != kAXErrorSuccess) { mouse(kCGEventLeftMouseDown, r.centro, kCGMouseButtonLeft, 1); mouse(kCGEventLeftMouseUp, r.centro, kCGMouseButtonLeft, 1); }
            ultClickT = 0;  // a following tap is not a double click
            onda(r.centro, 2);
            xlog(@"magnet: tap (%.0f,%.0f) -> %@ of %@ (%@; lookup %.0f ms; %@)", p.x, p.y, r.nome, r.janela, r.motivo, ms,
                 e == kAXErrorSuccess ? @"AXPress" : [NSString stringWithFormat:@"AXPress err %d, clicked", e]);
            CFRelease(r.botao);
        });
    });
}

static int testeIma(double x, double y) {  // xe-toque --test-magnet x y: report only, never clicks
    printf("accessibility=%d\n", AXIsProcessTrusted());
    double t = agora();
    Ima r = imaProcurar(CGPointMake(x, y));
    NSString *s = [NSString stringWithFormat:@"test-magnet (%.0f,%.0f): %@ | window: %@ | %@ | hit %@ | %.0f ms", x, y,
                   r.ok ? [NSString stringWithFormat:@"-> %@ button at (%.0f,%.0f)", r.nome, r.centro.x, r.centro.y] : @"no redirect (plain click)",
                   r.janela ?: @"-", r.motivo, r.hit ?: @"-", (agora() - t) * 1000];
    printf("%s\n", s.UTF8String);
    xlog(@"%@", s);
    if (r.botao) CFRelease(r.botao);
    return r.ok ? 0 : 1;
}

static double espalhamento(Contato *c, int n, CGPoint m) {
    double s = 0; for (int i = 0; i < n; i++) s += dist(c[i].p, m); return n ? s / n : 0;
}

static void entrarMulti(Contato *c, int n, CGPoint m, double t) {
    est = MULTI; mN = n; mC0 = m; mS0 = espalhamento(c, n, m); mT0 = t; mScroll = NO; mFired = NO; pUlt = m; nam = 0;
    gDropMoves = 1;
}

static void soltar(double t) {  // all fingers lifted
    switch (est) {
        case PEND: if (maxN == 1 && opt(@"scroll1")) toque1(p0); break;
        case EDGER: case EDGEB: if (maxN == 1) toque1(p0); break;
        case SCROLL: scrollFim(YES, t); break;
        case HOLD: cliqueDireito(p0); break;
        case DRAG: mouse(kCGEventLeftMouseUp, pUlt, kCGMouseButtonLeft, 1); break;
        case MULTI: if (mScroll) scrollFim(YES, t); lupaEsconder(); break;
    }
    if (claimed) gClaimUntil = t + 0.3;
    setClaim(NO); gDropMoves = 0;
    est = IDLE;
}

static void bordaFalhou(void) {  // an edge touch that was not an edge swipe
    if (opt(@"scroll1")) est = PEND;           // re-evaluated as a normal touch on the next frame
    else { est = DONE; gDropMoves = 1; }
}

static void motor(Contato *c, int n, double t) {
    if (n == 0) { if (est != IDLE) soltar(t); return; }
    CGPoint m = CGPointZero;
    for (int i = 0; i < n; i++) { m.x += c[i].p.x / n; m.y += c[i].p.y / n; }

    if (est == IDLE) {
        pararMomentum();
        maxN = n; p0 = pUlt = m; t0g = t; nam = 0;
        if (n >= 2) { setClaim(gOwnMulti); entrarMulti(c, n, m, t); return; }
        if (opt(@"edges") && m.x >= CGRectGetMaxX(B) - EDGE) est = EDGER;
        else if (opt(@"edges") && m.y >= CGRectGetMaxY(B) - EDGE) est = EDGEB;
        else est = PEND;
        setClaim(opt(@"scroll1") || est != PEND);
        return;
    }
    if (n > maxN) maxN = n;
    if (n >= 2 && est != MULTI && est != DONE) {
        if (est == SCROLL) scrollFim(NO, t);
        if (est == DRAG) mouse(kCGEventLeftMouseUp, pUlt, kCGMouseButtonLeft, 1);
        if (est == NATIVE) { pUlt = m; entrarMulti(c, n, m, t); gDropMoves = 0; return; }
        setClaim(YES);
        entrarMulti(c, n, m, t);
        return;
    }

    double d = dist(m, p0);
    switch (est) {
        case PEND:
            if (d > MOVE) {
                if (opt(@"scroll1")) { est = SCROLL; gDropMoves = 1; nam = 0; rolar(m.x - p0.x, m.y - p0.y, 1, 0); amostra(m, t); }
                else { est = NATIVE; setClaim(NO); }
            } else if (t - t0g >= HOLD_T) {
                if (opt(@"scroll1")) { est = HOLD; gDropMoves = 1; onda(p0, 1); }
                else { est = NATIVE; setClaim(NO); }
            }
            break;
        case SCROLL:
            if (m.x != pUlt.x || m.y != pUlt.y) rolar(m.x - pUlt.x, m.y - pUlt.y, 2, 0);
            amostra(m, t);
            break;
        case HOLD:
            if (d > MOVE) {
                est = DRAG;
                mouse(kCGEventLeftMouseDown, p0, kCGMouseButtonLeft, 1);
                mouse(kCGEventLeftMouseDragged, m, kCGMouseButtonLeft, 1);
            }
            break;
        case DRAG:
            if (dist(m, pUlt) > 0.3) mouse(kCGEventLeftMouseDragged, m, kCGMouseButtonLeft, 1);
            break;
        case EDGER: {
            double dx = p0.x - m.x, dy = m.y - p0.y;
            if (dx > EDGE_DIST && fabs(dy) < dx * 0.7) { centralNotificacoes(); onda(m, 1); est = DONE; gDropMoves = 1; }
            else if (m.x - p0.x > MOVE || fabs(dy) > 40 || t - t0g > 0.8) bordaFalhou();
            break;
        }
        case EDGEB: {
            double dy = p0.y - m.y, dx = m.x - p0.x;
            if (dy > EDGE_DIST && fabs(dx) < dy * 0.7) { missionControl(); onda(m, 1); est = DONE; gDropMoves = 1; }
            else if (fabs(dx) > 40 || t - t0g > 0.8) bordaFalhou();
            break;
        }
        case MULTI: {
            if (n != mN) {  // finger count changed: start over from here
                if (mScroll) { scrollFim(NO, t); mScroll = NO; }
                if (n < 2) lupaEsconder();
                mN = n; mC0 = m; mS0 = espalhamento(c, n, m); mT0 = t;
                break;
            }
            if (opt(@"lupa") && n == 2) {
                if (!lupaOn && !mScroll && !mFired && dist(m, mC0) < 12 && fabs(espalhamento(c, n, m) - mS0) < 12 && t - mT0 > 0.6) lupaMostrar(m);
                else if (lupaOn) { lupaC = m; lupaPedir(); }
            }
            if (lupaOn || !gOwnMulti || mFired) break;
            if (n == 2) {
                if (!mScroll && dist(m, mC0) > MOVE) { mScroll = YES; nam = 0; rolar(m.x - mC0.x, m.y - mC0.y, 1, 0); }
                else if (mScroll && (m.x != pUlt.x || m.y != pUlt.y)) rolar(m.x - pUlt.x, m.y - pUlt.y, 2, 0);
                if (mScroll) amostra(m, t);
            } else if (n == 3) {
                double dx = m.x - mC0.x, dy = m.y - mC0.y;
                if (fabs(dx) > 100 && fabs(dx) > 2 * fabs(dy)) { trocarEspaco(dx < 0 ? +1 : -1); mFired = YES; }
            } else if (n >= 5) {
                double s = espalhamento(c, n, m);
                if (mS0 > 20 && s / mS0 < 0.6) { appsLaunchpad(); onda(m, 1); mFired = YES; }
            }
            break;
        }
    }
    pUlt = m;
}

// ---------------------------------------------------------------- HID input

static NSMutableSet<NSNumber *> *idsAntes;

static int contatos(Contato *out) {
    if (agora() - gLastHID > 0.15) return 0;  // watchdog: the panel reports continuously while touched
    Slot *ord[16]; int k = 0;
    for (int i = 0; i < nslots; i++) ord[k++] = &slots[i];
    for (int i = 1; i < k; i++) for (int j = i; j > 0 && ord[j]->cookie < ord[j-1]->cookie; j--) { Slot *s = ord[j]; ord[j] = ord[j-1]; ord[j-1] = s; }
    int n = 0;
    for (int i = 0; i < k; i++) {
        Slot *s = ord[i];
        if (ccHID >= 0 && i >= ccHID) break;  // only the first ContactCount collections are valid
        if (!s->tip || !s->temX || !s->temY) continue;
        out[n].id = s->id >= 0 ? s->id : i;
        out[n].p = CGPointMake(B.origin.x + s->x / xmax * B.size.width, B.origin.y + s->y / ymax * B.size.height);
        n++;
    }
    return n;
}

static void penPasso(Contato *c, int n) {
    NSMutableSet *vivos = [NSMutableSet set];
    for (int i = 0; i < n; i++) {
        NSNumber *k = @(c[i].id); [vivos addObject:k];
        CGPoint p = local(c[i].p);
        NSBezierPath *b = tracoAtual[k];
        if (!b) { b = [NSBezierPath bezierPath]; b.lineWidth = 4; b.lineCapStyle = NSLineCapStyleRound; b.lineJoinStyle = NSLineJoinStyleRound;
                  [b moveToPoint:p]; [b lineToPoint:p]; tracoAtual[k] = b; }
        else if (dist(b.currentPoint, p) > 0.5) [b lineToPoint:p];
    }
    for (NSNumber *k in tracoAtual.allKeys) if (![vivos containsObject:k]) { [tracos addObject:tracoAtual[k]]; [tracoAtual removeObjectForKey:k]; }
}

static double ultimoDesenho;
static void tick(void) {
    double t = agora();
    Contato c[16]; int n = contatos(c);
    NSMutableSet *ids = [NSMutableSet set];
    for (int i = 0; i < n; i++) {
        [ids addObject:@(c[i].id)];
        if (![idsAntes containsObject:@(c[i].id)] && !gPen) onda(c[i].p, 0);
    }
    BOOL desenhar = ondas.length > 0 || lupaOn;
    // trail
    if (opt(@"trail") && !gPen) {
        for (int f = 0; f < MAXF; f++) {
            NSMutableData *d = rastros[f];
            Ponto p = { CGPointMake(-1, -1), t };
            for (int i = 0; i < n; i++) if (c[i].id % MAXF == f) p.p = local(c[i].p);
            Ponto *ult = d.length ? (Ponto *)d.mutableBytes + d.length / sizeof(Ponto) - 1 : NULL;
            if (p.p.x < 0 && (!ult || ult->p.x < 0)) { if (d.length) desenhar = YES; continue; }  // already lifted
            [d appendBytes:&p length:sizeof p]; desenhar = YES;
        }
    }
    if (gPen) { if (n || tracoAtual.count) { penPasso(c, n); desenhar = YES; } }
    else motor(c, n, t);
    passoMomentum();
    idsAntes = ids;
    if (desenhar || t - ultimoDesenho < 0.8) { tela.needsDisplay = YES; if (desenhar) ultimoDesenho = t; }
}

static Slot *slotDe(IOHIDElementRef el) {
    IOHIDElementRef col = IOHIDElementGetParent(el);
    for (int i = 0; i < nslots; i++) if (slots[i].col == col) return &slots[i];
    if (nslots == 16) return NULL;
    slots[nslots] = (Slot){ col, (uint32_t)IOHIDElementGetCookie(col), 0, -1, 0, 0, NO, NO };
    return &slots[nslots++];
}

static void valor(void *ctx, IOReturn r, void *snd, IOHIDValueRef v) {
    gLastHID = agora();
    IOHIDElementRef el = IOHIDValueGetElement(v);
    uint32_t pg = IOHIDElementGetUsagePage(el), us = IOHIDElementGetUsage(el);
    CFIndex iv = IOHIDValueGetIntegerValue(v);
    if (pg == kHIDPage_Digitizer && us == kHIDUsage_Dig_ContactCount) { ccHID = (int)iv; return; }
    if (pg == kHIDPage_Digitizer && us == 0x56) return;  // scan time (every report: used only as heartbeat above)
    Slot *s = slotDe(el); if (!s) return;
    if (pg == kHIDPage_Digitizer && us == kHIDUsage_Dig_TipSwitch) s->tip = (int)iv;
    else if (pg == kHIDPage_Digitizer && us == kHIDUsage_Dig_ContactIdentifier) s->id = (int)iv;
    else if (pg == kHIDPage_GenericDesktop && us == kHIDUsage_GD_X) {
        s->x = iv; s->temX = YES; double m = IOHIDElementGetLogicalMax(el); if (m > 1) xmax = m;
    } else if (pg == kHIDPage_GenericDesktop && us == kHIDUsage_GD_Y) {
        s->y = iv; s->temY = YES; double m = IOHIDElementGetLogicalMax(el); if (m > 1) ymax = m;
    }
}

// ---------------------------------------------------------------- event tap (own thread)

static BOOL ehDescendente(uint64_t s) {
    int n = gNTSDesc;
    for (int i = 0; i < n; i++) if (gTSDesc[i] == s) return YES;
    return NO;
}

static void sairPen(void);

static CGEventRef cbTap(CGEventTapProxy p, CGEventType type, CGEventRef e, void *ctx) {
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) { CGEventTapEnable(tapRef, true); return e; }
    if (CGEventGetIntegerValueField(e, kCGEventSourceUserData) == MAGIC) return e;
    if (type == kCGEventKeyDown) {
        if (gPen && CGEventGetIntegerValueField(e, kCGKeyboardEventKeycode) == 53) {  // Esc leaves pen mode
            dispatch_async(dispatch_get_main_queue(), ^{ sairPen(); });
            return NULL;
        }
        return e;
    }
    if (type == kCGEventKeyUp || type == kCGEventFlagsChanged) return e;
    uint64_t s = (uint64_t)CGEventGetIntegerValueField(e, F_SENDER);
    if (!s || !gTS) return e;
    BOOL ts = s == gTS, mt = !ts && ehDescendente(s);
    if (!ts && !mt) return e;                       // trackpad, mouse, keyboard, synthetic: never touched
    double t = agora();
    if (t - gLastHID > 0.6 && t > gClaimUntil) return e;  // fail open when the touchscreen is idle
    if (gPen) return NULL;
    if (mt) return gOwnMulti ? NULL : e;
    if (gClaim || t < gClaimUntil) {
        switch ((int)type) {
            case kCGEventLeftMouseDown: case kCGEventLeftMouseUp: case kCGEventLeftMouseDragged:
            case kCGEventRightMouseDown: case kCGEventRightMouseUp: case kCGEventRightMouseDragged:
            case kCGEventOtherMouseDown: case kCGEventOtherMouseUp: case kCGEventOtherMouseDragged:
            case 14:  // NX_SYSDEFINED aux-button events that accompany the clicks
                return NULL;
            case kCGEventMouseMoved:
                if (gDropMoves) return NULL;
        }
    }
    return e;
}

static void lerIDs(void) {
    io_iterator_t it = 0;
    gTS = 0; gNTSDesc = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("VoodooI2CTouchscreenHIDEventDriver"), &it)) return;
    io_object_t drv = IOIteratorNext(it);
    IOObjectRelease(it);
    if (!drv) { xlog(@"VoodooI2CTouchscreenHIDEventDriver not found: suppression disabled"); return; }
    uint64_t id = 0; IORegistryEntryGetRegistryEntryID(drv, &id);
    io_iterator_t sub = 0; int n = 0;
    if (!IORegistryEntryCreateIterator(drv, kIOServicePlane, kIORegistryIterateRecursively, &sub)) {
        io_object_t o;
        while ((o = IOIteratorNext(sub))) {
            uint64_t d = 0; IORegistryEntryGetRegistryEntryID(o, &d);
            if (n < 64) gTSDesc[n++] = d;
            IOObjectRelease(o);
        }
        IOObjectRelease(sub);
    }
    IOObjectRelease(drv);
    gNTSDesc = n; gTS = id;
    xlog(@"touchscreen driver 0x%llx, %d descendants", id, n);
}

static void threadTap(void) {
    [NSThread detachNewThreadWithBlock:^{
        BOOL avisou = NO;
        for (;;) {
            tapRef = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, kCGEventMaskForAllEvents, cbTap, NULL);
            if (tapRef) break;
            if (!avisou) { xlog(@"event tap failed: grant Accessibility (and Input Monitoring) to xe-toque; retrying every 3 s"); avisou = YES; }
            [NSThread sleepForTimeInterval:3];
        }
        gTapOK = 1;
        xlog(@"event tap active");
        CFRunLoopAddSource(CFRunLoopGetCurrent(), CFMachPortCreateRunLoopSource(NULL, tapRef, 0), kCFRunLoopCommonModes);
        CGEventTapEnable(tapRef, true);
        CFRunLoopRun();
    }];
}

// ---------------------------------------------------------------- touch mode (UI scale through xe-res)

static NSString *xeResPath(void) { return [[ud stringForKey:@"xeRes"] stringByExpandingTildeInPath]; }

static NSString *rodarXeRes(NSArray<NSString *> *args) {  // runs xe-res, returns stdout (nil on failure)
    NSString *path = xeResPath();
    if (![NSFileManager.defaultManager isExecutableFileAtPath:path]) return nil;
    NSTask *tk = [NSTask new]; tk.executableURL = [NSURL fileURLWithPath:path]; tk.arguments = args;
    NSPipe *pp = [NSPipe pipe]; tk.standardOutput = pp; tk.standardError = [NSPipe pipe];
    if (![tk launchAndReturnError:nil]) return nil;
    NSData *d = [pp.fileHandleForReading readDataToEndOfFile];
    [tk waitUntilExit];
    if (tk.terminationStatus) return nil;
    return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
}

static NSArray<NSDictionary *> *modos(void) {  // parse `xe-res` listing
    NSString *o = rodarXeRes(@[]);
    if (!o) return nil;
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"^([* ])\\s*(\\d+):\\s+(\\d+) x (\\d+)\\s.*?(HiDPI)?\\s+[\\d.]+ Hz"
                                                                        options:NSRegularExpressionAnchorsMatchLines error:nil];
    NSMutableArray *r = [NSMutableArray array];
    for (NSTextCheckingResult *m in [re matchesInString:o options:0 range:NSMakeRange(0, o.length)]) {
        [r addObject:@{@"cur": @([[o substringWithRange:[m rangeAtIndex:1]] isEqual:@"*"]),
                       @"idx": @([o substringWithRange:[m rangeAtIndex:2]].integerValue),
                       @"w": @([o substringWithRange:[m rangeAtIndex:3]].integerValue),
                       @"h": @([o substringWithRange:[m rangeAtIndex:4]].integerValue),
                       @"hidpi": @([m rangeAtIndex:5].location != NSNotFound)}];
    }
    return r;
}

static void atualizarMenu(void);

static void modoToque(BOOL ligar) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *erro = nil;
        NSArray *ms = modos();
        if (!ms.count) erro = [NSString stringWithFormat:@"xe-res not found or failed (%@)", xeResPath()];
        else if (ligar) {
            NSDictionary *cur = nil;
            for (NSDictionary *m in ms) if ([m[@"cur"] boolValue]) { cur = m; break; }
            long alvo = MIN(MAX([ud integerForKey:@"touchWidth"], 640), 3000);
            if (!cur) erro = @"current mode not found in xe-res listing";
            else if ([cur[@"w"] longValue] <= alvo) erro = nil;  // already as large as touch mode: nothing to do
            else {
                [ud setObject:cur forKey:@"prevMode"];
                if (!rodarXeRes(@[[NSString stringWithFormat:@"w%ld", alvo]])) erro = @"xe-res w<width> failed";
            }
        } else {
            NSDictionary *prev = [ud dictionaryForKey:@"prevMode"];
            if (prev) {
                NSDictionary *alvo = nil;
                for (NSDictionary *m in ms) if ([m[@"idx"] isEqual:prev[@"idx"]] && [m[@"w"] isEqual:prev[@"w"]] &&
                                                [m[@"h"] isEqual:prev[@"h"]] && [m[@"hidpi"] isEqual:prev[@"hidpi"]]) alvo = m;
                if (!alvo) for (NSDictionary *m in ms) if ([m[@"w"] isEqual:prev[@"w"]] && [m[@"h"] isEqual:prev[@"h"]] &&
                                                         [m[@"hidpi"] isEqual:prev[@"hidpi"]]) { alvo = m; break; }
                if (!alvo) erro = @"previous mode no longer listed";
                else if (!rodarXeRes(@[[NSString stringWithFormat:@"%ld", [alvo[@"idx"] longValue]]])) erro = @"xe-res <mode> failed";
                if (!erro) [ud removeObjectForKey:@"prevMode"];
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (erro) xlog(@"touch mode: %@", erro);
            else [ud setBool:ligar forKey:@"touchMode"];
            atualizarMenu();
        });
    });
}

// ---------------------------------------------------------------- menu bar

@interface Ctl : NSObject <NSMenuDelegate>
@end
static Ctl *ctl;
static NSStatusItem *item;

static void sairPen(void) { gPen = 0; penLimpar(); atualizarMenu(); }

static void geometria(void) {
    NSScreen *sc = nil;
    for (NSScreen *s in NSScreen.screens) {
        CGDirectDisplayID d = [s.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
        if (CGDisplayIsBuiltin(d)) { sc = s; gDisp = d; }
    }
    if (!sc) { sc = NSScreen.screens.firstObject; gDisp = [sc.deviceDescription[@"NSScreenNumber"] unsignedIntValue]; }
    B = CGDisplayBounds(gDisp);
    [janela setFrame:sc.frame display:YES];
    tela.frame = (NSRect){NSZeroPoint, sc.frame.size};
    lupaFiltro = nil;
}

@implementation Ctl
- (void)alternar:(NSMenuItem *)mi {
    NSString *k = mi.representedObject;
    [ud setBool:![ud boolForKey:k] forKey:k];
    gOwnMulti = [ud boolForKey:@"ownMulti"];
    if ([k isEqual:@"trail"] && !opt(@"trail")) for (NSMutableData *d in rastros) d.length = 0;
    if ([k isEqual:@"lupa"] && opt(@"lupa") && !CGPreflightScreenCaptureAccess()) CGRequestScreenCaptureAccess();
    atualizarMenu();
}
- (void)toque:(NSMenuItem *)mi { modoToque(!opt(@"touchMode")); }
- (void)pen:(NSMenuItem *)mi {
    if (gPen) { sairPen(); return; }
    if (est != IDLE) soltar(agora());
    pararMomentum();
    gPen = 1; tela.needsDisplay = YES; atualizarMenu();
}
- (void)limpar:(NSMenuItem *)mi { penLimpar(); }
- (void)ajustes:(NSMenuItem *)mi {
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:[NSString stringWithFormat:
        @"x-apple.systempreferences:com.apple.preference.security?%@", mi.representedObject]]];
}
- (void)menuNeedsUpdate:(NSMenu *)m { atualizarMenu(); }
@end

static NSMenuItem *add(NSMenu *m, NSString *t, SEL s, id obj, NSString *key) {
    NSMenuItem *mi = [m addItemWithTitle:t action:s keyEquivalent:key ?: @""];
    mi.target = ctl; mi.representedObject = obj;
    return mi;
}

static void atualizarMenu(void) {
    NSMenu *m = item.menu;
    [m removeAllItems];
    NSArray *ts = @[@[@"Trail", @"trail"], @[@"Ripple at touch", @"ripple"], @[@"One-finger scroll (tap/hold/drag)", @"scroll1"],
                    @[@"Edge swipes (right: Notifications, bottom: Mission Control)", @"edges"],
                    @[@"xe-toque handles 2–5 fingers (else macOS)", @"ownMulti"],
                    @[@"Magnifier (two-finger hold)", @"lupa"],
                    @[@"Window buttons magnet (taps near close/minimize/zoom)", @"magnet"]];
    for (NSArray *a in ts) add(m, a[0], @selector(alternar:), a[1], nil).state = opt(a[1]) ? NSControlStateValueOn : NSControlStateValueOff;
    [m addItem:NSMenuItem.separatorItem];
    NSString *tt = [NSString stringWithFormat:@"Touch mode (larger UI, %ld pt wide)", (long)[ud integerForKey:@"touchWidth"]];
    NSMenuItem *mi = add(m, tt, @selector(toque:), nil, nil);
    mi.state = opt(@"touchMode") ? NSControlStateValueOn : NSControlStateValueOff;
    if (![NSFileManager.defaultManager isExecutableFileAtPath:xeResPath()]) { mi.enabled = NO; mi.action = nil; }
    [m addItem:NSMenuItem.separatorItem];
    add(m, @"Pen mode (Esc exits)", @selector(pen:), nil, @"").state = gPen ? NSControlStateValueOn : NSControlStateValueOff;
    add(m, @"Clear drawing", @selector(limpar:), nil, nil);
    [m addItem:NSMenuItem.separatorItem];
    BOOL ax = AXIsProcessTrusted(), im = CGPreflightListenEventAccess();
    add(m, [NSString stringWithFormat:@"Accessibility: %@", ax ? @"OK" : @"missing — click to open Settings"],
        ax ? nil : @selector(ajustes:), @"Privacy_Accessibility", nil);
    add(m, [NSString stringWithFormat:@"Input Monitoring: %@", im ? @"OK" : @"missing — click to open Settings"],
        im ? nil : @selector(ajustes:), @"Privacy_ListenEvent", nil);
    add(m, [NSString stringWithFormat:@"Event filter: %@", gTapOK ? (gTS ? @"active" : @"active (touchscreen driver not found)") : @"waiting for permission"], nil, nil, nil);
    if (opt(@"lupa")) {
        BOOL sr = CGPreflightScreenCaptureAccess();
        add(m, [NSString stringWithFormat:@"Screen Recording (magnifier): %@", sr ? @"OK" : @"missing"], sr ? nil : @selector(ajustes:), @"Privacy_ScreenCapture", nil);
    }
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"Quit xe-toque" action:@selector(terminate:) keyEquivalent:@"q"];
    item.button.image = [NSImage imageWithSystemSymbolName:gPen ? @"pencil.tip" : @"hand.point.up.left"
                                  accessibilityDescription:@"xe-toque"];
}

// ---------------------------------------------------------------- main

int xeProbeEvents(int argc, const char **argv);

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc > 1 && !strcmp(argv[1], "--probe-events")) return xeProbeEvents(argc, argv);
    if (argc > 3 && !strcmp(argv[1], "--test-magnet")) return testeIma(atof(argv[2]), atof(argv[3]));
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSFileManager.defaultManager createDirectoryAtPath:[@"~/Library/Logs/xe-toque" stringByExpandingTildeInPath]
                            withIntermediateDirectories:YES attributes:nil error:nil];
    ud = NSUserDefaults.standardUserDefaults;
    // default xe-res: tools/build/xe-res, next to tools/xe-toque.app (docs/INSTALL.md step 6)
    NSString *xeResDef = [NSBundle.mainBundle.bundlePath.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"build/xe-res"];
    [ud registerDefaults:@{@"trail": @YES, @"ripple": @YES, @"scroll1": @YES, @"edges": @YES, @"ownMulti": @NO,
                           @"lupa": @NO, @"magnet": @YES, @"touchMode": @NO, @"touchWidth": @1152, @"xeRes": xeResDef}];
    gOwnMulti = opt(@"ownMulti");
    src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    rastros = [NSMutableArray array];
    for (int i = 0; i < MAXF; i++) [rastros addObject:[NSMutableData data]];
    ondas = [NSMutableData data]; tracos = [NSMutableArray array]; tracoAtual = [NSMutableDictionary dictionary];
    idsAntes = [NSMutableSet set];
    filaIma = dispatch_queue_create("xe.toque.magnet", DISPATCH_QUEUE_SERIAL);

    janela = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 100, 100) styleMask:NSWindowStyleMaskBorderless
                                           backing:NSBackingStoreBuffered defer:NO];
    janela.opaque = NO; janela.backgroundColor = NSColor.clearColor; janela.hasShadow = NO;
    janela.ignoresMouseEvents = YES; janela.level = NSScreenSaverWindowLevel;
    janela.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary |
                                NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorIgnoresCycle;
    tela = [[Tela alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    janela.contentView = tela;
    geometria();
    [janela orderFrontRegardless];
    // follow resolution / UI-scale changes (xe-res): resize the overlay to the built-in panel again
    [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil
                                                     queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { geometria(); }];

    ctl = [Ctl new];
    item = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
    item.menu = [NSMenu new]; item.menu.delegate = ctl; item.menu.autoenablesItems = NO;
    atualizarMenu();

    // permissions: ask once (no-ops when already granted)
    if (!CGPreflightListenEventAccess()) CGRequestListenEventAccess();
    if (!AXIsProcessTrusted()) AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)@{(__bridge id)kAXTrustedCheckOptionPrompt: @YES});

    lerIDs();
    threadTap();

    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(m, (__bridge CFDictionaryRef)@{@kIOHIDDeviceUsagePageKey: @(kHIDPage_Digitizer),
                                                                 @kIOHIDDeviceUsageKey: @(kHIDUsage_Dig_TouchScreen)});
    IOHIDManagerRegisterInputValueCallback(m, valor, NULL);
    IOHIDManagerScheduleWithRunLoop(m, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    __block IOReturn r = IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    NSSet *devs = CFBridgingRelease(IOHIDManagerCopyDevices(m));
    xlog(@"start: %lu touchscreen(s), HID open=0x%x, listen=%d, accessibility=%d", (unsigned long)devs.count, r,
         CGPreflightListenEventAccess(), AXIsProcessTrusted());
    if (r) {  // Input Monitoring not granted yet: retry until it is
        [NSTimer scheduledTimerWithTimeInterval:3 repeats:YES block:^(NSTimer *t) {
            r = IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
            if (!r) { xlog(@"HID open ok"); [t invalidate]; }
        }];
    }
    if (opt(@"lupa")) lupaPreparar();

    NSTimer *tm = [NSTimer timerWithTimeInterval:1.0/60 repeats:YES block:^(NSTimer *t) { tick(); }];
    [NSRunLoop.mainRunLoop addTimer:tm forMode:NSRunLoopCommonModes];
    [NSApp run];
} }
