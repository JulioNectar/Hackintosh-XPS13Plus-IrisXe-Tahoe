// xe-pblock: a physical press of the power button locks the screen at once (like a Mac with Touch ID).
// The button is the ACPI device PBTN (PNP0C0C) -> AppleACPIButton, an HID service that sends
// keyboard/consumer page 0x0C, usage 0x30 (Power). The Goodix fingerprint sensor in the button is a separate
// (USB vendor-class) device and does not go through here: touching the sensor does not produce this event.
// Two channels, both event-driven (no polling); the debounce merges them:
//   1) an IOHIDEventSystem monitor client (private API), filtered on the Power page/usage;
//   2) a listen-only CGEventTap on NSSystemDefined (subtype 1 = power key, or subtype 8 with key NX_POWER_KEY).
// Modes: (default) agent | --dry-run (log only, never lock) | --probe N (print everything for N s)
//        | --count N (print on stdout how many presses were seen in N s; used by install.sh) | --check-lock
#import <Cocoa/Cocoa.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <sys/time.h>

// --- IOHIDEventSystemClient (private, exported by IOKit) ---
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
typedef void (*HIDEvtCB)(void *target, void *refcon, IOHIDServiceClientRef sender, IOHIDEventRef ev);
extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef, int type, CFDictionaryRef);
extern void IOHIDEventSystemClientScheduleWithRunLoop(IOHIDEventSystemClientRef, CFRunLoopRef, CFStringRef);
extern void IOHIDEventSystemClientSetMatching(IOHIDEventSystemClientRef, CFDictionaryRef);
extern void IOHIDEventSystemClientRegisterEventCallback(IOHIDEventSystemClientRef, HIDEvtCB, void *, void *);
extern uint32_t IOHIDEventGetType(IOHIDEventRef);
extern CFIndex IOHIDEventGetIntegerValue(IOHIDEventRef, uint32_t field);
extern uint64_t IOHIDEventGetSenderID(IOHIDEventRef);
#define HID_MONITOR   1          // kIOHIDEventSystemClientTypeMonitor
#define EV_KEYBOARD   3          // kIOHIDEventTypeKeyboard
#define F_PAGE  ((EV_KEYBOARD << 16) | 0)
#define F_USAGE ((EV_KEYBOARD << 16) | 1)
#define F_DOWN  ((EV_KEYBOARD << 16) | 2)

#define DEBOUNCE 1.0             // s: presses (and duplicate channels) within this count as one
#define NX_SUBTYPE_POWER_KEY 1
#define NX_SUBTYPE_AUX_CONTROL_BUTTONS 8
#define NX_POWER_KEY 6

enum { MODE_AGENT, MODE_DRY, MODE_PROBE, MODE_COUNT };
static int mode = MODE_AGENT;
static FILE *logf_;
static double lastPress = -1e9;  // time of the last accepted press
static int presses, nTap, nHid;
static uint64_t acpiButtons[8]; static int nButtons;  // registry IDs of the AppleACPIButton (PNP0C0C) services
static CFMachPortRef tap;

static double now(void) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static void LOG(const char *fmt, ...) {
    char stamp[32]; struct timeval tv; gettimeofday(&tv, NULL);
    strftime(stamp, sizeof stamp, "%F %T", localtime(&tv.tv_sec));
    FILE *f = logf_ ? logf_ : stderr;
    va_list ap; va_start(ap, fmt);
    fprintf(f, "%s.%03d ", stamp, (int)(tv.tv_usec / 1000)); vfprintf(f, fmt, ap); fputc('\n', f);
    va_end(ap); fflush(f);
}

static BOOL screenLocked(void) {
    NSDictionary *d = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    return [d[@"CGSSessionScreenIsLocked"] boolValue];
}

// lock: SACLockScreenImmediate from login.framework (the same call as  menu > Lock Screen)
typedef int (*SACLockFn)(void);
static SACLockFn findSAC(void) {
    void *h = dlopen("/System/Library/PrivateFrameworks/login.framework/Versions/Current/login", RTLD_LAZY);
    return h ? (SACLockFn)dlsym(h, "SACLockScreenImmediate") : NULL;
}

static void lockScreen(const char *src) {
    if (screenLocked()) { LOG("press (%s), screen already locked: nothing to do", src); return; }
    if (mode == MODE_DRY) { LOG("press (%s): DRY-RUN, would lock now", src); return; }
    SACLockFn f = findSAC();
    if (f) { int r = f(); LOG("press (%s): SACLockScreenImmediate -> %d", src, r); return; }
    // fallback: display off (only locks if \"require password immediately\" is on)
    LOG("press (%s): SACLockScreenImmediate unavailable, using pmset displaysleepnow", src);
    system("/usr/bin/pmset displaysleepnow");
}

// a press arrived on some channel; the debounce merges down/up and the two channels
static void press(const char *src) {
    double t = now();
    if (t - lastPress < DEBOUNCE) { if (mode == MODE_PROBE) LOG("  (%s ignored: debounce)", src); return; }
    lastPress = t; presses++;
    if (mode == MODE_PROBE || mode == MODE_COUNT) { LOG(">>> PRESS #%d seen via %s", presses, src); return; }
    lockScreen(src);
}

static BOOL isACPIButton(uint64_t id) {
    for (int i = 0; i < nButtons; i++) if (acpiButtons[i] == id) return YES;
    return NO;
}

// --- channel 1: IOHIDEventSystem ---
static void hidEvt(void *tg, void *rc, IOHIDServiceClientRef snd, IOHIDEventRef ev) {
    if (IOHIDEventGetType(ev) != EV_KEYBOARD) return;
    CFIndex pg = IOHIDEventGetIntegerValue(ev, F_PAGE), us = IOHIDEventGetIntegerValue(ev, F_USAGE);
    CFIndex dn = IOHIDEventGetIntegerValue(ev, F_DOWN);
    uint64_t id = IOHIDEventGetSenderID(ev);
    BOOL acpi = isACPIButton(id);
    BOOL power = (pg == 0x0C && us == 0x30) || (pg == 0x01 && us >= 0x81 && us <= 0x83);
    if (mode == MODE_PROBE && (acpi || power))
        LOG("hid: page=0x%lx usage=0x%lx down=%ld sender=0x%llx%s", (long)pg, (long)us, (long)dn, id, acpi ? " [AppleACPIButton]" : "");
    if (power && dn) { nHid++; press("hid"); }
}

// --- channel 2: listen-only CGEventTap on NSSystemDefined ---
static CGEventRef tapCB(CGEventTapProxy p, CGEventType type, CGEventRef ev, void *rc) {
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        LOG("tap disabled by the system (%d), re-enabling", type); CGEventTapEnable(tap, true); return ev;
    }
    if ((int)type != (int)NSEventTypeSystemDefined) return ev;
    NSEvent *e = [NSEvent eventWithCGEvent:ev];
    long st = e.subtype, d1 = e.data1;
    int key = (int)((d1 & 0xFFFF0000) >> 16), state = (int)((d1 & 0xFF00) >> 8);
    if (mode == MODE_PROBE) LOG("tap: systemDefined subtype=%ld data1=0x%lx data2=0x%lx", st, d1, (long)e.data2);
    BOOL power = st == NX_SUBTYPE_POWER_KEY ||
                 (st == NX_SUBTYPE_AUX_CONTROL_BUTTONS && key == NX_POWER_KEY && state == 0x0A);
    if (power) { nTap++; press("tap"); }
    return ev;
}

// --- probe only: IOPMrootDomain messages (to see whether the press also arrives there) ---
static void rootMsg(void *rc, io_service_t s, natural_t type, void *arg) {
    LOG("IOPMrootDomain: message 0x%x", type);
}

static void findButtons(void) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AppleACPIButton"), &it)) return;
    io_service_t s;
    while ((s = IOIteratorNext(it)) && nButtons < 8) {
        uint64_t id = 0; IORegistryEntryGetRegistryEntryID(s, &id);
        NSString *nm = CFBridgingRelease(IORegistryEntryCreateCFProperty(s, CFSTR("IONameMatched"), 0, 0));
        if (!nm || [nm isEqual:@"PNP0C0C"] || [nm isEqual:@"power-button"]) acpiButtons[nButtons++] = id;
        LOG("AppleACPIButton 0x%llx (%s)", id, nm.UTF8String ?: "?");
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
}

static int usage(void) {
    fprintf(stderr, "usage: xe-pblock [--dry-run | --probe SECS | --count SECS | --check-lock]\n");
    return 2;
}

int main(int argc, char **argv) { @autoreleasepool {
    double dur = 0;
    if (argc > 1) {
        if (!strcmp(argv[1], "--dry-run")) mode = MODE_DRY;
        else if (!strcmp(argv[1], "--probe") && argc > 2) { mode = MODE_PROBE; dur = atof(argv[2]); }
        else if (!strcmp(argv[1], "--count") && argc > 2) { mode = MODE_COUNT; dur = atof(argv[2]); }
        else if (!strcmp(argv[1], "--check-lock")) {  // never locks: only checks that the symbol exists
            SACLockFn f = findSAC();
            printf("SACLockScreenImmediate: %s; screen locked now: %s\n", f ? "OK" : "MISSING", screenLocked() ? "yes" : "no");
            return f ? 0 : 1;
        } else return usage();
    }
    if (mode == MODE_AGENT || mode == MODE_DRY) {
        NSString *p = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/xe-pblock.log"];
        logf_ = fopen(p.fileSystemRepresentation, "a");
    }
    LOG("xe-pblock start (mode %d, pid %d)", mode, getpid());
    findButtons();

    // channel 1
    IOHIDEventSystemClientRef cli = IOHIDEventSystemClientCreateWithType(kCFAllocatorDefault, HID_MONITOR, NULL);
    if (cli) {
        // agent: only the button service (consumer 0x0C/0x01 = AppleACPIButton), so trackpad/keyboard never wake it
        if (mode != MODE_PROBE)
            IOHIDEventSystemClientSetMatching(cli, (__bridge CFDictionaryRef)@{@"PrimaryUsagePage": @12, @"PrimaryUsage": @1});
        IOHIDEventSystemClientRegisterEventCallback(cli, hidEvt, NULL, NULL);
        IOHIDEventSystemClientScheduleWithRunLoop(cli, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    }
    LOG("hid channel: %s", cli ? "active" : "FAILED");

    // channel 2
    BOOL canListen = CGPreflightListenEventAccess();
    if (!canListen && mode == MODE_AGENT) CGRequestListenEventAccess();  // adds the app to the Input Monitoring list
    tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
                           CGEventMaskBit(NSEventTypeSystemDefined), tapCB, NULL);
    if (tap) {
        CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
        CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopCommonModes);
        CGEventTapEnable(tap, true);
    }
    LOG("tap channel: %s; Input Monitoring: %s; SACLockScreenImmediate: %s", tap ? "active" : "FAILED",
        canListen ? "yes" : "no", findSAC() ? "ok" : "MISSING");

    if (mode == MODE_PROBE) {
        IONotificationPortRef np = IONotificationPortCreate(kIOMainPortDefault);
        io_service_t rd = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));
        io_object_t n;
        if (rd && !IOServiceAddInterestNotification(np, rd, kIOGeneralInterest, rootMsg, NULL, &n)) {
            CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(np), kCFRunLoopDefaultMode);
            LOG("rootDomain channel (diagnostic only): active");
        }
        LOG("probe for %.0f s: press the button and/or touch the sensor; Ctrl-C quits", dur);
    }
    if (!cli && !tap) { LOG("no channel active, exiting"); return 1; }

    if (dur > 0) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, dur, false);
        if (mode == MODE_PROBE) LOG("end: %d press(es); power events: hid=%d tap=%d", presses, nHid, nTap);
        if (mode == MODE_COUNT) {
            fprintf(stderr, "power events: hid=%d tap=%d\n", nHid, nTap);
            printf("%d %d %d\n", presses, nHid, nTap);
        }
        return 0;
    }
    CFRunLoopRun();
    return 0;
} }
