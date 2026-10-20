// Ponte: reexporta o libLLVM do Tahoe (32023) e fornece 3 simbolos que o plugin antigo (LLVM 3902) importa.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <syslog.h>
#include <string>
#include <cstring>
#include <unistd.h>
extern "C" {
// WriteBitcodeToFile(const Module*, ...) -> (const Module&, ...): mesma ABI, ponteiro == referencia.
__attribute__((visibility("default"))) void _bridge_dummy() {}
}
static void* L() {
  static void* h = dlopen("/System/Library/PrivateFrameworks/GPUCompiler.framework/Versions/32023/Libraries/libLLVM.dylib", RTLD_NOW|RTLD_GLOBAL);
  return h;
}
template <class F> static F sym(const char* n) { void* p = dlsym(L(), n+1); if (!p) syslog(LOG_ERR, "xe-bridge: FALTA %s", n); return (F)p; }

// 1) WriteBitcodeToFile(const Module*, ...) -> (const Module&, ...), com log
extern "C" void* xe_wbf;
void* xe_wbf;
extern "C" {
void* LLVMWriteBitcodeToMemoryBuffer(void*);
const char* LLVMGetBufferStart(void*);
size_t LLVMGetBufferSize(void*);
void LLVMDisposeMemoryBuffer(void*);
}
#include <mach-o/dyld.h>
// getSerializedModule(llvm::Module*, unsigned) do MTLCompiler (build 25G241): serializa o modulo em "MTL bitcode version N" (3902 = formato que um LLVM 7 le).
// Descoberto pelo autor do NootedGreen (V274); aqui chamamos a mesma funcao a partir da ponte. Offsets validados com o prologo.
#include <mach-o/getsect.h>
// v2: o offset fixo 0x7FB06 não confere dentro do MTLCompilerService. Procura a função pela assinatura do começo dela
// (mesma no MTLCompiler do Sequoia e, pelo código-fonte, no do Tahoe):
//   push rbp; mov rbp,rsp; ...; cmp edx,0xf3d (81 fa 3d 0f 00 00) ... cmp edx,0xf3e (81 fa 3e 0f 00 00)
// Aceita só se houver exatamente um candidato em todas as imagens .../MTLCompiler carregadas.
static void* xe_find_gsm() {
  static void* fn; static int done; if (done) return fn; done = 1;
  static const unsigned char k3d[6] = {0x81, 0xfa, 0x3d, 0x0f, 0x00, 0x00}, k3e[6] = {0x81, 0xfa, 0x3e, 0x0f, 0x00, 0x00};
  int found = 0;
  for (uint32_t i = 0; i < _dyld_image_count(); i++) {
    const char* nm = _dyld_get_image_name(i); size_t l = nm ? strlen(nm) : 0;
    if (!(l >= 12 && !strcmp(nm + l - 12, "/MTLCompiler"))) continue;
    unsigned long sz = 0;
    unsigned char* t = getsectiondata((const struct mach_header_64*)_dyld_get_image_header(i), "__TEXT", "__text", &sz);
    if (!t) { syslog(LOG_ERR, "xe-bridge: %s sem __text", nm); continue; }
    for (unsigned long o = 32; o + 64 < sz; o++) {
      if (memcmp(t + o, k3d, 6)) continue;
      unsigned char* start = NULL;
      for (int back = 4; back <= 32; back++)
        if (t[o - back] == 0x55 && t[o - back + 1] == 0x48 && t[o - back + 2] == 0x89 && t[o - back + 3] == 0xe5) { start = t + o - back; break; }
      if (!start) continue;
      int has3e = 0;
      for (int f = 6; f < 64; f++) if (!memcmp(t + o + f, k3e, 6)) { has3e = 1; break; }
      if (!has3e) continue;
      found++; fn = start;
      syslog(LOG_ERR, "xe-bridge: candidato getSerializedModule em %s+0x%lx", nm, (unsigned long)(start - (unsigned char*)_dyld_get_image_header(i)));
    }
  }
  if (found != 1) { syslog(LOG_ERR, "xe-bridge: getSerializedModule: %d candidatos, usando API C", found); fn = NULL; }
  else syslog(LOG_ERR, "xe-bridge: getSerializedModule em %p", fn);
  return fn;
}
extern "C" void xe_wbf_impl(void* m, void* os, bool a, const void* idx, bool b, void* hash) {
  // layout antigo do raw_string_ostream (plugin LLVM 3902): [0x28] = std::string* OS. Gravamos o bitcode direto na std::string.
  std::string* dst = *(std::string**)((char*)os + 0x28);
  void* gsm = xe_find_gsm();
  if (gsm && !getenv("XE_NO_GSM")) {
    typedef void (*GSM)(void**, void*, unsigned);
    void* obj = nullptr; ((GSM)gsm)(&obj, m, 3902);
    if (obj) {
      char* bg = *(char**)((char*)obj + 8); char* en = *(char**)((char*)obj + 0x10);
      syslog(LOG_ERR, "xe-bridge: WriteBitcodeToFile module=%p -> %zu bytes (formato 3902 via MTLCompiler)", m, (size_t)(en - bg));
      if (dst) dst->append(bg, en - bg);
      void** vt = *(void***)obj; ((void (*)(void*))vt[1])(obj);
      return;
    }
  }
  void* buf = LLVMWriteBitcodeToMemoryBuffer(m);
  size_t n = buf ? LLVMGetBufferSize(buf) : 0;
  syslog(LOG_ERR, "xe-bridge: WriteBitcodeToFile module=%p -> %zu bytes (dst=%p, API C)", m, n, (void*)dst);
  if (buf && dst) dst->append(LLVMGetBufferStart(buf), n);
  if (buf) LLVMDisposeMemoryBuffer(buf);
}
__asm__(".globl __ZN4llvm18WriteBitcodeToFileEPKNS_6ModuleERNS_11raw_ostreamEbPKNS_18ModuleSummaryIndexEbPNSt3__15arrayIjLm5EEE\n"
        "__ZN4llvm18WriteBitcodeToFileEPKNS_6ModuleERNS_11raw_ostreamEbPKNS_18ModuleSummaryIndexEbPNSt3__15arrayIjLm5EEE:\n"
        "  jmp _xe_wbf_impl\n");

// 2) raw_string_ostream::~raw_string_ostream() (D1): flush() inline + ~raw_ostream()
extern "C" void xe_rso_dtor(void* self) { /* objeto no layout antigo: nada a fazer (sem buffer proprio) */ }
__asm__(".globl __ZN4llvm18raw_string_ostreamD1Ev\n__ZN4llvm18raw_string_ostreamD1Ev:\n  jmp _xe_rso_dtor\n");

// 3) parseIRFile: antiga (StringRef, SMDiagnostic&, LLVMContext&, bool) -> nova (..., function_ref<Optional<string>(StringRef)>)
struct StringRef { const char* p; size_t n; };
struct OptStr { unsigned char s[24]; bool has; };
extern "C" OptStr xe_dl_cb(intptr_t, StringRef) { OptStr o; memset(&o, 0, sizeof o); return o; }
struct FuncRef { OptStr (*cb)(intptr_t, StringRef); intptr_t callable; };
extern "C" void* xe_parseIRFile(StringRef f, void* diag, void* ctx, bool) {
  typedef void* (*Fn)(StringRef, void*, void*, FuncRef);
  static Fn fn = sym<Fn>("__ZN4llvm11parseIRFileENS_9StringRefERNS_12SMDiagnosticERNS_11LLVMContextENS_12function_refIFNS_8OptionalINSt3__112basic_stringIcNS7_11char_traitsIcEENS7_9allocatorIcEEEEEES0_EEE");
  void* r = fn ? fn(f, diag, ctx, FuncRef{xe_dl_cb, 0}) : nullptr;
  syslog(LOG_ERR, "xe-bridge: parseIRFile(%.*s) -> %p", (int)f.n, f.p, r);
  return r;
}
__asm__(".globl __ZN4llvm11parseIRFileENS_9StringRefERNS_12SMDiagnosticERNS_11LLVMContextEb\n__ZN4llvm11parseIRFileENS_9StringRefERNS_12SMDiagnosticERNS_11LLVMContextEb:\n  jmp _xe_parseIRFile\n");

__attribute__((constructor)) static void init() {
  syslog(LOG_ERR, "xe-bridge: carregada pid=%d", getpid());
  xe_wbf = sym<void*>("__ZN4llvm18WriteBitcodeToFileERKNS_6ModuleERNS_11raw_ostreamEbPKNS_18ModuleSummaryIndexEbPNSt3__15arrayIjLm5EEE");
}

// 4) Twine: o plugin antigo monta Twine inline no layout velho (LHS@0, RHS@8, kinds@0x10/0x11); o libLLVM novo espera LHS@0(16B),
//    RHS@0x10, kinds@0x20/0x21. Achatamos o Twine antigo numa std::string e passamos um Twine novo (StdStringKind=4).
static void xe_flat(const unsigned char* t, std::string& out, int depth = 0) {
  if (!t || depth > 16) return;
  auto one = [&](const unsigned char* p, unsigned char k) {
    switch (k) {
      case 0: case 1: break;
      case 2: xe_flat(*(const unsigned char**)p, out, depth + 1); break;           // TwineKind
      case 3: { const char* c = *(const char**)p; if (c) out += c; break; }        // CStringKind
      case 4: { const std::string* s = *(const std::string**)p; if (s) out += *s; break; } // StdStringKind
      case 5: { /* PtrAndLength nao existe no LLVM 7 antigo */ break; }
      case 6: { struct SR { const char* p; size_t n; }; const SR* r = *(const SR**)p; if (r && r->p) out.append(r->p, r->n); break; } // StringRefKind
      default: syslog(LOG_ERR, "xe-bridge: Twine kind %d nao tratado", k); break;
    }
  };
  one(t, t[0x10]);
  one(t + 8, t[0x11]);
}
static void xe_newtwine(unsigned char* nt, const std::string& s) {
  memset(nt, 0, 0x40);
  *(const std::string**)nt = &s;
  nt[0x20] = 4; nt[0x21] = 1;
}
extern "C" void* xe_getNamedMetadata(void* mod, const unsigned char* oldTw) {
  typedef void* (*Fn)(void*, void*);
  static Fn fn = sym<Fn>("__ZNK4llvm6Module16getNamedMetadataERKNS_5TwineE");
  std::string s; xe_flat(oldTw, s);
  alignas(16) unsigned char nt[0x40]; xe_newtwine(nt, s);
  void* r = fn ? fn(mod, nt) : nullptr;
  syslog(LOG_ERR, "xe-bridge: getNamedMetadata('%s') -> %p", s.c_str(), r);
  return r;
}
__asm__(".globl __ZNK4llvm6Module16getNamedMetadataERKNS_5TwineE\n__ZNK4llvm6Module16getNamedMetadataERKNS_5TwineE:\n  jmp _xe_getNamedMetadata\n");
extern "C" void xe_setName(void* val, const unsigned char* oldTw) {
  typedef void (*Fn)(void*, void*);
  static Fn fn = sym<Fn>("__ZN4llvm5Value7setNameERKNS_5TwineE");
  std::string s; xe_flat(oldTw, s);
  alignas(16) unsigned char nt[0x40]; xe_newtwine(nt, s);
  if (fn) fn(val, nt);
}
__asm__(".globl __ZN4llvm5Value7setNameERKNS_5TwineE\n__ZN4llvm5Value7setNameERKNS_5TwineE:\n  jmp _xe_setName\n");
