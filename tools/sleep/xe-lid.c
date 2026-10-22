// xe-lid: user LaunchAgent. When the lid closes, put the display to sleep through the
// path that is known to work here (XE-FBDPMS: backlight off, eDP link kept up); when it
// opens, declare user activity so the display comes back. It never asks for system sleep.
// Intended to run with `pmset -a disablesleep 1` (otherwise macOS sleeps on lid close
// before this agent matters).
// build: clang -O2 -o xe-lid xe-lid.c -framework IOKit -framework CoreFoundation
#include <IOKit/IOKitLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static io_service_t root;
static int lastClosed = -1;

static int lidClosed(void) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(root, CFSTR(kAppleClamshellStateKey), kCFAllocatorDefault, 0);
    int c = (v && CFGetTypeID(v) == CFBooleanGetTypeID()) ? CFBooleanGetValue(v) : 0;
    if (v) CFRelease(v);
    return c;
}

static void logmsg(const char *m) {
    time_t t = time(NULL); char b[32]; strftime(b, sizeof b, "%F %T", localtime(&t));
    printf("%s %s\n", b, m); fflush(stdout);
}

static void apply(void) {
    int c = lidClosed();
    if (c == lastClosed) return;
    lastClosed = c;
    if (c) {
        logmsg("lid closed -> pmset displaysleepnow");
        system("/usr/bin/pmset displaysleepnow");
    } else {
        logmsg("lid open -> declare user activity (display on)");
        IOPMAssertionID id;
        IOPMAssertionDeclareUserActivity(CFSTR("xe-lid open"), kIOPMUserActiveLocal, &id);
    }
}

static void cb(void *ref, io_service_t s, natural_t type, void *arg) {
    if (type == kIOPMMessageClamshellStateChange) apply();
}

int main(void) {
    root = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));
    if (!root) { fprintf(stderr, "no IOPMrootDomain\n"); return 1; }
    IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
    io_object_t n;
    if (IOServiceAddInterestNotification(port, root, kIOGeneralInterest, cb, NULL, &n)) {
        fprintf(stderr, "interest notification failed\n"); return 1;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);
    lastClosed = lidClosed();
    logmsg(lastClosed ? "start: lid closed" : "start: lid open");
    CFRunLoopRun();
    return 0;
}
