// xe-brilho: backlight control for the Iris Xe panel through the XeShim kext (PWM controller 1).
//   xe-brilho            print current brightness (%)
//   xe-brilho <1-100>    set brightness
//   xe-brilho + | -      one step up / down
//   xe-brilho daemon     restore the saved level, then follow the keyboard brightness keys
//                        plus extra bindings from ~/.xe-brilho-teclas ("up|down <keycode> <mods-hex>")
// Levels are perceptual: duty = period * (pct/100)^2.2. The last level is kept in ~/.xe-brilho.
#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "xe.h"

#define BLC_CTL   0xC8250u  // bit 31 = PWM enable
#define BLC_FREQ  0xC8254u  // period in PWM clocks
#define BLC_DUTY  0xC8258u  // on-time in PWM clocks
#define GAMMA     2.2
#define STEPS     16        // key presses from 0 to 100 %
#define MIN_PCT   1.0

static char statePath[1024];

static double get_pct(void) {
    uint32_t period = xe_rd(BLC_FREQ), duty = xe_rd(BLC_DUTY);
    if (!period || period == 0xDEADBEEF) return -1;
    return 100.0 * pow((double)duty / period, 1.0 / GAMMA);
}

static int set_pct(double pct) {
    if (pct < MIN_PCT) pct = MIN_PCT;
    if (pct > 100) pct = 100;
    if (!(xe_rd(BLC_CTL) & 0x80000000u)) { fprintf(stderr, "PWM desligado; nada alterado\n"); return -1; }
    uint32_t period = xe_rd(BLC_FREQ);
    if (!period || period == 0xDEADBEEF) { fprintf(stderr, "periodo PWM invalido\n"); return -1; }
    uint32_t duty = (uint32_t)lround(period * pow(pct / 100.0, GAMMA));
    if (duty < 1) duty = 1;
    xe_wr(BLC_DUTY, duty);
    FILE *f = fopen(statePath, "w");
    if (f) { fprintf(f, "%.2f\n", pct); fclose(f); }
    return 0;
}

static void step(int dir) {
    double pct = get_pct();
    if (pct < 0) return;
    // snap to the step grid so repeated presses land on the same levels
    double s = 100.0 / STEPS, k = round(pct / s) + dir;
    set_pct(k * s);
}


static char keysPath[1024];
static struct { long code; unsigned long long mask; int dir; } binds[8];
static int nbinds;
static void load_binds(void) {
    nbinds = 0;
    FILE *f = fopen(keysPath, "r");
    if (!f) return;
    char dir[16]; long code; unsigned long long mask;
    while (nbinds < 8 && fscanf(f, "%15s %ld %llx", dir, &code, &mask) == 3) {
        binds[nbinds].code = code; binds[nbinds].mask = mask;
        binds[nbinds].dir = !strcmp(dir, "up") ? +1 : -1; nbinds++;
    }
    fclose(f);
}
// modifier bits that must match exactly (cmd, alt, ctrl, shift, fn)
#define MOD_MASK 0x9e0000ULL
// NX_SYSDEFINED aux-control keys: data1 = keyCode << 16 | keyState << 8; 0xA = key down.
#define NX_KEYTYPE_BRIGHTNESS_UP   2
#define NX_KEYTYPE_BRIGHTNESS_DOWN 3
static CGEventRef onEvent(CGEventTapProxy proxy, CGEventType type, CGEventRef ev, void *ctx) {
    (void)proxy; (void)ctx;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        CGEventTapEnable(*(CFMachPortRef *)ctx, true);
        return ev;
    }
    if (type == kCGEventKeyDown) {
        long code = (long)CGEventGetIntegerValueField(ev, kCGKeyboardEventKeycode);
        unsigned long long flags = (unsigned long long)CGEventGetFlags(ev) & MOD_MASK;
        if (getenv("XE_BRILHO_DEBUG")) fprintf(stderr, "tecla: keycode=%ld mods=0x%llx\n", code, flags);
        for (int i = 0; i < nbinds; i++)
            if (binds[i].code == code && binds[i].mask == flags) { step(binds[i].dir); return NULL; }
        return ev;
    }
    if (type != NX_SYSDEFINED) return ev;
    NSEvent *e = [NSEvent eventWithCGEvent:ev];
    if (!e) return ev;
    NSInteger subtype = e.subtype, data1 = e.data1;
    if (getenv("XE_BRILHO_DEBUG"))
        fprintf(stderr, "evento: subtype=%ld key=%ld state=0x%lx\n", (long)subtype,
                (long)((data1 & 0xFFFF0000) >> 16), (long)((data1 & 0xFF00) >> 8));
    if (subtype != 8) return ev;
    int key = (int)((data1 & 0xFFFF0000) >> 16), down = ((data1 & 0xFF00) >> 8) == 0xA;
    if (key == NX_KEYTYPE_BRIGHTNESS_UP || key == NX_KEYTYPE_BRIGHTNESS_DOWN) {
        if (down) step(key == NX_KEYTYPE_BRIGHTNESS_UP ? +1 : -1);
        return NULL;  // consume: macOS has no backlight driver for this panel anyway
    }
    return ev;
}

static int daemon_mode(void) {
    FILE *f = fopen(statePath, "r");
    double saved;
    if (f) { if (fscanf(f, "%lf", &saved) == 1) set_pct(saved); fclose(f); }
    static CFMachPortRef tap;
    tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
                           CGEventMaskBit(NX_SYSDEFINED) | CGEventMaskBit(kCGEventKeyDown), onEvent, &tap);
    if (!tap) {
        fprintf(stderr, "sem permissao para ler o teclado: libere xe-brilho em Ajustes > Privacidade e Seguranca > Acessibilidade\n");
        return 1;
    }
    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(NULL, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    fprintf(stderr, "xe-brilho: escutando as teclas de brilho\n");
    CFRunLoopRun();
    return 0;
}

int main(int argc, char **argv) {
    snprintf(statePath, sizeof statePath, "%s/.xe-brilho", getenv("HOME") ? getenv("HOME") : "/tmp");
    snprintf(keysPath, sizeof keysPath, "%s/.xe-brilho-teclas", getenv("HOME") ? getenv("HOME") : "/tmp");
    load_binds();
    setvbuf(stderr, NULL, _IONBF, 0);
    if (xe_open()) return 1;
    if (argc < 2) { printf("%.0f%%\n", get_pct()); return 0; }
    if (!strcmp(argv[1], "daemon")) return daemon_mode();
    if (!strcmp(argv[1], "+")) { step(+1); printf("%.0f%%\n", get_pct()); return 0; }
    if (!strcmp(argv[1], "-")) { step(-1); printf("%.0f%%\n", get_pct()); return 0; }
    char *end; double v = strtod(argv[1], &end);
    if (*end || v <= 0 || v > 100) { fprintf(stderr, "uso: xe-brilho [1-100 | + | - | daemon]\n"); return 2; }
    if (set_pct(v)) return 1;
    printf("%.0f%%\n", get_pct());
    return 0;
}
