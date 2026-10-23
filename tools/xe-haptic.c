// xe-haptic: haptic click intensity of the Elan 04F3:31D1 piezo touchpad (Dell XPS 13 Plus 9320)
// through VoodooI2CHID's IOHIDDevice getReport/setReport (I2C-HID GET_REPORT/SET_REPORT, feature type).
//
// Usage: xe-haptic [get]          -> intensity (Haptics 0x0E:0x23, feature report 0x09 byte 0). On this firmware GET 0x09
//                                    comes back empty (write-only), so it prints "unknown" and exits 3
//        xe-haptic list           -> every feature report of the device with its current bytes (read-only)
//        xe-haptic set N [B]      -> report 0x09 = {N, B}: N = intensity 0..100, B = the second byte (usage 0x0E:0x01,
//                                    0..100, meaning unknown). B is required while GET 0x09 is unreadable
//        xe-haptic test [N] [s]   -> SET N, read back, wait s s, restore the original (only if GET 0x09 works)
//        xe-haptic dump ID        -> full hex/ASCII of one feature report (read-only), e.g. dump 0x0a
//        xe-haptic elements       -> the feature/output elements (usage, report ID, size, range) as macOS parsed them
// A reboot (HID RESET + power-up in VoodooI2CHID::start) puts the firmware default back.
// Writes are limited to the Haptics Intensity field. Vendor reports (0xFF00/0xFF05) and the surface/button switch
// (report 0x05) are never written. Every SET is appended to ~/Library/Logs/xe-toque/xe-haptic.log.
//
// Build: clang -O2 -Wall -o tools/build/xe-haptic tools/xe-haptic.c -framework IOKit -framework CoreFoundation
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ELAN_VID 0x04F3
#define ELAN_PID 0x31D1
#define PAGE_HAPTICS 0x0E
#define USAGE_INTENSITY 0x23

static FILE *logf_;

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (logf_) {
        char ts[32]; time_t t = time(NULL); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
        fprintf(logf_, "%s ", ts);
        va_start(ap, fmt); vfprintf(logf_, fmt, ap); va_end(ap);
        fflush(logf_);
    }
}

static long num(IOHIDDeviceRef d, CFStringRef key) {
    long v = -1;
    CFTypeRef r = IOHIDDeviceGetProperty(d, key);
    if (r && CFGetTypeID(r) == CFNumberGetTypeID()) CFNumberGetValue(r, kCFNumberLongType, &v);
    return v;
}

static IOHIDDeviceRef openElan(IOHIDManagerRef *mgrOut) {
    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int vid = ELAN_VID, pid = ELAN_PID;
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid), p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    const void *k[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey)}, *vals[] = {v, p};
    CFDictionaryRef match = CFDictionaryCreate(NULL, k, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(m, match);
    CFRelease(match); CFRelease(v); CFRelease(p);
    CFSetRef set = IOHIDManagerCopyDevices(m);
    if (!set || CFSetGetCount(set) == 0) {
        fprintf(stderr, "xe-haptic: no HID device %04X:%04X (is VoodooI2CHID attached to the touchpad?)\n", ELAN_VID, ELAN_PID);
        exit(1);
    }
    CFIndex n = CFSetGetCount(set);
    const void **devs = calloc(n, sizeof *devs);
    CFSetGetValues(set, devs);
    IOHIDDeviceRef dev = NULL;
    // Pick the one whose MaxFeatureReportSize > 0 (the real VoodooI2CHIDDevice, not a child).
    for (CFIndex i = 0; i < n && !dev; i++)
        if (num((IOHIDDeviceRef)devs[i], CFSTR(kIOHIDMaxFeatureReportSizeKey)) > 0) dev = (IOHIDDeviceRef)devs[i];
    if (!dev) dev = (IOHIDDeviceRef)devs[0];
    CFRetain(dev);
    free(devs); CFRelease(set);
    IOReturn r = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "xe-haptic: IOHIDDeviceOpen failed 0x%08x%s\n", r,
                r == (IOReturn)0xe00002e2 ? " (not permitted: grant Input Monitoring to the terminal)" : "");
        exit(1);
    }
    *mgrOut = m;
    return dev;
}

// GET_REPORT(feature). buf[0] = report ID on return (VoodooI2CHID copies the I2C payload after the 2-byte length).
static IOReturn getFeature(IOHIDDeviceRef d, uint8_t id, uint8_t *buf, CFIndex *len) {
    memset(buf, 0, *len);
    buf[0] = id;
    return IOHIDDeviceGetReport(d, kIOHIDReportTypeFeature, id, buf, len);
}

static void hexdump(const uint8_t *b, CFIndex n, CFIndex max) {
    for (CFIndex i = 0; i < n && i < max; i++) printf("%s%02x", (i && i % 32 == 0) ? "\n                " : (i ? " " : ""), b[i]);
    if (n > max) printf(" ... (%ld bytes)", (long)n);
    printf("\n");
}

typedef struct { uint8_t id; int bytes; const char *what; } FeatDesc;
// From the parsed report descriptor (~/xe/build/haptic/elan_04f3_31d1_desc.txt); bytes exclude the ID byte.
static const FeatDesc feats[] = {
    {0x02, 1, "Digitizer: Contact Count Max (0x55, low nibble) / Pad Type (0x59, high nibble)"},
    {0x03, 2, "Digitizer: Input Mode (0x52) 0=mouse 3=PTP"},
    {0x05, 2, "Digitizer: Surface Switch (0x57, bit0) / Button Switch (0x58, bit1)  [never written]"},
    {0x06, 256, "Vendor 0xFF00:0xC5 - PTP certification blob (THQA)"},
    {0x07, 2, "Digitizer: Latency Mode (0x60, bit0)"},
    {0x09, 2, "Haptics: byte0 = Intensity (0x0E:0x23, 0..100), byte1 = usage 0x0E:0x01 (0..100)"},
    {0x0A, 128, "Vendor 0xFF05:0x28"},
    {0x0B, 130, "Vendor 0xFF00:0xC7"},
    {0x0C, 896, "Vendor 0xFF00:0xC6"},
    {0x0D, 4, "Vendor 0xFF00:0xC5"},
    {0x0E, 4, "Vendor 0xFF00:0xC5 (mouse TLC)"},
    {0xBF, 1025, "Vendor 0xFF05:0xBF"},
    {0xDA, 1, "Vendor 0xFF05:0x21"},
};

// Locates the intensity element: report ID, byte offset inside the report payload, logical range.
static int findIntensity(IOHIDDeviceRef d, uint8_t *rid, int *off, long *lmin, long *lmax) {
    CFArrayRef els = IOHIDDeviceCopyMatchingElements(d, NULL, kIOHIDOptionsTypeNone);
    if (!els) return 0;
    int found = 0;
    CFIndex n = CFArrayGetCount(els);
    for (CFIndex i = 0; i < n && !found; i++) {
        IOHIDElementRef e = (IOHIDElementRef)CFArrayGetValueAtIndex(els, i);
        if (IOHIDElementGetType(e) != kIOHIDElementTypeFeature) continue;
        if (IOHIDElementGetUsagePage(e) != PAGE_HAPTICS || IOHIDElementGetUsage(e) != USAGE_INTENSITY) continue;
        *rid = (uint8_t)IOHIDElementGetReportID(e);
        *lmin = IOHIDElementGetLogicalMin(e);
        *lmax = IOHIDElementGetLogicalMax(e);
        // byte offset = sum of the sizes of the feature elements of this report that come before it (cookie order)
        int bits = 0;
        IOHIDElementCookie ck = IOHIDElementGetCookie(e);
        for (CFIndex j = 0; j < n; j++) {
            IOHIDElementRef f = (IOHIDElementRef)CFArrayGetValueAtIndex(els, j);
            if (IOHIDElementGetType(f) != kIOHIDElementTypeFeature || IOHIDElementGetReportID(f) != *rid) continue;
            if (IOHIDElementGetCookie(f) >= ck) continue;
            bits += (int)IOHIDElementGetReportSize(f);  // macOS: ReportSize is already size*count
        }
        if (bits % 8 || IOHIDElementGetReportSize(e) != 8) { fprintf(stderr, "xe-haptic: unexpected intensity layout\n"); break; }
        *off = bits / 8;
        found = 1;
    }
    CFRelease(els);
    return found;
}

static void dumpElements(IOHIDDeviceRef d) {
    CFArrayRef els = IOHIDDeviceCopyMatchingElements(d, NULL, kIOHIDOptionsTypeNone);
    if (!els) return;
    for (CFIndex i = 0; i < CFArrayGetCount(els); i++) {
        IOHIDElementRef e = (IOHIDElementRef)CFArrayGetValueAtIndex(els, i);
        IOHIDElementType t = IOHIDElementGetType(e);
        if (t != kIOHIDElementTypeFeature && t != kIOHIDElementTypeOutput) continue;
        printf("cookie %4u %-7s id 0x%02x usage %04X:%04X size %3u x%-4u range %ld..%ld\n", (unsigned)IOHIDElementGetCookie(e),
               t == kIOHIDElementTypeFeature ? "feature" : "output", (unsigned)IOHIDElementGetReportID(e),
               (unsigned)IOHIDElementGetUsagePage(e), (unsigned)IOHIDElementGetUsage(e), (unsigned)IOHIDElementGetReportSize(e),
               (unsigned)IOHIDElementGetReportCount(e), IOHIDElementGetLogicalMin(e), IOHIDElementGetLogicalMax(e));
    }
    CFRelease(els);
}

// Reads report `rid` (ID + 2 bytes). Returns 1 if the device really answered (first byte == report ID),
// 0 if it returned an empty/zero payload (this Elan firmware does that for 0x09: GET is not implemented), -1 on error.
static int readHaptic(IOHIDDeviceRef d, uint8_t rid, uint8_t rep[3], int verbose) {
    CFIndex len = 3;
    IOReturn r = getFeature(d, rid, rep, &len);
    if (r != kIOReturnSuccess) { logmsg("GET feature 0x%02x failed: 0x%08x\n", rid, r); return -1; }
    if (verbose) { printf("GET report 0x%02x raw: ", rid); hexdump(rep, len, 16); }
    return rep[0] == rid;
}

// SET report 0x09 = {ID, byte0, byte1}. Only the intensity byte is the target; byte1 (0x0E:0x01) is either kept from a
// valid GET or must be given explicitly (b1 >= 0). Both are clamped to the descriptor's logical range (refused otherwise).
static int writeHaptic(IOHIDDeviceRef d, uint8_t rid, int off, long lmin, long lmax, int val, int b1) {
    uint8_t rep[3];
    int ok = readHaptic(d, rid, rep, 0);
    if (ok < 0) return -1;
    if (val < lmin || val > lmax) { fprintf(stderr, "intensity %d outside the logical range %ld..%ld\n", val, lmin, lmax); return -1; }
    if (b1 < 0 && !ok) {
        fprintf(stderr, "the device does not answer GET for report 0x%02x, so the other byte (0x0E:0x01) is unknown.\n"
                        "Give it explicitly: xe-haptic set %d <0..100>   (e.g. 50)\n", rid, val);
        return -1;
    }
    if (b1 > 100) { fprintf(stderr, "byte1 %d outside 0..100\n", b1); return -1; }
    uint8_t out[3] = {rid, ok ? rep[1] : 0, ok ? rep[2] : 0};
    out[1 + off] = (uint8_t)val;
    if (b1 >= 0) out[1 + (1 - off)] = (uint8_t)b1;
    IOReturn r = IOHIDDeviceSetReport(d, kIOHIDReportTypeFeature, rid, out, 3);
    logmsg("SET feature 0x%02x -> %02x %02x %02x (intensity %d, previous %s): 0x%08x\n", rid, out[0], out[1], out[2], val,
           ok ? "read OK" : "unreadable", r);
    if (r != kIOReturnSuccess) return -1;
    usleep(30000);
    int ok2 = readHaptic(d, rid, rep, 0);
    if (ok2 == 1) logmsg("readback: %02x %02x %02x%s\n", rep[0], rep[1], rep[2], rep[1 + off] == val ? "" : "  MISMATCH");
    else logmsg("readback: device still does not answer GET 0x%02x (raw %02x %02x %02x); judge by feel\n", rid, rep[0], rep[1], rep[2]);
    return val;
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "get";
    IOHIDManagerRef mgr;
    IOHIDDeviceRef d = openElan(&mgr);
    printf("device %04lX:%04lX MaxFeatureReportSize %ld\n", num(d, CFSTR(kIOHIDVendorIDKey)), num(d, CFSTR(kIOHIDProductIDKey)),
           num(d, CFSTR(kIOHIDMaxFeatureReportSizeKey)));

    uint8_t rid = 0; int off = 0; long lmin = 0, lmax = 0;
    if (!findIntensity(d, &rid, &off, &lmin, &lmax)) { fprintf(stderr, "no Haptics Intensity (0x0E:0x23) feature element\n"); return 1; }

    if (!strcmp(cmd, "elements")) { dumpElements(d); return 0; }

    if (!strcmp(cmd, "list")) {
        static uint8_t buf[1100];
        for (size_t i = 0; i < sizeof feats / sizeof *feats; i++) {
            CFIndex len = feats[i].bytes + 1;
            IOReturn r = getFeature(d, feats[i].id, buf, &len);
            printf("0x%02x %-4d %s\n", feats[i].id, feats[i].bytes, feats[i].what);
            if (r != kIOReturnSuccess) { printf("     GET failed 0x%08x\n", r); continue; }
            printf("     len %-4ld ", (long)len);
            hexdump(buf, len, 64);
            if (buf[0] != feats[i].id) printf("     (first byte is not the report ID: the device did not answer this report)\n");
        }
        return 0;
    }

    if (!strcmp(cmd, "dump") && argc > 2) {   // read-only: full hex + ASCII of one listed feature report
        static uint8_t buf[1100];
        long id = strtol(argv[2], NULL, 0);
        for (size_t i = 0; i < sizeof feats / sizeof *feats; i++) {
            if (feats[i].id != id) continue;
            CFIndex len = feats[i].bytes + 1;
            IOReturn r = getFeature(d, feats[i].id, buf, &len);
            if (r != kIOReturnSuccess) { printf("GET failed 0x%08x\n", r); return 1; }
            for (CFIndex o = 0; o < len; o += 16) {
                printf("%04lx ", (long)o);
                for (CFIndex k = o; k < o + 16; k++) k < len ? printf(" %02x", buf[k]) : printf("   ");
                printf("  ");
                for (CFIndex k = o; k < o + 16 && k < len; k++) putchar(buf[k] >= 32 && buf[k] < 127 ? buf[k] : '.');
                putchar('\n');
            }
            return 0;
        }
        fprintf(stderr, "report 0x%02lx is not a known feature report\n", id);
        return 1;
    }

    uint8_t rep[3];
    if (!strcmp(cmd, "get")) {
        int ok = readHaptic(d, rid, rep, 1);
        if (ok < 0) return 1;
        if (!ok) {
            printf("intensity: unknown - the firmware returns an empty payload for GET report 0x%02x (write-only on this Elan)\n", rid);
            return 3;
        }
        printf("intensity %d (report 0x%02x byte %d, range %ld..%ld); other byte (0x0E:0x01) = %d\n", rep[1 + off], rid, off, lmin,
               lmax, rep[1 + (1 - off)]);
        return 0;
    }

    char path[1024];
    snprintf(path, sizeof path, "%s/Library/Logs/xe-toque", getenv("HOME"));
    mkdir(path, 0755);
    strlcat(path, "/xe-haptic.log", sizeof path);
    logf_ = fopen(path, "a");

    if (!strcmp(cmd, "set") && argc > 2) {
        int v = atoi(argv[2]), b1 = argc > 3 ? atoi(argv[3]) : -1;
        return writeHaptic(d, rid, off, lmin, lmax, v, b1) == v ? 0 : 1;
    }

    if (!strcmp(cmd, "test")) {
        // Needs a readable original value; otherwise there is nothing exact to restore to.
        int ok = readHaptic(d, rid, rep, 1);
        if (ok != 1) {
            fprintf(stderr, "test needs GET 0x%02x to work so it can restore the original exactly; it does not on this firmware.\n"
                            "Use 'set N B' by hand instead (a reboot restores the firmware default).\n", rid);
            return 1;
        }
        int orig = rep[1 + off];
        int v = argc > 2 ? atoi(argv[2]) : (orig >= 50 ? orig - 30 : orig + 30);
        int wait = argc > 3 ? atoi(argv[3]) : 0;
        logmsg("test: original intensity %d, trying %d\n", orig, v);
        int got = writeHaptic(d, rid, off, lmin, lmax, v, -1);
        if (wait > 0) { printf("click the touchpad now (%d s)...\n", wait); sleep(wait); }
        int back = writeHaptic(d, rid, off, lmin, lmax, orig, -1);
        logmsg("test result: set %s, restore %s\n", got == v ? "OK" : "FAILED", back == orig ? "OK" : "FAILED");
        return (got == v && back == orig) ? 0 : 1;
    }

    fprintf(stderr, "usage: xe-haptic [get | list | dump ID | elements | set N [B] | test [N] [seconds]]\n");
    return 2;
}
