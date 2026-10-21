// xe-vatrace: biblioteca de rastreio carregada pelo AppleIntelTGLGraphicsVADriver (LC_LOAD_DYLIB adicionado no driver).
// Redireciona os ponteiros de símbolos importados pelo driver (estilo fishhook) e registra no log do sistema
// (os_log, subsystem "xe.vatrace") cada chamada ao IOAccelerator/IOKit/dlopen com o resultado. Só diagnóstico.
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <IOKit/IOKitLib.h>
#include <os/log.h>
#include <stdlib.h>
static os_log_t L;
static void hook_tab(void);
#define T(...) os_log_error(L, __VA_ARGS__)

extern void *IOAccelDeviceCreateWithAPIProperty(io_service_t, const char *);
extern void *IOAccelSharedCreate(void *);
extern void *IOAccelContextCreate(void *, uint32_t);
extern io_connect_t IOAccelDeviceGetConnect(void *);
extern io_connect_t IOAccelVideoContextGetConnect(void *);
extern void *IOAccelVideoContextGetSidebandBuffer(void *, void *, void *);
extern void *IOAccelVideoContextGetFenceBuffer(void *, void *, void *);
extern int gpu_bundle_is_path_trusted(const char *);

static void *w_dev(io_service_t s, const char *api) { hook_tab(); void *r = IOAccelDeviceCreateWithAPIProperty(s, api); T("IOAccelDeviceCreateWithAPIProperty(svc=0x%x, %{public}s) = %p", s, api, r); return r; }
static void *w_shared(void *d) { void *r = IOAccelSharedCreate(d); T("IOAccelSharedCreate(%p) = %p", d, r); return r; }
static void *w_ctx(void *s, uint32_t t) { void *r = IOAccelContextCreate(s, t); T("IOAccelContextCreate(%p, 0x%x) = %p", s, t, r); return r; }
static io_connect_t w_devconn(void *d) { io_connect_t r = IOAccelDeviceGetConnect(d); T("IOAccelDeviceGetConnect(%p) = 0x%x", d, r); return r; }
static io_connect_t w_vconn(void *c) { io_connect_t r = IOAccelVideoContextGetConnect(c); T("IOAccelVideoContextGetConnect(%p) = 0x%x", c, r); return r; }
static void *w_side(void *c, void *a, void *b) { void *r = IOAccelVideoContextGetSidebandBuffer(c, a, b); T("IOAccelVideoContextGetSidebandBuffer(%p) = %p", c, r); return r; }
static void *w_fence(void *c, void *a, void *b) { void *r = IOAccelVideoContextGetFenceBuffer(c, a, b); T("IOAccelVideoContextGetFenceBuffer(%p) = %p", c, r); return r; }
static kern_return_t w_struct(mach_port_t c, uint32_t sel, const void *in, size_t ni, void *out, size_t *no) {
  size_t req = no ? *no : 0; kern_return_t r = IOConnectCallStructMethod(c, sel, in, ni, out, no);
  T("IOConnectCallStructMethod(0x%x, sel=0x%x, in=%zu, out=%zu->%zu) = 0x%x", c, sel, ni, req, no ? *no : 0, r);
  if (sel == 0xb && out && no && *no >= 12) T("  get_hw_info device-id=0x%08x", ((uint32_t *)out)[2]);
  if (out && no && *no >= 4) T("  saída: %{public}.*P", (int)(*no > 64 ? 64 : *no), out);
  return r; }
static kern_return_t w_method(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t ni, const void *is, size_t nis, uint64_t *out, uint32_t *no, void *os, size_t *nos) {
  kern_return_t r = IOConnectCallMethod(c, sel, in, ni, is, nis, out, no, os, nos); T("IOConnectCallMethod(0x%x, sel=0x%x) = 0x%x", c, sel, r); return r; }
static kern_return_t w_scalar(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t ni, uint64_t *out, uint32_t *no) {
  kern_return_t r = IOConnectCallScalarMethod(c, sel, in, ni, out, no); T("IOConnectCallScalarMethod(0x%x, sel=0x%x) = 0x%x", c, sel, r); return r; }
static kern_return_t w_open(io_service_t s, task_port_t t, uint32_t ty, io_connect_t *c) {
  kern_return_t r = IOServiceOpen(s, t, ty, c); T("IOServiceOpen(0x%x, type=%u) = 0x%x", s, ty, r); return r; }
static kern_return_t w_match(mach_port_t m, CFDictionaryRef d, io_iterator_t *it) { hook_tab();
  kern_return_t r = IOServiceGetMatchingServices(m, d, it); T("IOServiceGetMatchingServices = 0x%x it=0x%x", r, it ? *it : 0); return r; }
static io_object_t w_next(io_iterator_t it) { io_object_t r = IOIteratorNext(it); T("IOIteratorNext(0x%x) = 0x%x", it, r); return r; }
static void *w_dlopen(const char *p, int m) { void *r = dlopen(p, m); T("dlopen(%{public}s) = %p %{public}s", p ? p : "(null)", r, r ? "" : dlerror()); return r; }
static void *w_dlsym(void *h, const char *s) { void *r = dlsym(h, s); T("dlsym(%p, %{public}s) = %p", h, s, r); return r; }
static int w_trusted(const char *p) { int r = gpu_bundle_is_path_trusted(p); T("gpu_bundle_is_path_trusted(%{public}s) = %d", p, r); return r; }

extern uint64_t IOAccelResourceCreate();
static uint64_t g_IOAccelResourceCreate(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOAccelResourceCreate)(a,b,c,d,e,f); T("IOAccelResourceCreate(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOAccelResourceGetClientShared();
static uint64_t g_IOAccelResourceGetClientShared(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOAccelResourceGetClientShared)(a,b,c,d,e,f); T("IOAccelResourceGetClientShared(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOAccelResourceGetClientSharedPrivate();
static uint64_t g_IOAccelResourceGetClientSharedPrivate(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOAccelResourceGetClientSharedPrivate)(a,b,c,d,e,f); T("IOAccelResourceGetClientSharedPrivate(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOAccelResourceGetDataBytes();
static uint64_t g_IOAccelResourceGetDataBytes(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOAccelResourceGetDataBytes)(a,b,c,d,e,f); T("IOAccelResourceGetDataBytes(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOAccelResourceGetDataSize();
static uint64_t g_IOAccelResourceGetDataSize(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOAccelResourceGetDataSize)(a,b,c,d,e,f); T("IOAccelResourceGetDataSize(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOAccelResourceFinishSysMem();
static uint64_t g_IOAccelResourceFinishSysMem(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOAccelResourceFinishSysMem)(a,b,c,d,e,f); T("IOAccelResourceFinishSysMem(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOAccelResourceRelease();
static uint64_t g_IOAccelResourceRelease(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOAccelResourceRelease)(a,b,c,d,e,f); T("IOAccelResourceRelease(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOAccelSharedRelease();
static uint64_t g_IOAccelSharedRelease(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOAccelSharedRelease)(a,b,c,d,e,f); T("IOAccelSharedRelease(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOAccelVideoContextGetDataBufferResource();
static uint64_t g_IOAccelVideoContextGetDataBufferResource(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOAccelVideoContextGetDataBufferResource)(a,b,c,d,e,f); T("IOAccelVideoContextGetDataBufferResource(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOAccelVideoContextGetDataBufferClassCount();
static uint64_t g_IOAccelVideoContextGetDataBufferClassCount(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOAccelVideoContextGetDataBufferClassCount)(a,b,c,d,e,f); T("IOAccelVideoContextGetDataBufferClassCount(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOSurfaceLookup();
static uint64_t g_IOSurfaceLookup(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOSurfaceLookup)(a,b,c,d,e,f); T("IOSurfaceLookup(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOSurfaceCreate();
static uint64_t g_IOSurfaceCreate(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOSurfaceCreate)(a,b,c,d,e,f); T("IOSurfaceCreate(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOAccelDeviceTestEvent();
static uint64_t g_IOAccelDeviceTestEvent(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOAccelDeviceTestEvent)(a,b,c,d,e,f); T("IOAccelDeviceTestEvent(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
extern uint64_t IOAccelVideoContextFinishFenceEvent();
static uint64_t g_IOAccelVideoContextFinishFenceEvent(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { uint64_t r = ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))IOAccelVideoContextFinishFenceEvent)(a,b,c,d,e,f); T("IOAccelVideoContextFinishFenceEvent(%#llx, %#llx, %#llx, %#llx) = %#llx", a, b, c, d, r); return r; }
// ---- tabela de funções do driver (calloc(1, 0x88) em AVD_CreateAVDAccelerator; entradas +0x08..+0x80) ----
typedef uint64_t (*F6)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static F6 g_orig[17]; static void **g_tab; static int g_tabdone;
#define W(k) static uint64_t t##k(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { \
  if (k == 6 && b) T("tabela[6] pedido: %{public}.32P", (void *)b); if (k == 16 && b) { uint32_t n = *(uint32_t *)b; void *arr = *(void **)(b + 8); T("tabela[16] pedido: n=%u lista=%{public}.64P", n, arr ? arr : (void *)b); } uint64_t r = g_orig[k](a, b, c, d, e, f); T("tabela[%d](%#llx, %#llx, %#llx) = %#llx", k, a, b, c, r); return r; }
W(1) W(2) W(3) W(4) W(5) W(6) W(7) W(8) W(9) W(10) W(11) W(12) W(13) W(14) W(15) W(16)
static F6 g_w[17] = {0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16};
static void hook_tab(void) {
  if (!g_tab || g_tabdone) return; g_tabdone = 1;
  for (int k = 1; k <= 16; k++) { g_orig[k] = (F6)g_tab[k]; if (g_orig[k]) g_tab[k] = (void *)g_w[k]; }
  T("tabela do driver %p embrulhada", g_tab);
}
static void *w_calloc(size_t n, size_t sz) { void *r = calloc(n, sz); if (n == 1 && sz == 0x88) { g_tab = r; g_tabdone = 0; T("calloc tabela = %p", r); } return r; }

static const struct { const char *n; void *w; } HK[] = {
  {"_calloc", w_calloc},
  {"_IOAccelResourceCreate", g_IOAccelResourceCreate},
  {"_IOAccelResourceGetClientShared", g_IOAccelResourceGetClientShared},
  {"_IOAccelResourceGetClientSharedPrivate", g_IOAccelResourceGetClientSharedPrivate},
  {"_IOAccelResourceGetDataBytes", g_IOAccelResourceGetDataBytes},
  {"_IOAccelResourceGetDataSize", g_IOAccelResourceGetDataSize},
  {"_IOAccelResourceFinishSysMem", g_IOAccelResourceFinishSysMem},
  {"_IOAccelResourceRelease", g_IOAccelResourceRelease},
  {"_IOAccelSharedRelease", g_IOAccelSharedRelease},
  {"_IOAccelVideoContextGetDataBufferResource", g_IOAccelVideoContextGetDataBufferResource},
  {"_IOAccelVideoContextGetDataBufferClassCount", g_IOAccelVideoContextGetDataBufferClassCount},
  {"_IOSurfaceLookup", g_IOSurfaceLookup},
  {"_IOSurfaceCreate", g_IOSurfaceCreate},
  {"_IOAccelDeviceTestEvent", g_IOAccelDeviceTestEvent},
  {"_IOAccelVideoContextFinishFenceEvent", g_IOAccelVideoContextFinishFenceEvent},

  {"_IOAccelDeviceCreateWithAPIProperty", w_dev}, {"_IOAccelSharedCreate", w_shared}, {"_IOAccelContextCreate", w_ctx},
  {"_IOAccelDeviceGetConnect", w_devconn}, {"_IOAccelVideoContextGetConnect", w_vconn},
  {"_IOAccelVideoContextGetSidebandBuffer", w_side}, {"_IOAccelVideoContextGetFenceBuffer", w_fence},
  {"_IOConnectCallStructMethod", w_struct}, {"_IOConnectCallMethod", w_method}, {"_IOConnectCallScalarMethod", w_scalar},
  {"_IOServiceOpen", w_open}, {"_IOServiceGetMatchingServices", w_match}, {"_IOIteratorNext", w_next},
  {"_dlopen", w_dlopen}, {"_dlsym", w_dlsym}, {"_gpu_bundle_is_path_trusted", w_trusted}};

static void rebind(const struct mach_header_64 *h, intptr_t slide) {
  const struct segment_command_64 *le = NULL; const struct symtab_command *st = NULL; const struct dysymtab_command *dy = NULL;
  const struct load_command *lc = (const void *)(h + 1);
  for (uint32_t i = 0; i < h->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
    if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__LINKEDIT")) le = (const void *)lc;
    else if (lc->cmd == LC_SYMTAB) st = (const void *)lc; else if (lc->cmd == LC_DYSYMTAB) dy = (const void *)lc;
  }
  if (!le || !st || !dy) { T("rebind: comandos não achados"); return; }
  uintptr_t lb = (uintptr_t)slide + le->vmaddr - le->fileoff;
  const struct nlist_64 *sy = (const void *)(lb + st->symoff); const char *str = (const char *)(lb + st->stroff);
  const uint32_t *ind = (const uint32_t *)(lb + dy->indirectsymoff); int n = 0;
  lc = (const void *)(h + 1);
  for (uint32_t i = 0; i < h->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
    if (lc->cmd != LC_SEGMENT_64) continue;
    const struct segment_command_64 *sg = (const void *)lc; const struct section_64 *sc = (const void *)(sg + 1);
    for (uint32_t j = 0; j < sg->nsects; j++, sc++) {
      uint32_t ty = sc->flags & SECTION_TYPE;
      if (ty != S_LAZY_SYMBOL_POINTERS && ty != S_NON_LAZY_SYMBOL_POINTERS) continue;
      void **ptrs = (void **)((uintptr_t)slide + sc->addr); uint32_t cnt = (uint32_t)(sc->size / 8);
      uintptr_t pg = (uintptr_t)ptrs & ~0xfffUL; mprotect((void *)pg, ((uintptr_t)ptrs + sc->size - pg + 0xfff) & ~0xfffUL, PROT_READ | PROT_WRITE);
      for (uint32_t k = 0; k < cnt; k++) {
        uint32_t si = ind[sc->reserved1 + k]; if (si & (INDIRECT_SYMBOL_ABS | INDIRECT_SYMBOL_LOCAL)) continue;
        const char *nm = str + sy[si].n_un.n_strx;
        for (size_t q = 0; q < sizeof HK / sizeof *HK; q++) if (!strcmp(nm, HK[q].n)) { ptrs[k] = HK[q].w; n++; }
      }
    }
  }
  T("rebind: %d ponteiros redirecionados", n);
}
static void added(const struct mach_header *mh, intptr_t slide) {
  // só o binário do driver (o caminho desta biblioteca também contém "AppleIntelTGLGraphicsVADriver": não se redirecionar)
  Dl_info di; if (!dladdr(mh, &di) || !di.dli_fname) return;
  size_t n = strlen(di.dli_fname), m = strlen("/MacOS/AppleIntelTGLGraphicsVADriver");
  if (n < m || strcmp(di.dli_fname + n - m, "/MacOS/AppleIntelTGLGraphicsVADriver")) return;
  T("driver de vídeo carregado: %{public}s", di.dli_fname); rebind((const struct mach_header_64 *)mh, slide);
}
__attribute__((constructor)) static void init(void) {
  L = os_log_create("xe.vatrace", "va"); T("xe-vatrace carregado em %{public}s", getprogname());
  _dyld_register_func_for_add_image(added);
}
