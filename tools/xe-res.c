// xe-res: lista os modos da tela principal (incluindo os HiDPI "escalados") e troca o modo sem abrir Ajustes > Telas.
// Uso: xe-res            -> lista os modos (o atual marcado com *)
//      xe-res N          -> passa para o modo N e grava como permanente
// Compilar: clang -O2 -o ~/xe/build/xe-res ~/xe/tools/xe-res.c -framework CoreGraphics -framework CoreFoundation
#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    CGDirectDisplayID d = CGMainDisplayID();
    const void *k[] = {kCGDisplayShowDuplicateLowResolutionModes};
    const void *v[] = {kCFBooleanTrue};
    CFDictionaryRef opt = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(d, opt);
    CFRelease(opt);
    if (!modes) { fprintf(stderr, "sem modos\n"); return 1; }
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(d);
    CFIndex n = CFArrayGetCount(modes);

    if (argc < 2) {
        printf("tela %u: %ld modos (pontos = tamanho dos itens; menor = itens maiores)\n", d, (long)n);
        for (CFIndex i = 0; i < n; i++) {
            CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
            size_t w = CGDisplayModeGetWidth(m), h = CGDisplayModeGetHeight(m);
            size_t pw = CGDisplayModeGetPixelWidth(m), ph = CGDisplayModeGetPixelHeight(m);
            int atual = cur && CGDisplayModeGetIODisplayModeID(m) == CGDisplayModeGetIODisplayModeID(cur);
            printf("%c %3ld: %5zu x %-5zu pontos  (%zu x %zu pixels)%s  %.0f Hz\n", atual ? '*' : ' ', (long)i, w, h, pw, ph,
                   pw > w ? "  HiDPI" : "       ", CGDisplayModeGetRefreshRate(m));
        }
        return 0;
    }

    long idx = -1;
    if (argv[1][0] == 'w') {
        // "wNNNN": modo HiDPI com largura NNNN pontos (ou o HiDPI de largura mais próxima)
        long alvo = strtol(argv[1] + 1, NULL, 10), melhor = -1, dif = 1L << 30;
        for (CFIndex i = 0; i < n; i++) {
            CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
            if (CGDisplayModeGetPixelWidth(m) <= CGDisplayModeGetWidth(m)) continue;  // só HiDPI
            long d = labs((long)CGDisplayModeGetWidth(m) - alvo);
            if (d < dif || (d == dif && CGDisplayModeGetRefreshRate(m) > 0)) { dif = d; melhor = i; }
        }
        if (melhor < 0) { fprintf(stderr, "nenhum modo HiDPI disponível; modos existentes:\n"); argc = 1; }
        else { idx = melhor; if (dif) printf("sem HiDPI de %ld pontos; usando o mais próximo\n", alvo); }
        if (melhor < 0) {
            for (CFIndex i = 0; i < n; i++) {
                CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
                fprintf(stderr, "  %3ld: %zu x %zu pontos (%zu x %zu pixels)\n", (long)i, CGDisplayModeGetWidth(m),
                        CGDisplayModeGetHeight(m), CGDisplayModeGetPixelWidth(m), CGDisplayModeGetPixelHeight(m));
            }
            return 1;
        }
    } else idx = strtol(argv[1], NULL, 10);
    if (idx < 0 || idx >= n) { fprintf(stderr, "modo %ld inexistente (0..%ld)\n", idx, (long)n - 1); return 1; }
    CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, idx);
    CGDisplayConfigRef cfg;
    if (CGBeginDisplayConfiguration(&cfg) != kCGErrorSuccess) { fprintf(stderr, "CGBeginDisplayConfiguration falhou\n"); return 1; }
    CGConfigureDisplayWithDisplayMode(cfg, d, m, NULL);
    CGError e = CGCompleteDisplayConfiguration(cfg, kCGConfigurePermanently);
    printf("modo %ld (%zu x %zu pontos): %s\n", idx, CGDisplayModeGetWidth(m), CGDisplayModeGetHeight(m),
           e == kCGErrorSuccess ? "aplicado" : "ERRO");
    return e == kCGErrorSuccess ? 0 : 1;
}
