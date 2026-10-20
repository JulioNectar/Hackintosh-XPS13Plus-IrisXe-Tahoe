// Adaptador: o MTLCompiler do Tahoe chama MTLCompilerBuildRequestWithSerializedBitcode(compiler,input,rdx,flags,bitcode,size,stack...);
// o plugin antigo (TGL) so exporta MTLCompilerBuildRequestWithOptions(compiler,input,rdx,flags,Module*,stack...).
// Mapeamento (confirmado no disassembly dos dois): r9 antigo = stack[0x10] novo; stack antigo [0x10..0x38] = novo [0x18..0x40].
#include <cstdint>
#include <cstddef>
#include <dlfcn.h>
#include <syslog.h>
#include <setjmp.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <new>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
extern "C" {
void* LLVMGetModuleContext(void*);
void* LLVMModuleCreateWithNameInContext(const char*, void*);
void* LLVMContextCreate(void);
void* LLVMCreateMemoryBufferWithMemoryRange(const char*, size_t, const char*, int);
int   LLVMParseBitcodeInContext2(void*, void*, void**);
}

static int g_dumps = 0;
static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void xe_dump(const char* tag, int seq, const void* p, size_t n) {
  // base64 em linhas de ~720 chars: "xe-dump <seq> <tag> <idx>/<tot> <chars>"
  if (!p || !n || n > 200000) { syslog(LOG_ERR, "xe-dump %d %s vazio/grande (%zu)", seq, tag, n); return; }
  const unsigned char* d = (const unsigned char*)p;
  size_t olen = ((n + 2) / 3) * 4; char* o = (char*)malloc(olen + 1); size_t j = 0;
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    o[j++] = B64[v >> 18 & 63]; o[j++] = B64[v >> 12 & 63];
    o[j++] = i + 1 < n ? B64[v >> 6 & 63] : '='; o[j++] = i + 2 < n ? B64[v & 63] : '=';
  }
  o[j] = 0; size_t CH = 720, tot = (olen + CH - 1) / CH;
  syslog(LOG_ERR, "xe-dump %d %s hdr bytes=%zu chunks=%zu", seq, tag, n, tot);
  for (size_t c = 0; c < tot; c++) { char buf[CH + 1]; size_t l = olen - c * CH < CH ? olen - c * CH : CH; memcpy(buf, o + c * CH, l); buf[l] = 0;
    syslog(LOG_ERR, "xe-dump %d %s %zu/%zu %s", seq, tag, c, tot, buf); }
  free(o);
}

// ---- contencao de crash do libigc: um sinal fatal durante a compilacao vira erro (1) em vez de matar o servico ----
static __thread sigjmp_buf t_jb; static __thread volatile int t_armed; static __thread void* t_bt[24]; static __thread volatile int t_btn, t_sig; static __thread volatile void* t_addr;
static void xe_fault(int sig, siginfo_t* si, void*) {
  if (!t_armed) { signal(sig, SIG_DFL); raise(sig); return; }
  t_sig = sig; t_addr = si ? si->si_addr : 0; t_btn = backtrace(t_bt, 24); t_armed = 0; siglongjmp(t_jb, 1);
}
static void xe_install_handlers() {
  static int done; if (done) return; done = 1;
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = xe_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
  static char altstack[1 << 16]; stack_t ss; ss.ss_sp = altstack; ss.ss_size = sizeof altstack; ss.ss_flags = 0; sigaltstack(&ss, 0); sa.sa_flags |= SA_ONSTACK;
  sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGILL, &sa, 0); sigaction(SIGABRT, &sa, 0); sigaction(SIGFPE, &sa, 0);
}
extern "C" long MTLCompilerBuildRequestWithOptions(void*, void*, void*, uint32_t, void*, void*, void*, void*, void*, void*, void*, void*, void*);
typedef long (*OldFn)(void*, void*, void*, uint32_t, void*, void*, void*, void*, void*, void*, void*, void*, void*);

extern "C" __attribute__((visibility("default")))
long MTLCompilerBuildRequestWithSerializedBitcode(void* comp, void* input, void* rdx, uint32_t flags,
    const char* bc, size_t bcSize,
    void* s10, void* s18, void* s20, void* s28, void* s30, void* s38, void* s40) {
  OldFn old = (OldFn)&MTLCompilerBuildRequestWithOptions;
  syslog(LOG_ERR, "xe-adapter: BuildRequestWithSerializedBitcode input=%p type=%d inputsize=%zu flags=0x%x bc=%p size=%zu old=%p",
         input, input ? *(int*)input : -1, (size_t)rdx, flags, bc, bcSize, (void*)old);
  if (!old) return 0;
  void* ctx = LLVMContextCreate();
  void* buf = LLVMCreateMemoryBufferWithMemoryRange(bc, bcSize, "xe", 0);
  void* mod = nullptr;
  int bad = LLVMParseBitcodeInContext2(ctx, buf, &mod);
  syslog(LOG_ERR, "xe-adapter: parse bitcode -> bad=%d module=%p", bad, mod);
  if (bad || !mod) return 0;
  int seq = g_dumps < 40 ? ++g_dumps : 0;
  if (seq) { xe_dump("bc", seq, bc, bcSize); xe_dump("input", seq, input, (size_t)rdx > 0 && (size_t)rdx <= 0x10000 ? (size_t)rdx : 0x400); }
  xe_install_handlers();
  long r;
  if (seq) usleep(250000);   // deixa o logd receber os dumps antes de uma possivel queda
  if (sigsetjmp(t_jb, 1) == 0) {
    t_armed = 1;
    r = old(comp, input, rdx, flags, mod, s10, s18, s20, s28, s30, s38, s40, nullptr);
    t_armed = 0;
  } else {
    syslog(LOG_ERR, "xe-adapter: FALHA CONTIDA sinal=%d addr=%p type=%d bc=%zu", (int)t_sig, (void*)t_addr, input ? *(int*)input : -1, bcSize);
    for (int i = 0; i < t_btn && i < 16; i++) { Dl_info di; const char* nm = "?"; long off = 0;
      if (dladdr(t_bt[i], &di) && di.dli_fname) { nm = strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname; off = (char*)t_bt[i] - (char*)di.dli_fbase; }
      syslog(LOG_ERR, "xe-adapter:   #%d %s+0x%lx %s", i, nm, off, di.dli_sname ? di.dli_sname : ""); }
    xe_dump("bc-falhou", 99, bc, bcSize); xe_dump("input-falhou", 99, input, (size_t)rdx > 0 && (size_t)rdx <= 0x10000 ? (size_t)rdx : 0x400);
    if (s10) *(void**)s10 = nullptr; if (s18) *(size_t*)s18 = 0;
    const char* msg = "xe: o compilador IGC falhou (sinal contido)";
    if (s40) { char* e = new char[strlen(msg) + 1]; strcpy(e, msg); *(const char**)s40 = e; }
    r = 1;
  }
  { void* rp = s10 ? *(void**)s10 : nullptr; size_t rn = s18 ? *(size_t*)s18 : 0; const char* em = s40 ? *(const char**)s40 : nullptr;
    syslog(LOG_ERR, "xe-adapter: resultado r=%ld reply=%p size=%zu err=%s", r, rp, rn, em ? em : "(nenhum)");
    if (seq) xe_dump("reply", seq, rp, rn); }
  syslog(LOG_ERR, "xe-adapter: old BuildRequestWithOptions -> %ld", r);
  return r;
}


// ---- "helper" do MTLCompiler antigo: o plugin lê compiler+0x10 (definido por MTLCompilerSetHelper, que o Tahoe NAO chama; o ctor deixa o campo
// sem inicializar -> lixo de heap). getReducedICBBuiltinModule faz: provider = helper->vt[0](helper, path); module = provider->vt[3](provider, M).
// Fornecemos um helper cujo provider devolve modulo nulo (= "nenhuma funcao ICB a ligar"), correto para shaders que nao usam ICB.
static void* xe_prov_noop(void*) { return nullptr; }
static void* xe_prov_reduce(void* self, void* module) { (void)self; return nullptr; }
static void* g_prov_vt[8] = {(void*)xe_prov_noop, (void*)xe_prov_noop, (void*)xe_prov_noop, (void*)xe_prov_reduce, (void*)xe_prov_noop, (void*)xe_prov_noop, (void*)xe_prov_noop, (void*)xe_prov_noop};
static void* g_prov_obj[2] = {(void*)g_prov_vt, nullptr};
static void* xe_helper_get(void* self, const char* path) { (void)self; (void)path; return (void*)g_prov_obj; }
static void* g_helper_vt[8] = {(void*)xe_helper_get, (void*)xe_prov_noop, (void*)xe_prov_noop, (void*)xe_prov_noop, (void*)xe_prov_noop, (void*)xe_prov_noop, (void*)xe_prov_noop, (void*)xe_prov_noop};
static void* g_helper_obj[2] = {(void*)g_helper_vt, nullptr};
extern "C" __attribute__((visibility("default"))) void* MTLCompilerCreate(void* hw) {
  typedef void* (*Fn)(void*);
  static Fn fn = nullptr;
  if (!fn) { void* h = dlopen("/System/Library/Caches/xe.bundle/Contents/MacOS/libMTLIGCCompilerPluginOld.dylib", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("/System/Library/Caches/xe.bundle/Contents/MacOS/libMTLIGCCompilerPluginOld.dylib", RTLD_NOW);
    fn = h ? (Fn)dlsym(h, "MTLCompilerCreate") : nullptr; }
  syslog(LOG_ERR, "xe-adapter: MTLCompilerCreate hw=%p family=%u", hw, hw ? *(unsigned*)hw : 0);
  static int once; if (!once && hw) { once = 1; xe_dump("hwinfo", 0, hw, 0x200); }
  void* comp = fn ? fn(hw) : nullptr;
  if (comp) {
    typedef void (*SetFn)(void*, void*);
    void* h2 = dlopen("/System/Library/Caches/xe.bundle/Contents/MacOS/libMTLIGCCompilerPluginOld.dylib", RTLD_NOW | RTLD_NOLOAD);
    SetFn sh = h2 ? (SetFn)dlsym(h2, "MTLCompilerSetHelper") : nullptr;
    if (sh) { sh(comp, (void*)g_helper_obj); syslog(LOG_ERR, "xe-adapter: helper ICB instalado em compiler=%p", comp); }
    else syslog(LOG_ERR, "xe-adapter: MTLCompilerSetHelper nao encontrado");
  }
  return comp;
}
__attribute__((constructor)) static void init() { syslog(LOG_ERR, "xe-adapter: carregado"); }
