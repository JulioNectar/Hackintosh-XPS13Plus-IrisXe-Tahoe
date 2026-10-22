// xe-hapticbar: menu-bar slider for the touchpad click-vibration intensity (Elan/Aito haptic pad on the XPS 13 Plus).
// Applies the saved value at launch and on every slider change by running xe-haptic set <N> 100.
// build: zsh tools/build-xe-hapticbar.sh (bundles xe-haptic inside the app as Contents/MacOS/xe-haptic)
#import <Cocoa/Cocoa.h>

static NSString *toolPath(void) {   // the copy inside the app; otherwise one on the usual install path
    NSString *p = [[NSBundle mainBundle] pathForAuxiliaryExecutable:@"xe-haptic"];
    return p ?: @"/usr/local/bin/xe-haptic";
}

@interface App : NSObject <NSApplicationDelegate>
@property NSStatusItem *item;
@property NSSlider *slider;
@property NSTextField *label;
@end

@implementation App
- (void)apply:(int)v {
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:toolPath()];
    t.arguments = @[@"set", [NSString stringWithFormat:@"%d", v], @"100"];
    t.standardOutput = [NSFileHandle fileHandleWithNullDevice];
    t.standardError = [NSFileHandle fileHandleWithNullDevice];
    [t launchAndReturnError:nil];
}
- (void)changed:(NSSlider *)s {
    int v = (int)lround(s.doubleValue);
    self.label.stringValue = [NSString stringWithFormat:@"Vibração do trackpad: %d", v];
    NSEvent *e = NSApp.currentEvent;   // apply once the knob is released (or on click/keys)
    if (e.type == NSEventTypeLeftMouseDragged) return;
    [[NSUserDefaults standardUserDefaults] setInteger:v forKey:@"intensity"];
    [self apply:v];
}
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    [d registerDefaults:@{@"intensity": @100}];
    int v = (int)[d integerForKey:@"intensity"];

    self.item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    NSImage *img = [NSImage imageWithSystemSymbolName:@"hand.tap" accessibilityDescription:@"Vibração"];
    img.template = YES;
    self.item.button.image = img;

    NSMenu *m = [NSMenu new];
    NSView *v1 = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 240, 56)];
    self.label = [NSTextField labelWithString:@""];
    self.label.frame = NSMakeRect(16, 30, 210, 18);
    self.slider = [NSSlider sliderWithValue:v minValue:0 maxValue:100 target:self action:@selector(changed:)];
    self.slider.frame = NSMakeRect(14, 6, 212, 22);
    self.slider.continuous = YES;
    [v1 addSubview:self.label]; [v1 addSubview:self.slider];
    NSMenuItem *si = [NSMenuItem new]; si.view = v1; [m addItem:si];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Sair" action:@selector(terminate:) keyEquivalent:@""];
    self.item.menu = m;
    self.label.stringValue = [NSString stringWithFormat:@"Vibração do trackpad: %d", v];

    // the touchpad may not be ready right after login: apply now and again a bit later
    [self apply:v];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [self apply:v]; });
}
@end

int main(void) { @autoreleasepool {
    NSApplication *a = [NSApplication sharedApplication];
    App *d = [App new]; a.delegate = d;
    [a setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [a run];
} }
