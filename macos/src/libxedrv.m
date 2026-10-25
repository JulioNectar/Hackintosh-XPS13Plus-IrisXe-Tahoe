// libxedrv v19 — injetado no driver Metal TGL 16.0.32 (LC_LOAD_DYLIB). Adapta o driver (era Big Sur) ao Metal do Tahoe 25G241. v16: escritas em buffer pelo caminho 3D (ver "v16"). v17: stencil fora do bloco de 64 KB de outro render target (ver "v17"). v19: mipmaps das texturas zero-copy (ver "v19").
// Mudanças em relação à v7 (xe/build/llvm-bridge/xedrv.m), comparando com o adaptador do IntelIGPU-Tahoe (25G83) e com o layout medido:
//  1. _descriptorPrivate: o deslocamento de 0x10 começa em 0xa8 (o driver antigo também tem shaderValidation em 0xa0; a v7 punha
//     textureWriteRoundingMode ali). Layout antigo lido da codificação de tipo do próprio driver:
//     @[8Q]QQQQcQQQQQc q (QQ)Q(If)QQ (misc) IIQ @@@@@@@@ ^v^v @ c I Q @ Q  (0x160 bytes)
//  2. instalação SÍNCRONA no constructor (a v7 usava threads com sondagem: os primeiros pipelines podiam nascer sem os adaptadores).
//  3. dynamic libraries: o Tahoe herda supportsDynamicLibraries=YES, o driver TGL não implementa -> rejeitar com erro (como o IntelIGPU-Tahoe).
//  4. preflight de layout com log (tamanho da classe, offset do ivar _private, nomes no encoding); se falhar, NÃO converte e avisa.
//  5. modo laboratório: se existir /Library/GPUBundles/xe-lab, o WindowServer não recebe MTLDevice (fica no renderizador por software)
//     e só os nossos programas de teste usam a GPU -> iterar sem reboot.
// Logs: syslog "xedrv8: ..." (log show --predicate 'eventMessage CONTAINS "xedrv8"').
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <mach/mach.h>
#import <mach/mach_vm.h>
#import <dlfcn.h>
#import <pthread.h>
#import <syslog.h>
#import <stdint.h>
#import <string.h>
#import <unistd.h>
#import <signal.h>
#import <execinfo.h>
#import <sys/ucontext.h>
#import <stdarg.h>
#import <time.h>
#import <IOSurface/IOSurface.h>

// v13 (20/10): além do syslog, grava em /tmp/xedrv-<processo>.log (o log show não funciona sem privilégio).
static FILE* g_logf;
static void xe_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void xe_log(const char* fmt, ...) {
  char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
  syslog(LOG_ERR, "xedrv17: %s", b);
  if (!g_logf) { char p[256]; snprintf(p, sizeof p, "/tmp/xedrv-%s.log", getprogname()); g_logf = fopen(p, "a"); if (g_logf) setvbuf(g_logf, NULL, _IOLBF, 0); }
  if (g_logf) fprintf(g_logf, "%ld %s\n", (long)time(NULL), b);
}
#define LOG(...) xe_log(__VA_ARGS__)
#define PRIV_SZ 0x2e0
#define LAB_FLAG "/Library/GPUBundles/xe-lab"

static const void* g_drv_base;   // base do AppleIntelTGLGraphicsMTLDriver (para reconhecer chamadas vindas dele)
static int g_layout_ok;

// ---- campo de bits "misc" (union em 0xe0 no Tahoe, 0xd0 no antigo): {pos_antiga, larg_antiga, pos_nova, larg_nova} ----
static const struct { int op, ow, np, nw; } MISC[21] = {
  {0,1,0,2},{1,1,2,2},{2,1,4,1},{3,2,5,2},{5,1,7,1},{6,1,8,1},{7,1,9,1},{8,1,10,1},{9,1,11,1},{10,1,12,1},{11,1,13,1},
  {12,1,14,1},{13,1,15,1},{14,1,16,1},{15,8,17,8},{23,3,25,3},{26,1,28,1},{27,4,32,4},{31,1,36,1},{32,1,37,1},{33,1,38,1}};
static uint64_t misc_to_old(uint64_t n) {
  uint64_t o = 0;
  for (int i = 0; i < 21; i++) {
    uint64_t v = (n >> MISC[i].np) & ((1ULL << MISC[i].nw) - 1);
    o |= (v & ((1ULL << MISC[i].ow) - 1)) << MISC[i].op;
  }
  return o;
}

static int from_driver(void* ra) {
  Dl_info di;
  return g_drv_base && dladdr(ra, &di) && di.dli_fbase == g_drv_base;
}

// ---- _descriptorPrivate: o driver recebe uma CÓPIA no layout antigo; o Metal continua vendo a estrutura real ----
#define RING 256
static __thread uint8_t* t_ring[RING]; static __thread int t_pos;
static __thread uint64_t t_att[RING]; static __thread int t_apos;
static IMP g_orig_pd, g_orig_att;

static void* pd_wrapper(id self, SEL _cmd) {
  void* real = ((void*(*)(id, SEL))g_orig_pd)(self, _cmd);
  if (!real || !g_layout_ok || !from_driver(__builtin_return_address(0))) return real;
  int i = t_pos++ % RING;
  if (!t_ring[i]) t_ring[i] = (uint8_t*)calloc(1, 0x400);
  uint8_t* p = t_ring[i];
  const uint8_t* n = (const uint8_t*)real;
  memcpy(p, n, PRIV_SZ);                          // 0x00..0xa8 iguais (até shaderValidation inclusive)
  memcpy(p + 0xa8, n + 0xb8, 0x60);               // 0xa8..0x108 antigo <- 0xb8..0x118 novo (pula shaderValidationState/textureWriteRoundingMode)
  *(uint64_t*)(p + 0xd0) = misc_to_old(*(const uint64_t*)(n + 0xe0));
  memcpy(p + 0x108, n + 0x198, 0x20);             // vertexBuffers, fragmentBuffers, driverCompilerOptions, gpuCompilerSPIOptions
  memcpy(p + 0x128, n + 0x1c0, 0x20);             // pad0, pad1, pluginData, needsCustomBorderColorSamplers/maxVertexAmplificationCount
  memcpy(p + 0x148, n + 0x1e0, 0x18);
  static int logged; if (!logged) { logged = 1; LOG("_descriptorPrivate(pipeline) convertido p/ layout antigo"); }
  return p;
}
static void* att_wrapper(id self, SEL _cmd) {
  void* real = ((void*(*)(id, SEL))g_orig_att)(self, _cmd);
  if (!real || !g_layout_ok || !from_driver(__builtin_return_address(0))) return real;
  uint64_t n = *(uint64_t*)real;
  MTLRenderPipelineColorAttachmentDescriptor* a = (MTLRenderPipelineColorAttachmentDescriptor*)self;
  uint64_t old = (a.blendingEnabled ? 1ULL : 0ULL) | ((uint64_t)a.rgbBlendOperation << 1) | ((uint64_t)a.alphaBlendOperation << 4) |
    ((uint64_t)a.sourceRGBBlendFactor << 7) | ((uint64_t)a.sourceAlphaBlendFactor << 12) |
    ((uint64_t)a.destinationRGBBlendFactor << 17) | ((uint64_t)a.destinationAlphaBlendFactor << 22) |
    (((uint64_t)a.writeMask & 15) << 27) | (((n >> 33) & 31) << 31) | ((uint64_t)a.pixelFormat << 36);
  int i = t_apos++ % RING;
  t_att[i] = old;
  static int logged; if (!logged) { logged = 1; LOG("_descriptorPrivate(attachment) convertido p/ bits antigos"); }
  return &t_att[i];
}

// preflight: confere que o Metal deste sistema tem o layout que a conversão assume (mesmos testes do IntelIGPU-Tahoe + sonda dinâmica)
static int check_layout(void) {
  Class c = objc_getClass("MTLRenderPipelineDescriptorInternal"), ac = objc_getClass("MTLRenderPipelineColorAttachmentDescriptorInternal");
  Ivar p = c ? class_getInstanceVariable(c, "_private") : NULL, ap = ac ? class_getInstanceVariable(ac, "_private") : NULL;
  const char* e = p ? ivar_getTypeEncoding(p) : NULL; const char* ae = ap ? ivar_getTypeEncoding(ap) : NULL;
  LOG("layout: RPDInternal size=0x%zx _private@0x%tx; attInternal size=0x%zx _private@0x%tx",
      c ? class_getInstanceSize(c) : 0, p ? ivar_getOffset(p) : -1, ac ? class_getInstanceSize(ac) : 0, ap ? ivar_getOffset(ap) : -1);
  int ok = e && strstr(e, "\"shaderValidation\"q\"shaderValidationState\"q\"textureWriteRoundingMode\"q") &&
           strstr(e, "\"alphaToCoverageEnabled\"b2\"alphaToOneEnabled\"b2") &&
           ae && strstr(ae, "\"blendingEnabled\"b2") && strstr(ae, "\"writeMask\"b5\"logicOpEnabled\"b1\"logicOp\"b4\"pixelFormat\"b22");
  if (!ok) { LOG("layout: encoding inesperado -> conversão DESLIGADA"); return 0; }
  // sonda dinâmica: rasterSampleCount em +0xb8 e vertexDescriptor em +0x110
  SEL s = sel_registerName("_descriptorPrivate");
  MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
  d.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor]; d.rasterSampleCount = 4;
  const uint8_t* raw = ((const void*(*)(id, SEL))objc_msgSend)(d, s);
  uint64_t cnt = 0; void* vd = NULL; memcpy(&cnt, raw + 0xb8, 8); memcpy(&vd, raw + 0x110, 8);
  ok = cnt == 4 && vd == (__bridge void*)d.vertexDescriptor;
  [d release];
  LOG("layout: sonda sampleCount@0xb8=%llu vertexDescriptor@0x110 %s -> %s", cnt, ok ? "ok" : "ERRADO", ok ? "conversão ligada" : "conversão DESLIGADA");
  return ok;
}

// ---- lacunas de API: selectors que o Metal do Tahoe usa e o driver TGL antigo não implementa ----
typedef struct { uint8_t r, g, b, a; } XeSwz;
static IMP g_view[6], g_buf[2];
static uint64_t g_invalid_idx;
static BOOL idx_ok(SEL c, uint64_t idx) {
  if (idx == g_invalid_idx) return YES;
  LOG("%s com resourceIndex explícito não suportado", sel_getName(c)); return NO;
}
static id v_initFmt(id self, SEL c, id src, NSUInteger f, uint64_t idx) { if (!idx_ok(c, idx)) { [self release]; return nil; }
  return ((id(*)(id, SEL, id, NSUInteger))g_view[0])(self, sel_registerName("initWithTexture:pixelFormat:"), src, f); }
static id v_initRange(id self, SEL c, id src, NSUInteger f, NSUInteger t, NSRange l, NSRange s_, uint64_t idx) { if (!idx_ok(c, idx)) { [self release]; return nil; }
  return ((id(*)(id, SEL, id, NSUInteger, NSUInteger, NSRange, NSRange))g_view[1])(self, sel_registerName("initWithTexture:pixelFormat:textureType:levels:slices:"), src, f, t, l, s_); }
static id v_initSwz(id self, SEL c, id src, NSUInteger f, NSUInteger t, NSRange l, NSRange s_, XeSwz sw, uint64_t idx) { if (!idx_ok(c, idx)) { [self release]; return nil; }
  return ((id(*)(id, SEL, id, NSUInteger, NSUInteger, NSRange, NSRange, XeSwz))g_view[2])(self, sel_registerName("initWithTexture:pixelFormat:textureType:levels:slices:swizzle:"), src, f, t, l, s_, sw); }
static id v_newFmt(id self, SEL c, NSUInteger f, uint64_t idx) { if (!idx_ok(c, idx)) return nil;
  return ((id(*)(id, SEL, NSUInteger))g_view[3])(self, sel_registerName("newTextureViewWithPixelFormat:"), f); }
static id v_newRange(id self, SEL c, NSUInteger f, NSUInteger t, NSRange l, NSRange s_, uint64_t idx) { if (!idx_ok(c, idx)) return nil;
  return ((id(*)(id, SEL, NSUInteger, NSUInteger, NSRange, NSRange))g_view[4])(self, sel_registerName("newTextureViewWithPixelFormat:textureType:levels:slices:"), f, t, l, s_); }
static id v_newSwz(id self, SEL c, NSUInteger f, NSUInteger t, NSRange l, NSRange s_, XeSwz sw, uint64_t idx) { if (!idx_ok(c, idx)) return nil;
  return ((id(*)(id, SEL, NSUInteger, NSUInteger, NSRange, NSRange, XeSwz))g_view[5])(self, sel_registerName("newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:"), f, t, l, s_, sw); }
static id rej_id(id self, SEL c) { LOG("%s não implementado", sel_getName(c)); return nil; }
static id rej_id_arg(id self, SEL c, void* x) { return rej_id(self, c); }
static void* rej_ptr(id self, SEL c) { rej_id(self, c); return NULL; }
static void rej_void(id self, SEL c) { LOG("%s não implementado", sel_getName(c)); }
static void rej_indir(id self, SEL c, id r, uint64_t i) { rej_void(self, c); }
static BOOL no_cap(id self, SEL c) { return NO; }
static id rej_archive(id self, SEL c, id d, NSError** e) {
  if (e) *e = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFeatureUnsupportedError userInfo:@{NSLocalizedDescriptionKey: @"Binary archives não suportados no adaptador TGL"}];
  return nil; }
static id rej_dynlib(id self, SEL c, id in, NSError** e) {
  LOG("%s rejeitado (dynamic libraries não suportadas)", sel_getName(c));
  if (e) *e = [NSError errorWithDomain:MTLDynamicLibraryDomain code:MTLDynamicLibraryErrorUnsupported userInfo:@{NSLocalizedDescriptionKey: @"Dynamic libraries não suportadas no adaptador TGL"}];
  return nil; }
static id buf_desc(id self, SEL c, id d) {
  if (!d) return nil;
  uint64_t len = ((uint64_t(*)(id, SEL))objc_msgSend)(d, sel_registerName("length"));
  uint64_t opt = ((uint64_t(*)(id, SEL))objc_msgSend)(d, sel_registerName("resourceOptions"));
  uint64_t addr = ((uint64_t(*)(id, SEL))objc_msgSend)(d, sel_registerName("pinnedGPUAddress"));
  id dealloc = ((id(*)(id, SEL))objc_msgSend)(d, sel_registerName("deallocator"));
  const void* cont = ((void*(*)(id, SEL))objc_msgSend)(d, sel_registerName("contents"));
  uint64_t page = (uint64_t)getpagesize();
  if ((opt & ~0x3ffULL) || (opt & 15) > 1 || ((opt >> 4) & 15) > 2 || ((opt >> 8) & 3) > 2 || addr || dealloc || !len || len > UINT64_MAX - (page - 1)) {
    LOG("newBufferWithDescriptor: opções não suportadas (opt=0x%llx addr=%llx len=%llu)", opt, addr, len); return nil; }
  if (cont) return ((id(*)(id, SEL, const void*, NSUInteger, NSUInteger))g_buf[1])(self, sel_registerName("newBufferWithBytes:length:options:"), cont, len, opt);
  return ((id(*)(id, SEL, NSUInteger, NSUInteger))g_buf[0])(self, sel_registerName("newBufferWithLength:options:"), len, opt);
}

// modo laboratório: o WindowServer não recebe device da GPU (fica em software); programas de teste continuam recebendo
static IMP g_orig_initport;
static id initport_wrapper(id self, SEL c, unsigned int port) {
  if (!strcmp(getprogname(), "WindowServer") && access(LAB_FLAG, F_OK) == 0) {
    static int logged; if (!logged) { logged = 1; LOG("modo laboratório: WindowServer sem MTLDevice da Iris Xe"); }
    [self release]; return nil;
  }
  return ((id(*)(id, SEL, unsigned int))g_orig_initport)(self, c, port);
}

static BOOL install_entries(void) {
  Class dev = objc_getClass("MTLIGAccelDevice"), buf = objc_getClass("MTLIGAccelBuffer"), tex = objc_getClass("MTLIGAccelTexture");
  if (!dev || !buf || !tex) return NO;
  Method anchor = class_getInstanceMethod(dev, sel_registerName("initWithAcceleratorPort:"));
  Dl_info di;
  if (!anchor || !dladdr((void*)method_getImplementation(anchor), &di)) { LOG("initWithAcceleratorPort: ausente"); return NO; }
  g_drv_base = di.dli_fbase;
  void* mh = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
  const uint64_t* sent = mh ? (const uint64_t*)dlsym(mh, "_MTLInvalidResourceIndex") : NULL;
  g_invalid_idx = sent ? *sent : 0;
  const char* leg[6] = {"initWithTexture:pixelFormat:", "initWithTexture:pixelFormat:textureType:levels:slices:", "initWithTexture:pixelFormat:textureType:levels:slices:swizzle:",
    "newTextureViewWithPixelFormat:", "newTextureViewWithPixelFormat:textureType:levels:slices:", "newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:"};
  for (int i = 0; i < 6; i++) { Method m = class_getInstanceMethod(tex, sel_registerName(leg[i])); if (!m) { LOG("textura sem %s", leg[i]); return NO; } g_view[i] = method_getImplementation(m); }
  Method b0 = class_getInstanceMethod(dev, sel_registerName("newBufferWithLength:options:")), b1 = class_getInstanceMethod(dev, sel_registerName("newBufferWithBytes:length:options:"));
  if (!b0 || !b1) return NO; g_buf[0] = method_getImplementation(b0); g_buf[1] = method_getImplementation(b1);
  struct { Class c; const char* sel; const char* t; IMP imp; } E[] = {
    {dev, "getGlobalMemoryBuffer", "@16@0:8", (IMP)rej_id}, {dev, "getGlobalMemoryBuffer:", "@24@0:8^v16", (IMP)rej_id_arg},
    {dev, "getGlobalMemoryBufferHeader", "^v16@0:8", (IMP)rej_ptr}, {dev, "initializeComputeIndirectCommandBuffers", "v16@0:8", (IMP)rej_void},
    {dev, "newBufferWithDescriptor:", "@24@0:8@16", (IMP)buf_desc}, {buf, "generateIndirectionIndex:resourceIndex:", "v32@0:8@16Q24", (IMP)rej_indir},
    {tex, "initWithTexture:pixelFormat:resourceIndex:", "@40@0:8@16Q24Q32", (IMP)v_initFmt},
    {tex, "initWithTexture:pixelFormat:textureType:levels:slices:resourceIndex:", "@80@0:8@16Q24Q32{_NSRange=QQ}40{_NSRange=QQ}56Q72", (IMP)v_initRange},
    {tex, "initWithTexture:pixelFormat:textureType:levels:slices:swizzle:resourceIndex:", "@84@0:8@16Q24Q32{_NSRange=QQ}40{_NSRange=QQ}56{?=CCCC}72Q76", (IMP)v_initSwz},
    {tex, "newTextureViewWithPixelFormat:resourceIndex:", "@32@0:8Q16Q24", (IMP)v_newFmt},
    {tex, "newTextureViewWithPixelFormat:textureType:levels:slices:resourceIndex:", "@72@0:8Q16Q24{_NSRange=QQ}32{_NSRange=QQ}48Q64", (IMP)v_newRange},
    {tex, "newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:resourceIndex:", "@76@0:8Q16Q24{_NSRange=QQ}32{_NSRange=QQ}48{?=CCCC}64Q68", (IMP)v_newSwz}};
  int added = 0;
  for (unsigned i = 0; i < sizeof(E) / sizeof(E[0]); i++) if (class_addMethod(E[i].c, sel_registerName(E[i].sel), E[i].imp, E[i].t)) added++;
  const char* caps[] = {"supportsDynamicAttributeStride", "supportsCMPIndirectCommandBuffers", "supportsMeshShaders", "supportsMeshShadersInICB", "supportsBinaryArchives",
    "supportsAIRNTBinaryArchiveSpecializedFunctions", "supportsAIRNTBinaryArchiveFunctionPointers", "supportsAIRNTBinaryArchiveStitchedFunctions",
    "supportsDynamicLibraries", "supportsRenderDynamicLibraries"};
  for (unsigned i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) class_replaceMethod(dev, sel_registerName(caps[i]), (IMP)no_cap, "c16@0:8");
  class_replaceMethod(dev, sel_registerName("newBinaryArchiveWithDescriptor:error:"), (IMP)rej_archive, "@32@0:8@16^@24");
  class_replaceMethod(dev, sel_registerName("newDynamicLibrary:error:"), (IMP)rej_dynlib, "@32@0:8@16^@24");
  class_replaceMethod(dev, sel_registerName("newDynamicLibraryWithURL:error:"), (IMP)rej_dynlib, "@32@0:8@16^@24");
  g_orig_initport = method_setImplementation(anchor, (IMP)initport_wrapper);
  LOG("%d métodos adicionados, caps/dynlib/archive desligados (índice inválido=%llu, driver base=%p)", added, g_invalid_idx, g_drv_base);
  return YES;
}

static BOOL install_pd(void) {
  Class cp = objc_getClass("MTLRenderPipelineDescriptorInternal"), ca = objc_getClass("MTLRenderPipelineColorAttachmentDescriptorInternal");
  SEL s = sel_registerName("_descriptorPrivate");
  Method mp = cp ? class_getInstanceMethod(cp, s) : NULL, ma = ca ? class_getInstanceMethod(ca, s) : NULL;
  if (!mp || !ma) return NO;
  g_layout_ok = check_layout();
  g_orig_pd = method_setImplementation(mp, (IMP)pd_wrapper);
  g_orig_att = method_setImplementation(ma, (IMP)att_wrapper);
  return YES;
}

// Metal(Tahoe): -[MTLCompiler releaseHashMaps:] crashava com valor inválido no mapa. Mantido da v7: registra e NÃO libera (vaza).
static int safe_read(const void* a, void* buf, size_t n) {
  mach_vm_size_t out = 0; return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)a, n, (mach_vm_address_t)buf, &out) == KERN_SUCCESS && out == n;
}
static void rhm_wrapper(id self, SEL _cmd, void* map) {
  static int logged;
  void* node = NULL; int n = 0;
  if (map && safe_read((char*)map + 0x10, &node, 8)) {
    while (node && n < 64) {
      uint64_t w[4] = {0, 0, 0, 0};
      if (!safe_read(node, w, 32)) { if (logged < 40) LOG("hashMap nó=%p ILEGÍVEL", node); break; }
      uint64_t isa = 0; int ok = w[3] && safe_read((void*)w[3], &isa, 8);
      if (logged < 40) LOG("hashMap[%d] nó=%p k0=%llx k1=%llx valor=%p isa=%s%llx", n, node, w[1], w[2], (void*)w[3], ok ? "" : "ILEGÍVEL/", ok ? isa : 0ULL);
      logged++; node = (void*)w[0]; n++;
    }
  }
}
static BOOL install_rhm(void) {
  Class c = objc_getClass("MTLCompiler"); Method m = c ? class_getInstanceMethod(c, sel_registerName("releaseHashMaps:")) : NULL;
  if (!m) return NO;
  method_setImplementation(m, (IMP)rhm_wrapper); return YES;
}

// v9: registrador de crash. Em SIGSEGV/SIGBUS/SIGABRT/SIGILL grava no log o endereço da falha, registradores e a pilha
// simbolizada (imagem+offset+símbolo), depois devolve o sinal ao comportamento padrão (o processo morre como antes).
static void crash_handler(int sig, siginfo_t* si, void* uc_) {
  ucontext_t* uc = (ucontext_t*)uc_;
  if (uc && uc->uc_mcontext) {
    _STRUCT_X86_THREAD_STATE64* r = &uc->uc_mcontext->__ss;
    LOG("CRASH sinal=%d addr=%p rip=%p rdi=%p rsi=%p rdx=%p rcx=%p rax=%p rbx=%p", sig, si ? si->si_addr : NULL, (void*)r->__rip,
        (void*)r->__rdi, (void*)r->__rsi, (void*)r->__rdx, (void*)r->__rcx, (void*)r->__rax, (void*)r->__rbx);
    Dl_info di;
    if (dladdr((void*)r->__rip, &di)) LOG("CRASH rip em %s+0x%lx (%s)", di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "?",
        (unsigned long)(r->__rip - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "?");
    // pilha a partir da falha: segue a cadeia de rbp (frames com frame pointer)
    uint64_t fp = r->__rbp;
    for (int i = 0; i < 32 && fp; i++) {
      uint64_t fr[2];
      if (!safe_read((void*)fp, fr, 16) || !fr[1]) break;
      if (dladdr((void*)fr[1], &di) && di.dli_fname)
        LOG("CRASH f%d %s+0x%lx %s", i, strrchr(di.dli_fname, '/') + 1, (unsigned long)(fr[1] - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "?");
      else LOG("CRASH f%d %p", i, (void*)fr[1]);
      if (fr[0] <= fp) break;
      fp = fr[0];
    }
  }
  void* bt[48]; int n = backtrace(bt, 48);
  for (int i = 0; i < n; i++) {
    Dl_info di;
    if (dladdr(bt[i], &di) && di.dli_fname)
      LOG("CRASH #%d %s+0x%lx %s", i, strrchr(di.dli_fname, '/') + 1, (unsigned long)((uintptr_t)bt[i] - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "?");
    else LOG("CRASH #%d %p", i, bt[i]);
  }
  signal(sig, SIG_DFL);
  if (sig == SIGABRT) raise(sig);   // nos outros, retornar refaz a instrução e o processo morre com o sinal original
}
static void install_crash(void) {
  struct sigaction sa; memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT};
  for (unsigned i = 0; i < 4; i++) sigaction(sigs[i], &sa, NULL);
}

// v12 (19/10): sem compressão de textura. O hang do RCS era um render-target FAST CLEAR (3DSTATE_PS com
// "Render Target Fast Clear Enable") e um clear de stencil comprimido (3DSTATE_WM_HZ_OP), que dependem da
// tabela AUX (AUX-TT), não configurada no RPL. O jeito oficial do Metal de pedir "sem compressão" é
// allowGPUOptimizedContents = NO no descritor; o driver TGL consulta essa propriedade.
static IMP g_tex_dev, g_tex_dev_ios, g_tex_heap, g_tex_heap_off, g_heap_size;
static int g_nocomp_logs;
static MTLTextureDescriptor* sem_compressao(MTLTextureDescriptor* d) {
  if (!d || ![d respondsToSelector:@selector(setAllowGPUOptimizedContents:)]) return d;
  MTLTextureDescriptor* c = [[d copy] autorelease];
  c.allowGPUOptimizedContents = NO;
  if (0 && g_nocomp_logs < 8) { g_nocomp_logs++; LOG("textura %lux%lu fmt=%lu usage=0x%lx: allowGPUOptimizedContents -> NO",
      (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)d.usage); }
  return c;
}
// v13: registrador de texturas. Conta por (caminho, formato Metal, formato da IOSurface, ok/nil) e grava as primeiras
// ocorrências de cada combinação + todas as falhas (o driver devolve nil).
#define TSTAT 256
static struct { int via; unsigned long fmt; uint32_t iosf; int ok; long n; } g_ts[TSTAT]; static int g_tsn;
static pthread_mutex_t g_tsm = PTHREAD_MUTEX_INITIALIZER;
static void tex_stat(int via, MTLTextureDescriptor* d, void* io, NSUInteger pl, id r) {
  if (!d) return;
  uint32_t iosf = io ? IOSurfaceGetPixelFormat((IOSurfaceRef)io) : 0;
  int ok = r != nil, i; long n = 0;
  pthread_mutex_lock(&g_tsm);
  for (i = 0; i < g_tsn; i++) if (g_ts[i].via == via && g_ts[i].fmt == d.pixelFormat && g_ts[i].iosf == iosf && g_ts[i].ok == ok) break;
  if (i == g_tsn && g_tsn < TSTAT) { g_ts[i].via = via; g_ts[i].fmt = d.pixelFormat; g_ts[i].iosf = iosf; g_ts[i].ok = ok; g_tsn++; }
  if (i < TSTAT) n = ++g_ts[i].n;
  pthread_mutex_unlock(&g_tsm);
  if (n <= 3 || !ok || (n & (n - 1)) == 0)
    LOG("TEX via=%s fmt=%lu %lux%lu mips=%lu usage=0x%lx storage=%lu type=%lu ios=0x%08x plano=%lu -> %s (n=%ld)",
        via == 0 ? "dev" : via == 1 ? "iosurface" : via == 2 ? "heap" : "heapoff", (unsigned long)d.pixelFormat,
        (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.mipmapLevelCount, (unsigned long)d.usage,
        (unsigned long)d.storageMode, (unsigned long)d.textureType,
        iosf,
        (unsigned long)pl, ok ? "ok" : "NIL", n);
}
// v17 (21/10): RELÓGIO DA TELA DE BLOQUEIO. O driver 16.0.32 aloca texturas Stencil8 pelo device com alinhamento de 4 KB
// (todas as outras texturas tiled ficam em 64 KB; o próprio heapTextureSizeAndAlign do driver diz 64 KB para Stencil8).
// O stencil então começa no último bloco de 64 KB de um render target de 32/64 bpp alocado logo antes. Se esse render
// target e o stencil são usados no MESMO render pass, a parte do stencil dentro desse bloco perde escritas (linhas de
// cache ímpares lidas como 0). O RenderBox (vidro do relógio, textos com Liquid Glass) aloca em sequência c0 R16F, c1
// RG16F e o stencil, e desenha os três no mesmo pass: o teste de stencil falha no topo das letras -> faixas.
// Prova: ~/xe/build/glitch (stenciltest: 5/5 falhas -> 0 com stencil alinhado; glasstext + passdump).
// Correção: textura privada de render target cujo endereço GPU não é múltiplo de 64 KB é recriada num MTLHeap próprio
// (tamanho arredondado para 64 KB, hazard tracking ligado), que começa em 64 KB e ocupa blocos inteiros. XE_STENCIL_NATIVO=1 desliga.
static int g_s64_off = -1; static long g_s64_n;
static uint64_t xe_gpuaddr(id t) {
  static SEL sg; if (!sg) sg = sel_registerName("gpuAddress");
  return [t respondsToSelector:sg] ? ((uint64_t(*)(id, SEL))objc_msgSend)(t, sg) : 0;
}
static id xe_realinha64k(id dev, id r, MTLTextureDescriptor* d) {
  if (g_s64_off < 0) g_s64_off = getenv("XE_STENCIL_NATIVO") != NULL;
  if (g_s64_off || !r || !d || d.storageMode != MTLStorageModePrivate || !(d.usage & MTLTextureUsageRenderTarget)) return r;
  MTLPixelFormat f = d.pixelFormat;
  if (f != MTLPixelFormatStencil8 && f != MTLPixelFormatX32_Stencil8) return r;
  uint64_t ga = xe_gpuaddr(r);
  if (!ga || !(ga & 0xffff)) return r;
  MTLSizeAndAlign sa = ((MTLSizeAndAlign(*)(id, SEL, id))objc_msgSend)(dev, sel_registerName("heapTextureSizeAndAlignWithDescriptor:"), d);
  if (!sa.size) return r;
  MTLHeapDescriptor* hd = [[MTLHeapDescriptor new] autorelease];
  hd.storageMode = MTLStorageModePrivate; hd.cpuCacheMode = d.cpuCacheMode; hd.size = (sa.size + 0xffff) & ~(NSUInteger)0xffff;
  if ([hd respondsToSelector:@selector(setHazardTrackingMode:)]) hd.hazardTrackingMode = MTLHazardTrackingModeTracked;
  id<MTLHeap> h = [dev newHeapWithDescriptor:hd];
  id t = h ? [h newTextureWithDescriptor:d] : nil;
  uint64_t gb = t ? xe_gpuaddr(t) : 0;
  long n = __sync_add_and_fetch(&g_s64_n, 1);
  if (!t || (gb & 0xffff)) {
    if (n <= 8) LOG("STENCIL64K %lux%lu fmt=%lu em 0x%llx: heap falhou (heap=%p tex=%p 0x%llx), mantida a original", (unsigned long)d.width,
        (unsigned long)d.height, (unsigned long)f, ga, h, t, gb);
    [t release]; [h release]; return r;
  }
  if (r && [r label]) [t setLabel:[r label]];
  [r release]; [h release];   // a textura mantém o heap vivo
  if (n <= 4 || (n & (n - 1)) == 0) LOG("STENCIL64K %lux%lu fmt=%lu estava em 0x%llx (não alinhado a 64 KB) -> heap próprio em 0x%llx (n=%ld)",
      (unsigned long)d.width, (unsigned long)d.height, (unsigned long)f, ga, gb, n);
  return t;
}
static id w_tex_dev(id s, SEL c, MTLTextureDescriptor* d) { MTLTextureDescriptor* dd = sem_compressao(d); id r = ((id(*)(id, SEL, id))g_tex_dev)(s, c, dd);
  r = xe_realinha64k(s, r, dd); tex_stat(0, d, NULL, 0, r); return r; }
// v18 (23/10): IOSurface com bytesPerRow >= 32768. O kernel não usa a IOSurface no lugar: a GPU trabalha num "cache"
// Y-tiled e IGAccelResource::pageon/pageoff copiam com IntelAccelerator::submitBlit. O blit 2D só aceita pitch linear
// até 0x7ffe; acima disso vai para o Blit3D, que não funciona neste spoof (o mesmo do BLITALIGN no NootedGreen): a
// GPU lê zeros e o que ela escreve nunca volta. Sintoma: RGhA >= 4096 px de largura sai preta (AVAssetImageGenerator
// do Aerial de 5632 px, wallpaper da tela de login). Correção: textura linear sobre um MTLBuffer no-copy que aponta
// para a própria memória da IOSurface, sem cache nem blit. Só 2D, 1 mip, 1 amostra, pitch/offset alinhados ao que o
// driver exige. Desliga com XE_NOWIDEPITCH.
static long g_wide_n;
static id xe_tex_largo(id dev, MTLTextureDescriptor* d, IOSurfaceRef s, NSUInteger pl) {
  if (!s || !d || getenv("XE_NOWIDEPITCH")) return nil;
  size_t np = IOSurfaceGetPlaneCount(s);
  size_t bpr = np ? IOSurfaceGetBytesPerRowOfPlane(s, pl) : IOSurfaceGetBytesPerRow(s);
  if (bpr < 32768 || d.textureType != MTLTextureType2D || d.mipmapLevelCount != 1 || d.sampleCount != 1) return nil;
  NSUInteger al = [dev minimumLinearTextureAlignmentForPixelFormat:d.pixelFormat];
  char* base = IOSurfaceGetBaseAddress(s);
  size_t off = np ? (size_t)((char*)IOSurfaceGetBaseAddressOfPlane(s, pl) - base) : 0;
  size_t len = (IOSurfaceGetAllocSize(s) + 4095) & ~(size_t)4095;
  if (!al || ((uintptr_t)base & 4095) || (bpr % al) || (off % al)) {
    if (__sync_add_and_fetch(&g_wide_n, 1) <= 8) LOG("PITCH LARGO bpr=%zu off=%zu al=%lu: não alinhado, caminho original", bpr, off, (unsigned long)al);
    return nil;
  }
  CFRetain(s);
  id<MTLBuffer> b = [dev newBufferWithBytesNoCopy:base length:len options:MTLResourceStorageModeShared
                                      deallocator:^(void* p, NSUInteger l) { CFRelease(s); }];
  if (!b) { CFRelease(s); return nil; }
  MTLTextureDescriptor* dd = [d copy];
  dd.storageMode = MTLStorageModeShared;
  dd.resourceOptions = (dd.resourceOptions & ~MTLResourceStorageModeMask) | MTLResourceStorageModeShared;
  id t = [b newTextureWithDescriptor:dd offset:off bytesPerRow:bpr];
  [dd release]; [b release];
  long n = __sync_add_and_fetch(&g_wide_n, 1);
  if (n <= 8 || (n & (n - 1)) == 0) LOG("PITCH LARGO %lux%lu fmt=%lu plano=%lu bpr=%zu -> textura linear %p (n=%ld)", (unsigned long)d.width,
      (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)pl, bpr, t, n);
  return t;
}
static id w_tex_dev_ios(id s, SEL c, MTLTextureDescriptor* d, void* io, NSUInteger pl) {
  MTLTextureDescriptor* dd = sem_compressao(d);
  id r = xe_tex_largo(s, dd, (IOSurfaceRef)io, pl);
  if (!r) r = ((id(*)(id, SEL, id, void*, NSUInteger))g_tex_dev_ios)(s, c, dd, io, pl);
  tex_stat(1, d, io, pl, r); return r; }
static id w_tex_heap(id s, SEL c, MTLTextureDescriptor* d) { id r = ((id(*)(id, SEL, id))g_tex_heap)(s, c, sem_compressao(d)); tex_stat(2, d, NULL, 0, r); return r; }
static id w_tex_heap_off(id s, SEL c, MTLTextureDescriptor* d, NSUInteger off) { id r = ((id(*)(id, SEL, id, NSUInteger))g_tex_heap_off)(s, c, sem_compressao(d), off); tex_stat(3, d, NULL, 0, r); return r; }
static MTLSizeAndAlign w_heap_size(id s, SEL c, MTLTextureDescriptor* d) { return ((MTLSizeAndAlign(*)(id, SEL, id))g_heap_size)(s, c, sem_compressao(d)); }
static IMP troca(Class k, const char* sel, IMP nova) {
  Method m = k ? class_getInstanceMethod(k, sel_registerName(sel)) : NULL;
  if (!m) { LOG("sem %s", sel); return NULL; }
  return method_setImplementation(m, nova);
}
static void install_nocomp(void) {
  if (getenv("XE_COMPRESSAO")) { LOG("compressão mantida (XE_COMPRESSAO)"); return; }
  Class dev = objc_getClass("MTLIGAccelDevice"), heap = objc_getClass("MTLIGAccelHeap");
  g_tex_dev = troca(dev, "newTextureWithDescriptor:", (IMP)w_tex_dev);
  g_tex_dev_ios = troca(dev, "newTextureWithDescriptor:iosurface:plane:", (IMP)w_tex_dev_ios);
  g_tex_heap = troca(heap, "newTextureWithDescriptor:", (IMP)w_tex_heap);
  g_tex_heap_off = troca(heap, "newTextureWithDescriptor:atOffset:", (IMP)w_tex_heap_off);
  g_heap_size = troca(dev, "heapTextureSizeAndAlignWithDescriptor:", (IMP)w_heap_size);
  LOG("sem compressão: dev=%d ios=%d heap=%d heapoff=%d heapsize=%d", !!g_tex_dev, !!g_tex_dev_ios, !!g_tex_heap, !!g_tex_heap_off, !!g_heap_size);
}


// v14 (20/10): registra criação de pipelines (render/compute) em _MTLDevice; grava TODAS as falhas com o erro do
// compilador e uma amostra dos sucessos (funções vertex/fragment, formatos de cor).
static IMP g_rp_err, g_rp_opt, g_rp_cb, g_rp_optcb, g_cp_err, g_cp_fn;
static long g_rp_ok, g_rp_fail;
static void rp_log(MTLRenderPipelineDescriptor* d, id r, NSError* e, const char* via) {
  if (r) { long n = __sync_add_and_fetch(&g_rp_ok, 1); if (n > 6 && (n & (n - 1))) return; }
  else __sync_add_and_fetch(&g_rp_fail, 1);
  NSString* vf = d.vertexFunction.name, * ff = d.fragmentFunction.name;
  MTLPixelFormat c0 = d.colorAttachments[0].pixelFormat;
  LOG("PIPE %s %s vf=%s ff=%s cor0=%lu blend=%d ok=%ld falhas=%ld %s", via, r ? "ok" : "FALHOU", vf ? vf.UTF8String : "-",
      ff ? ff.UTF8String : "-", (unsigned long)c0, (int)d.colorAttachments[0].blendingEnabled, g_rp_ok, g_rp_fail,
      e ? [[e description] substringToIndex:MIN((NSUInteger)600, [[e description] length])].UTF8String : "");
}
static id w_rp_err(id s, SEL c, MTLRenderPipelineDescriptor* d, NSError** e) {
  NSError* le = nil; id r = ((id(*)(id, SEL, id, NSError**))g_rp_err)(s, c, d, &le); rp_log(d, r, le, "sync"); if (e) *e = le; return r; }
static id w_rp_opt(id s, SEL c, MTLRenderPipelineDescriptor* d, NSUInteger o, id* refl, NSError** e) {
  NSError* le = nil; id r = ((id(*)(id, SEL, id, NSUInteger, id*, NSError**))g_rp_opt)(s, c, d, o, refl, &le); rp_log(d, r, le, "sync-opt"); if (e) *e = le; return r; }
static void w_rp_cb(id s, SEL c, MTLRenderPipelineDescriptor* d, void (^h)(id, NSError*)) {
  MTLRenderPipelineDescriptor* dd = [d retain];
  void (^h2)(id, NSError*) = ^(id r, NSError* e) { rp_log(dd, r, e, "async"); [dd release]; if (h) h(r, e); };
  ((void(*)(id, SEL, id, id))g_rp_cb)(s, c, d, h2); }
static void w_rp_optcb(id s, SEL c, MTLRenderPipelineDescriptor* d, NSUInteger o, void (^h)(id, id, NSError*)) {
  MTLRenderPipelineDescriptor* dd = [d retain];
  void (^h2)(id, id, NSError*) = ^(id r, id refl, NSError* e) { rp_log(dd, r, e, "async-opt"); [dd release]; if (h) h(r, refl, e); };
  ((void(*)(id, SEL, id, NSUInteger, id))g_rp_optcb)(s, c, d, o, h2); }
static id w_cp_err(id s, SEL c, id fn, NSError** e) {
  NSError* le = nil; id r = ((id(*)(id, SEL, id, NSError**))g_cp_err)(s, c, fn, &le);
  if (!r) LOG("COMPUTE FALHOU fn=%s %s", [[fn name] UTF8String], le ? [[le description] UTF8String] : "");
  if (e) *e = le; return r; }
static void install_pipelog(void) {
  Class D = objc_getClass("_MTLDevice"); if (!D) { LOG("sem _MTLDevice"); return; }
  g_rp_err = troca(D, "newRenderPipelineStateWithDescriptor:error:", (IMP)w_rp_err);
  g_rp_opt = troca(D, "newRenderPipelineStateWithDescriptor:options:reflection:error:", (IMP)w_rp_opt);
  g_rp_cb = troca(D, "newRenderPipelineStateWithDescriptor:completionHandler:", (IMP)w_rp_cb);
  g_rp_optcb = troca(D, "newRenderPipelineStateWithDescriptor:options:completionHandler:", (IMP)w_rp_optcb);
  g_cp_err = troca(D, "newComputePipelineStateWithFunction:error:", (IMP)w_cp_err);
  LOG("registrador de pipelines: %d %d %d %d %d", !!g_rp_err, !!g_rp_opt, !!g_rp_cb, !!g_rp_optcb, !!g_cp_err);
}

// v15 (20/10): ÍCONES VAZIOS. O CoreAnimation cria texturas zero-copy sobre a memória das imagens com
// -[MTLIOAccelDevice newTiledTextureWithBytesNoCopy:length:(deallocator:)descriptor:offset:bytesPerRow:]; para imagens no
// espaço de cor do destino o offset é 0x20 (cabeçalho do buffer da imagem) e a GPU (driver TGL no RPL) não lê textura
// linear nesse offset -> camada invisível (ícones do Finder, fundos de DMG, conteúdo f16 estendido). Com offset não
// alinhado a 256, cria textura própria (managed) com CÓPIA dos pixels; o deallocator do chamador roda quando a nossa
// textura for liberada. Provado com ~/xe/build/xe-carender (CARenderer). XE_NOCOPY_ZERO=1 desliga.
@interface XeDeallocV15 : NSObject { void (^_b)(void*, NSUInteger); void* _p; NSUInteger _n; } - (id)initWithBlock:(id)b ptr:(void*)p len:(NSUInteger)n; @end
@implementation XeDeallocV15
- (id)initWithBlock:(id)b ptr:(void*)p len:(NSUInteger)n { if ((self = [super init])) { _b = [b copy]; _p = p; _n = n; } return self; }
- (void)dealloc { if (_b) { _b(_p, _n); [_b release]; } [super dealloc]; }
@end
static IMP g_nc3, g_nc4; static long g_nc_n;
static const void* kNcDealloc = &kNcDealloc;
static BOOL nc_precisa(MTLTextureDescriptor* d, NSUInteger off) {
  return (off & 255) != 0 && d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 && d.arrayLength == 1;
}
static id nc_copia(id dev, void* ptr, MTLTextureDescriptor* d, NSUInteger off, NSUInteger bpr) {
  MTLTextureDescriptor* c = [[d copy] autorelease]; c.storageMode = MTLStorageModeManaged;
  id<MTLTexture> t = [dev newTextureWithDescriptor:c];
  if (!t) return nil;
  [t replaceRegion:MTLRegionMake2D(0, 0, d.width, d.height) mipmapLevel:0 withBytes:(uint8_t*)ptr + off bytesPerRow:bpr];
  long n = __sync_add_and_fetch(&g_nc_n, 1);
  if (n <= 4 || (n & (n - 1)) == 0) LOG("NOCOPY textura %lux%lu fmt=%lu off=0x%lx bpr=%lu -> cópia alinhada (n=%ld)", (unsigned long)d.width,
      (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)off, (unsigned long)bpr, n);
  return t;
}
// v19 (25/10): BADGE DO DOCK SEM NÚMERO (#10). Para camadas com minificationFilter trilinear o CoreAnimation pede a
// mesma textura zero-copy com mipmapLevelCount > 1 (108x108 -> 7 níveis) sobre a memória da imagem, que só tem o nível 0;
// o driver 16.0.32 não gera os outros níveis e a amostragem trilinear lê níveis vazios -> camada invisível (o número do
// badge, qualquer imagem reduzida com filtro trilinear). Provado com ~/xe/build/badge/trace.m (CARenderer: trilinear =
// cor do fundo, linear/nearest = imagem). Correção: textura própria (managed) com os mesmos níveis, nível 0 copiado e
// os demais gerados com generateMipmapsForTexture: numa fila própria antes de devolver. XE_MIP_NATIVO=1 desliga.
static long g_mip_n; static int g_mip_off = -1;
static const void* kXeMipQ = &kXeMipQ;
static pthread_mutex_t g_mipm = PTHREAD_MUTEX_INITIALIZER;
static BOOL nc_mip(MTLTextureDescriptor* d) {
  if (g_mip_off < 0) g_mip_off = getenv("XE_MIP_NATIVO") != NULL;
  return !g_mip_off && d.mipmapLevelCount > 1 && d.textureType == MTLTextureType2D && d.arrayLength == 1 && d.sampleCount == 1;
}
static id nc_copia_mip(id dev, void* ptr, MTLTextureDescriptor* d, NSUInteger off, NSUInteger bpr) {
  MTLTextureDescriptor* c = [[d copy] autorelease]; c.storageMode = MTLStorageModeManaged;
  c.usage = c.usage | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
  id<MTLTexture> t = [dev newTextureWithDescriptor:c];
  if (!t) return nil;
  [t replaceRegion:MTLRegionMake2D(0, 0, d.width, d.height) mipmapLevel:0 withBytes:(uint8_t*)ptr + off bytesPerRow:bpr];
  pthread_mutex_lock(&g_mipm);
  id<MTLCommandQueue> q = objc_getAssociatedObject(dev, kXeMipQ);
  if (!q) { q = [dev newCommandQueue]; if (q) { objc_setAssociatedObject(dev, kXeMipQ, q, OBJC_ASSOCIATION_RETAIN); [q release]; } }
  pthread_mutex_unlock(&g_mipm);
  id<MTLCommandBuffer> cb = [q commandBuffer];
  id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
  if (!cb || !e) { [t release]; return nil; }
  [e generateMipmapsForTexture:t]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
  long n = __sync_add_and_fetch(&g_mip_n, 1);
  if (n <= 4 || (n & (n - 1)) == 0) LOG("MIP textura %lux%lu fmt=%lu níveis=%lu off=0x%lx -> cópia com mipmaps gerados (n=%ld, cb=%ld)",
      (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)d.mipmapLevelCount,
      (unsigned long)off, n, (long)cb.status);
  return t;
}
static id w_nc3(id s, SEL c, void* ptr, NSUInteger len, MTLTextureDescriptor* d, NSUInteger off, NSUInteger bpr) {
  if (nc_mip(d)) { id t = nc_copia_mip(s, ptr, d, off, bpr); if (t) return t; }
  if (nc_precisa(d, off)) { id t = nc_copia(s, ptr, d, off, bpr); if (t) return t; }
  return ((id(*)(id, SEL, void*, NSUInteger, id, NSUInteger, NSUInteger))g_nc3)(s, c, ptr, len, d, off, bpr);
}
static id w_nc4(id s, SEL c, void* ptr, NSUInteger len, id dealloc, MTLTextureDescriptor* d, NSUInteger off, NSUInteger bpr) {
  if (nc_mip(d) || nc_precisa(d, off)) {
    id t = nc_mip(d) ? nc_copia_mip(s, ptr, d, off, bpr) : nc_copia(s, ptr, d, off, bpr);
    if (t) {
      if (dealloc) { XeDeallocV15* x = [[XeDeallocV15 alloc] initWithBlock:dealloc ptr:ptr len:len]; objc_setAssociatedObject(t, kNcDealloc, x, OBJC_ASSOCIATION_RETAIN); [x release]; }
      return t;
    }
  }
  return ((id(*)(id, SEL, void*, NSUInteger, id, id, NSUInteger, NSUInteger))g_nc4)(s, c, ptr, len, dealloc, d, off, bpr);
}
static void install_nocopyfix(void) {
  if (getenv("XE_NOCOPY_ZERO")) { LOG("nocopyfix desligado (XE_NOCOPY_ZERO)"); g_nc3 = (IMP)1; return; }
  Class k = objc_getClass("MTLIOAccelDevice"); if (!k) return;
  g_nc3 = troca(k, "newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:", (IMP)w_nc3);
  g_nc4 = troca(k, "newTiledTextureWithBytesNoCopy:length:deallocator:descriptor:offset:bytesPerRow:", (IMP)w_nc4);
  LOG("nocopyfix: %d %d", !!g_nc3, !!g_nc4);
}
// v16 (21/10): CHROME SEM ÍCONES / LOGO COMO BARRA. No driver 16.0.32 o copyFromBuffer:toBuffer: e o fillBuffer usam dois
// caminhos (IGBL::shouldUse2DBlit, driver+0x8bf48): "2D" (comandos do blitter, segmento próprio do command buffer) quando
// (srcOffset - dstOffset) % 64 == 0 e para fills; "3D" (cópia por shader) quando o deslocamento relativo não é múltiplo de 64.
// Neste sistema o trabalho 3D de um command buffer NÃO espera as escritas 2D anteriores do MESMO command buffer (a
// sincronização entre segmentos não funciona): fill/cópia alinhada seguidos de cópia desalinhada ou render -> corrida.
// O Dawn (Chrome/Graphite) zera cada buffer novo com fillBuffer e logo depois sobe os dados com cópias quase todas
// desalinhadas no mesmo encoder: o zero chegava DEPOIS e o index buffer estático do Graphite ficava zerado -> rrects,
// paths e ícones sumiam (por processo, ao acaso; com cache cheio o Chrome inicia mais rápido e perde quase sempre).
// Correção: nenhuma escrita em buffer usa mais o caminho 2D. fillBuffer vira cópia de um buffer-padrão (compartilhado) com
// deslocamento relativo 4, e a cópia alinhada vira duas cópias desalinhadas por um temporário (abaixo); tudo no caminho 3D,
// que fica em ordem com as cópias e renders seguintes. Provas: ~/xe/build/chromecache/tests (fillcopy, paths, fcoff,
// blitrender). XE_FILL_NATIVO=1 desliga tudo; XE_COPIA_NATIVA=1 desliga só a parte das cópias. A causa de fundo (fence
// entre segmentos 2D/3D) é do lado do kernel e continua lá para cópias buffer<->textura pelo caminho 2D (não vistas falhar).
#define XE_PAT_LEN 65536
static IMP g_fill, g_fill4, g_b2b; static long g_fill_n;
static Class g_igblit;
static const void* kXePat = &kXePat;
static pthread_mutex_t g_patm = PTHREAD_MUTEX_INITIALIZER;
static id<MTLBuffer> xe_pattern(id<MTLDevice> dev, uint32_t pat4) {
  pthread_mutex_lock(&g_patm);
  NSMutableDictionary* m = objc_getAssociatedObject(dev, kXePat);
  if (!m) { m = [NSMutableDictionary dictionary]; objc_setAssociatedObject(dev, kXePat, m, OBJC_ASSOCIATION_RETAIN); }
  id<MTLBuffer> b = m[@(pat4)];
  if (!b) {
    b = [dev newBufferWithLength:XE_PAT_LEN + 64 options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
    if (b) { uint32_t* p = (uint32_t*)b.contents; for (NSUInteger i = 0; i < (XE_PAT_LEN + 64) / 4; i++) p[i] = pat4; m[@(pat4)] = b; [b release]; }
  }
  pthread_mutex_unlock(&g_patm);
  return b;
}
static BOOL xe_fill_as_copy(id enc, id<MTLBuffer> buf, NSRange r, uint32_t pat4) {
  if (!g_b2b || !buf || !r.length || ![enc isKindOfClass:g_igblit]) return NO;
  id<MTLBuffer> p = xe_pattern(buf.device, pat4); if (!p) return NO;
  SEL cs = sel_registerName("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:");
  for (NSUInteger off = 0; off < r.length; off += XE_PAT_LEN) {
    NSUInteger dst = r.location + off, n = MIN((NSUInteger)XE_PAT_LEN, r.length - off), so = (dst + 4) & 63;   // (so - dst) % 64 == 4
    ((void(*)(id, SEL, id, NSUInteger, id, NSUInteger, NSUInteger))g_b2b)(enc, cs, p, so, buf, dst, n);
  }
  long k = __sync_add_and_fetch(&g_fill_n, 1);
  if (k <= 3 || (k & (k - 1)) == 0) LOG("FILL buffer len=%lu range=%lu+%lu padrão=%08x -> cópia (caminho 3D) (n=%ld)", (unsigned long)buf.length,
      (unsigned long)r.location, (unsigned long)r.length, pat4, k);
  return YES;
}
static void w_fill(id s, SEL c, id<MTLBuffer> b, NSRange r, uint8_t v) {
  if (xe_fill_as_copy(s, b, r, v * 0x01010101u)) return;
  ((void(*)(id, SEL, id, NSRange, uint8_t))g_fill)(s, c, b, r, v); }
static void w_fill4(id s, SEL c, id<MTLBuffer> b, NSRange r, uint32_t v) {
  if (!(r.location & 3) && !(r.length & 3) && xe_fill_as_copy(s, b, r, v)) return;
  ((void(*)(id, SEL, id, NSRange, uint32_t))g_fill4)(s, c, b, r, v); }
// Cópia ALINHADA buffer->buffer (caminho 2D) vira duas cópias desalinhadas (caminho 3D) por um buffer temporário:
// src -> tmp[t] -> dst com t = (so + 4) % 64, assim (so - t) % 64 = 60 e (t - do) % 64 = 4. Tudo no caminho 3D fica em
// ordem (3D->3D e 3D->render testados). O temporário vem de um pool por device e volta ao pool quando o command buffer
// termina (addCompletedHandler). XE_COPIA_NATIVA=1 desliga só esta parte.
#define XE_TMP_SMALL (65536 + 128)
#define XE_TMP_BIG ((4u << 20) + 128)
static int g_copia_nativa; static long g_b2b_n;
static const void* kXeTmp = &kXeTmp;
static id<MTLBuffer> xe_tmp_get(id<MTLDevice> dev, NSUInteger need) {
  NSUInteger sz = need <= XE_TMP_SMALL ? XE_TMP_SMALL : XE_TMP_BIG;
  id<MTLBuffer> b = nil;
  pthread_mutex_lock(&g_patm);
  NSMutableArray* pool = objc_getAssociatedObject(dev, kXeTmp);
  if (!pool) { pool = [NSMutableArray array]; objc_setAssociatedObject(dev, kXeTmp, pool, OBJC_ASSOCIATION_RETAIN); }
  for (NSUInteger i = 0; i < pool.count; i++) if ([pool[i] length] == sz) { b = [pool[i] retain]; [pool removeObjectAtIndex:i]; break; }
  pthread_mutex_unlock(&g_patm);
  if (!b) b = [dev newBufferWithLength:sz options:MTLResourceStorageModePrivate];
  return b;   // +1
}
static void xe_tmp_put(id<MTLDevice> dev, id<MTLBuffer> b) {
  pthread_mutex_lock(&g_patm);
  NSMutableArray* pool = objc_getAssociatedObject(dev, kXeTmp);
  if (pool && pool.count < 8) [pool addObject:b];
  pthread_mutex_unlock(&g_patm);
}
static void w_b2b(id s, SEL c, id<MTLBuffer> src, NSUInteger so, id<MTLBuffer> dst, NSUInteger dof, NSUInteger n) {
  typedef void (*CP)(id, SEL, id, NSUInteger, id, NSUInteger, NSUInteger);
  if (g_copia_nativa || !n || ((so - dof) & 63) || !src || !dst || ![s isKindOfClass:g_igblit]) { ((CP)g_b2b)(s, c, src, so, dst, dof, n); return; }
  id<MTLCommandBuffer> cb = [s respondsToSelector:@selector(commandBuffer)] ? ((id(*)(id, SEL))objc_msgSend)(s, @selector(commandBuffer)) : nil;
  id<MTLDevice> dev = dst.device;
  NSUInteger chunk = n <= XE_TMP_SMALL - 128 ? n : (XE_TMP_BIG - 128);
  id<MTLBuffer> tmp = cb ? xe_tmp_get(dev, chunk + 128) : nil;
  if (!tmp) { ((CP)g_b2b)(s, c, src, so, dst, dof, n); return; }
  for (NSUInteger off = 0; off < n; off += chunk) {
    NSUInteger k = MIN(chunk, n - off), t = (so + off + 4) & 63;
    ((CP)g_b2b)(s, c, src, so + off, tmp, t, k);
    ((CP)g_b2b)(s, c, tmp, t, dst, dof + off, k);
  }
  [cb addCompletedHandler:^(id<MTLCommandBuffer> done) { xe_tmp_put(dev, tmp); [tmp release]; }];
  long q = __sync_add_and_fetch(&g_b2b_n, 1);
  if (q <= 3 || (q & (q - 1)) == 0) LOG("COPIA alinhada %lu bytes (%lu->%lu) -> 2 cópias pelo caminho 3D (n=%ld)", (unsigned long)n, (unsigned long)so, (unsigned long)dof, q);
}
static void install_fillfix(void) {
  if (getenv("XE_FILL_NATIVO")) { LOG("fillfix desligado (XE_FILL_NATIVO)"); g_fill = (IMP)1; return; }
  Class k = objc_getClass("MTLIGAccelBlitCommandEncoder"), io = objc_getClass("MTLIOAccelBlitCommandEncoder");
  Method cm = k ? class_getInstanceMethod(k, sel_registerName("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:")) : NULL;
  if (!cm) return;
  static int done; if (done) return; done = 1;
  g_igblit = k; g_b2b = method_getImplementation(cm);
  g_fill = troca(k, "fillBuffer:range:value:", (IMP)w_fill);
  if (io) g_fill4 = troca(io, "fillBuffer:range:pattern4:", (IMP)w_fill4);
  g_copia_nativa = getenv("XE_COPIA_NATIVA") != NULL;
  method_setImplementation(cm, (IMP)w_b2b);
  LOG("fillfix: fill=%d fill4=%d cópia2D->3D=%d", !!g_fill, !!g_fill4, !g_copia_nativa);
  if (!g_fill) g_fill = (IMP)1;   // sem o método: não tentar de novo
}

// fallback: só se algo não existia no constructor (não deveria acontecer: o Metal é carregado antes do driver)
static void* late(void* a) {
  int e = 0, p = 0, r = 0;
  for (int t = 0; t < 20000 && !(e && p && r && g_tex_dev && g_fill); t++) {
    if (!e) e = install_entries(); if (!p) p = install_pd(); if (!r) r = install_rhm();
    if (!g_tex_dev && objc_getClass("MTLIGAccelDevice")) install_nocomp();
    if (!g_nc3 && objc_getClass("MTLIOAccelDevice")) install_nocopyfix();
    if (!g_fill && objc_getClass("MTLIGAccelBlitCommandEncoder")) install_fillfix();
    if (!(e && p && r && g_tex_dev && g_fill)) usleep(500);
  }
  LOG("instalação tardia: entries=%d pd=%d rhm=%d", e, p, r);
  return NULL;
}
__attribute__((constructor)) static void xedrv17_init(void) {
#ifdef SO_PIPELOG
  install_pipelog(); return;
#endif
  install_crash();
  install_nocomp();
  install_pipelog();
  install_nocopyfix();
  install_fillfix();
  int e = install_entries(), p = install_pd(), r = install_rhm();
  LOG("carregada em %s[%d]: entries=%d pd=%d rhm=%d layout=%d lab=%d", getprogname(), getpid(), e, p, r, g_layout_ok, access(LAB_FLAG, F_OK) == 0);
  if (!(e && p && r && g_tex_dev && g_fill)) { pthread_t th; pthread_create(&th, NULL, late, NULL); pthread_detach(th); }
}
