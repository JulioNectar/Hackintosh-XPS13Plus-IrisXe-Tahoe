//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_gen11.hpp"
static bool xeFixEnabled();  // xe/xefix, definido junto do FastWriteRegister32
#include "AppleIntelParams.hpp"
#include <Headers/kern_api.hpp>
#include "kern_genx.hpp"
#include "kern_green.hpp"
#include "IntelDPLinkTraining.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <kern/thread_call.h>
// xe/blitscratch2: definidos em kern_green.cpp (wrapLockCpu)
extern void *volatile gXeBlitMem, *volatile gXeBlitCpu;
static volatile uint32_t gXeGtFreqApple = 0, gXeGtFreqN = 0;  // XE-GTFREQ: último pedido da Apple, nº de pedidos
static volatile uint32_t gXeDpmsBlocked = 0;  // XE-NODPMS: escritas de desligamento do caminho de vídeo bloqueadas
static volatile uint32_t gXeFbDpmsN = 0;  // XE-FBDPMS: transições de repouso interceptadas
static volatile uint32_t gXeResFreeFix = 0;  // XE-RESFREE: entradas inválidas neutralizadas
extern volatile uint32_t gXeOrphanMiss;  // XE-NOORPHAN (kern_green.cpp)
extern bool gXeLockRouted;

// ==== 6 kextInfos: ICL fallback + dual TGL identities (com.xxxxx and com.apple) from /Library/Extensions ====
//trivial
// ICL FB — com.apple (fallback path)
static const char *pathsICLFB[] = {
    "/System/Library/Extensions/AppleIntelICLLPGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelICLLPGraphicsFramebuffer",
};
static KernelPatcher::KextInfo kextG11FB {"com.apple.driver.AppleIntelICLLPGraphicsFramebuffer", pathsICLFB, 1, {}, {},
    KernelPatcher::KextInfo::Unloaded};

// ICL HW — com.apple (fallback path)
static const char *pathsICLHW[] = {
    "/System/Library/Extensions/AppleIntelICLGraphics.kext/Contents/MacOS/AppleIntelICLGraphics",
};
static KernelPatcher::KextInfo kextG11HW {"com.apple.driver.AppleIntelICLGraphics", pathsICLHW, 1, {}, {},
    KernelPatcher::KextInfo::Unloaded};

// TGL FB — com.xxxxx (loaded from /Library/Extensions/)
static const char *pathsTGLFB[] = {
    "/Library/Extensions/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer",
};
static KernelPatcher::KextInfo kextG11FBT {"com.xxxxx.driver.AppleIntelTGLGraphicsFramebuffer", pathsTGLFB, 1,
    {false, false, false, true}, {},
    KernelPatcher::KextInfo::Unloaded};

// TGL FB — com.apple (loaded from /Library/Extensions/)
static KernelPatcher::KextInfo kextG11FBTA {"com.apple.driver.AppleIntelTGLGraphicsFramebuffer", pathsTGLFB, 1,
	{false, false, false, true}, {},
	KernelPatcher::KextInfo::Unloaded};

// TGL HW — com.xxxxx (loaded from /Library/Extensions/)
static const char *pathsTGLHW[] = {
    "/Library/Extensions/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics",
};
static KernelPatcher::KextInfo kextG11HWT {"com.xxxxx.driver.AppleIntelTGLGraphics", pathsTGLHW, 1,
    {false, false, false, true}, {},
    KernelPatcher::KextInfo::Unloaded};

// TGL HW — com.apple (loaded from /Library/Extensions/)
static KernelPatcher::KextInfo kextG11HWTA {"com.apple.driver.AppleIntelTGLGraphics", pathsTGLHW, 1,
	{false, false, false, true}, {},
	KernelPatcher::KextInfo::Unloaded};

// IOAcceleratorFamily2 symbols are resolved inside NGreen::processKext via
// kextIOAcceleratorFamily2 (kern_green.cpp) which already has the valid path.
// No separate kextIOAF2 registration needed here.

Gen11 *Gen11::callback = nullptr;

// xe/bootmode: contadores do hook getCurrentDisplayMode, impressos junto do HANGCHECK (os SYSLOGs do load do kext
// são sobrescritos no msgbuf pelo log do IGFB antes de o logd drenar).
static volatile uint32_t gFbModeCalls = 0, gFbModeBootProg = 0, gFbModeSubst = 0, gFbModeLastRet = 0, gFbModeLastMode = 0;
static volatile uint32_t gFbModeArgSeen = 0;
static volatile uint32_t gTglFbBlockRuns = 0, gTglFbRoutesDone = 0, gIcLFbBlockRuns = 0;
static volatile uint64_t gOrgGcdm = 0, gOrgGpi = 0, gOrgVdm = 0;

void Gen11::init() {
	callback = this;

	if (checkKernelArgument("-ngreentglfb") || checkKernelArgument("-ngreentglwithgfx")) {
		SYSLOG("ngreen", "Gen11::init: FB tier → TGL (ICL FB skipped)");
		lilu.onKextLoadForce(&kextG11FBT);
		lilu.onKextLoadForce(&kextG11FBTA);
		if (checkKernelArgument("-ngreentglwithgfx")) {
			SYSLOG("ngreen", "Gen11::init: HW tier → TGL (ICL HW skipped)");
			lilu.onKextLoadForce(&kextG11HWT);
			lilu.onKextLoadForce(&kextG11HWTA);
		}
	} else if (checkKernelArgument("-ngreentglgfx")) {
		SYSLOG("ngreen", "Gen11::init: HW tier → TGL (ICL HW skipped)");
		lilu.onKextLoadForce(&kextG11HWT);
		lilu.onKextLoadForce(&kextG11HWTA);
	} else if (checkKernelArgument("-ngreenicl")) {
		SYSLOG("ngreen", "Gen11::init: FB tier → ICL fallback");
		lilu.onKextLoadForce(&kextG11FB);
		SYSLOG("ngreen", "Gen11::init: HW tier → ICL fallback");
		lilu.onKextLoadForce(&kextG11HW);
	}
}

static bool isWEGCoexistMode() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngwegcoex", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngwegcoex");
}

static bool isExperimentalMonitorEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenexp", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenexp");
}

static bool isV60MonitorEnabled() {
	int forceOff = 0;
	if (PE_parse_boot_argn("ngreenv60off", &forceOff, sizeof(forceOff))) {
		if (forceOff != 0)
			return false;
	}

	if (checkKernelArgument("-ngreenv60off")) {
		return false;
	}

	int forceOn = 0;
	if (PE_parse_boot_argn("ngreenv60", &forceOn, sizeof(forceOn))) {
		if (forceOn != 0)
			return true;
	}

	if (checkKernelArgument("-ngreenv60")) {
		return true;
	}

	return isExperimentalMonitorEnabled();
}

static bool isDisplayPipeForceDisabled() {
	// NOTE: isLegacyOwnershipModeEnabled() (V80 plane-linearization) is intentionally
	// NOT checked here. V80 writes are self-limiting to the first 3 ticks (≤150ms) and
	// must not prevent WindowServer from opening the display pipe after that window.
	// Coupling these two features caused DisplayPipeSupported=0 → black screen forever.

	// Stage-3 baseline now has DYLD-side NULL guards for DisplayPipe path.
	// Keep native DisplayPipe ON by default and use ngreendp0 only as an explicit
	// fallback switch when troubleshooting.
	int nativeDisplayPipe = 0;
	if (PE_parse_boot_argn("ngreendp1", &nativeDisplayPipe, sizeof(nativeDisplayPipe))) {
		SYSLOG("ngreen", "V78A: parsed ngreendp1=%d", nativeDisplayPipe);
		if (nativeDisplayPipe != 0)
			return false;
	}

	if (checkKernelArgument("-ngreendp1")) {
		SYSLOG("ngreen", "V78A: detected -ngreendp1");
		return false;
	}

	int enabled = 0;
	if (PE_parse_boot_argn("ngreendp0", &enabled, sizeof(enabled))) {
		SYSLOG("ngreen", "V78A: parsed ngreendp0=%d", enabled);
		return enabled != 0;
	}

	if (checkKernelArgument("-ngreendp0")) {
		SYSLOG("ngreen", "V78A: detected -ngreendp0");
		return true;
	}

	static bool v78aLogged = false;
	if (!v78aLogged) {
		v78aLogged = true;
		SYSLOG("ngreen", "V78A (capped): default native DisplayPipeSupported ON");
	}
	return false;
}

static bool isV93PlaneGuardEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenv93", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenv93");
}

static bool shouldForceFullMetalPath() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenfullmtlcore", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}
	if (checkKernelArgument("-ngreenfullmtlcore")) {
		return true;
	}
	// Legacy: unified arg still accepted as fallback.
	if (PE_parse_boot_argn("ngreenfullmtl", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}
	return checkKernelArgument("-ngreenfullmtl");
}

static int getV142SubmitBlitMode() {
	int parsed = 0;
	if (PE_parse_boot_argn("ngreenV142", &parsed, sizeof(parsed))) {
		return parsed;
	}

	if (checkKernelArgument("-ngreenV142orig"))
		return 3;
	if (checkKernelArgument("-ngreenV142pass"))
		return 2;
	if (checkKernelArgument("-ngreenV142ok"))
		return 1;
	if (checkKernelArgument("-ngreenV142hardunsupported"))
		return 0;
	if (checkKernelArgument("-ngreenV142unsupported"))
		return 1;

	// V170: RPL (and any non-TGL spoof) cannot execute the TGL-format 3D blit commands that
	// the original IGAccelBlit::submitBlit generates — the RCS EU stalls indefinitely
	// (INSTDONE=0xfffffffe, bit-0 stuck) causing an infinite hangcheck→reset→retry loop
	// that kills WindowServer within 120 s. Default to mode 1 (return 0 = success, no-op)
	// so the compositor initialises without hanging. Real TGL hardware still uses mode 3.
	// Override with -ngreenV142orig or ngreenV142=3 to restore original blit submission.
	return 1;
}

static bool isV142Diag2DBypassEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenV142diag2d", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenV142diag2d");
}

static bool isV88ScanoutFillEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenv88", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenv88");
}

static bool isV60AggressiveMonitorEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenv60rw", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenv60rw");
}

static bool isV80LEnforcerEnabled() {
	return checkKernelArgument("-ngreenv80l");
}

static uint32_t getV65Tier1WantBits(bool isRealTGL) {
	// Real TGL: always use native behavior (RCS + BCS).
	if (isRealTGL) {
		return (1u << GEN11_RCS0) | (1u << GEN11_BCS);
	}

	// RPL/ADL spoof path: BCS must be enabled when the original submitBlit path
	// (V142 mode 3) is active — that path submits directly to BCS ring. Leaving
	// BCS out of the want bits means RING_CTRL never gets the enable bit set,
	// commands queue up and the ring stalls (gpuRestart Signature 801).
	// For other V142 modes (0/1/2 = bypass/return0) BCS is idle, keep RCS-only.
	if (getV142SubmitBlitMode() == 3 || checkKernelArgument("-ngreenbcsirq")) {
		return (1u << GEN11_RCS0) | (1u << GEN11_BCS);
	}

	return (1u << GEN11_RCS0);
}

static int getV77KillDelayIterations() {
	int delay = 0;
	if (PE_parse_boot_argn("ngreenV77DelayKill", &delay, sizeof(delay))) {
		if (delay < 0)
			return 0;
		if (delay > 60)
			return 60;
		return delay;
	}

	// Final/default behavior: do not kill DisplayPipe clients unless explicitly requested.
	return 60;
}

// xe/bootmode (-ngreenfixbootmode): o FB TGL nasce com fCurrentMode = kIOFBBootProgrammable (0xfffffffb, sem detailed
// timings), e o IOAccelDisplayPipe::init_framebuffer_resource pede o pixel info desse "modo" antes de o WindowServer
// definir qualquer coisa. getPixelInformation falha (0xe00002c2), o CoreDisplay marca a tela principal como offline e o
// WindowServer aborta em loop. As rotas do Lilu para getCurrentDisplayMode/getPixelInformation foram instaladas mas não
// são chamadas, então corrige-se na origem: o imediato de "movl $0xfffffffb, 0x4250(%rbx)" (fCurrentMode, offset
// 0x53de3 em 16.0.32) passa a ser 0x80001000, o modo nativo 3840x2400 que a EFI deixa programado (-ngreenkeepefi).
static volatile uint32_t gFbModePatchTried = 0, gFbModePatchOk = 0;
static void applyFixBootMode(KernelPatcher &patcher, KernelPatcher::KextInfo *kext, mach_vm_address_t address, size_t size) {
	if (!checkKernelArgument("-ngreenfixbootpatch")) return;
	static const uint8_t fbm[] = {0xc7, 0x83, 0x50, 0x42, 0x00, 0x00, 0xfb, 0xff, 0xff, 0xff};
	static const uint8_t rbm[] = {0xc7, 0x83, 0x50, 0x42, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80};
	LookupPatchPlus const bm[] = {{kext, fbm, rbm, arrsize(fbm), 1}};
	gFbModePatchTried = 1;
	gFbModePatchOk = LookupPatchPlus::applyAll(patcher, bm, address, size) ? 1 : 0;
	SYSLOG("ngreen", "fixbootmode: fCurrentMode initial 0xfffffffb -> 0x80001000 patch %s", gFbModePatchOk ? "applied" : "NOT found");
}

// xe/bootmode (-ngreenfixbootmode): o WindowServer abre o display pipe (IOAccelDisplayPipe::init_framebuffer_resource)
// ~2 s antes de o FB receber a tabela de timings (fDetailedTimings, this+0x4260, setDetailedTimings). Sem a tabela
// validateDisplayMode devolve 0xe00002c2 e o DisplayPipe não é criado. Aqui montamos a tabela cedo, com o timing que a
// própria EFI deixou programado no CRTC (hwCRTCToIODetailedTimingInformation), e fazemos o que setDetailedTimings faz:
// setProperty("IOFBDetailedTimings") + guardar o ponteiro. O setDetailedTimings real, quando vier, substitui a nossa.
// xe/flipdiag (19/10): o WindowServer para de fazer flips e cai por "Display not ready": o trabalho do stamp 1 fica
// retido esperando algo (provavelmente a conclusão da transação de display). Contadores no caminho de conclusão
// (TGL: submit/isComplete/signalComplete/signalDisplayStampUpdate), com IOLog nas primeiras chamadas e a cada 64.
static void xeEv(uint32_t tag, uint32_t a, uint32_t b, uint32_t c);
static uint8_t *volatile gAccelObj = nullptr;  // xe/csstate: IntelAccelerator (capturado em signalDisplayStampUpdate)
static mach_vm_address_t gOrgDpSubmit = 0, gOrgDpIsComplete = 0, gOrgDpSignal = 0, gOrgDispStamp = 0;
static volatile uint32_t gDpSubmitN = 0, gDpIsCompN = 0, gDpIsCompTrue = 0, gDpSignalN = 0, gDispStampN = 0;
static uint64_t xeDpSubmit(void *that, void *t) {
	uint32_t n = ++gDpSubmitN;
	xeEv('FSUB', n, gDpIsCompTrue, gDpSignalN);
	return FunctionCast(xeDpSubmit, gOrgDpSubmit)(that, t);
}
static bool xeDpIsComplete(void *that, void *t) {
	bool r = FunctionCast(xeDpIsComplete, gOrgDpIsComplete)(that, t);
	uint32_t n = ++gDpIsCompN;
	if (r) gDpIsCompTrue++;
	if (n <= 400) xeEv('FISC', n, r, gDpIsCompTrue);
	return r;
}
static void xeDpSignal(void *that, void *ev) {
	uint32_t n = ++gDpSignalN;
	xeEv('FSIG', n, 0, 0);
	FunctionCast(xeDpSignal, gOrgDpSignal)(that, ev);
}
static void xeDispStamp(void *that, int idx, uint32_t stamp) {
	gAccelObj = static_cast<uint8_t *>(that);
	uint32_t n = ++gDispStampN;
	xeEv('FDST', n, (uint32_t)idx, stamp);
	FunctionCast(xeDispStamp, gOrgDispStamp)(that, idx, stamp);
}

// xe/scheddiag (19/10): o trabalho do stamp 56 (índice 1) nunca chega à GPU. Contadores no escalonador 5:
// push (entrada na fila), notify, submitExecList (escrita no hardware), pause/resume.
static mach_vm_address_t gOrgS5Push = 0, gOrgS5Notify = 0, gOrgCs5Submit = 0, gOrgS5Pause = 0, gOrgS5Resume = 0;
static volatile uint32_t gS5PushN = 0, gS5NotifyN = 0, gCs5SubmitN = 0, gS5PauseN = 0, gS5ResumeN = 0;
static uint32_t gThrtN = 0, gThrtSimN = 0;
static volatile uint32_t gCsbResyncN = 0;  // 21/10: active->idle salvos pelo xeCsbResync (CSBR)
static volatile uint32_t gCsbSkipN = 0;  // 19/10: eventos active->idle descartados pelo xeCsbA2I
// 20/10: brilho (PWM sul 0xC8254 período / 0xC8258 duty), estilo BacklightRegistersFix do WhateverGreen
static volatile uint32_t gBlHwFreq = 0, gBlAppleFreq = 0, gBlLastDuty = 0, gBlMaxDuty = 0, gBlLastOut = 0, gBlFreqWrites = 0;
static volatile uint32_t gIdlwN = 0, gIdlwMax = 0, gIdlwTimeout = 0;  // 19/10: esperas do IDLW (medir travadas)
static volatile uint64_t gIdlwUs = 0;
static volatile uint32_t gPageoffN = 0, gPageonN = 0;  // 19/10: chamadas de pageoff/pageon ignoradas (XE-NOPAGE)
static volatile uint32_t gPageonFix = 0;
static volatile uint32_t gSameTailForced = 0;  // 20/10: reenvios com tail igual forçados a restore completo  // 19/10: níveis com lista de retângulos não mapeada (PAGEONGUARD)  // 19/10: chamadas a verifyThrottling / quantas pediram throttle
static void xeRetryWorker(thread_call_param_t, thread_call_param_t);
static thread_call_t gRetryTc = nullptr;
static uint64_t xeS5Push(void *that, void *ctx, uint32_t a, uint32_t b, bool c, bool d) {
	if (!gRetryTc) gRetryTc = thread_call_allocate(xeRetryWorker, nullptr);
	uint32_t n = ++gS5PushN;
	uint64_t r = FunctionCast(xeS5Push, gOrgS5Push)(that, ctx, a, b, c, d);
	xeEv('PUSH', n, a, b);
	return r;
}
static uint64_t xeS5Notify(void *that, int cs) {
	++gS5NotifyN;
	return FunctionCast(xeS5Notify, gOrgS5Notify)(that, cs);
}
// xe/csstate (19/10): estado interno dos command streamers 5 e do DPSM, a cada 2 s (thread_call), para ver por que
// o escalonador deixa de submeter: porta da execlist ocupada (0x30/0x38 por porta, 16 B), flags 0x1c00/0x1c38/0x1c3c,
// e o bit "ocioso" do DPSM (IntelAccelerator+0x1458 bit 0; o kick só limpa se 0x1191 bit 2).
static uint8_t *volatile gCs5Obj[4];
static thread_call_t gCsStateTc = nullptr;
struct XeCsbEnt;
static void xeCsbFlush();
static void xeEvFlush();
static void xeEvDumpRecent(uint32_t count, int seq);
static void xeCsStateWorker(thread_call_param_t, thread_call_param_t) {
	static uint32_t tick = 0;
	tick++;
	// 19/10: a impressão contínua (milhares de linhas/s) apagava o dmesg (log do itlwm sumia) e gastava CPU.
	// Com a GPU estável, só imprime com -ngreenevlog (os eventos continuam sendo gravados no anel).
	static int evLog = -1;
	if (evLog < 0) evLog = checkKernelArgument("-ngreenevlog") ? 1 : 0;
	if (evLog == 1) {
		xeCsbFlush();  // CSB primeiro
		xeEvFlush();
	}
	// 20/10: quando o guard do CSB descarta um evento, imprime os ~120 eventos anteriores (até 6 vezes por boot),
	// para achar por que o hardware e o escalonador discordam da execlist (raiz do "EL[0] was invalidated").
	{
		static uint32_t lastSkip = 0;
		static int dumps = 0;
		uint32_t sk = gCsbSkipN;
		if (sk != lastSkip && dumps < 6) { lastSkip = sk; dumps++; xeEvDumpRecent(120, dumps); }
	}
	if (tick % 8 == 0) for (int i = 0; i < 4; i++) {
		uint8_t *c = gCs5Obj[i];
		if (!c) continue;
		uint32_t cur = *reinterpret_cast<volatile uint32_t *>(c + 0x50);
		IOLog("XeNG CS t=%u cs%d cur=%u p0=%d/%d p1=%d/%d f1c00=%u f1c38=%u f1c3c=%u\n", tick, i, cur,
			  *reinterpret_cast<void *volatile *>(c + 0x30) != nullptr, *reinterpret_cast<void *volatile *>(c + 0x38) != nullptr,
			  *reinterpret_cast<void *volatile *>(c + 0x40) != nullptr, *reinterpret_cast<void *volatile *>(c + 0x48) != nullptr,
			  c[0x1c00], *reinterpret_cast<volatile uint32_t *>(c + 0x1c38), c[0x1c3c]);
	}
	if (gAccelObj && tick % 8 == 0)
		IOLog("XeNG CS t=%u dpsm1458=0x%x cfg1190=0x%x (push %u exec %u) thrt=%u thrtSim=%u csbx=%u csbr=%u idlw=%u/%llums max=%uus to=%u pgoff=%u pgon=%u pgfix=%u stail=%u resfix=%u orf=%u dpms=%u fbdpms=%u gtf=%u/0x%x bl hw=%u apple=%u fw=%u duty=%u max=%u out=%u rc1474=%u\n", tick, *reinterpret_cast<volatile uint32_t *>(gAccelObj + 0x1458),
			  *reinterpret_cast<volatile uint32_t *>(gAccelObj + 0x1190), gS5PushN, gCs5SubmitN, gThrtN, gThrtSimN, gCsbSkipN, gCsbResyncN, gIdlwN, (unsigned long long)(gIdlwUs / 1000), gIdlwMax, gIdlwTimeout, gPageoffN, gPageonN, gPageonFix, gSameTailForced, gXeResFreeFix, gXeOrphanMiss, gXeDpmsBlocked, gXeFbDpmsN, gXeGtFreqN, gXeGtFreqApple, gBlHwFreq, gBlAppleFreq, gBlFreqWrites, gBlLastDuty, gBlMaxDuty, gBlLastOut,
			  (unsigned)*reinterpret_cast<volatile uint8_t *>(gAccelObj + 0x1474));
	if (tick < 2400 && gCsStateTc) {  // a cada 250 ms por ~10 min
		uint64_t deadline;
		clock_interval_to_deadline(250, kMillisecondScale, &deadline);
		thread_call_enter_delayed(gCsStateTc, deadline);
	}
}
static void xeEv(uint32_t tag, uint32_t a, uint32_t b, uint32_t c);
// xe/elsqretry (19/10): o RPL recusa a escrita na ELSQ do RCS quando o status está com troca de elemento pendente
// (0x14198/0x18098: as duas execlists válidas, arbitragem ligada) -> fica o bit 30 e a submissão some.
// Detecta (bit 30 depois da escrita), guarda os descritores e, numa thread, espera o motor ficar ocioso e repete o
// EL_CTRL_LOAD (EXECLIST_CONTROL 0x2550 = 1) se a ELSQ ainda tiver os mesmos descritores. -ngreennoelsqretry desliga.
static volatile uint32_t gRetryPend = 0, gRetrySeq = 0;
static uint32_t gRetryQ[4];
static void *gRetryAcc = nullptr;
static void xeRetryWorker(thread_call_param_t, thread_call_param_t) {
	auto *cb = NGreen::callback;
	if (!cb || !gRetryPend) return;
	if (gRetryAcc) Gen11::xeForceWakePub(gRetryAcc, true, 0b101, 3);
	uint32_t st = 0, us = 0, resultado = 0;
	for (; us < 200000; us += 50) {
		st = cb->readReg32(0x2234);
		if (!(st & 0x40000000)) { resultado = 1; break; }  // o hardware aceitou sozinho
		if (cb->readReg32(0x2510) != gRetryQ[0] || cb->readReg32(0x2514) != gRetryQ[1]) { resultado = 2; break; }  // driver escreveu outra
		if ((st & 0xff) == 0x01 || !(st & 0x18)) {  // ocioso: repete a carga
			cb->writeReg32(0x2550, 1);
			IODelay(20);
			resultado = 3; st = cb->readReg32(0x2234);
			break;
		}
		IODelay(50);
	}
	if (gRetryAcc) Gen11::xeForceWakePub(gRetryAcc, false, 0b101, 3);
	xeEv('RTRY', resultado, st, us);
	gRetryPend = 0;
}
// xe/sametail (20/10, do WhateverXe efa9241): o Linux evita "lite restore" com TAIL igual (o hardware se comporta mal).
// Se o RCS reenvia o item ativo com contexto/descritor/imagem/tail idênticos aos da submissão anterior, liga o flag
// one-shot de restauração forçada do contexto da Apple (ctx+0xc9; o código em 0x3a7b6 o converte em FORCE_RESTORE no
// descritor). Layout conferido com o mesmo driver 16.0.32. -ngreennosametail desliga.
struct XeSameTailEnt { uint64_t item, ctx, desc, image; uint32_t tail; };
static XeSameTailEnt gSameTailPrev = {};
static inline bool xeKptr(uint64_t p) { return (p >> 47) == 0x1ffff; }
static void xeSameTail(void *that) {
	static int on = -1;
	if (on < 0) on = checkKernelArgument("-ngreennosametail") ? 0 : 1;
	if (on != 1 || !that) return;
	auto *st = static_cast<uint8_t *>(that);
	auto *desc = *reinterpret_cast<uint8_t **>(st + 0x28);
	if (!xeKptr((uint64_t)desc) || desc[0] != 0) return;  // só o RCS
	uint32_t bank = *reinterpret_cast<uint32_t *>(st + 0x50);
	if (bank >= 2) { gSameTailPrev = {}; return; }
	uint64_t active = *reinterpret_cast<uint64_t *>(st + 0x30 + bank * 16);
	uint64_t next = *reinterpret_cast<uint64_t *>(st + 0x30 + (1 - bank) * 16);
	XeSameTailEnt e = {};
	e.item = next;
	if (!xeKptr(next)) { gSameTailPrev = {}; return; }
	e.ctx = *reinterpret_cast<uint64_t *>(next);
	if (!xeKptr(e.ctx) || *reinterpret_cast<uint8_t *>(e.ctx + 0x6c) != 0) { gSameTailPrev = {}; return; }
	memcpy(&e.desc, reinterpret_cast<void *>(e.ctx + 0x89), 8);
	uint64_t buf = *reinterpret_cast<uint64_t *>(e.ctx + 0x98);
	if (!(e.desc & 1) || !xeKptr(buf) || *reinterpret_cast<uint64_t *>(buf + 0x20) != 81920) { gSameTailPrev = {}; return; }
	e.image = *reinterpret_cast<uint64_t *>(buf + 0x38);
	if (!xeKptr(e.image)) { gSameTailPrev = {}; return; }
	e.tail = *reinterpret_cast<uint32_t *>(e.image + 0x101c);
	if (active == next && !memcmp(&e, &gSameTailPrev, sizeof(e))) {
		auto *forced = reinterpret_cast<volatile uint8_t *>(e.ctx + 0xc9);
		if (!*forced) { *forced = 1; gSameTailForced++; }
	}
	gSameTailPrev = e;
}
static uint64_t xeCs5Submit(void *that, uint32_t x) {
	for (int i = 0; i < 4; i++) {
		if (gCs5Obj[i] == that) break;
		if (!gCs5Obj[i]) { gCs5Obj[i] = static_cast<uint8_t *>(that); break; }
	}
	if (!gCsStateTc) {
		gCsStateTc = thread_call_allocate(xeCsStateWorker, nullptr);
		if (gCsStateTc) thread_call_enter(gCsStateTc);
	}
	uint32_t n = ++gCs5SubmitN;
	xeEv('EXEC', n, x, gS5PushN);
	xeSameTail(that);
	uint64_t r = FunctionCast(xeCs5Submit, gOrgCs5Submit)(that, x);
	// xe/elsq (19/10): o que foi escrito na fila de submissão. Base do motor pelo tipo do CS (cs+0x28 -> byte 0: 0=RCS)
	auto *c28 = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(that) + 0x28);
	uint32_t eb = (c28 && c28[0] != 0) ? 0x22000 : 0x2000;
	if (NGreen::callback) {
		auto rr = [&](uint32_t o) { return NGreen::callback->readReg32(eb + o); };
		uint32_t tag = eb == 0x2000 ? 'ELR0' : 'ELB0';
		xeEv(tag, rr(0x510), rr(0x514), rr(0x518));
		xeEv(tag + 1, rr(0x51c), rr(0x550), rr(0x234));
		xeEv(tag + 2, rr(0x238), rr(0x3a0), rr(0x034) | (rr(0x030) << 16));
		static int retryArg = -1;
		// 19/10: desligado por padrão (suspeito do pânico "EL[0] was invalidated!!": recarregar 0x2550 com a lista já
		// aceita invalida o elemento 0); -ngreenelsqretry religa
		if (retryArg < 0) retryArg = checkKernelArgument("-ngreenelsqretry") ? 1 : 0;
		if (retryArg == 1 && eb == 0x2000 && (rr(0x234) & 0x40000000) && gRetryTc && !gRetryPend) {
			gRetryQ[0] = rr(0x510); gRetryQ[1] = rr(0x514); gRetryQ[2] = rr(0x518); gRetryQ[3] = rr(0x51c);
			gRetryAcc = *reinterpret_cast<void **>(static_cast<uint8_t *>(that) + 0x10);
			gRetryPend = 1;
			xeEv('RQUE', gRetryQ[0], gRetryQ[1], ++gRetrySeq);
			thread_call_enter(gRetryTc);
		}
	}
	return r;
}
static uint64_t xeS5Pause(void *that) {
	uint32_t n = ++gS5PauseN;
	if (n <= 40) IOLog("XeNG SCHED pause #%u (resume %u)\n", n, gS5ResumeN);
	return FunctionCast(xeS5Pause, gOrgS5Pause)(that);
}
static uint64_t xeS5Resume(void *that) {
	uint32_t n = ++gS5ResumeN;
	if (n <= 40) IOLog("XeNG SCHED resume #%u (pause %u)\n", n, gS5PauseN);
	return FunctionCast(xeS5Resume, gOrgS5Resume)(that);
}

// xe/ringdiag (19/10): por índice de stamp, cada submitToRing (anel -> hardware) e o último writeStamp emitido.
static mach_vm_address_t gOrgRbSubmit = 0, gOrgRbWriteStamp = 0, gOrgRbSemPoll = 0;
static volatile uint32_t gRbCount[64], gRbLastStamp[64];
static uint64_t xeRbWriteStamp(void *that, uint32_t stamp, bool b) {
	int idx = *reinterpret_cast<int *>(static_cast<uint8_t *>(that) + 0x38);
	if (idx >= 0 && idx < 64) gRbLastStamp[idx] = stamp;
	return FunctionCast(xeRbWriteStamp, gOrgRbWriteStamp)(that, stamp, b);
}
static uint64_t xeRbSubmit(void *that) {
	auto *p = static_cast<uint8_t *>(that);
	int idx = *reinterpret_cast<int *>(p + 0x38);
	uint32_t tail = *reinterpret_cast<uint32_t *>(p + 0x64);
	uint32_t c = (idx >= 0 && idx < 64) ? ++gRbCount[idx] : 0;
	uint64_t r = FunctionCast(xeRbSubmit, gOrgRbSubmit)(that);
	xeEv('RING', (uint32_t)idx, c, tail);
	return r;
}
static uint64_t xeRbSemPoll(void *that, uint32_t value, int widx) {
	int idx = *reinterpret_cast<int *>(static_cast<uint8_t *>(that) + 0x38);
	uint64_t r = FunctionCast(xeRbSemPoll, gOrgRbSemPoll)(that, value, widx);
	static volatile uint32_t n = 0;
	if (++n <= 60) xeEv('SEMW', (uint32_t)idx, (uint32_t)widx, value);
	return r;
}

// xe/csbdiag (19/10): cada entrada do CSB processada pelo escalonador 5: palavras brutas (lo/hi), o tipo que o driver
// deduz (saída de processCSBDetail) e o estado das portas da execlist antes do tratador.
static mach_vm_address_t gOrgCsbDetail = 0;
#define XE_CSB_MAX 4096
struct XeCsbEnt { uint32_t cs, idx, lo, hi, tipo, cur, portas; uint64_t t; };
static XeCsbEnt gCsbLog[XE_CSB_MAX];
static uint32_t gCsbN = 0, gCsbPrinted = 0;
static uint64_t xeCsbDetail(void *that, uint32_t *out, uint32_t *idxp) {
	auto *cs = static_cast<uint8_t *>(that);
	uint32_t idx = idxp ? *idxp : 0xff, lo = 0, hi = 0;
	auto *acc = *reinterpret_cast<uint8_t **>(cs + 0x10);
	if (acc && idx < 6) {
		uint8_t *raw = nullptr;
		if (static_cast<int8_t>(acc[0x1190]) < 0) {
			auto *base = *reinterpret_cast<uint8_t **>(acc + 0x1240);
			auto *c28 = *reinterpret_cast<uint8_t **>(cs + 0x28);
			if (base && c28) raw = base + *reinterpret_cast<uint32_t *>(c28 + 0x29) + idx * 8;
		} else {
			auto *b68 = *reinterpret_cast<uint8_t **>(cs + 0x68);
			if (b68) raw = b68 + 0x40 + idx * 8;
		}
		if (raw) { lo = reinterpret_cast<volatile uint32_t *>(raw)[0]; hi = reinterpret_cast<volatile uint32_t *>(raw)[1]; }
	}
	uint64_t r = FunctionCast(xeCsbDetail, gOrgCsbDetail)(that, out, idxp);
	// sem IOLog aqui (pode ser contexto de interrupção): grava num buffer que o worker de 2 s imprime
	uint32_t k = __atomic_fetch_add(&gCsbN, 1, __ATOMIC_RELAXED);
	{  // 19/10: anel circular
		auto &e = gCsbLog[k % XE_CSB_MAX];
		e.cs = (uint32_t)(uintptr_t)that & 0xfff0; e.idx = idx; e.lo = lo; e.hi = hi; e.tipo = out ? out[0] : 0;
		e.cur = *reinterpret_cast<uint32_t *>(cs + 0x50);
		e.portas = (*reinterpret_cast<void **>(cs + 0x30) != nullptr) << 3 | (*reinterpret_cast<void **>(cs + 0x38) != nullptr) << 2 |
				   (*reinterpret_cast<void **>(cs + 0x40) != nullptr) << 1 | (*reinterpret_cast<void **>(cs + 0x48) != nullptr);
		e.t = mach_absolute_time();
	}
	return r;
}
// xe/evaldiag (19/10): eventos genéricos (tag, a, b, c) no mesmo esquema de buffer (seguro em interrupção).
#define XE_EV_MAX 16384
struct XeEv { uint32_t tag, a, b, c; uint64_t t; };
static XeEv gEvLog[XE_EV_MAX];
static uint32_t gEvN = 0, gEvPrinted = 0;
static void xeEv(uint32_t tag, uint32_t a, uint32_t b, uint32_t c) {
	uint32_t k = __atomic_fetch_add(&gEvN, 1, __ATOMIC_RELAXED);
	// 19/10: anel circular (antes enchia em ~130 s e os restarts seguintes ficavam sem eventos)
	{ auto &e = gEvLog[k % XE_EV_MAX]; e.tag = tag; e.a = a; e.b = b; e.c = c; e.t = mach_absolute_time(); }
}
static mach_vm_address_t gOrgEvalWait = 0, gOrgVerifyThr = 0, gOrgPrepSubmit = 0;
// xe/csbguard (19/10): pânico "EL[0] was invalidated!!" ao trocar o modo da tela: na interrupção de troca de contexto,
// csbProcessActiveToIdle recebeu um evento active->idle com o EL[0] já vazio (evento repetido/atrasado) e
// ContextStatusBufferValidate entrou em pânico. Descarta o evento nos dois casos do validate (EL nulo ou ID de contexto
// diferente) e registra 'CSBX'. -ngreennocsbguard desliga.
static mach_vm_address_t gOrgCsbA2I = 0, gOrgCsbPreempt = 0, gOrgCsbElSwitch = 0;
// xe/nopage (19/10): pânico em IGAccelResource::pageon (Ajustes > Telas e troca de DPI: o WallpaperImageExtension leu a
// lista de retângulos de um nível além do fim, numa página não mapeada; contagem 0x79a8). Com memória unificada, não
// paginar texturas: pageoff/pageon viram no-op, como os de IOAccelResource2 (corpo vazio). -ngreenpaging volta ao original.
static mach_vm_address_t gOrgPageoff = 0, gOrgPageon = 0;
static int xeNoPaging() {
	static int v = -1;
	// 19/10: REVERTIDO para opt-in: com o no-op as janelas ficaram vazias (pgoff=771 pgon=2069 em ~3 min; o pageon
	// é usado na preparação normal das texturas, não só depois de pressão de memória). Só com -ngreennopaging.
	if (v < 0) v = checkKernelArgument("-ngreennopaging") ? 1 : 0;
	return v;
}
// xe/pagesync (20/10): pageon/pageoff copiam IOSurface <-> device cache no motor de cópia (BCS) e voltam sem esperar;
// quem ordena o render/compute depois da cópia é o barrierSubmission da Apple (MI_SEMAPHORE_WAIT no stamp do BCS), que o
// Gen11::barrierSubmission (V130) pula no RPL. Sem isso o shader lia a superfície pela metade e as escritas voltavam
// misturadas (testes de 20/10: 3-53% certos com offset 32, 88-99% alinhado). Depois do original, espera o evento da
// cópia na CPU com IOAccelEventMachineFast2::finishEvent (vtable +0x188, o mesmo fallback do barrierSubmission da
// Apple). accel = resource+0x18, máquina de eventos = accel+0x380. -ngreennopagesync desliga.
static uint32_t gPageSyncN = 0;
static void xePageSync(void *that, void *ev) {
	static int on = -1;
	if (on < 0) on = checkKernelArgument("-ngreennopagesync") ? 0 : 1;
	if (on != 1 || !that || !ev) return;
	auto *acc = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(that) + 0x18);
	auto *em = acc ? *reinterpret_cast<void **>(acc + 0x380) : nullptr;
	auto *vt = em ? *reinterpret_cast<void ***>(em) : nullptr;
	if (!vt || !vt[0x188 / 8]) return;
	reinterpret_cast<void (*)(void *, void *)>(vt[0x188 / 8])(em, ev);
	gPageSyncN++;
}
static void xePageoff(void *that, void *ev, bool b, bool *out, uint64_t x) {
	if (xeNoPaging()) { gPageoffN++; return; }
	FunctionCast(xePageoff, gOrgPageoff)(that, ev, b, out, x);
	xePageSync(that, ev);
}
// xe/pageonguard (19/10): o no-op estragou as janelas. Guard cirúrgico: antes do original, para cada entrada de nível
// (resource+0x250, (faces=+0xac) x (níveis=+0xad), 40 B; contagem u16 em +0x18, array de pares u32 em +0x20) confere pelo
// pmap que o array inteiro está mapeado; se não estiver, zera a contagem (só aquele nível não é restaurado).
static mach_vm_address_t gPmapFindPhys = 0, gKernelPmap = 0;
static bool xeMapeado(uint64_t va) {
	if ((va >> 47) != 0x1ffff) return false;  // não canônico / não é endereço do kernel
	if (!gPmapFindPhys || !gKernelPmap) return true;
	void *kp = *reinterpret_cast<void **>(gKernelPmap);
	return reinterpret_cast<uint32_t (*)(void *, uint64_t)>(gPmapFindPhys)(kp, va) != 0;
}
// xe/vadiag (21/10, default on, -ngreennovadiag disables): hardware video decode returned empty frames. Every decode submission is
// "dropped on floor" by IOAccelFIFOChannel2::submitCommands because IGAccelVideoContext::bind_resource clears
// info.valid (+0x3c) for the FIRST relocation it binds (any handle; user-space bisect). This hook only classifies the
// failure, no behaviour change: *addr/*pitch get sentinels before the original call:
//   res==0                    -> IOAccelContext2::lookupResource failed (handle not in the IOAccelShared2 namespace)
//   res!=0, addr==sentinel    -> IOAccelContext2::bindResource failed (Tahoe: write check on accel+0xc90 bit 30 /
//                                res+0xd bit 1, or IOAccelResource2::prepare() -> map()/memory map list)
//   addr set, pitch==0        -> IGAccelResource::get_tex_data found no plane/size (resource layout/type)
// Logs the first 48 failures and counts all of them (gVaBindFail/gVaBindOk).
static mach_vm_address_t gOrgVaBind = 0;
static uint32_t gVaBindFail = 0, gVaBindOk = 0;
static bool xeVaBind(void *that, void *info, uint32_t handle, void **res, uint64_t *addr, uint32_t *pitch, bool write) {
	auto *inf = static_cast<uint8_t *>(info);
	uint8_t validBefore = inf ? inf[0x3c] : 0xff;
	if (addr) *addr = 0xdeadbeefdeadbeefULL;
	if (pitch) *pitch = 0xdeadbeef;
	bool r = FunctionCast(xeVaBind, gOrgVaBind)(that, info, handle, res, addr, pitch, write);
	if (r) { gVaBindOk++; return r; }
	if (gVaBindFail++ < 48) {
		auto *rs = (res && *res) ? static_cast<uint8_t *>(*res) : nullptr;
		const char *why = !rs ? "lookup" : (addr && *addr == 0xdeadbeefdeadbeefULL) ? "bindResource/prepare" : (pitch && *pitch == 0) ? "get_tex_data pitch=0" : "other";
		IOLog("XeNG VADIAG bind fail #%u: h=%#x w=%d valid %u->%u why=%s res=%p type=%#x f0d=%#x f0e=%#x prep=%u map=%p addr=%#llx pitch=%#x (ok=%u)\n",
		      gVaBindFail, handle, write, validBefore, inf ? inf[0x3c] : 0xff, why, rs, rs ? rs[0x14] : 0, rs ? rs[0xd] : 0, rs ? rs[0xe] : 0,
		      rs ? *reinterpret_cast<uint32_t *>(rs + 0x28) : 0, rs ? *reinterpret_cast<void **>(rs + 0x40) : nullptr,
		      addr ? (unsigned long long)*addr : 0ULL, pitch ? *pitch : 0, gVaBindOk);
	}
	return r;
}
static void xePageon(void *that, void *ev, bool b, uint64_t x) {
	if (xeNoPaging()) { gPageonN++; return; }
	static int guard = -1;
	// 20/10: opt-in (-ngreenpageonguard) até testar isolado
	// 20/10 01h20: LIGADO por padrão (os 3 últimos pânicos foram GP fault aqui: ponteiro não canônico); -ngreennopageonguard desliga
	if (guard < 0) guard = checkKernelArgument("-ngreennopageonguard") ? 0 : 1;
	if (guard == 1 && that) {
		auto *r = static_cast<uint8_t *>(that);
		auto *ent = *reinterpret_cast<uint8_t **>(r + 0x250);
		uint32_t n = (uint32_t)r[0xac] * (uint32_t)r[0xad];
		if (ent && n && n <= 4096 && xeMapeado((uint64_t)ent) && xeMapeado((uint64_t)ent + n * 40 - 1)) {
			for (uint32_t i = 0; i < n; i++) {
				uint8_t *e = ent + (uint64_t)i * 40;
				uint16_t cnt = *reinterpret_cast<uint16_t *>(e + 0x18);
				uint64_t arr = *reinterpret_cast<uint64_t *>(e + 0x20);
				if (!cnt || !arr) continue;
				bool ok = xeMapeado(arr) && xeMapeado(arr + (uint64_t)cnt * 8 - 1);
				for (uint64_t pg = (arr & ~0xFFFull) + 0x1000; ok && pg < arr + (uint64_t)cnt * 8; pg += 0x1000) ok = xeMapeado(pg);
				if (!ok) { *reinterpret_cast<uint16_t *>(e + 0x18) = 0; gPageonFix++; }
			}
		}
	}
	FunctionCast(xePageon, gOrgPageon)(that, ev, b, x);
	xePageSync(that, ev);
}
// 19/10: o mesmo pânico veio também de csbProcessPreempted (troca para "itens grandes"); os 3 tratadores que chamam
// ContextStatusBufferValidate passam pelo mesmo teste. elSwitch: também exige o EL[1] (+0x38), que ele lê em seguida.
static bool xeCsbEventoInvalido(void *that, const uint32_t *e, bool exigeEl1, uint32_t tag) {
	static int guard = -1;
	if (guard < 0) guard = checkKernelArgument("-ngreennocsbguard") ? 0 : 1;
	if (guard != 1 || !that || !e) return false;
	auto *cs = static_cast<uint8_t *>(that);
	uint32_t idx = *reinterpret_cast<uint32_t *>(cs + 0x50);
	auto *el = *reinterpret_cast<uint8_t **>(cs + 0x30 + (uint64_t)idx * 16);
	auto *el1 = *reinterpret_cast<uint8_t **>(cs + 0x38 + (uint64_t)idx * 16);
	uint32_t id = (e[1] >> 5) & 0x7ff;
	auto *ctx = el ? *reinterpret_cast<uint8_t **>(el) : nullptr;
	if (!el || !ctx || *reinterpret_cast<uint32_t *>(ctx + 0x7c) != id || (exigeEl1 && !el1)) {
		xeEv(tag, idx, id, ++gCsbSkipN);
		return true;
	}
	return false;
}
// xe/csbresync (21/10): hang do WindowServer (antes do login e após troca de modo/DPI). Em todo travamento o
// CSBX descartou o "active->idle" REAL de um contexto curto (~200 us após o EXEC): o "idle->active" correspondente
// não foi processado, então o elemento ficou no slot pendente (1-cur) e EL[cur] estava vazio. Descartar deixa a
// execlist "ocupada" para sempre com a GPU ociosa (EXECLIST_STATUS 0x18001): o stamp nunca anda, o IOAF chama
// eventTimeout -> checkForProgress (3,3 s) -> getActiveContexts vazio -> "GPURestartSkipped / no channel" a cada 5 s.
// Em vez de descartar: se EL[cur] está vazio e o slot pendente tem exatamente esse contexto, sintetiza o
// idle->active que faltou (csbProcessIdleToActive original: troca cur e limpa o slot velho) e processa o evento.
// Eventos realmente repetidos/atrasados (nenhum slot bate) continuam descartados. -ngreennocsbresync desliga.
static mach_vm_address_t gOrgCsbI2A = 0;  // só resolvido, não roteado
static bool xeCsbResync(void *that, const uint32_t *e) {
	static int on = -1;
	if (on < 0) on = checkKernelArgument("-ngreennocsbresync") ? 0 : 1;
	if (on != 1 || !gOrgCsbI2A || !that || !e) return false;
	auto *cs = static_cast<uint8_t *>(that);
	uint32_t idx = *reinterpret_cast<uint32_t *>(cs + 0x50);
	if (idx > 1) return false;
	uint32_t pend = 1 - idx;
	auto *elCur = *reinterpret_cast<uint8_t **>(cs + 0x30 + (uint64_t)idx * 16);
	auto *elPend = *reinterpret_cast<uint8_t **>(cs + 0x30 + (uint64_t)pend * 16);
	auto *ctxPend = elPend ? *reinterpret_cast<uint8_t **>(elPend) : nullptr;
	uint32_t id = (e[1] >> 5) & 0x7ff;
	if (elCur || !ctxPend || *reinterpret_cast<uint32_t *>(ctxPend + 0x7c) != id) return false;
	reinterpret_cast<void (*)(void *, const uint32_t *)>(gOrgCsbI2A)(that, e);  // não lê 'e'; cur := pend
	xeEv('CSBR', idx, id, ++gCsbResyncN);
	return true;
}
static void xeCsbA2I(void *that, const uint32_t *e) {
	if (xeCsbEventoInvalido(that, e, false, 'CSBX')) {
		if (!xeCsbResync(that, e)) return;
		if (xeCsbEventoInvalido(that, e, false, 'CSBY')) return;  // não deveria acontecer
	}
	FunctionCast(xeCsbA2I, gOrgCsbA2I)(that, e);
}
static void xeCsbPreempt(void *that, const uint32_t *e) {
	if (xeCsbEventoInvalido(that, e, false, 'CSBP')) return;
	FunctionCast(xeCsbPreempt, gOrgCsbPreempt)(that, e);
}
static void xeCsbElSwitch(void *that, const uint32_t *e) {
	if (xeCsbEventoInvalido(that, e, true, 'CSBS')) return;
	FunctionCast(xeCsbElSwitch, gOrgCsbElSwitch)(that, e);
}
static uint64_t xeEvalWait(void *that, const bool *thr, bool x, bool y) {
	uint64_t r = FunctionCast(xeEvalWait, gOrgEvalWait)(that, thr, x, y);
	auto *cs = static_cast<uint8_t *>(that);
	(void)cs; (void)thr; (void)x; (void)y;  // 19/10: EVAL não é mais registrado (tempestade enchia o buffer)
	return r;
}
static uint64_t xeVerifyThr(void *that, bool *out) {
	uint64_t r = FunctionCast(xeVerifyThr, gOrgVerifyThr)(that, out);
	// 19/10: era chamado ~6000x/s (dequeueExecList em laço) e o log disso inundava o dmesg; agora só conta
	__atomic_fetch_add(&gThrtN, 1, __ATOMIC_RELAXED);
	if (out && *out) __atomic_fetch_add(&gThrtSimN, 1, __ATOMIC_RELAXED);
	return r;
}
// xe/fwsubmit (19/10): a execlist escrita logo depois do fim do CSB se perdia (motor recém-ocioso em RC6). Segura o
// forcewake de Render+Blitter num bit próprio (ctx=3) em volta da submissão. Desliga com -ngreennofwsubmit.
static uint64_t xePrepSubmit(void *that, uint32_t a, uint32_t b, const uint32_t *c) {
	xeEv('SUBM', (uint32_t)(uintptr_t)that & 0xfff0, a, b);
	static int fwSub = -1;
	if (fwSub < 0) fwSub = checkKernelArgument("-ngreennofwsubmit") ? 0 : 1;
	auto *acc = *reinterpret_cast<void **>(static_cast<uint8_t *>(that) + 0x10);
	if (fwSub == 1 && acc) Gen11::xeForceWakePub(acc, true, 0b101, 3);
	// xe/elsqwait (19/10): a submissão escrita logo depois do "terminou" de um contexto se perdia: o EXECLIST_STATUS
	// ainda mostrava o contexto anterior com o bit 0x40000000 (troca/salvamento em andamento). No RPL a escrita na
	// ELSQ nessa janela é descartada. Espera o bit cair (passos de 5 us, até 2 ms). Desliga com -ngreennoelsqwait.
	static int elsqWait = -1;
	if (elsqWait < 0) elsqWait = checkKernelArgument("-ngreennoelsqwait") ? 0 : 1;
	auto *c28 = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(that) + 0x28);
	if (elsqWait == 1 && NGreen::callback && c28) {
		uint32_t eb = c28[0] == 0 ? 0x2000 : 0x22000;
		uint32_t st0 = NGreen::callback->readReg32(eb + 0x234), st = st0, us = 0;
		while ((st & 0x40000000) && us < 2000) { IODelay(5); us += 5; st = NGreen::callback->readReg32(eb + 0x234); }
		if (st0 & 0x40000000) xeEv('WAIT', st0, st, us);
		// xe/elsqdelay (19/10): a submissão escrita no mesmo instante em que o contexto anterior termina se perde
		// (com ctx7 também, não só ctx16). Atraso curto antes de cada submissão do RCS; ngreenelsqdelay=N (us), 0 desliga.
		static int dly = -1;
		if (dly < 0) { int v = 0; PE_parse_boot_argn("ngreenelsqdelay", &v, sizeof(v)); dly = (v >= 0 && v <= 5000) ? v : 50; }
		if (eb == 0x2000) {
			uint32_t pre = NGreen::callback->readReg32(eb + 0x234), preHi = NGreen::callback->readReg32(eb + 0x238);
			uint32_t csp = NGreen::callback->readReg32(eb + 0x3a0);
			xeEv('PRE0', pre, preHi, csp);
			// xe/elsqidle (19/10): submissões boas encontram EXECLIST_STATUS lo = 0x...01 (ocioso, hi=0); a perdida
			// encontrou 0x00018098 com o contexto anterior ainda atual (hi=ID), mesmo depois do CSB "terminou".
			// Espera o motor sair desse estado (bit 0x80 ou byte baixo != 0x01), passos de 5 us, até 2 ms.
			// Só quando havia contexto atual (hi != 0); -ngreennoelsqidle desliga.
			static int idleW = -1;
			// 20/10: DESLIGADO por padrão: com o SAMETAIL não houve mais eventos inválidos (csbx=0) e as esperas somavam
			// 8-16 s de CPU parada em ~3 min (mouse a cada 20 s, congelamento). -ngreenelsqidle religa.
			// 20/10 02h30: RELIGADO: sem ele voltou o hang do Blit3D lendo página scratch (c01dbead) — 2 boots seguidos,
			// nunca com a espera ligada. Limite agora de 5 ms. -ngreennoelsqidle desliga.
			if (idleW < 0) idleW = checkKernelArgument("-ngreennoelsqidle") ? 0 : 1;
			// 19/10 (XE-NOPREEMPT): as recusas acontecem com arbitragem ligada (bit 16) e contexto atual — a nova
			// submissão dispara preempção, que emperra o RCS no RPL. Espera até 50 ms (ngreenpreemptwait=us) o motor
			// ficar ocioso (byte baixo 0x01) ou a arbitragem desligar, para nunca preemptar.
			static int maxw = -1;
			// 19/10: 50 ms -> 2 ms. Medido: 1444 esperas = 5,4 s em ~3 min, 8 batendo no limite (travadas visíveis). A causa dos
				// hangs que a espera contornava (kernels blit3d zerados, VS herdado) já foi corrigida.
				// 19/10 23h45: 2 ms trouxe de volta artefatos (861 envios com o motor ocupado em ~3 min); meio-termo de 15 ms.
				// 20/10: de volta a 50 ms (valor da "versão boa"); 2 e 15 ms vieram junto com glitches novos
				if (maxw < 0) { int v = 5000; PE_parse_boot_argn("ngreenpreemptwait", &v, sizeof(v)); maxw = (v >= 0 && v <= 200000) ? v : 5000; }
			// 19/10 (XE-IDLEANY): o hang do login começou numa submissão com o RCS ocupado SEM bit 16 (0x3098, ctx5
			// rodando o clear) que reenviou o ctx5 (lite restore) com o ctx7 no elemento 1. Agora espera o ocioso
			// mesmo sem arbitragem; -ngreenidlearb volta à condição antiga (só com bit 16).
			static int arbOnly = -1;
			if (arbOnly < 0) arbOnly = checkKernelArgument("-ngreenidlearb") ? 1 : 0;
			uint32_t arbMask = arbOnly ? 0x10000 : 0;
			if (idleW == 1 && preHi != 0 && (pre & 0xff) != 0x01 && (!arbMask || (pre & arbMask))) {
				uint32_t st = pre, hi = preHi, us = 0;
				while (((st & 0xff) != 0x01) && (!arbMask || (st & arbMask)) && hi != 0 && us < (uint32_t)maxw) {
					IODelay(5); us += 5;
					st = NGreen::callback->readReg32(eb + 0x234); hi = NGreen::callback->readReg32(eb + 0x238);
				}
				xeEv('IDLW', st, hi, us);
				gIdlwN++; gIdlwUs += us; if (us > gIdlwMax) gIdlwMax = us; if (us >= (uint32_t)maxw) gIdlwTimeout++;
			} else if (dly) IODelay((unsigned)dly);
		}
	}
	uint64_t r = FunctionCast(xePrepSubmit, gOrgPrepSubmit)(that, a, b, c);
	if (fwSub == 1 && acc) Gen11::xeForceWakePub(acc, false, 0b101, 3);
	return r;
}
static void xeEvDumpRecent(uint32_t count, int seq) {
	{
		uint32_t cn = __atomic_load_n(&gCsbN, __ATOMIC_RELAXED), cf = cn > 40 ? cn - 40 : 0;
		for (uint32_t i = cf; i < cn; i++) {
			auto &e = gCsbLog[i % XE_CSB_MAX];
			uint64_t ns = 0; absolutetime_to_nanoseconds(e.t, &ns);
			IOLog("XeNG D%d CSB #%u t=%llu.%06llu cs=%x idx=%u raw=%08x:%08x tipo=%08x cur=%u portas=%x\n", seq, i + 1,
				  ns / 1000000000ULL, (ns / 1000ULL) % 1000000, e.cs, e.idx, e.hi, e.lo, e.tipo, e.cur, e.portas);
		}
	}
	uint32_t n = __atomic_load_n(&gEvN, __ATOMIC_RELAXED);
	uint32_t from = n > count ? n - count : 0;
	IOLog("XeNG CSBDUMP #%d: eventos %u..%u (skip=%u)\n", seq, from + 1, n, gCsbSkipN);
	for (uint32_t i = from; i < n; i++) {
		auto &e = gEvLog[i % XE_EV_MAX];
		uint64_t ns = 0; absolutetime_to_nanoseconds(e.t, &ns);
		char tag[5] = {(char)(e.tag >> 24), (char)(e.tag >> 16), (char)(e.tag >> 8), (char)e.tag, 0};
		IOLog("XeNG D%d #%u t=%llu.%06llu %s a=%x b=%x c=%x\n", seq, i + 1, ns / 1000000000ULL, (ns / 1000ULL) % 1000000, tag, e.a, e.b, e.c);
	}
}
static void xeEvFlush() {
	uint32_t n = __atomic_load_n(&gEvN, __ATOMIC_RELAXED);
	if (n - gEvPrinted > XE_EV_MAX - 1024) {  // o worker ficou para trás: pula para os mais recentes
		IOLog("XeNG EV perdidos %u\n", n - gEvPrinted - (XE_EV_MAX - 1024));
		gEvPrinted = n - (XE_EV_MAX - 1024);
	}
	uint32_t lim = gEvPrinted + 800;
	for (; gEvPrinted < n && gEvPrinted < lim; gEvPrinted++) {
		auto &e = gEvLog[gEvPrinted % XE_EV_MAX];
		uint64_t ns = 0; absolutetime_to_nanoseconds(e.t, &ns);
		char tag[5] = {(char)(e.tag >> 24), (char)(e.tag >> 16), (char)(e.tag >> 8), (char)e.tag, 0};
		IOLog("XeNG EV #%u t=%llu.%03llu %s a=%x b=%x c=%x\n", gEvPrinted + 1, ns / 1000000000ULL, (ns / 1000000ULL) % 1000, tag, e.a, e.b, e.c);
	}
}
static void xeCsbFlush() {
	uint32_t n = __atomic_load_n(&gCsbN, __ATOMIC_RELAXED);
	if (n - gCsbPrinted > XE_CSB_MAX - 256) gCsbPrinted = n - (XE_CSB_MAX - 256);
	uint32_t lim = gCsbPrinted + 400;
	for (; gCsbPrinted < n && gCsbPrinted < lim; gCsbPrinted++) {
		auto &e = gCsbLog[gCsbPrinted % XE_CSB_MAX];
		uint64_t ns = 0; absolutetime_to_nanoseconds(e.t, &ns);
		IOLog("XeNG CSB #%u t=%llu.%03llu cs=%x idx=%u raw=%08x:%08x tipo=%08x cur=%u portas=%x\n", gCsbPrinted + 1,
			  ns / 1000000000ULL, (ns / 1000000ULL) % 1000, e.cs, e.idx, e.hi, e.lo, e.tipo, e.cur, e.portas);
	}
}

// xe/posh (19/10): o ctx18 (1ª submissão nunca começa) vem de restoreFromSafeImage, que copia a imagem-modelo; em
// contextos SEM POSH (ctx+0x6e bit 4 = 0) o POSH_LRCA (0x21b0) fica com o buffer POSH do 1º contexto do boot.
// Única diferença estrutural para o ctx16 (que roda). Zera o POSH_LRCA na página 1 desses contextos.
// Desliga com -ngreennoposhfix.
static mach_vm_address_t gOrgRestoreSafe = 0, gSmbGetVA = 0;
static uint64_t xeRestoreSafe(void *that) {
	// xe/poshctx (20/10): o RCS emperra em "troca de elemento pendente" (0x18098) depois que um contexto POSH
	// (ctx+0x6e bit 0x10: CTX_CTRL bit4, buffer POSH, batches com bit 20) termina. O driver já diz PoshEnable=0 ao
	// userspace. Apaga a flag na criação (antes de montar a imagem), para o contexto ser comum de ponta a ponta.
	// -ngreenposhctx mantém o POSH.
	{
		static int keep = -1;
		if (keep < 0) keep = checkKernelArgument("-ngreenposhctx") ? 1 : 0;
		auto *c = static_cast<uint8_t *>(that);
		if (!keep && (c[0x6e] & 0x10)) {
			c[0x6e] &= ~0x10;
			static volatile uint32_t n = 0;
			if (++n <= 32) IOLog("XeNG POSHCTX: flag POSH apagada no contexto (#%u)\n", n);
		}
	}
	uint64_t r = FunctionCast(xeRestoreSafe, gOrgRestoreSafe)(that);
	static int arg = -1;
	if (arg < 0) arg = checkKernelArgument("-ngreennoposhfix") ? 0 : 1;
	auto *ctx = static_cast<uint8_t *>(that);
	if (arg != 1 || !(r & 1) || (ctx[0x6e] & 0x10) || !gSmbGetVA) return r;
	void *smb = *reinterpret_cast<void **>(ctx + 0x98);
	if (!smb) return r;
	auto *img = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uint8_t *(*)(void *)>(gSmbGetVA)(smb));
	if (!img) return r;
	volatile uint32_t *pg1 = img + 0x400;
	for (int k = 1; k + 1 < 0x100; k++) {
		if (pg1[k] == 0x21b0) {
			uint32_t antes = pg1[k + 1];
			pg1[k + 1] = 0;
			static volatile uint32_t n = 0;
			if (++n <= 32) IOLog("XeNG POSH ctx lrca=%x POSH_LRCA %x -> 0 (DW%x)\n", *reinterpret_cast<uint32_t *>(ctx + 0x89) & ~0xfffu, antes, k + 1);
			break;
		}
	}
	return r;
}

static mach_vm_address_t gHwCrtcToTimingAddr = 0;
static thread_call_t gFbtTc = nullptr;
static volatile uint32_t gFbtTries = 0, gFbtSeen = 0, gFbtOnline = 0, gFbtAlready = 0, gFbtBuilt = 0, gFbtRet = 0xffff;
static volatile uint32_t gFbtH = 0, gFbtV = 0, gFbtDone = 0;

static void fbTimingsWorker(thread_call_param_t, thread_call_param_t) {
	gFbtTries++;
	bool done = false;
	if (gHwCrtcToTimingAddr) {
		if (auto *match = IOService::serviceMatching("AppleIntelFramebufferDiags")) {
			if (auto *it = IOService::getMatchingServices(match)) {
				while (auto *obj = it->getNextObject()) {
					auto *fb = OSDynamicCast(IOService, obj);
					if (!fb || getMember<uint32_t>(fb, 0x1dc) != 0) continue;
					gFbtSeen = 1;
					if (getMember<uint8_t>(fb, 0x1e0) == 0) continue;
					gFbtOnline = 1;
					if (getMember<void *>(fb, 0x4260) != nullptr) { gFbtAlready = 1; done = true; break; }
					// xe (19/10): hwCRTCToIODetailedTimingInformation divide por p->0x164, com
					// p = *(fb + (fb->0x3fd8 != -1 ? 0x4a08 : 0x49f0)). Cedo no boot isso ainda vale 0 -> KERNEL PANIC
					// "divide error" (boot 23:56). Só chama quando o divisor já existe; senão tenta de novo em 50 ms.
					{
						auto *pp = getMember<uint8_t *>(fb, getMember<int32_t>(fb, 0x3fd8) != -1 ? 0x4a08 : 0x49f0);
						if (!pp || *reinterpret_cast<volatile uint32_t *>(pp + 0x164) == 0) continue;
					}
					IODetailedTimingInformationV2 t;
					bzero(&t, sizeof(t));
					auto ctrl = getMember<void *>(fb, 0x1d0);
					typedef uint32_t (*crtc_fn)(void *, void *, void *);
					gFbtRet = ((crtc_fn)gHwCrtcToTimingAddr)(ctrl, fb, &t);
					gFbtH = t.horizontalActive; gFbtV = t.verticalActive;
					if (t.horizontalActive == 0 || t.verticalActive == 0) continue; // CRTC ainda sem timing: tenta de novo
					// O primeiro dword da entrada é o ID do modo: getCurrentDisplayMode faz fCurrentMode = *(uint32_t *)entrada ao casar
					// com o CRTC, e validateDisplayMode exige ID >= 0x10000 (indice = ID & 0x3ff). 0x80001000 = indice 0.
					*reinterpret_cast<uint32_t *>(&t) = 0x80001000u;
					auto *arr = OSArray::withCapacity(1);
					auto *dat = OSData::withBytes(&t, sizeof(t));
					if (arr && dat) {
						arr->setObject(dat);
						fb->setProperty("IOFBDetailedTimings", arr);
						getMember<OSArray *>(fb, 0x4260) = arr;
						gFbtBuilt = 1;
						done = true;
						SYSLOG("ngreen", "fixbootmode: IOFBDetailedTimings injected early: %ux%u pclk=%llu", gFbtH, gFbtV,
							   (unsigned long long)t.pixelClock);
					}
					if (dat) dat->release();
					if (arr) arr->release(); // a propriedade mantém a referência
					break;
				}
				it->release();
			}
			match->release();
		}
	}
	if (done) { gFbtDone = 1; return; }
	if (gFbtTries < 6000 && gFbtTc) { // 50 ms x 6000 = 5 min
		uint64_t deadline;
		clock_interval_to_deadline(50, kMillisecondScale, &deadline);
		thread_call_enter_delayed(gFbtTc, deadline);
	}
}

bool Gen11::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (kextG11FB.loadIndex == index) {
		gIcLFbBlockRuns++;
		if (this->tglFBLoaded) {
			DBGLOG("ngreen", "Skipping ICL FB — TGL FB already loaded");
			return true;
		}
		auto *activeKext = &kextG11FB;
		DBGLOG("ngreen", "init AppleIntelICLLPGraphicsFramebuffer!");
		//NGreen::callback->igfxGen = iGFXGen::Gen11;
		NGreen::callback->setRMMIOIfNecessary();
		
		const bool wegCoexist = isWEGCoexistMode();
		if (wegCoexist) {
			SYSLOG("nblue", "WEG coexist mode enabled: skipping NootedBlue CDCLK route overlap");
		}

		if (wegCoexist) {
			SolveRequestPlus solveRequests[] = {
			//		{"__ZN31AppleIntelFramebufferController14disableCDClockEv", this->orgDisableCDClock},
			//		{"__ZN31AppleIntelFramebufferController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
			//		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments", this->orgFBClientDoAttribute},
			//		{"__ZN31AppleIntelFramebufferController5startEP9IOService",	this->ostart},
			//		{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",	this->oreadRegister32},
		 		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb", this->ohwConfigureCustomAUX},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "nblue",	"Failed to resolve symbols");
		} else {
			SolveRequestPlus solveRequests[] = {
			//		{"__ZN31AppleIntelFramebufferController14disableCDClockEv", this->orgDisableCDClock},
			//		{"__ZN31AppleIntelFramebufferController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
			//		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments", this->orgFBClientDoAttribute},
			//		{"__ZN31AppleIntelFramebufferController5startEP9IOService",	this->ostart},
			//		{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",	this->oreadRegister32},
		 		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb", this->ohwConfigureCustomAUX},
				{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv", this->orgProbeCDClockFrequency},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "nblue",	"Failed to resolve symbols");
		}
		
		if (wegCoexist) {
			RouteRequestPlus requests[] = {
			// Keep stock ReadRegister32 while stabilizing display pipeline behavior.
			//		{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",wrapReadRegister32,	this->owrapReadRegister32},
			//		{"__ZN21AppleIntelFramebuffer13SaveNVRAMModeEv",handleLinkIntegrityCheck},
			// Keep stock wake/sleep lifecycle handlers to avoid broken restore paths.
			//{"__ZN21AppleIntelFramebuffer18prepareToEnterWakeEv",dovoid},
			//{"__ZN21AppleIntelFramebuffer17prepareToExitWakeEv",dovoid},
			//{"__ZN21AppleIntelFramebuffer18prepareToExitSleepEv",dovoid},
			//{"__ZN21AppleIntelFramebuffer19prepareToEnterSleepEv",dovoid},
			// Keep stock doAttribute while chasing UI stalls / high WindowServer CPU.
			//		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments",wrapFBClientDoAttribute,	this->orgFBClientDoAttribute},
			//ADDED
			//{"__ZN21AppleIntelFramebuffer4initEP31AppleIntelFramebufferControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
			//		{"__ZN31AppleIntelFramebufferController10hwShutdownEP21AppleIntelFramebuffer",handleLinkIntegrityCheck},
				{"__ZN31AppleIntelFramebufferController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
			//		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
			//	{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "nblue","Failed to route symbols");
		} else {
			RouteRequestPlus requests[] = {
			// Keep stock ReadRegister32 while stabilizing display pipeline behavior.
			//		{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",wrapReadRegister32,	this->owrapReadRegister32},
			//		{"__ZN21AppleIntelFramebuffer13SaveNVRAMModeEv",handleLinkIntegrityCheck},
			// Keep stock wake/sleep lifecycle handlers to avoid broken restore paths.
			//{"__ZN21AppleIntelFramebuffer18prepareToEnterWakeEv",dovoid},
			//{"__ZN21AppleIntelFramebuffer17prepareToExitWakeEv",dovoid},
			//{"__ZN21AppleIntelFramebuffer18prepareToExitSleepEv",dovoid},
			//{"__ZN21AppleIntelFramebuffer19prepareToEnterSleepEv",dovoid},
			// Keep stock doAttribute while chasing UI stalls / high WindowServer CPU.
			//		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments",wrapFBClientDoAttribute,	this->orgFBClientDoAttribute},
			//ADDED
			//{"__ZN21AppleIntelFramebuffer4initEP31AppleIntelFramebufferControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
			//		{"__ZN31AppleIntelFramebufferController10hwShutdownEP21AppleIntelFramebuffer",handleLinkIntegrityCheck},
				{"__ZN31AppleIntelFramebufferController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
			//		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
			//	{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
				{"__ZN31AppleIntelFramebufferController11initCDClockEv",initCDClock,this->oinitCDClock}
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "nblue","Failed to route symbols");
		}

		//static const uint8_t f15[]= {0x00,0x02, 0x00, 0x5c, 0x8a};
		//static const uint8_t r15[]= {0x00,0x00, 0x00, 0x49, 0x9a};
		
		
		// AppleIntelFramebufferController::hwSetMode skip hwRegsNeedUpdate
		static const uint8_t f2[] = {0xE8, 0x31, 0xE5, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x3D};
		static const uint8_t r2[] = {0xE8, 0x31, 0xE5, 0xFF, 0xFF, 0x84, 0xC0, 0xEB, 0x3D};
		
		//sonoma
		static const uint8_t f2b[] = {0xE8, 0x54, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x5C};
		static const uint8_t r2b[] = {0xE8, 0x54, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0xeb, 0x5C};
		
		//sequoia
		static const uint8_t f2c[] = {0xE8, 0x74, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x5C};
		static const uint8_t r2c[] = {0xE8, 0x74, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0xeb, 0x5C};
		
		/*if (getKernelVersion() <= KernelVersion::Ventura) {
			KernelPatcher::LookupPatch patch { &kextG11FB, f2, r2, sizeof(f2), 1 };
			patcher.applyLookupPatch(&patch);
		}
		
		if (getKernelVersion() == KernelVersion::Sonoma) {
			KernelPatcher::LookupPatch patchb { &kextG11FB, f2b, r2b, sizeof(f2b), 1 };
			patcher.applyLookupPatch(&patchb);
		}
		
		if (getKernelVersion() >= KernelVersion::Sequoia) {
			KernelPatcher::LookupPatch patchc { &kextG11FB, f2c, r2c, sizeof(f2c), 1 };
			patcher.applyLookupPatch(&patchc);
		}*/
		
		

		// Variant-consistent remap for constructor entries:
		// B8 xx 00 5C 8A -> B8 xx 00 49 9A and exact C7 05 ... 02 00 5C 8A site.
		static const uint8_t kPatchPlatformRemapMovEaxFind0[] = {0xB8, 0x00, 0x00, 0x5C, 0x8A};
		static const uint8_t kPatchPlatformRemapMovEaxReplace0[] = {0xB8, 0x00, 0x00, 0x49, 0x9A};
		static const uint8_t kPatchPlatformRemapMovEaxFind1[] = {0xB8, 0x01, 0x00, 0x5C, 0x8A};
		static const uint8_t kPatchPlatformRemapMovEaxReplace1[] = {0xB8, 0x01, 0x00, 0x49, 0x9A};
		static const uint8_t kPatchPlatformRemapMovEaxFind2[] = {0xB8, 0x02, 0x00, 0x5C, 0x8A};
		static const uint8_t kPatchPlatformRemapMovEaxReplace2[] = {0xB8, 0x02, 0x00, 0x49, 0x9A};
		static const uint8_t kPatchPlatformRemapC705Find2[] = {0xC7, 0x05, 0xE9, 0x9B, 0x05, 0x00, 0x02, 0x00, 0x5C, 0x8A};
		static const uint8_t kPatchPlatformRemapC705Replace2[] = {0xC7, 0x05, 0xE9, 0x9B, 0x05, 0x00, 0x02, 0x00, 0x49, 0x9A};

		// hwSetMode: bypass hwRegsNeedUpdate result (CALL hwRegsNeedUpdate; TEST AL,AL: JE+0x62 → JMP+0x62)
		// Verified unique (1 match at 0x94055) in ICL LP le binary. Forces register reprogram unconditionally. [ICL-LP]
		static const uint8_t kPatchHwRegsNeedUpdateBypassFind[] = {0xe8, 0xe2, 0xcc, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x62};
		static const uint8_t kPatchHwRegsNeedUpdateBypassReplace[] = {0xe8, 0xe2, 0xcc, 0xff, 0xff, 0x84, 0xc0, 0xeb, 0x62};

		LookupPatchPlus const minPatches[] = {
			{&kextG11FB, kPatchPlatformRemapMovEaxFind0, kPatchPlatformRemapMovEaxReplace0, arrsize(kPatchPlatformRemapMovEaxFind0), 1},
			{&kextG11FB, kPatchPlatformRemapMovEaxFind1, kPatchPlatformRemapMovEaxReplace1, arrsize(kPatchPlatformRemapMovEaxFind1), 1},
			{&kextG11FB, kPatchPlatformRemapMovEaxFind2, kPatchPlatformRemapMovEaxReplace2, arrsize(kPatchPlatformRemapMovEaxFind2), 1},
			{&kextG11FB, kPatchPlatformRemapC705Find2, kPatchPlatformRemapC705Replace2, arrsize(kPatchPlatformRemapC705Find2), 1},
			{&kextG11FB, kPatchHwRegsNeedUpdateBypassFind, kPatchHwRegsNeedUpdateBypassReplace, arrsize(kPatchHwRegsNeedUpdateBypassFind), 1},  // hwSetMode always reprogram [ICL-LP]
		};
		
		PANIC_COND(!LookupPatchPlus::applyAll(patcher, minPatches , address, size), "ngreen", "kextG11FB Failed to apply patches!");
		//PANIC_COND
		
		DBGLOG("ngreen", "Loaded AppleIntelICLLPGraphicsFramebuffer!");
		return true;
		
		
	}	else if (kextG11FBT.loadIndex == index || kextG11FBTA.loadIndex == index) {
		gTglFbBlockRuns++;
		this->tglFBLoaded = true;
		auto *activeKext = (kextG11FBTA.loadIndex == index) ? &kextG11FBTA : &kextG11FBT;
		NGreen::callback->setRMMIOIfNecessary();
		SYSLOG("ngreen", "init AppleIntelTGLGraphicsFramebuffer");
		
		bool isprod=false;
		auto prod=patcher.solveSymbol(index, "__ZN24AppleIntelBaseController5startEP9IOService", address, size);
		if (!prod) isprod=true;
		
		if (isprod) {
			
			SolveRequestPlus solveRequests[] = {
				{"__ZN31AppleIntelFramebufferController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
				{"_gPlatformInformationList", this->gPlatformInformationList},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "ngreen",	"Failed to resolve symbols");
		}
		else
		{
			SolveRequestPlus solveRequests[] = {
				{"__ZN24AppleIntelBaseController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
				{"_gPlatformInformationList", this->gPlatformInformationList},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "ngreen",	"Failed to resolve symbols");
			
		}

		RouteRequestPlus requests[] = {
			// ...existing routes...
			//{"__ZN24AppleIntelBaseController17registerWithAICPMEPv", alwaysReturnSuccess, this->oalwaysReturnSuccess},
			// ...existing routes...
			/*{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.1",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.2",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.3",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.4",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.5",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.6",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.7",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.8",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.9",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.10",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.11",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.12",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell22hwSetPowerWellStateAuxEbj.cold.1",releaseDoorbell},*/
			// V204: re-enable AppleIntelScaler/Plane init constructors so ccont is set
			// once at object construction. Friend's working version routes only these and
			// keeps the per-method patches commented; we keep both as belt-and-suspenders.
			{"__ZN16AppleIntelScaler4initE10IGScalerID", AppleIntelScalerinit, this->oAppleIntelScalerinit},
			{"__ZN15AppleIntelPlane4initE9IGPlaneID",     AppleIntelPlaneinit,  this->oAppleIntelPlaneinit},
			// AppleIntelPlaneinit/Scalerinit hooks are disabled in kern_genx.cpp, so fields
			// 0x90 (plane RAM) and 0x28/0x10 (scaler RAM) are never set at object-init time.
			// Every entry point into Plane/Scaler that calls ReadRegister32 will crash with
			// this=NULL unless we patch ccont in here.  These hooks must stay until the init
			// hooks are re-enabled (see kern_genx.cpp lines 75-76).
			{"__ZN16AppleIntelScaler13disableScalerEb",disableScaler, this->odisableScaler},
			{"__ZN15AppleIntelPlane11enablePlaneEb",enablePlane, this->oenablePlane},
			{"__ZN16AppleIntelScaler17programPipeScalerEP21AppleIntelDisplayPath",programPipeScaler, this->oprogramPipeScaler},
			// V400: read-only logger for setupPipeScaler. Confirms whether Apple's
			// pipe scaler is downscaling (PIPE_SRCSZ vs PS_PS_WIN_SZ mismatch) — if
			// yes, that's a direct cause of the fragmented/repeated scanout.
			{"__ZN16AppleIntelScaler15setupPipeScalerEP21AppleIntelDisplayPathP10CRTCParams", setupPipeScaler, this->osetupPipeScaler},
			// V401: paramsSurfCompare — read-only. Fires per flip. Logs PLANE_CTL
			// tiling bits (mask 0xF800000), PLANE_STRIDE (+0x18), PLANE_SURF (+0x20)
			// for both old and new PLANEPARAMS, plus CRTCParams PIPE_SRCSZ.
			{"__ZN24AppleIntelBaseController17paramsSurfCompareEP10CRTCParamsS1_PN15AppleIntelPlane11PLANEPARAMSES4_", paramsSurfCompare, this->oparamsSurfCompare},
			// V402: setupDSCEngineParams — read-only. Logs entry of DSC config path.
			// Linux says DSC=off; this hook reveals whether Apple still goes through it.
			{"__ZN24AppleIntelBaseController20setupDSCEngineParamsEP21AppleIntelFramebufferP10CRTCParamsP21AppleIntelDisplayPathP29IODetailedTimingInformationV2", setupDSCEngineParams, this->osetupDSCEngineParams},
			// V403: SetupParams — master CRTCParams builder. Read-only logger of all
			// key fields after Apple populates the struct. Single chokepoint where
			// future origin-level overrides should land.
			{"__ZN24AppleIntelBaseController11SetupParamsEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2", setupParams, this->osetupParams},
			// V404: setupPipeWatermarks — runs INSIDE SetupParams before setupPipeScaler.
			// Per-pipe DBUF/watermark allocator. Prime suspect for setting PIPE_SEAM_EXCESS=0x1.
			{"__ZN24AppleIntelBaseController19setupPipeWatermarksEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParams", setupPipeWatermarks, this->osetupPipeWatermarks},
			// V405: AppleIntelPlane::configureColorPipeLine(FlipTransactionArgs*, bool)
			// Dispatches 8/10/12/12SEG gamma pipeline based on FlipTransactionArgs BPC mode.
			// Hook logs GAMMA_MODE (0x4A480) and PIPE_MISC (0x70030) before/after the call
			// to confirm Apple writes correct values for ADL-P Display 13 on our 8bpc eDP.
			{"__ZN15AppleIntelPlane22configureColorPipeLineEP19FlipTransactionArgsb", configureColorPipeLine, this->oConfigureColorPipeLine},
			// V406: AppleIntelPlane::configurePlane(FlipTransactionArgs*)
			// Patches FlipTransactionArgs+0x3c tiling enum to 2 (linear) so Apple builds
			// PLANE_CTL with bits[12:10]=000 and PLANE_STRIDE=pitch/512 natively.
			{"__ZN15AppleIntelPlane14configurePlaneEP19FlipTransactionArgs", configurePlane, this->oConfigurePlane},
			{"__ZN15AppleIntelPlane19updateRegisterCacheEv",AppleIntelPlaneupdateRegisterCache, this->oAppleIntelPlaneupdateRegisterCache},
			{"__ZN16AppleIntelScaler19updateRegisterCacheEv",AppleIntelScalerupdateRegisterCache, this->oAppleIntelScalerupdateRegisterCache},
			{"__ZN19AppleIntelPowerWell20disableDisplayEngineEv",disableDisplayEngine, this->odisableDisplayEngine},
			{"__ZN19AppleIntelPowerWell19enableDisplayEngineEv",enableDisplayEngine, this->oenableDisplayEngine},
			// V35: Removed ComboPhyEv hook — causes MCE on RPL/ADL. Firmware calibration sufficient.
			{"__ZN14AppleIntelPort16computeLaneCountEPK29IODetailedTimingInformationV2jjPj",computeLaneCount, this->ocomputeLaneCount},
			{"__ZN14AppleIntelPort21setupOptimalLaneCountEPK29IODetailedTimingInformationV2j",setupOptimalLaneCount, this->osetupOptimalLaneCount},
			// V97: Log AUX transactions to diagnose eDP link training failures on RPL
			{"__ZN14AppleIntelPort7readAUXEjPvj", wrapICLReadAUX, this->orgICLReadAUX},
			// V96: Force display online — WEG's getDisplayStatus hook (FOD) fails with
			// "err 2" on TGL kext because that symbol doesn't exist. TGL uses getOnlineInfo.
			{"__ZN21AppleIntelFramebuffer13getOnlineInfoEP21AppleIntelDisplayPathPhS2_", getOnlineInfo, this->ogetOnlineInfo},
			// 20/10 (XE-FBDPMS): repouso da tela sem desmontar pipe/transcoder/DDI (ver wrapSetFramebufferPowerState).
			{"__ZN21AppleIntelFramebuffer24setFramebufferPowerStateEj", wrapSetFramebufferPowerState, this->oSetFramebufferPowerState},
			// V208/V209/V210 routes DISABLED at 3/3/3 baseline. They actively cause
			// the IOFramebuffer::getAttributeExt+0x25 NULL deref when fb1/fb2 start()
			// is refused but IGAccelDisplayPipe still tracks them. Re-enable only
			// alongside pinfo 1/1/1 if pursuing FB-count-reduction direction.
			//{"__ZN21AppleIntelFramebuffer5startEP9IOService", wrapAppleIntelFramebufferStart, this->oAppleIntelFramebufferStart},
			//{"__ZN21AppleIntelFramebuffer16enableControllerEv", wrapEnableController, this->oEnableController},
			//{"__ZN21AppleIntelDisplayPath24getFreeJoinablePathCountEv", wrapGetFreeJoinablePathCount, this->oGetFreeJoinablePathCount},
			// Path B: Force aperture memory under dp0 mode to prevent WS=0x3 migration to
			// non-aperture, which leaves the display scanning empty pages and triggers
			// the 0x3→0x1 WS degradation.
			{"__ZN21AppleIntelFramebuffer24isApertureMemoryRequiredEv", wrapIsApertureMemoryRequired, this->oIsApertureMemoryRequired},
			// Path C: Coerce kIOWindowServerActiveAttribute=0x1 (WS degrade) to 0x3 (stay-active)
			// under dp0 mode so kernel-tracked fWSAAState never drops below 3 once WS goes active.
			{"__ZN21AppleIntelFramebuffer12setAttributeEjm", wrapSetAttribute, this->oSetAttribute},
			// V183: write-only ADL-P power well handler; no callthrough (TGL poll loop hangs on RPL).
			// Real TGL falls through to original via ohwSetPowerWellStatePGE.
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj", hwSetPowerWellStatePGE, this->ohwSetPowerWellStatePGE},
			{"__ZN19AppleIntelPowerWell22hwSetPowerWellStateAuxEbj",hwSetPowerWellStateAux, this->ohwSetPowerWellStateAux},
			{"__ZN19AppleIntelPowerWell22hwSetPowerWellStateDDIEbj",hwSetPowerWellStateDDI, this->ohwSetPowerWellStateDDI},
			{"__ZN31AppleIntelRegisterAccessManager19FastWriteRegister32Emj",FastWriteRegister32, this->oFastWriteRegister32},
			// V60: ReadRegister32 hooks DISABLED — V59 proved they cause 0-children regression
			// (display driver loops in forceWake power-well cycling, never completes init)
			/*{"__ZN31AppleIntelRegisterAccessManager14ReadRegister32Em",raReadRegister32, this->oraReadRegister32},
			{"__ZN31AppleIntelRegisterAccessManager14ReadRegister32EPVvm",raReadRegister32b},*/
			{"__ZN31AppleIntelRegisterAccessManager15WriteRegister32Emj",raWriteRegister32, this->oraWriteRegister32},
			{"__ZN31AppleIntelRegisterAccessManager15WriteRegister32EPVvmj",raWriteRegister32b},
			// V410: hook sleep/wake transition methods to intercept panel state tracking.
			// Apple's implementations are known to corrupt WS state on RPL/ADL on resume.
			{"__ZN21AppleIntelFramebuffer17prepareToExitWakeEv",   prepareToExitWake,   this->oprepareToExitWake},
			{"__ZN21AppleIntelFramebuffer18prepareToEnterWakeEv",  prepareToEnterWake,  this->oprepareToEnterWake},
			{"__ZN21AppleIntelFramebuffer18prepareToExitSleepEv",  prepareToExitSleep,  this->oprepareToExitSleep},
			{"__ZN21AppleIntelFramebuffer19prepareToEnterSleepEv", prepareToEnterSleep, this->oprepareToEnterSleep},
			{"__ZN24AppleIntelBaseController15enableVDDForAuxEP14AppleIntelPort", releaseDoorbell},
			// Keep native SST timing setup; forcing custom clocks can break CoreDisplay validation.
			//{"__ZN24AppleIntelBaseController17SetupDPSSTTimingsEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParams", SetupDPSSTTimings, this->oSetupDPSSTTimings},
			//{"__ZN24AppleIntelBaseController12SetupTimingsEP21AppleIntelFramebufferP21AppleIntelDisplayPathPK29IODetailedTimingInformationV2P10CRTCParams", SetupTimings, this->oSetupTimings},
			// Keep native detailed timing validation; avoid overriding pixel clock fields.
			//{"__ZN21AppleIntelFramebuffer22validateDetailedTimingEPvy", validateDetailedTiming, this->ovalidateDetailedTiming},
			{"__ZN21AppleIntelFramebuffer19validateDisplayModeEiPPKNS_15ModeDescriptionEPPK29IODetailedTimingInformationV2", validateDisplayMode, this->ovalidateDisplayMode},
			{"__ZN21AppleIntelFramebuffer21getCurrentDisplayModeEPiS0_", getCurrentDisplayMode, this->ogetCurrentDisplayMode},
	   		//{"__ZN21AppleIntelFramebuffer18setupDisplayTimingEPK29IODetailedTimingInformationV2PS0_", setupDisplayTiming, this->osetupDisplayTiming},
			//{"__ZN21AppleIntelFramebuffer18maxSupportedDepthsEPK29IODetailedTimingInformationV2", maxSupportedDepths, this->omaxSupportedDepths},
			//{"__ZN21AppleIntelFramebuffer17validateModeDepthEPK29IODetailedTimingInformationV2j", validateModeDepth, this->ovalidateModeDepth},
			//*****
			{"__ZN21AppleIntelFramebuffer19getPixelInformationEiiiP18IOPixelInformation", getPixelInformation, this->ogetPixelInformation},
			//{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments",wrapFBClientDoAttribute, this->orgFBClientDoAttribute},
			//{"__ZN20IntelFBClientControl24vendor_doDeviceAttributeEjPmmS0_S0_P25IOExternalMethodArguments", releaseDoorbell},
			//{"__ZN21AppleIntelFramebuffer16enableControllerEv", isPanelPowerOn},
		};
		PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route dp symbols");
		gTglFbRoutesDone++;
		if (checkKernelArgument("-ngreenfixbootmode")) {
			SolveRequestPlus crtcSolve[] = {
				{"__ZN24AppleIntelBaseController35hwCRTCToIODetailedTimingInformationEP21AppleIntelFramebufferP29IODetailedTimingInformationV2", gHwCrtcToTimingAddr},
			};
			if (SolveRequestPlus::solveAll(patcher, index, crtcSolve, address, size) && gHwCrtcToTimingAddr && !gFbtTc) {
				gFbtTc = thread_call_allocate(fbTimingsWorker, nullptr);
				if (gFbtTc) {
					uint64_t deadline;
					clock_interval_to_deadline(50, kMillisecondScale, &deadline);
					thread_call_enter_delayed(gFbtTc, deadline);
				}
			}
		}
		gOrgGcdm = this->ogetCurrentDisplayMode; gOrgGpi = this->ogetPixelInformation; gOrgVdm = this->ovalidateDisplayMode;
		SYSLOG("ngreen", "TGL FB routes applied (fixbootmode: getCurrentDisplayMode org=0x%llx, arg=%d)",
			   (unsigned long long)this->ogetCurrentDisplayMode, (int)checkKernelArgument("-ngreenfixbootmode"));
		
		if (isprod) {
			RouteRequestPlus requests[] = {
				{"__ZN21AppleIntelFramebuffer4initEP31AppleIntelFramebufferControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
				{"__ZN31AppleIntelFramebufferController23initPlatformWorkaroundsEv", initPlatformWorkarounds, this->oinitPlatformWorkarounds},
				{"__ZN31AppleIntelFramebufferController16getOSInformationEv", getOSInformation, this->ogetOSInformation},
				{"__ZN31AppleIntelFramebufferController10hwShutdownEP21AppleIntelFramebuffer",handleLinkIntegrityCheck},
				{"__ZN31AppleIntelFramebufferController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
				{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
				{"__ZN19AppleIntelPowerWell4initEP31AppleIntelFramebufferController",AppleIntelPowerWellinit, this->oAppleIntelPowerWellinit},
				{"__ZN31AppleIntelFramebufferController5startEP9IOService",AppleIntelBaseControllerstart, this->oAppleIntelBaseControllerstart},
				{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
				{"__ZN31AppleIntelFramebufferController11initCDClockEv",initCDClock, this->oinitCDClock},
				{"__ZN31AppleIntelFramebufferController28setCDClockFrequencyOnHotplugEv",setCDClockFrequencyOnHotplug, this->osetCDClockFrequencyOnHotplug},
				{"__ZN31AppleIntelFramebufferController14disableCDClockEv",disableCDClock,this->odisableCDClock},
				{"__ZN31AppleIntelFramebufferController16hwRegsNeedUpdateEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2PN16AppleIntelScaler12SCALERPARAMSE",hwRegsNeedUpdate, this->ohwRegsNeedUpdate},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route p symbols");
			
		} else
		{
			RouteRequestPlus requests[] = {
				{"__ZN21AppleIntelFramebuffer4initEP24AppleIntelBaseControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
				{"__ZN24AppleIntelBaseController23initPlatformWorkaroundsEv", initPlatformWorkarounds, this->oinitPlatformWorkarounds},
				{"__ZN24AppleIntelBaseController16getOSInformationEv", getOSInformation, this->ogetOSInformation},
				{"__ZN24AppleIntelBaseController10hwShutdownEP21AppleIntelFramebuffer",handleLinkIntegrityCheck},
				{"__ZN24AppleIntelBaseController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
				{"__ZN24AppleIntelBaseController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
				{"__ZN19AppleIntelPowerWell4initEP24AppleIntelBaseController",AppleIntelPowerWellinit, this->oAppleIntelPowerWellinit},
				{"__ZN24AppleIntelBaseController5startEP9IOService",AppleIntelBaseControllerstart, this->oAppleIntelBaseControllerstart},
				{"__ZN24AppleIntelBaseController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
				{"__ZN24AppleIntelBaseController11initCDClockEv",initCDClock, this->oinitCDClock},
				{"__ZN24AppleIntelBaseController28setCDClockFrequencyOnHotplugEv",setCDClockFrequencyOnHotplug, this->osetCDClockFrequencyOnHotplug},
				{"__ZN24AppleIntelBaseController14disableCDClockEv",disableCDClock,this->odisableCDClock},
				{"__ZN24AppleIntelBaseController16hwRegsNeedUpdateEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2PN16AppleIntelScaler12SCALERPARAMSE",hwRegsNeedUpdate, this->ohwRegsNeedUpdate},
				// V201 diagnostic: read scanout buffer right after hwSetupMemory returns to
				// detect what fills it (zeros vs wallpaper pixels). FB-only scope.
				{"__ZN24AppleIntelBaseController13hwSetupMemoryEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsb", wrapHwSetupMemory, this->ohwSetupMemory},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route d symbols");
			
		}
		
		//powerwell
		static const uint8_t f1[]= {0xe8, 0x99, 0x9f, 0xfd, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0x74, 0x78};
		static const uint8_t r1[]= {0xe8, 0x99, 0x9f, 0xfd, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0xeb, 0x78};
		
		static const uint8_t f1p[]= {0xe8, 0x66, 0xb0, 0xfe, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0x74, 0x45};
		static const uint8_t r1p[]= {0xe8, 0x66, 0xb0, 0xfe, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0xeb, 0x45};
		
		//osinfo
		/*fInfoHasLid                  : 1
		fInfoPipeCount               : 3
		fInfoPortCount               : 3
		fInfoFramebufferCount        : 3*/

		static const uint8_t f2[]= {0xc7, 0x05, 0x07, 0x81, 0x10, 0x00, 0x01, 0x03, 0x09, 0x03, 0xb8, 0x00, 0x00, 0x00, 0x04};
		static const uint8_t r2[]= {0xc7, 0x05, 0x07, 0x81, 0x10, 0x00, 0x01, 0x04, 0x03, 0x02, 0xb8, 0x00, 0x00, 0x00, 0x04};

		static const uint8_t f2p[]= {0xc7, 0x05, 0x57, 0xe5, 0x0b, 0x00, 0x01, 0x03, 0x09, 0x03};
		static const uint8_t r2p[]= {0xc7, 0x05, 0x57, 0xe5, 0x0b, 0x00, 0x01, 0x04, 0x03, 0x02};
		
		static const uint8_t f2b[]= {0x49, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x35, 0x17, 0x81, 0x10, 0x00, 0xb8, 0x08, 0x00, 0x00, 0x00};
		static const uint8_t r2b[]= {0x49, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x35, 0x17, 0x81, 0x10, 0x00, 0xb8, 0x08, 0x00, 0x00, 0x00};
		
		static const uint8_t f2c[]= {0x48, 0x89, 0x1d, 0xc0, 0x81, 0x10, 0x00, 0x4c, 0x89, 0x35, 0xc1, 0x81, 0x10, 0x00, 0x48, 0xb8, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
		static const uint8_t r2c[]= {0x48, 0x89, 0x1d, 0xc0, 0x81, 0x10, 0x00, 0x4c, 0x89, 0x35, 0xc1, 0x81, 0x10, 0x00, 0x48, 0xb8, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
		
		//mem
		static const uint8_t f2d[]= {0x0f, 0x94, 0xc0, 0xb9, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x80, 0x00};
		static const uint8_t r2d[]= {0x0f, 0x94, 0xc0, 0xb9, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x40, 0x00};
		
		//mem
		static const uint8_t f2dp[]= {0xb8, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x80, 0x00, 0x0f, 0x44, 0xd0, 0x0f, 0x94, 0xc1, 0x48, 0x01, 0x0d, 0xa2, 0xd0, 0x09, 0x00};
		static const uint8_t r2dp[]= {0xb8, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x40, 0x00, 0x0f, 0x44, 0xd0, 0x0f, 0x94, 0xc1, 0x48, 0x01, 0x0d, 0xa2, 0xd0, 0x09, 0x00};
		
		//cdclock
		static const uint8_t f2e[]= {0x48, 0xc7, 0x83, 0x60, 0x43, 0x00, 0x00, 0x00, 0x2d, 0x31, 0x01, 0x48, 0xc7, 0x83, 0x68, 0x43, 0x00, 0x00, 0x00, 0x54, 0xea, 0x2a, 0xc6, 0x83, 0xb4, 0x45, 0x00, 0x00, 0x00};
		static const uint8_t r2e[]= {0x48, 0xc7, 0x83, 0x60, 0x4a, 0x00, 0x00, 0x00, 0xa3, 0x02, 0x00, 0x48, 0xc7, 0x83, 0x68, 0x4a, 0x00, 0x00, 0x00, 0xf6, 0x09, 0x00, 0xc6, 0x83, 0xb4, 0x45, 0x00, 0x00, 0x00};
		


		//conn
		static const uint8_t f3[]= {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x03, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x04, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x05, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x06, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x07, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x08, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00};
		
		static const uint8_t r3[]= {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
		
		
		//lcd power reg
		static const uint8_t f4[]= {0x00, 0x72, 0x0c, 0x00};
		static const uint8_t r4[]= {0x00, 0x12, 0x06, 0x00};
		
		static const uint8_t f4a[]= {0x04, 0x72, 0x0c, 0x00};
		static const uint8_t r4a[]= {0x04, 0x12, 0x06, 0x00};
		
		static const uint8_t f4b[]= {0x08, 0x72, 0x0c, 0x00};
		static const uint8_t r4b[]= {0x08, 0x12, 0x06, 0x00};
		
		static const uint8_t f4c[]= {0x0c, 0x72, 0x0c, 0x00};
		static const uint8_t r4c[]= {0x0c, 0x12, 0x06, 0x00};
		
		
		//jalavoui
		static const uint8_t f6a[]= { 0xbe, 0x04, 0x00, 0x00, 0x00, 0x48, 0x89, 0xda, 0x31, 0xc9, 0xe8, 0x8c, 0xac, 0x04, 0x00};
		static const uint8_t r6a[]= { 0xbe, 0x04, 0x00, 0x00, 0x00, 0x48, 0x89, 0xda, 0x31, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		//ReadRegister64
		static const uint8_t f7[]= {0x83, 0xc0, 0xfc, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0xca, 0xf5, 0x0c, 0x00};
		static const uint8_t r7[]= {0x83, 0xc0, 0xf8, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0xca, 0xf5, 0x0c, 0x00};
		
		static const uint8_t f7p[]= {0x83, 0xc0, 0xfc, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0x84, 0x40, 0x08, 0x00};
		static const uint8_t r7p[]= {0x83, 0xc0, 0xf8, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0x84, 0x40, 0x08, 0x00};

		
		//hwreg
		static const uint8_t f10[]= {0xe8, 0xaf, 0xe2, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x5b};
		static const uint8_t r10[]= {0xe8, 0xaf, 0xe2, 0xff, 0xff, 0x84, 0xc0, 0xeb, 0x5b};
		
		static const uint8_t f10p[]= {0xe8, 0x9e, 0xf3, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x3d};
		static const uint8_t r10p[]= {0xe8, 0x9e, 0xf3, 0xff, 0xff, 0x84, 0xc0, 0xeb, 0x3d};
		
		//probeportmode
		static const uint8_t f13b[]= {0xff, 0x90, 0x90, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};
		static const uint8_t r13b[]= {0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};

		static const uint8_t f13[]= {0xff, 0x91, 0x90, 0x01, 0x00, 0x00, 0x83, 0xf8, 0x02, 0x0f, 0x84, 0xec, 0x00, 0x00, 0x00};
		static const uint8_t r13[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		static const uint8_t f13p[]= {0xff, 0x91, 0x78, 0x01, 0x00, 0x00, 0x83, 0xf8, 0x02, 0x74, 0x64};
		static const uint8_t r13p[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		static const uint8_t f13pb[]= {0xff, 0x90, 0x78, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};
		static const uint8_t r13pb[]= {0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};

		
		//getPathByPipe logs
		static const uint8_t f15[]= {0x74, 0x36, 0x48, 0xff, 0x05, 0x7e, 0x51, 0x08, 0x00, 0x44, 0x89, 0x3c, 0x24, 0x48, 0x8d, 0x15, 0x4d, 0x88, 0x03, 0x00, 0x4c, 0x8d, 0x05, 0x28, 0x8a, 0x03, 0x00};
		static const uint8_t r15[]= {0xeb, 0x36, 0x48, 0xff, 0x05, 0x7e, 0x51, 0x08, 0x00, 0x44, 0x89, 0x3c, 0x24, 0x48, 0x8d, 0x15, 0x4d, 0x88, 0x03, 0x00, 0x4c, 0x8d, 0x05, 0x28, 0x8a, 0x03, 0x00};
		
		//getBuiltInPor
		static const uint8_t f16[]= {0x48, 0x89, 0x05, 0xfc, 0x39, 0x12, 0x00, 0x48, 0x8b, 0x83, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x75, 0x0d};
		static const uint8_t r16[]= {0x48, 0x89, 0x05, 0xfc, 0x39, 0x12, 0x00, 0x48, 0x8b, 0x83, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x90, 0x90};
		
		static const uint8_t f16p[]= {0x48, 0x8b, 0x80, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x75, 0x0a};
		static const uint8_t r16p[]= {0x48, 0x8b, 0x80, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x90, 0x90};

		//getHPDState
		static const uint8_t f19[]= {0xbe, 0x70, 0x44, 0x04, 0x00};
		static const uint8_t r19[]= {0xbe, 0xa0, 0x38, 0x16, 0x00};
		
		//savenvram
		static const uint8_t f20[]= {0xff, 0x90, 0xf8, 0x09, 0x00, 0x00, 0x41, 0x89, 0xc6, 0x48, 0x85, 0xdb, 0x74, 0x17};
		static const uint8_t r20[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x85, 0xdb, 0x74, 0x17};
		
		static const uint8_t f20p[]= {0xff, 0x90, 0xf8, 0x09, 0x00, 0x00, 0x41, 0x89, 0xc6, 0x48, 0x85, 0xdb, 0x74, 0x17};
		static const uint8_t r20p[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x85, 0xdb, 0x74, 0x17};
		
		//SafeForceWake
		static const uint8_t f21[]= {0x0f, 0x84, 0x96, 0x00, 0x00, 0x00, 0x48, 0xff, 0x05, 0xbc, 0x05, 0x0f, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7};
		static const uint8_t r21[]= {0x48, 0xe9, 0x96, 0x00, 0x00, 0x00, 0x48, 0xff, 0x05, 0xbc, 0x05, 0x0f, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7};
		
		static const uint8_t f21p[]= {0x74, 0x3c, 0x48, 0xff, 0x05, 0x7a, 0x73, 0x09, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7, 0xe8, 0x97, 0x80, 0x01, 0x00};
		static const uint8_t r21p[]= {0xeb, 0x3c, 0x48, 0xff, 0x05, 0x7a, 0x73, 0x09, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7, 0xe8, 0x97, 0x80, 0x01, 0x00};
        
        //pixel hwcrt
        static const uint8_t f22[]= {0x48, 0x69, 0xc2, 0x50, 0xc3, 0x00, 0x00, 0x49, 0x89, 0x47, 0x28, 0xbf, 0x08, 0x00, 0x00, 0x00, 0xbe, 0x06, 0x00, 0x00, 0x00, 0xe8, 0x9e, 0x81, 0x01, 0x00, 0x84, 0xc0, 0x74, 0x3a};
        
        static const uint8_t r22[]= {0x48, 0xc7, 0xc0, 0xc0, 0x40, 0xd0, 0x2e, 0x49, 0x89, 0x47, 0x28, 0xbf, 0x08, 0x00, 0x00, 0x00, 0xbe, 0x06, 0x00, 0x00, 0x00, 0xe8, 0x9e, 0x81, 0x01, 0x00, 0x84, 0xc0, 0x90, 0x90};
		

		// force eDP panel detection regardless of pipe number (from NootedBlue)
		// NOPs two JE and one JNE that would skip eDP init when pipe != 1.
		// Required because our pinfo sets eDP on pipe=0, not pipe=1.
		static const uint8_t f6nb[]= {0x74, 0x2a, 0x83, 0xf8, 0x01, 0x74, 0x43, 0x85, 0xc0, 0x75, 0x60};
		static const uint8_t r6nb[]= {0x90, 0x90, 0x83, 0xf8, 0x01, 0x90, 0x90, 0x85, 0xc0, 0x90, 0x90};

		// fix register addresses if pipe=0 (jne→jmp to always use pipe-0 register offsets)
		static const uint8_t f24bp[]= {0x83, 0x78, 0x08, 0x00, 0x75, 0x0c};
		static const uint8_t r24bp[]= {0x83, 0x78, 0x08, 0x00, 0xeb, 0x0c};
		static const uint8_t f24cp[]= {0x00, 0x4c, 0x89, 0xea, 0x75, 0x12};
		static const uint8_t r24cp[]= {0x00, 0x4c, 0x89, 0xea, 0xeb, 0x12};
		static const uint8_t f24dp[]= {0x83, 0x78, 0x08, 0x00, 0x75, 0x0d};
		static const uint8_t r24dp[]= {0x83, 0x78, 0x08, 0x00, 0xeb, 0x0d};
		static const uint8_t f24b[]= {0x83, 0x78, 0x08, 0x00, 0x75, 0x0c};
		static const uint8_t r24b[]= {0x83, 0x78, 0x08, 0x00, 0xeb, 0x0c};
		static const uint8_t f24c[]= {0x48, 0x8b, 0x55, 0xd0, 0x75, 0x13};
		static const uint8_t r24c[]= {0x48, 0x8b, 0x55, 0xd0, 0xeb, 0x13};
		static const uint8_t f24d[]= {0x83, 0x78, 0x08, 0x00, 0x75, 0x0d};
		static const uint8_t r24d[]= {0x83, 0x78, 0x08, 0x00, 0xeb, 0x0d};
		// link training speed constant fix
		static const uint8_t f25[]= {0x77, 0x77, 0x00, 0x00};
		static const uint8_t r25[]= {0x33, 0x00, 0x00, 0x00};

		// Path E (TCON ID): guarded by -ngreentglwithgfx boot-arg (requires GFX kext present).
		// Rewrites CamelliaTcon2/BanksiaTcon DPCD ID comparisons so they match this panel's
		// DPCD bytes [14 1e c4 c1] → 0xc1c41e14. Also sets cameliav=2 in getOSInformation.
		// DO NOT enable on FB-only boot (no GFX): calls into AGDC services → hang.
		static const uint8_t f_tcon_camellia[]= {0x3d, 0x11, 0x0a, 0x84, 0x41};
		static const uint8_t r_tcon_camellia[]= {0x3d, 0x14, 0x1e, 0xc4, 0xc1};
		static const uint8_t f_tcon_banksia[] = {0x3d, 0x12, 0x14, 0xc4, 0x41};
		static const uint8_t r_tcon_banksia[] = {0x3d, 0x14, 0x1e, 0xc4, 0xc1};
		const bool enableTcon = checkKernelArgument("-ngreentglwithgfx");

		if (isprod){
			LookupPatchPlus const patchesp[] = {// tgl production kext

				// f1p (powerwell JZ→JMP) commented out — not present in NootedBlue's
				// working TGL FBT prod patch list. Was an extra NootedGreen accumulated.
				//{activeKext, f1p, r1p, arrsize(f1p),	1},
				{activeKext, f2p, r2p, arrsize(f2p),	1},
				{activeKext, f2dp, r2dp, arrsize(f2dp),	1},
				// f3 (connector data table rewrite) commented out — not in NootedBlue.
				//{activeKext, f3, r3, arrsize(f3),	1},
				// f4 family ("lcd power reg" 0x72→0x12, 4 variants ~27 binary mods) commented out
				// — NOT present in NootedBlue's working TGL FBT prod patch list. These rewrite
				// what appears to be LCD power register access patterns; on Display 13 hardware
				// the original Apple code paths may be correct without the rewrite.
				//{activeKext, f4, r4, arrsize(f4),	11},
				//{activeKext, f4a, r4a, arrsize(f4a),	11},
				//{activeKext, f4b, r4b, arrsize(f4b),	2},
				//{activeKext, f4c, r4c, arrsize(f4c),	2},
				{activeKext, f7p, r7p, arrsize(f7p),	1},
				// f10p ("hwreg" CALL+JZ→JMP bypass) commented out — NOT in NootedBlue.
				//{activeKext, f10p, r10p, arrsize(f10p),	1},
				// Keep native probe-port mode flow for compatibility.
				//{activeKext, f13p, r13p, arrsize(f13p),	1},
				//{activeKext, f13pb, r13pb, arrsize(f13pb),	1},
				//{activeKext, f16p, r16p, arrsize(f16p),	1},
				{activeKext, f6nb, r6nb, arrsize(f6nb),	1},
				{activeKext, f13p, r13p, arrsize(f13p),	1},
				{activeKext, f13pb, r13pb, arrsize(f13pb),	1},
				{activeKext, f19, r19, arrsize(f19),	1},
				{activeKext, f20p, r20p, arrsize(f20p),	1},
				{activeKext, f24bp, r24bp, arrsize(f24bp),	14},
				{activeKext, f24cp, r24cp, arrsize(f24cp),	1},
				{activeKext, f24dp, r24dp, arrsize(f24dp),	4},
				{activeKext, f25,  r25,  arrsize(f25),	6},
			};

			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesp , address, size), "ngreen", "kextG11FBT Failed to apply production patches!");
			applyFixBootMode(patcher, activeKext, address, size);
			if (enableTcon) {
				LookupPatchPlus const tconPatches[] = {
					{activeKext, f_tcon_camellia, r_tcon_camellia, arrsize(f_tcon_camellia), 1},
					{activeKext, f_tcon_banksia,  r_tcon_banksia,  arrsize(f_tcon_banksia),  1},
				};
				LookupPatchPlus::applyAll(patcher, tconPatches, address, size);
				SYSLOG("ngreen", "Path E: TCON ID patches applied (prod)");
			}
		}
		else {
			LookupPatchPlus const patches[] = {// tgl debug kext
				// f1 (powerwell JZ→JMP) commented out — not present in NootedBlue's
				// working TGL FBT debug patch list. Was an extra NootedGreen accumulated.
				//{activeKext, f1, r1, arrsize(f1),	1},
				// f2/f2d: osinfo pipe/port/fb counts now set via getOSInformation hook — no binary patch needed.
				// f3 (connector data table rewrite) commented out — not in NootedBlue.
				//{activeKext, f3, r3, arrsize(f3),	1},
				// f4 family ("lcd power reg" 0x72→0x12, 4 variants ~27 binary mods) commented out
				// — NOT in NootedBlue's working TGL FBT debug patch list. Suspected contributor
				// to the fragmentation/repetition symptom: rewriting LCD power register access
				// patterns may corrupt panel-side state on Display 13 (ADL-P) hardware.
				//{activeKext, f4, r4, arrsize(f4),	12},
				//{activeKext, f4a, r4a, arrsize(f4a),	11},
				//{activeKext, f4b, r4b, arrsize(f4b),	2},
				//{activeKext, f4c, r4c, arrsize(f4c),	2},
		//		{activeKext, f6a, r6a, arrsize(f6a),	1},
		//		{activeKext, f7, r7, arrsize(f7),	1},
				// f10 ("hwreg" CALL+JZ→JMP bypass) commented out — NOT in NootedBlue.
				{activeKext, f10, r10, arrsize(f10),	1},
				// f13: mandatory. Without this port-probe bypass the spoofed TGL path freezes during boot.
				{activeKext, f13, r13, arrsize(f13),	1},
				// f13b: mandatory. Without this bypass AppleIntelPort::probePortStateEv hits
				// a pure-virtual call and panics in WindowServer during enableController.
		//		{activeKext, f13b, r13b, arrsize(f13b),	1},
				// f15: suppress getPathByPipe log flood at IGLogLevel=8.
				// On platform 0x9a490000 all paths are on pipe 0; every scan cycle logs
				// "pipe = 0" many times per second, flooding the log unreadably.
				// The je→jmp makes the branch unconditionally skip the IGFB log emit.
				// Purely cosmetic: no behavioral change, no display-pipe impact.
				{activeKext, f15, r15, arrsize(f15),	1},
				//{activeKext, f16, r16, arrsize(f16),	1},
				{activeKext, f19, r19, arrsize(f19),	1},
		//		{activeKext, f20, r20, arrsize(f20),	1},
				//{activeKext, f21, r21, arrsize(f21),	1},
				//{activeKext, f22, r22, arrsize(f22),    1},
		//		{activeKext, f6nb, r6nb, arrsize(f6nb),	1},
		//		{activeKext, f19, r19, arrsize(f19),	1},
		//		{activeKext, f20, r20, arrsize(f20),	1},
				{activeKext, f24b, r24b, arrsize(f24b),	11},
		//		{activeKext, f24c, r24c, arrsize(f24c),	1},
		//		{activeKext, f24d, r24d, arrsize(f24d),	6},
		//		{activeKext, f25,  r25,  arrsize(f25),	6},
				};

			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patches , address, size), "ngreen", "kextG11FBT Failed to apply dbg patches!");
			applyFixBootMode(patcher, activeKext, address, size);
			if (enableTcon) {
				LookupPatchPlus const tconPatches[] = {
					{activeKext, f_tcon_camellia, r_tcon_camellia, arrsize(f_tcon_camellia), 1},
					{activeKext, f_tcon_banksia,  r_tcon_banksia,  arrsize(f_tcon_banksia),  1},
				};
				LookupPatchPlus::applyAll(patcher, tconPatches, address, size);
				SYSLOG("ngreen", "Path E: TCON ID patches applied (dbg)");
			}
		}

		// xe/vald (19/10): no Tahoe todo flip do WindowServer era recusado com 0xe00002c7 por
		// AppleIntelFramebuffer::ValidateTransaction (callPlatformFunction "vald", via IGAccelDisplayPipe::validateTransaction):
		//  (1) getTransactionOptions() & 0x500 e (2) getPlaneTransactionOptions(p) & 0x500 -> unsupported
		//      (bits que o IOAcceleratorFamily2 do Tahoe passa por padrão; no Big Sur tinham outro sentido);
		//  (3) o formato de pixel é lido de IOSurface+0xa4 direto da memória: no Tahoe o layout do IOSurface mudou e
		//      vem 0 (o IntelIGPU-Tahoe documenta o mesmo); sem a leitura fica o padrão da própria função, 'BGRA'.
		// Padrões únicos no TGL FB 16.0 (0x3ecf1, 0x3ed1d, 0x3ed88). Não fatal: só registra se não achar.
		{
			static const uint8_t f_vopt[]  = {0xa9, 0x00, 0x05, 0x00, 0x00, 0x74, 0x17};
			static const uint8_t r_vopt[]  = {0xa9, 0x00, 0x00, 0x00, 0x00, 0x74, 0x17};
			static const uint8_t f_vpopt[] = {0xa9, 0x00, 0x05, 0x00, 0x00, 0x74, 0x1e};
			static const uint8_t r_vpopt[] = {0xa9, 0x00, 0x00, 0x00, 0x00, 0x74, 0x1e};
			static const uint8_t f_vfmt[]  = {0x8b, 0x80, 0xa4, 0x00, 0x00, 0x00, 0x89, 0x45, 0xb4};
			static const uint8_t r_vfmt[]  = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
			LookupPatchPlus const p1[] = {{activeKext, f_vopt, r_vopt, arrsize(f_vopt), 1}};
			LookupPatchPlus const p2[] = {{activeKext, f_vpopt, r_vpopt, arrsize(f_vpopt), 1}};
			LookupPatchPlus const p3[] = {{activeKext, f_vfmt, r_vfmt, arrsize(f_vfmt), 1}};
			bool a = LookupPatchPlus::applyAll(patcher, p1, address, size);
			bool b = LookupPatchPlus::applyAll(patcher, p2, address, size);
			bool c = LookupPatchPlus::applyAll(patcher, p3, address, size);
			SYSLOG("ngreen", "XE-VALD: ValidateTransaction opts=%d planeopts=%d fmtBGRA=%d", a, b, c);
		}

		return true;
		
	}else if (kextG11HW.loadIndex == index) {
		if (this->tglHWLoaded) {
			DBGLOG("ngreen", "Skipping ICL HW — TGL HW already loaded");
			return true;
		}
		auto *activeKext = &kextG11HW;
		DBGLOG("ngreen", "init AppleIntelICLGraphics!");
		injectAcceleratorPersonality(false);
		NGreen::callback->setRMMIOIfNecessary();
		const bool wegCoexist = isWEGCoexistMode();

		{
			// loadGuCBinary: always route — WEG's firmware path is Mojave-gated and dead on Sonoma.
			// Without this hook, no GuC binary loads at all in coexist mode → ring dead.
			RouteRequestPlus firmwareRoute[] = {
				{"__ZN13IGHardwareGuC13loadGuCBinaryEv", loadGuCBinary, this->oloadGuCBinary},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, firmwareRoute, address, size), "ngreen", "Failed to route loadGuCBinary (ICL)");
		}

		if (!wegCoexist) {
			RouteRequestPlus requests[] = {
				// PAVP/DRM: intercept session command callback (ICL hardware path, shared hook with TGL)
				{"__ZN16IntelAccelerator19PAVPCommandCallbackE22PAVPSessionCommandID_tjPjb", wrapPavpSessionCallback, this->orgPavpSessionCallback},
				// initHardwareCaps NOT routed: NBlue's wrapper reads TGL offset 0x1120 for SKU,
				// but ICL stores SKU at 0x1150. Let the original ICL code run — SKU gates are patched.
				// IGScheduler5resume NOT routed: kIGHwCsDesc is only resolved for kextG11HWT.
				// With -disablegfxfirmware, Host Preemptive scheduler is selected (not IGScheduler5).
				//last	 {"__ZN12IGScheduler56resumeEv", IGScheduler5resume, this->oIGScheduler5resume},
				// resetGraphicsEngine NOT routed: NBlue wrapper applies TGL GT workarounds which
				// target TGL MMIO offsets. Hardware is RPL-P (adlp/raptorlake) — using TGL workarounds
				// on RPL MMIO could corrupt the command streamer. Let the ICL original run unmodified.
			//symbol absent in kext	{"__ZN20IGHardwareRingBuffer19resetGraphicsEngineEP17IGHardwareContext", resetGraphicsEngine, this->oresetGraphicsEngine},
				//last	 {"__ZN13IGHardwareGuC18checkWOPCMSettingsEmR14IOVirtualRange", checkWOPCMSettings, this->ocheckWOPCMSettings},
				//last	 {"__ZN11IGScheduler15canLoadFirmwareEP16IntelAccelerator", canLoadFirmware, this->ocanLoadFirmware},
				 // V36: Hook readAndClearInterrupts to initialize Gen11 multi-engine GT interrupts.
				 // Same implementation as TGL path — Gen11 IRQ registers are identical for ICL/TGL.
				 // V37: DISABLED — caused boot hang on TGL path; disabling ICL too for safety.
				 // {"__ZN16IntelAccelerator23readAndClearInterruptsEPv", readAndClearInterrupts, this->oreadAndClearInterrupts},
			};

			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route dp symbols");

			RouteRequestPlus gpuInfoRoute[] = {
				// getGPUInfo: override topology at ICL object offsets (different from TGL offsets)
				{"__ZN16IntelAccelerator10getGPUInfoEv", getGPUInfoICL, this->ogetGPUInfoICL},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, gpuInfoRoute, address, size), "ngreen", "Failed to route getGPUInfoICL");
		}
		
		// SKU gate 1+2: NOP JNZ/JA + XOR eax,eax (Sonoma AppleIntelICLGraphics, verified in KC)
		static const uint8_t fSKUGates12[] = {
			0x83, 0xF9, 0x01,
			0x0F, 0x85, 0x0B, 0x01, 0x00, 0x00,
			0xFF, 0xC8,
			0x83, 0xF8, 0x07,
			0x0F, 0x87, 0x00, 0x01, 0x00, 0x00,
			0x48, 0x8D, 0x0D, 0x77, 0x02, 0x00, 0x00, 0x48
		};
		static const uint8_t rSKUGates12[] = {
			0x83, 0xF9, 0x01,
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
			0x31, 0xC0,
			0x83, 0xF8, 0x07,
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
			0x48, 0x8D, 0x0D, 0x77, 0x02, 0x00, 0x00, 0x48
		};

		// SKU gate 3: NOP JNZ (Sonoma AppleIntelICLGraphics, verified in KC)
		static const uint8_t fSKUGate3[] = {
			0x83, 0xF8, 0x08, 0x0F, 0x85, 0xC2, 0x00, 0x00, 0x00, 0xC7
		};
		static const uint8_t rSKUGate3[] = {
			0x83, 0xF8, 0x08, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC7
		};

		// SKU bypass: TEST rax,rax; JZ->JMP (Sonoma, f2Long verified in KC at 0x14152bcd)
		static const uint8_t fSkuBypassLong[] = {
			0x48, 0x85, 0xC0, 0x74, 0x72, 0x48, 0x0F, 0xBC, 0xC0, 0x48, 0xFF, 0xC0, 0x48,
			0x8D, 0x15, 0x00, 0xCD, 0x0F, 0x00, 0x48, 0x8D, 0x48, 0xFF, 0x48, 0xF7, 0xC1,
			0xFD, 0xFF, 0xFF, 0xFF, 0x74, 0x27, 0x48, 0x6B, 0xC9, 0x79
		};
		static const uint8_t rSkuBypassLong[] = {
			0x48, 0x85, 0xC0, 0xEB, 0x72, 0x48, 0x0F, 0xBC, 0xC0, 0x48, 0xFF, 0xC0, 0x48,
			0x8D, 0x15, 0x00, 0xCD, 0x0F, 0x00, 0x48, 0x8D, 0x48, 0xFF, 0x48, 0xF7, 0xC1,
			0xFD, 0xFF, 0xFF, 0xFF, 0x74, 0x27, 0x48, 0x6B, 0xC9, 0x79
		};

		LookupPatchPlus const patches[] = {
			{&kextG11HW, fSKUGates12,    rSKUGates12,    arrsize(fSKUGates12),    1},
			{&kextG11HW, fSKUGate3,      rSKUGate3,      arrsize(fSKUGate3),      1},
			{&kextG11HW, fSkuBypassLong, rSkuBypassLong, arrsize(fSkuBypassLong), 1},
		};
		
		/*auto catalina = getKernelVersion() == KernelVersion::Catalina;
		if (catalina)
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesc , address, size), "ngreen", "cata Failed to apply patches!");
		else*/
		for (size_t i = 0; i < sizeof(patches)/sizeof(patches[0]); ++i) {
			//IOSleep(delay);
			PANIC_COND(!patches[i].apply(patcher, address, size), "ngreen", "kextG11HW Failed to apply patch %zu", i);
		}
		DBGLOG("ngreen", "Loaded AppleIntelICLGraphics!");

		return true;

	} else if (kextG11HWT.loadIndex == index || kextG11HWTA.loadIndex == index) {
		this->tglHWLoaded = true;
		auto *activeKext = (kextG11HWTA.loadIndex == index) ? &kextG11HWTA : &kextG11HWT;
		SYSLOG("ngreen", "init AppleIntelTGLGraphics (HW accelerator)");
		injectAcceleratorPersonality(true);
		NGreen::callback->setRMMIOIfNecessary();
		SYSLOG("ngreen", "V165: setRMMIO done, starting symbol resolve");

		// V144: Resolve the Blit3D context params struct and the ExtendedContext initWithOptions.
		// Blit3DExtendedCtxParams is a static data symbol — address passed as param_2 to initWithOptions.
		// Without these, IGHardwareExtendedContextinitWithOptions is called with param_2=0
		// causing it to return 0 (ok=0) and leave ctx+0xb8=NULL → submitBlit crash at +0x28e.
		{
			SolveRequestPlus solveRequests[] = {
				{"__ZN23IGHardwareBlit3DContext17ExtendedCtxParamsE", this->Blit3DExtendedCtxParams},
			};
			SYSLOG_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "ngreen",
			            "V144: Failed to resolve Blit3DExtendedCtxParams");
		}

		const bool wegCoexist = isWEGCoexistMode();
		const bool forceFullMTL = shouldForceFullMetalPath();

		RouteRequestPlus requests[] = {
			
			 {"__ZN16IntelAccelerator20_PAVPCommandCallbackEP8OSObject22PAVPSessionCommandID_tjPj", wrapPavpSessionCallback, this->orgPavpSessionCallback},
			
			// V163: Hook startGraphicsEngine to clear PERCTX_PREEMPT_CTRL (FF_SLICE_CS_CHICKEN1 bit 14)
			// immediately after the TGL kext enables it. The TGL kext writes 0x40004000 to reg 0x20E0
			// (mask=bit14, value=bit14=1). On RPL this causes the EU thread dispatcher to freeze on
			// first context switch because hardware snapshots the register into the context image DMA
			// buffer — clearing it here, before any execlist context is created, prevents the bad
			// value from ever reaching the DMA buffer.
		 	{"__ZN16IntelAccelerator19startGraphicsEngineEv", startGraphicsEngine, this->ostartGraphicsEngine},
			{"__ZN16IntelAccelerator18stopGraphicsEngineEv",  stopGraphicsEngine,  this->ostopGraphicsEngine},

			// V212: Hook IGScheduler{4,5}::isGpuIdle — the bool watchdog query called post-startup.
			// startGraphicsEngine SUCCEEDS (returns non-zero = success path in decomp). But the GPU
			// watchdog polls isGpuIdle() after init; INSTDONE bit0 stuck at 0 makes it return false
			// → watchdog declares GPU hung → resets → startGraphicsEngine retry loop forever.
			// Fix: when INSTDONE == 0xfffffffe (RPL-P idle with stuck bit0), return true.
			{"__ZNK12IGScheduler59isGpuIdleEv", wrapIGScheduler5IsGpuIdle, this->oIGScheduler5IsGpuIdle},
			{"__ZNK12IGScheduler49isGpuIdleEv", wrapIGScheduler4IsGpuIdle, this->oIGScheduler4IsGpuIdle},

			// V164: Hook populateResetRegisterList which reads live MMIO values into the per-context
			// replay list (a batch of MI_LRI commands executed before every context switch).
			// startGraphicsEngine enables PERCTX_PREEMPT_CTRL then calls populateResetRegisterList,
			// which snapshots the live 0x4000 into the list. Hardware then replays 0x4000 back to
			// 0x20E0 on every context switch, overriding any post-hoc MMIO clear.
			// Fix: clear bit 14 BEFORE calling original so the snapshot captures 0x0000.
			{"__ZN16IntelAccelerator25populateResetRegisterListEv", populateResetRegisterList, this->opopulateResetRegisterList},


			 // V132: Hook task producers so submitBlit never sees a null IGAccelTask on spoofed RPL.
			 {"__ZN16IntelAccelerator17createUserGPUTaskEv", createUserGPUTask, this->ocreateUserGPUTask},
			 {"__ZN11IGAccelTask11withOptionsEP16IntelAccelerator", igAccelTaskWithOptions, this->oigAccelTaskWithOptions},

			 // V36: Hook readAndClearInterrupts to initialize Gen11 multi-engine GT interrupts.
			 // Without this, RCS/BCS user interrupts and context-switch notifications may not
			 // be properly enabled, preventing IOAccelF2 from seeing stamp completions.
			 // V37: DISABLED — caused boot hang (symbol may not exist in TGL kext, or
			 // interrupt reprogramming too early causes deadlock/panic).
			 // {"__ZN16IntelAccelerator23readAndClearInterruptsEPv", readAndClearInterrupts, this->oreadAndClearInterrupts},

			// V119: Route getBlit3DContext so our guard checks context+0xb8 before storing.
			 // Without this, Apple's original runs, stores a context with +0xb8=NULL to
			 // that+0x298 (when init path leaves +0xb8 unset), and submitBlit+0x28e crashes.
			 {"__ZN11IGAccelTask16getBlit3DContextEb", getBlit3DContext, this->ogetBlit3DContext},
		
			 // V112: Resolve the NootedBlue Blit3D helpers so the fallback path can
			 // initialize scratch/context state instead of leaving the blit object empty.
			 {"__Z31blit3d_initialize_scratch_spaceP16IGAccelSysMemory", blit3d_initialize_scratch_space, this->oblit3d_initialize_scratch_space},
			 {"__Z15blit3d_init_ctxP23IGHardwareBlit3DContext", blit3d_init_ctx, this->oblit3d_init_ctx},
			 // V69: ENABLED — original crashes at +0x4c memcpy'ing 68 bytes to unmapped GPU buffer
			 // at VA 0xfffffff034136000 (page 0xD of context buffer). Our replacement skips the
			 // dangerous memcpy and logs diagnostic info about the mapping.
			 {"__ZN23IGHardwareBlit3DContext10initializeEv", IGHardwareBlit3DContextinitialize, this->oIGHardwareBlit3DContextinitialize},
			 // 20/10 (XE-RESFREE): pânico kfree no Wallpaper em IGAccelResource::free; ver a função.
			 {"__ZN15IGAccelResource4freeEv", IGAccelResourcefree, this->oIGAccelResourcefree},
			 // 20/10 (XE-GTFREQ): sem GuC o pedido de frequência da Apple é sempre 0 -> GPU presa em 100 MHz.
			 {"__ZN26IGHardwareCommandStreamer518setGTFrequencyMMIOEj", wrapSetGTFrequencyMMIO, this->oSetGTFrequencyMMIO},
			 // 21/10 (XE-IDLEBOUND): IGScheduler5::waitForGpuIdle (stopGraphicsEngine no desligamento) espera até 25000 x 100 us.
			 {"__ZN12IGScheduler514waitForGpuIdleEv", wrapWaitForGpuIdle, this->oWaitForGpuIdle},
			 // V143: Register operator new for IGHardwareBlit3DContext so getBlit3DContext can
			 // actually allocate the context object. Without this, oIGHardwareBlit3DContextoperatornew
			 // is always null → alloc always returns nullptr → ctx=0 forever → NULL-task submitBlit
			 // flood → BCS ring WAIT_ON_SCANLINE stall.
			 {"__ZN23IGHardwareBlit3DContextnwEm", IGHardwareBlit3DContextoperatornew, this->oIGHardwareBlit3DContextoperatornew},
			 // V508: Hook base-class IGHardwareContext::withOptions to inspect ctx+0xb8 (FIFO
			 // channel / ring buffer allocation) and dump LRCA page1 via wbinvd+aperture right
			 // after initWithOptions writes it — determines if RING_START=0 is real or LLC artifact.
			 {"__ZN17IGHardwareContext11withOptionsEP11IGAccelTaskRK23IGHardwareContextParamsh", IGHardwareContextwithOptions, this->oIGHardwareContextwithOptions},
			 // V509: Hook base-class IGHardwareContext::initWithOptions — repair LRCA page1 when
			 // restoreFromSafeImage skips g_cInitGfxRingContextRCS memcpy (leaves DW1=0x00ffffff).
			 // Writes Gen12 ring context MI_LRI block so ExecList context-restore works on RPL.
			 {"__ZN17IGHardwareContext15initWithOptionsEP11IGAccelTaskRK23IGHardwareContextParamsh", IGHardwareContextinitWithOptions, this->oIGHardwareContextinitWithOptions},

			 // V144: Hook IGHardwareExtendedContext::initWithOptions so it actually runs with
			 // the resolved Blit3DExtendedCtxParams. Without this, oIGHardwareExtendedContextinitWithOptions
			 // is null → init returns 0 → ctx+0xb8 stays NULL → submitBlit+0x28e crash.
			 {"__ZN25IGHardwareExtendedContext15initWithOptionsEP11IGAccelTaskRK31IGHardwareExtendedContextParams", IGHardwareExtendedContextinitWithOptions, this->oIGHardwareExtendedContextinitWithOptions},
			 {"__ZNK14IGMappedBuffer9getMemoryEv", IGMappedBuffergetMemory, this->oIGMappedBuffergetMemory},
			 
			 // V120: Hook submitBlit to protect against nullptr context.
			 // Even with getBlit3DContext hooked and returning null, Apple's blitCopy
			 // doesn't check the return value and passes it straight to submitBlit.
			 // submitBlit+0x28e unconditionally dereferences param_5+0xb8 (where param_5
			 // is the Blit3D or Blit2D context) -> page fault. 
			 {"__ZN16IntelAccelerator10submitBlitEP15blit3d_params_tRK8IGVectorI11rect_pair_t25IGIOMallocAllocatorPolicyEP11IGAccelTaskb", submitBlit, this->osubmitBlit},

			 // V121: Hook IGAccelSegmentResourceList::initBlitUsage which also crashes at +0x17
			 // dereferencing [member+0xb8] when the Blit3D context was never populated
			 // (same root cause: incomplete Blit context init leaves context+0xb8 null).
			 // The crash arrives via: coalesceSegment -> prepare -> initBlitUsage.
			 {"__ZN26IGAccelSegmentResourceList13initBlitUsageEv", initBlitUsage, this->oinitBlitUsage},

			 // V122: markBlitUsage+0x17 has the identical [ctx+0xb8] dereference and is called
			 // from prepare+0x2a immediately after initBlitUsage. Must be guarded too.
			 {"__ZN26IGAccelSegmentResourceList13markBlitUsageEv", markBlitUsage, this->omarkBlitUsage},

			 // V123b: Register the prepare hook — the implementation (returning 0 early) existed
			 // but was never routed, so Apple's original kept calling initBlitUsage/markBlitUsage.
			 {"__ZN26IGAccelSegmentResourceList7prepareEv", IGAccelSegmentResourceListprepare, this->oIGAccelSegmentResourceListprepare},

			 // V124: IGAccelCommandQueue::beginCoalescedSegment+0x2f dereferences [member+0xb8]
			 // (same null as all previous crashes — context never initialized on RPL).
			 // Called from processAndSubmitCoalescedSegments even when prepare returns 0.
			 // NOTE: processCommandBuffer (the caller) calls [off_C8178+0xA30] which IS the
			 // actual render submission — hooking that would break display. So we guard here
			 // at the lowest safe level instead.
			 {"__ZN19IGAccelCommandQueue21beginCoalescedSegmentEv", beginCoalescedSegment, this->obeginCoalescedSegment},

			 // V126b: barrierSubmission itself must keep original side effects/contract.
			 // We route it only to scope temporary getter fallback (no broad bypass).
			 {"__Z17barrierSubmissionR19IGAccelCommandQueueR16IntelAcceleratorR24IGAccelCommandDescriptorR12IOAccelEventtPKt", barrierSubmission, this->obarrierSubmission},
			 
			 // V117: Hook getBlit2DContext for null-guarding only.
			 // submitBlit expects a real Blit2D context layout and unconditionally reads
			 // [ctx+0xb8], so we must never substitute depth/color/3D context objects here.
			 {"__ZN11IGAccelTask16getBlit2DContextEb", getBlit2DContext, this->ogetBlit2DContext},

			 // Resolve-context getters are used by barrierSubmission and are generally safe.
			 {"__ZN11IGAccelTask22getDepthResolveContextEb", getDepthResolveContext, this->ogetDepthResolveContext},
			 {"__ZN11IGAccelTask22getColorResolveContextEb", getColorResolveContext, this->ogetColorResolveContext},
			 
		 };
		SYSLOG("ngreen", "V165: routing %zu HW accelerator symbols", sizeof(requests)/sizeof(requests[0]));
		PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route symbols");
		SYSLOG("ngreen", "V165: HW accelerator symbols routed OK");

		if (checkKernelArgument("-ngreenxefix")) {  // xe/flipdiag: só contadores, sem mudar comportamento
			RouteRequestPlus flipDiag[] = {
				{"__ZN18IGAccelDisplayPipe17submitTransactionEP30IOAccelDisplayPipeTransaction2", xeDpSubmit, gOrgDpSubmit},
				{"__ZN18IGAccelDisplayPipe21isTransactionCompleteEP30IOAccelDisplayPipeTransaction2", xeDpIsComplete, gOrgDpIsComplete},
				{"__ZN18IGAccelDisplayPipe25signalTransactionCompleteEP12IOAccelEvent", xeDpSignal, gOrgDpSignal},
				{"__ZN16IntelAccelerator24signalDisplayStampUpdateEij", xeDispStamp, gOrgDispStamp},
				{"__ZN12IGScheduler54pushEP17IGHardwareContextjjbb", xeS5Push, gOrgS5Push},
				{"__ZN12IGScheduler56notifyE10IGHwCsType", xeS5Notify, gOrgS5Notify},
				{"__ZN26IGHardwareCommandStreamer514submitExecListEj", xeCs5Submit, gOrgCs5Submit},
				{"__ZN12IGScheduler55pauseEv", xeS5Pause, gOrgS5Pause},
				{"__ZN12IGScheduler56resumeEv", xeS5Resume, gOrgS5Resume},
			};
			RouteRequestPlus ringDiag[] = {
				{"__ZN20IGHardwareRingBuffer12submitToRingEv", xeRbSubmit, gOrgRbSubmit},
				{"__ZN20IGHardwareRingBuffer10writeStampEjb", xeRbWriteStamp, gOrgRbWriteStamp},
				{"__ZN20IGHardwareRingBuffer23insertSemaphoreWaitPollEji", xeRbSemPoll, gOrgRbSemPoll},
				{"__ZN26IGHardwareCommandStreamer516processCSBDetailEP28SGfxContextStatusBufferEntryRj", xeCsbDetail, gOrgCsbDetail},
				{"__ZN26IGHardwareCommandStreamer522csbProcessActiveToIdleEPK28SGfxContextStatusBufferEntry", xeCsbA2I, gOrgCsbA2I},
				{"__ZN15IGAccelResource7pageoffEP12IOAccelEventbPby", xePageoff, gOrgPageoff},
				{"__ZN15IGAccelResource6pageonEP12IOAccelEventby", xePageon, gOrgPageon},
				{"__ZN26IGHardwareCommandStreamer519csbProcessPreemptedEPK28SGfxContextStatusBufferEntry", xeCsbPreempt, gOrgCsbPreempt},
				{"__ZN26IGHardwareCommandStreamer523csbProcessElementSwitchEPK28SGfxContextStatusBufferEntry", xeCsbElSwitch, gOrgCsbElSwitch},
				{"__ZN26IGHardwareCommandStreamer513evalWaitQueueEPKbbb", xeEvalWait, gOrgEvalWait},
				{"__ZN12IGScheduler516verifyThrottlingEPb", xeVerifyThr, gOrgVerifyThr},
				{"__ZN26IGHardwareCommandStreamer524prepareExecListAndSubmitEjjPKj", xePrepSubmit, gOrgPrepSubmit},
				{"__ZN17IGHardwareContext20restoreFromSafeImageEv", xeRestoreSafe, gOrgRestoreSafe},
			};
			SolveRequestPlus poshSolve[] = {{"__ZNK20IGSharedMappedBuffer17getVirtualAddressEv", gSmbGetVA}};
			SolveRequestPlus csbI2ASolve[] = {{"__ZN26IGHardwareCommandStreamer522csbProcessIdleToActiveEPK28SGfxContextStatusBufferEntry", gOrgCsbI2A}};
			if (!SolveRequestPlus::solveAll(patcher, index, csbI2ASolve, address, size)) { patcher.clearError(); gOrgCsbI2A = 0; }
			IOLog("XeNG CSBRESYNC: idleToActive=%s\n", gOrgCsbI2A ? "ok" : "NAO");
			gPmapFindPhys = patcher.solveSymbol(KernelPatcher::KernelID, "_pmap_find_phys");
			if (!gPmapFindPhys) patcher.clearError();
			gKernelPmap = patcher.solveSymbol(KernelPatcher::KernelID, "_kernel_pmap");
			if (!gKernelPmap) patcher.clearError();
			IOLog("XeNG PAGEONGUARD: pmap_find_phys=%s kernel_pmap=%s\n", gPmapFindPhys ? "ok" : "NAO", gKernelPmap ? "ok" : "NAO");
			if (!SolveRequestPlus::solveAll(patcher, index, poshSolve, address, size)) { patcher.clearError(); IOLog("XeNG POSH: getVirtualAddress nao resolvido\n"); }
			bool okr = RouteRequestPlus::routeAll(patcher, index, ringDiag, address, size);
			if (!okr) patcher.clearError();
			IOLog("XeNG RING diag hooks: %s\n", okr ? "OK" : "FALHOU");
			if (!checkKernelArgument("-ngreennovadiag")) {  // log-only, default on (boot-args are near the 228-char limit)
				RouteRequestPlus vaDiag[] = {{"__ZN19IGAccelVideoContext13bind_resourceER24IOAccelCommandStreamInfojPP15IGAccelResourcePyPjb", xeVaBind, gOrgVaBind}};
				bool okv = RouteRequestPlus::routeAll(patcher, index, vaDiag, address, size);
				if (!okv) patcher.clearError();
				IOLog("XeNG VADIAG: bind_resource hook %s\n", okv ? "OK" : "FALHOU");
			}
			bool ok = RouteRequestPlus::routeAll(patcher, index, flipDiag, address, size);
			if (!ok) patcher.clearError();
			IOLog("XeNG FLIP diag hooks: %s\n", ok ? "OK" : "FALHOU");
		}

		{
			RouteRequestPlus startRoute[] = {
				{"__ZN16IntelAccelerator5startEP9IOService", start, this->ostart},
			};
			if (RouteRequestPlus::routeAll(patcher, index, startRoute, address, size)) {
				SYSLOG("ngreen", "V44: Hooked IntelAccelerator::start");
			} else {
				SYSLOG("ngreen", "V44: IntelAccelerator::start symbol not found; Gen11::start logs unavailable on this build");
			}
		}

		{
			// loadGuCBinary: always route — WEG's firmware path is Mojave-gated and dead on Sonoma.
			// Without this hook, no GuC binary loads at all in coexist mode → ring dead.
			RouteRequestPlus firmwareRoute[] = {
				{"__ZN13IGHardwareGuC13loadGuCBinaryEv", loadGuCBinary, this->oloadGuCBinary},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, firmwareRoute, address, size), "ngreen", "Failed to route loadGuCBinary");
		}

		if (!NGreen::callback->isRealTGL) {
			// V111: Hook IGAccelDevice::deviceStart to force success on RPL.
			// Binary pattern (f_devstart) may not match every Sonoma build variant.
			// This symbol-based hook guarantees deviceStart returns true regardless of
			// BCS ring state, so the accelerator device is always registered with IOKit.
			RouteRequestPlus devStartRoute[] = {
				{"__ZN13IGAccelDevice11deviceStartEv", deviceStart, this->odeviceStart},
			};
			if (RouteRequestPlus::routeAll(patcher, index, devStartRoute, address, size)) {
				SYSLOG("ngreen", "V111: Hooked IGAccelDevice::deviceStart for RPL force-success");
			} else {
				SYSLOG("ngreen", "V111: IGAccelDevice::deviceStart symbol not found — relying on binary patch");
			}
		}

		if (!wegCoexist || forceFullMTL) {
			RouteRequestPlus coexistOffRoutes[] = {
				// ForceWake: replace Apple's SafeForceWakeMultithreaded with i915-ported version.
				// Apple's code uses 90ms timeouts and no fallback; ours uses 50ms + reserve-bit fallback.
				// The original domain mapping was broken (d<<1 loop misaligned Apple's 3-bit dom bitmap).
				{"__ZN16IntelAccelerator26SafeForceWakeMultithreadedEbjj", forceWake, this->oforceWake},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, coexistOffRoutes, address, size), "ngreen", "Failed to route coexist-off symbols");
			if (forceFullMTL && wegCoexist) {
				SYSLOG("ngreen", "FULL_MTL: forcing SafeForceWakeMultithreaded route despite coexist mode");
			}
		}

		if (!wegCoexist) {
			RouteRequestPlus gpuInfoRoute[] = {
				{"__ZN16IntelAccelerator10getGPUInfoEv", getGPUInfo, this->ogetGPUInfo},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, gpuInfoRoute, address, size), "ngreen", "Failed to route getGPUInfo");
		}

		// SKU/device-ID panic bypass (verified @ 0x23c1d in LE binary)
		// Original: mov edi,[rsi]; cmp edi,0xDEAFBEEE; jg sentinel; cmp edi,0x9A408086; je ok; cmp edi,0x9A488086; je ok; call panic
		// Patch:    nop the sentinel-jg + change last "je ok" → "jmp ok" → always jumps to GT2 init path.
		// Necessary: our spoofed 0x9A498086 is not in the whitelist (0x9A408086 / 0x9A488086).
		static const uint8_t f3[] = {
			0x8b, 0x3e, 0x81, 0xff, 0xee, 0xbe, 0xaf, 0xde, 0x7f, 0x15, 0x81, 0xff, 0x86, 0x80, 0x40, 0x9a, 0x74, 0x2d
		};
		static const uint8_t r3[] = {
			0x8b, 0x3e, 0x81, 0xff, 0xee, 0xbe, 0xaf, 0xde, 0x90, 0x90, 0x81, 0xff, 0x86, 0x80, 0x40, 0x9a, 0xeb, 0x2d
		};
		// GT tier override: stores GT1 (0x1) instead of GT2 (0x2) at IGAccelDevice+0x1120.
		// Disabled – 0x9A49 is GT2 so the default path (which writes 0x2) is correct.
		static const uint8_t f3a[] = {//gt1
			0x41, 0xc7, 0x86, 0x20, 0x11, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xe9, 0xda, 0xfc, 0xff, 0xff
		};
		static const uint8_t r3a[] = {
			0x41, 0xc7, 0x86, 0x20, 0x11, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xe9, 0xda, 0xfc, 0xff, 0xff
		};
		
		// L3BankCount bypass (verified @ 0x28776 in LE binary)
		// Original: topology-gated conditionals (cmp slices/eu/threads) → only set L3BankCount=8 for a specific config.
		// Patch:    NOP all conditional branches → always store L3BankCount=8 @ IGAccelDevice+0x1164.
		static const uint8_t f3b[] = {// jmp L3BankCount
			0x74, 0x23, 0x83, 0xf9, 0x02, 0x0f, 0x85, 0x89, 0x01, 0x00, 0x00, 0x83, 0xfe, 0x01, 0x75, 0x59, 0x83, 0xfa, 0x0c, 0x75, 0x54, 0x41, 0xc7, 0x87, 0x64, 0x11, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
		};
		static const uint8_t r3b[] = {
			0x90, 0x90, 0x83, 0xf9, 0x02, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x83, 0xfe, 0x01, 0x90, 0x90, 0x83, 0xfa, 0x0c, 0x90, 0x90, 0x41, 0xc7, 0x87, 0x64, 0x11, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
		};
		
		// MaxEUPerSubSlice override (verified @ 0x28692 in LE binary)
		// Original: MaxEUPerSubSlice = 8 - popcount(EUDisableFuse)  → stores result at IGAccelDevice+0x116c
		// Patch:    hardcodes MaxEUPerSubSlice=8 for RPL 96EU (8 EU per traditional sub-slice).
		//           TGL binary counts sub-slices (SS), not dual sub-slices (DSS).
		//           Linux shows 16 EU/DSS = 8 EU/SS since each DSS has 2 SS.
		static const uint8_t f3bb[] = {//MaxEUPerSubSlice
			0xbe, 0x08, 0x00, 0x00, 0x00, 0x29, 0xde, 0x41, 0x89, 0xb7, 0x6c, 0x11, 0x00, 0x00, 0x41, 0x8b, 0x8f, 0x58, 0x11, 0x00, 0x00
		};
		static const uint8_t r3bb[] = {
			0xbe, 0x08, 0x00, 0x00, 0x00, 0x90, 0x90, 0x41, 0x89, 0xb7, 0x6c, 0x11, 0x00, 0x00, 0x41, 0x8b, 0x8f, 0x58, 0x11, 0x00, 0x00
		};
		
		// NumSubSlices override (verified @ 0x28654 in LE binary)
		// Original: mov ebx,[rbp-0x30]; popcnt esi,ebx; add esi,esi; mov [r15+0x1158],esi
		//           → NumSubSlices = popcount(subsliceMask) * 2  (hardware-detected)
		// Patch:    hardcodes NumSubSlices=12 for RPL 96EU (6 DSS × 2 SS/DSS = 12 SS).
		//           Linux i915: subslice total=6 mask=0x3f, so 6 DSS doubled to 12 SS.
		static const uint8_t f3bbb[] = {//NumSubSlices
			0x8b, 0x5d, 0xd0, 0xf3, 0x0f, 0xb8, 0xf3, 0x01, 0xf6, 0x41, 0x89, 0xb7, 0x58, 0x11, 0x00, 0x00
		};
		static const uint8_t r3bbb[] = {
			0x8b, 0x5d, 0xd0, 0xbe, 0x0c, 0x00, 0x00, 0x00, 0x90, 0x41, 0x89, 0xb7, 0x58, 0x11, 0x00, 0x00
		};
		
		// GPU caps override (disabled) – would change MaxSlices 6→5, SARation 2→1, MaxEU/SS 6→5.
		// Leave disabled unless acceleration shows wrong Metal tier/feature set.
		static const uint8_t f4[] = {// CAPS
			0xc7, 0x83, 0x48, 0x11, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x8b, 0x83, 0x58, 0x11, 0x00, 0x00, 0xd1, 0xe8, 0xba, 0x02, 0x00, 0x00, 0x00, 0xbe, 0x06, 0x00, 0x00, 0x00
		};
		static const uint8_t r4[] = {
			0xc7, 0x83, 0x48, 0x11, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x8b, 0x83, 0x58, 0x11, 0x00, 0x00, 0x90, 0x90, 0xba, 0x01, 0x00, 0x00, 0x00, 0xbe, 0x05, 0x00, 0x00, 0x00
		};

		// V46: IGAccelDevice::deviceStart bypass — NOP the BCS failure gate
		// IGAccelDevice::deviceStart() makes one vtable check (at vtable+0x970); if it
		// returns 0, the whole device start aborts → IOServiceOpen fails → MTLDevice=nil.
		// Root cause: IGHardwareCommandStreamer5::init for BCS fails (BCS CTL=0x0, ring
		// not running), sets encodeFailureStack[1]=1, and the readiness check reads that.
		// The RCS ring IS operational (CTL=0x7000, HWS_PGA valid, 5 CSB events confirmed).
		// NOPing the je lets deviceStart always take the success path so IOServiceOpen
		// succeeds, MTLDevice is non-nil, and WindowServer stops hanging.
		//
		// Pattern (masked, Sonoma-variant tolerant):
		//   ff 90 70 09 00 00  callq *0x970(%rax)   ← readiness vtable call
		//   84 c0              testb %al,%al
		//   74 xx              je failure_path       ← PATCH: 74 xx → 90 90
		//   48 8d xx           lea ...               ← allow minor compiler/reg variance
		static const uint8_t f_devstart[] = {
			0xff, 0x90, 0x70, 0x09, 0x00, 0x00,
			0x84, 0xc0, 0x74, 0x00, 0x48, 0x8d, 0x00
		};
		static const uint8_t m_devstart[] = {
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00
		};
		static const uint8_t r_devstart[] = {
			0xff, 0x90, 0x70, 0x09, 0x00, 0x00,
			0x84, 0xc0, 0x90, 0x90, 0x48, 0x8d, 0x00
		};
		static const uint8_t rm_devstart[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00
		};

		// Some Sonoma builds encode the same readiness failure jump as long form:
		//   ff 90 70 09 00 00; 84 c0; 0f 84 xx xx xx xx
		// Patch 0f 84 rel32 -> 6x NOP to force success path.
		// Keep this optional (non-fatal) to avoid boot regressions when pattern differs.
		static const uint8_t f_devstart_long[] = {
			0xff, 0x90, 0x70, 0x09, 0x00, 0x00,
			0x84, 0xc0, 0x0f, 0x84, 0x00, 0x00, 0x00, 0x00
		};
		static const uint8_t m_devstart_long[] = {
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00
		};
		static const uint8_t r_devstart_long[] = {
			0xff, 0x90, 0x70, 0x09, 0x00, 0x00,
			0x84, 0xc0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
		};
		static const uint8_t rm_devstart_long[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
		};

		// V139: RPL-only mitigation for GP faults inside blit3d_submit_rectlist.
		// Some command-buffer pointers on spoofed paths are 8-byte aligned; Apple emits
		// aligned SSE stores (movaps [r9+...], xmmN), which faults on unaligned targets.
		// Convert the hot-path stores to movups to tolerate unaligned command pointers.
		static const uint8_t f_v139_movaps_10[] = {
			0x41, 0x0f, 0x29, 0x51, 0x10, 0x0f, 0x28, 0xd4
		};
		static const uint8_t r_v139_movaps_10[] = {
			0x41, 0x0f, 0x11, 0x51, 0x10, 0x0f, 0x28, 0xd4
		};
		static const uint8_t f_v139_movaps_30[] = {
			0x66, 0x0f, 0x3a, 0x21, 0xd4, 0x23, 0x41, 0x0f, 0x29, 0x51, 0x30
		};
		static const uint8_t r_v139_movaps_30[] = {
			0x66, 0x0f, 0x3a, 0x21, 0xd4, 0x23, 0x41, 0x0f, 0x11, 0x51, 0x30
		};
		static const uint8_t f_v139_movaps_50[] = {
			0x0f, 0x57, 0xd2, 0x0f, 0x16, 0xd4, 0x41, 0x0f, 0x29, 0x51, 0x50
		};
		static const uint8_t r_v139_movaps_50[] = {
			0x0f, 0x57, 0xd2, 0x0f, 0x16, 0xd4, 0x41, 0x0f, 0x11, 0x51, 0x50
		};
		static const uint8_t f_v139_movaps_00[] = {
			0x41, 0x0f, 0x29, 0x09
		};
		static const uint8_t r_v139_movaps_00[] = {
			0x41, 0x0f, 0x11, 0x09
		};
		static const uint8_t f_v139_movaps_20[] = {
			0x41, 0x0f, 0x29, 0x49, 0x20
		};
		static const uint8_t r_v139_movaps_20[] = {
			0x41, 0x0f, 0x11, 0x49, 0x20
		};
		static const uint8_t f_v139_movaps_40[] = {
			0x41, 0x0f, 0x29, 0x59, 0x40
		};
		static const uint8_t r_v139_movaps_40[] = {
			0x41, 0x0f, 0x11, 0x59, 0x40
		};
		static const uint8_t f_v141_movapd_00[] = {
			0x66, 0x41, 0x0f, 0x29, 0x09
		};
		static const uint8_t r_v141_movapd_00[] = {
			0x66, 0x41, 0x0f, 0x11, 0x09
		};

		// V140: Cover the switch cases that use contextual aligned stores at [r9] / [r9+20].
		// These are the remaining sites after V139's generic signatures.
		static const uint8_t f_v140_default_store00[] = {
			0x0f, 0x28, 0xcb, 0x66, 0x0f, 0x15, 0xcc, 0x41, 0x0f, 0x29, 0x09,
			0x0f, 0x28, 0xcb, 0x0f, 0xc6, 0xcc, 0xcc
		};
		static const uint8_t r_v140_default_store00[] = {
			0x0f, 0x28, 0xcb, 0x66, 0x0f, 0x15, 0xcc, 0x41, 0x0f, 0x11, 0x09,
			0x0f, 0x28, 0xcb, 0x0f, 0xc6, 0xcc, 0xcc
		};
		static const uint8_t f_v140_default_store20[] = {
			0x0f, 0xc6, 0xcc, 0xcc, 0x41, 0x0f, 0x29, 0x49, 0x20, 0x0f, 0x16, 0xdc
		};
		static const uint8_t r_v140_default_store20[] = {
			0x0f, 0xc6, 0xcc, 0xcc, 0x41, 0x0f, 0x11, 0x49, 0x20, 0x0f, 0x16, 0xdc
		};
		static const uint8_t f_v140_case4_store00[] = {
			0x0f, 0x28, 0xcc, 0xf2, 0x0f, 0x12, 0xcb, 0x66, 0x41, 0x0f, 0x29, 0x09,
			0x0f, 0x28, 0xcb, 0x0f, 0xc6, 0xcc, 0xc6
		};
		static const uint8_t r_v140_case4_store00[] = {
			0x0f, 0x28, 0xcc, 0xf2, 0x0f, 0x12, 0xcb, 0x66, 0x41, 0x0f, 0x11, 0x09,
			0x0f, 0x28, 0xcb, 0x0f, 0xc6, 0xcc, 0xc6
		};
		static const uint8_t f_v140_case4_store20[] = {
			0x0f, 0xc6, 0xcc, 0xc6, 0x41, 0x0f, 0x29, 0x49, 0x20, 0x0f, 0xc6, 0xdc, 0x4e
		};
		static const uint8_t r_v140_case4_store20[] = {
			0x0f, 0xc6, 0xcc, 0xc6, 0x41, 0x0f, 0x11, 0x49, 0x20, 0x0f, 0xc6, 0xdc, 0x4e
		};

		{
			// V52: Split patches into always-apply and RPL-only groups.
			// Real TGL reads topology from fuses correctly; RPL must hardcode
			// because fuse layout differs and BCS ring doesn't start.
			LookupPatchPlus const patchesAlways[] = {
				// SKU/device-ID bypass — needed for both (0x9A49 not in whitelist)
				{activeKext, f3, r3, arrsize(f3),	1},
			};
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesAlways, address, size), "ngreen",
				"kextG11HWT Failed to apply base patches!");
			
			if (!NGreen::callback->isRealTGL) {
				// RPL-only: hardcode topology and bypass BCS readiness check
				LookupPatchPlus const patchesRPL[] = {
					{activeKext, f3b, r3b, arrsize(f3b),	1},      // L3BankCount=8
					{activeKext, f3bb, r3bb, arrsize(f3bb),	1},    // MaxEU/SS=8
					{activeKext, f3bbb, r3bbb, arrsize(f3bbb),	1},// NumSubSlices=12
					{activeKext, f_devstart, m_devstart, r_devstart, rm_devstart, arrsize(f_devstart), 1}, // BCS bypass
				};
				PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesRPL, address, size), "ngreen",
					"kextG11HWT Failed to apply RPL-specific patches!");

				// xe/noccs3 (19/10): o XE-NOCCS2 (cortar enableRenderCompressionWithAccelTask inteira) deixava o driver Metal
				// usando fast clear / stencil comprimido SEM mapeamento AUX -> hang do RCS no PIPE_CONTROL após o fast clear.
				// O que panicava era só o RESOLVE: IntelAccelerator::submitCCSResolve -> resolve_ccs escreve no buffer 2 do
				// IOAccelCommandBufferPool2, que no IOAcceleratorFamily2 do Tahoe não existe/difere (page fault).
				// Agora: a compressão/AUX segue normal e submitCCSResolve retorna logo (xor eax,eax; ret); o chamador
				// (IGAccelResource::submitCCSResolve) ignora o retorno. Prólogo (24 bytes, único) em 0x2c84c no 16.0.32.
				static const uint8_t f_noccs3[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
				                                    0x53, 0x48, 0x83, 0xec, 0x48, 0x48, 0x89, 0x75, 0xc0, 0xf6, 0x87, 0x78};
				static const uint8_t r_noccs3[] = {0x31, 0xc0, 0xc3, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
				                                    0x53, 0x48, 0x83, 0xec, 0x48, 0x48, 0x89, 0x75, 0xc0, 0xf6, 0x87, 0x78};
				// xe/fmt (19/10): IGAccelDisplayPipe::performTransaction lê o formato do quadro de IOSurface+0xa4 direto da
				// memória (0x81361 no 16.0.32). No Tahoe o layout do IOSurface mudou e vem 0 -> o FB programa o plano como
				// YUV 4:2:2 (PLANE_CTL 0x90010400): tela verde em 2 tons + metade com lixo. Usa o formato do desktop, 'BGRA'.
				static const uint8_t f_fmt[] = {0x8b, 0x83, 0xa4, 0x00, 0x00, 0x00, 0x89, 0x85, 0xf4, 0xfe, 0xff, 0xff};
				static const uint8_t r_fmt[] = {0xb8, 0x41, 0x52, 0x47, 0x42, 0x90, 0x89, 0x85, 0xf4, 0xfe, 0xff, 0xff};
				LookupPatchPlus const patchFmt[] = {{activeKext, f_fmt, r_fmt, arrsize(f_fmt), 1}};
				SYSLOG("ngreen", "XE-FMT: performTransaction formato do IOSurface -> 'BGRA': %s",
					   LookupPatchPlus::applyAll(patcher, patchFmt, address, size) ? "aplicado" : "NAO encontrado");
				// xe/noposh (19/10): IGHardwareRingBuffer::submitBatchBuffer liga o bit 20 do MI_BATCH_BUFFER_START quando o
				// contexto tem POSH (ctx+0x6e bit 0x10): (flags & 0x10) << 16. Só o ctx16 roda batches 0x18900101 e, depois dele,
				// o RCS não troca mais de contexto (execlists válidas, nenhuma ativa). O RPL não tem o pipe POSH do TGL.
				// "and eax,0x10" -> "and eax,0" em 0x43021 (único). Desliga com -ngreenposh.
				static const uint8_t f_noposh[] = {0x31, 0xc9, 0x83, 0xe0, 0x10, 0x48, 0xc1, 0xe0, 0x10};
				static const uint8_t r_noposh[] = {0x31, 0xc9, 0x83, 0xe0, 0x00, 0x48, 0xc1, 0xe0, 0x10};
				if (checkKernelArgument("-ngreennoposh")) {  // 19/10: desligar o POSH deixou a tela preta; só por boot-arg
					LookupPatchPlus const patchNoPosh[] = {{activeKext, f_noposh, r_noposh, arrsize(f_noposh), 1}};
					bool okp = LookupPatchPlus::applyAll(patcher, patchNoPosh, address, size);
					IOLog("XeNG NOPOSH: submitBatchBuffer sem bit POSH: %s\n", okp ? "aplicado" : "NAO encontrado");
				}
				LookupPatchPlus const patchNoCCS3[] = {{activeKext, f_noccs3, r_noccs3, arrsize(f_noccs3), 1}};
				SYSLOG("ngreen", "XE-NOCCS3: IntelAccelerator::submitCCSResolve -> retorno imediato: %s",
					   LookupPatchPlus::applyAll(patcher, patchNoCCS3, address, size) ? "aplicado" : "NAO encontrado");
				// xe/nodepth (20/10): mesmo problema do NOCCS3 pelo caminho irmão. Pânico do iconservicesagent em resolve_hiz_g7
				// <- IntelAccelerator::submitDepthResolve <- IGAccelResource::submitDepthResolve <- submitCCSResolve <-
				// enableRenderCompressionWithAccelTask: o resolve HiZ também escreve no IOAccelCommandBufferPool2 do Tahoe.
				// Os dois chamadores (0x744d7, 0x788e6) ignoram o retorno. Prólogo + "0f 84" (único) em 0x2c656 do 16.0.32.
				// Desliga com -ngreendepthresolve.
				static const uint8_t f_nodepth[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
				                                     0x48, 0x83, 0xec, 0x38, 0xf6, 0x87, 0x78, 0x0c, 0x00, 0x00, 0x02, 0x0f, 0x84};
				static const uint8_t r_nodepth[] = {0x31, 0xc0, 0xc3, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
				                                     0x48, 0x83, 0xec, 0x38, 0xf6, 0x87, 0x78, 0x0c, 0x00, 0x00, 0x02, 0x0f, 0x84};
				if (!checkKernelArgument("-ngreendepthresolve")) {
					LookupPatchPlus const patchNoDepth[] = {{activeKext, f_nodepth, r_nodepth, arrsize(f_nodepth), 1}};
					SYSLOG("ngreen", "XE-NODEPTH: IntelAccelerator::submitDepthResolve -> retorno imediato: %s",
						   LookupPatchPlus::applyAll(patcher, patchNoDepth, address, size) ? "aplicado" : "NAO encontrado");
				}
				// xe/noaux (19/10): o RCS trava (sem fault) no PIPE_CONTROL depois de desenhar em RTs pequenos com
				// RENDER_SURFACE_STATE AuxiliarySurfaceMode=5 (CCS_E): a AUX-TT não cobre essas superfícies no RPL.
				// IGAccelResource::fillSurfaceStateAuxInfo põe aux=4/5 quando ResourceInfoEntry+0x91 bit 0x02; senão
				// zera os 3 bits. "jne" (75 0b) -> NOPs em 0x73fd0 do 16.0.32: sempre aux=NONE. -ngreenccs desliga.
				static const uint8_t f_noaux[] = {0xf6, 0x86, 0x91, 0x00, 0x00, 0x00, 0x02, 0x8b, 0x42, 0x18, 0x75, 0x0b,
				                                  0x83, 0xe0, 0xf8, 0x89, 0x42, 0x18};
				static const uint8_t r_noaux[] = {0xf6, 0x86, 0x91, 0x00, 0x00, 0x00, 0x02, 0x8b, 0x42, 0x18, 0x90, 0x90,
				                                  0x83, 0xe0, 0xf8, 0x89, 0x42, 0x18};
				// xe/notcf (19/10): o 1º restart de cada boot é o contexto do pipe de display parado no PIPE_CONTROL do flip
				// (IGAccelDisplayPipe::generateFlip, dw1 0x50105021 = DC/RT/depth flush + CS stall + post-sync + TILE CACHE
				// FLUSH), logo depois do desenho 3D do flip, com o 3D ocioso. O resto do driver usa 0x40105021 (sem o bit 28)
				// e passa. Tira o Tile Cache Flush dos 3 "movabs rsi, 0x501050217a000004". -ngreentcf desliga.
				static const uint8_t f_notcf[] = {0x48, 0xbe, 0x04, 0x00, 0x00, 0x7a, 0x21, 0x50, 0x10, 0x50};
				static const uint8_t r_notcf[] = {0x48, 0xbe, 0x04, 0x00, 0x00, 0x7a, 0x21, 0x50, 0x10, 0x40};
				// xe/notcf2 (19/10): o PIPE_CONTROL do anel que trava NÃO vem de lá: é o flush antes do stamp em
				// IGHardwareRingBuffer{Main,Compute}::commitStampCommand(stamp, bool): dw1 = 0x40105021 | (bool << 28).
				// "shl r14, 0x3c" -> "xor r14d, r14d; nop" (o bool continua salvo em [rbp-0x58] para o resto da função).
				static const uint8_t f_notcf2[] = {0x4c, 0x89, 0x75, 0xa8, 0x49, 0xc1, 0xe6, 0x3c, 0x49, 0x09, 0xde};
				static const uint8_t r_notcf2[] = {0x4c, 0x89, 0x75, 0xa8, 0x45, 0x31, 0xf6, 0x90, 0x49, 0x09, 0xde};
				if (!checkKernelArgument("-ngreentcf")) {
					LookupPatchPlus const patchNoTcf[] = {{activeKext, f_notcf, r_notcf, arrsize(f_notcf), 3}};
					bool okt = LookupPatchPlus::applyAll(patcher, patchNoTcf, address, size);
					IOLog("XeNG NOTCF: generateFlip PIPE_CONTROL sem Tile Cache Flush: %s\n", okt ? "aplicado" : "NAO encontrado");
					LookupPatchPlus const patchNoTcf2[] = {{activeKext, f_notcf2, r_notcf2, arrsize(f_notcf2), 2}};
					bool okt2 = LookupPatchPlus::applyAll(patcher, patchNoTcf2, address, size);
					IOLog("XeNG NOTCF2: commitStampCommand sem Tile Cache Flush: %s\n", okt2 ? "aplicado" : "NAO encontrado");
				}
				// xe/blit16 (19/10): o hang do contexto Blit3D (usado pelo pipe de display) continua com o flush limpo: a
				// Apple diz EUs/WMFE/PSS "not done" -> o PS do blit3d em SIMD32 (IGBlit3DKernel_default_simd32, KSP1=0x540)
				// não termina no RPL. blit3d_submit_commands já usa SÓ SIMD16 (KSP0 = kernel simd16, dw7=0x60000) quando
				// params+0x55 == 0x10; força esse caminho trocando "cmp cl,0x10" por "cmp cl,cl" (ZF=1) nos 3 pontos.
				static const uint8_t f_b16a[] = {0x31, 0xf6, 0x80, 0xf9, 0x10, 0x40, 0x0f, 0x95, 0xc6};
				static const uint8_t r_b16a[] = {0x31, 0xf6, 0x38, 0xc9, 0x90, 0x40, 0x0f, 0x95, 0xc6};
				static const uint8_t f_b16b[] = {0x80, 0xf9, 0x10, 0x48, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00};
				static const uint8_t r_b16b[] = {0x38, 0xc9, 0x90, 0x48, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00};
				// xe/planes (19/10): abrir Ajustes > Telas dava pânico "divide error" em
				// IGAccelResource::calculateIOSurfaceDeviceCacheVRAMBytes (0x72bcd, divl r15d = 0): a contagem de planos é
				// lida direto de IOSurface+0xb0, offset que mudou no Tahoe (como o +0xa4 do XE-FMT), e superfícies comuns
				// pareciam planares sem informação de plano. "mov eax,[rbx+0xb0]" -> "xor eax,eax" (sem planos).
				// xe/planes2 (20/10): o "xor" fazia toda superfície ter 1 plano -> a tabela de planos (this+0x250) não tinha
				// o plano 1 e get_tex_data devolvia endereço vazio: YUV (HEIC, vídeo) com croma zerado = papel de parede
				// verde, quadros de vídeo vazios. No Tahoe IOSurface::getPlaneCount() lê [rdi+0x98] (KC 25G241), então
				// lê do offset certo: "mov eax,[rbx+0x98]". -ngreenplanesxor volta ao patch antigo.
				static const uint8_t f_planes[] = {0x8b, 0x83, 0xb0, 0x00, 0x00, 0x00, 0x85, 0xc0, 0xba, 0x01, 0x00, 0x00, 0x00};
				static const uint8_t r_planes_xor[] = {0x31, 0xc0, 0x90, 0x90, 0x90, 0x90, 0x85, 0xc0, 0xba, 0x01, 0x00, 0x00, 0x00};
				static const uint8_t r_planes_98[] = {0x8b, 0x83, 0x98, 0x00, 0x00, 0x00, 0x85, 0xc0, 0xba, 0x01, 0x00, 0x00, 0x00};
				const uint8_t *r_planes = checkKernelArgument("-ngreenplanesxor") ? r_planes_xor : r_planes_98;
				if (!checkKernelArgument("-ngreennoplanesfix")) {
					LookupPatchPlus const patchPlanes[] = {{activeKext, f_planes, r_planes, arrsize(f_planes), 1}};
					bool okp2 = LookupPatchPlus::applyAll(patcher, patchPlanes, address, size);
					IOLog("XeNG PLANES2: calculateIOSurfaceDeviceCacheVRAMBytes planos de IOSurface+%s: %s\n", r_planes == r_planes_98 ? "0x98" : "nenhum (xor)", okp2 ? "aplicado" : "NAO encontrado");
				}
				// xe/blitalign (20/10): plano de IOSurface com offset não múltiplo de 64 (o CoreVideo usa 32 e 2073632 no 420v)
				// = textura zerada e escrita perdida -> gravação de tela verde, conversões VTPixelTransfer vazias, Safari -6680.
				// A GPU usa uma cópia da superfície (device cache, +0x04 da tabela de planos); pageon/pageoff copiam com
				// IntelAccelerator::submitBlit. blit2d_common_supported exige (endereço & 0x3f) == 0 também para o
				// XY_SRC_COPY_BLT comum (só o fast copy precisa de 64 B), e cópias lineares grandes não-fast vão para o
				// blit 3D, que não funciona aqui. Máscara 0x3f -> 0x0f (origem e destino) e o "je" que manda cópia grande
				// para o 3D vira nop: vão para blt2d_source_copy (motor de cópia). -ngreennoblitalign desliga.
				static const uint8_t f_ba_src[] = {0x8b, 0x0f, 0x03, 0x4f, 0x2c, 0x83, 0xe1, 0x3f, 0x48, 0x85, 0xc9};
				static const uint8_t r_ba_src[] = {0x8b, 0x0f, 0x03, 0x4f, 0x2c, 0x83, 0xe1, 0x0f, 0x48, 0x85, 0xc9};
				static const uint8_t f_ba_dst[] = {0x8b, 0x4f, 0x48, 0x03, 0x4f, 0x74, 0x83, 0xe1, 0x3f, 0x48, 0x85, 0xc9};
				static const uint8_t r_ba_dst[] = {0x8b, 0x4f, 0x48, 0x03, 0x4f, 0x74, 0x83, 0xe1, 0x0f, 0x48, 0x85, 0xc9};
				// 21/10 (BLITALIGN v2): a v1 trocava o "je 3D" por nop e mandava TODA cópia linear grande não-fast para o
				// blt2d_source_copy -> ícones/imagens do Chrome sumiam. Agora o bloco de roteamento (submitBlit 0x2c228,
				// P = r14) só desvia para o 2D quando ((src|dst) & 0x3f) != 0; alinhadas voltam ao 3D como no original.
				// src = [P]+[P+0x2c], dst = [P+0x48]+[P+0x74] (as mesmas expressões do blit2d_common_supported).
				static const uint8_t f_ba_rota[] = {0x41,0x0f,0xb6,0x8e,0xa0,0x00,0x00,0x00,0x48,0x83,0xf9,0x10,0x0f,0x94,0xc0,0x48,0x0f,0xaf,0xd1,0x48,0x81,0xfa,0x01,0x10,0x05,0x00,0x0f,0x92,0xc1,0xbe,0x01,0x00,0x00,0x00,0x48,0x81,0xfa,0x01,0x00,0x02,0x00,0x0f,0x82,0xb5,0xfc,0xff,0xff,0x20,0xc8,0x0f,0x84,0xc9,0xfa,0xff,0xff,0xe9,0xa8,0xfc,0xff,0xff};
				static const uint8_t r_ba_rota[] = {0x41,0x0f,0xb6,0x8e,0xa0,0x00,0x00,0x00,0x48,0x0f,0xaf,0xd1,0xbe,0x01,0x00,0x02,0x00,0x80,0xf9,0x10,0x75,0x05,0xbe,0x01,0x10,0x05,0x00,0x48,0x39,0xf2,0x72,0x19,0x41,0x8b,0x06,0x41,0x03,0x46,0x2c,0x41,0x8b,0x4e,0x48,0x41,0x03,0x4e,0x74,0x09,0xc8,0xa8,0x3f,0x0f,0x84,0xc7,0xfa,0xff,0xff,0xeb,0xbe,0xcc};
				if (!checkKernelArgument("-ngreennoblitalign")) {
					LookupPatchPlus const patchBa[] = {
						{activeKext, f_ba_src, r_ba_src, arrsize(f_ba_src), 1},
						{activeKext, f_ba_dst, r_ba_dst, arrsize(f_ba_dst), 1},
						{activeKext, f_ba_rota, r_ba_rota, arrsize(f_ba_rota), 1}};
					bool okba = LookupPatchPlus::applyAll(patcher, patchBa, address, size);
					IOLog("XeNG BLITALIGN: cópias lineares de IOSurface desalinhadas (offset %% 64) pelo motor de cópia: %s\n", okba ? "aplicado" : "NAO encontrado");
				}
				// xe/rpcs (20/10, do WhateverXe 1163642): o construtor do contexto RCS grava R_PWR_CLK_STATE (0x20c8) = 0x80000000
				// (só ENABLE). O Linux pede ENABLE | S_CNT_ENABLE | 1 fatia (0x80041000); o RPL também tem 1 fatia (SLICE_EN=1).
				// "mov dword [r15+0x110c], 0x80000000" em 0x7c313 do 16.0.32. -ngreennorpcs desliga.
				static const uint8_t f_rpcs[] = {0x41, 0xc7, 0x87, 0x0c, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80};
				static const uint8_t r_rpcs[] = {0x41, 0xc7, 0x87, 0x0c, 0x11, 0x00, 0x00, 0x00, 0x10, 0x04, 0x80};
				// 20/10: opt-in (-ngreenrpcs): o boot com RPCS+SAMETAIL ficou com o envio muito lento; isolando
				if (checkKernelArgument("-ngreenrpcs")) {
					LookupPatchPlus const patchRpcs[] = {{activeKext, f_rpcs, r_rpcs, arrsize(f_rpcs), 1}};
					bool okr2 = LookupPatchPlus::applyAll(patcher, patchRpcs, address, size);
					IOLog("XeNG RPCS: R_PWR_CLK_STATE do contexto RCS = 0x80041000: %s\n", okr2 ? "aplicado" : "NAO encontrado");
				}
				if (!checkKernelArgument("-ngreenblit32")) {
					LookupPatchPlus const patchB16[] = {
						{activeKext, f_b16a, r_b16a, arrsize(f_b16a), 1},
						{activeKext, f_b16b, r_b16b, arrsize(f_b16b), 2},
					};
					bool okb = LookupPatchPlus::applyAll(patcher, patchB16, address, size);
					IOLog("XeNG BLIT16: blit3d PS só SIMD16: %s\n", okb ? "aplicado" : "NAO encontrado");
				}
				if (!checkKernelArgument("-ngreenccs")) {
					LookupPatchPlus const patchNoAux[] = {{activeKext, f_noaux, r_noaux, arrsize(f_noaux), 1}};
					bool oka = LookupPatchPlus::applyAll(patcher, patchNoAux, address, size);
					IOLog("XeNG NOAUX: fillSurfaceStateAuxInfo sempre AUX_NONE: %s\n", oka ? "aplicado" : "NAO encontrado");
				}
				// xe/rcpolicy (21/10, from WhateverXe jkbuha 1a9b960/76a9459): IntelAccelerator+0x1474 is the display/IOSurface
				// "renderCompression" policy. start() leaves it 0 on real hardware (it is 1 only in the simulator), but the TGL
				// FB's setDisplayMode calls callPlatformFunction("renderCompression", !hasExternalDisplay()) when the YTiling
				// feature is on (FeatureControl YTiling=1 here) -> 1 with the internal panel only. XE-NOCCS's properties do not
				// touch it. With 1: calculateIOSurfaceDeviceCacheVRAMBytes reserves CCS space for IOSurfaces, flips may keep a
				// compressed scanout and generateFlip may set PLANE_CTL bit 15 (render decompression) although AUX is forced
				// NONE (XE-NOAUX) and the resolves are stubbed (NOCCS3/NODEPTH). With 0 the accelerator runs Apple's
				// "external display connected" path: every flip goes through disableRenderCompression first.
				// Startup immediate (0x246e0) 1->0 and the setter (0x2b872) "mov al,[r15]" -> "mov al,0; nop".
				// -ngreennorc disables.
				if (xeFixEnabled() && !checkKernelArgument("-ngreennorc")) {
					static const uint8_t f_rcstart[] = {0x41, 0xf6, 0x06, 0x20, 0x74, 0x08, 0x41, 0xc6, 0x85, 0x74, 0x14, 0x00, 0x00, 0x01};
					static const uint8_t r_rcstart[] = {0x41, 0xf6, 0x06, 0x20, 0x74, 0x08, 0x41, 0xc6, 0x85, 0x74, 0x14, 0x00, 0x00, 0x00};
					static const uint8_t f_rcset[] = {0x41, 0x8a, 0x07, 0x41, 0x88, 0x86, 0x74, 0x14, 0x00, 0x00, 0xeb, 0x16};
					static const uint8_t r_rcset[] = {0xb0, 0x00, 0x90, 0x41, 0x88, 0x86, 0x74, 0x14, 0x00, 0x00, 0xeb, 0x16};
					LookupPatchPlus const patchRcStart[] = {{activeKext, f_rcstart, r_rcstart, arrsize(f_rcstart), 1}};
					LookupPatchPlus const patchRcSet[] = {{activeKext, f_rcset, r_rcset, arrsize(f_rcset), 1}};
					bool oks = LookupPatchPlus::applyAll(patcher, patchRcStart, address, size);
					if (!oks) patcher.clearError();
					bool okr = LookupPatchPlus::applyAll(patcher, patchRcSet, address, size);
					if (!okr) patcher.clearError();
					IOLog("XeNG RCPOLICY: renderCompression (accel+0x1474) sempre 0: start %s, setter %s\n",
						  oks ? "aplicado" : "NAO encontrado", okr ? "aplicado" : "NAO encontrado");
				}
				// xe/ffdop + xe/objpreempt (21/10, opt-in; WhateverXe r145/r135): IntelAccelerator::startGraphicsEngine writes
				// CS_DEBUG_MODE1 (0x20ec) = 0x10001 and feeds the same EDX into CS_CHICKEN1 (0x2580). Our V163/V164 already
				// clear FF_SLICE_CS_CHICKEN1 bit 14, so 3D object-level preemption is selected by 0x20ec bit 0 (=1 today).
				//   -ngreenffdop        : also set 0x20ec bit 1 FF_DOP_CLOCK_GATE_DISABLE (Linux Wa_1606700617 tgl/dg1/adl-p).
				//   -ngreenobjpoff: clear bit 0 in 0x20ec and 0x2580 (object-level preemption off; WEX TGL-B0 erratum).
				// Window at 0x25166 (16.0.32): "mov edx,[rax+0x20ec]; mov edx,0x10001; mov [rax+0x20ec],edx" becomes
				// "mov edx,<20ec>; mov [rax+0x20ec],edx; mov edx,<2580 bits>; nop" (the discarded MMIO read goes away).
				{
					bool ffdop = checkKernelArgument("-ngreenffdop");
					bool objoff = checkKernelArgument("-ngreenobjpoff");
					if (ffdop || objoff) {
						uint8_t v20ec = (ffdop ? 0x02 : 0x00) | (objoff ? 0x00 : 0x01);
						uint8_t m20ec = 0x01 | (ffdop ? 0x02 : 0x00);
						uint8_t v2580 = objoff ? 0x00 : 0x01;
						static const uint8_t f_preempt[] = {0x8b, 0x90, 0xec, 0x20, 0x00, 0x00, 0xba, 0x01, 0x00, 0x01, 0x00,
						                                    0x89, 0x90, 0xec, 0x20, 0x00, 0x00};
						uint8_t r_preempt[] = {0xba, v20ec, 0x00, m20ec, 0x00, 0x89, 0x90, 0xec, 0x20, 0x00, 0x00,
						                       0xba, v2580, 0x00, 0x01, 0x00, 0x90};
						static_assert(sizeof(r_preempt) == sizeof(f_preempt), "preempt patch size");
						LookupPatchPlus const patchPreempt[] = {{activeKext, f_preempt, r_preempt, arrsize(f_preempt), 1}};
						bool okp = LookupPatchPlus::applyAll(patcher, patchPreempt, address, size);
						if (!okp) patcher.clearError();
						IOLog("XeNG PREEMPT: 0x20ec=0x%02x%04x 0x2580|=0x1%04x (ffdop=%d objoff=%d): %s\n", m20ec, v20ec, v2580,
							  ffdop, objoff, okp ? "aplicado" : "NAO encontrado");
					}
				}

				// Optional secondary signature for Sonoma variants with long JE encoding.
				/*LookupPatchPlus const patchRPLDevstartLong {
					activeKext,
					f_devstart_long,
					m_devstart_long,
					r_devstart_long,
					rm_devstart_long,
					arrsize(f_devstart_long),
					1
				};
				if (patchRPLDevstartLong.apply(patcher, address, size)) {
					SYSLOG("ngreen", "V52: Applied optional long-form deviceStart readiness bypass");
				} else {
					SYSLOG("ngreen", "V52: Optional long-form deviceStart bypass not found on this build");
				}*/

				// Optional SSE unaligned-store mitigation for blit3d_submit_rectlist.
				LookupPatchPlus const patchV139Store10 {
					activeKext,
					f_v139_movaps_10,
					r_v139_movaps_10,
					arrsize(f_v139_movaps_10),
					1
				};
				LookupPatchPlus const patchV139Store30 {
					activeKext,
					f_v139_movaps_30,
					r_v139_movaps_30,
					arrsize(f_v139_movaps_30),
					1
				};
				LookupPatchPlus const patchV139Store50 {
					activeKext,
					f_v139_movaps_50,
					r_v139_movaps_50,
					arrsize(f_v139_movaps_50),
					1
				};
				LookupPatchPlus const patchV139Store00 {
					activeKext,
					f_v139_movaps_00,
					r_v139_movaps_00,
					arrsize(f_v139_movaps_00),
					1
				};
				LookupPatchPlus const patchV139Store00Skip1 {
					activeKext,
					f_v139_movaps_00,
					r_v139_movaps_00,
					arrsize(f_v139_movaps_00),
					1,
					1
				};
				LookupPatchPlus const patchV139Store00Skip2 {
					activeKext,
					f_v139_movaps_00,
					r_v139_movaps_00,
					arrsize(f_v139_movaps_00),
					1,
					2
				};
				LookupPatchPlus const patchV139Store00Skip3 {
					activeKext,
					f_v139_movaps_00,
					r_v139_movaps_00,
					arrsize(f_v139_movaps_00),
					1,
					3
				};
				LookupPatchPlus const patchV139Store00Skip4 {
					activeKext,
					f_v139_movaps_00,
					r_v139_movaps_00,
					arrsize(f_v139_movaps_00),
					1,
					4
				};
				LookupPatchPlus const patchV139Store00Skip5 {
					activeKext,
					f_v139_movaps_00,
					r_v139_movaps_00,
					arrsize(f_v139_movaps_00),
					1,
					5
				};
				LookupPatchPlus const patchV139Store20 {
					activeKext,
					f_v139_movaps_20,
					r_v139_movaps_20,
					arrsize(f_v139_movaps_20),
					1
				};
				LookupPatchPlus const patchV139Store20Skip1 {
					activeKext,
					f_v139_movaps_20,
					r_v139_movaps_20,
					arrsize(f_v139_movaps_20),
					1,
					1
				};
				LookupPatchPlus const patchV139Store20Skip2 {
					activeKext,
					f_v139_movaps_20,
					r_v139_movaps_20,
					arrsize(f_v139_movaps_20),
					1,
					2
				};
				LookupPatchPlus const patchV139Store20Skip3 {
					activeKext,
					f_v139_movaps_20,
					r_v139_movaps_20,
					arrsize(f_v139_movaps_20),
					1,
					3
				};
				LookupPatchPlus const patchV139Store40 {
					activeKext,
					f_v139_movaps_40,
					r_v139_movaps_40,
					arrsize(f_v139_movaps_40),
					1
				};
				LookupPatchPlus const patchV141StoreMovapd00 {
					activeKext,
					f_v141_movapd_00,
					r_v141_movapd_00,
					arrsize(f_v141_movapd_00),
					1
				};
				LookupPatchPlus const patchV140DefaultStore00 {
					activeKext,
					f_v140_default_store00,
					r_v140_default_store00,
					arrsize(f_v140_default_store00),
					1
				};
				LookupPatchPlus const patchV140DefaultStore20 {
					activeKext,
					f_v140_default_store20,
					r_v140_default_store20,
					arrsize(f_v140_default_store20),
					1
				};
				LookupPatchPlus const patchV140Case4Store00 {
					activeKext,
					f_v140_case4_store00,
					r_v140_case4_store00,
					arrsize(f_v140_case4_store00),
					1
				};
				LookupPatchPlus const patchV140Case4Store20 {
					activeKext,
					f_v140_case4_store20,
					r_v140_case4_store20,
					arrsize(f_v140_case4_store20),
					1
				};
				int v139Applied = 0;
				v139Applied += patchV139Store10.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store30.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store50.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store00.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store00Skip1.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store00Skip2.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store00Skip3.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store00Skip4.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store00Skip5.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store20.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store20Skip1.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store20Skip2.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store20Skip3.apply(patcher, address, size) ? 1 : 0;
				v139Applied += patchV139Store40.apply(patcher, address, size) ? 1 : 0;
				SYSLOG("ngreen", "V139: blit3d unaligned-store mitigation applied %d/12 signatures", v139Applied);
				int v141Applied = 0;
				v141Applied += patchV141StoreMovapd00.apply(patcher, address, size) ? 1 : 0;
				SYSLOG("ngreen", "V141: blit3d movapd mitigation applied %d/1 signatures", v141Applied);
				int v140Applied = 0;
				v140Applied += patchV140DefaultStore00.apply(patcher, address, size) ? 1 : 0;
				v140Applied += patchV140DefaultStore20.apply(patcher, address, size) ? 1 : 0;
				v140Applied += patchV140Case4Store00.apply(patcher, address, size) ? 1 : 0;
				v140Applied += patchV140Case4Store20.apply(patcher, address, size) ? 1 : 0;
				SYSLOG("ngreen", "V140: contextual blit3d store mitigation applied %d/4 signatures", v140Applied);

				SYSLOG("ngreen", "V52: Applied RPL-specific patches (topology hardcode + BCS bypass)");
			} else {
				SYSLOG("ngreen", "V52: Real TGL — skipping topology hardcodes and BCS bypass");
			}
		}

		SYSLOG("ngreen", "Loaded AppleIntelTGLGraphics! %s",
			   NGreen::callback->isRealTGL ? "Real TGL — native topology" :
			   "RPL spoofed — slices=1 subslices=12(6DSS) maxEU/SS=8 totalEU=96 L3=8");

		return true;
	}

    return false;
}

void *ccont;
void *ccont2;  // AppleIntelBaseController pointer (captured in FBMemMgr_Init)

//FB Hooks

uint64_t Gen11::AppleIntelScalerinit(AppleIntel::AppleIntelScaler *that, uint32_t pipeIndex)
{
	auto ret = FunctionCast(AppleIntelScalerinit, callback->oAppleIntelScalerinit)(that, pipeIndex);
	that->fWriteAccessor = ccont;
	that->fController    = reinterpret_cast<AppleIntel::AppleIntelBaseController *>(ccont2);
	return ret;
}

uint64_t Gen11::AppleIntelPlaneinit(AppleIntel::AppleIntelPlane *that, uint32_t pipeIndex)
{
	auto ret = FunctionCast(AppleIntelPlaneinit, callback->oAppleIntelPlaneinit)(that, pipeIndex);
	getMember<void *>(that, 0x90) = ccont; // fWriteAccessor — ccont must NOT go to real fRegCache at +0x88

	return ret;
}

void Gen11::disableScaler(AppleIntel::AppleIntelScaler *that, bool disable)
{
	that->fWriteAccessor = ccont;
	FunctionCast(disableScaler, callback->odisableScaler)(that, disable);
}

void Gen11::enablePlane(AppleIntel::AppleIntelPlane *that, bool enable)
{
	getMember<void *>(that, 0x90) = ccont; // fWriteAccessor — ccont must NOT go to real fRegCache at +0x88
	FunctionCast(enablePlane, callback->oenablePlane)(that, enable);

}

void Gen11::programPipeScaler(AppleIntel::AppleIntelScaler *that, AppleIntel::AppleIntelDisplayPath *displayPath)
{
	that->fWriteAccessor = ccont;
	FunctionCast(programPipeScaler, callback->oprogramPipeScaler)(that, displayPath);
}

// V400: AppleIntelScaler::setupPipeScaler(AppleIntelDisplayPath *, CRTCParams *)
// ORIGIN-LEVEL FIX for seam-joining scaler wrongly enabled on single-pipe panels.
//
// Apple's setupPipeScaler enters the "Seam joining scaler enabled" branch when
// a byte gate is non-zero — disasm reveals:
//   rax = this->[+0x10]                     (AppleIntelScaler's controller ptr)
//   if [rax+0x3FD8] == 0xFFFFFFFF (device-tree absent on spoofed RPL)
//       rax += 0x1E5
//   else
//       rax += 0x1E3
//   if [rax] != 0:  enable seam joining
//   else:           skip seam joining
//
// Linux i915 on this exact hardware confirms `bigjoiner: no, pipes: 0x0` —
// dual-pipe joining must NOT be active for this single-pipe 2560x1600 eDP panel.
// Apple is doing it anyway, producing the per-row offset / fragmented pattern.
//
// Fix: before calling original, zero BOTH gate bytes (+0x1E3 and +0x1E5) at
// this->[+0x10]. Belt-and-suspenders covers both device-tree-present and absent
// branches. Origin-level — modifies Apple's own decision input, not MMIO. The
// rest of Apple's code paths run unchanged with seam joining naturally skipped.
//
// Gated on !isRealTGL since real TGL doesn't need this.
//
// Post-call still logs CRTCParams scaler fields to verify SEAM_EXCESS==0.
void Gen11::setupPipeScaler(AppleIntel::AppleIntelScaler *that, AppleIntel::AppleIntelDisplayPath *path, AppleIntel::CRTCParams *params)
{
	// ccont fixup (same as programPipeScaler) — needed because V204 init hooks
	// don't always populate ccont; original would crash on this=NULL ccont path.
	that->fWriteAccessor = ccont;


	// Pre-call snapshot — captures values BEFORE setupPipeScaler runs so we can
	// tell whether THIS function sets PIPE_SEAM_EXCESS=0x1 or whether something
	// upstream did. Also captures the gate bytes our zero-the-gate attempt targeted.
	uint32_t pre_seam = 0, pre_winsz = 0, pre_winpos = 0, pre_hphase = 0;
	uint8_t pre_gate1E3 = 0xFF, pre_gate1E5 = 0xFF;
	bool have_base = false;
	if (NGreen::callback != nullptr && !NGreen::callback->isRealTGL) {
		if (params != nullptr) {
			pre_seam   = params->PIPE_SEAM_EXCESS;
			pre_winsz  = params->PS_PS_WIN_SZ;
			pre_winpos = params->PS_PS_WIN_POS;
			pre_hphase = params->PS_HPHASE;
		}
		if (that != nullptr) {
			auto *base = reinterpret_cast<uint8_t *>(that->fController);
			if (base != nullptr) {
				have_base    = true;
				pre_gate1E3  = base[0x1E3];
				pre_gate1E5  = base[0x1E5];
				// Still try the gate clear — harmless if not the right gate.
				base[0x1E3]  = 0;
				base[0x1E5]  = 0;
			}
		}
	}

	FunctionCast(setupPipeScaler, callback->osetupPipeScaler)(that, path, params);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || params == nullptr)
		return;

	static int v400Count = 0;
	if (v400Count >= 12) return;
	++v400Count;

	// PIPE_SRCSZ per Intel spec: high 16 = horizontal-1, low 16 = vertical-1.
	const uint32_t src_w = ((params->PIPE_SRCSZ >> 16) & 0xFFFF) + 1;
	const uint32_t src_h = (params->PIPE_SRCSZ & 0xFFFF) + 1;
	SYSLOG("ngreen", "V400[%d]: setupPipeScaler %s gates[+0x1E3,+0x1E5]=(0x%02x,0x%02x) "
		   "PRE: SEAM=0x%x WINSZ=0x%x WINPOS=0x%x HPHASE=0x%x | "
		   "POST: SRC=%ux%u SEAM=0x%x WINSZ=0x%x WINPOS=0x%x HPHASE=0x%x "
		   "HTOTAL=0x%x VTOTAL=0x%x TRANS_CONF=0x%x",
		   v400Count, have_base ? "base-ok" : "NO-base",
		   pre_gate1E3, pre_gate1E5,
		   pre_seam, pre_winsz, pre_winpos, pre_hphase,
		   src_w, src_h, params->PIPE_SEAM_EXCESS, params->PS_PS_WIN_SZ, params->PS_PS_WIN_POS, params->PS_HPHASE,
		   params->TRANS_HTOTAL, params->TRANS_VTOTAL, params->TRANS_CONF);
}

// V401: AppleIntelBaseController::paramsSurfCompare — READ-ONLY logger.
// Fires per flip. Apple uses the return value to decide whether to fully reprogram
// the plane. We just log the inputs.
bool Gen11::paramsSurfCompare(AppleIntel::AppleIntelBaseController *that,
                              AppleIntel::CRTCParams *p1, AppleIntel::CRTCParams *p2,
                              AppleIntel::PLANEPARAMS *pl1, AppleIntel::PLANEPARAMS *pl2)
{
	// V408: force linear tiling in the NEW (pl2) PLANEPARAMS before the comparison.
	// hwRegsNeedUpdate has no PLANEPARAMS argument — it cannot fix tiling there.
	// paramsSurfCompare is the last point where pl2 can be patched before its values
	// trigger raWriteRegister32 writes for PLANE_CTL and PLANE_STRIDE.
	// Old (pl1/hardware) is already linear from UEFI; patching pl2 to match suppresses
	// the X-tiled commit without any MMIO intercept.
	/*if (NGreen::callback && !NGreen::callback->isRealTGL && pl2) {
		pl2->PLANE_CTL    = (pl2->PLANE_CTL & ~(0x7u << 10));  // bits[12:10] = 000 → linear
		pl2->PLANE_STRIDE = 0xa0;                               // 2560×4 / 64 = 160 = 0xa0
	}*/

	// Capture Apple's natural values BEFORE any force, for V401 logging.
	uint32_t nat_ctl_pl1    = pl1 ? pl1->PLANE_CTL    : 0;
	uint32_t nat_stride_pl1 = pl1 ? pl1->PLANE_STRIDE : 0;
	uint32_t nat_surf_pl1   = pl1 ? pl1->PLANE_SURF   : 0;
	uint32_t nat_ctl_pl2    = pl2 ? pl2->PLANE_CTL    : 0;
	uint32_t nat_stride_pl2 = pl2 ? pl2->PLANE_STRIDE : 0;
	uint32_t nat_surf_pl2   = pl2 ? pl2->PLANE_SURF   : 0;

	if (!NGreen::callback->isRealTGL && pl2 && !xeFixEnabled()) {
		// xe/xtile (-ngreenxtile): keep Apple's native X-tiled PLANE_CTL (V401 logs: bits[12:10]=001)
		// and use the X-tile stride (pitch / 512 B). 2560 px -> 0x14, 3840 px -> 0x1e.
		uint32_t srcW = p2 ? (((p2->PIPE_SRCSZ >> 16) & 0xFFFF) + 1) : 0;
		if (checkKernelArgument("-ngreenxtile")) {
			pl2->PLANE_STRIDE = (srcW >= 640 && srcW <= 8192) ? (srcW * 4 / 512) : 0x14;
		} else {
			pl2->PLANE_CTL    = (pl2->PLANE_CTL & ~(0x7u << 10));  // bits[12:10] = 000 → linear
			// xe/nooted: linear stride (64-byte units) from the real pipe width; the original
			// hardcoded 0xa0 only fits a 2560-px-wide panel (3840 px needs 0xf0).
			pl2->PLANE_STRIDE = (srcW >= 640 && srcW <= 8192) ? (srcW * 4 / 64) : 0xa0;
			// xe/stride (-ngreenstride=N): sobrescreve o stride linear, para medir o efeito na largura das bandas.
			uint32_t strideOv = 0;
			if (PE_parse_boot_argn("ngreenstride", &strideOv, sizeof(strideOv)) && strideOv)
				pl2->PLANE_STRIDE = strideOv;
		}
	}

	bool ret = FunctionCast(paramsSurfCompare, callback->oparamsSurfCompare)(that, p1, p2, pl1, pl2);

	SYSLOG("ngreen", "V401, original ret [%d]", ret);
	// ADL-P: the ICL/TGL driver's SURF-only fast-path (used when paramsSurfCompare returns
	// false) does not work correctly under this spoof — PLANE_SURF never gets written on
	// subsequent flips.  Force a full reprogram whenever the surface address actually changes.
	if (!NGreen::callback->isRealTGL && !ret && pl1 && pl2) {
		if (pl1->PLANE_SURF != pl2->PLANE_SURF)
			ret = true;
	}

	// Real fix for seam scaler 2: Apple reads PIPE_SEAM_EXCESS and PS_PS_WIN_SZ from p2
	// AFTER paramsSurfCompare returns, to fill the GPU DSB buffer entry for PS_WIN_SZ_2_A
	// (0x68274). On a single-pipe ADL-P panel there is no seam scaler 2; if the DSB gets
	// 0x3fff04f there, the pipeline aborts the flip immediately (param2=0 at V405).
	// Zeroing here prevents any non-zero seam value from reaching the DSB.
	if (!NGreen::callback->isRealTGL && p2 != nullptr) {
		p2->PIPE_SEAM_EXCESS = 0;
		p2->PS_PS_WIN_SZ     = 0;
	}

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL) return ret;

	static int v401Count = 0;
	if (v401Count >= 20) return ret;
	++v401Count;

	uint32_t old_ctl    = pl1 ? pl1->PLANE_CTL    : 0;
	uint32_t new_ctl    = pl2 ? pl2->PLANE_CTL    : 0;
	uint32_t old_stride = pl1 ? pl1->PLANE_STRIDE : 0;
	uint32_t new_stride = pl2 ? pl2->PLANE_STRIDE : 0;
	uint32_t old_surf   = pl1 ? pl1->PLANE_SURF   : 0;
	uint32_t new_surf   = pl2 ? pl2->PLANE_SURF   : 0;
	uint32_t old_src    = p1  ? p1->PIPE_SRCSZ    : 0;
	uint32_t new_src    = p2  ? p2->PIPE_SRCSZ    : 0;

	// PLANE_CTL tiling is bits[12:10]: 000=linear, 001=X-tile, 100=Tile4.
	uint32_t old_tile     = (old_ctl     >> 10) & 0x7;
	uint32_t new_tile     = (new_ctl     >> 10) & 0x7;
	uint32_t nat_tile_pl1 = (nat_ctl_pl1 >> 10) & 0x7;
	uint32_t nat_tile_pl2 = (nat_ctl_pl2 >> 10) & 0x7;

	SYSLOG("ngreen", "V401[%d]: paramsSurfCompare ret=%d | "
		   "OLD: CTL=0x%x tile=%u STRIDE=0x%x SURF=0x%x SRC=0x%x | "
		   "NEW: CTL=0x%x tile=%u STRIDE=0x%x SURF=0x%x SRC=0x%x | "
		   "NAT plold: tile_pl1=%u stride_pl1=0x%x surf_pl1=0x%x | "
		   "NAT: plnew tile_pl2=%u stride_pl2=0x%x surf_pl2=0x%x",
		   v401Count, ret,
		   old_ctl, old_tile, old_stride, old_surf, old_src,
		   new_ctl, new_tile, new_stride, new_surf, new_src,
		   nat_tile_pl1, nat_tile_pl2, nat_stride_pl1, nat_surf_pl1,
		   nat_tile_pl2, nat_stride_pl2, nat_surf_pl2);

	return ret;
}

// V402: AppleIntelBaseController::setupDSCEngineParams — READ-ONLY logger.
// Logs entry of DSC config path. Linux says DSC=off on our panel; if Apple still
// goes through this with non-zero DSC bits, the call is the origin of any DSC
// corruption and disabling it here would replace V300's CRTCParams write-back.
void Gen11::setupDSCEngineParams(AppleIntel::AppleIntelBaseController *that,
                                 AppleIntel::AppleIntelFramebuffer *fb,
                                 AppleIntel::CRTCParams *params,
								 AppleIntel::AppleIntelDisplayPath *path,
                                 IODetailedTimingInformationV2 *timing)
{
	// Pre-call snapshot: did anyone set DSC fields before us?
	uint32_t pre_dsc_engine = 0, pre_dsc_joiner = 0, pre_pps0 = 0;
	if (NGreen::callback != nullptr && !NGreen::callback->isRealTGL && params != nullptr) {
		pre_dsc_engine = params->DSC_ENGINE_SEL;
		pre_dsc_joiner = params->DSC_JOINER_CTL;
		pre_pps0       = params->PPS_0;
	}

	FunctionCast(setupDSCEngineParams, callback->osetupDSCEngineParams)(that, fb, params, path, timing);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || params == nullptr) return;

	static int v402Count = 0;
	if (v402Count >= 6) return;
	++v402Count;

	const bool dsc_was_enabled = (params->DSC_ENGINE_SEL & 0xF0000000u) != 0;
	SYSLOG("ngreen", "V402[%d]: setupDSCEngineParams %s | "
		   "PRE: DSC_ENGINE=0x%x DSC_JOINER=0x%x PPS_0=0x%x | "
		   "POST: DSC_ENGINE=0x%x DSC_JOINER=0x%x PPS_0=0x%x PPS_16=0x%x",
		   v402Count,
		   dsc_was_enabled ? "*** DSC ENABLED (vs Linux=off) ***" : "(DSC stayed off)",
		   pre_dsc_engine, pre_dsc_joiner, pre_pps0,
		   params->DSC_ENGINE_SEL, params->DSC_JOINER_CTL, params->PPS_0, params->PPS_16);
}

// V403: AppleIntelBaseController::SetupParams — post-call.
// Logs full CRTCParams + causation test: force PIPE_SEAM_EXCESS=0 and PS_PS_WIN_SZ=0
// so any seam-join config Apple set up earlier in the modeset is wiped after the
// master builder finishes. If fragmentation disappears with these zeroed, seam was
// the cause. If unchanged, seam fields are irrelevant and we look elsewhere.
void Gen11::setupParams(AppleIntel::AppleIntelBaseController *that,
                        AppleIntel::AppleIntelFramebuffer *fb,
						AppleIntel::AppleIntelDisplayPath *path,
                        AppleIntel::CRTCParams *params,
                        const IODetailedTimingInformationV2 *timing)
{
	FunctionCast(setupParams, callback->osetupParams)(that, fb, path, params, timing);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || params == nullptr) return;

	// V403-zero: causation test. Wipe seam-join CRTCParams fields BEFORE any
	// post-SetupParams consumer reads them (paramsSurfCompare, MMIO emit etc).
	uint32_t pre_seam  = params->PIPE_SEAM_EXCESS;
	uint32_t pre_winsz = params->PS_PS_WIN_SZ;
	uint32_t pre_hphase = params->PS_HPHASE;
	if (pre_seam != 0 || pre_winsz != 0 || pre_hphase != 0) {
		params->PIPE_SEAM_EXCESS = 0;
		params->PS_PS_WIN_SZ     = 0;
		params->PS_HPHASE        = 0;
	}

	static int v403Count = 0;
	if (v403Count >= 6) return;
	++v403Count;

	SYSLOG("ngreen", "V403[%d]: SetupParams post-call CRTCParams: "
		   "CLK_SEL=0x%x DDI_FUNC_CTL=0x%x DDI_FUNC_CTL2=0x%x MSA_MISC=0x%x "
		   "HTOTAL=0x%x HBLANK=0x%x HSYNC=0x%x VTOTAL=0x%x VBLANK=0x%x VSYNC=0x%x "
		   "PIPE_SRCSZ=0x%x TRANS_CONF=0x%x | "
		   "PS_WIN_POS=0x%x | PRE: SEAM=0x%x WINSZ=0x%x HPHASE=0x%x -> NOW 0 | "
		   "DSC_ENGINE=0x%x DSC_JOINER=0x%x PPS_0=0x%x PPS_16=0x%x",
		   v403Count,
		   params->TRANS_CLK_SEL, params->TRANS_DDI_FUNC_CTL, params->TRANS_DDI_FUNC_CTL2, params->TRANS_MSA_MISC,
		   params->TRANS_HTOTAL, params->TRANS_HBLANK, params->TRANS_HSYNC,
		   params->TRANS_VTOTAL, params->TRANS_VBLANK, params->TRANS_VSYNC,
		   params->PIPE_SRCSZ, params->TRANS_CONF,
		   params->PS_PS_WIN_POS,
		   pre_seam, pre_winsz, pre_hphase,
		   params->DSC_ENGINE_SEL, params->DSC_JOINER_CTL, params->PPS_0, params->PPS_16);
}

// V404: AppleIntelBaseController::setupPipeWatermarks — READ-ONLY pre/post.
// Tells us if setupPipeWatermarks is what sets PIPE_SEAM_EXCESS=0x1. Called
// from inside SetupParams before setupPipeScaler — if pre=0 post=1 here, this
// is our seam-join origin (rather than setupPipeScaler).
void Gen11::setupPipeWatermarks(AppleIntel::AppleIntelBaseController *that,
                                AppleIntel::AppleIntelFramebuffer *fb,
								AppleIntel::AppleIntelDisplayPath *path,
                                AppleIntel::CRTCParams *params)
{
	uint32_t pre_seam = 0, pre_winsz = 0, pre_winpos = 0;
	if (NGreen::callback != nullptr && !NGreen::callback->isRealTGL && params != nullptr) {
		pre_seam   = params->PIPE_SEAM_EXCESS;
		pre_winsz  = params->PS_PS_WIN_SZ;
		pre_winpos = params->PS_PS_WIN_POS;
	}

	FunctionCast(setupPipeWatermarks, callback->osetupPipeWatermarks)(that, fb, path, params);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || params == nullptr) return;

	static int v404Count = 0;
	if (v404Count >= 4) return;
	++v404Count;

	SYSLOG("ngreen", "V404[%d]: setupPipeWatermarks | "
		   "PRE: SEAM=0x%x WINSZ=0x%x WINPOS=0x%x | "
		   "POST: SEAM=0x%x WINSZ=0x%x WINPOS=0x%x %s",
		   v404Count,
		   pre_seam, pre_winsz, pre_winpos,
		   params->PIPE_SEAM_EXCESS, params->PS_PS_WIN_SZ, params->PS_PS_WIN_POS,
		   (pre_seam == 0 && params->PIPE_SEAM_EXCESS != 0)
			   ? "*** SEAM SET HERE ***"
			   : (pre_seam != 0 ? "(seam pre-existed)" : "(no seam)"));
}

// V405: AppleIntelPlane::configureColorPipeLine(FlipTransactionArgs*, bool)
//
// Dispatches to configurePipePostCSCGamma_{8,10,12,12SEG}Bit based on a BPC/gamma
// mode selector field within FlipTransactionArgs.  The field is at byte offset 0x1C
// (IDA's "param_1[7].Tiling" — element 7 of a dword array inside the struct).
//
// For our 2560×1600 eDP at 8bpc this always hits case 0 → 8Bit, which writes:
//   GAMMA_MODE   (0x4A480, Pipe A) = 0x0  (LEGACY_8BIT)
//   PIPE_MISC    (0x70030, Pipe A) bits[5:3] = 0b000 (8 bpc)
//
// Display 13 (ADL-P) uses the same register addresses as Display 12 (TGL), so
// Apple's writes are structurally correct.  The hook logs pre/post snapshots so
// we can confirm the hardware actually sees the right values each flip cycle.
void Gen11::configureColorPipeLine(AppleIntel::AppleIntelPlane *that, AppleIntel::FlipTransactionArgs *flipArgs, bool param_2)
{
	static int v405Count = 0;

	uint32_t pre_gamma = 0, pre_misc = 0;
	uint8_t  sel = 0xFF;

	if (NGreen::callback != nullptr && !NGreen::callback->isRealTGL) {
		pre_gamma = NGreen::callback->readReg32(0x4A480);  // GAMMA_MODE Pipe A
		pre_misc  = NGreen::callback->readReg32(0x70030);  // PIPE_MISC  Pipe A
		if (flipArgs != nullptr)
			sel = static_cast<uint8_t>(*reinterpret_cast<const uint32_t *>(&flipArgs->flt_001C) >> 24);

		// DIAG: log the color pipeline bitmask (FB+0x4248) to identify which bit drives PIPE_MISC bit 23.
		// The bitmask at FB+0x4248 (FB = *(that+0x68)) selects which sub-functions run inside
		// configureColorPipeLine. Bit 10 = configurePipeHDRMode candidate. Log pre/post to verify.
		if (that != nullptr) {
			void *fb = getMember<void *>(that, 0x68);
			if (fb != nullptr) {
				uint32_t bm_before = getMember<uint32_t>(fb, 0x4248);
				getMember<uint32_t>(fb, 0x4248) &= ~(1u << 10);
				uint32_t bm_after  = getMember<uint32_t>(fb, 0x4248);
				SYSLOG("ngreen", "DIAG[BM]: fb=%p bitmask 0x%08x→0x%08x param2=%d",
				       fb, bm_before, bm_after, (int)param_2);
			}
		}
	}

	FunctionCast(configureColorPipeLine, callback->oConfigureColorPipeLine)(that, flipArgs, param_2);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL) return;

	if (v405Count >= 20) return;
	++v405Count;

	const uint32_t post_gamma = NGreen::callback->readReg32(0x4A480);
	const uint32_t post_misc  = NGreen::callback->readReg32(0x70030);
	// PIPE_MISC BPC field: bits[5:3].  0=8bpc, 1=10bpc, 2=6bpc, 3=12bpc.
	const uint8_t bpc_pre  = (pre_misc  >> 3) & 0x7u;
	const uint8_t bpc_post = (post_misc >> 3) & 0x7u;
	// GAMMA_MODE encoding: 0=legacy8bit, 1=prec10bit, 2=prec12interp, 3=split
	const char *gammaName[] = {"LEGACY_8BIT", "PREC_10BIT", "PREC_12INTERP", "SPLIT"};
	const char *gname = (post_gamma <= 3) ? gammaName[post_gamma] : "unknown";

	SYSLOG("ngreen", "V405[%d]: configureColorPipeLine sel=0x%02x param2=%d | "
		   "GAMMA_MODE: 0x%x→0x%x(%s) | PIPE_MISC: 0x%x→0x%x bpc[5:3]=%u→%u",
		   v405Count, sel, (int)param_2,
		   pre_gamma, post_gamma, gname,
		   pre_misc, post_misc, bpc_pre, bpc_post);
}

// V406: CORE origin-level plane tiling/stride manipulation for ADL-P/RPL spoofed as TGL.
// Physical framebuffer pages are CPU-written in linear order. Apple's IOSurface allocates
// with tiling enum=0 (X-tiled) → configurePlane ORs PLANE_CTL bit10 → display engine
// fetches X-tile geometry → black screen.
//
// Linux i915 on this exact ADL-P hardware: modifier=0x0 (LINEAR), stride=0x2800 bytes (0x14 units)
//
// FlipTransactionArgs+0x3c tiling enum (from disasm VA 0x59ad-0x59c3):
//   0x0 → X-tiled:  ORs 0x400 into PLANE_CTL bits[12:10] = 001
//   0x1 → Y-tiled:  ORs 0x1000 into bits[12:10] = 100
//   else → linear:  ANDs 0xffe7e7ff (clears bits[12:10] = 000)
//
// AppleIntelPlane shadow offsets (from configurePlane disasm):
//   +0x100 = PLANE_CTL shadow (tiling bits built here, read @ VA 0x5a41)
//   +0x104 = PLANE_STRIDE shadow (read @ VA 0x6867)
//   +0x154 = PLANE_COLOR_CTL shadow (post-OR write @ VA 0x6687)
//
// ============================================================================
// === CHOOSE ONE: Main approach (uncomment your choice below) ===
// ============================================================================

// ✓ APPROACH 1: PRE-CALL TILING PATCH (origin-level, Apple's natural path)
#define V406_APPROACH_TILING_PATCH 1

// Alternative approaches below (comment out APPROACH 1 if trying these):
// #define V406_APPROACH_POST_SHADOW_PATCH 1      // Post-call shadow field write
// #define V406_APPROACH_STRIDE_DIVISOR 1          // Stride divisor manipulation
// #define V406_APPROACH_DUAL_TILING_STRIDE 1      // Tiling + stride combined
// #define V406_APPROACH_DIAGNOSTIC_ONLY 1         // Pass-through with logging

// ============================================================================
// === TILING ENUM SELECTOR (active for approaches using pre-call patch) ===
// ============================================================================

// #define V406_TILING_VALUE 2       // linear — CPU BAR2 compositor path
// #define V406_TILING_VALUE 1       // Y-tiled
#define V406_TILING_VALUE 0          // X-tiled — matches Apple IOSurface allocator

// ============================================================================
// === STRIDE FORCE (active for stride-manipulation approaches) ===
// ============================================================================

#define V406_STRIDE_FORCE_ENABLE 0              // stride handled by paramsSurfCompare V408
#define V406_STRIDE_VALUE 0x14                  // X-tile: 2560×4 / 512 = 20 = 0x14

// ============================================================================
// === PLANE_CTL BITS MANIPULATION (advanced—leave as 0 unless testing) ===
// ============================================================================

#define V406_PLANE_CTL_CLEAR_MASK 0x0           // Bits to clear (0xFFE7E7FF = tiling bits)
#define V406_PLANE_CTL_SET_MASK 0x0             // Bits to set (0x400=X-tiled, 0x1000=Y-tiled)

// ============================================================================
// === SHADOW FIELD OFFSET OVERRIDES (for post-call patching) ===
// ============================================================================

#define V406_STRIDE_SHADOW_OFFSET 0x104         // AppleIntelPlane+0x104 = PLANE_STRIDE
#define V406_CTL_SHADOW_OFFSET 0x100            // AppleIntelPlane+0x100 = PLANE_CTL

// ============================================================================

void Gen11::configurePlane(AppleIntel::AppleIntelPlane *that, AppleIntel::FlipTransactionArgs *flipArgs)
{
	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || flipArgs == nullptr) {
		FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);
		return;
	}

	// Pre-call snapshot
	uint32_t incomingTiling = 0;
	if (flipArgs != nullptr)
		incomingTiling = flipArgs->TilingEnum;

	static int v406CallCount = 0;
	static int v406LogCount = 0;
	++v406CallCount;

#ifdef V406_APPROACH_TILING_PATCH
	// ===== APPROACH 1: PRE-CALL TILING PATCH =====
	// Patch FlipTransactionArgs+0x3c before calling original
	// Apple's configurePlane reads this, takes the corresponding disasm branch,
	// builds PLANE_CTL with correct tiling bits natively
	
	uint32_t *tilingField = &flipArgs->TilingEnum;
	const uint32_t savedTiling = *tilingField;
	*tilingField = V406_TILING_VALUE;

	if (v406LogCount < 20) {
		++v406LogCount;
		SYSLOG("ngreen", "V406[%d]: TILING_PATCH call#%d | "
			   "Incoming tiling=0x%x → Patched=0x%x (%s)",
			   v406LogCount, v406CallCount, incomingTiling, V406_TILING_VALUE,
			   (V406_TILING_VALUE == 0) ? "X-tiled" :
			   (V406_TILING_VALUE == 1) ? "Y-tiled" : "linear/else");
	}

	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	*tilingField = savedTiling;   // restore

#elif defined(V406_APPROACH_POST_SHADOW_PATCH)
	// ===== APPROACH 2: POST-CALL SHADOW PATCH =====
	// Call original, then modify AppleIntelPlane shadow fields directly
	
	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	if (that != nullptr) {
		// Patch PLANE_CTL shadow at +0x100 (or custom offset)
		uint32_t &ctlShadow = getMember<uint32_t>(that, V406_CTL_SHADOW_OFFSET);
		const uint32_t ctlBefore = ctlShadow;
		
		// Clear tiling bits if needed
		if (V406_PLANE_CTL_CLEAR_MASK != 0)
			ctlShadow &= ~V406_PLANE_CTL_CLEAR_MASK;
		
		// Set specific bits if needed
		if (V406_PLANE_CTL_SET_MASK != 0)
			ctlShadow |= V406_PLANE_CTL_SET_MASK;
		
		if (v406LogCount < 20) {
			++v406LogCount;
			SYSLOG("ngreen", "V406[%d]: POST_SHADOW_PATCH call#%d | "
				   "PLANE_CTL shadow @+0x%x: 0x%x → 0x%x (clear=0x%x set=0x%x)",
				   v406LogCount, v406CallCount, V406_CTL_SHADOW_OFFSET,
				   ctlBefore, ctlShadow, V406_PLANE_CTL_CLEAR_MASK, V406_PLANE_CTL_SET_MASK);
		}

		// Optionally patch STRIDE shadow
		if (V406_STRIDE_FORCE_ENABLE) {
			uint32_t &strideShadow = getMember<uint32_t>(that, V406_STRIDE_SHADOW_OFFSET);
			const uint32_t strideBefore = strideShadow;
			strideShadow = V406_STRIDE_VALUE;
			if (v406LogCount < 20) {
				SYSLOG("ngreen", "V406[%d]: STRIDE shadow @+0x%x: 0x%x → 0x%x",
					   v406LogCount, V406_STRIDE_SHADOW_OFFSET, strideBefore, V406_STRIDE_VALUE);
			}
		}
	}

#elif defined(V406_APPROACH_STRIDE_DIVISOR)
	// ===== APPROACH 3: STRIDE DIVISOR (experimental) =====
	// Patch stride divisor used in configurePlane's pitch/divisor calculation
	// WARNING: This requires knowing the exact location where stride divisor is stored
	// (likely in a local stack variable or controller object). This is highly experimental.
	
	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	if (v406LogCount < 20) {
		++v406LogCount;
		SYSLOG("ngreen", "V406[%d]: STRIDE_DIVISOR call#%d | "
			   "Attempting stride divisor override (experimental) | "
			   "Incoming tiling=0x%x",
			   v406LogCount, v406CallCount, incomingTiling);
	}

	// NOTE: Actual stride divisor patching would require identifying the controller
	// field or register that holds 0x200 (stride divisor for X-tiled).
	// This approach is a placeholder—override if you find the divisor location.

#elif defined(V406_APPROACH_DUAL_TILING_STRIDE)
	// ===== APPROACH 4: DUAL TILING + STRIDE =====
	// Combine pre-call tiling patch + post-call stride shadow patch
	
	uint32_t *tilingField = &flipArgs->TilingEnum;
	const uint32_t savedTiling = *tilingField;
	*tilingField = V406_TILING_VALUE;

	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	*tilingField = savedTiling;

	// Post-call: patch STRIDE shadow
	if (V406_STRIDE_FORCE_ENABLE && that != nullptr) {
		uint32_t &strideShadow = getMember<uint32_t>(that, V406_STRIDE_SHADOW_OFFSET);
		const uint32_t strideBefore = strideShadow;
		strideShadow = V406_STRIDE_VALUE;
		
		if (v406LogCount < 20) {
			++v406LogCount;
			SYSLOG("ngreen", "V406[%d]: DUAL_TILING_STRIDE call#%d | "
				   "Tiling=0x%x STRIDE shadow: 0x%x → 0x%x",
				   v406LogCount, v406CallCount, V406_TILING_VALUE,
				   strideBefore, V406_STRIDE_VALUE);
		}
	}

#else
	// ===== APPROACH 5: DIAGNOSTIC PASS-THROUGH =====
	// No changes—pure logging to see original behavior
	
	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	if (v406LogCount < 20) {
		++v406LogCount;
		SYSLOG("ngreen", "V406[%d]: DIAGNOSTIC_ONLY call#%d | "
			   "Incoming tiling=0x%x (unchanged)",
			   v406LogCount, v406CallCount, incomingTiling);
	}
#endif

	// Always log final state if STRIDE monitoring enabled
	if (V406_STRIDE_FORCE_ENABLE && that != nullptr && v406LogCount < 20) {
		uint32_t ctlFinal = getMember<uint32_t>(that, V406_CTL_SHADOW_OFFSET);
		uint32_t strideFinal = getMember<uint32_t>(that, V406_STRIDE_SHADOW_OFFSET);
		SYSLOG("ngreen", "V406-final[%d]: CTL@+0x%x=0x%x STRIDE@+0x%x=0x%x",
			   v406LogCount, V406_CTL_SHADOW_OFFSET, ctlFinal,
			   V406_STRIDE_SHADOW_OFFSET, strideFinal);
	}
}

void Gen11::AppleIntelPlaneupdateRegisterCache(AppleIntel::AppleIntelPlane *that)
{
	getMember<void *>(that, 0x90) = ccont; // fWriteAccessor — ccont must NOT go to real fRegCache at +0x88
	FunctionCast(AppleIntelPlaneupdateRegisterCache, callback->oAppleIntelPlaneupdateRegisterCache)(that);
}

void Gen11::AppleIntelScalerupdateRegisterCache(AppleIntel::AppleIntelScaler *that)
{
	that->fWriteAccessor = ccont;
	FunctionCast(AppleIntelScalerupdateRegisterCache, callback->oAppleIntelScalerupdateRegisterCache)(that);
}

void Gen11::disableDisplayEngine(AppleIntel::AppleIntelBaseController *that)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(disableDisplayEngine, callback->odisableDisplayEngine)(that );
}

void Gen11::enableDisplayEngine(AppleIntel::AppleIntelBaseController *that)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(enableDisplayEngine, callback->oenableDisplayEngine)(that );
}

void Gen11::computeLaneCount(AppleIntel::AppleIntelBaseController *that, const IODetailedTimingInformationV2 *timing, unsigned int linkRate, unsigned int bpp, unsigned int *laneCount) {
	if (!laneCount) return;

	// Call original first — handles all standard DP rates on both real TGL and spoofed paths.
	FunctionCast(computeLaneCount, callback->ocomputeLaneCount)(that, timing, linkRate, bpp, laneCount);

	// Real TGL: preserve Apple's result unchanged.
	if (NGreen::callback->isRealTGL)
		return;

	// For !isRealTGL (RPL spoofed): Apple's DPCD-based result (MAX_LANE_COUNT) reflects
	// the panel's capability, but UEFI/GOP may have trained the link at a different lane
	// count.  Read DDI_BUF_CTL_A to discover the actual HW-trained lane count and
	// override Apple's result if UEFI trained more lanes than Apple computed.
	//
	// DDI_BUF_CTL PORT_WIDTH field bits[3:1]:
	//   000 = x1 (1 lane),  001 = x2 (2 lanes),  011 = x4 (4 lanes)
	const uint32_t ddiA    = NGreen::callback->readReg32(0x64000);  // DDI_BUF_CTL_A
	const unsigned int width   = (ddiA >> 1) & 0x7u;
	const unsigned int hwLanes = (width >= 3) ? 4u : (width >= 1) ? 2u : 1u;

	static int v90L4Logs = 0;
	if (v90L4Logs < 10) {
		v90L4Logs++;
		SYSLOG("ngreen", "V90L4[%d]: linkRate=%u bpp=%u appleLC=%u DDI_BUF_CTL_A=0x%x hwLanes=%u",
			   v90L4Logs, linkRate, bpp, *laneCount, ddiA, hwLanes);
	}

	if (hwLanes > *laneCount)
		*laneCount = hwLanes;
}

// setupOptimalLaneCount is called by hwSetMode to pick the lane count that gets
// stored in the port's cached LinkConfig (and ultimately into TRANS_DDI_FUNC_CTL).
// Apple's implementation runs: optimal = computeLaneCount(...); then caps it to
// port->maxLaneCount (from DPCD MAX_LANE_COUNT, which is 2 on this panel).
// On !isRealTGL we instead snap the cached count to match DDI_BUF_CTL_A so that
// SetupParams builds TRANS_DDI_FUNC_CTL with the correct HW-trained lane field.
void Gen11::setupOptimalLaneCount(AppleIntel::AppleIntelBaseController *that, const IODetailedTimingInformationV2 *timing, unsigned int bpp) {
	// Always run Apple's original first to populate all other LinkConfig fields.
	FunctionCast(setupOptimalLaneCount, callback->osetupOptimalLaneCount)(that, timing, bpp);

	if (NGreen::callback->isRealTGL)
		return;

	// Read HW-trained lane count from DDI_BUF_CTL_A bits[3:1].
	// PORT_WIDTH: 0=x1, 1=x2, 3=x4.
	const uint32_t ddiA    = NGreen::callback->readReg32(0x64000);
	const unsigned int width   = (ddiA >> 1) & 0x7u;
	const unsigned int hwLanes = (width >= 3) ? 4u : (width >= 1) ? 2u : 1u;

	// The port object stores the cached optimal lane count at a known offset.
	// AppleIntelPort::setupOptimalLaneCount writes fOptimalLaneCount (confirmed
	// by IDA: str result into [x0 + offset] before returning).
	// We patch it post-call so the cap-to-DPCD logic is overridden.
	// Offset 0x148 is fOptimalLaneCount in AppleIntelPort on this kext version.
	unsigned int &cached = getMember<unsigned int>(that, 0x148);

	static int v90L5Logs = 0;
	if (v90L5Logs < 10) {
		v90L5Logs++;
		SYSLOG("ngreen", "V90L5[%d]: setupOptimalLC: was=%u DDI_BUF_CTL_A=0x%x hwLanes=%u",
			   v90L5Logs, cached, ddiA, hwLanes);
	}

	if (hwLanes > cached)
		cached = hwLanes;
}

IOReturn Gen11::wrapICLReadAUX(void *that, uint32_t address, void *buffer, uint32_t length) {

	IOReturn retVal = FunctionCast(wrapICLReadAUX, callback->orgICLReadAUX)(that, address, buffer, length);

	// V97AUX: log first ~40 AUX reads to diagnose eDP link training failures.
	static int auxLogCount = 0;
	if (auxLogCount < 40) {
		auxLogCount++;
		uint8_t *b = reinterpret_cast<uint8_t *>(buffer);
		if (length >= 2)
			SYSLOG("ngreen", "V97AUX[%d]: addr=0x%04x len=%u ret=0x%x [0]=0x%02x [1]=0x%02x",
				   auxLogCount, address, length, retVal, b ? b[0] : 0xFF, (b && length >= 2) ? b[1] : 0xFF);
		else
			SYSLOG("ngreen", "V97AUX[%d]: addr=0x%04x len=%u ret=0x%x",
				   auxLogCount, address, length, retVal);
	}

	// V98T removed: do NOT clamp DPCD[0x0100-0x0101] (LINK_BW_SET / LANE_COUNT_SET).
	// Capping these to HBR2/2-lanes caused Apple to train the link at HBR2×2 lanes.
	// V97P then wrote a 4-lane DDI_FUNC_CTL value to a 2-lane trained link → black screen.
	// UEFI already trained the link at HBR3×4 lanes; we must let Apple see those values
	// so it (re-)trains consistently and V97P's bit16-only correction stays coherent.
	if (NGreen::callback && !NGreen::callback->isRealTGL && address == 0x0100 && buffer && length >= 1) {
		auto *raw = reinterpret_cast<uint8_t *>(buffer);
		static int v98tLogs = 0;
		if (v98tLogs < 5) {
			v98tLogs++;
			if (length >= 2)
				SYSLOG("ngreen", "V98T[%d]: DPCD 0x0100 passthrough bw=0x%02x lanes=0x%02x",
					   v98tLogs, raw[0], raw[1]);
			else
				SYSLOG("ngreen", "V98T[%d]: DPCD 0x0100 passthrough bw=0x%02x (len=1)",
					   v98tLogs, raw[0]);
		}
	}

	// V99: Suppress spurious LINK_STATUS_UPDATED (DPCD[0x204] bit7) on RPL-P.
	// HDCP probing reads DPCD 0x6921d, which causes the eDP panel to assert IRQ_HPD,
	// setting LINK_STATUS_UPDATED=1. Apple's checkLinkStatus then sees
	// INTERLANE_ALIGN_DONE=0 and tears down the display (~10s after boot).
	// The physical link is healthy; only the IRQ flag is spurious.
	// Clearing bit7 of DPCD[0x204] prevents the driver from acting on the IRQ.
	if (NGreen::callback && !NGreen::callback->isRealTGL && address == 0x0202 && buffer && length >= 3) {
		auto *raw = reinterpret_cast<uint8_t *>(buffer);
		if (raw[2] & 0x80) {
			static int v99Logs = 0;
			if (v99Logs < 10) {
				v99Logs++;
				SYSLOG("ngreen", "V99[%d]: suppressed DPCD 0x204 LINK_STATUS_UPDATED "
					   "(was 0x%02x, lanes=[0x%02x 0x%02x])",
					   v99Logs, raw[2], raw[0], raw[1]);
			}
			raw[2] &= ~0x80u; // clear LINK_STATUS_UPDATED
		}
	}

	if (address != 0x0000 && address != 0x2200) return retVal;

	if (length < sizeof(DPCDCap16) || buffer == nullptr)
		return retVal;

	auto caps = reinterpret_cast<DPCDCap16 *>(buffer);

	if (NGreen::callback && !NGreen::callback->isRealTGL) {
		// V98: Do NOT cap maxLaneCount or maxLinkRate.
		// Previous versions capped maxLaneCount to 2, which caused Apple to train at
		// 2 lanes while V97P subsequently wrote a 4-lane DDI_FUNC_CTL value → black screen.
		// The hardware is trained at HBR3×4 lanes by UEFI; let Apple see those real caps
		// so its LightUpEDP (re-)trains consistently at HBR3×4 lanes.
		static int v98Logs = 0;
		if (v98Logs < 5) {
			v98Logs++;
			SYSLOG("ngreen", "V98[%d]: DPCD caps @0x%04x maxLinkRate=0x%02x maxLane=0x%02x (passthrough)",
				   v98Logs, address, caps->maxLinkRate, caps->maxLaneCount);
		}
	}

	if (caps->revision < 0x03) {
		caps->maxLinkRate = 0;
	}

	return retVal;
}

void Gen11::getOnlineInfo(AppleIntel::AppleIntelFramebuffer *that, AppleIntel::AppleIntelDisplayPath *displayPath, unsigned char *online, unsigned char *changed) {
	// V96 removed (was: force *online=1 for fbId==0). Confirmed no-op on this hardware:
	// baseline log shows Apple's getOnlineInfo natively reports orig=1 for FB0, so the
	// V96 forcing was already redundant. Keeping the wrapper as a logging shell so we
	// can observe original online behavior across boots — if any orig!=1 case shows up
	// we'll know V96 was masking a real status bug, not just being redundant.
	FunctionCast(getOnlineInfo, callback->ogetOnlineInfo)(that, displayPath, online, changed);
	uint32_t fbId = getMember<uint32_t>(that, 0x1DC);
	unsigned char origOnline = online ? *online : 0xFF;
	static int v96PassLogs = 0;
	if (v96PassLogs < 12) {
		v96PassLogs++;
		SYSLOG("ngreen", "V96p: fb%u getOnlineInfo: orig=%d passthrough (V96 hack removed)",
			   fbId, origOnline);
	}
}

// Path B: hook AppleIntelFramebuffer::isApertureMemoryRequired().
// On real TGL: pass-through (preserve native behavior).
// On RPL/ADL with -ngreendp0: force return true so setupScanoutMemory never migrates
// from aperture → non-aperture. setupScanoutMemory's logic is:
//   if (isApertureMemoryRequired() && nonAperSurf!=0) → migrate-TO-aperture
//   else if (!isApertureMemoryRequired() && nonAperSurf==0) → migrate-FROM-aperture (the bad path)
//   else → no migration; if nonAperSurf==0 → "Using aperture memory"
// Forcing true with nonAperSurf==0 (the boot state) keeps us on the aperture path forever,
// matching what V99S+V99G already do at the hardware level — but cleanly, at the driver level,
// so WS sees a coherent fWSAAState→memory mapping and shouldn't degrade 0x3→0x1.
bool Gen11::wrapIsApertureMemoryRequired(AppleIntel::AppleIntelFramebuffer *that) {
	bool orig = FunctionCast(wrapIsApertureMemoryRequired, callback->oIsApertureMemoryRequired)(that);
	const bool isRealTGL = NGreen::callback && NGreen::callback->isRealTGL;

	// V205 freeze probe + continuous PSR1 disable. PSR1 at 0x60800 was found re-enabled
	// post-V105 (V105 was writing wrong register 0x64800). Even with the V105 fix, Apple
	// or DMC may re-arm PSR1 later — keep stomping it on every call.
	if (NGreen::callback) {
		static uint32_t v205Calls = 0;
		v205Calls++;

		// V99Z dirty-rect test removed: wipe ran but visually no change because WS
		// rewrites the whole buffer every frame. The "frozen images" symptom is
		// actually the X-tile-as-linear scanout artifact appearing more pronounced
		// on low-frequency content (text, solid blocks) than high-frequency content
		// (wallpaper texture). Single root cause = scanout-vs-buffer tile mismatch.
		// (Wipe proved BAR2 writes reach scanout in earlier magenta test, but WS's
		// per-frame rewriting makes the wipe invisible.)

		// Continuous PSR1+PSR2 disable — every call. PSR enabled = panel refreshes from
		// its own cache and ignores new SURF arms → frozen frame even while pipe vsyncs.
		uint32_t psr1Now = NGreen::callback->readReg32(0x60800);
		if (psr1Now & 1) {
			NGreen::callback->writeReg32(0x60800, 0);
			static uint32_t v205PSR1Resets = 0;
			if (v205PSR1Resets < 8) {
				v205PSR1Resets++;
				SYSLOG("ngreen", "V205PSR1[%u]: re-disabled PSR1 (was 0x%x) at call=%u",
					   v205PSR1Resets, psr1Now, v205Calls);
			}
		}
		uint32_t psr2Now = NGreen::callback->readReg32(0x60A10);
		if (psr2Now & 1) {
			NGreen::callback->writeReg32(0x60A10, 0);
			static uint32_t v205PSR2Resets = 0;
			if (v205PSR2Resets < 8) {
				v205PSR2Resets++;
				SYSLOG("ngreen", "V205PSR2[%u]: re-disabled PSR2 (was 0x%x) at call=%u",
					   v205PSR2Resets, psr2Now, v205Calls);
			}
		}

		// Periodic state snapshot for freeze diagnosis. Reduced thresholds since
		// wrapIsApertureMemoryRequired only fires ~44 times per boot in FB-only mode.
		const bool sample = (v205Calls == 1  || v205Calls == 2  || v205Calls == 5  ||
							 v205Calls == 10 || v205Calls == 20 || v205Calls == 30 ||
							 v205Calls == 40 || v205Calls == 44);
		if (sample) {
			uint32_t frm   = NGreen::callback->readReg32(0x70040);
			uint32_t pstat = NGreen::callback->readReg32(0x70024);
			uint32_t pcfg  = NGreen::callback->readReg32(0x70008);
			uint32_t dcst  = NGreen::callback->readReg32(0x45504);
			// V211: also probe Plane 2 (overlay) and Plane 3 (sprite) on Pipe A.
			// The visible "frozen overlay over animating background" symptom suggests
			// these planes hold stale content because WS's dp0 path only writes Plane 1.
			uint32_t p1ctl = NGreen::callback->readReg32(0x70180);
			uint32_t p1surf = NGreen::callback->readReg32(0x7019C);
			uint32_t p1liv = NGreen::callback->readReg32(0x701AC);
			uint32_t p2ctl = NGreen::callback->readReg32(0x71180);
			uint32_t p2surf = NGreen::callback->readReg32(0x7119C);
			uint32_t p2liv = NGreen::callback->readReg32(0x711AC);
			uint32_t p3ctl = NGreen::callback->readReg32(0x72180);
			uint32_t p3surf = NGreen::callback->readReg32(0x7219C);
			uint32_t p3liv = NGreen::callback->readReg32(0x721AC);
			uint32_t curctl = NGreen::callback->readReg32(0x70080);
			uint32_t curbase = NGreen::callback->readReg32(0x70084);
			uint32_t curpos = NGreen::callback->readReg32(0x70088);
			SYSLOG("ngreen", "V205[c=%u]: FRM=%u STAT=%08x CONF=%08x DC=%08x | P1 CTL=%08x SURF=%08x LIVE=%08x | P2 CTL=%08x SURF=%08x LIVE=%08x | P3 CTL=%08x SURF=%08x LIVE=%08x | CUR CTL=%08x BASE=%08x POS=%08x | PSR1=%08x PSR2=%08x",
				   v205Calls, frm, pstat, pcfg, dcst,
				   p1ctl, p1surf, p1liv,
				   p2ctl, p2surf, p2liv,
				   p3ctl, p3surf, p3liv,
				   curctl, curbase, curpos,
				   NGreen::callback->readReg32(0x60800), NGreen::callback->readReg32(0x60A10));
		}
	}

	if (isRealTGL || !isDisplayPipeForceDisabled()) {
		return orig;
	}
	static int logCount = 0;
	if (logCount < 8 && orig != true) {
		logCount++;
		SYSLOG("ngreen", "PathB: isApertureMemoryRequired forced true (orig=%d) fb=%p", orig, that);
	}
	return true;
}

// Path C: hook AppleIntelFramebuffer::setAttribute(IOSelect, uintptr_t).
// We intercept exactly one path: kIOWindowServerActiveAttribute ('wsrv' = 0x77737276).
// When WindowServer writes 0x1 (degrade-from-active), coerce the value to 0x3 before
// calling the original — keeping the driver's fWSAAState pinned at the active value so
// it never takes the hwDeferFeatures / degraded path.  Real TGL is unaffected.
IOReturn Gen11::wrapSetAttribute(void *that, uint32_t attr, uintptr_t value) {
	const bool isRealTGL = NGreen::callback && NGreen::callback->isRealTGL;
	if (attr == 0x77737276u /* 'wsrv' */ && !isRealTGL && isDisplayPipeForceDisabled()) {
		static int logCount = 0;
		uintptr_t newValue = value;
		bool coerced = false;
		if ((value & 0xFFu) == 0x1u) {
			newValue = (value & ~uintptr_t(0xFF)) | 0x3u;
			coerced = true;
		}
		if (logCount < 16) {
			logCount++;
			SYSLOG("ngreen", "PathC: wsrv setAttribute fb=%p value=0x%llx%s",
				   that, (unsigned long long)value, coerced ? " → coerced 0x3" : "");
		}
		return FunctionCast(wrapSetAttribute, callback->oSetAttribute)(that, attr, newValue);
	}
	return FunctionCast(wrapSetAttribute, callback->oSetAttribute)(that, attr, value);
}

// V183: ADL-P/RPL write-only power well handler.
// HSW_PWR_WELL_CTL1 (0x45400): REQ bits = odd bits (mask 0xAA: bits 1,3,5,7,...);
//                               STATE bits = even bits (mask 0x55: bits 0,2,4,6,...).
// TGL original polls STATE bits after writing REQ — on ADL-P those ACKs
// never arrive within the 20-iteration timeout, spinning the CPU.
// Fix: write REQ bits directly to CTL1, skip polling entirely.
// Previously this wrote to CTL2 (0x45404) — wrong register. CTL2 controls DDI/AUX
// wells; PG display wells (PG1/PG2) are in CTL1. Writing to CTL2 left CTL1 at the
// DMC-reset value (0x405, no REQ bits) → hardware never enabled display power domains
// → vsync interrupts never delivered → GPU ring idle → framebuffer stayed black.
void Gen11::hwSetPowerWellStatePGE(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2)
{
	if (!NGreen::callback->isRealTGL) {
		uint32_t ctl1 = NGreen::callback->readReg32(0x45400);
		uint32_t newVal;
		if (param_1) {
			// Enable: clear all REQ bits then set the requested ones.
			newVal = (ctl1 & 0xFFFFFF55U) | (param_2 & 0xAAU);
		} else {
			// Disable: clear the requested REQ bits.
			newVal = ctl1 & ~(param_2 & 0xAAU);
		}
		SYSLOG("ngreen", "V183.PGE: en=%u mask=0x%x ctl1: 0x%x->0x%x",
			   (unsigned)param_1, param_2, ctl1, newVal);
		NGreen::callback->writeReg32(0x45400, newVal);
		return;
	}
	// Real TGL: use original with ccont fixup.
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStatePGE, callback->ohwSetPowerWellStatePGE)(that, param_1, param_2);
}

void Gen11::hwSetPowerWellStateAux(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStateAux, callback->ohwSetPowerWellStateAux)(that,param_1,param_2);
}

void Gen11::hwSetPowerWellStateDDI(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStateDDI, callback->ohwSetPowerWellStateDDI)(that,param_1,param_2);
}


// xe/xefix (-ngreenxefix, 2026-10-17): correção do display 13 (ADL-P/RPL-P) provada por CRC do plano:
//  - o FB da Apple é X-tiled; no display 13 superfícies tiled passam pela DPT -> desligar com CHICKEN_MISC_2
//    (0x42084) bit 30 (i915 intel_dpt_configure) para o plano X-tiled ler pela GGTT, como no TGL;
//  - o driver TGL desliga a fatia S1 do DBUF (DBUF_CTL_S1 0x44FE8); sem ela a leitura do plano vira ruído
//    -> manter o pedido de energia (bit 31) sempre ligado.
static bool xeFixEnabled() {
	static int v = -1;
	if (v < 0) v = checkKernelArgument("-ngreenxefix") ? 1 : 0;
	return v == 1;
}
// 20/10 (XE-NODPMS): a tela não acordava depois do repouso. Diagnóstico com XeShim (~/xe/tools/xe-sleep-diag.sh):
// ao dormir o FB TGL desliga pipe A (TRANSCONF 0x70008), TRANS_DDI_FUNC_CTL_A (0x60400), DDI_BUF_CTL_A (0x64000),
// o escalador (PS_CTRL_1A 0x68180), backlight e energia do painel; ao acordar religa painel e plano, mas NUNCA o
// caminho pipe/transcoder/DDI (no boot ele herda a tela acesa pelo firmware, -ngreenkeepefi, e não sabe treinar o
// link eDP no display 13). Aqui o caminho de vídeo do painel nunca é desmontado: escritas que limpam o bit de enable
// desses registradores (e do clock da DDI-A em DPCLKA_CFGCR0, e da energia do painel em PP_CONTROL) são trocadas pelo
// último valor ligado. A luz de fundo segue apagando (o painel fica escuro). -ngreendpms volta ao comportamento original.
// v2 (20/10 20h): a v1 tinha IOLog aqui e também segurava o clock da DDI-A (DPCLKA_CFGCR0) -> spinlock timeout e
// "lock recursively" no boot. v2: sem log, sem DPCLKA; só bloqueia depois de ver o registrador ligado.
static uint32_t xeNoDpms(uint32_t r, uint32_t val) {
	// 20/10 20h: DESLIGADO por padrão. Bloquear o desligamento do pipe também quebra as trocas de modo do FB (ele
	// desliga o pipe e espera o hardware confirmar com uma trava segurada -> NMIPI/spinlock timeout no boot).
	// Fica opt-in (-ngreennodpms) até o bloqueio ficar restrito ao caminho de repouso (hook de energia do FB).
	static int on = -1;
	if (on < 0) on = checkKernelArgument("-ngreennodpms") ? 1 : 0;
	if (!on) return val;
	static uint32_t lastOn[6];
	int idx; uint32_t bit;
	switch (r) {
		case 0x70008:  idx = 0; bit = 0x80000000u; break;   // TRANSCONF_A
		case 0x60400:  idx = 1; bit = 0x80000000u; break;   // TRANS_DDI_FUNC_CTL_A
		case 0x64000:  idx = 2; bit = 0x80000000u; break;   // DDI_BUF_CTL_A
		case 0x68180:  idx = 3; bit = 0x80000000u; break;   // PS_CTRL_1A
		case 0xC7204:  idx = 4; bit = 0x00000001u; break;   // PP_CONTROL: power on
		default: return val;
	}
	if (val & bit) { lastOn[idx] = val; return val; }
	if (!lastOn[idx]) return val;          // nunca vimos ligado: não inventa valor
	gXeDpmsBlocked++;   // sem IOLog: este filtro roda dentro de seções travadas do FB (log aqui = deadlock/spinlock timeout)
	return r == 0xC7204 ? (val | 1u) : lastOn[idx];   // painel: só força o power on, o resto (backlight) passa
}
// xe/bootbar (21/10). XE-BOOTDIAG (só log, limitado): anel com as escritas da Apple no plano A, FBC, PSR, PIPE_SRCSZ
// e DC_STATE_EN (sem log no caminho de escrita: pode ser contexto de interrupção), impresso com um retrato dos
// registradores e do console do kernel antes/depois do FBController::start, em cada setDisplayMode e em +10/+40 s.
// XE-BOOTHOLD: no boot o plano A varre o buffer linear 1024x768 da GOP em GGTT 0 (SURF=0). No 1º modo (~t=20.7 s) a
// Apple grava PLANE_CTL X-tiled, STRIDE 0x1e, SIZE/PIPESRC 3840x2400 mas mantém SURF=0 por ~1 s até ter o buffer
// próprio (SURF=0x45beb000 em t=21.69) -> a tela mostra o buffer de boot lido como X-tiled 3840x2400 (flash de lixo
// em colunas). Enquanto SURF ainda é 0 e a Apple pede CTL tiled, o plano fica DESLIGADO (tela preta); na 1ª SURF != 0
// v3 (bootbar4): espera a SURF voltar a um buffer já visto (anel do WindowServer deu a volta); timeout de 500 ms após a 1ª.
// Grava-se o CTL real antes da SURF (que arma os dois). -xenobb desliga.
static volatile SInt32 gXeBhDone = 0, gXeBhHolding = 0, gXeBhOn = -1;
static volatile UInt32 gXeBhCtl = 0, gXeBhFirst = 0;
static volatile UInt32 gXeBhBlc = 0; static uint64_t gXeBhFirstNs = 0; static int gXeBhMinMs = 600, gXeBhMaxMs = 1500;
struct XeBdEnt { uint64_t ns; uint32_t reg, val, old; };
static XeBdEnt gXeBd[256];
static volatile SInt32 gXeBdN = 0, gXeBdPrinted = 0;
static bool xeBdWatched(uint32_t r) {
	switch (r) {
		case 0x43200: case 0x43208: case 0x43224: case 0x45000: case 0x4208c:         // FBC: CFB base, DPFC_CTL, chicken, chicken par
		case 0x70180: case 0x70188: case 0x7018C: case 0x70190: case 0x7019C: case 0x701A4:  // plane A CTL/STRIDE/POS/SIZE/SURF/OFFSET
		case 0x6001C: case 0x60800: case 0x60900:                                     // PIPE_SRCSZ, PSR1/PSR2 CTL
		case 0x45504:                                                                 // DC_STATE_EN
			return true;
	}
	return false;
}
static void xeBootDiagRecord(unsigned long reg, uint32_t val) {
	uint32_t r = (uint32_t)(reg & 0xFFFFF);
	if (!xeBdWatched(r)) return;
	SInt32 i = OSIncrementAtomic((volatile SInt32 *)&gXeBdN);
	if (i >= (SInt32)arrsize(gXeBd)) return;
	uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	gXeBd[i].ns = ns; gXeBd[i].reg = r; gXeBd[i].val = val;
	gXeBd[i].old = NGreen::callback ? NGreen::callback->readReg32(r) : 0xFFFFFFFF;
}
static void xeBootDiagDump(const char *tag) {
	if (!NGreen::callback) return;
	static int dumps = 0;
	if (dumps++ >= 12) return;
	uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	PE_Video v {};
	IOPlatformExpert *pe = OSDynamicCast(IOPlatformExpert, IOService::getPlatform());
	bool hv = pe && pe->getConsoleInfo(&v) == kIOReturnSuccess;
	#define R(a) NGreen::callback->readReg32(a)
	IOLog("XeNG BOOTDIAG %s t=%llu.%03llu console=%d base=0x%lx rowBytes=%lu %lux%lu depth=%lu disp=%lu\n", tag,
		  ns / 1000000000ULL, (ns / 1000000ULL) % 1000ULL, (int)hv, (unsigned long)v.v_baseAddr, (unsigned long)v.v_rowBytes,
		  (unsigned long)v.v_width, (unsigned long)v.v_height, (unsigned long)v.v_depth, (unsigned long)v.v_display);
	IOLog("XeNG BOOTDIAG %s PLANE_A CTL=%08x STRIDE=%08x POS=%08x SIZE=%08x SURF=%08x SURFLIVE=%08x OFF=%08x SRCSZ=%08x CUR=%08x/%08x\n", tag,
		  R(0x70180), R(0x70188), R(0x7018C), R(0x70190), R(0x7019C), R(0x701AC), R(0x701A4), R(0x6001C), R(0x70080), R(0x70084));
	IOLog("XeNG BOOTDIAG %s FBC CTL=%08x CFB=%08x CHICKEN=%08x STATUS=%08x/%08x PAR1=%08x 4208c=%08x PSR=%08x PSR2=%08x DC=%08x CDCLK_CTL=%08x DE_PLL=%08x\n", tag,
		  R(0x43208), R(0x43200), R(0x43224), R(0x43210), R(0x43214), R(0x45000), R(0x4208c), R(0x60800), R(0x60900), R(0x45504), R(0x46000), R(0x46070));
	for (int f = 0; f < 32; f++) {   // fences (aperture detiling used by CPU writes to tiled surfaces)
		uint32_t lo = R(0x100000 + 8 * f), hi = R(0x100004 + 8 * f);
		if (lo & 1) IOLog("XeNG BOOTDIAG %s FENCE%d lo=%08x hi=%08x\n", tag, f, lo, hi);
	}
	#undef R
	SInt32 n = gXeBdN; if (n > (SInt32)arrsize(gXeBd)) n = arrsize(gXeBd);
	for (SInt32 i = gXeBdPrinted; i < n; i++)
		IOLog("XeNG BOOTDIAG w#%d t=%llu.%03llu reg=%05x val=%08x (was %08x)\n", (int)i, gXeBd[i].ns / 1000000000ULL,
			  (gXeBd[i].ns / 1000000ULL) % 1000ULL, gXeBd[i].reg, gXeBd[i].val, gXeBd[i].old);
	gXeBdPrinted = n;
	IOLog("XeNG BOOTDIAG %s boothold blc0=%08x min=%d max=%d on=%d holding=%d done=%d ctl=%08x first=%08x\n", tag, (unsigned)gXeBhBlc, gXeBhMinMs, gXeBhMaxMs, (int)gXeBhOn, (int)gXeBhHolding, (int)gXeBhDone, (unsigned)gXeBhCtl, (unsigned)gXeBhFirst);
	if (gXeBdN > (SInt32)arrsize(gXeBd)) IOLog("XeNG BOOTDIAG %s ring full (%d writes)\n", tag, (int)gXeBdN);
}

static thread_call_t gXeBhTimer = nullptr;
static void xeBootHoldRelease(const char *why) {   // write Apple's last CTL, re-arm with the current SURF, backlight back on
	if (gXeBhDone) return;
	gXeBhDone = 1;
	if (gXeBhHolding && NGreen::callback) {
		NGreen::callback->writeReg32(0x70180, gXeBhCtl);
		if (why) NGreen::callback->writeReg32(0x7019C, NGreen::callback->readReg32(0x7019C));
		if (gXeBhBlc & 0x80000000u) NGreen::callback->writeReg32(0xC8250, NGreen::callback->readReg32(0xC8250) | 0x80000000u);
	}
	gXeBhHolding = 0;
}
static void xeBootHoldInit() {   // thread context (FBController::start): timer for the safety release
	if (!gXeBhTimer) gXeBhTimer = thread_call_allocate([](thread_call_param_t, thread_call_param_t) {
		if (!gXeBhDone) { xeBootHoldRelease("timeout"); IOLog("XeNG BOOTHOLD: released by timeout %d ms (first SURF 0x%x)\n", gXeBhMaxMs, (unsigned)gXeBhFirst); }
	}, nullptr);
}
// XE-BOOTHOLD v4 (bootbar5): as v3, plus backlight (BLC_PWM_PCH_CTL1 0xC8250 bit 31) off during the hold and release
// only after the ring wrapped AND >= xebbms (default 600) ms since the first surface; timeout xebbms+900 ms.
// v3: the first show of EACH buffer of WindowServer's ring is unrendered (garbage), so the plane stays off
// until a SURF returns to a buffer already shown (ring wrapped); 500 ms after the first non-zero SURF it is released anyway.
static uint32_t xeBootHold(unsigned long reg, uint32_t val) {
	if (gXeBhDone) return val;
	uint32_t r = (uint32_t)(reg & 0xFFFFF);
	if (r == 0xC8250) {   // Apple re-enabling the backlight during the hold: remember it, keep it off until release
		if (gXeBhHolding) { gXeBhBlc = val; return val & ~0x80000000u; }
		return val;
	}
	if (r != 0x70180 && r != 0x7019C) return val;
	if (gXeBhOn < 0) gXeBhOn = checkKernelArgument("-xenobb") ? 0 : 1;
	if (!gXeBhOn || !NGreen::callback) { gXeBhDone = 1; return val; }
	if (r == 0x70180) {
		if (!(val & 0x80000000u)) return val;
		bool start = !gXeBhHolding && NGreen::callback->readReg32(0x7019C) == 0 && (val & (0x7u << 10));  // tiled CTL on the boot buffer
		if (start) {   // v4: also switch the backlight off for the whole hold (hides anything not coming from plane A)
			PE_parse_boot_argn("xebbms", &gXeBhMinMs, sizeof(gXeBhMinMs));
			if (gXeBhMinMs < 0 || gXeBhMinMs > 5000) gXeBhMinMs = 600;
			gXeBhMaxMs = gXeBhMinMs + 900;
			gXeBhBlc = NGreen::callback->readReg32(0xC8250);
			if (gXeBhBlc & 0x80000000u) NGreen::callback->writeReg32(0xC8250, gXeBhBlc & ~0x80000000u);
		}
		if (start || gXeBhHolding) { gXeBhCtl = val; gXeBhHolding = 1; return val & ~0x80000000u; }
		return val;
	}
	if (val == 0) return val;
	if (!gXeBhHolding) { gXeBhDone = 1; return val; }               // never held: nothing to do
	static uint32_t seen[8], nseen = 0, last = 0;
	if (!gXeBhFirst) {                                              // first real surface: keep off, start the safety timer
		gXeBhFirst = val; absolutetime_to_nanoseconds(mach_absolute_time(), &gXeBhFirstNs);
		if (gXeBhTimer) { uint64_t d; clock_interval_to_deadline(gXeBhMaxMs, kMillisecondScale, &d); thread_call_enter_delayed(gXeBhTimer, d); }
	}
	if (val == last) return val;                                    // same buffer re-armed in the same flip
	last = val;
	for (uint32_t k = 0; k < nseen; k++)
		if (seen[k] == val) {   // ring wrapped (every buffer rendered once) AND >= xebbms since the first surface
			uint64_t now = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &now);
			if (now - gXeBhFirstNs >= (uint64_t)gXeBhMinMs * 1000000ULL) xeBootHoldRelease(nullptr);   // CTL first, this SURF arms both
			return val;
		}
	if (nseen < 8) seen[nseen++] = val;
	return val;
}
static uint32_t xeFixFilterWrite(unsigned long reg, uint32_t val) {
	xeBootDiagRecord(reg, val);
	val = xeBootHold(reg, val);
	if (!xeFixEnabled()) return val;
	val = xeNoDpms((uint32_t)(reg & 0x1FFFFF), val);
	// 20/10 (XE-BLR): o brilho ia só até ~50%. Como o BacklightRegistersFix do WhateverGreen: mantém o período do
	// hardware (lido antes da 1ª escrita da Apple) e reescala o duty por período_hw / período_Apple. Se a Apple não
	// gravar o período, ngreenblmax=N diz qual duty ela considera 100%. -ngreennoblr desliga.
	{
		uint32_t r = (uint32_t)(reg & 0xFFFFF);
		if (r == 0xC8254 || r == 0xC8258) {
			static int blr = -1, blmax = 0;
			if (blr < 0) { blr = checkKernelArgument("-ngreennoblr") ? 0 : 1; PE_parse_boot_argn("ngreenblmax", &blmax, sizeof(blmax)); }
			if (!gBlHwFreq && NGreen::callback) gBlHwFreq = NGreen::callback->readReg32(0xC8254);
			if (r == 0xC8254) {
				gBlFreqWrites++;
				if (val) gBlAppleFreq = val;
				if (blr == 1 && gBlHwFreq && gBlHwFreq != 0xFFFFFFFF) val = gBlHwFreq;
			} else {
				gBlLastDuty = val;
				if (val > gBlMaxDuty) gBlMaxDuty = val;
				uint32_t ref = blmax > 0 ? (uint32_t)blmax : gBlAppleFreq;
				if (blr == 1 && ref && gBlHwFreq && gBlHwFreq != 0xFFFFFFFF && ref != gBlHwFreq) {
					uint64_t v = (uint64_t)val * gBlHwFreq / ref;
					val = v > gBlHwFreq ? gBlHwFreq : (uint32_t)v;
				}
				gBlLastOut = val;
			}
		}
	}
	switch (reg & 0xFFFFF) {
		case 0x44FE8: if (!(val & 0x80000000u)) { static int n = 0; if (n++ < 8) IOLog("XeNG xefix: DBUF_CTL_S1 0x%x -> power on\n", val); } return val | 0x80000000u;
		case 0x42084: return val | 0x40000000u;
		// GAMMA_MODE_A: o driver TGL liga gamma pós-CSC de 10 bits com tabela no formato errado para o
		// display 13 -> texto/cores degradados. Provado ao vivo (17/10 20h): com 0 (bypass) a imagem fica certa.
		case 0x4A480: return 0;
	}
	return val;
}
// xe/inert: descarta escritas de Apple nos blocos dos engines (RCS 0x2000-0x2FFF, BCS 0x22000-0x22FFF, VDBOX/VEBOX 0x1C0000-0x1CFFFF)
// quando -ngreenengineinert esta ligado; display (0x6xxxx/0x7xxxx) nao e afetado.
static bool xeEngineInertOn() {
	static int v = -1;
	if (v < 0) v = checkKernelArgument("-ngreenengineinert") ? 1 : 0;
	return v == 1;
}
static bool xeInertDrop(unsigned long reg) {
	if (!xeEngineInertOn()) return false;
	unsigned long r = reg & 0xFFFFFF;
	bool eng = (r >= 0x2000 && r <= 0x2FFF) || (r >= 0x22000 && r <= 0x22FFF) || (r >= 0x1C0000 && r <= 0x1CFFFF);
	if (eng) { static int n = 0; if (n++ < 24) IOLog("XeNG inert: escrita descartada reg=0x%lx\n", r); }
	return eng;
}
static void xeFixEnsure() {
	if (!xeFixEnabled() || !NGreen::callback) return;
	uint32_t ck = NGreen::callback->readReg32(0x42084);
	if (!(ck & 0x40000000u)) NGreen::callback->writeReg32(0x42084, ck | 0x40000000u);
	if (NGreen::callback->readReg32(0x4A480) != 0) NGreen::callback->writeReg32(0x4A480, 0);
	uint32_t s1 = NGreen::callback->readReg32(0x44FE8);
	if (!(s1 & 0x80000000u)) NGreen::callback->writeReg32(0x44FE8, s1 | 0x80000000u | 0x40c000u);
	static int n = 0; if (n++ < 4) IOLog("XeNG xefix: ensure CK=0x%x S1=0x%x\n", NGreen::callback->readReg32(0x42084), NGreen::callback->readReg32(0x44FE8));
}
void Gen11::FastWriteRegister32(AppleIntel::AppleIntelBaseController *that, unsigned long param_1, uint32_t param_2)
{
	if (xeInertDrop(param_1)) return;
	param_2 = xeFixFilterWrite(param_1, param_2);
	if ((param_1 & 0xFFFFF) == 0x7019C) xeFixEnsure();
	// V99D: Diagnose — log all FastWrite calls near display engine range on first boot
	// to understand what addresses/values flow through this path.
	{
		static int v99DCount = 0;
		if (v99DCount < 30) {
			v99DCount++;
			SYSLOG("ngreen", "V99D[%d]: FastWrite addr=0x%lx val=0x%x",
				   v99DCount, param_1, param_2);
		}
	}

	// V72F removed (was: force RCS/BCS RING_EMR FastWrite path to 0xFFFFFFFF — same
	// blanket-mask hack as V72R/V72W, on the FastWriteRegister32 entry). Last of the
	// "raWriteRegister32-side" EMR mask trio. The V74 50ms enforcer still re-forces
	// EMR via direct writeReg32 from a polling thread, so this passthrough alone is
	// not yet a full "no blanket EMR mask" test — V74's EMR portion is stripped
	// separately in v71EmrEnforcer.
	if (param_1 == 0x20b4 || param_1 == 0x220b4) {
		static int v72FPassCount = 0;
		if (v72FPassCount < 6) {
			++v72FPassCount;
			SYSLOG("ngreen", "V72Fp[%d]: EMR @ 0x%lx val=0x%x passthrough (V72F hack removed)",
				   v72FPassCount, param_1, param_2);
		}
	}

	// V99F[S] removed (was: PLANE_STRIDE *= 8 — same X-tile-units → 64B-cacheline-units
	// rewrite as V99R[S], on the FastWriteRegister32 entry). Pair-mate to V99R[Sp].
	// V99S downstream still re-forces STRIDE=0xa0 at SURF arm via direct MMIO, so
	// this passthrough does not affect what scans out.
	if ((param_1 & 0xFFFFF) == 0x70188) {
		static int v99FSpCount = 0;
		if (v99FSpCount < 3) {
			++v99FSpCount;
			SYSLOG("ngreen", "V99F[Sp%d]: PLANE_STRIDE 0x%x passthrough (V99F[S] hack removed)",
				   v99FSpCount, param_2);
		}
	}
	// V99F[C] removed (was: PLANE_CTL X-tiled (001) → Y-tiled-legacy (100) rewrite,
	// FastWriteRegister32 twin of V99R[C]). V99S at SURF arm still re-forces CTL
	// downstream, so this is a no-op for actually-displayed values.
	if ((param_1 & 0xFFFFF) == 0x70180) {
		static int v99FCpCount = 0;
		if (v99FCpCount < 3) {
			++v99FCpCount;
			uint32_t tiling = (param_2 >> 10) & 0x7;
			SYSLOG("ngreen", "V99F[Cp%d]: PLANE_CTL 0x%x passthrough tiling=%d (V99F[C] hack removed)",
				   v99FCpCount, param_2, tiling);
		}
	}
	// V103F removed (was: FastWriteRegister32 twin of V103 — block DC_STATE_EN
	// non-zero writes). Pair-mate to V103/V103P; all three sites now passthrough.
	if ((param_1 & 0xFFFFF) == 0x45504 && NGreen::callback && NGreen::callback->dmcIsAdlp) {
		static int v103FpCount = 0;
		if (v103FpCount < 6) {
			++v103FpCount;
			SYSLOG("ngreen", "V103Fp[%d]: DC_STATE_EN FastWrite 0x%x passthrough (V103F hack removed)",
				   v103FpCount, param_2);
		}
	}

	// V195F removed (FastWriteRegister32 site of V195 — same hack pile).
	if ((param_1 & 0xFFFFF) == 0x45400 && NGreen::callback && !NGreen::callback->isRealTGL
		&& NGreen::callback->uefiCtl1 != 0) {
		static int v195FpCount = 0;
		if (v195FpCount < 6) {
			++v195FpCount;
			SYSLOG("ngreen", "V195Fp[%d]: CTL1 FastWrite 0x%x passthrough (V195F hack removed)",
				   v195FpCount, param_2);
		}
	}

	// V99F[SURF] removed (was: at SURF arm via FastWriteRegister32, force PLANE_STRIDE=0xa0
	// — FastWrite-path twin of V99S non-dp0 STRIDE=0xa0 force in raWriteRegister32). The
	// raWriteRegister32 V99S still re-forces STRIDE at SURF arm via direct MMIO, so this
	// passthrough doesn't change what scans out.
	if ((param_1 & 0xFFFFF) == 0x7019C && NGreen::callback) {
		uint32_t hwStride = NGreen::callback->readReg32(0x70188);
		static int v99FSurfPassCount = 0;
		if (v99FSurfPassCount < 3) {
			++v99FSurfPassCount;
			SYSLOG("ngreen", "V99F[SURFp%d]: SURF arm 0x%x STRIDE=0x%x passthrough (V99F[SURF] hack removed)",
				   v99FSurfPassCount, param_2, hwStride);
		}
	}

	return FunctionCast(FastWriteRegister32, callback->oFastWriteRegister32)(that,param_1,param_2 );
}

void Gen11::raWriteRegister32b(void *that,void *param_1,unsigned long param_2, UInt32 param_3)
{
	if (xeInertDrop(param_2)) return;
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return;
	//if (reinterpret_cast<volatile uint64_t*>(param_1)==nullptr) return;
	raWriteRegister32(that, reinterpret_cast<uint64_t>(param_1) + param_2,param_3);
};

void Gen11::raWriteRegister32(void *that,unsigned long param_1, UInt32 param_2)
{
	if (xeInertDrop(param_1)) return;
	param_2 = xeFixFilterWrite(param_1, param_2);
	if ((param_1 & 0xFFFFF) == 0x7019C) xeFixEnsure();
	// V93: optional plane SURF zero-write guard.
	// Disabled by default due black-screen regressions; enable only with -ngreenv93.
	struct V93PlaneSurfState {
		uint32_t surfReg;
		uint32_t lastNonZeroSurf;
	};
	static V93PlaneSurfState v93States[8] {};
	static int v93LogCount = 0;

	auto getV93State = [](uint32_t surfReg) -> V93PlaneSurfState * {
		for (auto &state : v93States) {
			if (state.surfReg == surfReg)
				return &state;
		}
		for (auto &state : v93States) {
			if (state.surfReg == 0) {
				state.surfReg = surfReg;
				state.lastNonZeroSurf = 0;
				return &state;
			}
		}
		return nullptr;
	};

	if (NGreen::callback && isV93PlaneGuardEnabled()) {
		const uint32_t reg = static_cast<uint32_t>(param_1 & 0xFFFFF);
		const bool looksLikePlaneSurf =
			(reg >= 0x60000 && reg <= 0xBFFFF) &&
			((reg & 0xFFF) == 0x19C);

		if (looksLikePlaneSurf) {
			auto *state = getV93State(reg);
			if (param_2 != 0) {
				if (state)
					state->lastNonZeroSurf = param_2;
			} else {
				const uint32_t ctlReg = reg - 0x1C; // PLANE_CTL is SURF - 0x1C on Gen11 paths.
				uint32_t planCtl = NGreen::callback->readReg32(ctlReg);
				if (planCtl & 0x80000000u) {
					if (state && state->lastNonZeroSurf != 0) {
						if (v93LogCount < 32) {
							SYSLOG("ngreen", "V93: blocked zero SURF@0x%x while enabled; keeping last 0x%x", reg, state->lastNonZeroSurf);
							v93LogCount++;
						}
						param_2 = state->lastNonZeroSurf;
					} else {
						uint32_t currentSurf = NGreen::callback->readReg32(reg);
						if (currentSurf != 0) {
							if (state)
								state->lastNonZeroSurf = currentSurf;
							if (v93LogCount < 32) {
								SYSLOG("ngreen", "V93: blocked zero SURF@0x%x while enabled; keeping current 0x%x", reg, currentSurf);
								v93LogCount++;
							}
							param_2 = currentSurf;
						} else {
							// Last resort: disable plane before allowing SURF=0 write.
							NGreen::callback->writeReg32(ctlReg, planCtl & ~0x80000000u);
							if (v93LogCount < 32) {
								SYSLOG("ngreen", "V93: forced plane disable for SURF@0x%x before zero write", reg);
								v93LogCount++;
							}
						}
					}
				}
			}
		}
	}

	// V72R removed (was: force RING_EMR (0x20b4 / 0x220b4) writes to 0xFFFFFFFF —
	// blanket-mask-all-GT-engine-errors). Hack that hides root cause; Linux i915
	// doesn't do blanket EMR masking. Remove and observe what real engine errors
	// emerge so the actual cause can be fixed.
	// NOTE: V72W (in wrapWriteRegister32 helper) and V72F (in FastWriteRegister32)
	// and V74 50ms permanent enforcer still mask RING_EMR. To fully test "no EMR
	// blanket mask" they need to be disabled in subsequent steps.

	// V99R[S] removed (was: PLANE_STRIDE *= 8 — convert X-tiled tile-units 0x14 to
	// Y-tiled/linear cacheline-units 0xa0). Hack guessing the right scanout stride
	// without knowing the actual buffer layout. Linux i915 picks stride based on
	// the IOSurface/buffer's documented tiling, not by rewriting Apple's value.
	// Removing this lets Apple's natural PLANE_STRIDE (0x14) reach hardware. If
	// scanout shows X-tiled bytes correctly → buffer was X-tiled. If garbled →
	// real layer is buffer/renderer side, not register-write.
	// NOTE: V99S (SURF arm) and V99f (fast-write variant) and V99F (FastWrite) still
	// have their own PLANE_STRIDE rewrites — to fully test, those need removal too.
	// For now keep the address-match shell so future legitimate intercepts can use it.
	if ((param_1 & 0xFFFFF) == 0x70188) { // PLANE_STRIDE Pipe A Plane 1
		// xe/stride: override no caminho MMIO (o que realmente chega ao hardware). A escrita na struct do plano não chega.
		uint32_t strideOv = 0;
		uint32_t strideIn = param_2;
		if (PE_parse_boot_argn("ngreenstride", &strideOv, sizeof(strideOv)) && strideOv)
			param_2 = strideOv;
		static int strideLogs = 0;
		if (strideLogs++ < 20)
			IOLog("XeNG STRIDE[%d] in=0x%x out=0x%x ov=0x%x\n", strideLogs, strideIn, param_2, strideOv);
		static int v99SPassCount = 0;
		if (v99SPassCount < 3) {
			++v99SPassCount;
			SYSLOG("ngreen", "V99R[Sp%d]: PLANE_STRIDE 0x%x passthrough (V99R hack removed)", v99SPassCount, param_2);
		}
	}
	// V99R[C] / V99R[Cdp] removed (was: PLANE_CTL tiling rewrites — X-tiled (001) →
	// Y-tiled legacy (100) on default path, force linear under -ngreendp0). Same
	// "guess the tiling" antipattern as V99R[S]; pairs with the stride-rewrite hack
	// just removed. Linux i915 picks PLANE_CTL tiling bits from the IOSurface/buffer's
	// declared tiling, never rewrites Apple's value. With V99R[S] passthrough, leaving
	// the corresponding CTL field rewritten is contradictory: STRIDE is now 0x14
	// (X-tiled tile-units) but CTL would still be forced to Y-tiled or linear, which
	// is guaranteed-wrong scanout. Removing both lets Apple's natural CTL reach HW so
	// we can observe what tiling Apple's allocator actually chose.
	if ((param_1 & 0xFFFFF) == 0x70180) { // PLANE_CTL Pipe A Plane 1
		static int v99CPassCount = 0;
		if (v99CPassCount < 3) {
			++v99CPassCount;
			uint32_t tiling = (param_2 >> 10) & 0x7;
			SYSLOG("ngreen", "V99R[Cp%d]: PLANE_CTL 0x%x passthrough tiling=%d (V99R hack removed)",
				   v99CPassCount, param_2, tiling);
		}
	}

	// V97 removed (was: clear bit[16] PORT_SYNC_MODE_MASTER_SELECT[0] from
	// TRANS_DDI_FUNC_CTL_A on !isRealTGL — Apple's SetupParams sets it but UEFI GOP
	// doesn't, allegedly disrupting trained eDP link → "InterLane Alignment is lost"
	// ~10s later). Symptom-hiding hack: real fix should match Linux i915's Display 13
	// transcoder programming, not strip a bit Apple's stack expects to set. Removing
	// reveals whether the InterLane loss still happens; if yes, look at Linux's
	// TRANS_DDI_FUNC_CTL field layout for ADL-P (Display 13) since PORT_SYNC fields
	// changed across display generations.
	if ((param_1 & 0xFFFFF) == 0x60400 && NGreen::callback && !NGreen::callback->isRealTGL) {
		static int v97RpCount = 0;
		if (v97RpCount < 6) {
			++v97RpCount;
			SYSLOG("ngreen", "V97Rp[%d]: TRANS_DDI_FUNC_CTL_A 0x%x passthrough bit16=%d (V97 hack removed)",
				   v97RpCount, param_2, !!(param_2 & (1u << 16)));
		}
	}

	// V103 removed (was: block all non-zero DC_STATE_EN writes when ADL-P DMC is
	// loaded — keeps the DMC from entering DC3/DC5/DC6 because Apple's ICL-targeted
	// driver has no ADL-P DC exit recovery, all eDP lanes drop ~70s after a write).
	// Symptom-hiding hack: Linux i915 has proper DC enter/exit for Display 13 via
	// the DMC. Permanently disabling DC states masks Apple's broken DC exit instead
	// of providing one. Removing reveals when DC_STATE_EN gets written and what
	// value Apple wants set; the fix is to ensure DC exit is properly handled (or
	// to align with what Linux ADL-P DMC expects).
	if ((param_1 & 0xFFFFF) == 0x45504 && NGreen::callback && NGreen::callback->dmcIsAdlp) {
		static int v103PassCount = 0;
		if (v103PassCount < 10) {
			++v103PassCount;
			SYSLOG("ngreen", "V103p[%d]: DC_STATE_EN 0x%x passthrough (V103 hack removed)",
				   v103PassCount, param_2);
		}
	}

	// V195 removed (was: same OR-in bits 14,12 hack as V195W, on raWriteRegister32).
	if ((param_1 & 0xFFFFF) == 0x45400 && NGreen::callback && !NGreen::callback->isRealTGL
		&& NGreen::callback->uefiCtl1 != 0) {
		static int v195pCount = 0;
		if (v195pCount < 6) {
			++v195pCount;
			SYSLOG("ngreen", "V195p[%d]: CTL1 ra 0x%x passthrough (V195 hack removed)",
				   v195pCount, param_2);
		}
	}

	// V99S: PLANE_SURF arm — force correct STRIDE/CTL immediately before latching.
	// raWriteRegister32/WriteRegister32 is a CACHE-ONLY update; hardware MMIO for
	// double-buffered PLANE_STRIDE is written via a volatile* path not caught by
	// FastWriteRegister32. Forcing writeReg32 here ensures the hardware shadow is
	// correct right before SURF arms the double-buffer flip.
	//
	// dp0 path: redirect non-aperture SURF to 0x0 AND remap GGTT[0..] to the same
	// physical pages (V99G).  setupScanoutMemory migrates SURF from aperture to
	// ≥0x10000000 (non-aperture stolen RAM, phys ~0x7f…) when WindowServer sets
	// kIOWindowServerActiveAttribute=3. After migration WS writes to the non-aperture
	// physical pages; PLANE_SURF must also scan them. V99G copies the GGTT PTEs from the
	// non-aperture range down to GGTT[0..3999] so SURF=0x0 scans the same pages.
	if ((param_1 & 0xFFFFF) == 0x7019C && NGreen::callback) {
		uint32_t hwStride = NGreen::callback->readReg32(0x70188);
		uint32_t hwCtl    = NGreen::callback->readReg32(0x70180);
		static int v99SCount = 0;
		if (v99SCount < 8) {
			++v99SCount;
			// V201B: read TOP-LEFT (gray border) AND CENTER (Apple logo / loading bar
			// area) of the buffer. Linear byte offset of pixel (x,y) = y*10240 + x*4.
			// Sample points (BGRA, 2560×1600):
			//  - (0,0)         offset 0          → top-left, gray bg
			//  - (1280,800)    offset 0x7D2800   → screen center, Apple logo
			//  - (1280,1000)   offset 0x9C7800   → loading-bar row
			//  - (640,800)     offset 0x7D1A00   → mid-left of logo area
			NGreen::callback->setApertureIfNecessary();
			uint32_t tlCtr = 0xDEADBEEF, ctr = 0xDEADBEEF, bar = 0xDEADBEEF, mid = 0xDEADBEEF;
			if (NGreen::callback->aperturePtr && NGreen::callback->apertureLen >= 0xA00000) {
				volatile uint32_t *fb32 = NGreen::callback->aperturePtr;
				tlCtr = fb32[0];             // top-left
				ctr   = fb32[0x7D2800 / 4];  // center (Apple logo)
				bar   = fb32[0x9C7800 / 4];  // loading bar row
				mid   = fb32[0x7D1A00 / 4];  // mid-left of logo area
			}
			SYSLOG("ngreen", "V99S[%d]: SURF arm 0x%x STRIDE=0x%x CTL=0x%x | tl=%08x ctr(1280,800)=%08x bar(1280,1000)=%08x mid(640,800)=%08x",
				   v99SCount, (uint32_t)param_2, hwStride, hwCtl, tlCtr, ctr, bar, mid);

			// V203: on the LAST sampled SURF arm, dump every pipe/transcoder/DSC/scaler
			// register relevant to the duplicated-content symptom. We're hunting an
			// off-by-2 in stride / src-vs-active / DSC bpp / pipe-bpc / M-N.
			if (v99SCount == 8) {
				#define R(addr) NGreen::callback->readReg32(addr)
				SYSLOG("ngreen", "V203: --- SCANOUT REGISTER DUMP ---");
				// Plane A
				SYSLOG("ngreen", "V203 PLANE_A: CTL=%08x STRIDE=%08x POS=%08x SIZE=%08x OFFSET=%08x SURF=%08x SURFLIVE=%08x AUX_DIST=%08x AUX_OFFSET=%08x KEYVAL=%08x KEYMSK=%08x KEYMAX=%08x COLOR_CTL=%08x",
					   R(0x70180), R(0x70188), R(0x7018C), R(0x70190), R(0x701A4),
					   R(0x7019C), R(0x701AC), R(0x701C0), R(0x701C4),
					   R(0x70194), R(0x70198), R(0x701A0), R(0x701CC));
				// Pipe A general
				SYSLOG("ngreen", "V203 PIPE_A: SRCSZ=%08x CONF=%08x MISC=%08x MISC2=%08x STAT=%08x",
					   R(0x6001C), R(0x70008), R(0x70030), R(0x7002C), R(0x70024));
				// Transcoder A timings
				SYSLOG("ngreen", "V203 TRANS_A: HTOTAL=%08x HBLANK=%08x HSYNC=%08x VTOTAL=%08x VBLANK=%08x VSYNC=%08x VSYNCSHIFT=%08x",
					   R(0x60000), R(0x60004), R(0x60008), R(0x6000C),
					   R(0x60010), R(0x60014), R(0x60028));
				// DDI function control + MSA
				SYSLOG("ngreen", "V203 TRANS_A_DDI: DDI_FUNC_CTL=%08x DDI_FUNC_CTL2=%08x MSA_MISC=%08x CONF=%08x CLK_SEL=%08x",
					   R(0x60400), R(0x60404), R(0x60410), R(0x70008), R(0x46140));
				// DP M/N values for Pipe A
				SYSLOG("ngreen", "V203 TRANS_A_DPMN: DATAM1=%08x DATAN1=%08x DATAM2=%08x DATAN2=%08x LINKM1=%08x LINKN1=%08x LINKM2=%08x LINKN2=%08x",
					   R(0x60030), R(0x60034), R(0x60038), R(0x6003C),
					   R(0x60040), R(0x60044), R(0x60048), R(0x6004C));
				// Pipe A scaler 1
				SYSLOG("ngreen", "V203 PIPE_A_PS1: CTRL=%08x WIN_POS=%08x WIN_SZ=%08x VPHASE=%08x HPHASE=%08x",
					   R(0x68180), R(0x68170), R(0x68174), R(0x68188), R(0x68194));
				// DSC slice control / PPS for Pipe A (DSC_BASE_A around 0x6B200; varies by gen)
				SYSLOG("ngreen", "V203 DSC_A: PIC_RC=%08x PPS0=%08x PPS1=%08x PPS2=%08x PPS3=%08x PPS4=%08x",
					   R(0x6B200), R(0x6B210), R(0x6B214), R(0x6B218), R(0x6B21C), R(0x6B220));
				// DDI buf / port
				SYSLOG("ngreen", "V203 DDI_BUF: A_CTL=%08x B_CTL=%08x | DP_TP_CTL_A=%08x DP_TP_STATUS_A=%08x",
					   R(0x64000), R(0x64100), R(0x64040), R(0x64044));
				// Display Buffer programming
				SYSLOG("ngreen", "V203 DBUF: CTL_S0=%08x CTL_S1=%08x DBUF_BUF_CFG_A_PA=%08x A_PB=%08x",
					   R(0x44300), R(0x44304), R(0x70B80), R(0x70B84));
				#undef R
			}
		}
		// ------ MAN IN THE MIDDLE CHIP HACK ------- (V99R/V99G ativo: teste xtile2)
		// V99R[P] + V99G + linear CTL/STRIDE forces — CORE scanout coherence (!isRealTGL).
		// Confirmed load-bearing for visible scanout on spoofed RPL/ADL-P in dp0, dp1, AND
		// without any -ngreendp* boot arg. Real TGL hardware programs these correctly via
		// Apple's native code path — gating the entire triad on !isRealTGL.
		//
		// Friend's architectural feedback (Visual Ehrmanntraut, NootedBlue lineage):
		// the right long-term fix is intercepting CRTCParams / PLANEPARAMS / SCALERPARAMS
		// at hwSetupMemory / paramsSurfCompare / hwRegsNeedUpdate (already partial via
		// V97P / V97C) instead of hooking MMIO writes after-the-fact. "Hacking regs only
		// won't work — leave WS alone, problem is in apple code."
		//
		//   1. SURF redirect: non-aperture writes (>=0x10000000) → 0, so the display engine
		//      always scans from the same GGTT page range (GGTT[0..3999]) no matter where
		//      Apple's setupScanoutMemory chose to migrate the surface.
		//   2. V99G: per-flip GGTT remap — copies the PTEs at the migrated surface pages
		//      down to GGTT[0..3999] so SURF=0 fetches the same physical memory WS's CPU
		//      compositor is writing into. Re-runs whenever Apple's SURF address changes
		//      (handles double/triple buffering — WS rotates between 2-3 IOSurfaces per flip).
		//   3. Linear CTL/STRIDE forces (tiling→0, STRIDE=0xa0): required for visible
		//      output. Removing them produces a black screen with no scanout activity,
		//      confirmed empirically. Side effect: produces the fragmented/repeated
		//      pattern when Apple's allocator stores buffer in non-linear physical layout
		//      — known cost of this path, removable only by struct-level fix.
		// xe (19/10): com -ngreenxefix o V99R/V99G fica desligado (religar com -ngreenv99g). Os slots fixos de 4000
		// páginas (feitos para 2560x1600) só cobriam 16 MB dos 36,9 MB de 3840x2400: o resto da tela lia a memória
		// stolen (glitch embaixo), e o slot 0 (GGTT[0..3999]) pisava no LRCA do contexto em GGTT 0x18000.
		// Sem o desvio, o plano lê a superfície que o driver da Apple aponta (GGTT já mapeada por ele).
		static int v99Arg = -1;  // sem guard variable no kernel: calcula uma vez na mão
		if (v99Arg < 0) v99Arg = (checkKernelArgument("-ngreenv99g") || !checkKernelArgument("-ngreenxefix")) ? 1 : 0;
		const bool v99On = v99Arg == 1;
		if (!v99On && param_2 >= 0x10000000u) {
			static int v99OffCount = 0;
			if (v99OffCount < 4) IOLog("XeNG V99 off: SURF 0x%x direto\n", (uint32_t)param_2), ++v99OffCount;
		}
		if (v99On && NGreen::callback && !NGreen::callback->isRealTGL && param_2 >= 0x10000000u) {
			static int v99PCount = 0;
			if (v99PCount < 8)
				SYSLOG("ngreen", "V99R[P%d]: SURF 0x%x->aperture (non-aperture redirect)",
					   ++v99PCount, (uint32_t)param_2);

			// ngreen-buf=N: 1=single, 2=double (default), 3=triple buffering.
			// Each buffer slot occupies 4000 GGTT pages = 0xFA0000 bytes of aperture:
			//   slot 0 → GGTT[0..3999],      SURF=0x0
			//   slot 1 → GGTT[4000..7999],   SURF=0xFA0000
			//   slot 2 → GGTT[8000..11999],  SURF=0x1F40000
			// Each unique non-aperture srcPage Apple presents is assigned a fixed slot.
			// The GGTT PTEs for that slot are remapped to point at the IOSurface's
			// physical pages. SURF is rewritten to the matching aperture slot address
			// so the display engine scans the correct physical memory.
			// With single buffering all flips always land on slot 0 (SURF=0x0).
			static int  bufCount       = -1;
			static uint32_t slotPages[3] = {0, 0, 0}; // srcPage assigned to each slot
			static int  slotCount      = 0;
			static int  v99GCount      = 0;

			if (bufCount < 0) {
				int val = 2;
				PE_parse_boot_argn("ngreen-buf", &val, sizeof(val));
				bufCount = (val >= 1 && val <= 3) ? val : 2;
				SYSLOG("ngreen", "V99G: ngreen-buf=%d (buffering slots)", bufCount);
			}

			uint32_t srcPage = (uint32_t)param_2 >> 12;

			// Find existing slot or assign a new one.
			int slot = -1;
			for (int s = 0; s < slotCount; s++) {
				if (slotPages[s] == srcPage) { slot = s; break; }
			}
			if (slot < 0) {
				if (slotCount < bufCount) {
					slot = slotCount++;
				} else {
					// All slots occupied — evict oldest (slot 0), shift down.
					for (int s = 0; s < bufCount - 1; s++) slotPages[s] = slotPages[s + 1];
					slot = bufCount - 1;
				}
				slotPages[slot] = srcPage;
			}

			// Remap GGTT[slot*4000 .. slot*4000+3999] from srcPage..srcPage+3999.
			{
				int base = slot * 4000;
				int remapped = 0, remapSkipped = 0, firstSkip = -1, lastSkip = -1;
				for (int i = 0; i < 4000; i++) {
					uint32_t lo = NGreen::callback->readReg32(GGTT_PTE_LO(srcPage + i));
					uint32_t hi = NGreen::callback->readReg32(GGTT_PTE_HI(srcPage + i));
					if (!(lo & 1)) { if (remapSkipped == 0) firstSkip = i; lastSkip = i; remapSkipped++; continue; }
					NGreen::callback->writeReg32(GGTT_PTE_LO(base + i), lo);
					NGreen::callback->writeReg32(GGTT_PTE_HI(base + i), hi);
					remapped++;
				}
				NGreen::callback->writeReg32(0x101008, 0x1); // flush GGTT TLB
				if (++v99GCount <= 8 || (v99GCount & 0x3F) == 0)
					SYSLOG("ngreen", "V99G[%d]: GGTT[%d..%d] <- srcPage=0x%x slot=%d remapped=%d skip=%d",
						   v99GCount, base, base + 3999, srcPage, slot, remapped, remapSkipped);
					// xe/diag: IOLog reaches the kernel log (SYSLOG does not reach the unified log).
					if (v99GCount <= 16 || (v99GCount & 0x3F) == 0)
						IOLog("XeNG V99G[%d] slot=%d srcPage=0x%x remapped=%d skip=%d firstSkip=%d lastSkip=%d\n",
							  v99GCount, slot, srcPage, remapped, remapSkipped, firstSkip, lastSkip);
			}

			// Redirect SURF to the aperture address of the assigned slot.
			// slot 0 → 0x0, slot 1 → 0xFA0000, slot 2 → 0x1F40000.
			param_2 = (uint32_t)slot * 0xFA0000u;
		}
		// CTL/STRIDE forces — MATCH APPLE'S NATURAL INTENT.
		// V401 paramsSurfCompare logs prove Apple wants: CTL bits[12:10]=001 (X-tiled),
		// STRIDE=0x14 (20 X-tile units = 10240B/row = 2560*4bpp). Apple's IOSurface
		// allocator produces X-tiled physical buffers — Y-tile and linear forces both
		// scan wrong bytes from an X-tile buffer. Match Apple = same tile mode as the
		// buffer = correct scanout, IF the SURF address reaches the right pages
		// (V99R[P]+V99G handle the SURF redirect / GGTT remap unconditionally).
		//
		// Gated on !isRealTGL. Real TGL programs natively.
		/* linear forces com STRIDE=0xa0 (2560 px) ficam desligados: o paramsSurfCompare já força linear com stride do modo real.
		if (NGreen::callback && !NGreen::callback->isRealTGL) {
			// force CTL linear and STRIDE=0xa0 (CPU compositor writes linearly via BAR2).
			uint32_t hwTiling = (hwCtl >> 10) & 0x7;
			if (hwTiling != 0)
				NGreen::callback->writeReg32(0x70180, hwCtl & ~(0x7u << 10));
			if (hwStride != 0xa0)
				NGreen::callback->writeReg32(0x70188, 0xa0);
		}*/
	}

	if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return NGreen::callback->writeReg32(param_1,param_2);
	if (!callback->oraWriteRegister32) return NGreen::callback->writeReg32(param_1,param_2);
	FunctionCast(raWriteRegister32, callback->oraWriteRegister32)( that,param_1,param_2);
};

uint32_t Gen11::AppleIntelFramebufferinit(AppleIntel::AppleIntelFramebuffer *frame,
                                          AppleIntel::AppleIntelBaseController *cont,
                                          uint32_t pipeIndex)
{
	// Offsets into the full IOFramebuffer subclass hierarchy (much larger than the
	// tail fields captured in AppleIntelParams::AppleIntelFramebuffer).
	getMember<void *>(frame, 0x4a40) = ccont;
	getMember<void *>(frame, 0xc40)  = ccont;
	auto ret = FunctionCast(AppleIntelFramebufferinit, callback->oAppleIntelFramebufferinit)(frame, cont, pipeIndex);
	getMember<void *>(frame, 0x4a40) = ccont;
	getMember<void *>(frame, 0xc40)  = ccont;
	return ret;
}



void Gen11::initPlatformWorkarounds(AppleIntel::AppleIntelBaseController *that)
{
	// Platform workaround flags for ADL-P (RPL-P) running under TGL driver.
	// flags_ig (+0xC58): boot info flags — checked by PowerWell::init to set fAlwaysOn.
	//   Must be set BEFORE PowerWell::init runs if we want Apple's native fAlwaysOn path.
	//   We also force fAlwaysOn=1 in our PowerWell::init hook as belt-and-suspenders.
	// fInfoFlags2 (+0xC5C): display feature flags.
	//   ADL-P uses PCH PWM for backlight (cnp_setup_backlight confirmed in Linux syslog).
	//   Do NOT set FB_FLAG_ENABLE_BACKLIGHT_REG_CONTROL (that forces CPU-register backlight).
	that->flags_ig    = FB_FLAG_BOOST_PIXEL_FREQUENCY_LIMIT;
	that->fInfoFlags2 =
		FB_FLAG_ALTERNATE_PWM_INCREMENT1 |
		FB_FLAG_ALTERNATE_PWM_INCREMENT2 |
		FB_FLAG_ENABLE_SLICE_FEATURES    |
		FB_FLAG_FORCE_POWER_ALWAYS_CONNECTED |
		FB_FLAG_AVOID_FAST_LINK_TRAINING;

	FunctionCast(initPlatformWorkarounds, callback->oinitPlatformWorkarounds)(that);

	// V212: ADL-P (Display 13) specific display workarounds, ported from Linux i915.
	// Apple's TGL kext targets Display 12 and doesn't apply these chicken bits / clock-
	// gating / error masks on the spoofed setup. Linux marks them as REQUIRED for
	// Display 13+; their absence can manifest as display engine misbehavior
	// (timing/underrun/error-recovery loops). Gated by !isRealTGL so genuine TGL HW
	// (Display 12) is unaffected.
	if (NGreen::callback && !NGreen::callback->isRealTGL) {
		// Wa_22011091694:adlp — DPCE_GATING_DIS = REG_BIT(17) in GEN9_CLKGATE_DIS_5 (0x46540)
		NGreen::callback->intel_de_rmw(0x46540, 0, 1u << 17);

		// Bspec/49189 ADL-P init — CLEAR DDI_CLOCK_REG_ACCESS = REG_BIT(7) in GEN8_CHICKEN_DCPR_1 (0x46430)
		NGreen::callback->intel_de_rmw(0x46430, 1u << 7, 0);

		// PIPE_CHICKEN Pipe A (0x70038):
		//   bit 30 = UNDERRUN_RECOVERY_DISABLE_ADLP — required on Display 13+
		//   bit 7  = PER_PIXEL_ALPHA_BYPASS_EN     — Display WA #1153
		//   bit 15 = PIXEL_ROUNDING_TRUNC_FB_PASSTHRU — Display WA #1605353570
		NGreen::callback->intel_de_rmw(0x70038, 0, (1u << 30) | (1u << 15) | (1u << 7));

		// XELPD_DISPLAY_ERR_FATAL_MASK (0x4421C) ← mask all fatal display errors on
		// Display 13 (per icl_display_core_init in Linux i915). Without this, fatal
		// error events can trigger pipeline restart loops.
		NGreen::callback->writeReg32(0x4421C, 0xFFFFFFFFu);

		uint32_t pipeChicken    = NGreen::callback->readReg32(0x70038);
		uint32_t clkGateDis5    = NGreen::callback->readReg32(0x46540);
		uint32_t chickenDcpr1   = NGreen::callback->readReg32(0x46430);
		uint32_t errFatalMask   = NGreen::callback->readReg32(0x4421C);
		SYSLOG("ngreen", "V212: ADL-P Display 13+ workarounds applied — PIPE_CHICKEN(A)=0x%x CLKGATE_DIS_5=0x%x CHICKEN_DCPR_1=0x%x ERR_FATAL_MASK=0x%x",
			   pipeChicken, clkGateDis5, chickenDcpr1, errFatalMask);
	}
}

uint64_t Gen11::getOSInformation(AppleIntel::AppleIntelBaseController *that)
{
	auto *pinfo = reinterpret_cast<PlatformInfo *>(callback->gPlatformInformationList);
	if (pinfo) {
		// Index 1 = the mobile TGL/ADL-P platform entry (0x9A490000 and variants).
		pinfo[1].fInfoFlags =
			FB_FLAG_DISABLE_PIPE_SCRAMBLE      |
			FB_FLAG_FRAMEBUFFER_COMPRESSION    |
			FB_FLAG_ALLOW_CONNECTOR_RECOVER    |
			FB_FLAG_FORCE_POWER_ALWAYS_CONNECTED |
			FB_FLAG_AVOID_FAST_LINK_TRAINING;

		// cameliav=2 (CamelliaTcon2) requires GFX kext present — gate on -ngreentglwithgfx.
		pinfo[1].cameliav = checkKernelArgument("-ngreentglwithgfx") ? 2 : 0;
		pinfo[1].fMobile  = 1;
		// 3/3/3 baseline restored. Multi-pipe reduction is whack-a-mole — every count
		// reduction reveals new cross-pipe NULL-deref sites in TGL FB internals
		// (enableController +0x1356, getFreeJoinablePathCount +0xa7, etc).
		// Best known config: 3/3/3 + -ngreendp0 → reaches login with banded display.
		pinfo[1].fPipeCount            = 3;
		pinfo[1].fInfoPortCount        = 3;
		pinfo[1].fInfoFramebufferCount = 3;
		pinfo[1].fSliceCount  = 1;
		pinfo[1].fmaxEuCount  = 8;
		pinfo[1].fsubslices   = 10;

		// Connector 0: built-in eDP (LVDS), DDI-A, pipe 0
		pinfo[1].connectors[0].index = 0;
		pinfo[1].connectors[0].busId = 0;
		pinfo[1].connectors[0].pipe  = 0;
		pinfo[1].connectors[0].pad   = 0;
		pinfo[1].connectors[0].type  = ConnectorLVDS;
		pinfo[1].connectors[0].flags = 0x8 | 0x10;

		// Connector 1: external USB-C/Thunderbolt DP (TC1/DDI-D), pipe 2
		pinfo[1].connectors[1].index = 1;
		pinfo[1].connectors[1].busId = 1;
		pinfo[1].connectors[1].pipe  = 2;
		pinfo[1].connectors[1].pad   = 0;
		pinfo[1].connectors[1].type  = ConnectorDP;
		pinfo[1].connectors[1].flags = 0x1 | 0x400;

		// Connectors 2-3: Dummy
		pinfo[1].connectors[2] = { 2, 2, 2, 0, ConnectorDummy, 0 };
		pinfo[1].connectors[3] = { 3, 3, 3, 0, ConnectorDummy, 0 };

		SYSLOG("ngreen", "getOSInformation: patched pinfo[1] for ADL-P (LVDS+HDMI, mobile)");
	}
	return FunctionCast(getOSInformation, callback->ogetOSInformation)(that);
}

int Gen11::handleLinkIntegrityCheck()
{
	return 0;
};

void Gen11::hwInitializeCState(AppleIntel::AppleIntelBaseController *that)
{
	SYSLOG("ngreen", "NB-BUILD-V50-ALLOW-METAL");

	int origB48 = getMember<int>(that, 0xB48);
	int origCE4 = getMember<int>(that, 0xCE4);
	SYSLOG("ngreen", "hwInitCState B48=%d CE4=%d", origB48, origCE4);

	// Boot-arg "ngreen-dmc":
	//   not set or "skip" → safe fallback: passthrough original + AUX only (proven working)
	//   "tgl"             → load TGL DMC v2.12 blob + TGL display engine registers
	//                       + ICL/TGL combo PHY signal levels (PHY_A eDP, PHY_B DP)
	//   "adlp"            → load ADL-P DMC v2.16 blob + ADL-P display engine registers
	//                       + combo PHY signal levels (PHY_A eDP)
	//   "icl"             → passthrough original ICL DMC load + ICL combo PHY signal levels
	char dmcArg[16] = {};
	PE_parse_boot_argn("ngreen-dmc", dmcArg, sizeof(dmcArg));

	if (dmcArg[0] == 't' || dmcArg[0] == 'T') {
		// ── TGL DMC ──
		SYSLOG("ngreen", "hwInitCState: ngreen-dmc=tgl, loading TGL DMC v2.12 (%u dwords)", tgl_dmc_ver2_12_bin_s / 4);
		// Write TGL DMC blob to MMIO 0x80000+
		for (unsigned long off = 0; off < tgl_dmc_ver2_12_bin_s; off += 4)
			FastWriteRegister32(reinterpret_cast<AppleIntel::AppleIntelBaseController *>(ccont), off + 0x80000,
				*(const uint32_t *)((const char *)tgl_dmc_ver2_12_bin + off));

		// Disable DC states before touching display engine registers (same as ADL-P path).
		// DC_STATE_EN = 0x45504
		NGreen::callback->writeReg32(DC_STATE_EN, 0);

		// Use UEFI CTL1 as base (same pattern as ADL-P path).
		uint32_t tglUefiCtl1 = NGreen::callback->readReg32(0x45400);
		{
			uint32_t newCtl1 = tglUefiCtl1 | 0x00000401u;
			NGreen::callback->writeReg32(0x45400, newCtl1); // HSW_PWR_WELL_CTL1
			NGreen::callback->writeReg32(0x45404, 0x00000C03); // HSW_PWR_WELL_CTL2
			SYSLOG("ngreen", "V101T: PWR_WELL CTL1 uefi=0x%x->0x%x CTL2=0xc03", tglUefiCtl1, newCtl1);
		}
		NGreen::callback->writeReg32(0x45408, 0x40000000); // HSW_PWR_WELL_CTL3
		NGreen::callback->writeReg32(0x4540C, 0x00000401); // HSW_PWR_WELL_CTL4
		NGreen::callback->writeReg32(0x45440, 0x00000003); // ICL_PWR_WELL_CTL_AUX1 — AUX A
		NGreen::callback->writeReg32(0x45444, 0x00000003); // ICL_PWR_WELL_CTL_AUX2 — AUX B
		NGreen::callback->writeReg32(0x45450, 0x00000003); // ICL_PWR_WELL_CTL_DDI1 — DDI A
		NGreen::callback->writeReg32(0x45454, 0x00000003); // ICL_PWR_WELL_CTL_DDI2 — DDI B

		// TGL display engine registers (DMC trigger/context regs in 0x8Fxxx range)
		// Values from IDA of original hwInitializeCState (TGL-native values)
		NGreen::callback->writeReg32(0x8F074, 0x00006FC0);
		NGreen::callback->writeReg32(0x8F004, 0x00A40088);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);

		// Enable DMC — DC_STATE_DEBUG (0x45520) = 2
		NGreen::callback->writeReg32(0x45520, 2); // DC_STATE_DEBUG
		NGreen::callback->dmcIsAdlp = true;        // reuse flag: also protects TGL path via V103/V104P
		NGreen::callback->uefiCtl1  = tglUefiCtl1;
		SYSLOG("ngreen", "hwInitCState: TGL DMC loaded");
		// setSignalLevels block REMOVED for the same reason as the ADL-P branch:
		// Linux i915 (per /Volumes/EFI/syslog.txt drm trace) only calls setSignalLevels
		// during DP link training, where swing=0/0/0/0 is the negotiation STARTING point
		// before DPRX adjust-request bumps it to vswing=1/1/1/1. Calling it once at
		// hwInitCState with all-zero levels freezes the combo PHY at the lowest drive
		// strength forever → DP receivers read garbage bitRates, link silently fails.
		// {
		// 	uint8_t swing[4]   = {0, 0, 0, 0};
		// 	uint8_t preEmph[4] = {0, 0, 0, 0};
		// 	IntelDPLinkTraining::setSignalLevels(/*phy=*/0, /*lanes=*/4, /*isHBR2=*/false, /*isDP=*/true, swing, preEmph);
		// 	IntelDPLinkTraining::setSignalLevels(/*phy=*/1, /*lanes=*/4, /*isHBR2=*/false, /*isDP=*/true, swing, preEmph);
		// }
		// Let original run with B48=1 (ICL CSR blob loads to SRAM).
		// Same rationale as ADL-P: the TGL DMC firmware also causes lane drops because its
		// DC state management runs on ADL-P hardware; the ICL DMC is safer on this silicon.
		// TGL context regs (8Fxxx) are re-applied after so the display engine sees TGL values.
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
		// Re-apply TGL context regs overwritten by original's ICL blob load.
		NGreen::callback->writeReg32(0x8F074, 0x00006FC0);
		NGreen::callback->writeReg32(0x8F004, 0x00A40088);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);
		SYSLOG("ngreen", "V104T: TGL context regs re-applied after ICL blob load");
		// V102T: restore CTL1 after original (ICL DMC save/restore table may write 0x401).
		{
			uint32_t postCtl1 = NGreen::callback->readReg32(0x45400);
			uint32_t fixCtl1  = tglUefiCtl1 | 0x00000401u;
			if (postCtl1 != fixCtl1) {
				NGreen::callback->writeReg32(0x45400, fixCtl1);
				SYSLOG("ngreen", "V102T: restore PWR_WELL CTL1 0x%x->0x%x", postCtl1, fixCtl1);
			}
		}

	} else if (dmcArg[0] == 'a' || dmcArg[0] == 'A') {
		// ── ADL-P DMC ──
		SYSLOG("ngreen", "hwInitCState: ngreen-dmc=adlp, loading ADL-P DMC v2.16 (%u dwords)", adlp_dmc_ver2_16_bin_s / 4);
		// adlp_dmc_ver2_16_bin is the raw firmware payload extracted from the v3 blob
		// (adlp_dmc_ver2_16.bin: CSS+package header stripped, pipe-A payload at file offset 0x310).
		// Write directly to SRAM starting at 0x80000.
		for (unsigned long off = 0; off < adlp_dmc_ver2_16_bin_s; off += 4)
			FastWriteRegister32(reinterpret_cast<AppleIntel::AppleIntelBaseController *>(ccont), off + 0x80000,
				*(const uint32_t *)((const char *)adlp_dmc_ver2_16_bin + off));

		// Disable DC states before touching display engine registers.
		// If DC5/DC6 is active when we write, the clock-gated blocks won't latch the writes.
		// DC_STATE_EN = 0x45504 (confirmed: Archive HIGH, linux display/intel_display_regs.h)
		NGreen::callback->writeReg32(DC_STATE_EN, 0);

		// Power wells — Gen12 ICL-style DDI + AUX power well enable.
		// HSW_PWR_WELL_CTL1/2 (0x45400/45404): PG1/PG2 enable+state — values read from
		// Linux intel_reg dump on same hardware (reg_dump.txt):
		//   CTL1=0x00000401 (PG1 req+enabled), CTL2=0x00000C03 (PG1+PG2 req+enabled)
		//   CTL3=0x40000000 (PG3 state only), CTL4=0x00000401
		// V100: preserve UEFI CTL1 state bits (bits 12,14 = display power wells enabled).
		// Writing the hardcoded 0x401 clears those bits, breaking vsync interrupt delivery
		// → WindowServer crash at ~60s ("Display not ready").
		// Fix: read UEFI CTL1 and OR in our minimum bits; keep CTL2 hardcoded at 0x0C03.
		// DO NOT OR CTL2 with UEFI: UEFI leaves 0xfc00 (TC port "state" bits) set in CTL2.
		// Asserting TC bits without IOM handshake disrupts the display domain during mode
		// change, preventing the WSA un-blank (stays at 0x1, no cursor, no desktop).
		uint32_t uefiCtl1 = NGreen::callback->readReg32(0x45400);
		{
			uint32_t newCtl1  = uefiCtl1 | 0x00000401u;  // ensure PG1 req+state bits set
			NGreen::callback->writeReg32(0x45400, newCtl1); // HSW_PWR_WELL_CTL1
			NGreen::callback->writeReg32(0x45404, 0x00000C03); // HSW_PWR_WELL_CTL2 — hardcoded, no TC bits
			SYSLOG("ngreen", "V101: PWR_WELL CTL1 uefi=0x%x->0x%x CTL2=0xc03",
				   uefiCtl1, newCtl1);
		}
		NGreen::callback->writeReg32(0x45408, 0x40000000); // HSW_PWR_WELL_CTL3
		NGreen::callback->writeReg32(0x4540C, 0x00000401); // HSW_PWR_WELL_CTL4
		// ICL_PWR_WELL_CTL_AUX1/2 (0x45440/45444): enable AUX power wells A+B
		// bit1=req, bit0=enabled per ICL DDI power well HW spec (intel_display_regs.h)
		NGreen::callback->writeReg32(0x45440, 0x00000003); // ICL_PWR_WELL_CTL_AUX1 — AUX A enabled
		NGreen::callback->writeReg32(0x45444, 0x00000003); // ICL_PWR_WELL_CTL_AUX2 — AUX B enabled
		// ICL_PWR_WELL_CTL_DDI1/2 (0x45450/45454): enable DDI power wells A+B
		NGreen::callback->writeReg32(0x45450, 0x00000003); // ICL_PWR_WELL_CTL_DDI1 — DDI A enabled
		NGreen::callback->writeReg32(0x45454, 0x00000003); // ICL_PWR_WELL_CTL_DDI2 — DDI B enabled

		// ADL-P / RPL-P display engine registers — exact MMIO init pairs from v3 blob header
		// (adlp_dmc_ver2_16.bin header @ 0x0210, MMIO[0..6], stepping='A' variant)
		NGreen::callback->writeReg32(0x8F074, 0x00086FC0);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);
		NGreen::callback->writeReg32(0x8F004, 0x01240108);
		NGreen::callback->writeReg32(0x8F038, 0xC003B200); // was missing
		NGreen::callback->writeReg32(0x8F008, 0x4FE44F98); // corrected from 0x512050D4
		NGreen::callback->writeReg32(0x8F03C, 0xC003B300);
		NGreen::callback->writeReg32(0x8F00C, 0x571056C0); // corrected from 0x584C57FC
		// DDI C-F (ADL-P TC port registers — DKL PHY, 0x5Fxxx range)
		NGreen::callback->writeReg32(0x5F074, 0x00096FC0);
		NGreen::callback->writeReg32(0x5F034, 0xC003DF00);
		NGreen::callback->writeReg32(0x5F004, 0x214C2114);
		NGreen::callback->writeReg32(0x5F038, 0xC003E000);
		NGreen::callback->writeReg32(0x5F008, 0x22402208);
		NGreen::callback->writeReg32(0x5F03C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5F00C, 0x241422FC);
		NGreen::callback->writeReg32(0x5F040, 0xC0033100);
		NGreen::callback->writeReg32(0x5F010, 0x26F826CC);
		NGreen::callback->writeReg32(0x5F474, 0x0009EFC0);
		NGreen::callback->writeReg32(0x5F434, 0xC003DF00);
		NGreen::callback->writeReg32(0x5F404, 0xA968A930);
		NGreen::callback->writeReg32(0x5F438, 0xC003E000);
		NGreen::callback->writeReg32(0x5F408, 0xAA5CAA24);
		NGreen::callback->writeReg32(0x5F43C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5F40C, 0xAC30AB18);
		NGreen::callback->writeReg32(0x5F440, 0xC0033100);
		NGreen::callback->writeReg32(0x5F410, 0xAF14AEE8);
		NGreen::callback->writeReg32(0x5F874, 0x00053FC0);
		NGreen::callback->writeReg32(0x5F83C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5F80C, 0x25202408);
		NGreen::callback->writeReg32(0x5F840, 0xC0033100);
		NGreen::callback->writeReg32(0x5F810, 0x280427D8);
		NGreen::callback->writeReg32(0x5FC3C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5FC0C, 0x95209408);
		NGreen::callback->writeReg32(0x5FC40, 0xC0033100);
		NGreen::callback->writeReg32(0x5FC10, 0x980497D8);

		// Transcoder A DDI function + timing registers (re-imported from EFI reference for safety).
		// Values from Linux intel_reg dump on this hardware (reg_dump.txt).
		//   0x60400 = TRANS_DDI_FUNC_CTL_A  — DDI A enabled, DP SST, 8bpc, 2 lanes
		//   0x60000 = TRANS_HTOTAL_A        — 2560 active, 2720 total
		//   0x60004 = TRANS_HBLANK_A        — same as HTOTAL on eDP
		//   0x60008 = TRANS_HSYNC_A         — sync positions
		//   0x6000C = TRANS_VTOTAL_A        — 1600 active, 1750 total
		//   0x60010 = TRANS_VBLANK_A
		//   0x60014 = TRANS_VSYNC_A
		//   0x60028 = TRANS_VSYNCSHIFT_A
		//   0x60030/34 TRANS_DATA_M1/N1_A   — DP M/N values (TU 64, link rate)
		//   0x60040/44 TRANS_LINK_M1/N1_A
		// 0x8A000106: bit31=enable, [27:24]=DDI_A, [19:16]=2lanes, [3:1]=DP_SST(0x01)
		// Pre-matches Apple's target so paramsFbCompare sees no lane-count change; changing
		// lane count in TRANS_DDI_FUNC_CTL while transcoder is live resets the DDI buffer
		// and drops the trained link.
		// xe/nooted: the values below are a register dump of the author's 2560x1600, 2-lane eDP panel.
		// On a different panel they replace the firmware-trained native mode (here 3840x2400, 4 lanes
		// HBR3) and the panel goes black; Apple's FB then keeps them for the online boot display.
		// -ngreenkeepefi: keep what UEFI/GOP programmed when transcoder A is already running.
		const bool keepEfiTiming = checkKernelArgument("-ngreenkeepefi") &&
			(NGreen::callback->readReg32(0x70008) & 0x80000000u);
		if (keepEfiTiming) {
			SYSLOG("ngreen", "keepefi: TRANS_DDI_FUNC_CTL_A=0x%x HTOTAL=0x%x VTOTAL=0x%x PP_CONTROL=0x%x kept",
				   NGreen::callback->readReg32(0x60400), NGreen::callback->readReg32(0x60000),
				   NGreen::callback->readReg32(0x6000C), NGreen::callback->readReg32(0xC7204));
		} else {
			NGreen::callback->writeReg32(0x60400, 0x8A000106); // TRANS_DDI_FUNC_CTL_A
			NGreen::callback->writeReg32(0x60000, 0x0A9F09FF); // TRANS_HTOTAL_A
			NGreen::callback->writeReg32(0x60004, 0x0A9F09FF); // TRANS_HBLANK_A
			NGreen::callback->writeReg32(0x60008, 0x0A4F0A2F); // TRANS_HSYNC_A
			NGreen::callback->writeReg32(0x6000C, 0x06D5063F); // TRANS_VTOTAL_A
			NGreen::callback->writeReg32(0x60010, 0x06D50000); // TRANS_VBLANK_A
			NGreen::callback->writeReg32(0x60014, 0x06480642); // TRANS_VSYNC_A
			NGreen::callback->writeReg32(0x60028, 0x00000000); // TRANS_VSYNCSHIFT_A
			NGreen::callback->writeReg32(0x60030, 0x7E5D159E); // TRANS_DATA_M1_A  (TU 64, M=0x5d159e)
			NGreen::callback->writeReg32(0x60034, 0x00800000); // TRANS_DATA_N1_A  (N=0x800000)
			NGreen::callback->writeReg32(0x60040, 0x0007C1CD); // TRANS_LINK_M1_A  (M=0x7c1cd)
			NGreen::callback->writeReg32(0x60044, 0x00080000); // TRANS_LINK_N1_A  (N=0x80000)

			// Panel power sequencer (re-imported from EFI reference).
			// TGL/ADL-P both have PCH_SPLIT (ICP/TGP PCH) → intel_pps_setup sets
			// mmio_base = PCH_PPS_BASE = 0xC7200 (not 0x61200 which is BXT/APL).
			// Values from Linux intel_reg dump on this hardware:
			//   0xC7204 (PP_CONTROL)   = 0x00000067 (panel on, VDD on, power-on target)
			//   0xC7208 (PP_ON_DELAYS) = 0x07D00001 (T1=1, T3=2000ms power-on delays)
			NGreen::callback->writeReg32(0xC7204, 0x00000067); // PP_CONTROL
			NGreen::callback->writeReg32(0xC7208, 0x07D00001); // PP_ON_DELAYS
		}

		// PIPE_CLK_SEL_A (0x46140 = 0x10000000) REMOVED after the EFI re-import caused
		// "CD Clock PLL is locked" line to disappear from the FB log and link bitRates to
		// turn into garbage (27/40/63 instead of 179/204/206). Empirical: writing this
		// value to 0x46140 here either selects a clock source that prevents CD PLL lock,
		// or 0x46140 isn't actually PIPE_CLK_SEL on Display 13 (ADL-P) — Linux i915
		// names it differently for ADL-P. Leave for Apple's later mode-setup to program.
		// NGreen::callback->writeReg32(0x46140, 0x10000000); // PIPE_CLK_SEL_A

		// Enable DMC — DC_STATE_DEBUG (0x45520) = 2
		NGreen::callback->writeReg32(0x45520, 2); // DC_STATE_DEBUG
		NGreen::callback->dmcIsAdlp = true;
		NGreen::callback->uefiCtl1  = uefiCtl1;  // save for V60 re-enforcement
		SYSLOG("ngreen", "hwInitCState: ADL-P DMC loaded");
		// setSignalLevelsADLP block REMOVED after the EFI re-import caused link bitRates
		// to turn into garbage (27/40/63 instead of 179/204/206). Likely cause: calling
		// it with swing[]=preEmph[]={0,0,0,0} BEFORE Apple's link training overwrites
		// the UEFI-trained combo PHY DW2/4/5/7 with zero levels → DP link reads back
		// nonsense bitrates. The setSignalLevels function should only be invoked DURING
		// link training when swing/preEmph are properly populated, not as init scaffolding.
		// Left as a comment so we can re-enable surgically once real swing values are known.
		// {
		// 	uint8_t swing[4]   = {0, 0, 0, 0};
		// 	uint8_t preEmph[4] = {0, 0, 0, 0};
		// 	IntelDPLinkTraining::setSignalLevelsADLP(/*phy=*/0, /*lanes=*/4, /*isHBR2=*/false, /*isEDP=*/true,  swing, preEmph);
		// 	IntelDPLinkTraining::setSignalLevelsADLP(/*phy=*/1, /*lanes=*/4, /*isHBR2=*/false, /*isEDP=*/false, swing, preEmph);
		// }
		// Let original run with B48=1 so the ICL CSR blob is loaded to SRAM.
		// The ADL-P DMC firmware (even correct binary) autonomously drops all eDP lanes
		// at ~10s because its DC state management code runs on ADL-P hardware and performs
		// link maintenance that the ICL driver can't recover from. The ICL DMC running on
		// ADL-P hardware does NOT cause lane drops (it doesn't know ADL-P DC3CO/PSR2).
		// Our ADL-P display engine context regs (8Fxxx) are re-applied after the original
		// so the display engine still sees ADL-P-correct values despite ICL SRAM content.
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
		// Re-apply ADL-P context regs overwritten by original's ICL blob load.
		NGreen::callback->writeReg32(0x8F074, 0x00086FC0);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);
		NGreen::callback->writeReg32(0x8F004, 0x01240108);
		NGreen::callback->writeReg32(0x8F038, 0xC003B200);
		NGreen::callback->writeReg32(0x8F008, 0x4FE44F98);
		NGreen::callback->writeReg32(0x8F03C, 0xC003B300);
		NGreen::callback->writeReg32(0x8F00C, 0x571056C0);
		SYSLOG("ngreen", "V104: ADL-P context regs re-applied after ICL blob load");
		// V102: CTL1 restore — ICL DMC save/restore table writes CTL1=0x401, clearing
		{
			uint32_t postCtl1 = NGreen::callback->readReg32(0x45400);
			uint32_t fixCtl1  = uefiCtl1 | 0x00000401u;
			if (postCtl1 != fixCtl1) {
				NGreen::callback->writeReg32(0x45400, fixCtl1);
				SYSLOG("ngreen", "V102: restore PWR_WELL CTL1 0x%x->0x%x", postCtl1, fixCtl1);
			}
		}
		// V105: disable PSR1+PSR2 — the ICL DMC initializer (original hwInitializeCState)
		// enables PSR2 (EDP_PSR2_CTL bit 0 = 1) for the eDP panel. On ADL-P hardware with
		// the ICL driver the PSR2 selective-update path is non-functional: the panel locks
		// into self-refresh showing the initial black frame; only the cursor plane (separate
		// SU path) updates. Clearing both registers before the first frame is displayed
		// restores normal scanout. Confirmed root cause via V76: PSR2_CTL=0x4811, V88
		// direct physical writes to SURF invisible despite valid GGTT PTEs.
		{
			// V105 ADDRESS FIX: PSR1 control on TGL is at 0x60800 (transcoder EDP space),
			// NOT 0x64800 (older Gen ICL/SKL layout). Pre-fix V105 was writing to a wrong
			// register; PSR1 stayed enabled at 0x60800 = 0x00100001, panel kept refreshing
			// from its own cache, screen frozen on first frame even while PIPE_FRMCOUNT
			// kept advancing. Probe V205 caught this. Now writes to both addresses for
			// safety (0x60800 = TGL, 0x64800 = legacy/ICL — covers both spoof paths).
			uint32_t psr2ctl     = NGreen::callback->readReg32(0x60A10);  // EDP_PSR2_CTL TGL
			uint32_t psr1ctlTgl  = NGreen::callback->readReg32(0x60800);  // EDP_PSR_CTL  TGL
			uint32_t psr1ctlIcl  = NGreen::callback->readReg32(0x64800);  // EDP_PSR_CTL  ICL
			uint32_t psr2ctlIcl  = NGreen::callback->readReg32(0x60900);  // EDP_PSR2_CTL ICL
			NGreen::callback->writeReg32(0x60A10, 0);  // PSR2 TGL
			NGreen::callback->writeReg32(0x60800, 0);  // PSR1 TGL  ← THE ACTUAL FIX
			NGreen::callback->writeReg32(0x60900, 0);  // PSR2 legacy
			NGreen::callback->writeReg32(0x64800, 0);  // PSR1 legacy
			SYSLOG("ngreen", "V105: PSR disabled — TGL(PSR1=0x%x PSR2=0x%x) legacy(PSR1=0x%x PSR2=0x%x)",
				   psr1ctlTgl, psr2ctl, psr1ctlIcl, psr2ctlIcl);
		}

	} else if (dmcArg[0] == 'i' || dmcArg[0] == 'I') {
		// ── ICL ──
		// ICL is the native target: let original hwInitializeCState load the ICL DMC blob,
		// then program ICL combo PHY signal levels (same DW2/4/5/7 layout as TGL).
		SYSLOG("ngreen", "hwInitCState: ngreen-dmc=icl, passthrough + ICL combo PHY signal levels");
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
		// Program combo PHY signal levels — PHY_A (eDP, 4 lanes, HBR) + PHY_B (DP-B, 4 lanes, HBR)
		{
			uint8_t swing[4]   = {0, 0, 0, 0};
			uint8_t preEmph[4] = {0, 0, 0, 0};
			IntelDPLinkTraining::setSignalLevels(/*phy=*/0, /*lanes=*/4, /*isHBR2=*/false, /*isDP=*/true, swing, preEmph);
			IntelDPLinkTraining::setSignalLevels(/*phy=*/1, /*lanes=*/4, /*isHBR2=*/false, /*isDP=*/true, swing, preEmph);
		}
		SYSLOG("ngreen", "hwInitCState: ICL done");

	} else {
		// ── skip (default, safe fallback) ──
		// Proven working: just let original run + configure AUX.
		// No DMC blob, no display engine register writes.
		SYSLOG("ngreen", "hwInitCState: skip (safe fallback), passthrough original");
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
	}

	hwConfigureCustomAUX(that, true);
	SYSLOG("ngreen", "hwInitCState: done");
}

void NGreen::adlpDcExit(const char *caller) {
	if (!dmcIsAdlp) return;
	const uint32_t dcState = readReg32(0x45504); // DC_STATE_EN
	if (dcState == 0) return;
	static int adlpDcExitCount = 0;
	if (adlpDcExitCount < 48) {
		adlpDcExitCount++;
		SYSLOG("ngreen", "adlpDcExit[%s/%d]: DC_STATE_EN=0x%x — restoring ADL-P display state", caller, adlpDcExitCount, dcState);
	}
	// 1. Disable DC states so clock-gated blocks latch the writes below.
	writeReg32(0x45504, 0);
	// 2. Restore power wells (request enable on CTL1/2/3/4 + AUX A/B + DDI A/B).
	writeReg32(0x45400, uefiCtl1 | 0x401u);  // PWR_WELL_CTL1
	writeReg32(0x45404, 0x0C03u);             // PWR_WELL_CTL2
	writeReg32(0x45408, 0x40000000u);         // PWR_WELL_CTL3
	writeReg32(0x4540C, 0x401u);              // PWR_WELL_CTL4
	writeReg32(0x45440, 0x3u);               // ICL_PWR_WELL_CTL_AUX1 — AUX A
	writeReg32(0x45444, 0x3u);               // ICL_PWR_WELL_CTL_AUX2 — AUX B
	writeReg32(0x45450, 0x3u);               // ICL_PWR_WELL_CTL_DDI1 — DDI A
	writeReg32(0x45454, 0x3u);               // ICL_PWR_WELL_CTL_DDI2 — DDI B
	// 3. Restore ADL-P display engine context registers (saved by DMC on DC entry).
	writeReg32(0x8F074, 0x00086FC0u);
	writeReg32(0x8F034, 0xC003B400u);
	writeReg32(0x8F004, 0x01240108u);
	writeReg32(0x8F038, 0xC003B200u);
	writeReg32(0x8F008, 0x4FE44F98u);
	writeReg32(0x8F03C, 0xC003B300u);
	writeReg32(0x8F00C, 0x571056C0u);
	// 4. Restore panel power sequencer.
	writeReg32(0xC7204, 0x67u);  // PP_CONTROL
	// 5. Disable PSR — DMC may have re-enabled it on DC exit.
	writeReg32(0x60800, 0u);   // EDP_PSR_CTL (TGL)
	writeReg32(0x60A10, 0u);   // EDP_PSR2_CTL (TGL)
	if (adlpDcExitCount <= 48) {
		SYSLOG("ngreen", "adlpDcExit[%s]: restore complete", caller);
	}
}

void Gen11::AppleIntelPowerWellinit(AppleIntel::AppleIntelPowerWell *that, AppleIntel::AppleIntelBaseController *param_1)
{
	ccont = param_1->fRegCachePool;

	FunctionCast(AppleIntelPowerWellinit, callback->oAppleIntelPowerWellinit)(that, param_1);

	// After callthrough the south display domain is clocked; direct BAR read is safe.
	uint32_t pg  = NGreen::callback->readReg32(0x45404);
	uint32_t ddi = NGreen::callback->readReg32(0x45454);
	uint32_t aux = NGreen::callback->readReg32(0x45444);
	SYSLOG("ngreen", "PowerWell::init UEFI state — PG=0x%08x DDI=0x%08x AUX=0x%08x", pg, ddi, aux);

	// Apple's PowerWell::init checks fController->flags_ig & FB_FLAG_BOOST_PIXEL_FREQUENCY_LIMIT
	// (+0xC58) before setting fAlwaysOn=1. On RPL/ADL that flag isn't set when the kext first
	// calls PowerWell::init (initPlatformWorkarounds runs later), so fAlwaysOn stays 0 and
	// Apple can gate power wells off. We force fAlwaysOn=1 unconditionally on non-real-TGL.
	// Also stamp fMMIO in case Apple's TGL-path init skipped it on ADL-P hardware.
	SYSLOG("ngreen", "PowerWell::init — flags_ig=0x%x fAlwaysOn(before)=%u fMMIO=%p",
		   param_1->flags_ig, that->fAlwaysOn, that->fMMIO);
	if (!NGreen::callback->isRealTGL) {
		that->fAlwaysOn = 1;
		if (!that->fMMIO) that->fMMIO = reinterpret_cast<AppleIntel::AppleIntelMMIO *>(ccont);
		SYSLOG("ngreen", "PowerWell::init forced fAlwaysOn=1, fMMIO=%p fMMIOBase=%p",
			   that->fMMIO, that->fMMIO ? that->fMMIO->fMMIOBase : nullptr);
	}
	SYSLOG("ngreen", "PowerWell::init done — fAlwaysOn=%u fPGBase=%u PG1=%u PG2=%u PG3=%u PG4=%u",
		   that->fAlwaysOn, that->fPGBase, that->fPG1, that->fPG2, that->fPG3, that->fPG4);
	SYSLOG("ngreen", "PowerWell::init DDI — [0]=%u [1]=%u [2]=%u [3]=%u [4]=%u [5]=%u [6]=%u [7]=%u [8]=%u",
		   that->fDDI[0], that->fDDI[1], that->fDDI[2], that->fDDI[3], that->fDDI[4],
		   that->fDDI[5], that->fDDI[6], that->fDDI[7], that->fDDI[8]);
	SYSLOG("ngreen", "PowerWell::init AUX — [0]=%u [1]=%u [2]=%u [3]=%u [4]=%u [5]=%u [6]=%u [7]=%u [8]=%u",
		   that->fAUX[0], that->fAUX[1], that->fAUX[2], that->fAUX[3], that->fAUX[4],
		   that->fAUX[5], that->fAUX[6], that->fAUX[7], that->fAUX[8]);
}

// ─── Sleep/wake lifecycle hooks ──────────────────────────────────────────────
// All four Apple implementations set this+0x49e0=4 as their final operation.
// If Apple's code exits early (null Camellia ptr, lock contention, etc.) that
// write never happens and the FB state machine is stuck.  We callthrough and
// then enforce state=4 regardless, plus track panel power for IOKit properties.
//
// Lifecycle order:
//   Sleep: prepareToEnterSleep → prepareToExitWake
//   Wake:  prepareToExitSleep  → prepareToEnterWake

static void logFBCtrlState(const char *tag, AppleIntel::AppleIntelFramebuffer *that)
{
	auto *ctrl = that->fController;
	uint32_t fbState = getMember<uint32_t>(that, 0x49e0);
	if (ctrl) {
		SYSLOG("ngreen", "%s fb=%p fbState=%u flags_ig=0x%x fInfoFlags2=0x%x fGPUIsAwake=%u pipe=%u",
			   tag, that, fbState,
			   ctrl->flags_ig, ctrl->fInfoFlags2, getMember<uint32_t>(ctrl, 0x1A00),
			   that->fPipeIndex);
	} else {
		SYSLOG("ngreen", "%s fb=%p fbState=%u ctrl=NULL pipe=%u", tag, that, fbState, that->fPipeIndex);
	}
}

// Wake stage 2: confirm panel is on, log state.
// Apple does: panel-power property, some vtable dispatch, then state=4.
void Gen11::prepareToEnterWake(AppleIntel::AppleIntelFramebuffer *that)
{
	logFBCtrlState("prepareToEnterWake>>", that);
	bool panelOn = isPanelPowerOn(getMember<void *>(that, 0x1d0));
	if (panelOn) {
		reinterpret_cast<IORegistryEntry *>(that)->setProperty("AAPL,LCD-PowerState-ON", true);
	}
	FunctionCast(prepareToEnterWake, callback->oprepareToEnterWake)(that);
	uint32_t state_after = getMember<uint32_t>(that, 0x49e0);
	if (state_after != 4) {
		SYSLOG("ngreen", "prepareToEnterWake: state=0x%x (expected 4), forcing", state_after);
		getMember<uint32_t>(that, 0x49e0) = 4;
	}
	logFBCtrlState("prepareToEnterWake<<", that);
}

// Sleep stage 2: NVRAM save, Camellia backlight off, StopTransactions,
// disableDisplay, hwSetPanelPower(0), two handleEvent calls, then state=4.
void Gen11::prepareToExitWake(AppleIntel::AppleIntelFramebuffer *that)
{
	logFBCtrlState("prepareToExitWake>>", that);
	reinterpret_cast<IORegistryEntry *>(that)->setProperty("AAPL,LCD-PowerState-ON", false);
	FunctionCast(prepareToExitWake, callback->oprepareToExitWake)(that);
	uint32_t state_after = getMember<uint32_t>(that, 0x49e0);
	if (state_after != 4) {
		SYSLOG("ngreen", "prepareToExitWake: state=0x%x (expected 4), forcing", state_after);
		getMember<uint32_t>(that, 0x49e0) = 4;
	}
	logFBCtrlState("prepareToExitWake<<", that);
}

// Sleep stage 1: fSleeping=1, cancel timers, clientNotify(2,0), state=4,
// handleEvent(sleep), hwSaveState, hwDisableInterrupts, fGPUIsAwake=0.
void Gen11::prepareToEnterSleep(AppleIntel::AppleIntelFramebuffer *that)
{
	logFBCtrlState("prepareToEnterSleep>>", that);
	reinterpret_cast<IORegistryEntry *>(that)->setProperty("AAPL,LCD-PowerState-ON", false);
	FunctionCast(prepareToEnterSleep, callback->oprepareToEnterSleep)(that);
	uint32_t state_after = getMember<uint32_t>(that, 0x49e0);
	if (state_after != 4) {
		SYSLOG("ngreen", "prepareToEnterSleep: state=0x%x (expected 4), forcing", state_after);
		getMember<uint32_t>(that, 0x49e0) = 4;
	}
	logFBCtrlState("prepareToEnterSleep<<", that);
}

// Wake stage 1: restorePowerWellsState, hwEnableInterrupts, hwInitializeCState,
// hwRestoreState, probePortState per port, gamma/reg-cache restore,
// clientNotify(2,1), handleEvent(wake), fGPUIsAwake=1, setDPPowerState, state=4.
void Gen11::prepareToExitSleep(AppleIntel::AppleIntelFramebuffer *that)
{
	logFBCtrlState("prepareToExitSleep>>", that);
	FunctionCast(prepareToExitSleep, callback->oprepareToExitSleep)(that);
	uint32_t state_after = getMember<uint32_t>(that, 0x49e0);
	if (state_after != 4) {
		SYSLOG("ngreen", "prepareToExitSleep: state=0x%x (expected 4), forcing", state_after);
		getMember<uint32_t>(that, 0x49e0) = 4;
	}
	reinterpret_cast<IORegistryEntry *>(that)->setProperty("AAPL,LCD-PowerState-ON", true);
	logFBCtrlState("prepareToExitSleep<<", that);
}

bool Gen11::AppleIntelBaseControllerstart(AppleIntel::AppleIntelBaseController *that, IOService *param_1)
{
	// V25: Display workarounds BEFORE start (no ForceWake needed for display regs 0x4xxxx+).
	// GT workarounds moved AFTER start (ForceWake must be held for GT regs 0x0-0x7FFF).
	
	SYSLOG("ngreen", "AppleIntelBaseControllerstart: applying display workarounds");

	// Disable DC states during init to prevent power domain conflicts
	NGreen::callback->writeReg32(DC_STATE_EN, 0);
	
	/* Wa_14011294188:ehl,jsl,tgl,rkl,adl-s */
	NGreen::callback->intel_de_rmw(SOUTH_DSPCLK_GATE_D, 0,
				PCH_DPMGUNIT_CLOCK_GATE_DISABLE);
	
	// PCH reset handshake
	NGreen::callback->intel_de_rmw(HSW_NDE_RSTWRN_OPT, RESET_PCH_HANDSHAKE_ENABLE,
				RESET_PCH_HANDSHAKE_ENABLE);
	
	/* Wa_14011508470:tgl,dg1,rkl,adl-s,adl-p,dg2 */
	NGreen::callback->intel_de_rmw(GEN11_CHICKEN_DCPR_2, 0,
				DCPR_CLEAR_MEMSTAT_DIS | DCPR_SEND_RESP_IMM |
				DCPR_MASK_LPMODE | DCPR_MASK_MAXLATENCY_MEMUP_CLR);
	
	/* Display WA #1185 WaDisableDARBFClkGating:glk,icl,ehl,tgl (Wa_14010480278) */
	NGreen::callback->intel_de_rmw(GEN9_CLKGATE_DIS_0, 0, DARBF_GATING_DIS);
	
	/* Wa_14013723622 */
	NGreen::callback->intel_de_rmw(CLKREQ_POLICY, CLKREQ_POLICY_MEM_UP_OVRD, 0);
	
	SYSLOG("ngreen", "AppleIntelBaseControllerstart: display workarounds applied");

	SYSLOG("ngreen", "FBController::start() entering...");
	xeBootHoldInit();
	xeBootDiagDump("pre-FBstart");
	auto ret=FunctionCast(AppleIntelBaseControllerstart, callback->oAppleIntelBaseControllerstart)(that,param_1 );
	SYSLOG("ngreen", "FBController::start() returned %d", ret);
	xeBootDiagDump("post-FBstart");
	{   // again 10 s and 40 s later (thread context): writes made while the boot screen/progress bar were up and at the WindowServer handoff
		static thread_call_t tc10 = nullptr, tc40 = nullptr;
		if (!tc10) {
			tc10 = thread_call_allocate([](thread_call_param_t, thread_call_param_t) { xeBootDiagDump("t+10s"); }, nullptr);
			tc40 = thread_call_allocate([](thread_call_param_t, thread_call_param_t) { xeBootDiagDump("t+40s"); }, nullptr);
			uint64_t d10, d40; clock_interval_to_deadline(10, kSecondScale, &d10); clock_interval_to_deadline(40, kSecondScale, &d40);
			if (tc10) thread_call_enter_delayed(tc10, d10);
			if (tc40) thread_call_enter_delayed(tc40, d40);
		}
	}
	
	if (ret) {
		// The personality dict itself was registered in IOCatalogue from the HW-kext
		// processKext branch (see Gen11::injectAcceleratorPersonality). All this wrapper
		// does — and all an FB-tier route should do — is poke the FBController service
		// so IOKit re-runs matching now that the personality is present.
		auto *service = OSDynamicCast(IOService, reinterpret_cast<OSObject *>(that));
		if (service) {
			SYSLOG("ngreen", "FBController: calling registerService() to trigger accelerator matching");
			service->registerService();
		}
	}

	return ret;
}

uint32_t Gen11::wrapProbeCDClockFrequency(AppleIntel::AppleIntelBaseController *that) {

	// Sonoma probeCDClockFrequency checks reg 0x46070 (BXT_DE_PLL_ENABLE) bit 31 first.
	// If bit 31 is CLEAR it panics immediately: "Wrong CD clock frequency set by EFI".
	// EFI on Hackintosh may leave this clear, so we ensure it is set before calling the original.
	auto squash = NGreen::callback->readReg32(BXT_DE_PLL_ENABLE);  // byte addr OK — readReg32 divides internally
	if (!(squash & BXT_DE_PLL_PLL_ENABLE)) {
		DBGLOG("ngreen", "wrapProbeCDClockFrequency: BXT_DE_PLL_PLL_ENABLE (0x46070 bit31) was clear (0x%x), setting it", squash);
		NGreen::callback->writeReg32(BXT_DE_PLL_ENABLE, squash | BXT_DE_PLL_PLL_ENABLE);
	}

	// Always sanitize (disable + reprogram PLL) regardless of current CDCLK value.
	//
	// When BIOS leaves CDCLK already at >= 648 MHz (threshold), skipping sanitize causes
	// orgProbeCDClockFrequency to take the "cdclk already at target" path, which reads PCU
	// mailbox command 0x6 (PCODE_CDCLK_CONFIG verify). On ADL-P this returns 0x9b9b9b9b
	// (PCU not responding / command not supported), causing initCDClock to return an error.
	// start() then skips port allocation and boot display setup entirely — no hwUpdateCursorMemory,
	// no cursor GGTT entries → full black screen even when the display link is trained.
	//
	// Always calling sanitize takes the "frequency changed" success path in initCDClock,
	// which writes mailbox(7,2) ACK and skips the failing PCU 0x6 verify read.
	auto cdclk = NGreen::callback->readReg32(ICL_REG_CDCLK_CTL) & CDCLK_FREQ_DECIMAL_MASK;  // byte addr OK
	SYSLOG("ngreen", "wrapProbeCDClockFrequency: cdclk=0x%x, force-sanitizing to bypass PCU mailbox failure", cdclk);
	sanitizeCDClockFrequency(that);

	auto retVal = callback->orgProbeCDClockFrequency(that);
	return retVal;
}

void Gen11::sanitizeCDClockFrequency(AppleIntel::AppleIntelBaseController *that) {

	//auto referenceFrequency = callback->wrapReadRegister32(that, SKL_DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK;
	auto referenceFrequency =NGreen::callback->readReg32(ICL_REG_DSSM)>> 29;
	//auto referenceFrequency = callback->wrapReadRegister32(that, ICL_REG_DSSM) >> 29;
	uint32_t newCdclkFrequency = 0;
	uint32_t newPLLFrequency = 0;
	switch (referenceFrequency) {
		case ICL_REF_CLOCK_FREQ_19_2:
			newCdclkFrequency = ICL_CDCLK_FREQ_652_8;
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_19_2;
			break;
			
		case ICL_REF_CLOCK_FREQ_24_0:
			newCdclkFrequency = ICL_CDCLK_FREQ_648_0;
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_24_0;
			break;
			
		case ICL_REF_CLOCK_FREQ_38_4:
			newCdclkFrequency = ICL_CDCLK_FREQ_652_8;
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_38_4;
			break;
			
		default:
			return;
	}

	DBGLOG("ngreen", "sanitizeCDClockFrequency: ref=%u targetCdclk=0x%x targetPll=0x%x", referenceFrequency, newCdclkFrequency, newPLLFrequency);

	// Use solved original directly so sanitize remains safe even when disableCDClock route is toggled off.
	if (callback->orgDisableCDClock) {
		callback->orgDisableCDClock(that);
	} else {
		disableCDClock(that);
	}

	callback->orgSetCDClockFrequency(that, newPLLFrequency);

}

/*
void Gen11::initCDClock(void *that)
{
	// Mirrors AppleIntelFramebufferController::initCDClock decompiled flow.
	// DSSM bits [31:29] encode the reference clock frequency (0=24MHz, 1=19.2MHz, 2=38.4MHz).
	// The check < 0x60000000 ensures bits[31:29] < 3 (i.e. a valid reference).
	auto dssm = NGreen::callback->readReg32(ICL_REG_DSSM);
	if (dssm >= 0x60000000) {
		// Invalid / unsupported reference clock — fall back to original which will panic/reset.
		return FunctionCast(initCDClock, callback->oinitCDClock)(that);
	}

	uint32_t refFreqIdx = dssm >> 0x1d;                       // bits [31:29]
	getMember<uint32_t>(that, 0xe98) = refFreqIdx;

	// Call through our safe wrapper so BXT_DE_PLL_ENABLE / CDCLK sanity checks are applied.
	auto probedFreq = static_cast<uint64_t>(wrapProbeCDClockFrequency(that));
	getMember<uint64_t>(that, 0xe88) = probedFreq;

	if (probedFreq == 0) {
		// Fallback table — mirrors DAT_00081140[refFreqIdx * 0x24], first uint32 per entry.
		static const uint32_t kDefaultCDClkFreq[] = {
			ICL_CDCLK_FREQ_648_0,   // index 0: 24.0 MHz ref → 648 MHz
			ICL_CDCLK_FREQ_652_8,   // index 1: 19.2 MHz ref → 652.8 MHz
			ICL_CDCLK_FREQ_652_8,   // index 2: 38.4 MHz ref → 652.8 MHz
		};
		probedFreq = (refFreqIdx < arrsize(kDefaultCDClkFreq))
			? kDefaultCDClkFreq[refFreqIdx]
			: ICL_CDCLK_FREQ_648_0;
	}

	getMember<uint64_t>(that, 0xe90) = probedFreq;
}
*/
void Gen11::initCDClock(AppleIntel::AppleIntelBaseController *that)
{
	return FunctionCast(initCDClock, callback->oinitCDClock)(that);
}

void Gen11::setCDClockFrequencyOnHotplug(AppleIntel::AppleIntelBaseController *that)
{
	return FunctionCast(setCDClockFrequencyOnHotplug, callback->osetCDClockFrequencyOnHotplug)(that );
}


void Gen11::disableCDClock(AppleIntel::AppleIntelBaseController *that)
{
	FunctionCast(disableCDClock, callback->odisableCDClock)(that );
}

uint8_t Gen11::hwRegsNeedUpdate
		  (AppleIntel::AppleIntelBaseController *that,
		   AppleIntel::AppleIntelFramebuffer *param_1,
		   AppleIntel::AppleIntelDisplayPath *param_2,
		   AppleIntel::CRTCParams *param_3,
		   const IODetailedTimingInformationV2 *param_4,
		   AppleIntel::SCALERPARAMS *param_5)
{
	// ADL-P DC exit: restore power wells and context before Apple writes registers.
	if (NGreen::callback->dmcIsAdlp)
		NGreen::callback->adlpDcExit("hwRNU");

	// param_3 is the pending CRTCParams built by SetupParams.
	if (!NGreen::callback->isRealTGL && param_3) {
		auto *params = param_3;

		// V97P: clear bit[16] in TRANS_DDI_FUNC_CTL.
		// Apple's SetupParams sets bit16 as a port-type flag; UEFI/HW never sets it.
		// Without this the DDI FUNC_CTL compare fires and triggers a full modeset
		// that disrupts the already-trained 4-lane eDP link → black screen.
		if (params->TRANS_DDI_FUNC_CTL & (1u << 16)) {
			static int v97PCount = 0;
			if (v97PCount < 12) {
				v97PCount++;
				SYSLOG("ngreen", "V97P[%d]: CRTCParams TRANS_DDI_FUNC_CTL 0x%x -> 0x%x",
					   v97PCount, params->TRANS_DDI_FUNC_CTL,
					   params->TRANS_DDI_FUNC_CTL & ~(1u << 16));
			}
			params->TRANS_DDI_FUNC_CTL &= ~(1u << 16);
		}

		// V97C: align pending TRANS_CONF with the live HW value.
		// paramsFbCompare logs "TRANS_CONF 0xc0000000->0xc0000024": HW has bits[5,2]
		// clear (UEFI default), Apple wants to set them (interlace/depth config).
		// Writing those bits to an active pipe causes a transient signal disruption
		// that sets the panel's DPCD InterLane Alignment Lost bit (0x202=0x80),
		// which checkLinkStatus detects ~10 s later and tears down the display.
		// Fix: replace the pending TRANS_CONF with the current HW register value so
		// paramsFbCompare sees no change and the partial pipe update is suppressed.
		// PIPE_CONF_A (= TRANS_CONF in ICL+) = 0x70008.
		// NOTE: 0x60008 is TRANS_HSYNC_A (horizontal sync timing) — do NOT use that.
		const uint32_t hwTransConf = NGreen::callback->readReg32(0x70008);
		if (params->TRANS_CONF != hwTransConf) {
			static int v97CCount = 0;
			if (v97CCount < 12) {
				v97CCount++;
				SYSLOG("ngreen", "V97C[%d]: CRTCParams TRANS_CONF 0x%x -> HW 0x%x (suppressed pipe update)",
					   v97CCount, params->TRANS_CONF, hwTransConf);
			}
			params->TRANS_CONF = hwTransConf;
		}

		// V300 REVERTED — was a memory-write hack zeroing CRTCParams[+0xE8]/[+0xEC] to
		// kill DSC engine select bits. Per jalavoui's feedback ("stop hacking memory
		// writes, fix the origin not the destination"), DSC should be controlled at
		// getDPCDInfo / Info.plist FeatureControl level, not patched after-the-fact in
		// the CRTCParams struct. Our Info.plist already sets DSCSupport=0/DSCCapReporting=0
		// (confirmed by fb.log "DSC supported: 0" / "DSC Caps Reporting enabled: 0"),
		// so if Apple still calls setupDSCEngineParams the path is somewhere downstream
		// that ignores FeatureControl — needs investigation at the origin function, not
		// here. Linux on this hardware: VBT Port A DSC:0, link runs "DSC off" → DSC was
		// likely a wrong hypothesis for the fragmentation symptom in the first place.
	}

	static int CCount = 0;
	CCount++;
	SYSLOG("ngreen", "V97C[%d]: hwRegsNeedUpdate called", CCount);
	// Return the original result so that register reprogramming proceeds normally.
	// The lane count mismatch (4→2) that previously broke the display is now fixed
	// by the computeLaneCount hook forcing 4 lanes.  Without register updates, the
	// plane surface address and stride never get written, leaving the stale BIOS
	// framebuffer on screen (grey/black vertical bars with only cursor visible).
	return FunctionCast(hwRegsNeedUpdate, callback->ohwRegsNeedUpdate)(that, param_1, param_2, param_3, param_4, param_5);
}

int Gen11::wrapHwSetupMemory(AppleIntel::AppleIntelBaseController *that, AppleIntel::AppleIntelFramebuffer *fb, AppleIntel::AppleIntelDisplayPath *displayPath, AppleIntel::CRTCParams *params, bool isAperture)
{
	int ret = FunctionCast(wrapHwSetupMemory, callback->ohwSetupMemory)(that, fb, displayPath, params, isAperture);

	// V201: post-hwSetupMemory probe — read 32 bytes at the SURF GTT offset through BAR2.
	// SURF address is at fb+0x4330 (per hwSetupMemory decomp at offset 0x6E046).
	// If buffer is all zeros → freshly-allocated IOBufferMemoryDescriptor pages, nothing
	// has rendered into them yet. If non-zero → some path filled them (EFI GOP carry-over,
	// CoreDisplay handoff blit, or other).
	static int v201Count = 0;
	if (v201Count < 8 && NGreen::callback) {
		uint32_t surfAddr = getMember<uint32_t>(fb, 0x4330);
		uint32_t fbSize   = getMember<uint32_t>(fb, 0x4334);
		uint8_t  fbIdx    = getMember<uint8_t>(fb,  0x4288);
		uint8_t  yTileFlg = getMember<uint8_t>(fb,  0x4A18);

		NGreen::callback->setApertureIfNecessary();
		// Sample top-left and screen-center for 2560×1600 BGRA layout.
		uint32_t tlCtr = 0xDEADBEEF, ctr = 0xDEADBEEF, bar = 0xDEADBEEF, mid = 0xDEADBEEF;
		if (NGreen::callback->aperturePtr && surfAddr + 0xA00000 <= NGreen::callback->apertureLen) {
			volatile uint32_t *fb32 = NGreen::callback->aperturePtr + (surfAddr / sizeof(uint32_t));
			tlCtr = fb32[0];
			ctr   = fb32[0x7D2800 / 4];  // (1280,800) Apple logo center
			bar   = fb32[0x9C7800 / 4];  // (1280,1000) loading-bar row
			mid   = fb32[0x7D1A00 / 4];  // (640,800) mid-left of logo area
		}
		// V201P: read GGTT PTE for the surface page — verifies display controller can access it.
		// GGTT PTE LO/HI at GEN8_GGTT_PTE_BASE(0x800000) + page*8. Valid: bit0=present, bit3=LLC.
		uint32_t pteLo = 0, pteHi = 0;
		if (surfAddr && NGreen::callback->mmioValid()) {
			uint32_t surfPage = surfAddr >> 12;
			pteLo = NGreen::callback->readReg32(GGTT_PTE_LO(surfPage));
			pteHi = NGreen::callback->readReg32(GGTT_PTE_HI(surfPage));
		}
		v201Count++;
		SYSLOG("ngreen", "V201[%d]: hwSetupMemory ret=0x%x fb=%p surf=0x%x size=0x%x idx=%u tile=%u | tl=%08x ctr=%08x bar=%08x mid=%08x | PTE=%08x:%08x(present=%d llc=%d)",
			   v201Count, ret, fb, surfAddr, fbSize, fbIdx, yTileFlg, tlCtr, ctr, bar, mid,
			   pteHi, pteLo, pteLo & 1, (pteLo >> 3) & 1);
	}

	return ret;
}

// HW hooks

static void v44ScheduleBundleLog(void *accelInstance, unsigned delayMs);
static void v45ScheduleDelayedCheck(void *accelInstance, unsigned delayMs);

unsigned long Gen11::start(void *that,void  *param_1)
{
	// V44: Configurable scheduler type.
	// populateAccelConfig reads "GraphicsSchedulerSelect" from the IORegistry.
	// Types: 3=IGGuC (firmware), 4=IGScheduler4, 5=IGScheduler5 (host preemptive).
	// Priority: boot-arg "ngreenSched=N" > Info.plist "SchedulerType" > default.
	// V52: Default depends on platform — real TGL uses GuC (3), RPL uses Host (5).
	auto *service = static_cast<IOService *>(that);
	{
		int schedType = NGreen::callback->isRealTGL ? 3 : 5;
		
		// 1. Check boot-arg first (highest priority)
		int bootArgSched = 0;
		if (PE_parse_boot_argn("ngreenSched", &bootArgSched, sizeof(bootArgSched)) && bootArgSched >= 3 && bootArgSched <= 5) {
			schedType = bootArgSched;
			SYSLOG("ngreen", "V44: scheduler type %d from boot-arg ngreenSched", schedType);
		} else {
			// 2. Check NootedGreen's own IORegistry properties (from Info.plist)
			auto *myService = OSDynamicCast(IOService, reinterpret_cast<OSObject *>(param_1));
			if (myService) {
				auto *stProp = OSDynamicCast(OSNumber, myService->getProperty("SchedulerType"));
				if (stProp) {
					int val = (int)stProp->unsigned32BitValue();
					if (val >= 3 && val <= 5) {
						schedType = val;
						SYSLOG("ngreen", "V44: scheduler type %d from Info.plist SchedulerType", schedType);
					}
				}
			}
			if (schedType == 5) {
				SYSLOG("ngreen", "V44: using default scheduler type 5 (host preemptive)");
			}
		}
		
		auto *schedNum = OSNumber::withNumber(static_cast<unsigned long long>(schedType), 32);
		if (schedNum) {
			service->setProperty("GraphicsSchedulerSelect", schedNum);
			schedNum->release();
			SYSLOG("ngreen", "V44: injected GraphicsSchedulerSelect=%d", schedType);
		}
	}

	// Inject MultiForceWakeSelect=1 into Development dictionary.
	// This tells the accelerator to use SafeForceWakeMultithreaded (which we hook)
	// instead of the framebuffer's SafeForceWake (which fails on RPL-P with ACK=0).
	// V52: Only needed on RPL — real TGL's native ForceWake works fine.
	if (!NGreen::callback->isRealTGL) {
	auto *devDict = OSDynamicCast(OSDictionary, service->getProperty("Development"));
	if (devDict) {
		auto *newDevDict = OSDictionary::withDictionary(devDict);
		if (newDevDict) {
			auto *num = OSNumber::withNumber(1ULL, 32);
			if (num) {
				newDevDict->setObject("MultiForceWakeSelect", num);
				num->release();
			}
			service->setProperty("Development", newDevDict);
			newDevDict->release();
			SYSLOG("ngreen", "Injected MultiForceWakeSelect=1 into Development dict");
		}
	} else {
		SYSLOG("ngreen", "No Development dict found, creating one with MultiForceWakeSelect=1");
		auto *newDevDict = OSDictionary::withCapacity(4);
		if (newDevDict) {
			auto *num = OSNumber::withNumber(1ULL, 32);
			if (num) {
				newDevDict->setObject("MultiForceWakeSelect", num);
				num->release();
			}
			service->setProperty("Development", newDevDict);
			newDevDict->release();
		}
	}
	// xe/noccs (19/10): desliga a compressão de render (CCS) do Gen12. Com ngreenSched=4 o SecurityAgent
	// panicou em resolve_ccs <- submitCCSResolve <- enableRenderCompressionWithAccelTask (AUX-TT não configurada no RPL).
	// populateAccelConfig lê RenderCompressionMode (padrão 1) e AllowIOSurfaceRenderCompression via utilGetProperty:
	// zera os dois no dicionário Development e no nível de cima do serviço.
	{
		auto *dd = OSDynamicCast(OSDictionary, service->getProperty("Development"));
		auto *nd = dd ? OSDictionary::withDictionary(dd) : OSDictionary::withCapacity(4);
		auto *zero = OSNumber::withNumber(0ULL, 32);
		if (nd && zero) {
			nd->setObject("RenderCompressionMode", zero);
			nd->setObject("AllowIOSurfaceRenderCompression", zero);
			service->setProperty("Development", nd);
			service->setProperty("RenderCompressionMode", zero);
			service->setProperty("AllowIOSurfaceRenderCompression", zero);
			SYSLOG("ngreen", "XE-NOCCS: RenderCompressionMode=0 AllowIOSurfaceRenderCompression=0 injetados");
		}
		if (nd) nd->release();
		if (zero) zero->release();
	}
	// xe/nogva (20/10): a personality TGL anuncia decodificador/escalador de vídeo por hardware (IOGVACodec/IOGVAScaler
	// "Gen10", capacidades HEVC). No RPL o VTDecompressionSession de hardware falha (-12913) e qualquer sessão de
	// software que peça formato de saída morre em -6680 (conversão pelo escalador GVA) -> YouTube "Ocorreu um erro".
	// Provado com ~/xe/build/xe-vttest: software sem formato decodifica certo. Sem estas chaves o VideoToolbox usa
	// decodificador e conversão por software. -ngreengva mantém o anúncio.
	if (!checkKernelArgument("-ngreengva")) {
		static const char *const gvaKeys[] = {"IOGVACodec", "IOGVAScaler", "IOGVABGRAEnc",
		                                      "IOGVAHEVCDecodeCapabilities", "IOGVAHEVCEncodeCapabilities"};
		for (auto *k : gvaKeys) service->removeProperty(k);
		SYSLOG("ngreen", "XE-NOGVA: anúncio de vídeo por hardware (IOGVA*) removido");
	} else {
		// xe/gvaid (20/10): com o anúncio mantido, o AppleGVA recusava o plugin ("PluginConnect: Plugin ID not a match
		// 1080040"): a personality TGL diz IOVARendererID 0x1080040, mas o AppleIntelTGLGraphicsVADriver se identifica
		// como 0x1080080 (o ID do ICL; o driver TGL veio do código do ICL) e o AppleGVA só conhece esse. Anuncia 0x1080080.
		auto *rid = OSNumber::withNumber(0x1080080ULL, 32);
		if (rid) { service->setProperty("IOVARendererID", rid); rid->release(); }
		SYSLOG("ngreen", "XE-GVAID: IOVARendererID = 0x1080080 (ID do AppleIntelTGLGraphicsVADriver)");
		// xe/gvahevc (21/10): o AppleGVA só oferece HEVC por hardware se o acelerador tiver IOGVAHEVCDecode (o kext ICL
		// do Tahoe traz "2" no Info.plist; a personality TGL só tem IOGVAHEVCDecodeCapabilities). Sem ela a sessão HEVC
		// falha com -12906. Com o plugin VA corrigido (macos/vadriver), a decodificação HEVC sai idêntica à de software.
		service->setProperty("IOGVAHEVCDecode", "2");
		SYSLOG("ngreen", "XE-GVAHEVC: IOGVAHEVCDecode = \"2\"");
	}
	} else {
		SYSLOG("ngreen", "V52: Real TGL — skipping MultiForceWakeSelect override");
	}

	// ── V29: Apply GT workarounds + GGTT PTE diagnostics ──
	SYSLOG("ngreen", "Pre-start: acquiring ForceWake for GT workarounds");
	NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 1);
	IODelay(1000);
	
	uint32_t fwAck = 0;
	uint64_t fwNow = 0, fwDeadline = 0;
	clock_interval_to_deadline(50, kMillisecondScale, &fwDeadline);
	for (clock_get_uptime(&fwNow); fwNow < fwDeadline; clock_get_uptime(&fwNow)) {
		fwAck = NGreen::callback->readReg32(FORCEWAKE_ACK_RENDER_GEN9);
		if (fwAck & 1) break;
	}
	SYSLOG("ngreen", "Pre-start ForceWake ACK: 0x%x %s", fwAck, (fwAck & 1) ? "OK" : "TIMEOUT");
	
	// ── V29 NEW: Pre-start GGTT PTE dump ──
	// Dump first few GGTT PTEs to verify format
	SYSLOG("ngreen", "GGTT PTE format check (first 4 pages):");
	for (int i = 0; i < 4; i++) {
		SYSLOG("ngreen", "  GGTT[%d]=0x%08x:%08x", i,
			NGreen::callback->readReg32(GGTT_PTE_HI(i)),
			NGreen::callback->readReg32(GGTT_PTE_LO(i)));
	}
	// Dump PTEs around the HWS page area (GGTT offset 0x40004000 → page 0x40004)
	// But that PTE would be at 0x800000 + 0x40004*8 = 0xA00020 — check if within BAR
	// Instead, dump some PTEs in the driver's active range
	SYSLOG("ngreen", "GGTT PTE near page 0x100:");
	for (int i = 0x100; i < 0x104; i++) {
		SYSLOG("ngreen", "  GGTT[0x%x]=0x%08x:%08x", i,
			NGreen::callback->readReg32(GGTT_PTE_HI(i)),
			NGreen::callback->readReg32(GGTT_PTE_LO(i)));
	}
	
	// ── V29 NEW: Context and engine config ──
	SYSLOG("ngreen", "RCS CTX_SIZE=0x%x CCID=0x%x CTX_CTRL=0x%x",
		NGreen::callback->readReg32(RING_CTX_SIZE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CCID(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CTX_CTRL(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS MI_MODE=0x%x RING_MODE=0x%x",
		NGreen::callback->readReg32(RING_MI_MODE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_MODE(RENDER_RING_BASE)));
	
	{
		uint32_t errPreStart = NGreen::callback->readReg32(ERROR_GEN6);
		SYSLOG("ngreen", "Pre-start ERROR_GEN6=0x%x", errPreStart);

		// V55: Full TLB flush + 5x clear BEFORE start().
		// Simple W1C (0x37 -> 0x37 confirmed in log) cannot clear sticky TLB-fault bits.
		// Post-start V55 proved: TLB invalidation via 0xcee8 + 5x clear succeeds (0x37 -> 0x0).
		// Moving the same sequence here so Apple's getBlit3DContext sees a clean ERROR_GEN6.
		// ForceWake is already held at this point (Pre-start ForceWake ACK logged above).
		if (errPreStart && !NGreen::callback->isRealTGL) {
			uint32_t ccidBefore = NGreen::callback->readReg32(RING_CCID(RENDER_RING_BASE));

			// 1. GDRST render domain — evicts the stale EFI/firmware context (CCID=0x80)
			// that keeps re-asserting ERROR_GEN6 into memory invalid in macOS address space.
			// TLB flush + W1C alone can't win: the broken context re-fires errors immediately.
			NGreen::callback->writeReg32(GEN6_GDRST, GEN6_GRDOM_RENDER);
			uint32_t gdrstVal = GEN6_GRDOM_RENDER;
			int gdrstPoll = 0;
			while ((gdrstVal & GEN6_GRDOM_RENDER) && gdrstPoll < 2000) {
				gdrstVal = NGreen::callback->readReg32(GEN6_GDRST);
				gdrstPoll++;
			}
			uint32_t ccidAfter = NGreen::callback->readReg32(RING_CCID(RENDER_RING_BASE));
			SYSLOG("ngreen", "V55G: pre-start GDRST poll=%d gdrst=0x%x CCID 0x%x->0x%x ERR->0x%x",
				gdrstPoll, gdrstVal, ccidBefore, ccidAfter,
				NGreen::callback->readReg32(ERROR_GEN6));

			// Re-acquire ForceWake — GDRST resets the render engine's FW tracking
			NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 1);
			IODelay(1000);

			// 2. Invalidate RCS + BCS TLBs (GEN12_GUC_TLB_INV_CR = 0xcee8)
			NGreen::callback->writeReg32(0xcee8, 0x1);   // RCS TLBs
			IODelay(500);
			NGreen::callback->writeReg32(0xcee8, 0x4);   // BCS TLBs
			IODelay(500);
			SYSLOG("ngreen", "V55: Pre-start TLB invalidation requested (RCS+BCS)");

			// 3. 5x clear loop — should now succeed with stale context evicted
			uint32_t errLoop = NGreen::callback->readReg32(ERROR_GEN6);
			for (int i = 0; i < 5 && errLoop; i++) {
				NGreen::callback->writeReg32(ERROR_GEN6, errLoop);
				IODelay(200);
				errLoop = NGreen::callback->readReg32(ERROR_GEN6);
			}
			SYSLOG("ngreen", "V55: Pre-start ERROR_GEN6 after GDRST + TLB flush + 5x clear = 0x%x", errLoop);

			// 4. Clear per-engine EIR
			uint32_t rcsEir = NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE));
			uint32_t bcsEir = NGreen::callback->readReg32(RING_EIR(BLT_RING_BASE));
			if (rcsEir) NGreen::callback->writeReg32(RING_EIR(RENDER_RING_BASE), rcsEir);
			if (bcsEir) NGreen::callback->writeReg32(RING_EIR(BLT_RING_BASE), bcsEir);

			uint32_t tlbStatus = NGreen::callback->readReg32(0xceec);
			SYSLOG("ngreen", "V55: Pre-start TLB_INV done status = 0x%x", tlbStatus);
		}
	}
	
	// GT workarounds
	uint32_t misccpctl = NGreen::callback->readReg32(GEN7_MISCCPCTL);
	misccpctl &= ~GEN12_DOP_CLOCK_GATE_RENDER_ENABLE;
	NGreen::callback->writeReg32(GEN7_MISCCPCTL, misccpctl);
	NGreen::callback->wa_write_or(VDBOX_CGCTL3F10(RENDER_RING_BASE), IECPUNIT_CLKGATE_DIS);
	NGreen::callback->wa_write_or(VDBOX_CGCTL3F10(BLT_RING_BASE), IECPUNIT_CLKGATE_DIS);
	NGreen::callback->wa_write_or(VDBOX_CGCTL3F10(GEN11_VEBOX_RING_BASE), IECPUNIT_CLKGATE_DIS);
	NGreen::callback->wa_mcr_write_or(GEN10_DFR_RATIO_EN_AND_CHICKEN, DFR_DISABLE);
	NGreen::callback->wa_masked_en(GEN11_COMMON_SLICE_CHICKEN3,
			GEN12_DISABLE_CPS_AWARE_COLOR_PIPE);
	NGreen::callback->wa_masked_field_set(GEN8_CS_CHICKEN1,
				GEN9_PREEMPT_GPGPU_LEVEL_MASK,
				GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL);
	uint32_t mcr = GEN8_MCR_SLICE(0) | GEN8_MCR_SUBSLICE(0);
	uint32_t mcr_mask = GEN8_MCR_SLICE_MASK | GEN8_MCR_SUBSLICE_MASK;
	NGreen::callback->wa_write_clr_set(GEN8_MCR_SELECTOR, mcr_mask, mcr);
	// V162: PERCTX_PREEMPT_CTRL (bit 14) is a TGL-specific workaround that bakes
	// into the execlist context image template.  On RPL the bit freezes the EU
	// thread dispatcher on the first Metal context-image DMA restore.  Only enable
	// it on genuine TGL hardware.
	if (NGreen::callback->isRealTGL) {
		NGreen::callback->wa_masked_en(GEN7_FF_SLICE_CS_CHICKEN1,
				GEN9_FFSC_PERCTX_PREEMPT_CTRL);
	}
	
	SYSLOG("ngreen", "Pre-start: GT workarounds applied, calling original start()");
	
	// Release Render ForceWake
	NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 0);
	
	// V42: Save accelerator instance for child enumeration in hangcheck
	callback->accelInstance = that;
	
	// V54: Pre-enable Master IRQ BEFORE calling original start().
	// V53 proved that the TGL driver never enables GFX_MSTR_IRQ bit 31 on RPL.
	// Without it, completion interrupts never reach the CPU and start() hangs
	// for ~15s until our hangcheck accidentally re-enabled it.
	if (!NGreen::callback->isRealTGL) {
		// Enable Master IRQ (bit 31)
		NGreen::callback->writeReg32(GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
		IODelay(100);
		SYSLOG("ngreen", "V54: Pre-enabled Master IRQ: GFX_MSTR_IRQ=0x%x",
			NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ));
		
		// V65: Pre-enable RCS0+BCS tier-1 interrupt routing BEFORE ring init.
		// V64 proved GEN11_RENDER_COPY_INTR_ENABLE had RCS0 (bit 0) DISABLED,
		// causing scheduler to never receive completion interrupts → timeout → dead engine.
		// Must be set BEFORE ring activates (T+8-10s) to prevent scheduler timeout.
		uint32_t rcIntrPre = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t wantBits = getV65Tier1WantBits(NGreen::callback->isRealTGL);
		uint32_t newEn = rcIntrPre | wantBits;
		NGreen::callback->writeReg32(GEN11_RENDER_COPY_INTR_ENABLE, newEn);
		// Also unmask RCS context-switch + user interrupts at tier-2
		uint32_t rcsMaskPre = NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK);
		uint32_t wantUnmasked = GT_CONTEXT_SWITCH_INTERRUPT | GT_RENDER_USER_INTERRUPT;
		NGreen::callback->writeReg32(GEN11_RCS0_RSVD_INTR_MASK, rcsMaskPre & ~wantUnmasked);
		// Pre-clear any stale ERROR_GEN6 to prevent error handler interference
		uint32_t errPre = NGreen::callback->readReg32(ERROR_GEN6);
		if (errPre) NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
		SYSLOG("ngreen", "V65: Pre-start interrupt fix: RC_INTR_EN 0x%x->0x%x, RCS_MASK 0x%x->0x%x, ERR=0x%x",
			rcIntrPre, NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE),
			rcsMaskPre, NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK), errPre);
	}
	
	// V54: Schedule IRQ watchdog timer — re-enables Master IRQ every 2s during start().
	// The driver's interrupt setup may disable it during init; we keep re-enabling.
	// Real TGL keeps native behavior; spoof path always needs this.
	if (!NGreen::callback->isRealTGL) {
		auto timerCall = thread_call_allocate(v54IrqWatchdog, nullptr);
		if (timerCall) {
			uint64_t deadline;
			clock_interval_to_deadline(2, kSecondScale, &deadline);
			thread_call_enter_delayed(timerCall, deadline);
			SYSLOG("ngreen", "V54: IRQ watchdog armed — will re-enable Master IRQ every 2s");
		}
	}
	
	// V116: Remap GGTT page 0 to a safe dummy page BEFORE original start().
	// Root cause of package-wide MCE (Banks 9/11/15, all CPUs, addr 0x3e800000):
	// GGTT[0] PTE points to stolen memory base. Any GPU access to VA=0
	// (ring buffer at addr 0, PLANE_SURF=0, etc.) reads stolen mem → LLC UCE → MCE.
	// Fix: redirect GGTT[0] to a wired zero-page so stray VA-0 reads are harmless.
	// Real TGL keeps native GGTT mapping; spoof path needs this MCE guard.
	if (!NGreen::callback->isRealTGL) {
		if (!v116DummyBuf) {
			v116DummyBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
				kernel_task,
				kIOMemoryPhysicallyContiguous | kIODirectionInOut,
				4096,
				0x00000000FFFFF000ULL);  // keep below 4GB for 32-bit GGTT addressing
			if (v116DummyBuf && v116DummyBuf->prepare() == kIOReturnSuccess) {
				v116DummyPhys = v116DummyBuf->getPhysicalAddress();
				auto *m = v116DummyBuf->createMappingInTask(kernel_task, 0, kIOMapAnywhere, 0, 4096);
				if (m) { bzero((void *)m->getVirtualAddress(), 4096); m->release(); }
				SYSLOG("ngreen", "V116: dummy page allocated phys=0x%llx",
					   (unsigned long long)v116DummyPhys);
			} else {
				OSSafeReleaseNULL(v116DummyBuf);
				SYSLOG("ngreen", "V116: dummy page alloc/prepare FAILED");
			}
		}
		if (v116DummyPhys && NGreen::callback->mmioValid()) {
			// PTE format (Gen8+): bits[47:12]=phys PFN, bit[3]=LLC-coherent, bit[0]=present
			uint32_t pteLo = (uint32_t)(v116DummyPhys & 0xFFFFF000ULL) | 0x9;  // LLC | present
			uint32_t pteHi = (uint32_t)(v116DummyPhys >> 32);
			uint32_t oldLo = NGreen::callback->readReg32(GGTT_PTE_LO(0));
			uint32_t oldHi = NGreen::callback->readReg32(GGTT_PTE_HI(0));
			NGreen::callback->writeReg32(GGTT_PTE_LO(0), pteLo);
			NGreen::callback->writeReg32(GGTT_PTE_HI(0), pteHi);
			NGreen::callback->writeReg32(0x101008, 0x1);  // GGTT TLB flush
			SYSLOG("ngreen", "V116: GGTT[0] 0x%08x:%08x -> 0x%08x:%08x (dummy phys=0x%llx)",
				   oldHi, oldLo, pteHi, pteLo, (unsigned long long)v116DummyPhys);
		}
	}

	auto ret= FunctionCast(start, callback->ostart)(that,param_1);

	// V221: Signal waitForStamp hook that the GFX interrupt handler is now installed.
	Gen11::gGfxAccelStartDone = true;
	SYSLOG("ngreen", "V221: GFX start() complete — gGfxAccelStartDone=true ret=%lu", ret);

	// V65: IMMEDIATELY after original start() returns, re-enable RCS0 interrupts.
	// Apple's init code may have overwritten our pre-start settings.
	// This is our earliest opportunity after ring activation.
	if (!NGreen::callback->isRealTGL) {
		uint32_t rcIntrPost = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t wantBits = getV65Tier1WantBits(NGreen::callback->isRealTGL);
		uint32_t newEn = rcIntrPost | wantBits;
		NGreen::callback->writeReg32(GEN11_RENDER_COPY_INTR_ENABLE, newEn);
		// Re-unmask tier-2
		uint32_t rcsMaskPost = NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK);
		uint32_t wantUnmasked = GT_CONTEXT_SWITCH_INTERRUPT | GT_RENDER_USER_INTERRUPT;
		NGreen::callback->writeReg32(GEN11_RCS0_RSVD_INTR_MASK, rcsMaskPost & ~wantUnmasked);
		// Re-enable Master IRQ (may have been toggled during start)
		NGreen::callback->writeReg32(GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
		// Clear any ERROR_GEN6 that fired during ring init
		uint32_t errPost = NGreen::callback->readReg32(ERROR_GEN6);
		if (errPost) NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
		SYSLOG("ngreen", "V65: Post-start interrupt fix: RC_INTR_EN 0x%x->0x%x, RCS_MASK 0x%x->0x%x, ERR=0x%x",
			rcIntrPost, NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE),
			rcsMaskPost, NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK), errPost);
		// V507: GFX_RUN_LIST_ENABLE must be set in the live RING_MODE register before any
		// ExecList submission — Apple's init does not set it on RPL-P (snapshots 0x0 into
		// the MI_LRI replay list, leaving ExecList mode off permanently).
		// RING_MODE uses the Intel masked-write convention: hi16=write-enable mask, lo16=values.
		// Writing 0x80008000 sets bit15 (GFX_RUN_LIST_ENABLE) without touching other bits.
		{
			uint32_t rmPre = NGreen::callback->readReg32(RING_MODE(RENDER_RING_BASE));
			NGreen::callback->writeReg32(RING_MODE(RENDER_RING_BASE), 0x80008000u);
			uint32_t rmPost = NGreen::callback->readReg32(RING_MODE(RENDER_RING_BASE));
			SYSLOG("ngreen", "V507: RING_MODE 0x%08x → 0x80008000 → readback 0x%08x (GFX_RUN_LIST_ENABLE=%d)",
				rmPre, rmPost, !!(rmPost & (1u << 15)));
		}
	}
	
	// ── V29: Post-start diagnostics ──
	NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 1);
	NGreen::callback->writeReg32(FORCEWAKE_BLITTER_GEN9, (1 << 16) | 1);
	IODelay(1000);
	
	SYSLOG("ngreen", "start() returned %d", ret);
	
	// V43: Scheduler type diagnostic — read field_1190 bits 23-25
	{
		uint64_t caps1190 = getMember<uint64_t>(that, 0x1190);
		uint32_t schedType = (caps1190 >> 23) & 7;
		uint32_t devType1120 = getMember<uint32_t>(that, 0x1120);
		SYSLOG("ngreen", "V43: field_1190=0x%llx schedType=%u (3=GuC,4=Sched4,5=Host) devType_1120=%u",
			   (unsigned long long)caps1190, schedType, devType1120);
		
		// Read encodeFailureStack entries (offsets 0x1c30-0x1c4c, count at 0x1c50)
		uint32_t failCount = getMember<uint32_t>(that, 0x1c50);
		SYSLOG("ngreen", "V43: encodeFailureStack count=%u", failCount);
		for (uint32_t i = 0; i < failCount && i < 8; i++) {
			uint32_t code = getMember<uint32_t>(that, 0x1c30 + i * 4);
			SYSLOG("ngreen", "V43:   failureStack[%u] = 0x%x", i, code);
		}
		
		// Read the scheduler pointer — check if scheduler was actually created
		// From start() disassembly, the scheduler is stored via virtual call;
		// try reading field at 0xe00 (common IOAccelF2 scheduler offset)
		uint64_t schedPtr = getMember<uint64_t>(that, 0xe00);
		SYSLOG("ngreen", "V43: scheduler ptr (0xe00) = 0x%llx %s",
			   (unsigned long long)schedPtr, schedPtr ? "EXISTS" : "NULL");
		
		// V44: Check if accelerator registered with framebuffer controller
		// Field 0x128c: set to 1 by registerWithFramebufferController() on success
		// Field 0xde8: callback handler pointer set during FB registration
		uint8_t fbRegistered = getMember<uint8_t>(that, 0x128c);
		uint64_t fbCallbackPtr = getMember<uint64_t>(that, 0xde8);
		SYSLOG("ngreen", "V44: fbRegistered(0x128c)=%u fbCallbackPtr(0xde8)=0x%llx",
			   fbRegistered, (unsigned long long)fbCallbackPtr);
		
		// V44: Dump key IORegistry properties of the accelerator service
		auto *accelSvc = static_cast<IOService *>(that);
		auto *metalProp = OSDynamicCast(OSString, accelSvc->getProperty("MetalPluginName"));
		auto *glProp    = OSDynamicCast(OSString, accelSvc->getProperty("IOGLBundleName"));
		auto *dvdProp   = OSDynamicCast(OSString, accelSvc->getProperty("IODVDBundleName"));
		auto *vaRIDProp = OSDynamicCast(OSNumber, accelSvc->getProperty("IOVARendererID"));
		const char *metalStr = (metalProp && metalProp->getCStringNoCopy()) ? metalProp->getCStringNoCopy() : "MISSING";
		const char *glStr = (glProp && glProp->getCStringNoCopy()) ? glProp->getCStringNoCopy() : "MISSING";
		const char *dvdStr = (dvdProp && dvdProp->getCStringNoCopy()) ? dvdProp->getCStringNoCopy() : "MISSING";
		SYSLOG("ngreen", "V44: MetalPlugin=%s GL=%s VARendID=0x%x",
			   metalStr,
			   glStr,
			   vaRIDProp ? (uint32_t)vaRIDProp->unsigned32BitValue() : 0);
		SYSLOG("ngreen", "V44: DVD=%s MTL_TGL=%d GL_TGL=%d VA_TGL=%d",
			   dvdStr,
			   !strcmp(metalStr, "AppleIntelTGLGraphicsMTLDriver"),
			   !strcmp(glStr, "AppleIntelTGLGraphicsGLDriver"),
			   !strcmp(dvdStr, "AppleIntelTGLGraphicsVADriver"));
	}
	
	// Ring buffer state
	uint32_t rcsHead = NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE));
	uint32_t rcsTail = NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE));
	uint32_t rcsCtl  = NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE));
	uint32_t rcsStart = NGreen::callback->readReg32(RING_START(RENDER_RING_BASE));
	uint32_t rcsHws  = NGreen::callback->readReg32(RING_HWS_PGA(RENDER_RING_BASE));
	SYSLOG("ngreen", "RCS0 HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x",
		rcsHead, rcsTail, rcsCtl, rcsStart);
	SYSLOG("ngreen", "RCS0 HWS_PGA=0x%x HWSTAM=0x%x",
		rcsHws, NGreen::callback->readReg32(RING_HWSTAM(RENDER_RING_BASE)));
	
	SYSLOG("ngreen", "RCS0 ACTHD=0x%x:%08x IPEHR=0x%x IPEIR=0x%x",
		NGreen::callback->readReg32(RING_ACTHD_UDW(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_ACTHD(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_IPEHR(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_IPEIR(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS0 INSTDONE=0x%x INSTPM=0x%x DMA_FADD=0x%x:%08x",
		NGreen::callback->readReg32(RING_INSTDONE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_INSTPM(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_DMA_FADD_UDW(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_DMA_FADD(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS0 EXECLIST_STATUS=0x%x CTX_STATUS_PTR=0x%x",
		NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE)));
	
	// Per-engine error
	SYSLOG("ngreen", "RCS0 EIR=0x%x ESR=0x%x EMR=0x%x",
		NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_ESR(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS0 RING_FAULT=0x%x",
		NGreen::callback->readReg32(RING_FAULT_REG(RENDER_RING_BASE)));
	
	// Global errors
	SYSLOG("ngreen", "ERROR_GEN6=0x%x RING_FAULT(global)=0x%x",
		NGreen::callback->readReg32(ERROR_GEN6),
		NGreen::callback->readReg32(GEN12_RING_FAULT_REG));
	SYSLOG("ngreen", "FAULT_TLB_DATA0=0x%x FAULT_TLB_DATA1=0x%x",
		NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0),
		NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1));
	SYSLOG("ngreen", "GT_INTR_DW0=0x%x DW1=0x%x",
		NGreen::callback->readReg32(GEN11_GT_INTR_DW0),
		NGreen::callback->readReg32(GEN11_GT_INTR_DW1));
	
	// ── V29 NEW: Context and engine config after start ──
	SYSLOG("ngreen", "RCS CTX_SIZE=0x%x CCID=0x%x CTX_CTRL=0x%x",
		NGreen::callback->readReg32(RING_CTX_SIZE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CCID(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CTX_CTRL(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS MI_MODE=0x%x RING_MODE=0x%x",
		NGreen::callback->readReg32(RING_MI_MODE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_MODE(RENDER_RING_BASE)));
	
	// ── V29 NEW: GGTT PTEs for HWS page and ring buffer ──
	// HWS_PGA is a GGTT address; read its PTE to check format
	if (rcsHws) {
		uint32_t hwsPage = rcsHws >> 12;
		SYSLOG("ngreen", "GGTT PTE for HWS (page 0x%x)=0x%08x:%08x", hwsPage,
			NGreen::callback->readReg32(GGTT_PTE_HI(hwsPage)),
			NGreen::callback->readReg32(GGTT_PTE_LO(hwsPage)));
		// Also check adjacent pages
		SYSLOG("ngreen", "GGTT PTE for HWS+1 (page 0x%x)=0x%08x:%08x", hwsPage+1,
			NGreen::callback->readReg32(GGTT_PTE_HI(hwsPage+1)),
			NGreen::callback->readReg32(GGTT_PTE_LO(hwsPage+1)));
	}
	if (rcsStart) {
		uint32_t ringPage = rcsStart >> 12;
		uint32_t rpteHi = NGreen::callback->readReg32(GGTT_PTE_HI(ringPage));
		uint32_t rpteLo = NGreen::callback->readReg32(GGTT_PTE_LO(ringPage));
		SYSLOG("ngreen", "GGTT PTE for RING (page 0x%x)=0x%08x:%08x", ringPage, rpteHi, rpteLo);

	}

	// CSB entries
	uint32_t csp = NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
	SYSLOG("ngreen", "CSB wr_ptr=%d rd_ptr=%d", (csp >> 8) & 0x7, csp & 0x7);
	for (int i = 0; i < 6; i++) {
		SYSLOG("ngreen", "CSB[%d]=0x%x:%08x", i,
			NGreen::callback->readReg32(RING_CONTEXT_STATUS_BUF_HI(RENDER_RING_BASE, i)),
			NGreen::callback->readReg32(RING_CONTEXT_STATUS_BUF(RENDER_RING_BASE, i)));
	}
	
	SYSLOG("ngreen", "ForceWake ACK: Render=0x%x Blitter=0x%x",
		NGreen::callback->readReg32(FORCEWAKE_ACK_RENDER_GEN9),
		NGreen::callback->readReg32(FORCEWAKE_ACK_BLITTER_GEN9));
	
	// BCS full state (now with Blitter ForceWake held!)
	uint32_t bcsHead = NGreen::callback->readReg32(RING_HEAD(BLT_RING_BASE));
	uint32_t bcsTail = NGreen::callback->readReg32(RING_TAIL(BLT_RING_BASE));
	uint32_t bcsCtl  = NGreen::callback->readReg32(RING_CTL(BLT_RING_BASE));
	uint32_t bcsStart = NGreen::callback->readReg32(RING_START(BLT_RING_BASE));
	SYSLOG("ngreen", "BCS HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x",
		bcsHead, bcsTail, bcsCtl, bcsStart);
	SYSLOG("ngreen", "BCS HWS_PGA=0x%x MI_MODE=0x%x",
		NGreen::callback->readReg32(RING_HWS_PGA(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_MI_MODE(BLT_RING_BASE)));
	SYSLOG("ngreen", "BCS ACTHD=0x%x:%08x IPEHR=0x%x IPEIR=0x%x",
		NGreen::callback->readReg32(RING_ACTHD_UDW(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_ACTHD(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_IPEHR(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_IPEIR(BLT_RING_BASE)));
	SYSLOG("ngreen", "BCS INSTDONE=0x%x EIR=0x%x ESR=0x%x EMR=0x%x",
		NGreen::callback->readReg32(RING_INSTDONE(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_EIR(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_ESR(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_EMR(BLT_RING_BASE)));
	SYSLOG("ngreen", "BCS EXECLIST_STATUS=0x%x CTX_STATUS_PTR=0x%x",
		NGreen::callback->readReg32(RING_EXECLIST_STATUS(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(BLT_RING_BASE)));
	
	// V36: Dump interrupt enable/mask state to verify GT interrupts are configured
	SYSLOG("ngreen", "IRQ: GFX_MSTR_IRQ=0x%x DISPLAY_INT_CTL=0x%x",
		NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ),
		NGreen::callback->readReg32(GEN11_DISPLAY_INT_CTL));
	SYSLOG("ngreen", "IRQ: RENDER_COPY_INTR_EN=0x%x VCS_VECS_INTR_EN=0x%x",
		NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE),
		NGreen::callback->readReg32(GEN11_VCS_VECS_INTR_ENABLE));
	SYSLOG("ngreen", "IRQ: RCS0_RSVD_MASK=0x%x BCS_RSVD_MASK=0x%x",
		NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK),
		NGreen::callback->readReg32(GEN11_BCS_RSVD_INTR_MASK));
	
	// V42: Log pending GT interrupts — do NOT clear (W1C would discard the tier-2 IIR
	// user-interrupt for the stamp(8,3) that V221 faked; the now-installed handler needs
	// to service it naturally so the cpu-side stamp counter is actually bumped).
	uint32_t gt0 = NGreen::callback->readReg32(0x190018);
	if (gt0) {
		SYSLOG("ngreen", "V42: GT_INTR_DW0 pending=0x%x (leaving for handler)", gt0);
	}
	
	// V42: Enumerate accelerator children to check if IOAccelDevice was created
	{
		auto *service = static_cast<IOService *>(that);
		OSIterator *iter = service->getClientIterator();
		int childCount = 0;
		if (iter) {
			OSObject *obj;
			while ((obj = iter->getNextObject())) {
				auto *child = OSDynamicCast(IOService, obj);
				if (child) {
					SYSLOG("ngreen", "V42: accel child[%d]: %s", childCount, child->getName());
					childCount++;
				}
			}
			iter->release();
		}
		SYSLOG("ngreen", "V42: accelerator has %d children after start()", childCount);
	}
	
	// ── V45: IORegistry state & service visibility diagnostics ──
	{
		auto *accelSvc = static_cast<IOService *>(that);
		
		// 1. IOService state bitmask — verify the service is registered, matched, published
		uint64_t svcState = accelSvc->getState();
		SYSLOG("ngreen", "V45: getState()=0x%llx (reg=%d match=%d pub=%d fmatch=%d inact=%d)",
			   (unsigned long long)svcState,
			   !!(svcState & 0x02),  // kIOServiceRegisteredState
			   !!(svcState & 0x04),  // kIOServiceMatchedState
			   !!(svcState & 0x08),  // kIOServiceFirstPublishState
			   !!(svcState & 0x10),  // kIOServiceFirstMatchState
			   !!(svcState & 0x01)); // kIOServiceInactiveState
		
		// 2. Provider chain — verify accelerator is attached to IGPU PCI device
		auto *provider = accelSvc->getProvider();
		if (provider) {
			SYSLOG("ngreen", "V45: provider name=%s class=%s state=0x%llx",
				   provider->getName(),
				   provider->getMetaClass()->getClassName(),
				   (unsigned long long)provider->getState());
			
			// 3. Enumerate ALL siblings on the same provider to find IOServiceCompatibility
			OSIterator *siblings = provider->getClientIterator();
			if (siblings) {
				OSObject *sib;
				int sibIdx = 0;
				while ((sib = siblings->getNextObject())) {
					auto *sibSvc = OSDynamicCast(IOService, sib);
					if (sibSvc) {
						SYSLOG("ngreen", "V45: provider child[%d]: name=%s class=%s state=0x%llx",
							   sibIdx, sibSvc->getName(),
							   sibSvc->getMetaClass()->getClassName(),
							   (unsigned long long)sibSvc->getState());
						sibIdx++;
					}
				}
				siblings->release();
			}
		} else {
			SYSLOG("ngreen", "V45: WARNING: accelerator has no provider!");
		}
		
		// 4. Check if key properties are present on the service itself
		bool hasMetal = (accelSvc->getProperty("MetalPluginName") != nullptr);
		bool hasGL    = (accelSvc->getProperty("IOGLBundleName") != nullptr);
		bool hasCFPI  = (accelSvc->getProperty("IOCFPlugInTypes") != nullptr);
		bool hasVARID = (accelSvc->getProperty("IOVARendererID") != nullptr);
		SYSLOG("ngreen", "V45: properties present: MetalPlugin=%d GL=%d CFPlugIn=%d VARend=%d",
			   hasMetal, hasGL, hasCFPI, hasVARID);
			   
		//v44ScheduleBundleLog(that, 500);
		v44ScheduleBundleLog(that, 3000);

		// 5. If MetalPluginName is missing, set GPU properties directly on the service
		//    (normally they come from the matched personality, but if IOKit didn't merge them...)
		if (!hasMetal) {
			const bool useTglNames = callback && callback->tglHWLoaded;
			const char *mtlName = useTglNames ? "AppleIntelTGLGraphicsMTLDriver" : "AppleIntelICLGraphicsMTLDriver";
			const char *glName = useTglNames ? "AppleIntelTGLGraphicsGLDriver" : "AppleIntelICLGraphicsGLDriver";
			const char *vaName = useTglNames ? "AppleIntelTGLGraphicsVADriver" : "AppleIntelICLGraphicsVADriver";
			SYSLOG("ngreen", "V45: MetalPluginName MISSING — injecting %s GPU props directly on service",
			       useTglNames ? "TGL" : "ICL");
			accelSvc->setProperty("MetalPluginName", mtlName);
			accelSvc->setProperty("IOGLBundleName", glName);
			accelSvc->setProperty("IODVDBundleName", vaName);
			accelSvc->setProperty("IOGVACodec", "Gen10");
			accelSvc->setProperty("IOGVAScaler", "Gen10");
			accelSvc->setProperty("IOGVABGRAEnc", "Gen10");
			accelSvc->setProperty("IOSourceVersion", "0.0.0.0.0");
			auto *vaRendID = OSNumber::withNumber(static_cast<unsigned long long>(17301568), 32);
			if (vaRendID) { accelSvc->setProperty("IOVARendererID", vaRendID); vaRendID->release(); }
			// V77: DisplayPipeSupported=0 — disable GPU display pipe compositing.
			// WindowServer crash-loops (consecutiveCrashCount=11) in
			// CoreDisplay::DisplayPipe::RunFullDisplayPipe because GPU can't render
			// surfaces (RCS ring idle). Force CPU compositing via FB aperture.
			auto *dpCaps = OSDictionary::withCapacity(2);
			if (dpCaps) {
				auto *v1 = OSNumber::withNumber(0ULL, 32);
				auto *v2 = OSNumber::withNumber(0ULL, 32);
				if (v1) { dpCaps->setObject("DisplayPipeSupported", v1); v1->release(); }
				if (v2) { dpCaps->setObject("TransactionsSupported", v2); v2->release(); }
				accelSvc->setProperty("IOAccelDisplayPipeCapabilities", dpCaps);
				dpCaps->release();
				SYSLOG("ngreen", "V77: DisplayPipeSupported=0 set on accelerator");
			}
			auto *cfpi = OSDictionary::withCapacity(1);
			if (cfpi) {
				auto *pn = OSString::withCString("IOAccelerator2D.plugin");
				if (pn) { cfpi->setObject("ACCF0000-0000-0000-0000-000a2789904e", pn); pn->release(); }
				accelSvc->setProperty("IOCFPlugInTypes", cfpi);
				cfpi->release();
			}
		}
		
		// V78: For spoofed (non-TGL) hardware, always force DisplayPipeSupported=0.
		// AccessComplete (V187) is stubbed to return 0 on spoofed hw to prevent a
		// crash at +1928 (null MTL device hash-table lookup). That stub silences the
		// crash but prevents the "FB Ready" surface-flip signal from ever being sent.
		// With DisplayPipeSupported=1 (GPU display pipe), WindowServer waits for
		// that signal indefinitely → watchdogd kills it twice → KP.
		// With DisplayPipeSupported=0, WindowServer uses CPU compositing via the
		// linear framebuffer aperture — bypassing the GPU pipe entirely — so the
		// FB Ready wait is never entered and WindowServer initialises successfully.
		// Real TGL hardware uses the native value (never enters this branch).
		// Explicit -ngreendp1 boot-arg restores native behavior for experimentation.
		if (isDisplayPipeForceDisabled()) {
			auto *dpCaps = OSDictionary::withCapacity(2);
			if (dpCaps) {
				auto *dpSupp = OSNumber::withNumber(0ULL, 32);
				auto *trSupp = OSNumber::withNumber(0ULL, 32);
				if (dpSupp) { dpCaps->setObject("DisplayPipeSupported", dpSupp); dpSupp->release(); }
				if (trSupp) { dpCaps->setObject("TransactionsSupported", trSupp); trSupp->release(); }
				accelSvc->setProperty("IOAccelDisplayPipeCapabilities", dpCaps);
				dpCaps->release();
				SYSLOG("ngreen", "V78: DisplayPipeSupported=0 forced by boot-arg");
			}
		} else {
			SYSLOG("ngreen", "V78: keeping native DisplayPipeSupported (real TGL)");
		}
		
		// 6. Explicit registerService() — ensures the service is visible to IOKit matching
		//    even if IntelAccelerator::start() didn't call it or the internal call failed.
		SYSLOG("ngreen", "V45: calling explicit registerService(kIOServiceAsynchronous) on accelerator");
		accelSvc->registerService(kIOServiceAsynchronous);
		
		// 7. Post-registerService state
		svcState = accelSvc->getState();
		SYSLOG("ngreen", "V45: post-registerService state=0x%llx (reg=%d match=%d)",
			   (unsigned long long)svcState, !!(svcState & 0x02), !!(svcState & 0x04));
		
		// V110: V59 delayed child checks + V74 EMR enforcer run unconditionally for
		// non-real TGL. Without V59, IGAccelDevice stays at state=0x0, WindowServer
		// never opens IOAccelDisplayPipeUserClient2, and the display pipe never activates.
		// V74 keeps EMR masked so Apple can't re-enable error interrupts behind our back.
		// V60 health monitor (which contains V77 DisplayPipe killer) remains opt-in via
		// -ngreenexp — without it, the display pipe is not terminated after opening.
		if (!NGreen::callback->isRealTGL) {
			// 8. V59: Schedule delayed child checks to rescue stuck IGAccelDevice children.
			v45ScheduleDelayedCheck(that, 3000);
			v45ScheduleDelayedCheck(that, 10000);
			v45ScheduleDelayedCheck(that, 15000);
			v45ScheduleDelayedCheck(that, 20000);
			v45ScheduleDelayedCheck(that, 30000);
			v45ScheduleDelayedCheck(that, 60000);
			v45ScheduleDelayedCheck(that, 120000);
			SYSLOG("ngreen", "V59: scheduled delayed child checks at T+3,10,15,20,30,60,120s");
			// V74: Start PERMANENT EMR enforcer — 50ms interval, runs forever.
			// Required: keeps EMR masked so Apple can't re-enable error interrupts.
			// Without this the display pipe fails to activate entirely (all-black screen).
			{
				SYSLOG("ngreen", "V74: config maskWrites=1 v116Reenforce=1 maxIters=unlimited");
				auto *emrSvc = static_cast<IOService *>(that);
				emrSvc->retain();
				auto emrTimer = thread_call_allocate(v71EmrEnforcer,
								 static_cast<thread_call_param_t>(emrSvc));
				if (emrTimer) {
					uint64_t deadline;
					clock_interval_to_deadline(50, kMillisecondScale, &deadline);
					thread_call_enter_delayed(emrTimer, deadline);
					SYSLOG("ngreen", "V74: EMR enforcer armed — 50ms interval, PERMANENT");
				} else {
					emrSvc->release(); // alloc failed — drop our retain
				}
			}

			// V60: GPU health monitor (contains V77 DisplayPipe terminator) — opt-in only.
			// Without this, IOAccelDisplayPipeUserClient2 is NOT killed after V59 opens it.
			// Retain the IOService so it stays alive while a timer is pending (UAF fix).
			if (isV60MonitorEnabled()) {
				auto *monSvc = static_cast<IOService *>(that);
				monSvc->retain();
				auto monTimer = thread_call_allocate(v60GpuHealthMonitor,
										 static_cast<thread_call_param_t>(monSvc));
				if (monTimer) {
					uint64_t deadline;
					clock_interval_to_deadline(2, kSecondScale, &deadline);
					thread_call_enter_delayed(monTimer, deadline);
					SYSLOG("ngreen", "V60: GPU health monitor armed — 2s interval, 60 iterations (120s)");
				} else {
					monSvc->release(); // alloc failed — drop our retain
				}
			} else {
				SYSLOG("ngreen", "V110: V60 health monitor disabled (use -ngreenexp/-ngreenv60 to enable, -ngreenv60off to force disable)");
			}
		}
	}
	
	// ── V47: Enable Master Interrupt (GFX_MSTR_IRQ bit 31) ──
	// Without this, the GPU can't deliver interrupts to the CPU, and the
	// scheduler never gets completion notifications. i915 does this in
	// gen11_master_intr_enable() — just write (1 << 31) to 0x190010.
	{
		uint32_t mstrIrq = NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ);
		SYSLOG("ngreen", "V47: GFX_MSTR_IRQ before enable = 0x%x (bit31=%d)", mstrIrq, !!(mstrIrq & GEN11_MASTER_IRQ));
		if (!(mstrIrq & GEN11_MASTER_IRQ)) {
			NGreen::callback->writeReg32(GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
			IODelay(100);
			uint32_t after = NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ);
			SYSLOG("ngreen", "V47: GFX_MSTR_IRQ after enable  = 0x%x (bit31=%d)", after, !!(after & GEN11_MASTER_IRQ));
		} else {
			SYSLOG("ngreen", "V47: Master IRQ already enabled");
		}
	}
	
	// ── V47: Log initial RING_TIMESTAMP for RCS and BCS ──
	SYSLOG("ngreen", "V47: RCS RING_TIMESTAMP=0x%x BCS RING_TIMESTAMP=0x%x",
		NGreen::callback->readReg32(RENDER_RING_BASE + 0x358),
		NGreen::callback->readReg32(BLT_RING_BASE + 0x358));
	
	// ── V50: Log Metal-readiness summary ──
	SYSLOG("ngreen", "V50: start() ret=%d — policy: TGL from /Library/Extensions, fallback ICL from /System/Library/Extensions", ret);
	SYSLOG("ngreen", "V50: Metal ON. ICL f2 mask-based (fallback). Use -ngreenNoMetal for display-only.");
	SYSLOG("ngreen", "V50: active GPU plugin track = %s (TGL and ICL both supported)",
	       (callback && callback->tglHWLoaded) ? "TGL" : "ICL");
	
	// ── V51: Clear GPU errors — give Metal a clean slate ──
	// ERROR_GEN6 is W1C (write-1-to-clear). Stale errors from init may cause
	// the Metal plugin to reject the device during MTLDevice creation.
	// Per-engine EIR/ESR registers are also W1C.
	{
		// 1. Clear global error register — try multiple times with W1C
		uint32_t errPre = NGreen::callback->readReg32(ERROR_GEN6);
		if (errPre) {
			// V55: Loop clear — stale errors may re-generate from in-flight ops
			for (int i = 0; i < 3; i++) {
				NGreen::callback->writeReg32(ERROR_GEN6, errPre);
				IODelay(200);
				errPre = NGreen::callback->readReg32(ERROR_GEN6);
				if (!errPre) break;
			}
			SYSLOG("ngreen", "V51: cleared ERROR_GEN6 → 0x%x (after loop)", errPre);
		} else {
			SYSLOG("ngreen", "V51: ERROR_GEN6 already clean");
		}
		
		// 2. Clear per-engine error interrupt registers
		uint32_t rcsEir = NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE));
		uint32_t bcsEir = NGreen::callback->readReg32(RING_EIR(BLT_RING_BASE));
		uint32_t rcsEsr = NGreen::callback->readReg32(RING_ESR(RENDER_RING_BASE));
		uint32_t bcsEsr = NGreen::callback->readReg32(RING_ESR(BLT_RING_BASE));
		if (rcsEir) NGreen::callback->writeReg32(RING_EIR(RENDER_RING_BASE), rcsEir);
		if (bcsEir) NGreen::callback->writeReg32(RING_EIR(BLT_RING_BASE), bcsEir);
		if (rcsEsr) NGreen::callback->writeReg32(RING_ESR(RENDER_RING_BASE), rcsEsr);
		if (bcsEsr) NGreen::callback->writeReg32(RING_ESR(BLT_RING_BASE), bcsEsr);
		SYSLOG("ngreen", "V51: cleared RCS EIR=0x%x ESR=0x%x, BCS EIR=0x%x ESR=0x%x",
			   rcsEir, rcsEsr, bcsEir, bcsEsr);
		
		// 3. Clear ring fault register
		uint32_t ringFault = NGreen::callback->readReg32(GEN12_RING_FAULT_REG);
		if (ringFault) {
			NGreen::callback->writeReg32(GEN12_RING_FAULT_REG, 0);
			SYSLOG("ngreen", "V51: cleared RING_FAULT=0x%x", ringFault);
		}
		
		// 4. Clear TLB fault data
		uint32_t tlb0 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0);
		uint32_t tlb1 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1);
		if (tlb0 || tlb1) {
			NGreen::callback->writeReg32(GEN8_FAULT_TLB_DATA0, 0);
			NGreen::callback->writeReg32(GEN8_FAULT_TLB_DATA1, 0);
			SYSLOG("ngreen", "V51: cleared TLB_FAULT data0=0x%x data1=0x%x", tlb0, tlb1);
		}
		
		// 5. Clear GT interrupt identity (W1C) so stale interrupts don't confuse scheduler
		uint32_t gtIntr0 = NGreen::callback->readReg32(GEN11_GT_INTR_DW0);
		uint32_t gtIntr1 = NGreen::callback->readReg32(GEN11_GT_INTR_DW1);
		if (gtIntr0) NGreen::callback->writeReg32(GEN11_GT_INTR_DW0, gtIntr0);
		if (gtIntr1) NGreen::callback->writeReg32(GEN11_GT_INTR_DW1, gtIntr1);
		SYSLOG("ngreen", "V51: cleared GT_INTR DW0=0x%x DW1=0x%x", gtIntr0, gtIntr1);
	}
	
	// ── V51: BCS engine stop+clear ──
	// V52: Only on RPL — real TGL's BCS works natively.
	if (!NGreen::callback->isRealTGL) {
	// BCS ring is dead (CTL=0x0, EXECLIST_STATUS=0x1). The Host scheduler
	// attempted context submission but the engine never started. Clear the
	// engine state so it can be retried by the scheduler on first Metal blit.
	{
		uint32_t bcsCtlNow = NGreen::callback->readReg32(RING_CTL(BLT_RING_BASE));
		if (bcsCtlNow == 0) {
			SYSLOG("ngreen", "V51: BCS ring dead (CTL=0x0) — clearing engine state");
			
			// Request engine stop via masked write (set STOP_RING with mask bit)
			NGreen::callback->writeReg32(RING_MI_MODE(BLT_RING_BASE), 
				(uint32_t)STOP_RING | ((uint32_t)STOP_RING << 16));
			IODelay(500);
			
			// Clear STOP request
			NGreen::callback->writeReg32(RING_MI_MODE(BLT_RING_BASE),
				(uint32_t)STOP_RING << 16);
			IODelay(100);
			
			// Reset HEAD and TAIL
			NGreen::callback->writeReg32(RING_HEAD(BLT_RING_BASE), 0);
			NGreen::callback->writeReg32(RING_TAIL(BLT_RING_BASE), 0);
			
			uint32_t bcsCtlAfter = NGreen::callback->readReg32(RING_CTL(BLT_RING_BASE));
			uint32_t bcsExec = NGreen::callback->readReg32(RING_EXECLIST_STATUS(BLT_RING_BASE));
			SYSLOG("ngreen", "V51: BCS after clear: CTL=0x%x EXECLIST=0x%x", bcsCtlAfter, bcsExec);
		} else {
			SYSLOG("ngreen", "V51: BCS ring active (CTL=0x%x) — no reset needed", bcsCtlNow);
		}
	}
	} else {
		SYSLOG("ngreen", "V52: Real TGL — skipping BCS engine reset");
	}
	
	// ── V55: TLB invalidation + aggressive error clearing ──
	// V54 showed ERROR_GEN6=0x7b cannot be cleared via simple W1C because the GPU
	// continuously re-generates errors (TLB_MISS + PAGE_TABLE_ERROR). The root cause
	// is stale TLB entries from the context init path. Flush TLBs first, then re-clear.
	if (!NGreen::callback->isRealTGL) {
		// 1. Invalidate GPU TLBs via GEN12 MMIO (no GuC needed)
		//    GEN12_GUC_TLB_INV_CR (0xcee8): write to trigger TLB invalidation
		//    Each bit corresponds to an engine: bit0=RCS, bit1=BCS, etc.
		NGreen::callback->writeReg32(0xcee8, 0x1);   // Invalidate RCS TLBs
		IODelay(500);
		NGreen::callback->writeReg32(0xcee8, 0x4);   // Invalidate BCS TLBs
		IODelay(500);
		SYSLOG("ngreen", "V55: TLB invalidation requested (RCS+BCS)");
		
		// 2. Try clearing ERROR_GEN6 multiple times after TLB flush
		uint32_t errLoop = NGreen::callback->readReg32(ERROR_GEN6);
		for (int attempt = 0; attempt < 5 && errLoop; attempt++) {
			NGreen::callback->writeReg32(ERROR_GEN6, errLoop);  // W1C
			IODelay(200);
			errLoop = NGreen::callback->readReg32(ERROR_GEN6);
		}
		SYSLOG("ngreen", "V55: ERROR_GEN6 after TLB flush + 5x clear = 0x%x", errLoop);
		
		// 3. Read back TLB invalidation status
		uint32_t tlbInvDone = NGreen::callback->readReg32(0xceec);
		SYSLOG("ngreen", "V55: TLB_INV done status = 0x%x", tlbInvDone);
		
		// ── V57: ERROR_GEN6 R/W clear + error masking ──
		// V56 DISCOVERY: Writing 0xFFFFFFFF to ERROR_GEN6 SET reserved bits (0x7b→0x7ffff).
		//   This proves ERROR_GEN6 is R/W (not W1C) on RPL-P Gen12.5!
		// V56 DISCOVERY: GDRST killed the ring permanently (CTL→0x0, HEAD→0x0).
		// V56 DISCOVERY: RING_RESET_CTL always reads 0x0 — not functional on RPL-P.
		// V57 approach: Write 0x0 to ERROR_GEN6 (R/W clear), mask all error interrupts.
		//   NO engine resets, NO GDRST — those are destructive.
		{
			uint32_t preErr57 = NGreen::callback->readReg32(ERROR_GEN6);
			uint32_t preEMR = NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE));
			uint32_t preEIR = NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE));
			uint32_t preESR = NGreen::callback->readReg32(RING_ESR(RENDER_RING_BASE));
			SYSLOG("ngreen", "V57: Pre-clear ERROR_GEN6=0x%x EMR=0x%x EIR=0x%x ESR=0x%x",
				   preErr57, preEMR, preEIR, preESR);
			
			// 1. Write 0x0 to ERROR_GEN6 — test R/W clear hypothesis
			//    If register is R/W (as V56 proved), writing 0 should clear all bits.
			//    If register is truly W1C, writing 0 has no effect (harmless test).
			NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
			IODelay(100);
			uint32_t afterZero = NGreen::callback->readReg32(ERROR_GEN6);
			SYSLOG("ngreen", "V57: ERROR_GEN6 after 0x0 write = 0x%x (was 0x%x)", afterZero, preErr57);
			
			// 2. If 0x0 didn't work, try writing just the set bits (proper W1C)
			if (afterZero == preErr57 && preErr57 != 0) {
				NGreen::callback->writeReg32(ERROR_GEN6, preErr57);
				IODelay(100);
				uint32_t afterW1C = NGreen::callback->readReg32(ERROR_GEN6);
				SYSLOG("ngreen", "V57: ERROR_GEN6 after W1C(0x%x) = 0x%x", preErr57, afterW1C);
			}
			
			// 3. Mask ALL per-ring error interrupts via EMR
			//    EMR bits: 0 = error enabled, 1 = error masked
			//    V56 showed EMR=0xfffffffa (bits 0,2 unmasked). Mask everything.
			NGreen::callback->writeReg32(RING_EMR(RENDER_RING_BASE), 0xFFFFFFFF);
			NGreen::callback->writeReg32(RING_EMR(BLT_RING_BASE), 0xFFFFFFFF);
			IODelay(100);
			uint32_t postEMR_rcs = NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE));
			uint32_t postEMR_bcs = NGreen::callback->readReg32(RING_EMR(BLT_RING_BASE));
			SYSLOG("ngreen", "V57: EMR after mask-all: RCS=0x%x BCS=0x%x", postEMR_rcs, postEMR_bcs);
			
			// 4. Clear per-ring EIR if set
			uint32_t rcsEir = NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE));
			if (rcsEir) {
				NGreen::callback->writeReg32(RING_EIR(RENDER_RING_BASE), rcsEir);
				IODelay(50);
				uint32_t eir2 = NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE));
				SYSLOG("ngreen", "V57: RCS EIR cleared 0x%x → 0x%x", rcsEir, eir2);
			}
			
			// 5. Log ring state to confirm it's still alive (V56 GDRST killed it!)
			uint32_t rcsHead = NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE));
			uint32_t rcsTail = NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE));
			uint32_t rcsCtl = NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE));
			SYSLOG("ngreen", "V57: Ring preserved — HEAD=0x%x TAIL=0x%x CTL=0x%x (alive=%d)",
				   rcsHead, rcsTail, rcsCtl, !!(rcsCtl & 0x1000));
			
			// 6. Error forensics: IPEIR/IPEHR show which instruction caused the error
			SYSLOG("ngreen", "V57: IPEIR=0x%x IPEHR=0x%x INSTDONE=0x%x",
				   NGreen::callback->readReg32(RING_IPEIR(RENDER_RING_BASE)),
				   NGreen::callback->readReg32(RING_IPEHR(RENDER_RING_BASE)),
				   NGreen::callback->readReg32(RING_INSTDONE(RENDER_RING_BASE)));
			
			// 7. Final ERROR_GEN6 snapshot
			uint32_t finalErr57 = NGreen::callback->readReg32(ERROR_GEN6);
			SYSLOG("ngreen", "V57: Final ERROR_GEN6=0x%x (goal: 0x0)", finalErr57);
		}
	}
	
	SYSLOG("ngreen", "V51: GPU error clearing complete — Metal should see a clean GPU");
	
	// Release both ForceWake domains
	NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 0);
	NGreen::callback->writeReg32(FORCEWAKE_BLITTER_GEN9, (1 << 16) | 0);
	
	return ret;
}

uint8_t Gen11::deviceStart(void *that)
{
	// V111: Force IGAccelDevice::deviceStart to succeed on spoofed RPL platform.
	// The original checks encodeFailureStack[1] which is set when BCS ring fails to
	// start. On RPL hardware with TGL driver, BCS init fails (RPL uses different
	// ring programming). We allow the original to run and then force success if it
	// returned false, preventing CoreDisplay from seeing a null accelerator device.
	auto ret = FunctionCast(deviceStart, callback->odeviceStart)(that);
	const bool isRealTGL = NGreen::callback && NGreen::callback->isRealTGL;
	if (!isRealTGL && !ret) {
		SYSLOG("ngreen", "V111: IGAccelDevice::deviceStart returned false on RPL — forcing true");
		return true;
	}
	DBGLOG("ngreen", "V111: IGAccelDevice::deviceStart returned %d", ret);
	return ret;
}

IOReturn Gen11::wrapPavpSessionCallback( void *intelAccelerator, int32_t sessionCommand, uint32_t sessionAppId, uint32_t *a4, bool flag) {

	//void* pPavpContext = *getMember<void**>(intelAccelerator, 0x1278);
	//void* pStampTrackingStruct = *(void**)getMember<char*>(pPavpContext, 0xb8);
	
	if (sessionCommand == 4) {
		//return kIOReturnTimeout;
		return kIOReturnSuccess;
	}

	return FunctionCast(wrapPavpSessionCallback, callback->orgPavpSessionCallback)(intelAccelerator, sessionCommand, sessionAppId, a4, flag);
}

void Gen11::getGPUInfoICL(void *that)
{
	FunctionCast(getGPUInfoICL, callback->ogetGPUInfoICL)(that);
	
	// --- GPU topology override for ICL HW binary ---
	// ICL object layout (verified from AppleIntelICLGraphics.sonoma.bin disassembly):
	//   0x1190 = NumSlices          0x12cc = NumSlices mirror
	//   0x1188 = NumSubSlices       0x12d0 = NumSubSlices mirror
	//   0x11a0 = MaxEUPerSubSlice
	//   0x1154 = ExecutionUnitCount (= MaxEUPerSubSlice × NumSubSlices)
	//   0x1198 = L3BankCount
	//   0x1150 = GPU Sku
	// ICL counts traditional sub-slices (same as TGL binary).
	// Use ICL GT2 LP config (1×8×8 = 64 EU) to stay within ICL-valid topology.
	unsigned int numSlices        = 1;
	unsigned int numSubSlices     = 8;   // ICL GT2 LP max (8 SS)
	unsigned int maxEUPerSubSlice = 8;
	unsigned int totalEU          = maxEUPerSubSlice * numSubSlices; // = 64
	
	getMember<UInt32>(that, 0x1190) = numSlices;
	getMember<UInt32>(that, 0x1188) = numSubSlices;
	getMember<UInt32>(that, 0x11a0) = maxEUPerSubSlice;
	getMember<UInt32>(that, 0x1154) = totalEU;
	getMember<UInt32>(that, 0x12cc) = numSlices;     // NumSlices mirror
	getMember<UInt32>(that, 0x12d0) = numSubSlices;  // NumSubSlices mirror
	getMember<UInt32>(that, 0x1198) = 8;             // L3BankCount
	
	SYSLOG("ngreen", "getGPUInfoICL: overridden topology → slices=%u subslices=%u maxEU/SS=%u totalEU=%u L3Banks=8",
		   numSlices, numSubSlices, maxEUPerSubSlice, totalEU);
}

void Gen11::getGPUInfo(void *that)
{

#define RPM_CONFIG0				(0xd00)
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT	3
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK	(1 << GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT)
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_19_2_MHZ	0
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_24_MHZ	1
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT	3
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK	(0x7 << GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT)
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_24_MHZ	0
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_19_2_MHZ	1
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_38_4_MHZ	2
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_25_MHZ	3
#define   GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT	1
#define   GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK	(0x3 << GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT)
	
	FunctionCast(getGPUInfo, callback->ogetGPUInfo)(that);

	// --- GPU topology override for RPL i7-13700H (verified from Linux i915 syslog) ---
	// Linux i915 reports: 1 slice, 6 DSS (mask=0x3f), 16 EU/DSS, 96 EU total.
	// TGL binary uses traditional sub-slices (SS), not dual sub-slices (DSS):
	//   6 DSS × 2 SS/DSS = 12 SS,  16 EU/DSS / 2 = 8 EU/SS, 12 × 8 = 96 EU.
	// Object layout (byte offsets from `this`, verified via disassembly):
	//   0x115c = NumSlices          0x0dd8 = NumSlices mirror
	//   0x1158 = NumSubSlices       0x0ddc = NumSubSlices mirror
	//   0x116c = MaxEUPerSubSlice
	//   0x1124 = ExecutionUnitCount (= MaxEUPerSubSlice × NumSubSlices)
	//   0x1150 = Frequency pair (low32=fMaxMHz, high32=fMinMHz)
	//   0x1164 = L3BankCount
	unsigned int numSlices        = 1;
	unsigned int numSubSlices     = 12;  // 6 DSS × 2 = 12 traditional SS
	unsigned int maxEUPerSubSlice = 8;   // 16 EU/DSS ÷ 2 SS/DSS = 8 EU/SS
	unsigned int totalEU          = maxEUPerSubSlice * numSubSlices; // = 96
	
	getMember<UInt32>(that, 0x115c) = numSlices;
	getMember<UInt32>(that, 0x1158) = numSubSlices;
	getMember<UInt32>(that, 0x116c) = maxEUPerSubSlice;
	getMember<UInt32>(that, 0x1124) = totalEU;
	getMember<UInt32>(that, 0x0dd8) = numSlices;
	getMember<UInt32>(that, 0x0ddc) = numSubSlices;
	getMember<UInt32>(that, 0x1164) = 8;  // L3BankCount (confirmed from InsanelyMac TGL logs)
	
	// Frequency: fMaxFrequencyInMhz=1000, fMinFrequencyInMhz=450 (TGL defaults)
	getMember<uint64_t>(that, 0x1150) = 0x1C2000003E8ULL;
	
	SYSLOG("ngreen", "getGPUInfo: overridden topology → slices=%u subslices=%u maxEU/SS=%u totalEU=%u L3Banks=8",
		   numSlices, numSubSlices, maxEUPerSubSlice, totalEU);

}

// V164: Hook populateResetRegisterList to clear PERCTX_PREEMPT_CTRL before snapshot.
//
// IntelAccelerator::startGraphicsEngine writes 0x40004000 to 0x20E0, then calls
// populateResetRegisterList which reads the live MMIO value via [rax+20E0h] and stores
// it into a per-context replay list (a batch of MI_LRI commands replayed before every
// context switch). If 0x4000 is snapshotted into that list, hardware perpetually
// replays it back, overriding any post-hoc MMIO clear (V162/V163 arrive too late).
//
// Fix: clear bit 14 BEFORE calling original so the snapshot captures 0x0000,
// making the replay batch write 0x0000 to 0x20E0 on every context switch instead.
// xe/lrca: reparo da pagina1 do LRCA (slot GGTT[0x19]) com diagnostico de PTE/readback.
void Gen11::xeLrcaRepair(const char *tag)
{
	// V507 LRCA repair: re-copy the ring-LRI from the saved RCS context buffer to the
	// ExecList submission slot (GGTT[0x19]) after every populateResetRegisterList.
	// The display retry loop reuses the same context without calling withOptions again,
	// so the slot gets wiped by stopGraphicsEngine and never repaired by V508.
	auto *cb = NGreen::callback;
	void *ctx = cb->lastRCSCtx;
	if (!cb->isRealTGL && ctx) {
		uint32_t lrcaGpuVa = getMember<uint32_t>(ctx, 0x89) & 0xFFFFF000;
		if (lrcaGpuVa) {
			uint32_t ctxPage1Idx = (lrcaGpuVa >> 12) + 1;
			cb->setApertureIfNecessary();
			if (cb->aperturePtr && cb->apertureLen >= 0x1000) {
				uint32_t slotLo = cb->readReg32(GGTT_PTE_LO(0x19));
				uint32_t slotHi = cb->readReg32(GGTT_PTE_HI(0x19));
				uint32_t ctxLo  = cb->readReg32(GGTT_PTE_LO(ctxPage1Idx));
				uint32_t ctxHi  = cb->readReg32(GGTT_PTE_HI(ctxPage1Idx));
				if ((slotLo & 1) && (ctxLo & 1)) {
					asm volatile("wbinvd" ::: "memory");
					volatile uint32_t *ap = cb->aperturePtr;
					uint32_t saveLo = cb->readReg32(GGTT_PTE_LO(0));
					uint32_t saveHi = cb->readReg32(GGTT_PTE_HI(0));

					uint32_t buf[96];
					cb->writeReg32(GGTT_PTE_LO(0), ctxLo);
					cb->writeReg32(GGTT_PTE_HI(0), ctxHi);
					cb->writeReg32(0x101008, 0x1);
					for (int i = 0; i < 96; i++) buf[i] = ap[i];

					cb->writeReg32(GGTT_PTE_LO(0), slotLo);
					cb->writeReg32(GGTT_PTE_HI(0), slotHi);
					cb->writeReg32(0x101008, 0x1);

					if ((ap[1] & 0x1F801000) != 0x11001000 &&
					    (buf[1] & 0x1F801000) == 0x11001000) {
						for (int i = 0; i < 96; i++) ap[i] = buf[i];
						asm volatile("sfence" ::: "memory");
						SYSLOG("ngreen", "XE-LRCA[%s]: repair slot<-ctx DW1=%08x slotPTE=%08x:%08x ctxPTE=%08x:%08x readback DW1=%08x", tag, buf[1], slotHi, slotLo, ctxHi, ctxLo, (uint32_t)ap[1]);
					}

					cb->writeReg32(GGTT_PTE_LO(0), saveLo);
					cb->writeReg32(GGTT_PTE_HI(0), saveHi);
					cb->writeReg32(0x101008, 0x1);
				}
			}
		}
	}
}

void Gen11::populateResetRegisterList(void *that)
{
	if (!NGreen::callback->isRealTGL) {
		// Masked clear: mask=bit14, value=0 → 0x40000000
		NGreen::callback->writeReg32(GEN7_FF_SLICE_CS_CHICKEN1,
			(GEN9_FFSC_PERCTX_PREEMPT_CTRL << 16) | 0);
		SYSLOG("ngreen", "V164: pre-snapshot clear FF_SLICE_CS_CHICKEN1=0x%08x",
			NGreen::callback->readReg32(GEN7_FF_SLICE_CS_CHICKEN1));

	}
	FunctionCast(populateResetRegisterList, callback->opopulateResetRegisterList)(that);

	// V503: Dump the exact values Apple just baked into the context reset register list.
	// Register set decoded from Ghidra decompile of IntelAccelerator::populateResetRegisterList.
	static bool v503Logged = false;
	if (!v503Logged && !NGreen::callback->isRealTGL) {
		v503Logged = true;
		auto *cb = NGreen::callback;
		SYSLOG("ngreen", "V503: context register snapshot (19 regs):");
		SYSLOG("ngreen", "V503:  0x2080 HWS_PGA           = 0x%08x", cb->readReg32(0x2080));
		SYSLOG("ngreen", "V503:  0x2134 RING_BUFFER_UHPTR = 0x%08x", cb->readReg32(0x2134));
		SYSLOG("ngreen", "V503:  0x20c0 INSTPM            = 0x%08x", cb->readReg32(0x20c0));
		SYSLOG("ngreen", "V503:  0x7000 CACHE_MODE_0      = 0x%08x", cb->readReg32(0x7000));
		SYSLOG("ngreen", "V503:  0x7004 CACHE_MODE_1      = 0x%08x", cb->readReg32(0x7004));
		SYSLOG("ngreen", "V503:  0x209c MI_MODE           = 0x%08x", cb->readReg32(0x209c));
		SYSLOG("ngreen", "V503:  0x2090 3D_CHICKEN3       = 0x%08x", cb->readReg32(0x2090));
		SYSLOG("ngreen", "V503:  0x4090 ECOCHK            = 0x%08x", cb->readReg32(0x4090));
		SYSLOG("ngreen", "V503:  0x20a0 FF_THREAD_MODE    = 0x%08x", cb->readReg32(0x20a0));
		SYSLOG("ngreen", "V503:  0x20e4 FF_SLICE_CS_CHKN2 = 0x%08x", cb->readReg32(0x20e4));
		SYSLOG("ngreen", "V503:  0x9430 UCGCTL6           = 0x%08x", cb->readReg32(0x9430));
		SYSLOG("ngreen", "V503:  0x7010 CMN_SLICE_CHKN1   = 0x%08x", cb->readReg32(0x7010));
		SYSLOG("ngreen", "V503:  0x0d08 RCPCONFIG         = 0x%08x", cb->readReg32(0x0d08));
		SYSLOG("ngreen", "V503:  0xe194 HALF_SLICE_CHKN7  = 0x%08x", cb->readReg32(0xe194));
		SYSLOG("ngreen", "V503:  0xb004 GARBCNTLREG       = 0x%08x", cb->readReg32(0xb004));
		SYSLOG("ngreen", "V503:  0x20ec CS_DEBUG_MODE1    = 0x%08x", cb->readReg32(0x20ec));
		SYSLOG("ngreen", "V503:  0x2580 CS_CHICKEN1       = 0x%08x", cb->readReg32(0x2580));
		SYSLOG("ngreen", "V503:  0x20e0 FF_SLICE_CS_CHKN1 = 0x%08x", cb->readReg32(0x20e0));
		SYSLOG("ngreen", "V503:  0x229c RCS_GFX_MODE      = 0x%08x", cb->readReg32(0x229c));
	}
	// V504: patch reset register list before context image bakes
	// Walk IGVector: each entry = {uint32_t mmio_addr, uint32_t value, char name[0x1c]}
	if (!NGreen::callback->isRealTGL) {
		auto *accel = reinterpret_cast<AppleIntel::IntelAccelerator*>(that);
		uint64_t count = accel->fResetRegCount;
		uint8_t *data  = reinterpret_cast<uint8_t*>(accel->fResetRegData);
		static int v504CallCount = 0;
		++v504CallCount;
		bool v504Verbose = (v504CallCount <= 3);
		if (v504Verbose)
			SYSLOG("ngreen", "V504[%d]: IGVector count=%llu data=%p", v504CallCount,
				   (unsigned long long)count, data);
		for (uint64_t i = 0; i < count; i++) {
			uint32_t *entry = reinterpret_cast<uint32_t*>(data + i * 0x24);
			uint32_t  addr  = entry[0];
			// V504-TEST: 0x20ec CS_DEBUG_MODE1 patch commented out to test if clearing
			// Apple's 0x11 (bits 0+4) is needed or harmful on RPL.
			// if (addr == 0x20ec) {
			//     entry[1] = 0x00000000;
			//     if (v504Verbose) SYSLOG("ngreen", "V504[%d]: patched CS_DEBUG_MODE1 → 0", v504CallCount);
			// }
			if (addr == 0x2580) {
				entry[1] = 0x00000002;
				if (v504Verbose) SYSLOG("ngreen", "V504[%d]: patched CS_CHICKEN1 → 0x2", v504CallCount);
			}
			// V506: The populateResetRegisterList snapshot reads RING_MODE before Apple enables
			// GFX_RUN_LIST_ENABLE (bit 15), so the baked value is 0x0.  On first lite-restore
			// the MI_LRI replays RING_MODE=0, which clears GFX_RUN_LIST_ENABLE mid-restore —
			// the ExecList state machine disables itself and the context never reaches ACTIVE.
			// Fix: force bit 15 in the image so every context restore re-arms ExecList mode.
			if (addr == 0x229c) {
				uint32_t old = entry[1];
				entry[1] |= (1u << 15);  // GFX_RUN_LIST_ENABLE
				if (v504Verbose) SYSLOG("ngreen", "V504[%d]: V506 RING_MODE 0x%08x → 0x%08x (GFX_RUN_LIST_ENABLE forced)",
					v504CallCount, old, entry[1]);
			}
		}
	}
	// V507: Re-arm GFX_RUN_LIST_ENABLE in the live register after each reset/recovery attempt.
	// Apple's recovery may clear RING_MODE; ensure ExecList mode stays on before context re-submit.
	{
		uint32_t rmPre = NGreen::callback->readReg32(RING_MODE(RENDER_RING_BASE));
		NGreen::callback->writeReg32(RING_MODE(RENDER_RING_BASE), 0x80008000u);
		uint32_t rmPost = NGreen::callback->readReg32(RING_MODE(RENDER_RING_BASE));
		SYSLOG("ngreen", "V507: post-resetRegList RING_MODE 0x%08x → readback 0x%08x (GFX_RUN_LIST_ENABLE=%d)",
			rmPre, rmPost, !!(rmPost & (1u << 15)));
	}

	xeLrcaRepair("resetRegList");
}

void *Gen11::createUserGPUTask(void *that)
{
	auto ensureTaskContext = [&](void *task, const char *origin) -> void * {
		if (!task || NGreen::callback->isRealTGL)
			return task ? getMember<void *>(task, 0xb8) : nullptr;

		void *taskCtx = getMember<void *>(task, 0xb8);
		if (taskCtx)
			return taskCtx;

		// Read task+0x298 directly — that's where getBlit3DContext(task,true) stores the
		// allocated context when Apple's driver naturally calls it. Calling getBlit3DContext
		// with true ourselves triggers initWithOptions too early (before GPU memory is ready)
		// causing a boot hang. Apple's call happens later; null here is safe — all callers
		// of ensureTaskContext that dereference +0xb8 already have null guards.
		void *ctx = getMember<void *>(task, 0x298);
		if (!ctx)
			return nullptr;

		auto *ctxSlot = reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(task) + 0xb8);
		*ctxSlot = ctx;

		if (isExperimentalMonitorEnabled()) {
			static int v138Count = 0;
			if (v138Count < 32) {
				v138Count++;
				SYSLOG("ngreen", "V138[%d]: %s task=%p initialized ctx=%p", v138Count, origin, task, ctx);
			}
		}

		return ctx;
	};

	auto *task = FunctionCast(createUserGPUTask, callback->ocreateUserGPUTask)(that);
	if (isExperimentalMonitorEnabled()) {
		static int v137CreateTaskCount = 0;
		if (v137CreateTaskCount < 24) {
			v137CreateTaskCount++;
			void *taskVtable = task ? getMember<void *>(task, 0x0) : nullptr;
			void *taskCtx = task ? getMember<void *>(task, 0xb8) : nullptr;
			void *kernelTask = getMember<void *>(that, 0x150);
			void *kernelTaskVtable = kernelTask ? getMember<void *>(kernelTask, 0x0) : nullptr;
			void *kernelTaskCtx = kernelTask ? getMember<void *>(kernelTask, 0xb8) : nullptr;
			SYSLOG("ngreen", "V137.createUserGPUTask[%d]: acc=%p task=%p vtbl=%p ctx=%p ktask=%p kvtbl=%p kctx=%p cached=%p",
				   v137CreateTaskCount, that, task, taskVtable, taskCtx,
				   kernelTask, kernelTaskVtable, kernelTaskCtx, callback->v132CachedTask);
		}
	}
	if (NGreen::callback->isRealTGL || task) {
		if (task)
			ensureTaskContext(task, "createUserGPUTask-user");
		return task;
	}

	void *kernelTask = getMember<void *>(that, 0x150);
	if (kernelTask) {
		callback->v132CachedTask = kernelTask;
		ensureTaskContext(kernelTask, "createUserGPUTask-kernel");
		SYSLOG("ngreen", "V132: createUserGPUTask returned null, using kernel task fallback=%p", kernelTask);
		return kernelTask;
	}

	if (callback->v132CachedTask) {
		ensureTaskContext(callback->v132CachedTask, "createUserGPUTask-cached");
		SYSLOG("ngreen", "V132: createUserGPUTask returned null, using cached task=%p", callback->v132CachedTask);
		return callback->v132CachedTask;
	}

	SYSLOG("ngreen", "V132: createUserGPUTask returned null, no fallback task available");
	return nullptr;
}

void *Gen11::igAccelTaskWithOptions(void *that)
{
	auto ensureTaskContext = [&](void *task, const char *origin) -> void * {
		if (!task || NGreen::callback->isRealTGL)
			return task ? getMember<void *>(task, 0xb8) : nullptr;

		void *taskCtx = getMember<void *>(task, 0xb8);
		if (taskCtx)
			return taskCtx;

		// Read task+0x298 directly — that's where getBlit3DContext(task,true) stores the
		// allocated context when Apple's driver naturally calls it. Calling getBlit3DContext
		// with true ourselves triggers initWithOptions too early (before GPU memory is ready)
		// causing a boot hang. Apple's call happens later; null here is safe — all callers
		// of ensureTaskContext that dereference +0xb8 already have null guards.
		void *ctx = getMember<void *>(task, 0x298);
		if (!ctx)
			return nullptr;

		auto *ctxSlot = reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(task) + 0xb8);
		*ctxSlot = ctx;

		if (isExperimentalMonitorEnabled()) {
			static int v138Count = 0;
			if (v138Count < 32) {
				v138Count++;
				SYSLOG("ngreen", "V138[%d]: %s task=%p initialized ctx=%p", v138Count, origin, task, ctx);
			}
		}

		return ctx;
	};

	auto *task = FunctionCast(igAccelTaskWithOptions, callback->oigAccelTaskWithOptions)(that);
	if (isExperimentalMonitorEnabled()) {
		static int v137WithOptionsCount = 0;
		if (v137WithOptionsCount < 24) {
			v137WithOptionsCount++;
			void *taskVtable = task ? getMember<void *>(task, 0x0) : nullptr;
			void *taskCtx = task ? getMember<void *>(task, 0xb8) : nullptr;
			SYSLOG("ngreen", "V137.withOptions[%d]: acc=%p task=%p vtbl=%p ctx=%p cached=%p",
				   v137WithOptionsCount, that, task, taskVtable, taskCtx, callback->v132CachedTask);
		}
	}
	if (NGreen::callback->isRealTGL)
		return task;

	if (task) {
		callback->v132CachedTask = task;
		ensureTaskContext(task, "withOptions");
		DBGLOG("ngreen", "V132: IGAccelTask::withOptions created task=%p", task);
		return task;
	}

	if (callback->v132CachedTask) {
		ensureTaskContext(callback->v132CachedTask, "withOptions-cached");
		SYSLOG("ngreen", "V132: IGAccelTask::withOptions returned null, reusing cached task=%p", callback->v132CachedTask);
		return callback->v132CachedTask;
	}

	return nullptr;
}

void * Gen11::getBlit3DContext(void *that,bool param_1)
{
	if (!that)
		return nullptr;

	// Preserve real TGL behavior.
	if (NGreen::callback->isRealTGL) {
		if (!callback->ogetBlit3DContext)
			return nullptr;
		return FunctionCast(getBlit3DContext, callback->ogetBlit3DContext)(that, param_1);
	}

	// V165: On RPL, ctx+0xb8 is NULL — base-class initWithOptions doesn't set it for RPL.
	// All callers that dereference ctx+0xb8 are guarded with !isRealTGL early-returns
	// (initBlitUsage V121, markBlitUsage V122, beginCoalescedSegment V124, barrierSubmission V130).
	// Accept any non-null ctx on RPL.
	//
	// V194: Always call Apple's original first. After gpuRestart all context objects are
	// freed/reallocated; original returns the live context for the task. If it returns a
	// different non-null ctx than our cache, a restart happened — update the cache.
	// Fall back to cache only when original returns null (early-init window).
	//
	// Tier-1 interrupt: ring init disables RENDER_COPY_INTR_ENABLE. Ensure it's set before
	// each call so the GPU context-creation completion interrupt can fire.
	uint32_t rcIntrPre = 0;
	if (!NGreen::callback->isRealTGL) {
		rcIntrPre = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t wantBits = getV65Tier1WantBits(false);
		if (!(rcIntrPre & wantBits)) {
			NGreen::callback->writeReg32(GEN11_RENDER_COPY_INTR_ENABLE, rcIntrPre | wantBits);
			SYSLOG("ngreen", "V148: tier-1 was 0x%x — enabled (0x%x) before getBlit3DContext",
				   rcIntrPre, rcIntrPre | wantBits);
		}
	}

	if (callback->ogetBlit3DContext) {
		// V502: log param_1 and task+0x298 before calling original — tells us whether
		// getBlit3DContext will attempt context creation (param_1=true) or just read cache.
		void *cached298 = getMember<void *>(that, 0x298);
		static int v502Count = 0;
		if (v502Count < 12) {
			v502Count++;
			SYSLOG("ngreen", "V502[%d]: getBlit3DContext task=%p param_1=%d cached298=%p",
				   v502Count, that, (int)param_1, cached298);
		}
		void *ctx = FunctionCast(getBlit3DContext, callback->ogetBlit3DContext)(that, param_1);
		void *b8 = ctx ? getMember<void *>(ctx, 0xb8) : nullptr;
		if (ctx && (b8 || !NGreen::callback->isRealTGL)) {
			if (ctx != callback->v131CachedBlit3DCtx) {
				SYSLOG("ngreen", "V194: getBlit3DContext ctx changed %p -> %p (restart recovery), updating cache",
					   callback->v131CachedBlit3DCtx, ctx);
				callback->v131CachedBlit3DCtx = ctx;
			} else if (isExperimentalMonitorEnabled()) {
				SYSLOG("ngreen", "V148: getBlit3DContext using cached ctx=%p ctx+0xb8=%p",
					   ctx, b8);
			}
			return ctx;
		}
		uint32_t errReg     = NGreen::callback->readReg32(ERROR_GEN6);
		uint32_t rcIntrPost = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		SYSLOG("ngreen", "V148: getBlit3DContext original returned invalid ctx=%p ctx+0xb8=%p "
			   "tier1_pre=0x%x tier1_post=0x%x ERROR_GEN6=0x%x param_1=%d cached298_pre=%p",
			   ctx, b8, rcIntrPre, rcIntrPost, errReg, (int)param_1, cached298);

	}

	// Apple's original returned null (early init) — fall back to previously cached ctx.
	if (callback->v131CachedBlit3DCtx) {
		if (isExperimentalMonitorEnabled())
			SYSLOG("ngreen", "V148: getBlit3DContext using cached ctx=%p (original returned null)",
				   callback->v131CachedBlit3DCtx);
		return callback->v131CachedBlit3DCtx;
	}

	// Fallbacks: depth/color resolve contexts share the same +0xb8 layout.
	// V165: On RPL accept fallback ctx regardless of +0xb8.
	void *depth = getDepthResolveContext(that, param_1);
	void *depthB8 = depth ? getMember<void *>(depth, 0xb8) : nullptr;
	if (depth && (depthB8 || !NGreen::callback->isRealTGL)) {
		SYSLOG("ngreen", "V148: getBlit3DContext using depth-resolve fallback ctx=%p ctx+0xb8=%p", depth, depthB8);
		return depth;
	}
	void *color = getColorResolveContext(that, param_1);
	void *colorB8 = color ? getMember<void *>(color, 0xb8) : nullptr;
	if (color && (colorB8 || !NGreen::callback->isRealTGL)) {
		SYSLOG("ngreen", "V148: getBlit3DContext using color-resolve fallback ctx=%p ctx+0xb8=%p", color, colorB8);
		return color;
	}

	SYSLOG("ngreen", "V148: getBlit3DContext all paths exhausted");
	return nullptr;
}

uint64_t Gen11::blit3d_init_ctx(void *that)
{
	// V128 rollback: allow original Blit3D context init on spoofed paths.
	return FunctionCast(blit3d_init_ctx, callback->oblit3d_init_ctx)(that);
}

void Gen11::blit3d_initialize_scratch_space(void *that)
{
	// Real TGL: always use Apple's original path unchanged.
	if (NGreen::callback->isRealTGL) {
		FunctionCast(blit3d_initialize_scratch_space, callback->oblit3d_initialize_scratch_space)(that);
		return;
	}

	// V181: On spoofed paths, allow scratch init now that IOAF2 lock/unlock symbols
	// are resolved. The scratch buffer must be populated for RCS (routeSel=3) blits
	// to execute correctly; without it the GPU stalls on null shader code (Sig 813).
	// Guard: only proceed if IOAF2 symbols resolved — if they're missing, suppressing
	// is safer than a GPU hang.
	if (!callback->oIOAF2_lockForCPUAccess || !callback->oIOAF2_unlockForCPUAccess) {
		SYSLOG("ngreen", "V181: suppressing scratch init on RPL — IOAF2 symbols unresolved lock=%p unlock=%p",
			   reinterpret_cast<void *>(callback->oIOAF2_lockForCPUAccess),
			   reinterpret_cast<void *>(callback->oIOAF2_unlockForCPUAccess));
		return;
	}

	static bool v181InitLogged = false;
	if (!v181InitLogged) {
		v181InitLogged = true;
		SYSLOG("ngreen", "V181: blit3d_initialize_scratch_space running on RPL (IOAF2 lock=%p unlock=%p)",
			   reinterpret_cast<void *>(callback->oIOAF2_lockForCPUAccess),
			   reinterpret_cast<void *>(callback->oIOAF2_unlockForCPUAccess));
	}
	FunctionCast(blit3d_initialize_scratch_space, callback->oblit3d_initialize_scratch_space)(that);
}

// 20/10 (XE-RESFREE): pânico "kfree: addr 0x300c000040001 ... not found in any zone" (pid Wallpaper) em
// IGAccelResource::free+0x182: a tabela por plano/mip em this+0x250 (tamanho this+0x258, entradas de 40 bytes,
// índice = i*[0xad]+j com i<[0xac], [0xae]<=j<[0xae]+[0xad]) tem ponteiros (entrada+0x20) inválidos no Tahoe.
// Antes do free original: entradas fora da tabela -> não percorre (zera 0xac); ponteiros não mapeados -> zera
// (vaza uns bytes em vez de panicar). -ngreennoresfree desliga.
// 20/10 (XE-GTFREQ): IGHardwareCommandStreamer5::handleChangeFrequencyInterrupt pede a frequência ao objeto de
// energia em accel+0xde8 (depende do GuC/SLPC, desligado com -disablegfxfirmware) e recebe 0 -> grava RPNSWREQ
// (0xA008) = 0 e a GPU fica no RPn (100 MHz; RP0 do i7-1360P = 1500 MHz). Provado ao vivo: pedir 900 MHz com
// forcewake sobe o CAGF na hora e a Apple zera em ~20 ms. Aqui o pedido nunca fica abaixo de ngreengtfreq (MHz,
// padrão 1500 = RP0 desde 20/10 noite, antes 1100; 0 desliga). Ocioso a GPU entra em RC6 de qualquer jeito, então o piso só vale com trabalho.
void Gen11::wrapSetGTFrequencyMMIO(void *that, uint32_t val)
{
	static int pisoMhz = -1;
	if (pisoMhz < 0) {
		pisoMhz = 1500;
		PE_parse_boot_argn("ngreengtfreq", &pisoMhz, sizeof(pisoMhz));
		if (pisoMhz > 1500) pisoMhz = 1500;
	}
	gXeGtFreqApple = val;
	gXeGtFreqN++;
	if (pisoMhz > 0) {
		uint32_t ratio = (val & 0xf) ? ((val >> 14) & 0x1ff) : (val >> 23);  // mesmo decode da Apple
		uint32_t piso = (uint32_t)pisoMhz * 3 / 50;                           // unidades de 16,67 MHz (Gen9+)
		if (ratio < piso) val = piso << 23;
	}
	FunctionCast(wrapSetGTFrequencyMMIO, callback->oSetGTFrequencyMMIO)(that, val);
}

// 21/10 (XE-IDLEBOUND): desligamentos de 60 a 185 s (e o pânico do watchdog): o WindowServer fica em CGXShutdownDisplays ->
// IOFBSetDisplayModeAndDepth -> IntelAccelerator::stopGraphicsEngine -> IGScheduler5::waitForGpuIdle (até 25000 x 100 us),
// chamado várias vezes, com stamps que nunca retornam. Se uma espera esgota (>= 2 s), as esperas dos próximos 30 s
// retornam na hora (a marca expira, para não afetar o uso normal). -ngreennoidlebound desliga.
static volatile uint64_t gXeWedgedUntil = 0; static volatile uint32_t gXeIdleSkips = 0;
void Gen11::wrapWaitForGpuIdle(void *that)
{
	static int off = -1;
	if (off < 0) off = checkKernelArgument("-ngreennoidlebound") ? 1 : 0;
	uint64_t now = mach_absolute_time();
	if (!off && now < gXeWedgedUntil) { gXeIdleSkips++; return; }
	FunctionCast(wrapWaitForGpuIdle, callback->oWaitForGpuIdle)(that);
	uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - now, &ns);
	if (ns >= 2000000000ULL) {
		uint64_t d = 0; nanoseconds_to_absolutetime(30ULL * 1000000000ULL, &d);
		gXeWedgedUntil = mach_absolute_time() + d;
		SYSLOG("ngreen", "XE-IDLEBOUND: waitForGpuIdle levou %llu ms; esperas puladas por 30 s", ns / 1000000ULL);
	}
}

// 20/10 (XE-FBDPMS): a tela não acordava do repouso. AppleIntelFramebuffer::setFramebufferPowerState(state) (2=Wake,
// 1=Doze, 0=Sleep; estado atual em fb+0x4284) chama prepareToExitWake ao sair de Wake, que desliga pipe A,
// TRANS_DDI_FUNC_CTL_A, DDI_BUF_CTL_A, escalador e painel; ao voltar, prepareToEnterWake religa painel e plano mas não
// o pipe/link eDP (o FB TGL não treina o link do display 13; no boot ele herda a tela do firmware). Medido com
// ~/xe/tools/xe-sleep-diag.sh. Aqui, no FB0 (painel interno): Wake -> Doze/Sleep só apaga a luz de fundo (BLC_CTL bit 31)
// e grava o novo estado; Doze/Sleep -> Wake só reacende. O caminho de vídeo nunca é desmontado, e as trocas de modo
// (que não passam por aqui) continuam normais. A XE-NODPMS (filtro de escritas) quebrava as trocas de modo.
// -ngreenfbdpms volta ao original.
static uint32_t gXeFbBlcCtl = 0, gXeFbBlcDuty = 0;
static bool gXeFbFake = false;
IOReturn Gen11::wrapSetFramebufferPowerState(void *that, unsigned int state)
{
	static int on = -1;
	if (on < 0) on = (xeFixEnabled() && !checkKernelArgument("-ngreenfbdpms")) ? 1 : 0;
	auto *fb = static_cast<uint8_t *>(that);
	if (on && fb && NGreen::callback && *reinterpret_cast<uint32_t *>(fb + 0x1dc) == 0) {
		uint32_t cur = *reinterpret_cast<uint32_t *>(fb + 0x4284);
		if (state > 2) state = 2;
		if (cur == 2 && state < 2) {
			gXeFbBlcCtl = NGreen::callback->readReg32(0xC8250);
			gXeFbBlcDuty = NGreen::callback->readReg32(0xC8258);
			NGreen::callback->writeReg32(0xC8250, gXeFbBlcCtl & ~0x80000000u);   // luz de fundo off
			*reinterpret_cast<uint32_t *>(fb + 0x4284) = state;
			gXeFbFake = true; gXeFbDpmsN++;
			IOLog("XeNG FBDPMS: FB0 %u -> %u: só luz de fundo off (BLC_CTL 0x%x duty 0x%x)\n", cur, state, gXeFbBlcCtl, gXeFbBlcDuty);
			return kIOReturnSuccess;
		}
		if (cur < 2 && state < 2 && gXeFbFake) {           // Doze <-> Sleep enquanto "dormindo": só registra
			*reinterpret_cast<uint32_t *>(fb + 0x4284) = state;
			return kIOReturnSuccess;
		}
		if (cur < 2 && state == 2 && gXeFbFake) {
			if (gXeFbBlcDuty) NGreen::callback->writeReg32(0xC8258, gXeFbBlcDuty);
			NGreen::callback->writeReg32(0xC8250, gXeFbBlcCtl | 0x80000000u);   // luz de fundo on
			*reinterpret_cast<uint32_t *>(fb + 0x4284) = 2;
			gXeFbFake = false; gXeFbDpmsN++;
			IOLog("XeNG FBDPMS: FB0 %u -> 2: luz de fundo on\n", cur);
			return kIOReturnSuccess;
		}
	}
	return FunctionCast(wrapSetFramebufferPowerState, callback->oSetFramebufferPowerState)(that, state);
}

void Gen11::IGAccelResourcefree(void *that)
{
	static int on = -1;
	if (on < 0) on = checkKernelArgument("-ngreennoresfree") ? 0 : 1;
	if (on == 1 && that) {
		auto *o = static_cast<uint8_t *>(that);
		uint8_t *tab = *reinterpret_cast<uint8_t **>(o + 0x250);
		if (tab && !xeMapeado(reinterpret_cast<uint64_t>(tab))) {
			*reinterpret_cast<uint8_t **>(o + 0x250) = nullptr;
			gXeResFreeFix++;
		} else if (tab) {
			uint32_t na = o[0xac], nb = o[0xad], nc = o[0xae], sz = *reinterpret_cast<uint32_t *>(o + 0x258);
			bool fora = false;
			for (uint32_t i = 0; i < na && !fora; i++)
				for (uint32_t j = nc; j < nc + nb; j++) {
					uint64_t off = (uint64_t)(i * nb + j) * 40;
					if (off + 40 > sz) { fora = true; break; }
					uint64_t p = *reinterpret_cast<uint64_t *>(tab + off + 0x20);
					if (p && !xeMapeado(p)) { *reinterpret_cast<uint64_t *>(tab + off + 0x20) = 0; gXeResFreeFix++; }
				}
			if (fora) { o[0xac] = 0; gXeResFreeFix++; }
		}
	}
	FunctionCast(IGAccelResourcefree, callback->oIGAccelResourcefree)(that);
}

void Gen11::IGHardwareBlit3DContextinitialize(void *that)
{
	// V69: Diagnostic hook — original crashes at +0x4c writing 0x44 bytes to buffer+0xD000.
	// That page is unmapped (CPU page fault, type 14). We replace the entire function body
	// to prevent the panic and log the buffer state for root-cause analysis.
	// V73: Fixed double-dereference bug — *getMember<T*> reads a pointer then follows it.
	//      Use getMember<T> to write to object fields directly.
	//      Added NULL guard for `that` (crash seen with RDI=0x0).

	if (!that) {
		SYSLOG("ngreen", "V73: IGHardwareBlit3DContext::initialize called with NULL this!");
		return;
	}

	static int v69CallCount = 0;
	v69CallCount++;
	const bool v69Verbose = v69CallCount <= 6;

	// Preserve real TGL behavior: always use Apple's original initializer.
	if (NGreen::callback->isRealTGL) {
		FunctionCast(IGHardwareBlit3DContextinitialize, callback->oIGHardwareBlit3DContextinitialize)(that);
		return;
	}

	if (v69Verbose)
		SYSLOG("ngreen", "V69: IGHardwareBlit3DContext::initialize[%d](%p)", v69CallCount, that);

	// Dump context object internals — look for the GPU buffer mapping info
	void *mappedBufPtr = getMember<void *>(that, 0xd8);
	if (v69Verbose)
		SYSLOG("ngreen", "V69: ctx->0xd8(IGMappedBuffer)=%p", mappedBufPtr);

	// Probe IGMappedBuffer internals: vtable, size, IOMemoryDescriptor*, base VA, etc.
	if (mappedBufPtr && v69Verbose) {
		for (int i = 0; i < 12; i++) {
			uint64_t val = getMember<uint64_t>(mappedBufPtr, i * 8);
			SYSLOG("ngreen", "V69: mappedBuf[0x%02x]=0x%016llx", i * 8, (unsigned long long)val);
		}
	}

	// V106: On spoofed paths, default to skip Apple's original init.
	// The original still faults at +0x4c on some boots (page fault in SecurityAgent path).
	// In full-MTL mode, keep skip-by-default for stability; allow original only via opt-in arg.
	// Use -ngreenV69AllowOriginal to test original init, and -ngreenV69SkipOriginal to hard-disable it.
	const bool allowOriginal = checkKernelArgument("-ngreenV69AllowOriginal");
	const bool forceFullMTL = shouldForceFullMetalPath();
	const bool skipOriginal = checkKernelArgument("-ngreenV69SkipOriginal");
	uint64_t gpuBufBase = mappedBufPtr ? getMember<uint64_t>(mappedBufPtr, 0x18) : 0;
	uint64_t gpuBufSize = mappedBufPtr ? getMember<uint64_t>(mappedBufPtr, 0x20) : 0;
	// Calling the original Blit3D initialize on spoofed RPL/ADL is still unsafe on Sonoma:
	// crashes observed at initialize+0x4c with a write fault to the mapped scratch page.
	// Keep original path blocked by default even when -ngreenV69AllowOriginal is set.
	// Only allow with explicit unsafe override for debugging.
	// GPU VA is at mappedBuf[0x38], size at [0x20]. [0x18] is the task pointer, not GPU VA.
	uint64_t gpuVA   = mappedBufPtr ? getMember<uint64_t>(mappedBufPtr, 0x38) : 0;
	bool safeForOriginal = mappedBufPtr && gpuVA != 0 && gpuBufSize >= 0xD040;
	const bool forceUnsafeOriginal = checkKernelArgument("-ngreenV69ForceOriginalUnsafe");
	const bool shouldTryOriginal = allowOriginal && !skipOriginal && safeForOriginal &&
		(NGreen::callback->isRealTGL || forceUnsafeOriginal);
	if (shouldTryOriginal) {
		SYSLOG("ngreen", "V111B: calling original Blit3D init (opt-in, fullMTL=%d allow=%d skip=%d base=0x%llx size=0x%llx)",
			   forceFullMTL, allowOriginal, skipOriginal,
			   (unsigned long long)gpuBufBase, (unsigned long long)gpuBufSize);
		FunctionCast(IGHardwareBlit3DContextinitialize, callback->oIGHardwareBlit3DContextinitialize)(that);
		SYSLOG("ngreen", "V111B: original Blit3D init completed");
		return;
	}
	if (!NGreen::callback->isRealTGL && allowOriginal && !forceUnsafeOriginal) {
		SYSLOG("ngreen", "V111B: -ngreenV69AllowOriginal blocked on spoofed RPL/ADL (use -ngreenV69ForceOriginalUnsafe only for crash debugging)");
	}
	if (allowOriginal && !safeForOriginal) {
		SYSLOG("ngreen", "V106: -ngreenV69AllowOriginal requested but rejected (base=0x%llx size=0x%llx)",
			   (unsigned long long)gpuBufBase, (unsigned long long)gpuBufSize);
	} else if (!NGreen::callback->isRealTGL && forceFullMTL && skipOriginal) {
		SYSLOG("ngreen", "V111B: full-MTL active and original Blit3D init disabled by -ngreenV69SkipOriginal");
	} else if (!NGreen::callback->isRealTGL && forceFullMTL && !allowOriginal) {
		SYSLOG("ngreen", "V111B: full-MTL active; original Blit3D init remains disabled by default (use -ngreenV69AllowOriginal to test)");
	} else if (v69Verbose || v69CallCount == 16 || v69CallCount == 64 || v69CallCount == 256) {
		SYSLOG("ngreen", "V106: skip original Blit3D init on non-real TGL (base=0x%llx size=0x%llx)",
			   (unsigned long long)gpuBufBase, (unsigned long long)gpuBufSize);
	}

	// Dump context fields 0xb0-0x120 (includes buffer pointers, sizes, GPU addresses)
	for (int i = 0; v69Verbose && i < 16; i++) {
		uint64_t val = getMember<uint64_t>(that, 0xb0 + i * 8);
		if (val != 0) {
			SYSLOG("ngreen", "V69: ctx[0x%03x]=0x%016llx", 0xb0 + i * 8, (unsigned long long)val);
		}
	}

	// V148: Replicate initialize() ourselves, skipping only blit3d_initialize_scratch_space.
	//
	// initWithOptions IDA (0x7CEBC) clarifies the full field layout when initialize() runs:
	//   ctx+0xB8 — set by BASE CLASS IGHardwareContext::initWithOptions before we are called
	//   ctx+0xD8 — IGSharedMappedBuffer* scratch buf, allocated by initWithOptions from params+0x10
	//   ctx+0xE0 — optional extra IGMappedBuffer, only if params+0x18 != 0 (Blit3D: not used)
	//   initialize() is called LAST (vtable[0x130]) after all fields are set.
	//
	// Original initialize() body (from disassembly at 0x7d8b8):
	//   1. Zero ctx+0xe8, 0xf0, 0xf8, 0x100, 0x108, 0x110
	//   2. ctx+0xd8 -> getMemory() -> blit3d_initialize_scratch_space()
	//      ← CRASH on RPL: memcpy writes static GPU blit shader kernels to write-protected page
	//      (allocation succeeds, GPU VA valid, but CPU-side lockForCPUAccess ptr is read-only)
	//   3. tail-call blit3d_init_ctx(ctx)
	//      ← reads ctx+0xD8 for getGPUVirtualAddress(), reads ctx+0xE0 for getBufferPtrNoInc()
	//      ← writes GPU 3D state init commands into the command buffer (ctx+0xE0 pool slot)
	//
	// We zero the fields (step 1), skip scratch-space init (step 2), then call
	// oblit3d_init_ctx directly (step 3). ctx+0xD8 must be non-null for blit3d_init_ctx
	// to safely call getGPUVirtualAddress() — guard before calling.
	auto *base = reinterpret_cast<uint8_t *>(that);
	*reinterpret_cast<uint64_t *>(base + 0xe8)  = 0;
	*reinterpret_cast<uint64_t *>(base + 0xf0)  = 0;
	*reinterpret_cast<uint64_t *>(base + 0xf8)  = 0;
	*reinterpret_cast<uint64_t *>(base + 0x100) = 0;
	*reinterpret_cast<uint64_t *>(base + 0x108) = 0;
	*reinterpret_cast<uint32_t *>(base + 0x110) = 0;

	if (!callback->oblit3d_init_ctx) {
		SYSLOG("ngreen", "V148: oblit3d_init_ctx is null, cannot init GPU command buffer");
		return;
	}
	// ctx+0xD8 must be valid before calling blit3d_init_ctx (it reads it for GPU VA).
	// In practice it is always set by initWithOptions from params+0x10; guard for safety.
	if (!mappedBufPtr) {
		SYSLOG("ngreen", "V148: ctx+0xD8 is null (scratch buf not allocated), skipping blit3d_init_ctx");
		return;
	}

	// xe/blitscratch (19/10): pular o passo 2 deixava o scratch dos kernels blit3d ZERADO (dump: instrBase 0x477a3000
	// +0x4c0 = 0). Todo blit 3D do kernel (flip do pipe de display, cópias de IOSurface) executava zeros nas EUs ->
	// RCS preso no PIPE_CONTROL depois do 3DPRIMITIVE -> gpuRestart em cascata. Refaz o passo 2 pelo wrapper
	// (que exige lock/unlock do IOAF2 resolvidos, V181). -ngreennoblitscratch volta ao comportamento antigo.
	static int blitScratch = -1;
	if (blitScratch < 0) blitScratch = (checkKernelArgument("-ngreenxefix") && !checkKernelArgument("-ngreennoblitscratch")) ? 1 : 0;
	if (blitScratch == 1 && callback->oIGMappedBuffergetMemory) {
		// 19/10 (BLITSCRATCH2): o wrapper V181 abortava (resolução de lock/unlock comentada em kern_green). Agora
		// chama o original com lockForCPUAccess redirecionado para a CPU VA do IGSharedMappedBuffer (ver wrapLockCpu).
		void *mem = FunctionCast(IGMappedBuffergetMemory, callback->oIGMappedBuffergetMemory)(mappedBufPtr);
		uint64_t bufSize = getMember<uint64_t>(mappedBufPtr, 0x20);
		void *cpu = gSmbGetVA ? reinterpret_cast<void *(*)(void *)>(gSmbGetVA)(mappedBufPtr) : nullptr;
		uint32_t antes = cpu ? *reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(cpu) + 0x4c0) : 0xdead;
		bool ok = mem && cpu && gXeLockRouted && bufSize >= 0xd210 && callback->oblit3d_initialize_scratch_space;
		// 19/10: a CPU VA do buffer só mapeia 0xd000 bytes (pânico: write fault em cpu+0xd000 no memcpy de 0x44 bytes,
		// o mesmo crash do V69). O original escreve num buffer temporário e só [0, 0xd000) é copiado (todos os kernels
		// ficam abaixo de 0xd000; perdem-se os estados de sampler/blend em 0xd000..0xd210).
		void *tmp = ok ? IOMallocZero(0xe000) : nullptr;
		if (ok && tmp) {
			gXeBlitCpu = tmp;
			gXeBlitMem = mem;
			FunctionCast(blit3d_initialize_scratch_space, callback->oblit3d_initialize_scratch_space)(mem);
			gXeBlitMem = nullptr;
			gXeBlitCpu = nullptr;
			memcpy(cpu, tmp, 0xd000);
		}
		if (tmp) IOFree(tmp, 0xe000);
		uint32_t depois = cpu ? *reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(cpu) + 0x4c0) : 0xdead;
		static int bsN = 0;
		if (bsN++ < 8)
			IOLog("XeNG BLITSCRATCH2: mem=%p cpu=%p size=0x%llx routed=%d ok=%d [0x4c0] %08x -> %08x (esperado 2420005b)\n",
				  mem, cpu, (unsigned long long)bufSize, gXeLockRouted, ok, antes, depois);
	}

	uint64_t rc = FunctionCast(blit3d_init_ctx, callback->oblit3d_init_ctx)(that);
	if (v69Verbose)
		SYSLOG("ngreen", "V148: blit3d_init_ctx returned %llu, ctx+0xb8=0x%llx ctx+0xd8=%p ctx+0xe0=%p",
			   (unsigned long long)rc,
			   (unsigned long long)getMember<uint64_t>(that, 0xb8),
			   mappedBufPtr,
			   getMember<void *>(that, 0xe0));

}

// V163: Hook startGraphicsEngine to clear PERCTX_PREEMPT_CTRL before first execlist context snapshot.
//
// Root cause: IntelAccelerator::startGraphicsEngine writes 0x40004000 to MMIO 0x20E0
// (FF_SLICE_CS_CHICKEN1). That masked write enables bit 14 (PERCTX_PREEMPT_CTRL).
// When the GPU loads its first execlist context it snapshots all GPU registers into
// the context image DMA buffer in GGTT memory. From that point every context-switch
// restore re-writes 0x4000 back to 0x20E0 from the DMA buffer — overriding any MMIO
// clear we do after the fact.
//
// Fix: call the original (must keep its side-effects), then immediately clear bit 14
// using a masked write (upper 16 = mask, lower 16 = 0). This fires before any context
// is ever submitted, so the DMA buffer snapshot captures 0x0000 instead of 0x4000.
// xe/inert (-ngreenengineinert, 2026-10-18): hipotese de teste — a GPU executando comandos escreve por DMA em enderecos errados
// e corrompe memoria (heap do WindowServer e do kernel; panics pmap/zalloc). Com este modo o startGraphicsEngine NAO ativa
// nenhum engine (RING_CTL/execlist ficam parados) e devolve sucesso; o display continua (nao depende dos engines).
// Esperado: Metal fica esperando a GPU (stamps) sem corromper memoria. Estado dos aneis e logado a cada chamada.
unsigned long Gen11::startGraphicsEngine(void *that)
{
	static int startCount = 0;

	if (xeEngineInertOn()) {
		static int inertN = 0;
		if (inertN++ < 16)
			SYSLOG("ngreen", "XE-INERT[%d]: startGraphicsEngine PULADO (RCS CTL=0x%08x HEAD=0x%x TAIL=0x%x | BCS CTL=0x%08x) -> sucesso",
				   inertN, NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE)), NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE)),
				   NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE)), NGreen::callback->readReg32(RING_CTL(BLT_RING_BASE)));
		return 1;
	}

	if (!NGreen::callback->isRealTGL) {
		startCount++;
		applyPreStartEngineWorkarounds(startCount);
	}

	unsigned long ret = FunctionCast(startGraphicsEngine, callback->ostartGraphicsEngine)(that);

	if (!NGreen::callback->isRealTGL) {
		// Masked clear: mask=bit14, value=0 → 0x40000000
		NGreen::callback->writeReg32(GEN7_FF_SLICE_CS_CHICKEN1,
			(GEN9_FFSC_PERCTX_PREEMPT_CTRL << 16) | 0);
		SYSLOG("ngreen", "V163: startGraphicsEngine post-clear FF_SLICE_CS_CHICKEN1=0x%08x (ret=%lu)",
			NGreen::callback->readReg32(GEN7_FF_SLICE_CS_CHICKEN1), ret);
		xeLrcaRepair("post-start");
	}

	return ret;
}

// Minimal pre-start safety: clear ERROR_GEN6 + mask EMR only.
// GT chicken/clock-gate WAs must NOT run before Apple's startGraphicsEngine — they
// interfere with ring initialization and leave CTL=0 (ring never enabled).
// GT WAs are applied pre-stop via applyPreStopEngineWorkarounds; they persist in
// hardware state and the context image (V504) across the stop→start cycle.

// xe/xefix: Wa_1607297627 (i915 rcs_engine_wa_init, tgl/adl-p): RING_PSMI_CTL(RCS) bits 12|7
// (RC_SEMA_IDLE_MSG_DISABLE | WAIT_FOR_EVENT_POWER_DOWN_DISABLE), registrador mascarado. No NootedGreen
// original essa WA ficava em código morto (depois de um return). Sem ela o RCS dorme esperando evento e o
// acelerador trava em "timeout waiting for IntelAccelerator stamp". O IntelIGPU-Tahoe aplica a mesma coisa.
static void xeFixApplyRcsPsmi(const char *where, int n) {
	if (!xeFixEnabled() || !NGreen::callback) return;
	uint32_t before = NGreen::callback->readReg32(0x2050);
	NGreen::callback->writeReg32(0x2050, (0x1080u << 16) | 0x1080u);
	uint32_t after = NGreen::callback->readReg32(0x2050);
	static int logs = 0;
	if (logs++ < 16) IOLog("XeNG xefix: RCS PSMI %s[%d] 0x%x -> 0x%x\n", where, n, before, after);
}
void Gen11::applyPreStartEngineWorkarounds(int callCount)
{
	xeFixApplyRcsPsmi("pre-start", callCount);
	uint32_t preErr = NGreen::callback->readReg32(ERROR_GEN6);
	if (preErr) {
		NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
		SYSLOG("ngreen", "V71S[%d]: pre-start ERR=0x%x cleared", callCount, preErr);
	}
	NGreen::callback->writeReg32(RING_EMR(RENDER_RING_BASE), 0xFFFFFFFF);
	NGreen::callback->writeReg32(RING_EMR(BLT_RING_BASE),    0xFFFFFFFF);
}

// Full GT workarounds + pre-engine error clearing + BCS drain.
// Called before stopGraphicsEngine only — these WAs are safe to apply to a running
// ring and need to be in place before the context image is captured on stop.
void Gen11::applyPreStopEngineWorkarounds(int callCount)
{
	xeFixApplyRcsPsmi("pre-stop", callCount);
	// ── GT workarounds (gen12_gt_workarounds_init / rcs_engine_wa_init) ──
	// Applied on every reset — same set as Linux i915 for TGL/ADL-P.
	/* Wa_14011060649:tgl,rkl,dg1,adl-s,adl-p */
	NGreen::callback->wa_write_or(VDBOX_CGCTL3F10(RENDER_RING_BASE), IECPUNIT_CLKGATE_DIS);
	NGreen::callback->wa_write_or(VDBOX_CGCTL3F10(BLT_RING_BASE),    IECPUNIT_CLKGATE_DIS);
	NGreen::callback->wa_write_or(VDBOX_CGCTL3F10(GEN11_VEBOX_RING_BASE), IECPUNIT_CLKGATE_DIS);
	/* Wa_14011059788:tgl,rkl,adl-s,dg1,adl-p */
	NGreen::callback->wa_mcr_write_or(GEN10_DFR_RATIO_EN_AND_CHICKEN, DFR_DISABLE);
	/* Wa_14015795083 */
	NGreen::callback->wa_add(GEN7_MISCCPCTL, GEN12_DOP_CLOCK_GATE_RENDER_ENABLE, 0, 0, false);
	/* Wa_1409142259:tgl,dg1,adl-p */
	NGreen::callback->wa_masked_en(GEN11_COMMON_SLICE_CHICKEN3, GEN12_DISABLE_CPS_AWARE_COLOR_PIPE);
	/* WaDisableGPGPUMidThreadPreemption:gen12 */
	NGreen::callback->wa_masked_field_set(GEN8_CS_CHICKEN1,
			GEN9_PREEMPT_GPGPU_LEVEL_MASK, GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL);
	/* MCR selector: slice 0, subslice 0 */
	NGreen::callback->wa_write_clr_set(GEN8_MCR_SELECTOR,
			GEN8_MCR_SLICE_MASK | GEN8_MCR_SUBSLICE_MASK,
			GEN8_MCR_SLICE(0) | GEN8_MCR_SUBSLICE(0));
	/* rcs_engine_wa_init */
	NGreen::callback->wa_masked_en(GEN7_FF_SLICE_CS_CHICKEN1, GEN9_FFSC_PERCTX_PREEMPT_CTRL);

	// V71 PRE: clear ERROR_GEN6 and mask EMR
	{
		uint32_t preErr = NGreen::callback->readReg32(ERROR_GEN6);
		if (preErr) {
			NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
			SYSLOG("ngreen", "V71[%d]: pre-engine ERR=0x%x cleared", callCount, preErr);
		}
		NGreen::callback->writeReg32(RING_EMR(RENDER_RING_BASE), 0xFFFFFFFF);
		NGreen::callback->writeReg32(RING_EMR(BLT_RING_BASE),    0xFFFFFFFF);
	}

	// V152: drain and disable BCS ring so Apple sees a clean BCS state
	{
		uint32_t bcsTail = NGreen::callback->readReg32(RING_TAIL(BLT_RING_BASE));
		uint32_t bcsHead = NGreen::callback->readReg32(RING_HEAD(BLT_RING_BASE));
		uint32_t bcsCtl  = NGreen::callback->readReg32(RING_CTL(BLT_RING_BASE));
		if (bcsCtl != 0 || bcsHead != bcsTail) {
			if (bcsHead != bcsTail) {
				NGreen::callback->writeReg32(RING_TAIL(BLT_RING_BASE), bcsHead);
				SYSLOG("ngreen", "V152[%d]: BCS drain TAIL 0x%x→0x%x HEAD=0x%x CTL=0x%x",
					   callCount, bcsTail, bcsHead, bcsHead, bcsCtl);
			}
			NGreen::callback->writeReg32(RING_CTL(BLT_RING_BASE), 0x0);
			SYSLOG("ngreen", "V152[%d]: BCS disabled (CTL was 0x%x)", callCount, bcsCtl);
		}
	}
}

// stopGraphicsEngine is the real GPU reset entry point on this binary (resetGraphicsEngine
// symbol is absent). Stop-specific: ring snapshot, V155 save/restore, V162 PERCTX clear,
// V71 POST CSB drain. GT workarounds are in applyPreStopEngineWorkarounds() above.
unsigned long Gen11::stopGraphicsEngine(void *that)
{
	static int   v63ResetCount            = 0;
	static uint32_t v155SavedRcsCtl       = 0;
	static int   v164GdrstCount           = 0;
	static int   v505Count                = 0;

	if (!NGreen::callback->isRealTGL) {
		v63ResetCount++;

		applyPreStopEngineWorkarounds(v63ResetCount);

		// V155: save RCS CTL while ring is still intact
		{
			uint32_t ctlNow = NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE));
			if (ctlNow != 0)
				v155SavedRcsCtl = ctlNow | 1;
		}

		// V53/V161 PRE: snapshot ring and EU state before stop
		uint32_t rcsHead  = NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE));
		uint32_t rcsTail  = NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE));
		uint32_t rcsStart = NGreen::callback->readReg32(RING_START(RENDER_RING_BASE));
		SYSLOG("ngreen", "V53[%d] stop PRE: RCS CTL=0x%x HEAD=0x%x TAIL=0x%x START=0x%x EXEC=0x%x",
			v63ResetCount,
			NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE)),
			rcsHead, rcsTail, rcsStart,
			NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE)));
		SYSLOG("ngreen", "V161[%d] stop PRE: INSTDONE_1=0x%08x SLICE_DONE=0x%08x CS_CHKN1=0x%08x FF_CHKN1=0x%08x",
			v63ResetCount,
			NGreen::callback->readReg32(0x206c),
			NGreen::callback->readReg32(0x2090),
			NGreen::callback->readReg32(GEN8_CS_CHICKEN1),
			NGreen::callback->readReg32(GEN7_FF_SLICE_CS_CHICKEN1));

		// V505: dump ring preamble contents — ring still running at this point
		if (v505Count < 3 && rcsStart) {
			uint32_t ringPage = rcsStart >> 12;
			uint32_t rpteHi   = NGreen::callback->readReg32(GGTT_PTE_HI(ringPage));
			uint32_t rpteLo   = NGreen::callback->readReg32(GGTT_PTE_LO(ringPage));
			if (rpteLo & 1) {
				v505Count++;
				uint64_t physBase = ((uint64_t)rpteHi << 32) | (rpteLo & 0xFFFFF000u);
				SYSLOG("ngreen", "V505[%d]: ring phys=0x%llx HEAD=0x%x TAIL=0x%x — 32 dwords:",
					   v505Count, (unsigned long long)physBase, rcsHead, rcsTail);
				for (int i = 0; i < 32; i++) {
					uint32_t dw = IOMappedRead32(physBase + (uint64_t)(i * 4));
					SYSLOG("ngreen", "V505[%d]:  [%02d] +0x%02x = 0x%08x", v505Count, i, i * 4, dw);
				}
			} else {
				SYSLOG("ngreen", "V505: PTE invalid rpteHi=0x%x rpteLo=0x%x", rpteHi, rpteLo);
			}
		}

		// V164: fire GDRST if ghost context detected (EXEC active, ring empty)
		{
			uint32_t execStatus = NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE));
			const bool ghostActive = (execStatus & 1) && (rcsHead == rcsTail);
			if (ghostActive && v164GdrstCount < 8) {
				v164GdrstCount++;
				NGreen::callback->writeReg32(GEN6_GDRST, GEN6_GRDOM_RENDER);
				uint32_t gdrstVal = GEN6_GRDOM_RENDER;
				int pollCount = 0;
				while ((gdrstVal & GEN6_GRDOM_RENDER) && pollCount < 1000) {
					gdrstVal = NGreen::callback->readReg32(GEN6_GDRST);
					pollCount++;
				}
				SYSLOG("ngreen", "V164[%d]: GDRST #%d poll=%d gdrst=0x%x EXEC 0x%x->0x%x",
					v63ResetCount, v164GdrstCount, pollCount, gdrstVal,
					execStatus, NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE)));
			}
		}
	}

	unsigned long ret = FunctionCast(stopGraphicsEngine, callback->ostopGraphicsEngine)(that);

	if (!NGreen::callback->isRealTGL) {
		// V53/V161 POST: snapshot after stop
		SYSLOG("ngreen", "V53[%d] stop POST: RCS CTL=0x%x HEAD=0x%x TAIL=0x%x EXEC=0x%x",
			v63ResetCount,
			NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE)));

		// V162: clear PERCTX_PREEMPT_CTRL after stop — TGL context image sets bit 14, RPL hangs
		NGreen::callback->writeReg32(GEN7_FF_SLICE_CS_CHICKEN1,
				(GEN9_FFSC_PERCTX_PREEMPT_CTRL << 16) | 0);

		// V155: restore RCS ring enable so Apple's startGraphicsEngine can take over
		if (v155SavedRcsCtl != 0)
			NGreen::callback->writeReg32(RING_CTL(RENDER_RING_BASE), v155SavedRcsCtl | 1);

		// V71 POST: clear any errors generated by stop + drain CSB
		{
			uint32_t postErr = NGreen::callback->readReg32(ERROR_GEN6);
			if (postErr) {
				NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
				SYSLOG("ngreen", "V71[%d]: post-stop ERR=0x%x cleared", v63ResetCount, postErr);
			}
			NGreen::callback->writeReg32(RING_EMR(RENDER_RING_BASE), 0xFFFFFFFF);
			NGreen::callback->writeReg32(RING_EMR(BLT_RING_BASE),    0xFFFFFFFF);
			// Drain CSB: advance read pointer to write pointer
			uint32_t csbReg   = NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
			uint32_t csbWrite = (csbReg >> 8) & 0xff;
			uint32_t csbRead  = csbReg & 0xff;
			if (csbRead != csbWrite)
				NGreen::callback->writeReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE),
											 (csbWrite << 8) | csbWrite);
			SYSLOG("ngreen", "V160[%d]: CSB rp %d→%d EXEC=0x%x FF_CS_CHKN1=0x%08x",
				v63ResetCount, csbRead, csbWrite,
				NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE)),
				NGreen::callback->readReg32(GEN7_FF_SLICE_CS_CHICKEN1));
		}
	}
	return ret;
}

void *  Gen11::IGHardwareBlit3DContextoperatornew(unsigned long size)
{
	if (!callback->oIGHardwareBlit3DContextoperatornew) {
		// V143: symbol was never registered — this is the root-cause of context alloc always
		// returning null, leading to the perpetual NULL-task submitBlit flood and BCS stall.
		if (isExperimentalMonitorEnabled()) {
			static bool v143NewNullLogged = false;
			if (!v143NewNullLogged) {
				v143NewNullLogged = true;
				SYSLOG("ngreen", "V143: oIGHardwareBlit3DContextoperatornew is null (RouteRequest missing?)");
			}
		}
		return nullptr;
	}
	auto ret = FunctionCast(IGHardwareBlit3DContextoperatornew, callback->oIGHardwareBlit3DContextoperatornew)(size);
	return ret;
}

// V509: Hook IGHardwareContext::initWithOptions (base class) — CPU-side LRCA page1 repair.
// Apple's initWithOptions calls restoreFromSafeImage() before copying g_cInitGfxRingContextRCS.
// On RPL that call returns true (skips the memcpy), leaving MI_LRI headers at DW1/DW8 as
// 0x00ffffff GEM-fill.  Hardware sees 0x00ffffff=MI_NOOP at DW1, skips register restore entirely,
// ring state is undefined, context posts IDLE immediately → startGraphicsEngine ret=3022544385.
//
// Fix: after original returns, if DW1 is still 0x00ffffff (only for RCS, this[0x6c]==0), write
// the minimal Gen12 ring context MI_LRI block via aperture (BAR2 remap of GGTT page1 PTE).
// LRCA GPU VA is in this+0x89 (set by initWithOptions on success path per Ghidra decompile).
// RING_START and RING_CTL are read from live MMIO so they match what Apple's ring-setup wrote.
uint64_t Gen11::IGHardwareContextinitWithOptions(void *that, void *task, const void *params, uint8_t arg)
{
	uint64_t ret = FunctionCast(IGHardwareContextinitWithOptions,
	                             callback->oIGHardwareContextinitWithOptions)(that, task, params, arg);
	auto *cb = NGreen::callback;
	if (cb->isRealTGL || !ret) return ret;

	uint8_t engType = getMember<uint8_t>(that, 0x6c);
	if (engType != 0) return ret;  // only repair RCS (case 0)

	// LRCA GPU VA is stored at this+0x89 (4-byte field, written by initWithOptions success path).
	// Bits [31:12] = 4K-aligned GPU physical address of LRCA page0.  Page1 = page0 + 0x1000.
	uint32_t lrcaGpuVa = getMember<uint32_t>(that, 0x89) & 0xFFFFF000;
	if (!lrcaGpuVa) {
		SYSLOG("ngreen", "V509: LRCA GPU VA zero, skipping repair");
		return ret;
	}
	uint32_t page1Idx = (lrcaGpuVa >> 12) + 1;

	// Flush CPU caches so aperture (UC) sees true DRAM content written by initWithOptions.
	asm volatile("wbinvd" ::: "memory");
	cb->setApertureIfNecessary();
	uint32_t p1Lo = cb->readReg32(GGTT_PTE_LO(page1Idx));
	uint32_t p1Hi = cb->readReg32(GGTT_PTE_HI(page1Idx));
	if (!(p1Lo & 1) || !cb->aperturePtr || cb->apertureLen < 0x1000) {
		SYSLOG("ngreen", "V509: LRCA page1 PTE not present (idx=%u lo=%08x), skipping", page1Idx, p1Lo);
		return ret;
	}

	volatile uint32_t *ap = cb->aperturePtr;
	uint32_t saveLo = cb->readReg32(GGTT_PTE_LO(0));
	uint32_t saveHi = cb->readReg32(GGTT_PTE_HI(0));
	cb->writeReg32(GGTT_PTE_LO(0), p1Lo);
	cb->writeReg32(GGTT_PTE_HI(0), p1Hi);
	cb->writeReg32(0x101008, 0x1);

	IOLog("XeNG V509 init lrca=%x DW0..3=%08x %08x %08x %08x DW80=%08x %s\n", lrcaGpuVa, (uint32_t)ap[0], (uint32_t)ap[1],
		  (uint32_t)ap[2], (uint32_t)ap[3], (uint32_t)ap[80], ap[1] == 0x00ffffff ? "REPARA" : "ok");
	if (ap[1] != 0x00ffffff) {
		// Already has a valid LRI header — restoreFromSafeImage gave us a good image.
		SYSLOG("ngreen", "V509: DW1=%08x (valid), no repair needed", (uint32_t)ap[1]);
	} else {
		// Read ring setup from live MMIO — Apple's ring-setup code runs before context creation.
		uint32_t rcsStart = cb->readReg32(0x2038);   // RCS RING_START
		uint32_t rcsCtl   = cb->readReg32(0x2040);   // RCS RING_CTL (bit0=0 when stopped)
		if (!rcsStart) rcsStart = 0x402eb000;         // fallback from HANGCHECK

		SYSLOG("ngreen", "V509: repairing LRCA page1 lrcaGpuVa=%08x RING_START=%08x RING_CTL=%08x",
		       lrcaGpuVa, rcsStart, rcsCtl);

		// Zero DW0-DW79 (replaces 0x00ffffff GEM-fill with MI_NOOPs).
		for (int i = 0; i < 80; i++) ap[i] = 0;

		// LRI block 0 (DW0-DW7): CTX_CTRL | RING_HEAD | RING_TAIL  — 3 pairs, force-posted.
		// Gen12 MI_LRI_FORCE_POSTED = bit 12 (confirmed from else-branch header 0x11001003).
		ap[1]  = 0x11001005;   // MI_LRI(3) | FORCE_POSTED
		ap[2]  = 0x00002090;   // RING_CONTEXT_CONTROL addr (RCS)
		ap[3]  = 0xffff0001;   // CTX_CTRL: inhibit sync ctx switch (from initWithOptions decompile)
		ap[4]  = 0x00002034;   // RING_HEAD addr
		ap[5]  = 0x00000000;   // RING_HEAD = 0
		ap[6]  = 0x00002030;   // RING_TAIL addr
		ap[7]  = 0x00000000;   // RING_TAIL = 0

		// LRI block 1 (DW8-DW12): RING_START | RING_CTL  — 2 pairs, force-posted.
		ap[8]  = 0x11001003;   // MI_LRI(2) | FORCE_POSTED
		ap[9]  = 0x00002038;   // RING_START addr
		ap[10] = rcsStart;     // RING_START value (from live MMIO)
		ap[11] = 0x00002040;   // RING_CTL addr
		ap[12] = rcsCtl | 1;   // RING_CTL | RING_VALID

		// Apple context-image valid marker at DW80 (from initWithOptions decompile:
		// *(pSVar10 + 0x1140) = 0x5000001, where 0x1140 - 0x1000 = 0x140 = 80*4).
		ap[80] = 0x05000001;

		asm volatile("sfence" ::: "memory");
		SYSLOG("ngreen", "V509: repair done DW1=%08x DW10(RING_START)=%08x DW12(CTL)=%08x",
		       (uint32_t)ap[1], rcsStart, rcsCtl | 1);
	}

	cb->writeReg32(GGTT_PTE_LO(0), saveLo);
	cb->writeReg32(GGTT_PTE_HI(0), saveHi);
	cb->writeReg32(0x101008, 0x1);
	return ret;
}

// V508/V509: Hook IGHardwareContext::withOptions.
// V508 (diagnostic, first 4 calls): logs FIFO/ring-buffer fields and page1 raw content.
// V509 (repair, every RCS call): wbinvd flushes any stale cache write that re-stamped DW1 with
//   0x00ffffff after initWithOptions, then writes the correct Gen12 ring context MI_LRI block to
//   GGTT[0x19] (ExecList submission slot for page1) via aperture.  Without this, hardware sees
//   MI_NOOP at DW1, skips all register restore, ring state is undefined, context posts IDLE.
void *Gen11::IGHardwareContextwithOptions(void *task, const void *params, uint8_t arg)
{
	void *ctx = FunctionCast(IGHardwareContextwithOptions,
	                         callback->oIGHardwareContextwithOptions)(task, params, arg);
	auto *cb = NGreen::callback;
	if (cb->isRealTGL || !ctx) return ctx;

	uint8_t engType = getMember<uint8_t>(ctx, 0x6c);

	// V508: diagnostic dump (limited to first 4 withOptions calls).
	static int v508Count = 0;
	if (v508Count < 4) {
		v508Count++;
		void *fifo = getMember<void *>(ctx, 0xb8);
		SYSLOG("ngreen", "V508[%d]: ctx=%p fifo=%p arg=%u engType=%u",
		       v508Count, ctx, fifo, (unsigned)arg, (unsigned)engType);
		if (fifo) {
			for (int i = 0; i < 12; i++) {
				uint64_t v = getMember<uint64_t>(fifo, i * 8);
				SYSLOG("ngreen", "V508[%d]: fifo[0x%02x]=0x%016llx", v508Count, i*8, (unsigned long long)v);
			}
			void *ringBuf = getMember<void *>(fifo, 0x18);
			if (ringBuf && v508Count == 1) {
				SYSLOG("ngreen", "V508[%d]: ring_buf=%p:", v508Count, ringBuf);
				for (int i = 0; i < 20; i++) {
					uint64_t v = getMember<uint64_t>(ringBuf, i * 8);
					SYSLOG("ngreen", "V508[%d]:   rb[0x%03x]=0x%016llx", v508Count, i*8, (unsigned long long)v);
				}
			}
		}
	}

	// V509 repair: RCS contexts only.
	// The ExecList submission slot (GGTT[0x19]) is the physical page hardware reads on
	// context-restore. It starts as GEM fill (0x00ffffff / 0x00bfbfbf) because Apple never
	// copies the valid LRI block that restoreFromSafeImage wrote into the actual context
	// buffer (GGTT[ctxPage1Idx]).  Fix: read context buffer → copy to submission slot.
	// Also save the ctx pointer so V507 can re-run the repair on every populateResetRegisterList
	// call (the display retry loop reuses the same context without calling withOptions again).
	if (engType != 0) return ctx;
	cb->lastRCSCtx = ctx;

	uint32_t lrcaGpuVa = getMember<uint32_t>(ctx, 0x89) & 0xFFFFF000;
	if (!lrcaGpuVa) { SYSLOG("ngreen", "V509: LRCA GPU VA zero"); return ctx; }
	uint32_t ctxPage1Idx = (lrcaGpuVa >> 12) + 1;

	cb->setApertureIfNecessary();
	if (!cb->aperturePtr || cb->apertureLen < 0x1000) return ctx;

	uint32_t slotLo = cb->readReg32(GGTT_PTE_LO(0x19));
	uint32_t slotHi = cb->readReg32(GGTT_PTE_HI(0x19));
	uint32_t ctxLo  = cb->readReg32(GGTT_PTE_LO(ctxPage1Idx));
	uint32_t ctxHi  = cb->readReg32(GGTT_PTE_HI(ctxPage1Idx));
	if (!(slotLo & 1) || !(ctxLo & 1)) return ctx;

	asm volatile("wbinvd" ::: "memory");

	volatile uint32_t *ap = cb->aperturePtr;
	uint32_t saveLo = cb->readReg32(GGTT_PTE_LO(0));
	uint32_t saveHi = cb->readReg32(GGTT_PTE_HI(0));

	// Pass 1: read DW[0..95] from actual context buffer page1 into stack buffer.
	uint32_t buf[96];
	cb->writeReg32(GGTT_PTE_LO(0), ctxLo);
	cb->writeReg32(GGTT_PTE_HI(0), ctxHi);
	cb->writeReg32(0x101008, 0x1);
	for (int i = 0; i < 96; i++) buf[i] = ap[i];

	if (v508Count <= 4) {
		SYSLOG("ngreen", "V508: ctx  pg1 DW0..7: %08x %08x %08x %08x  %08x %08x %08x %08x",
		       buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
	}

	// Pass 2: map submission slot, check validity, copy if needed.
	cb->writeReg32(GGTT_PTE_LO(0), slotLo);
	cb->writeReg32(GGTT_PTE_HI(0), slotHi);
	cb->writeReg32(0x101008, 0x1);

	if (v508Count <= 4) {
		SYSLOG("ngreen", "V508: slot pg1 DW0..7: %08x %08x %08x %08x  %08x %08x %08x %08x",
		       ap[0], ap[1], ap[2], ap[3], ap[4], ap[5], ap[6], ap[7]);
	}

	IOLog("XeNG V509 withOptions lrca=%x slot19 DW1=%08x ctx DW1=%08x %s\n", lrcaGpuVa, (uint32_t)ap[1], buf[1],
		  ((ap[1] & 0x1F801000) != 0x11001000 && (buf[1] & 0x1F801000) == 0x11001000) ? "COPIA->slot19" : "-");
	if ((ap[1] & 0x1F801000) != 0x11001000) {
		if ((buf[1] & 0x1F801000) == 0x11001000) {
			for (int i = 0; i < 96; i++) ap[i] = buf[i];
			asm volatile("sfence" ::: "memory");
			SYSLOG("ngreen", "V509: ctx→slot copy done  DW1=%08x DW10=%08x DW12=%08x",
			       buf[1], buf[10], buf[12]);
		} else {
			SYSLOG("ngreen", "V509: both invalid  slot=%08x ctx=%08x — skipping",
			       (uint32_t)ap[1], buf[1]);
		}
	} else if (v508Count <= 4) {
		SYSLOG("ngreen", "V509: slot DW1=%08x already valid — no repair", (uint32_t)ap[1]);
	}

	cb->writeReg32(GGTT_PTE_LO(0), saveLo);
	cb->writeReg32(GGTT_PTE_HI(0), saveHi);
	cb->writeReg32(0x101008, 0x1);
	return ctx;
}

uint64_t Gen11::IGHardwareExtendedContextinitWithOptions(void *that,void *param_1,void *param_2)
{
	if (!callback->oIGHardwareExtendedContextinitWithOptions) {
		static bool v136ExtInitLogged = false;
		if (!v136ExtInitLogged) {
			v136ExtInitLogged = true;
			SYSLOG("ngreen", "V136: oIGHardwareExtendedContextinitWithOptions is null");
		}
		return 0;
	}
	void *b8pre = getMember<void *>(that, 0xb8);
	// V502: log ExtendedCtxParams+0x10 (buffer size) and +0x18 (scheduler flag).
	// From Ghidra: if param_2+0x18==0 → simple buffer alloc path (no GPU cmd).
	// If param_2+0x18!=0 → GPU command submission via vtable+0x118 (hang source on RPL).
	uint64_t p2f10 = param_2 ? getMember<uint64_t>(param_2, 0x10) : 0;
	uint64_t p2f18 = param_2 ? getMember<uint64_t>(param_2, 0x18) : 0;
	static bool v502iwoLogged = false;
	if (!v502iwoLogged) {
		v502iwoLogged = true;
		SYSLOG("ngreen", "V502: initWithOptions ExtCtxParams=%p +0x10=0x%llx +0x18=0x%llx (0=simple,!=0=GPU-cmd)",
			   param_2, (unsigned long long)p2f10, (unsigned long long)p2f18);
	}
	uint64_t ret = FunctionCast(IGHardwareExtendedContextinitWithOptions, callback->oIGHardwareExtendedContextinitWithOptions)(that,param_1,param_2);
	void *b8post = getMember<void *>(that, 0xb8);
	// V115 REMOVED: V115 suppressed initWithOptions to prevent MCE from GGTT[0]→stolen mem.
	// V116 now remaps GGTT[0] to a safe dummy page, so the MCE can't happen.
	// V115 was causing Apple's (un-hooked) getBlit3DContext to return nullptr, which then
	// made submitBlit+0x28e dereference nullptr+0xb8 → page fault. Pass through real result.
	static int v501Count = 0;
	if (v501Count < 8) {
		v501Count++;
		SYSLOG("ngreen", "V501[%d]: initWithOptions(%p) ret=%llu b8_pre=%p b8_post=%p p1=%p p2=%p",
			   v501Count, that, (unsigned long long)ret, b8pre, b8post, param_1, param_2);
	}
	return ret;
	
	uint8_t ctx=getMember<uint8_t>(that, 0x6c);
	
	//intel_engine_init_workarounds(engine);
		//engine_fake_wa_init
		/*if (engine->class == COMPUTE_CLASS)
		ccs_engine_wa_init(engine, wal);
		 else if (engine->class == RENDER_CLASS)
		rcs_engine_wa_init(engine, wal);
		 else
		xcs_engine_wa_init(engine, wal);*/
	
	//intel_engine_init_whitelist(engine);
	//intel_engine_init_ctx_wa(engine);
	
	if (ctx==0)// RCS
	{
		
		//engine_fake_wa_init
		uint8_t mocs= 3;
		NGreen::callback->wa_masked_field_set(
					RING_CMD_CCTL(RENDER_RING_BASE),
					CMD_CCTL_MOCS_MASK,
					CMD_CCTL_MOCS_OVERRIDE(mocs, mocs));
		
		
		
		
		//	rcs_engine_wa_init(engine, wal);
		//rcs_engine_wa_init
		//Wa_1606700617:tgl,dg1,adl-p
		NGreen::callback->wa_masked_en(
				 GEN9_CS_DEBUG_MODE1,
				 FF_DOP_CLOCK_GATE_DISABLE);
		
		
		// Wa_1606931601:tgl,rkl,dg1,adl-s,adl-p
		NGreen::callback->wa_mcr_masked_en( GEN8_ROW_CHICKEN2, GEN12_DISABLE_EARLY_READ);


		 // Wa_1407928979:tgl A
		NGreen::callback->wa_write_or( GEN7_FF_THREAD_MODE,
				GEN12_FF_TESSELATION_DOP_GATE_DISABLE);

		//general_render_compute_wa_init
		// Wa_1406941453:tgl,rkl,dg1,adl-s,adl-p
		NGreen::callback->wa_mcr_masked_en(
				 GEN10_SAMPLER_MODE,
				 ENABLE_SMALLPL);
		
		
		// Wa_1409804808
		NGreen::callback->wa_mcr_masked_en( GEN8_ROW_CHICKEN2,
				 GEN12_PUSH_CONST_DEREF_HOLD_DIS);

		// Wa_14010229206 /
		NGreen::callback->wa_mcr_masked_en( GEN9_ROW_CHICKEN4, GEN12_DISABLE_TDL_PUSH);
		
		// Wa_1607297627
		NGreen::callback->wa_masked_en(
				RING_PSMI_CTL(RENDER_RING_BASE),
				GEN12_WAIT_FOR_EVENT_POWER_DOWN_DISABLE |
				GEN8_RC_SEMA_IDLE_MSG_DISABLE);
		
		
		//if (GRAPHICS_VER(i915) >= 9)
		NGreen::callback->wa_masked_en(
				 GEN7_FF_SLICE_CS_CHICKEN1,
				 GEN9_FFSC_PERCTX_PREEMPT_CTRL);
		
		
		
		//tgl_whitelist_build
		//WaAllowPMDepthAndInvocationCountAccessFromUMD
		NGreen::callback->whitelist_reg_ext( PS_INVOCATION_COUNT,
				  RING_FORCE_TO_NONPRIV_ACCESS_RD |
				  RING_FORCE_TO_NONPRIV_RANGE_4);

		
		 // Wa_1808121037:tgl

		NGreen::callback->whitelist_reg( GEN7_COMMON_SLICE_CHICKEN1);

		// Wa_1806527549:tgl
		NGreen::callback->whitelist_reg( HIZ_CHICKEN);

		// Required by recommended tuning setting (not a workaround)
		NGreen::callback->whitelist_reg( GEN11_COMMON_SLICE_CHICKEN3);
		
		
		
		//intel_engine_init_ctx_wa
		//gen12_ctx_workarounds_init
		// * Wa_1409142259:tgl,dg1,adl-p
		
		NGreen::callback->wa_masked_en( GEN11_COMMON_SLICE_CHICKEN3,
									  GEN12_DISABLE_CPS_AWARE_COLOR_PIPE);
		
		/* WaDisableGPGPUMidThreadPreemption:gen12 */
		NGreen::callback->wa_masked_field_set( GEN8_CS_CHICKEN1,
											 GEN9_PREEMPT_GPGPU_LEVEL_MASK,
											 GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL);
		
		//* Wa_16011163337 - GS_TIMER
		NGreen::callback->wa_add(
								GEN12_FF_MODE2,
								~0,
								FF_MODE2_TDS_TIMER_128 | FF_MODE2_GS_TIMER_224,
								0, false);
		
						
	}
	
	if (ctx==1)//CCS
	{
		//return 0;
		//engine_fake_wa_init
		uint8_t mocs= 3;
		NGreen::callback->wa_masked_field_set(
					RING_CMD_CCTL(GEN12_COMPUTE0_RING_BASE),
					CMD_CCTL_MOCS_MASK,
					CMD_CCTL_MOCS_OVERRIDE(mocs, mocs));
		
		//panic("x");
	}
	
	if (ctx==2)//BCS
	{

		//gen12_ctx_gt_mocs_init
		/*table->size  = ARRAY_SIZE(tgl_mocs_table);
		table->table = tgl_mocs_table;
		table->n_entries = GEN9_NUM_MOCS_ENTRIES;
		table->uc_index = 3;*/
		uint8_t mocs;
		//if (engine->class == COPY_ENGINE_CLASS) {
			mocs = 3;
			NGreen::callback->wa_write_clr_set(
					 BLIT_CCTL(BLT_RING_BASE),//engine->mmio_base
					 BLIT_CCTL_MASK,
					 BLIT_CCTL_MOCS(mocs, mocs));
		
	}
	
	if (ctx==3)//VCS
	{
	}
	
	if (ctx==4)//VCS2
	{
	}
	
	if (ctx==5)//VECS
	{
	}
	
	
	
	return FunctionCast(IGHardwareExtendedContextinitWithOptions, callback->oIGHardwareExtendedContextinitWithOptions)(that,param_1,param_2);
}

void  Gen11::initBlitUsage(void *that)
{
	// V121: initBlitUsage reads [ctx+0xb8]+0x178 without null checks.
	// Guard: skip if ctx+0xb8 is not yet populated (early init paths before
	// blit3d_init_ctx has run). In the render phase ctx+0xb8 is always set.
	if (!NGreen::callback->isRealTGL) {
		if (!getMember<void *>(that, 0xb8)) {
			DBGLOG("ngreen", "V121: initBlitUsage deferred — ctx+0xb8 null");
			return;
		}
	}
	FunctionCast(initBlitUsage, callback->oinitBlitUsage)(that);
}

// xe/blitdiag (19/10): cada saída do submitBlit registra a linha de origem, a rota (routeSel) e o retorno (IOLog;
// o SYSLOG não aparece no Tahoe).
static unsigned long xeBlitRet(uint32_t k, uint32_t rs, void *task, int linha, unsigned long r) {
	if (k <= 48 || !(k & 63)) IOLog("XeNG BLIT #%u routeSel=%u task=%p saida=L%d ret=0x%lx\n", k, rs, task, linha, r);
	return r;
}
unsigned long Gen11::submitBlit(void *that, void *param_1, void *param_2, void *param_3, bool param_4) {
	static volatile uint32_t xeBlitN = 0;
	const uint32_t xeK = ++xeBlitN;
	const uint32_t xeRs = param_1 ? ((reinterpret_cast<uint8_t *>(param_1)[0xA2] >> 3U) & 0x3U) : 9;
	// V186: For spoofed non-TGL, if V142 is configured to bypass submitBlit,
	// return before any task/context touching logic. The V149/V171 path writes
	// task+0x298 and can poison task lifetime on some boots, later crashing in
	// IGAccelTask::release from the garbage collector interrupt path.
	if (!NGreen::callback->isRealTGL) {
		static int v186Mode = -1;
		if (v186Mode < 0) {
			v186Mode = getV142SubmitBlitMode();
			SYSLOG("ngreen", "V186: early submitBlit spoof mode=%d (0=unsupported,1=ret0,2=ret1,3=orig)", v186Mode);
		}

		if (v186Mode != 3) {
			if (isV142Diag2DBypassEnabled() && param_1) {
				auto *blit = reinterpret_cast<uint8_t *>(param_1);
				const bool has3DFlags =
					((blit[0x3C] & 1U) != 0) ||
					((blit[0x84] & 1U) != 0) ||
					(*reinterpret_cast<uint64_t *>(blit + 0x20) != 0) ||
					(*reinterpret_cast<uint64_t *>(blit + 0x68) != 0);
				const uint32_t routeSel = (blit[0xA2] >> 3U) & 0x3U;
				static int v186DiagCount = 0;
				if (v186DiagCount < 64) {
					v186DiagCount++;
					SYSLOG("ngreen", "V186D[%d]: mode=%d routeSel=%u has3D=%d task=%p",
						   v186DiagCount, v186Mode, routeSel, (int)has3DFlags, param_3);
				}
			}
			if (v186Mode == 2)
				return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(1));
			if (v186Mode == 1)
				return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(0));
			return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(static_cast<uint32_t>(kIOReturnUnsupported)));
		}
	}

	// V149: blit3D context is cached at task+0x298 per IGAccelTask::getBlit3DContext IDA.
	// Previous code used 0xb8 — that overwrote an unrelated IGAccelTask member.
	auto ensureTaskContext = [&](void *task, const char *origin) -> void * {
		if (!task || NGreen::callback->isRealTGL)
			return task ? getMember<void *>(task, 0x298) : nullptr;

		void *taskCtx = getMember<void *>(task, 0x298);
		if (taskCtx)
			return taskCtx;

		// param_1=true triggers allocation; Apple then stores result at task+0x298 internally.
		// V171: Our getBlit3DContext hook returns the global cached ctx but does NOT write
		// task+0x298. Apple's original would store it per-task. Do it explicitly here so
		// the invalidTask check (getMember<void*>(task,0x298) == nullptr) passes.
		void *ctx = callback->getBlit3DContext(task, true);
		if (!ctx)
			return nullptr;

		getMember<void *>(task, 0x298) = ctx;

		if (isExperimentalMonitorEnabled()) {
			static int v149Count = 0;
			if (v149Count < 32) {
				v149Count++;
				SYSLOG("ngreen", "V149[%d]: %s task=%p stored ctx@298=%p", v149Count, origin, task, ctx);
			}
		}

		return ctx;
	};

	bool invalidTask = (param_3 == nullptr);
	if (isExperimentalMonitorEnabled()) {
		static int v137SubmitEntryCount = 0;
		if (v137SubmitEntryCount < 40) {
			v137SubmitEntryCount++;
			void *entryVtable = param_3 ? getMember<void *>(param_3, 0x0) : nullptr;
			void *entryCtx = param_3 ? getMember<void *>(param_3, 0x298) : nullptr;
			SYSLOG("ngreen", "V149.submitBlit.entry[%d]: acc=%p p1=%p p2=%p task=%p vtbl=%p ctx@298=%p b=%u c3D=%p c2D=%p cTask=%p",
				   v137SubmitEntryCount, that, param_1, param_2, param_3, entryVtable, entryCtx,
				   static_cast<unsigned>(param_4), callback->v131CachedBlit3DCtx,
				   callback->v131CachedBlit2DCtx, callback->v132CachedTask);
		}
	}
	if (!invalidTask) {
		// V135: Some panics still reached Apple submitBlit with a non-null but broken task object
		// (null vtable / null ctx field), which can lead to RIP=0 indirect calls in Apple code.
		// Validate minimal task invariants before calling through.
		ensureTaskContext(param_3, "submitBlit-incoming");
		void *taskVtable = getMember<void *>(param_3, 0x0);
		void *taskCtx = getMember<void *>(param_3, 0x298);
		invalidTask = (taskVtable == nullptr) || (taskCtx == nullptr);
		if (invalidTask) {
			static int v135Count = 0;
			if (v135Count < 16) {
				v135Count++;
				SYSLOG("ngreen", "V149[%d]: submitBlit invalid task=%p vtbl=%p ctx@298=%p", v135Count, param_3, taskVtable, taskCtx);
			}
		}
	}

	if (invalidTask && callback->v132CachedTask && callback->v132CachedTask != param_3) {
		void *cachedCtx = ensureTaskContext(callback->v132CachedTask, "submitBlit-cached");
		void *cachedVtable = getMember<void *>(callback->v132CachedTask, 0x0);
		if (cachedVtable && cachedCtx) {
			if (isExperimentalMonitorEnabled()) {
				static int v138SwapCount = 0;
				if (v138SwapCount < 24) {
					v138SwapCount++;
					SYSLOG("ngreen", "V138.swap[%d]: replacing task=%p with cached=%p ctx=%p",
						   v138SwapCount, param_3, callback->v132CachedTask, cachedCtx);
				}
			}
			param_3 = callback->v132CachedTask;
			invalidTask = false;
		}
	}

	if (invalidTask) {
		static int v120Mode = -1;
		if (v120Mode < 0) {
			int parsed = 0;
			if (!NGreen::callback->isRealTGL) {
				// V120 modes for spoofed path:
				//   ngreenV120=0 or -ngreenV120ok   -> return success (legacy behavior)
				//   ngreenV120=1 or -ngreenV120fail -> return unsupported (default)
				//   ngreenV120=2 or -ngreenV120pass -> return 1
				if (PE_parse_boot_argn("ngreenV120", &parsed, sizeof(parsed))) {
					v120Mode = parsed;
				} else if (checkKernelArgument("-ngreenV120pass")) {
					v120Mode = 2;
				} else if (checkKernelArgument("-ngreenV120ok")) {
					v120Mode = 0;
				} else if (checkKernelArgument("-ngreenV120fail")) {
					v120Mode = 1;
				} else {
					v120Mode = 1;
				}
			} else {
				v120Mode = 0;
			}
			SYSLOG("ngreen", "V120: submitBlit NULL-task mode=%d (0=ret0,1=unsupported,2=ret1)", v120Mode);
		}

		static int v120NullCount = 0;
		if (v120NullCount < 32) {
			v120NullCount++;
			SYSLOG("ngreen", "V120[%d]: submitBlit NULL task that=%p p1=%p p2=%p bool=%d mode=%d",
				   v120NullCount, that, param_1, param_2, param_4, v120Mode);
		}

		if (isExperimentalMonitorEnabled()) {
			static int v137SubmitInvalidCount = 0;
			if (v137SubmitInvalidCount < 40) {
				v137SubmitInvalidCount++;
				SYSLOG("ngreen", "V137.submitBlit.invalid[%d]: task=%p cTask=%p c3D=%p c2D=%p mode=%d",
					   v137SubmitInvalidCount, param_3, callback->v132CachedTask,
					   callback->v131CachedBlit3DCtx, callback->v131CachedBlit2DCtx, v120Mode);
			}
		}

		if (v120Mode == 2)
			return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(1));
		if (v120Mode == 1)
			return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(static_cast<uint32_t>(kIOReturnUnsupported)));
		return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(0));
	}

	if (!NGreen::callback->isRealTGL) {
		// V142: after the movaps/movapd fixes, the remaining watchdog still points at
		// Apple's original blit submit path wedging BCS. Keep original behavior by default
		// to avoid breaking the renderer, but provide an explicit no-log test switch for
		// spoofed RPL boots when we need to prove the hang is inside submitBlit itself.
		static int v142Mode = -1;
		if (v142Mode < 0) {
			v142Mode = getV142SubmitBlitMode();
			SYSLOG("ngreen", "V142: submitBlit spoof mode=%d (0=hard-unsupported,1=ret0,2=ret1,3=orig)", v142Mode);
		}

		if (v142Mode != 3) {
			static int v142Count = 0;
			if (v142Count < 24) {
				v142Count++;
				SYSLOG("ngreen", "V142[%d]: bypass submitBlit acc=%p p1=%p p2=%p task=%p b=%u mode=%d",
					   v142Count, that, param_1, param_2, param_3, static_cast<unsigned>(param_4), v142Mode);
			}

			if (v142Mode == 2)
				return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(1));
			if (v142Mode == 1)
				return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(0));
			// Mode 0 is intentionally harsh and should only be used for explicit diagnostics.
			return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(static_cast<uint32_t>(kIOReturnUnsupported)));
		}
	}

	// V193: On !isRealTGL (RPL spoof), block routeSel=3 before calling Apple's submitBlit.
	// routeSel=3 routes to blit3d_submit_commands which executes TGL-compiled EU shaders
	// from the scratch buffer. RPL has a different EU topology — the shader unit stalls
	// (INSTDONE_1 bit clear), RCS hangs inside the batch at BB_ADDR, BCS then deadlocks
	// on a MI_SEMAPHORE_WAIT for the value RCS never writes → Sig 803 GPU reset loop.
	// routeSel=0..2 (2D blits, Apple logo, compositor) pass through safely.
	// Side effect: cursor sprite (uses routeSel=3) becomes invisible — acceptable vs. GPU hang.
	// xe (19/10): com -ngreenxefix o V193 fica desligado (religar com -ngreenv193). O "hang do shader TGL no RPL" era o
	// estado do blit3d gravado 0x18 bytes fora do lugar (corrigido pelo XE-POOLALIGN); bloquear o blit sem executar
	// deixa o stamp reservado pendurado (índice 9) e o WindowServer espera para sempre.
	static int v193Arg = -1;
	if (v193Arg < 0) v193Arg = (checkKernelArgument("-ngreenv193") || !checkKernelArgument("-ngreenxefix")) ? 1 : 0;
	if (v193Arg == 1 && !NGreen::callback->isRealTGL && param_1) {
		auto *blit193 = reinterpret_cast<uint8_t *>(param_1);
		const uint32_t routeSel193 = (blit193[0xA2] >> 3U) & 0x3U;
		if (routeSel193 == 3) {
			static int v193Count = 0;
			if (v193Count < 32) {
				v193Count++;
				SYSLOG("ngreen", "V193[%d]: blocked routeSel=3 on RPL (TGL EU shader → RCS hang)", v193Count);
			}
			return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(1));
		}
	}

	// Safety guard: avoid null indirect call if route capture failed.
	if (!callback->osubmitBlit) {
		static bool v134Logged = false;
		if (!v134Logged) {
			v134Logged = true;
			SYSLOG("ngreen", "V134: osubmitBlit is null, preventing call-through crash");
		}
		if (!NGreen::callback->isRealTGL)
			return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(0));
		return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(static_cast<unsigned long>(kIOReturnUnsupported)));
	}

	return xeBlitRet(xeK, xeRs, param_3, __LINE__, (unsigned long)(FunctionCast(submitBlit, callback->osubmitBlit)(that, param_1, param_2, param_3, param_4)));
}

void  Gen11::markBlitUsage(void *that)
{
	// V122: markBlitUsage dereferences [ctx+0xb8] without null checks.
	// Same guard as V121: skip only when ctx+0xb8 is not yet set.
	if (!NGreen::callback->isRealTGL) {
		if (!getMember<void *>(that, 0xb8)) {
			DBGLOG("ngreen", "V122: markBlitUsage deferred — ctx+0xb8 null");
			return;
		}
	}
	FunctionCast(markBlitUsage, callback->omarkBlitUsage)(that);
}

// still to order..

void * Gen11::IGMappedBuffergetMemory(void *that)
{
	auto ret=FunctionCast(IGMappedBuffergetMemory, callback->oIGMappedBuffergetMemory)(that);
	return ret;
}

uint32_t  Gen11::IGAccelSegmentResourceListprepare(void *that)
{
	// V123c: keep original prepare flow. initBlitUsage/markBlitUsage are already
	// individually guarded for !isRealTGL, so short-circuiting prepare is no longer needed
	// and can leave command-queue state incomplete, leading to stalls.
	return FunctionCast(IGAccelSegmentResourceListprepare, callback->oIGAccelSegmentResourceListprepare)(that);
}

uint32_t Gen11::beginCoalescedSegment(void *that) {
	// V124/IDA verified: beginCoalescedSegment is pure DTrace observability.
	// Full disassembly:
	//   [queue+0x830] = 0xFFFFFFFF           ← segment marker
	//   task = *(*(queue+0x5C0)+0x150)
	//   ctx  = getBlit3DContext(task, true)   ← may return null in early init
	//   r15  = ctx+0xB8                       ← CRASH if ctx null or ctx+0xB8 null
	//   r14  = r15+0x178                      ← usage counter
	//   dtracePushUnique(queue+0x5C0+0xE08, queue+0x838)
	//   dtHookSubmitQueueKMD(queue+0x838, 4, r14+1, r15+0x20, 0, 0)
	// No GPU ring submission whatsoever.
	//
	// Always write queue+0x830 first (matches Apple's implementation).
	reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(that) + 0x830)[0] = 0xFFFFFFFF;

	if (!NGreen::callback->isRealTGL) {
		// Guard: ctx+0xB8 must be non-null before calling through (DTrace reads it).
		// In early init getBlit3DContext returns null → skip DTrace, return 1.
		// In render phase ctx+0xB8 is always populated → call through for correct tracing.
		void *cached = callback->v131CachedBlit3DCtx;
		if (!cached || !getMember<void *>(cached, 0xb8)) {
			static int v124EarlyCount = 0;
			if (v124EarlyCount < 8) {
				v124EarlyCount++;
				DBGLOG("ngreen", "V124[%d]: beginCoalescedSegment deferred — ctx+0xb8 null", v124EarlyCount);
			}
			return 1;
		}
	}
	return FunctionCast(beginCoalescedSegment, callback->obeginCoalescedSegment)(that);
}

uint8_t Gen11::barrierSubmission(void *queue, void *accelerator, void *cmdDesc,
								 void *event, uint16_t count, const uint16_t *list) {
	if (!NGreen::callback->isRealTGL) {
		// V130: spoof-path guard for deterministic barrierSubmission+0x198 crash.
		// Boot-arg control (value or flags):
		//   ngreenV130=0 or -ngreenV130fail -> bypass and return 0 (default)
		//   ngreenV130=1 or -ngreenV130pass -> bypass and return 1
		//   ngreenV130=2 or -ngreenV130orig -> call original implementation
		//   ngreenV130=3 or -ngreenV130hybrid -> bypass during init, original during render
		// NOTE: on spoofed RPL this is unsafe unless explicitly forced because
		// the original path submits Blit2D/Blit3D barriers through BCS.
		// Use -ngreenV130forceorig only when intentionally validating that path.
		static int v130Mode = -1;
		if (v130Mode < 0) {
			int parsed = 0;
			const bool forceOrig = checkKernelArgument("-ngreenV130forceorig");
			if (PE_parse_boot_argn("ngreenV130", &parsed, sizeof(parsed))) {
				v130Mode = parsed;
			} else if (checkKernelArgument("-ngreenV130orig")) {
				v130Mode = 2;
			} else if (checkKernelArgument("-ngreenV130hybrid")) {
				v130Mode = 3;
			} else if (checkKernelArgument("-ngreenV130pass")) {
				v130Mode = 1;
			} else if (checkKernelArgument("-ngreenV130fail")) {
				v130Mode = 0;
			} else {
				// Default: bypass+ret1 so GPU scheduler treats barrier as succeeded.
				// ret0 causes a spin/wait loop in the scheduler → hard freeze.
				v130Mode = 1;
			}

			if (v130Mode == 2 && !forceOrig) {
				SYSLOG("ngreen", "V150: ngreenV130orig requested on spoofed RPL; forcing mode=1 to avoid BCS barrier stall (use -ngreenV130forceorig to override)");
				v130Mode = 1;
			}
			SYSLOG("ngreen", "V130: INIT barrierSubmission spoof mode=%d (0=bypass-ret0, 1=bypass-ret1, 2=call-original, 3=hybrid init-bypass/render-orig) forceOrig=%d",
				   v130Mode, forceOrig ? 1 : 0);
		}

		static int v130CallCount = 0;
		static int v130CallInitPhase = 0;
		static int v130CallRenderPhase = 0;
		static int v130HybridWarmup = -1;
		if (v130HybridWarmup < 0) {
			int parsedWarmup = 12;
			if (PE_parse_boot_argn("ngreenV130warmup", &parsedWarmup, sizeof(parsedWarmup))) {
				if (parsedWarmup < 0) parsedWarmup = 0;
				if (parsedWarmup > 200) parsedWarmup = 200;
			}
			v130HybridWarmup = parsedWarmup;
			if (v130Mode == 3) {
				SYSLOG("ngreen", "V130: hybrid warmup=%d calls (override with ngreenV130warmup=<0..200>)", v130HybridWarmup);
			}
		}
		
		v130CallCount++;
		// V151 DIAGNOSTIC: Enhanced logging to trace call patterns through init vs render phases
		// For hybrid mode, keep bypass window short to avoid WindowServer init starvation.
		bool isInitPhase = v130CallCount <= v130HybridWarmup;
		if (isInitPhase) v130CallInitPhase++;
		else v130CallRenderPhase++;
		
		if (v130CallCount <= 24 || (v130CallCount > 100 && v130CallCount <= 124) ||
			(v130CallCount > 1000 && v130CallCount % 100 == 0)) {
			SYSLOG("ngreen", "V130[%d|init=%d|render=%d]: phase=%s q=%p acc=%p cmd=%p evt=%p count=%u mode=%d",
				   v130CallCount, v130CallInitPhase, v130CallRenderPhase,
				   isInitPhase ? "INIT" : "RENDER", queue, accelerator, cmdDesc, event,
				   static_cast<unsigned>(count), v130Mode);
		}

		uint8_t retVal = 0;
		if (v130Mode == 3 && !isInitPhase) {
			if (v130CallCount <= 24 || (v130CallCount > 100 && v130CallCount <= 124)) {
				SYSLOG("ngreen", "V130[%d]: HYBRID render phase -> ORIGINAL path", v130CallCount);
			}
			retVal = FunctionCast(barrierSubmission, callback->obarrierSubmission)(queue, accelerator,
																				 cmdDesc, event,
																				 count, list);
			if (v130CallCount <= 24 || (v130CallCount > 100 && v130CallCount <= 124)) {
				SYSLOG("ngreen", "V130[%d]: HYBRID original returned %u", v130CallCount, static_cast<unsigned>(retVal));
			}
			return retVal;
		}

		// Log the return path for first few calls to capture mode decision
		if (v130Mode == 2) {
			if (v130CallCount <= 24) {
				SYSLOG("ngreen", "V130[%d]: RETURNING ORIGINAL path (mode=2)", v130CallCount);
			}
			retVal = FunctionCast(barrierSubmission, callback->obarrierSubmission)(queue, accelerator,
																				 cmdDesc, event,
																				 count, list);
			if (v130CallCount <= 24) {
				SYSLOG("ngreen", "V130[%d]: ORIGINAL returned %u", v130CallCount, static_cast<unsigned>(retVal));
			}
			return retVal;
		}

		retVal = static_cast<uint8_t>((v130Mode == 1 || v130Mode == 3) ? 1 : 0);
		if (v130CallCount <= 24) {
			SYSLOG("ngreen", "V130[%d]: RETURNING BYPASS path (mode=%d, ret=%u)", v130CallCount, v130Mode, static_cast<unsigned>(retVal));
		}
		return retVal;
	}
	return FunctionCast(barrierSubmission, callback->obarrierSubmission)(queue, accelerator,
																		 cmdDesc, event,
																		 count, list);
}

void * Gen11::getBlit2DContext(void *that,bool param_1)
{
	// V151 DIAGNOSTIC: Track call frequency to understand init vs render phase
	if (!NGreen::callback->isRealTGL) {
		static int v127CallCount = 0;
		v127CallCount++;
		bool isInitPhase = v127CallCount < 100;
		if (v127CallCount <= 20 || (v127CallCount > 100 && v127CallCount <= 120)) {
			SYSLOG("ngreen", "V127[%d]: getBlit2DContext called (phase=%s) task=%p force=%d",
				   v127CallCount, isInitPhase ? "INIT" : "RENDER", that, param_1 ? 1 : 0);
		}
	}
	
	// V127: On non-real-TGL (RPL spoof), call the original getter and enforce only
	// Blit2D-compatible fallbacks. submitBlit dereferences [ctx+0xb8] and then uses the
	// resulting object as Blit2D FIFO/ring state, so returning depth/color/3D contexts here
	// can corrupt renderer behavior (black output / wrong ring routing).
	if (!NGreen::callback->isRealTGL) {
		auto cachedValid2D = [&]() -> void * {
			void *cached = callback->v131CachedBlit2DCtx;
			if (cached && getMember<void *>(cached, 0xb8))
				return cached;
			return nullptr;
		};

		if (!callback->ogetBlit2DContext) {
			static bool v136Logged = false;
			if (!v136Logged) {
				v136Logged = true;
				SYSLOG("ngreen", "V136: ogetBlit2DContext is null");
			}
			void *fallback = cachedValid2D();
			if (fallback)
				return fallback;
			return nullptr;
		}
		void *ctx = FunctionCast(getBlit2DContext, callback->ogetBlit2DContext)(that, param_1);
		if (isExperimentalMonitorEnabled()) {
			static int v137Blit2DOrigCount = 0;
			if (v137Blit2DOrigCount < 24) {
				v137Blit2DOrigCount++;
				void *ctxField = ctx ? getMember<void *>(ctx, 0xb8) : nullptr;
				SYSLOG("ngreen", "V137.getBlit2DContext.orig[%d]: task=%p ctx=%p ctx+0xb8=%p", v137Blit2DOrigCount, that, ctx, ctxField);
			}
		}
		if (!ctx) {
			DBGLOG("ngreen", "V127: getBlit2DContext original returned null on RPL");
			void *fallback = cachedValid2D();
			if (fallback)
				return fallback;
			// No cross-context fallback allowed here (depth/color/3D are incompatible layouts).
			return nullptr;
		}
		void *ctxField = getMember<void *>(ctx, 0xb8);
		if (!ctxField) {
			DBGLOG("ngreen", "V127: getBlit2DContext ctx+0xb8 NULL on RPL, trying cached Blit2D fallback");
			void *fallback = cachedValid2D();
			if (fallback)
				return fallback;
			return nullptr;
		}
		DBGLOG("ngreen", "V127: getBlit2DContext ctx=%p ctx+0xb8=%p OK on RPL", ctx, ctxField);
		// V131: Cache valid context for future NULL fallback
		callback->v131CachedBlit2DCtx = ctx;
		return ctx;
	}
	if (!callback->ogetBlit2DContext)
		return nullptr;
	return FunctionCast(getBlit2DContext, callback->ogetBlit2DContext)(that,param_1);
}

void * Gen11::getDepthResolveContext(void *that,bool param_1)
{
	if (!callback->ogetDepthResolveContext) {
		static bool v136DepthLogged = false;
		if (!v136DepthLogged) {
			v136DepthLogged = true;
			SYSLOG("ngreen", "V136: ogetDepthResolveContext is null");
		}
		return nullptr;
	}
	void *ctx = FunctionCast(getDepthResolveContext, callback->ogetDepthResolveContext)(that,param_1);
	if (isExperimentalMonitorEnabled()) {
		static int v137DepthCount = 0;
		if (v137DepthCount < 32) {
			v137DepthCount++;
			void *ctxField = ctx ? getMember<void *>(ctx, 0xb8) : nullptr;
			SYSLOG("ngreen", "V137.depthResolve[%d]: task=%p force=%u ctx=%p ctx+0xb8=%p", v137DepthCount, that,
				   static_cast<unsigned>(param_1), ctx, ctxField);
		}
	}
	return ctx;
}

void * Gen11::getColorResolveContext(void *that,bool param_1)
{
	if (!callback->ogetColorResolveContext) {
		static bool v136ColorLogged = false;
		if (!v136ColorLogged) {
			v136ColorLogged = true;
			SYSLOG("ngreen", "V136: ogetColorResolveContext is null");
		}
		return nullptr;
	}
	void *ctx = FunctionCast(getColorResolveContext, callback->ogetColorResolveContext)(that,param_1);
	if (isExperimentalMonitorEnabled()) {
		static int v137ColorCount = 0;
		if (v137ColorCount < 32) {
			v137ColorCount++;
			void *ctxField = ctx ? getMember<void *>(ctx, 0xb8) : nullptr;
			SYSLOG("ngreen", "V137.colorResolve[%d]: task=%p force=%u ctx=%p ctx+0xb8=%p", v137ColorCount, that,
				   static_cast<unsigned>(param_1), ctx, ctxField);
		}
	}
	return ctx;
}

int Gen11::blit3d_supported()
{
	return 0;
}

void  Gen11::setAsyncSliceCount(void *that,uint32_t configRaw)
{
		uint32_t sliceCount     = (configRaw >> 0) & 0xFF;
		uint32_t subsliceCount  = (configRaw >> 8) & 0xFF;
		uint32_t euCount        = (configRaw >> 16) & 0xFF;

	uint32_t sliceField = 0;
		switch (sliceCount) {
			case 1: sliceField = 1; break;
			case 2: sliceField = 2; break;
			default:
				panic("IGPU: setAsyncSliceCount - Invalid slice count: %u\n", sliceCount);
				break;
		}
		
		uint32_t subsliceField = 0;
		switch (subsliceCount) {
			case 2: subsliceField = 0x20; break;
			case 4: subsliceField = 0x40; break;
			case 5: subsliceField = 0x50; break;
			case 6: subsliceField = 0x60; break;
			case 8: subsliceField = 0x80; break;
			default:
						panic("IGPU: setAsyncSliceCount - Invalid subsliceCount: %u\n", subsliceCount);
						break;
		}

		uint32_t euField = 0;
		switch (euCount) {
			case 1: euField = 0x100; break;
			case 2: euField = 0x200; break;
			case 3: euField = 0x300; break;
			case 4: euField = 0x400; break;
			case 5: euField = 0x500; break;
			case 6: euField = 0x600; break;
			case 8: euField = 0x800; break;
			default:
						panic("IGPU: setAsyncSliceCount - Invalid EU count/power mode: %u\n", euCount);
						break;
		}

		uint32_t hwRegisterValue = sliceField | subsliceField | euField;

		getMember<uint32_t>(that, 0x12a0) = configRaw;


		//SafeForceWake(that,true, 4);
		volatile uint32_t* mmioBase = getMember<volatile uint32_t*>(that, 0x1240);
		mmioBase[0xa204 / 4] = hwRegisterValue;
		//SafeForceWake(that,false, 4);
	
}

static const uint8_t DAT_000b0bb0[] = {
	0x00, 0x36, 0x6e, 0x01, 0x00, 0xf8, 0x24, 0x01,
	0x00, 0xf0, 0x49, 0x02, 0x40, 0x78, 0x7d, 0x01
};

bool Gen11::initHardwareCaps(void *this_ptr) {
		uint32_t gpuSku = getMember<uint32_t>(this_ptr, 0x1120);
		bool result = false;
		
		uint32_t uVar1;
		int iVar2;
		int iVar3;
		int iVar4;

		if (gpuSku == 2) {
			// --- SKU 2 (TGLLP) - Original TGL values for 6 Dual SubSlices (12 SubSlices) ---
					
					// Buffer sizes for 12 SubSlices (6 DSS × 2 SS/DSS)
					// 0xc0 (192) = 16 bytes × 12 SS
					getMember<uint64_t>(this_ptr, 0x112c) = 0x222000000c0ULL;
					
					// 0x150 (336) = 28 bytes × 12 SS
					getMember<uint64_t>(this_ptr, 0x1134) = 0x22200000150ULL;
					getMember<uint32_t>(this_ptr, 0x113c) = 0x150;
					
					getMember<uint64_t>(this_ptr, 0x1174) = 0x200000007ULL;
					getMember<uint64_t>(this_ptr, 0x117c) = 0x1000000080ULL;
					
					getMember<uint32_t>(this_ptr, 0x1160) = 0xf00;
					
					// Max Dual SubSlices = 6 (matches hardware: 6 DSS)
					getMember<uint32_t>(this_ptr, 0x1148) = 0x6;
					
					// Calculate actual Dual SubSlices (SubSlices / 2)
					uVar1 = getMember<uint32_t>(this_ptr, 0x1158) >> 1;
					iVar3 = 2;
					
					// Reference Dual SubSlices count = 6 (must match max to avoid underflow)
					iVar4 = 0x6;
					iVar2 = 0x80;
		}
		else {
			if (gpuSku != 1) {
				result = false;
				return result;
			}
			
			// --- SKU 1 (TGLHP) - Modified for 5 DSS ---
			
			// Sizes for 10 SubSlices
			getMember<uint64_t>(this_ptr, 0x112c) = 0x2d800000140ULL;
			getMember<uint64_t>(this_ptr, 0x1134) = 0x27000000168ULL;
			getMember<uint32_t>(this_ptr, 0x113c) = 0x168;
			
			getMember<uint64_t>(this_ptr, 0x1174) = 0x100000007ULL;
			getMember<uint64_t>(this_ptr, 0x117c) = 0x1000000040ULL;
			
			getMember<uint32_t>(this_ptr, 0x1160) = 0x800;
			
			// Max SubSlices set to 10
			getMember<uint32_t>(this_ptr, 0x1148) = 0xA;
			
			uVar1 = getMember<uint32_t>(this_ptr, 0x1158);
			iVar3 = 1;
			
			// Reference count set to 10
			iVar4 = 0xA;
			iVar2 = 0x40;
		}

		// Final calculations
		getMember<uint32_t>(this_ptr, 0x114c) = uVar1;
		
		uint8_t &byteRef = getMember<uint8_t>(this_ptr, 0x1184);
		byteRef = byteRef & 0xFB;
		
		getMember<uint32_t>(this_ptr, 0x1128) = iVar3 * uVar1;
		getMember<uint32_t>(this_ptr, 0x1144) = (iVar4 - uVar1) * iVar3;
		getMember<uint32_t>(this_ptr, 0x1140) = iVar2 * uVar1;
		
		getMember<uint32_t>(this_ptr, 0x1168) = getMember<uint32_t>(this_ptr, 0x115c) << 4;
		
		result = true;
		return result;
}

void Gen11::checkWOPCMSettings(void *that,unsigned long param_1,void *param_2)
{
	const uint32_t GUC_WOPCM_OFFSET = 1 * 1024 * 1024;
	const uint32_t GUC_WOPCM_SIZE = 1 * 1024 * 1024;
	
	typedef struct {
		uint64_t address;
		uint64_t length;
	} IOVirtualRange;
	
	IOVirtualRange *wopcm_range = (IOVirtualRange *)param_2;
		
	wopcm_range->address = GUC_WOPCM_OFFSET;
	wopcm_range->length = GUC_WOPCM_SIZE;
	
}

IOReturn Gen11::wrapFBClientDoAttribute(void *fbclient, uint32_t attribute, unsigned long *unk1, unsigned long unk2, unsigned long *unk3, unsigned long *unk4,  void *externalMethodArguments) {
	if (attribute == 0x923) {
		return kIOReturnUnsupported;
	}
	return FunctionCast(wrapFBClientDoAttribute, callback->orgFBClientDoAttribute)(fbclient, attribute, unk1, unk2, unk3, unk4,  externalMethodArguments);
}

unsigned long Gen11::loadGuCBinary(void *that) {
	// V52: Real TGL can authenticate and load GuC firmware natively.
	// RPL cannot — stub to return 1 and use host scheduling instead.
	if (NGreen::callback->isRealTGL) {
		SYSLOG("ngreen", "loadGuCBinary: real TGL — calling original for GuC firmware load");
		return FunctionCast(loadGuCBinary, callback->oloadGuCBinary)(that);
	}
	SYSLOG("ngreen", "loadGuCBinary: RPL — stubbed to return 1 (host scheduling)");
	return 1;
}

UInt8 Gen11::wrapLoadGuCBinary(void *that) {

	if (callback->firmwareSizePointer)
		callback->performingFirmwareLoad = true;

	auto r = FunctionCast(wrapLoadGuCBinary, callback->orgLoadGuCBinary)(that);
	DBGLOG("ngreen", "loadGuCBinary returned %d", r);

	callback->performingFirmwareLoad = false;

	return r;
}

bool Gen11::wrapLoadFirmware(void *that) {

	//(*reinterpret_cast<uintptr_t **>(that))[35] = reinterpret_cast<uintptr_t>(wrapSystemWillSleep);
	//(*reinterpret_cast<uintptr_t **>(that))[36] = reinterpret_cast<uintptr_t>(wrapSystemDidWake);
	return FunctionCast(wrapLoadFirmware, callback->orgLoadFirmware)(that);
}

void Gen11::wrapSystemWillSleep(void *that) {
	DBGLOG("ngreen", "systemWillSleep GuC callback");
}

void Gen11::wrapSystemDidWake(void *that) {
	DBGLOG("ngreen", "systemDidWake GuC callback");

	// This is IGHardwareGuC class instance.
	auto &GuC = (reinterpret_cast<OSObject **>(that))[76];

	if (GuC)
	if (GuC->metaCast("IGHardwareGuC")) {
		DBGLOG("igfx", "reloading firmware on wake; discovered IGHardwareGuC - releasing");
		GuC->release();
		GuC = nullptr;
	}

	FunctionCast(wrapLoadFirmware, callback->orgLoadFirmware)(that);
}

bool Gen11::wrapInitSchedControl(void *that) {
	DBGLOG("ngreen", "attempting to init sched control with load %d", callback->performingFirmwareLoad);
	bool perfLoad = callback->performingFirmwareLoad;
	callback->performingFirmwareLoad = false;
	bool r = FunctionCast(wrapInitSchedControl, callback->orgInitSchedControl)(that);

	callback->performingFirmwareLoad = perfLoad;
	return r;
}

void *Gen11::wrapIgBufferWithOptions(void *accelTask, void* size, unsigned int type, unsigned int flags) {
	void *r = nullptr;

	if (callback->performingFirmwareLoad) {
		callback->dummyFirmwareBuffer = Buffer::create<uint8_t>(*(unsigned long*)size);

		const void *fw = nullptr;
		const void *fwsig = nullptr;
		size_t fwsize = 0;
		size_t fwsigsize = 0;


		/*fw = GuCFirmwareKBL;
		fwsig = GuCFirmwareKBLSignature;
		fwsize = GuCFirmwareKBLSize;
		fwsigsize = GuCFirmwareSignatureSize;*/

		unsigned long newsize = fwsize > *(unsigned long*)size ? ((fwsize + 0xFFFF) & (~0xFFFF)) : *(unsigned long*)size;
		r = FunctionCast(wrapIgBufferWithOptions, callback->orgIgBufferWithOptions)(accelTask, (void*)newsize,type,flags);
		if (r && callback->dummyFirmwareBuffer) {
			auto status = MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
			if (status == KERN_SUCCESS) {
				callback->realFirmwareBuffer = static_cast<uint8_t **>(r)[7];
				static_cast<uint8_t **>(r)[7] = callback->dummyFirmwareBuffer;
				lilu_os_memcpy(callback->realFirmwareBuffer, fw, fwsize);
				lilu_os_memcpy(callback->signaturePointer, fwsig, fwsigsize);
				callback->realBinarySize = static_cast<uint32_t>(fwsize);
				*callback->firmwareSizePointer = static_cast<uint32_t>(fwsize);
				MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
			} else {
				//SYSLOG("igfx", "ig buffer protection upgrade failure %d", status);
			}
		} else if (callback->dummyFirmwareBuffer) {
			//SYSLOG("igfx", "ig shared buffer allocation failure");
			Buffer::deleter(callback->dummyFirmwareBuffer);
			callback->dummyFirmwareBuffer = nullptr;
		} else {
			//SYSLOG("igfx", "dummy buffer allocation failure");
		}
	} else {
		r = FunctionCast(wrapIgBufferWithOptions, callback->orgIgBufferWithOptions)(accelTask, size,type,flags);
	}

	return r;
}

UInt64 Gen11::wrapIgBufferGetGpuVirtualAddress(void *that) {
	if (callback->performingFirmwareLoad && callback->realFirmwareBuffer) {
		static_cast<uint8_t **>(that)[7] = callback->realFirmwareBuffer;
		callback->realFirmwareBuffer = nullptr;
		Buffer::deleter(callback->dummyFirmwareBuffer);
		callback->dummyFirmwareBuffer = nullptr;
	}

	return FunctionCast(wrapIgBufferGetGpuVirtualAddress, callback->orgIgBufferGetGpuVirtualAddress)(that);
}


uint32_t Gen11::wrapReadRegister32(void *controller, uint32_t address) {
	if (controller == nullptr)
		return NGreen::callback->readReg32(address);  // readReg32 now takes byte offsets

	// Mirror Apple bounds logic, but keep a fallback path for the 2D-only dropped window.
	auto partInfo = getMember<uint8_t *>(controller, 0xCF8);
	auto mmioBase = getMember<uint8_t *>(controller, 0x9B8);
	auto mmioSize = static_cast<uint32_t>(getMember<int>(controller, 0xC38));

	bool twoDOnlyPart = partInfo && ((partInfo[0xB2] & 0x1) != 0);
	bool dropped2DWindow = (address >= 0x2000U && address <= 0x23FFFFU);

	if (twoDOnlyPart && dropped2DWindow) {
		return NGreen::callback->readReg32(address);  // readReg32 now takes byte offsets
	}

	if (mmioBase && mmioSize >= 4U && address < (mmioSize - 4U)) {
		return *reinterpret_cast<volatile uint32_t *>(mmioBase + address);
	}

	return FunctionCast(wrapReadRegister32, callback->owrapReadRegister32)(controller, address);
}

void Gen11::wrapWriteRegister32(void *controller, uint32_t address, uint32_t value) {
	if (xeInertDrop(address)) return;
	if ((address & 0xFFFFF) == 0xC8254 || (address & 0xFFFFF) == 0xC8258) value = xeFixFilterWrite(address, value);  // 20/10: só o brilho (XE-BLR) neste caminho
	if (controller == nullptr) {
		NGreen::callback->writeReg32(address, value);
		return;
	}

	// V195W removed (was: OR-in bits 14,12 from UEFI's HSW_PWR_WELL_CTL1 onto every
	// 0x45400 write because the ICL DMC save/restore table allegedly clears them
	// periodically, supposedly stalling WindowServer's vsync). Pair-mate to V195 in
	// raWriteRegister32 + V195F in FastWriteRegister32 — all three sites stripped
	// together. The DMC save/restore behavior on Display 13 (ADL-P) differs from
	// Display 11 (ICL); enforcing UEFI's specific bits hides what Apple's stack
	// actually wants and what the ADL-P DMC table actually contains. If vsync stalls
	// re-emerge, fix should come from a correct DMC table or proper power-well init,
	// not bit OR-in.
	if (address == 0x45400 && NGreen::callback && !NGreen::callback->isRealTGL
	    && NGreen::callback->uefiCtl1 != 0) {
		static int v195WpCount = 0;
		if (v195WpCount < 6) {
			++v195WpCount;
			SYSLOG("ngreen", "V195Wp[%d]: CTL1 0x%x passthrough uefiCtl1=0x%x (V195W hack removed)",
			       v195WpCount, value, NGreen::callback->uefiCtl1);
		}
	}

	// V72W removed (was: force RCS/BCS RING_EMR writes to 0xFFFFFFFF — same blanket
	// error-mask hack as V72R, on the wrapWriteRegister32 helper path). Pair-mate to
	// the V72R passthrough already in place. Keeping the address-match shell so a
	// future legitimate intercept can use this hook point.
	if (address == 0x20b4 || address == 0x220b4) {
		static int v72WPassCount = 0;
		if (v72WPassCount < 6) {
			++v72WPassCount;
			SYSLOG("ngreen", "V72Wp[%d]: EMR @ 0x%x val=0x%x passthrough (V72W hack removed)",
			       v72WPassCount, address, value);
		}
	}

	// ── V63: Broad RCS register write intercept (diagnostic only, no modification) ──
	// Catches ANY write to RCS control range: ELSP, EXECLIST, CTX_CTRL, CCID, TAIL, etc.
	// Rate-limited to first 100 writes to avoid flooding Lilu buffer.
	if (!NGreen::callback->isRealTGL) {
		static int v63WriteCount = 0;
		// RCS engine MMIO range: 0x2000-0x2FFF covers all ring control registers
		if (address >= 0x2000 && address <= 0x2FFF) {
			if (v63WriteCount < 100) {
				v63WriteCount++;
				SYSLOG("ngreen", "V63W[%d]: RCS reg 0x%x = 0x%x", v63WriteCount, address, value);
			} else if (v63WriteCount == 100) {
				v63WriteCount++;
				SYSLOG("ngreen", "V63W: rate limit reached (100 RCS writes logged)");
			}
		}
	}

	FunctionCast(wrapWriteRegister32, callback->owrapWriteRegister32)(controller,address,value);
}

// V45: Delayed child check callback — runs on a kernel thread after a configurable delay.
// Logs the IOService state and child count at the specified time after start().
static void v45DelayedChildCheck(thread_call_param_t p0, thread_call_param_t p1) {
	if (!isV60MonitorEnabled())
		return;

	auto *svc = static_cast<IOService *>(p0);
	unsigned delayMs = (unsigned)(uintptr_t)p1;
	
	uint64_t state = svc->getState();
	OSIterator *iter = svc->getClientIterator();
	int count = 0;
	if (iter) {
		OSObject *obj;
		while ((obj = iter->getNextObject())) {
			auto *child = OSDynamicCast(IOService, obj);
			if (child) {
				uint64_t childState = child->getState();
				SYSLOG("ngreen", "V45: T+%ums child[%d]: %s class=%s state=0x%llx",
					   delayMs, count, child->getName(),
					   child->getMetaClass()->getClassName(),
					   (unsigned long long)childState);
				
				// V55: Enhanced IGAccelDevice diagnostics + force-start
				if (delayMs >= 3000) {
					const char *childName = child->getName();
					if (childName && (strcmp(childName, "IGAccelDevice") == 0 ||
									  strcmp(childName, "IGAccelSharedUserClient") == 0 ||
									  strcmp(childName, "IOAccelDisplayPipeUserClient2") == 0 ||
									  strcmp(childName, "IGAccelCommandQueue") == 0)) {
						// Dump child's provider and property state
						auto *childProvider = child->getProvider();
						SYSLOG("ngreen", "V55: %s state=0x%llx provider=%s isOpen=%d",
							   childName, (unsigned long long)childState,
							   childProvider ? childProvider->getName() : "NULL",
							   child->isOpen());
						
						// Check if the accelerator (our parent) is open
						SYSLOG("ngreen", "V55: accelerator isOpen=%d", svc->isOpen());
						
						if (childState == 0) {
							if (strcmp(childName, "IOAccelDisplayPipeUserClient2") == 0) {
								// Never call registerService on IOAccelDisplayPipeUserClient2.
								// Starting it triggers stamp-9 timeouts every ~5.4s:
								// AccessComplete is stubbed → stamp never advances →
								// GPURestartSignaled → WS compositor interrupted per cycle.
								// This applies in all modes, not just dp0.
								SYSLOG("ngreen", "V55: %s at state=0x0 — skip registerService (prevent stamp-9 timeout loop)", childName);
							} else {
								SYSLOG("ngreen", "V55: %s at state=0x0 — calling registerService()", childName);
								child->registerService(kIOServiceAsynchronous);
								IODelay(500);
								uint64_t newState = child->getState();
								SYSLOG("ngreen", "V55: %s after registerService → state=0x%llx",
									   childName, (unsigned long long)newState);
							}
						}
					}
				}
				count++;
			}
		}
		iter->release();
	}
	
	// Also check if the service is open (someone called IOServiceOpen on it)
	bool isOpen = svc->isOpen();
	
	SYSLOG("ngreen", "V45: T+%ums: %d children, state=0x%llx (reg=%d match=%d pub=%d fmatch=%d inact=%d), isOpen=%d",
		   delayMs, count,
		   (unsigned long long)state,
		   !!(state & 0x02), !!(state & 0x04), !!(state & 0x08), !!(state & 0x10), !!(state & 0x01),
		   isOpen);
	
	// Check provider's children too (siblings in IOAccelerator match category)
	auto *provider = svc->getProvider();
	if (provider) {
		OSIterator *sibs = provider->getClientIterator();
		if (sibs) {
			int sibIdx = 0;
			OSObject *s;
			while ((s = sibs->getNextObject())) {
				auto *sibSvc = OSDynamicCast(IOService, s);
				if (sibSvc) {
					SYSLOG("ngreen", "V45: T+%ums provider child[%d]: %s class=%s",
						   delayMs, sibIdx, sibSvc->getName(),
						   sibSvc->getMetaClass()->getClassName());
					sibIdx++;
				}
			}
			sibs->release();
		}
	}
}

// V45: Helper to schedule a delayed child check
static void v45ScheduleDelayedCheck(void *accelInstance, unsigned delayMs) {
	if (!isV60MonitorEnabled())
		return;

	thread_call_t tc = thread_call_allocate(v45DelayedChildCheck,
											static_cast<thread_call_param_t>(accelInstance));
	if (tc) {
		uint64_t deadline;
		clock_interval_to_deadline(delayMs, kMillisecondScale, &deadline);
		thread_call_enter1_delayed(tc,
								   (thread_call_param_t)(uintptr_t)delayMs,
								   deadline);
	}
}

// V60: GPU health monitor — 2s interval with V57-proven R/W ERROR_GEN6 clear.
// V59 ReadRegister32 intercept caused regression (0 children). Back to timer-based.
// Also applies EMR mask-all every cycle to prevent Apple from unmasking errors.
void Gen11::v60GpuHealthMonitor(thread_call_param_t param0, thread_call_param_t param1) {
	if (!isV60MonitorEnabled())
		return;

	static int v60Count = 0;
	static uint32_t v60LastHead = 0xDEAD;
	static uint32_t v60LastTail = 0xDEAD;
	v60Count++;
	
	auto *svc = static_cast<IOService *>(param0);
	if (!svc) {
		SYSLOG("ngreen", "v60[%d]: null IOService, stopping health monitor", v60Count);
		return;
	}

	const bool v60Aggressive = isV60AggressiveMonitorEnabled();

	// Guard: bail if MMIO mapping has been released (GPU torn down before timer fired).
	// Release the retain we hold — stop the timer loop.
	if (!NGreen::callback->mmioValid()) {
		SYSLOG("ngreen", "v60[%d]: MMIO not ready, stopping health monitor", v60Count);
		svc->release();
		return;
	}

	// 1. Read GPU state via direct MMIO
	uint32_t realErr = NGreen::callback->readReg32(ERROR_GEN6);
	uint32_t rcsHead = NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE));
	uint32_t rcsTail = NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE));
	uint32_t rcsCtl  = NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE));
	uint32_t emr     = NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE));
	
	// 2. ExecList state
	uint32_t execStatus = NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE));
	uint32_t csbPtr = NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
	
	// V63: Additional scheduler/context diagnostics
	uint32_t acthd    = NGreen::callback->readReg32(RING_ACTHD(RENDER_RING_BASE));
	uint32_t ccid     = NGreen::callback->readReg32(RING_CCID(RENDER_RING_BASE));
	uint32_t ringFault = NGreen::callback->readReg32(GEN12_RING_FAULT_REG);
	
	// 3. Track ring HEAD/TAIL movement
	bool headChanged = (rcsHead != v60LastHead);
	bool tailChanged = (rcsTail != v60LastTail);
	
	// 4. Count children + find IGAccelDevice state
	int childCount = 0;
	uint64_t accelDevState = 0xDEAD;
	/*if (v60Aggressive) {
		OSIterator *iter = svc->getClientIterator();
		if (iter) {
			OSObject *obj;
			while ((obj = iter->getNextObject())) {
				auto *child = OSDynamicCast(IOService, obj);
				if (child) {
					const char *cn = child->getName();
					if (cn && strcmp(cn, "IGAccelDevice") == 0) {
						accelDevState = child->getState();
					}
					childCount++;
				}
			}
			iter->release();
		}
	}*/
	
	// V70: Track child transitions — dump all children when count or accelDev state changes
	/*if (v60Aggressive) {
		static int lastChildCount = -1;
		static uint64_t lastAccelDevState = 0xBEEF;
		if (childCount != lastChildCount || accelDevState != lastAccelDevState) {
			SYSLOG("ngreen", "V70: TRANSITION ch %d->%d dev 0x%llx->0x%llx [iter %d]",
				   lastChildCount, childCount,
				   (unsigned long long)lastAccelDevState, (unsigned long long)accelDevState,
				   v60Count);
			// Dump ALL children at the transition point
			OSIterator *iter2 = svc->getClientIterator();
			if (iter2) {
				int idx = 0;
				OSObject *obj2;
				while ((obj2 = iter2->getNextObject())) {
					auto *child2 = OSDynamicCast(IOService, obj2);
					if (child2) {
						SYSLOG("ngreen", "V70: ch[%d]: %s cls=%s st=0x%llx",
							   idx, child2->getName(),
							   child2->getMetaClass()->getClassName(),
							   (unsigned long long)child2->getState());
						idx++;
					}
				}
				iter2->release();
			}
			// If IGAccelDevice just disappeared, dump GPU error state at that moment
			if (accelDevState == 0xDEAD && lastAccelDevState != 0xDEAD && lastAccelDevState != 0xBEEF) {
				SYSLOG("ngreen", "V70: *** IGAccelDevice LOST! Was dev=0x%llx ***",
					   (unsigned long long)lastAccelDevState);
				uint32_t errAtLoss = NGreen::callback->readReg32(ERROR_GEN6);
				uint32_t tlb0Loss  = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0);
				uint32_t tlb1Loss  = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1);
				uint32_t faultLoss = NGreen::callback->readReg32(GEN12_RING_FAULT_REG);
				SYSLOG("ngreen", "V70: at loss: ERR=0x%x TLB0=0x%x TLB1=0x%x FAULT=0x%x",
					   errAtLoss, tlb0Loss, tlb1Loss, faultLoss);
				// Dump GGTT PTEs 0-3 to check general GGTT health at this moment
				for (int pg = 0; pg < 4; pg++) {
					uint32_t pteLo = NGreen::callback->readReg32(GGTT_PTE_LO(pg));
					uint32_t pteHi = NGreen::callback->readReg32(GGTT_PTE_HI(pg));
					SYSLOG("ngreen", "V70: loss GGTT[%d]=0x%08x:%08x %s",
						   pg, pteHi, pteLo, (pteLo & 1) ? "V" : "INV");
				}
			}
			lastChildCount = childCount;
			lastAccelDevState = accelDevState;
		}
	}*/
	
	// V78/V77: DisplayPipe terminator — DISABLED.
	// V115+V116 now prevent the Blit3D MCE that caused GPU rendering failure.
	// Killing IOAccelDisplayPipeUserClient2 every 2s blocks WindowServer's main
	// thread mid-IOKit-call → userspace watchdog fires at T+40s and T+80s.
	// Re-enable only if GPU rendering is still broken after V115/V116 testing.
	/*
	{
		const int v77KillDelay = getV77KillDelayIterations();
		if (v77KillDelay > 0 && v60Count <= v77KillDelay) {
			SYSLOG("ngreen", "V77: kill delayed at iter %d (ngreenV77DelayKill=%d)",
			       v60Count, v77KillDelay);
		} else {
		OSIterator *dpIter = svc->getClientIterator();
		if (dpIter) {
			OSObject *dpObj;
			int dpKilled = 0;
			while ((dpObj = dpIter->getNextObject())) {
				auto *dpChild = OSDynamicCast(IOService, dpObj);
				if (dpChild) {
					const char *dpCn = dpChild->getMetaClass()->getClassName();
					if (dpCn && strstr(dpCn, "DisplayPipe")) {
						SYSLOG("ngreen", "V77: TERMINATING %s (st=0x%llx) to prevent WS GPU compositing crash",
							   dpCn, (unsigned long long)dpChild->getState());
						dpChild->terminate(kIOServiceRequired);
						dpKilled++;
					}
				}
			}
			dpIter->release();
			if (dpKilled > 0) {
				SYSLOG("ngreen", "V77: Killed %d display pipe client(s) at iter %d", dpKilled, v60Count);
			}
		}
		}
	}
	*/

	// 5. V60: error suppression path. Default is read-only for stability.
	// Use -ngreenv60rw to enable active register writes during monitor ticks.
	/*if (v60Aggressive) {
		if (realErr) {
			NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
		}
		// Re-mask EMR every cycle — Apple may unmask during error recovery attempts
		if (emr != 0xFFFFFFFF) {
			NGreen::callback->writeReg32(RING_EMR(RENDER_RING_BASE), 0xFFFFFFFF);
		}
		
		// ── V79: Plane linearization (brief Apple logo flash) — aggressive mode only ──
		// Boot log shows SURF=0x0 for iters 1–6; SURF becomes non-zero (tiled) at iter 7 (~14s).
		// Old cadence (<=5, 10, 20, 30) skipped iter 7/8/9 — the first 6s window where the plane
		// is tiled. Writing at iter 10 (~20s) is too late: the login screen is already up and the
		// Apple logo phase has ended. Fix: fire every tick from 1–12 so we catch iter 7 immediately
		// (first time SURF is mapped and tiling != 0) while the Apple logo may still be on screen.
		if (v60Count <= 5 || v60Count == 10 || v60Count == 20 || v60Count == 30) {
			uint32_t planCtl  = NGreen::callback->readReg32(0x70180); // PLANE_CTL
			uint32_t planStrd = NGreen::callback->readReg32(0x70188); // PLANE_STRIDE
			uint32_t tiling = (planCtl >> 10) & 0x7; // bits[12:10]
			if (tiling != 0) {
				uint32_t newCtl = planCtl & ~(0x7u << 10); // clear tiling -> linear
				uint32_t newStrd = planStrd;
				if (tiling == 1) {
					newStrd = planStrd * 8;
				} else if (tiling == 4) {
					newStrd = planStrd * 16;
				}
				NGreen::callback->writeReg32(0x70180, newCtl);
				NGreen::callback->writeReg32(0x70188, newStrd);
				uint32_t planSurf = NGreen::callback->readReg32(0x7019C);
				NGreen::callback->writeReg32(0x7019C, planSurf);
				SYSLOG("ngreen", "V79[%d]: tiling %d->linear CTL 0x%x->0x%x STRIDE 0x%x->0x%x SURF=0x%x",
					   v60Count, tiling, planCtl, newCtl, planStrd, newStrd, planSurf);
			} else {
				SYSLOG("ngreen", "V79[%d]: already linear CTL=0x%x STRIDE=0x%x", v60Count, planCtl, planStrd);
			}
		}
	}*/

	// 5. V60: ACTIVE error suppression (V57 proven approach)
	//    Keep this write path strictly opt-in via -ngreenv60rw.
	/*if (v60Aggressive) {
		if (realErr) {
			NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
		}
		// Re-mask EMR every cycle — Apple may unmask during error recovery attempts
		if (emr != 0xFFFFFFFF) {
			NGreen::callback->writeReg32(RING_EMR(RENDER_RING_BASE), 0xFFFFFFFF);
		}
	}*/
	
	// 6. Log with ring + CSB focus
	uint32_t postErr = NGreen::callback->readReg32(ERROR_GEN6);
	uint32_t gtDw0   = NGreen::callback->readReg32(GEN11_GT_INTR_DW0);
	uint32_t rcIntr  = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
	SYSLOG("ngreen", "V60M[%d]: ERR=0x%x->0x%x HEAD=0x%x TAIL=0x%x CTL=0x%x EMR=0x%x EXEC=0x%x CSB=0x%x ch=%d dev=0x%llx%s%s",
		   v60Count, realErr, postErr, rcsHead, rcsTail, rcsCtl, emr,
		   execStatus, csbPtr, childCount, (unsigned long long)accelDevState,
		   headChanged ? " HEAD_MOVED!" : "",
		   tailChanged ? " TAIL_MOVED!" : "");
	SYSLOG("ngreen", "V66M[%d]: ACTHD=0x%x CCID=0x%x FAULT=0x%x GT_DW0=0x%x RC_EN=0x%x",
		   v60Count, acthd, ccid, ringFault, gtDw0, rcIntr);
	// V68: Log TLB fault data when ERROR_GEN6 fires — shows faulting address
	if (realErr) {
		uint32_t tlb0 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0);
		uint32_t tlb1 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1);
		SYSLOG("ngreen", "V68F[%d]: ERR=0x%x TLB0=0x%x TLB1=0x%x", v60Count, realErr, tlb0, tlb1);
	}
	
	// ── V64: CSB buffer dump + interrupt mask diagnostics + CSB read pointer advance ──
	if (v60Count == 1) {
		// Dump all 6 CSB entries (context status buffer) — GPU writes these during context switches
		for (int i = 0; i < 6; i++) {
			uint32_t csbHi = NGreen::callback->readReg32(RING_CONTEXT_STATUS_BUF_HI(RENDER_RING_BASE, i));
			uint32_t csbLo = NGreen::callback->readReg32(RING_CONTEXT_STATUS_BUF(RENDER_RING_BASE, i));
			SYSLOG("ngreen", "V64: CSB[%d] = 0x%x:0x%x", i, csbHi, csbLo);
		}
		// Dump interrupt enable + mask registers for RCS
		uint32_t rcIntrEn   = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t rcsIntrMask = NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK);
		uint32_t gtIntrDw0  = NGreen::callback->readReg32(GEN11_GT_INTR_DW0);
		uint32_t mstrIrq    = NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ);
		SYSLOG("ngreen", "V64: RC_INTR_EN=0x%x RCS_INTR_MASK=0x%x GT_DW0=0x%x MSTR=0x%x",
			   rcIntrEn, rcsIntrMask, gtIntrDw0, mstrIrq);
		// Dump identity registers
		uint32_t ident0 = NGreen::callback->readReg32(GEN11_INTR_IDENTITY_REG0);
		uint32_t ident1 = NGreen::callback->readReg32(GEN11_INTR_IDENTITY_REG1);
		SYSLOG("ngreen", "V64: INTR_IDENT0=0x%x INTR_IDENT1=0x%x", ident0, ident1);
		
		// ── V67: Enhanced scheduler + workloop + event source diagnostics ──
		// ── V68: + fault TLB data, e08obj class, early sched clear ──
		{
			// V68: Read fault detail registers (TLB data gives faulting address)
			uint32_t tlbData0 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0);
			uint32_t tlbData1 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1);
			uint32_t gucStatus = NGreen::callback->readReg32(0xC000); // GUC_STATUS
			uint32_t rcsEir = NGreen::callback->readReg32(RENDER_RING_BASE + 0xB0); // RING_EIR
			SYSLOG("ngreen", "V68: TLB_DATA0=0x%x TLB_DATA1=0x%x GUC=0x%x EIR=0x%x",
				tlbData0, tlbData1, gucStatus, rcsEir);

			uint64_t schedPtr = getMember<uint64_t>(svc, 0xe00);
			if (schedPtr) {
				SYSLOG("ngreen", "V67: scheduler ptr=0x%llx", (unsigned long long)schedPtr);
				auto *schedBase = reinterpret_cast<uint8_t *>(schedPtr);
				// V67: Expanded dump — 0x000-0x400 (128 QWORDs)
				for (int i = 0; i < 128; i++) {
					uint64_t val = *reinterpret_cast<uint64_t *>(schedBase + i * 8);
					if (val != 0) {  // Only log non-zero to reduce noise
						SYSLOG("ngreen", "V67: sched[0x%03x]=0x%016llx", i * 8, (unsigned long long)val);
					}
				}
				
				// V68: Clear sched error EARLY (don't wait for iteration 5).
				// Clearing an error flag is safe regardless of V60 mode — the scheduler
				// error 0x7b blocks all GPU submissions and must be cleared on boot.
				uint32_t schedErr = *reinterpret_cast<uint32_t *>(schedBase + 0x0C);
				if (schedErr) {
					*reinterpret_cast<uint32_t *>(schedBase + 0x0C) = 0;
					*reinterpret_cast<uint32_t *>(schedBase + 0x00) = 0;
					SYSLOG("ngreen", "V68: early sched clear err=0x%x->0x%x flag=0x0",
						schedErr, *reinterpret_cast<uint32_t *>(schedBase + 0x0C));
				}
			} else {
				SYSLOG("ngreen", "V67: scheduler ptr is NULL!");
			}
			
			// V67: Dump object at accel[0xe08] — may be the actual scheduler C++ object
			uint64_t schedObj = getMember<uint64_t>(svc, 0xe08);
			if (schedObj) {
				auto *objBase = reinterpret_cast<uint8_t *>(schedObj);
				// First 8 bytes should be vtable pointer for a C++ object
				uint64_t vtable = *reinterpret_cast<uint64_t *>(objBase);
				// V68: Identify class via getMetaClass()
				auto *e08Obj = reinterpret_cast<OSObject *>(schedObj);
				const char *e08Class = "?";
				if (e08Obj->getMetaClass()) {
					e08Class = e08Obj->getMetaClass()->getClassName();
				}
				SYSLOG("ngreen", "V68: accel[0xe08] obj=0x%llx vtable=0x%llx class=%s",
					(unsigned long long)schedObj, (unsigned long long)vtable, e08Class);
				// Dump first 256 bytes, non-zero only
				for (int i = 0; i < 32; i++) {
					uint64_t val = *reinterpret_cast<uint64_t *>(objBase + i * 8);
					if (val != 0) {
						SYSLOG("ngreen", "V68: e08obj[0x%03x]=0x%016llx", i * 8, (unsigned long long)val);
					}
				}
			}
			
			// V67: Dump objects at accel[0xe10..0xe38] — identify by vtable class
			for (int idx = 0; idx < 6; idx++) {
				uint64_t objPtr = getMember<uint64_t>(svc, 0xe10 + idx * 8);
				if (objPtr) {
					auto *obj = reinterpret_cast<OSObject *>(objPtr);
					const char *clsName = obj->getMetaClass() ? obj->getMetaClass()->getClassName() : "?";
					SYSLOG("ngreen", "V67: accel[0x%03x]=0x%llx class=%s",
						0xe10 + idx * 8, (unsigned long long)objPtr, clsName);
				}
			}
			
			// V67: Workloop event source chain enumeration
			auto *wl = svc->getWorkLoop();
			SYSLOG("ngreen", "V67: workLoop=%s", wl ? "EXISTS" : "NULL");
			if (wl) {
				SYSLOG("ngreen", "V67: workLoop class=%s ptr=0x%llx",
					wl->getMetaClass()->getClassName(), (unsigned long long)wl);
				// Dump workloop object — first 128 bytes to find event source chain
				auto *wlBase = reinterpret_cast<uint8_t *>(wl);
				for (int i = 0; i < 16; i++) {
					uint64_t val = *reinterpret_cast<uint64_t *>(wlBase + i * 8);
					SYSLOG("ngreen", "V67: wl[0x%02x]=0x%016llx", i * 8, (unsigned long long)val);
				}
				
				// V67: Walk event source chain from workloop dump.
				// IOWorkLoop layout: [0x00]=vtable [0x08-0x10]=OSObject fields
				// [0x18]=gateLock [0x20]=eventChain [0x28]=controlG [0x30]=workToDo etc.
				// Read eventChain pointer (offset ~0x20 or thereabouts in the dump above)
				// and follow the chain manually.
				uint64_t eventChain = *reinterpret_cast<uint64_t *>(wlBase + 0x20);
				SYSLOG("ngreen", "V67: wl eventChain=0x%016llx", (unsigned long long)eventChain);
				int esIdx = 0;
				uint64_t esPtr = eventChain;
				while (esPtr && esIdx < 16) {
					auto *esObj = reinterpret_cast<OSObject *>(esPtr);
					const char *esClass = "?";
					if (esObj->getMetaClass()) {
						esClass = esObj->getMetaClass()->getClassName();
					}
					SYSLOG("ngreen", "V67: eventSource[%d]: class=%s ptr=0x%016llx",
						esIdx, esClass, (unsigned long long)esPtr);
					// IOEventSource::eventChainNext is typically at offset 0x10
					auto *esBase = reinterpret_cast<uint8_t *>(esPtr);
					esPtr = *reinterpret_cast<uint64_t *>(esBase + 0x10);
					esIdx++;
				}
				SYSLOG("ngreen", "V67: workLoop has %d event sources in chain", esIdx);
			}
			
			// V67: Key accelerator fields
			for (int i = 0; i < 8; i++) {
				uint64_t val = getMember<uint64_t>(svc, 0xe00 + i * 8);
				SYSLOG("ngreen", "V67: accel[0x%03x]=0x%016llx", 0xe00 + i * 8, (unsigned long long)val);
			}
			
			// V70: Read ACTUAL GGTT PTEs from correct base (0x800000, not the 0x100000
			// fence registers that V68 used). Each PTE is 8 bytes (64-bit).
			// PTE format: phys_addr[47:12] | flags[11:0], bit 0 = VALID/PRESENT.
			SYSLOG("ngreen", "V70: GGTT PTE dump (base=0x%x):", GEN8_GGTT_PTE_BASE);
			for (int pg = 0; pg < 16; pg++) {
				uint32_t pteLo = NGreen::callback->readReg32(GGTT_PTE_LO(pg));
				uint32_t pteHi = NGreen::callback->readReg32(GGTT_PTE_HI(pg));
				SYSLOG("ngreen", "V70: GGTT[%2d] = 0x%08x:%08x %s",
					   pg, pteHi, pteLo, (pteLo & 1) ? "VALID" : "INVALID");
			}
			// Check pages around stolen memory boundary (128MB = 0x8000 pages)
			for (int i = 0; i < 4; i++) {
				int pg = 0x7FFE + i;
				uint32_t pteLo = NGreen::callback->readReg32(GGTT_PTE_LO(pg));
				uint32_t pteHi = NGreen::callback->readReg32(GGTT_PTE_HI(pg));
				SYSLOG("ngreen", "V70: GGTT[0x%x] = 0x%08x:%08x %s",
					   pg, pteHi, pteLo, (pteLo & 1) ? "VALID" : "INVALID");
			}
			// MMIO BAR length — tells us if GGTT PTE access is direct or indirect
			SYSLOG("ngreen", "V70: rmmio length=0x%llx (GGTT direct=%s)",
				   (unsigned long long)NGreen::callback->rmmio->getLength(),
				   (GEN8_GGTT_PTE_BASE + 16*8 <= NGreen::callback->rmmio->getLength()) ? "yes" : "no(indirect)");
		}
	}
	
	// V64: On iteration 3, advance CSB read pointer to match write pointer
	// CSB ptr format: bits[10:8]=write ptr (GPU), bits[2:0]=read ptr (software)
	// If write > read, scheduler is stuck waiting for CSB processing
	if (v60Count == 3) {
		uint32_t csbPtrNow = NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
		uint32_t writeIdx = (csbPtrNow >> 8) & 0x7;
		uint32_t readIdx  = csbPtrNow & 0x7;
		SYSLOG("ngreen", "V64: CSB ptr=0x%x write=%d read=%d", csbPtrNow, writeIdx, readIdx);
		if (writeIdx != readIdx && v60Aggressive) {
			// Advance read pointer to match write pointer
			uint32_t newPtr = (csbPtrNow & ~0x7) | (writeIdx & 0x7);
			NGreen::callback->writeReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE), newPtr);
			uint32_t verify = NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
			SYSLOG("ngreen", "V64: CSB read ptr advanced %d->%d (wrote 0x%x, readback 0x%x)",
				   readIdx, writeIdx, newPtr, verify);
		} else if (writeIdx != readIdx) {
			SYSLOG("ngreen", "V64: read-only mode (use -ngreenv60rw to advance CSB read ptr)");
		} else {
			SYSLOG("ngreen", "V64: CSB ptrs already matched (write=read=%d)", writeIdx);
		}
	}
	
	// V65: Enforce RCS0 interrupt enable on EVERY iteration (moved from V64 iteration 5).
	// V64 proved the fix at T+10s was too late — scheduler already gave up.
	// Now combined with V65 pre-start + V65W watchdog fixes for full coverage.
	/*if (v60Aggressive) {
		uint32_t rcIntrEn = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t rcsMask  = NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK);
		
		// Tier-1 IRQ policy: RCS only on spoofed non-TGL by default.
		uint32_t wantBits = getV65Tier1WantBits(NGreen::callback->isRealTGL);
		if ((rcIntrEn & wantBits) != wantBits) {
			uint32_t newEn = rcIntrEn | wantBits;
			NGreen::callback->writeReg32(GEN11_RENDER_COPY_INTR_ENABLE, newEn);
			if (v60Count <= 3) {
				SYSLOG("ngreen", "V65M[%d]: tier-1 fix 0x%x->0x%x", v60Count, rcIntrEn,
					NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE));
			}
		}
		// Unmask context-switch + user interrupt for RCS (0 = unmasked, 1 = masked)
		uint32_t wantUnmasked = GT_CONTEXT_SWITCH_INTERRUPT | GT_RENDER_USER_INTERRUPT;
		if (rcsMask & wantUnmasked) {
			uint32_t newMask = rcsMask & ~wantUnmasked;
			NGreen::callback->writeReg32(GEN11_RCS0_RSVD_INTR_MASK, newMask);
			if (v60Count <= 3) {
				SYSLOG("ngreen", "V65M[%d]: tier-2 fix mask 0x%x->0x%x", v60Count, rcsMask, newMask);
			}
		}
	}*/

	// V65: Enforce RCS0 interrupt enable on EVERY iteration (moved from V64 iteration 5).
	// V64 proved the fix at T+10s was too late — scheduler already gave up.
	// Now combined with V65 pre-start + V65W watchdog fixes for full coverage.
	/*if (v60Aggressive) {
		uint32_t rcIntrEn = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t rcsMask  = NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK);
		
		// Tier-1 IRQ policy follows V65 helper (RCS-only on spoof path unless V142 orig/BCS requested).
		uint32_t wantBits = getV65Tier1WantBits(NGreen::callback->isRealTGL);
		if ((rcIntrEn & wantBits) != wantBits) {
			uint32_t newEn = rcIntrEn | wantBits;
			NGreen::callback->writeReg32(GEN11_RENDER_COPY_INTR_ENABLE, newEn);
			if (v60Count <= 3) {
				SYSLOG("ngreen", "V65M[%d]: tier-1 fix 0x%x->0x%x", v60Count, rcIntrEn,
					NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE));
			}
		}
		// Unmask context-switch + user interrupt for RCS (0 = unmasked, 1 = masked)
		uint32_t wantUnmasked = GT_CONTEXT_SWITCH_INTERRUPT | GT_RENDER_USER_INTERRUPT;
		if (rcsMask & wantUnmasked) {
			uint32_t newMask = rcsMask & ~wantUnmasked;
			NGreen::callback->writeReg32(GEN11_RCS0_RSVD_INTR_MASK, newMask);
			if (v60Count <= 3) {
				SYSLOG("ngreen", "V65M[%d]: tier-2 fix mask 0x%x->0x%x", v60Count, rcsMask, newMask);
			}
		}
	}*/
	
	// ── V79: Plane monitor (strictly read-only) ──
	// Any plane-format writes here have caused either WS crash loops or WS hangs.
	// Keep this path logging-only; never modify PLANE_CTL/STRIDE/SURF in V60.
	/*{
		uint32_t planCtl  = NGreen::callback->readReg32(0x70180); // PLANE_CTL
		uint32_t planStrd = NGreen::callback->readReg32(0x70188); // PLANE_STRIDE
		uint32_t planSurf = NGreen::callback->readReg32(0x7019C); // PLANE_SURF
		uint32_t planSize = NGreen::callback->readReg32(0x70190); // PLANE_SIZE
		uint32_t planPos  = NGreen::callback->readReg32(0x7018C); // PLANE_POS
		uint32_t pipeConf = NGreen::callback->readReg32(0x70008); // PIPE_CONF_A
		uint32_t tiling = (planCtl >> 10) & 0x7; // bits[12:10]

		// Always log plane state for diagnostics
		SYSLOG("ngreen", "V79[%d]: PIPE=0x%x CTL=0x%x STRIDE=0x%x SURF=0x%x SIZE=0x%x POS=0x%x tiling=%d",
			   v60Count, pipeConf, planCtl, planStrd, planSurf, planSize, planPos, tiling);
	}*/

	// ── V79: Plane monitor / experimental linearization ──
	// The old copy only reached brief first-light when the experimental path forced
	// the scanout plane to linear and re-armed PLANE_SURF. Keep the default path
	// read-only; restore the old behavior only with -ngreenexp.
	/*if (isExperimentalMonitorEnabled() && v60Aggressive ) {
		if (v60Count <= 5 || v60Count == 10 || v60Count == 20 || v60Count == 30) {
			uint32_t planCtl  = NGreen::callback->readReg32(0x70180); // PLANE_CTL
			uint32_t planStrd = NGreen::callback->readReg32(0x70188); // PLANE_STRIDE
			uint32_t planSurf = NGreen::callback->readReg32(0x7019C);
			uint32_t tiling = (planCtl >> 10) & 0x7; // bits[12:10]
			// V99S already ensures STRIDE=0xa0 and CTL=Y-tiled before every SURF arm.
			// Do NOT re-convert tiling or recalculate stride (those are now correct).
			// Just enforce correct values and re-arm SURF to keep the display engine live.
			// Skip if SURF=0 (plane disabled — arming a null surface causes garbage).
			if (planSurf != 0) {
				uint32_t neededStrd = 0xa0;
				uint32_t neededCtl  = (planCtl & ~(0x7u << 10)) | (0x4u << 10); // Y-tiled
				bool strdWrong  = (planStrd != neededStrd);
				bool tilingWrong = (tiling != 4);
				if (strdWrong)   NGreen::callback->writeReg32(0x70188, neededStrd);
				if (tilingWrong) NGreen::callback->writeReg32(0x70180, neededCtl);
				NGreen::callback->writeReg32(0x7019C, planSurf); // re-arm
				SYSLOG("ngreen", "V79RW[%d]: CTL=0x%x->0x%x STRIDE=0x%x->0x%x SURF=0x%x",
					   v60Count,
					   planCtl,  (tilingWrong ? neededCtl  : planCtl),
					   planStrd, (strdWrong   ? neededStrd : planStrd),
					   planSurf);
			} else {
				SYSLOG("ngreen", "V79RW[%d]: SURF=0 skip. CTL=0x%x STRIDE=0x%x tiling=%d",
					   v60Count, planCtl, planStrd, tiling);
			}
		}
	} else {
		uint32_t planCtl  = NGreen::callback->readReg32(0x70180); // PLANE_CTL
		uint32_t planStrd = NGreen::callback->readReg32(0x70188); // PLANE_STRIDE
		uint32_t planSurf = NGreen::callback->readReg32(0x7019C); // PLANE_SURF
		uint32_t planSize = NGreen::callback->readReg32(0x70190); // PLANE_SIZE
		uint32_t planPos  = NGreen::callback->readReg32(0x7018C); // PLANE_POS
		uint32_t pipeConf = NGreen::callback->readReg32(0x70008); // PIPE_CONF_A
		uint32_t tiling = (planCtl >> 10) & 0x7; // bits[12:10]
		SYSLOG("ngreen", "V79[%d]: PIPE=0x%x CTL=0x%x STRIDE=0x%x SURF=0x%x SIZE=0x%x POS=0x%x tiling=%d",
			   v60Count, pipeConf, planCtl, planStrd, planSurf, planSize, planPos, tiling);

		// V105B: same blanking-pedestal check as V105, applied here after V79 reads SURF.
		if (planSurf != 0) {
			NGreen::callback->writeReg32(0x4A400, 0);    // index 0
			uint32_t g0b  = NGreen::callback->readReg32(0x4A404);
			NGreen::callback->writeReg32(0x4A400, 128);
			uint32_t g128 = NGreen::callback->readReg32(0x4A404);
			if (g0b == g128 || g128 == 0) {
				static int v105bCount = 0;
				if (v105bCount < 10) {
					v105bCount++;
					SYSLOG("ngreen", "V105B[%d]: Gamma bad (iter %d, SURF=0x%x g0=0x%x g128=0x%x) — linear LUT",
					       v105bCount, v60Count, planSurf, g0b, g128);
				}
				NGreen::callback->writeReg32(0x4A400, 0x8000);
				for (int i = 0; i < 256; i++) {
					uint32_t v10 = (uint32_t)i * 4;
					NGreen::callback->writeReg32(0x4A404, (v10 << 20) | (v10 << 10) | v10);
				}
			}
		}
	}*/

	// ── V75: Display pipeline register dump — diagnose black screen ──
	// System stays alive but display is black. Read pipe/plane/transcoder/backlight
	// registers to understand what state the display hardware is in.
	if (v60Count == 3 || v60Count == 30) {
		// Pipe A config
		uint32_t pipeConf  = NGreen::callback->readReg32(0x70008); // PIPE_CONF_A
		uint32_t pipeSrc   = NGreen::callback->readReg32(0x6001C); // PIPE_SRCSZ_A
		// Plane 1 on Pipe A
		uint32_t planCtl   = NGreen::callback->readReg32(0x70180); // PLANE_CTL
		uint32_t planStrd  = NGreen::callback->readReg32(0x70188); // PLANE_STRIDE
		uint32_t planSurf  = NGreen::callback->readReg32(0x7019C); // PLANE_SURF
		uint32_t planSize  = NGreen::callback->readReg32(0x70190); // PLANE_SIZE
		uint32_t planPos   = NGreen::callback->readReg32(0x7018C); // PLANE_POS
		uint32_t planOff   = NGreen::callback->readReg32(0x70194); // PLANE_OFFSET
		// Transcoder A
		uint32_t transConf = NGreen::callback->readReg32(0x60008); // TRANS_CONF_A (or PIPE_CONF for ICL+)
		uint32_t transH    = NGreen::callback->readReg32(0x60000); // HTOTAL_A
		uint32_t transV    = NGreen::callback->readReg32(0x6000C); // VTOTAL_A
		// DDI / port
		uint32_t ddiFuncA  = NGreen::callback->readReg32(0x60400); // DDI_FUNC_CTL_A (trans EDP on TGL)
		uint32_t ddiFunc1  = NGreen::callback->readReg32(0x60100); // DDI_FUNC_CTL_1 (trans A)
		// Backlight
		uint32_t blcPwm    = NGreen::callback->readReg32(0xC8250); // BLC_PWM_CTL2
		uint32_t blcDuty   = NGreen::callback->readReg32(0xC8254); // BLC_PWM_DATA / duty cycle
		uint32_t sblcPwm   = NGreen::callback->readReg32(0xC8254); // South BLC_PWM_CTL
		// Power wells
		uint32_t pwrWell   = NGreen::callback->readReg32(0x45400); // PWR_WELL_CTL2 (ICL+)
		uint32_t dcState   = NGreen::callback->readReg32(0x45504); // DC_STATE_EN

		SYSLOG("ngreen", "V75[%d]: PIPE_CONF=0x%x PIPE_SRC=0x%x", v60Count, pipeConf, pipeSrc);
		SYSLOG("ngreen", "V75[%d]: PLANE_CTL=0x%x STRIDE=0x%x SURF=0x%x SIZE=0x%x POS=0x%x OFF=0x%x",
			   v60Count, planCtl, planStrd, planSurf, planSize, planPos, planOff);
		SYSLOG("ngreen", "V75[%d]: TRANS_CONF=0x%x HTOTAL=0x%x VTOTAL=0x%x",
			   v60Count, transConf, transH, transV);
		SYSLOG("ngreen", "V75[%d]: DDI_FUNC_EDP=0x%x DDI_FUNC_A=0x%x",
			   v60Count, ddiFuncA, ddiFunc1);
		SYSLOG("ngreen", "V75[%d]: BLC_PWM=0x%x BLC_DUTY=0x%x SBLC=0x%x",
			   v60Count, blcPwm, blcDuty, sblcPwm);
		SYSLOG("ngreen", "V75[%d]: PWR_WELL=0x%x DC_STATE=0x%x",
			   v60Count, pwrWell, dcState);

		// V103P removed (was: every V60 tick, force DC_STATE_EN back to 0 when ADL-P
		// DMC is active — third belt of the V103 trio). Now read-only: log the value
		// when non-zero so we can see when Apple's DMC actually wants to enter a DC
		// state. With V103 raWriteRegister32-side and V103F FastWrite-side also off,
		// this is the full "no DC_STATE_EN block" experiment.
		if (NGreen::callback->dmcIsAdlp && dcState != 0)
			NGreen::callback->adlpDcExit("V60");
		// V105: Pipe-A gamma LUT enforcement — write linear pass-through when LUT is bad.
		// The Apple driver enables precision gamma mode before WindowServer writes the actual
		// LUT.  Two bad states are observed:
		//   zero LUT (g[128]=0): all pixels map to black
		//   blanking pedestal (g[128]=0x8020080, R=G=B=128/1023): uniform dim grey —
		//   the Apple IGFB power-management code sets this during display blanking and
		//   does not clear it before WindowServer takes over.  Both states cause the
		//   display to appear black/dark regardless of framebuffer content.
		// Detect: sample entries 0 and 128; if they are equal (constant LUT = blanking)
		// or both are zero, write a linear 8→10-bit pass-through until WS initialises.
		// PAL_PREC_DATA (0x4A404) format: bits[29:20]=red, [19:10]=green, [9:0]=blue.
		{
			uint32_t planSurf = NGreen::callback->readReg32(0x7019C); // PLANE_SURF_A
			if (planSurf != 0) {
				NGreen::callback->writeReg32(0x4A400, 0);    // PAL_PREC_INDEX = 0
				uint32_t g0   = NGreen::callback->readReg32(0x4A404);
				NGreen::callback->writeReg32(0x4A400, 128);  // PAL_PREC_INDEX = 128
				uint32_t g128 = NGreen::callback->readReg32(0x4A404);
				// Constant LUT (all entries equal) = blanking or zero = bad state
				if (g0 == g128 || g128 == 0) {
					static int v105Count = 0;
					if (v105Count < 10) {
						v105Count++;
						SYSLOG("ngreen", "V105[%d]: Gamma bad at iter %d (g0=0x%x g128=0x%x) — linear pass-through",
						       v105Count, v60Count, g0, g128);
					}
					// Write linear LUT: set index=0 + AUTO_INCREMENT (bit 15 = 0x8000)
					NGreen::callback->writeReg32(0x4A400, 0x8000);
					for (int i = 0; i < 256; i++) {
						uint32_t v10 = (uint32_t)i * 4;
						NGreen::callback->writeReg32(0x4A404, (v10 << 20) | (v10 << 10) | v10);
					}
				}
			}
		}

		// V104P removed (was: V60-tick log "V195 miss?" if CTL1 != uefiCtl1 | 0x401).
		// V195 is gone (passthrough), so the "miss" framing is meaningless. The V60 V75
		// SYSLOG above already prints PWR_WELL=0x%x every tick, so CTL1 drift is observable
		// without this duplicate log line.
	}

	// ── V76: Deep display diagnostic + framebuffer physical write test ──
	// V75 proved: ALL display regs are correctly configured (pipe, plane, DDI, backlight ON).
	// Black screen must be because SURF=0x412ad000 points to unwritten (zeroed) memory.
	// Verify GGTT mapping, check WM/PSR, and write a white test pattern directly
	// to the framebuffer surface physical memory to prove the display pipeline works.
	if (v60Count == 5) {
		// 1. Read PLANE_SURF to get current surface GGTT address
		uint32_t surfAddr = NGreen::callback->readReg32(0x7019C); // PLANE_SURF
		uint32_t surfPage = surfAddr >> 12;  // GGTT page number

		// 2. Read GGTT PTE for this page — verify mapping is valid
		uint32_t pteLo = NGreen::callback->readReg32(GGTT_PTE_LO(surfPage));
		uint32_t pteHi = NGreen::callback->readReg32(GGTT_PTE_HI(surfPage));
		uint64_t pte = ((uint64_t)pteHi << 32) | pteLo;
		uint64_t physAddr = pte & 0x0000FFFFFFFFF000ULL;
		bool pteValid = (pteLo & 0x1) != 0;
		SYSLOG("ngreen", "V76[%d]: SURF=0x%x page=0x%x PTE=0x%x:%08x phys=0x%llx valid=%d",
			   v60Count, surfAddr, surfPage, pteHi, pteLo,
			   (unsigned long long)physAddr, pteValid);

		// Also check a few neighboring GGTT pages for the surface
		for (int i = 1; i <= 3; i++) {
			uint32_t nLo = NGreen::callback->readReg32(GGTT_PTE_LO(surfPage + i));
			uint32_t nHi = NGreen::callback->readReg32(GGTT_PTE_HI(surfPage + i));
			uint64_t nPhys = (((uint64_t)nHi << 32) | nLo) & 0x0000FFFFFFFFF000ULL;
			SYSLOG("ngreen", "V76[%d]: SURF+%d PTE=0x%x:%08x phys=0x%llx v=%d",
				   v60Count, i, nHi, nLo, (unsigned long long)nPhys, nLo & 1);
		}

		// 3. Watermark registers — if wrong, the plane gets no FIFO bandwidth → blank
		uint32_t wm0 = NGreen::callback->readReg32(0x70240); // CUR_WM_A_0 (WM level 0)
		uint32_t wm1 = NGreen::callback->readReg32(0x70244); // CUR_WM_A_1
		uint32_t planWm0 = NGreen::callback->readReg32(0x70268); // PLANE_WM_A_0 (plane 1 WM level 0)
		uint32_t planWm1 = NGreen::callback->readReg32(0x7026C); // PLANE_WM_A_1
		uint32_t planWmT = NGreen::callback->readReg32(0x70278); // PLANE_WM_TRANS (transition WM)
		SYSLOG("ngreen", "V76[%d]: WM cur0=0x%x cur1=0x%x plan0=0x%x plan1=0x%x planT=0x%x",
			   v60Count, wm0, wm1, planWm0, planWm1, planWmT);

		// 4. PSR—Panel Self-Refresh: if active and confused, panel may show stale/black
		uint32_t psrCtl  = NGreen::callback->readReg32(0x64800); // EDP_PSR_CTL
		uint32_t psrSts  = NGreen::callback->readReg32(0x64840); // EDP_PSR_STATUS
		uint32_t psrCtl2 = NGreen::callback->readReg32(0x60900); // EDP_PSR2_CTL
		SYSLOG("ngreen", "V76[%d]: PSR_CTL=0x%x PSR_STATUS=0x%x PSR2_CTL=0x%x",
			   v60Count, psrCtl, psrSts, psrCtl2);

		// 5. Gamma LUT sample — if gamma table is zeroed, all output = black.
		// In read-only mode keep current index untouched and only sample current entry.
		uint32_t gammaIdx = NGreen::callback->readReg32(0x4A400); // PREC_PAL_INDEX_A
		uint32_t gamma0 = 0;
		uint32_t gamma128 = 0;
		uint32_t gamma255 = 0;
		if (v60Aggressive) {
			NGreen::callback->writeReg32(0x4A400, 0);                 // Set index to 0
			gamma0 = NGreen::callback->readReg32(0x4A404);            // PREC_PAL_DATA_A
			NGreen::callback->writeReg32(0x4A400, 128);               // Mid-range index
			gamma128 = NGreen::callback->readReg32(0x4A404);
			NGreen::callback->writeReg32(0x4A400, 255);               // Near max index
			gamma255 = NGreen::callback->readReg32(0x4A404);
			NGreen::callback->writeReg32(0x4A400, gammaIdx);          // Restore original index
		} else {
			gamma0 = NGreen::callback->readReg32(0x4A404);
			gamma128 = gamma0;
			gamma255 = gamma0;
		}
		SYSLOG("ngreen", "V76[%d]: GAMMA idx_was=0x%x g[0]=0x%x g[128]=0x%x g[255]=0x%x",
			   v60Count, gammaIdx, gamma0, gamma128, gamma255);

		// (V105 now runs every tick in the periodic section above)

		// 6. Framebuffer physical read test — read pixels from the display surface
		// to check current content. V83 handles filling; V76 only reads now.
		if (pteValid && physAddr != 0) {
			auto *desc = IOMemoryDescriptor::withPhysicalAddress(
				(IOPhysicalAddress)physAddr, 4096, kIODirectionIn);
			if (desc) {
				auto *map = desc->createMappingInTask(kernel_task, 0,
					kIOMapAnywhere | kIOMapInhibitCache, 0, 4096);
				if (map) {
					volatile uint32_t *fb = (volatile uint32_t *)map->getVirtualAddress();
					SYSLOG("ngreen", "V76[%d]: READ px[0]=0x%x px[512]=0x%x px[1023]=0x%x phys=0x%llx",
						   v60Count, fb[0], fb[512], fb[1023],
						   (unsigned long long)physAddr);
					map->release();
				}
				desc->release();
			}
		} else {
			SYSLOG("ngreen", "V76[%d]: SURF PTE invalid — cannot read framebuffer", v60Count);
		}
	}

	// ── V84: Page-by-page framebuffer fill — persistent, no GGTT modification ──
	// V82 proved: display engine reads from PLANE_SURF via GGTT (magenta visible).
	// V83 bug: SURF redirect to GGTT page 0 → MCE at phys 0x3e800000 (stolen mem base).
	// MC Bank 11 (GPU) uncorrected error on all 20 CPUs. NEVER touch stolen base.
	// V88: Color band diagnostic + GGTT TLB flush + plane toggle + transcoder probe
	// V87 PROVED: SURF==SURFLIVE always MATCH. Only Plane 1 active. PipeB OFF.
	// 4000 pages filled, magenta persists. Display still shows bars.
	// PARADOX: buffer is correct, plane reads it, yet bars appear.
	// V88: Fill 4 color bands (RED/GREEN/BLUE/WHITE) to test if display reads our
	// data at all. Flush GGTT TLB. Toggle plane off/on. Probe transcoders.

	// V88 scanout fill is intentionally opt-in: it paints debug bars/colors and can
	// override normal Apple UI composition. Enable only for diagnostics with -ngreenv88.
	// In dp0 mode V88 is harmful: readReg32(PLANE_SURF) returns the hardware echo of
	// setupScanoutMemory's 0x412be000 write (before V99S redirects it), causing V88[5]'s
	// plane toggle to arm with the wrong SURF and fill non-aperture pages with color bands.
	if (isExperimentalMonitorEnabled() && isV88ScanoutFillEnabled() && v60Count >= 1 && v60Count <= 30) {
		uint32_t surfAddr = NGreen::callback->readReg32(0x7019C);
		uint32_t surfPage = surfAddr >> 12;

		// 1. Iteration 1-3: Deep transcoder & pipe diagnostic
		if (v60Count <= 3) {
			// Check ALL transcoder configs (A, B, C, D, EDP)
			uint32_t transAconf  = NGreen::callback->readReg32(0x60008);
			uint32_t transBconf  = NGreen::callback->readReg32(0x61008);
			uint32_t transEDPconf = NGreen::callback->readReg32(0x6F008);
			SYSLOG("ngreen", "V88[%d]: TransA=0x%x TransB=0x%x TransEDP=0x%x",
				   v60Count, transAconf, transBconf, transEDPconf);

			// DDI function control for each transcoder
			uint32_t transA_ddi = NGreen::callback->readReg32(0x60400);  // TRANS_DDI_FUNC_CTL_A
			uint32_t transEDP_ddi = NGreen::callback->readReg32(0x6F400); // TRANS_DDI_FUNC_CTL_EDP
			SYSLOG("ngreen", "V88[%d]: TransA_DDI=0x%x TransEDP_DDI=0x%x",
				   v60Count, transA_ddi, transEDP_ddi);

			// Pipe status — check for underruns
			uint32_t pipeStatA = NGreen::callback->readReg32(0x70024);  // PIPE_STATUS_A (or PIPEASTAT)
			// DSB control
			uint32_t dsbCtl = NGreen::callback->readReg32(0x70840);  // DSB_CTRL pipe A
			// PLANE_COLOR_CTL
			uint32_t planeColorCtl = NGreen::callback->readReg32(0x701CC);
			SYSLOG("ngreen", "V88[%d]: PIPE_STAT=0x%x DSB_CTL=0x%x COLOR_CTL=0x%x",
				   v60Count, pipeStatA, dsbCtl, planeColorCtl);

			// Plane 1 KEY registers
			uint32_t plKeyVal = NGreen::callback->readReg32(0x70194);   // PLANE_KEYVAL
			uint32_t plKeyMax = NGreen::callback->readReg32(0x70198);   // PLANE_KEYMSK (or MAX)
			uint32_t plOffset = NGreen::callback->readReg32(0x701A4);   // PLANE_OFFSET
			uint32_t plAuxSurf = NGreen::callback->readReg32(0x701A0);  // PLANE_AUX_SURF
			SYSLOG("ngreen", "V88[%d]: KEYVAL=0x%x KEYMSK=0x%x OFFSET=0x%x AUX=0x%x",
				   v60Count, plKeyVal, plKeyMax, plOffset, plAuxSurf);
		}

		// 2. Flush GGTT TLB (GFX_FLSH_CNTL_GEN6 = 0x101008)
		NGreen::callback->writeReg32(0x101008, 0x1);
		// Wait for flush to complete (bit 0 clears when done)
		for (int i = 0; i < 100; i++) {
			if (!(NGreen::callback->readReg32(0x101008) & 1)) break;
		}

		// 3. Fill with 4 COLOR BANDS (different color per screen quarter)
		//    2560x1600 @ 4BPP, stride=10240B, 1600 lines total
		//    Each page = 4096B. Lines/page = 4096/10240 ≈ 0.4
		//    Pages per quarter = 4000/4 = 1000
		int filled = 0, failed = 0, skipped = 0;
		uint32_t firstPx = 0;
		uint64_t firstPhys = 0;

		for (int p = 0; p < 4000; p++) {
			uint32_t lo = NGreen::callback->readReg32(GGTT_PTE_LO(surfPage + p));
			uint32_t hi = NGreen::callback->readReg32(GGTT_PTE_HI(surfPage + p));
			uint64_t phys = (((uint64_t)hi << 32) | lo) & 0x0000FFFFFFFFF000ULL;
			if (!(lo & 1) || phys == 0) { failed++; continue; }
			if (phys < 0x40000000ULL) { skipped++; continue; }

			// Choose color by screen quarter
			uint32_t color;
			if (p < 1000)      color = 0xFFFF0000;  // RED    (top)
			else if (p < 2000) color = 0xFF00FF00;  // GREEN  (2nd)
			else if (p < 3000) color = 0xFF0000FF;  // BLUE   (3rd)
			else               color = 0xFFFFFFFF;  // WHITE  (bottom)

			auto *desc = IOMemoryDescriptor::withPhysicalAddress(
				(IOPhysicalAddress)phys, 4096, kIODirectionInOut);
			if (!desc) { failed++; continue; }
			auto *map = desc->createMappingInTask(kernel_task, 0,
				kIOMapAnywhere | kIOMapInhibitCache, 0, 4096);
			if (map) {
				volatile uint32_t *fb = (volatile uint32_t *)map->getVirtualAddress();
				if (p == 0) {
					firstPx = fb[0];
					firstPhys = phys;
					if (!v85PersistMap && v60Count >= 3) {
						v85PersistMap = desc->createMappingInTask(kernel_task, 0,
							kIOMapAnywhere | kIOMapInhibitCache, 0, 4096);
						v85SurfAddr = surfAddr;
					}
				}
				for (int px = 0; px < 1024; px++)
					fb[px] = color;
				filled++;
				map->release();
			} else { failed++; }
			desc->release();
		}

		// 4. Re-arm PLANE_SURF (guard: zero -> GGTT[0] -> stolen mem -> MCE)
		if (filled > 0 && surfAddr != 0)
			NGreen::callback->writeReg32(0x7019C, surfAddr);

		// 5. On iteration 5: toggle plane OFF then ON (force re-init)
		if (v60Count == 5 && filled > 0 && surfAddr != 0) {
			uint32_t planCtl = NGreen::callback->readReg32(0x70180);
			// Disable plane (clear bit 31)
			NGreen::callback->writeReg32(0x70180, planCtl & ~0x80000000u);
			NGreen::callback->writeReg32(0x7019C, surfAddr); // commit disable
			// Small delay — ~1000 register reads as delay
			for (volatile int d = 0; d < 1000; d++)
				NGreen::callback->readReg32(0x70180);
			// Re-enable plane with same settings
			NGreen::callback->writeReg32(0x70180, planCtl);
			NGreen::callback->writeReg32(0x7019C, surfAddr); // commit enable
			SYSLOG("ngreen", "V88[%d]: plane toggle OFF->ON CTL=0x%x", v60Count, planCtl);
		}

		if (v60Count <= 5 || v60Count == 10 || v60Count == 20) {
			SYSLOG("ngreen", "V88[%d]: SURF=0x%x filled=%d fail=%d skip=%d px0=0x%x phys=0x%llx",
				   v60Count, surfAddr, filled, failed, skipped, firstPx,
				   (unsigned long long)firstPhys);
		}
	}

	// ── V68: Iteration 5 — deep probe e08obj + GGTT PTE check ──
	// V67 proved: sched error clear sticks, but hardware ERROR_GEN6=0x7b recurs
	// every ~10s independently. Need to find WHAT generates the error.
	if (v60Count == 5) {
		uint64_t schedObj = getMember<uint64_t>(svc, 0xe08);
		if (schedObj) {
			auto *objBase = reinterpret_cast<uint8_t *>(schedObj);
			// V68: Dump e08obj bytes 0x100-0x200 — look for engine dead/reset flags
			for (int i = 0; i < 32; i++) {
				uint64_t val = *reinterpret_cast<uint64_t *>(objBase + 0x100 + i * 8);
				if (val != 0) {
					SYSLOG("ngreen", "V68: e08deep[0x%03x]=0x%016llx", 0x100 + i * 8, (unsigned long long)val);
				}
			}
			// V68: Check e08obj+0x200-0x300 — possible per-engine state arrays
			for (int i = 0; i < 32; i++) {
				uint64_t val = *reinterpret_cast<uint64_t *>(objBase + 0x200 + i * 8);
				if (val != 0) {
					SYSLOG("ngreen", "V68: e08deep[0x%03x]=0x%016llx", 0x200 + i * 8, (unsigned long long)val);
				}
			}
		}
		// V68: Re-check scheduler error after V68 early clear at iter 1
		uint64_t schedPtr = getMember<uint64_t>(svc, 0xe00);
		if (schedPtr) {
			auto *schedBase = reinterpret_cast<uint8_t *>(schedPtr);
			uint32_t schedFlag = *reinterpret_cast<uint32_t *>(schedBase + 0x00);
			uint32_t schedErr = *reinterpret_cast<uint32_t *>(schedBase + 0x0C);
			SYSLOG("ngreen", "V68: iter5 sched[0x00]=0x%x sched[0x0C]=0x%x (cleared at iter1)",
				schedFlag, schedErr);
		}
		// V68: Read GGTT PTE for ring buffer page (ring base = acthd area)
		// GGTT is accessed via aperture at BAR offset or via registers
		// Read the first few GGTT PTEs to check format
		uint32_t ringBase44k = 0x98 >> 12; // ring HEAD offset → page 0
		// Read fence regs to check for GGTT format clues
		uint32_t fence0 = NGreen::callback->readReg32(0x100000); // First GGTT PTE
		uint32_t fence1 = NGreen::callback->readReg32(0x100004);
		uint32_t fence2 = NGreen::callback->readReg32(0x100008);
		uint32_t fence3 = NGreen::callback->readReg32(0x10000C);
		SYSLOG("ngreen", "V68: GGTT[0]=0x%x:%x GGTT[1]=0x%x:%x", fence1, fence0, fence3, fence2);
		// Read TLB data again
		uint32_t tlb0 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0);
		uint32_t tlb1 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1);
		uint32_t faultReg = NGreen::callback->readReg32(GEN12_RING_FAULT_REG);
		SYSLOG("ngreen", "V68: FAULT=0x%x TLB0=0x%x TLB1=0x%x", faultReg, tlb0, tlb1);
		// Check encodeFailureStack
		uint32_t failCount = getMember<uint32_t>(svc, 0x1c50);
		SYSLOG("ngreen", "V68: encodeFailureStack count=%u", failCount);
	}
	
	// V68: Iteration 10 — re-check e08obj + scheduler after error suppression
	if (v60Count == 10) {
		uint64_t schedObj = getMember<uint64_t>(svc, 0xe08);
		if (schedObj) {
			auto *objBase = reinterpret_cast<uint8_t *>(schedObj);
			// Re-dump first 256 bytes to see if any state shifted
			for (int i = 0; i < 32; i++) {
				uint64_t val = *reinterpret_cast<uint64_t *>(objBase + i * 8);
				if (val != 0) {
					SYSLOG("ngreen", "V68B: e08obj[0x%03x]=0x%016llx", i * 8, (unsigned long long)val);
				}
			}
		}
		uint64_t schedPtr = getMember<uint64_t>(svc, 0xe00);
		if (schedPtr) {
			auto *schedBase = reinterpret_cast<uint8_t *>(schedPtr);
			// Dump non-zero fields in 0x000-0x400
			for (int i = 0; i < 128; i++) {
				uint64_t val = *reinterpret_cast<uint64_t *>(schedBase + i * 8);
				if (val != 0) {
					SYSLOG("ngreen", "V68B: sched[0x%03x]=0x%016llx", i * 8, (unsigned long long)val);
				}
			}
		}
	}
	
	v60LastHead = rcsHead;
	v60LastTail = rcsTail;
	
	// Re-arm: 60 iterations x 2s = 120s
	// Each timer holds a retain on the IOService (param0 = svc).
	auto *monSvc = static_cast<IOService *>(param0);
	if (v60Count < 60) {
		auto nextTimer = thread_call_allocate(v60GpuHealthMonitor, param0);
		if (nextTimer) {
			uint64_t deadline;
			clock_interval_to_deadline(2, kSecondScale, &deadline);
			thread_call_enter_delayed(nextTimer, deadline);
			// retain is transferred to the next timer — do NOT release here
		} else {
			// alloc failed — release the retain we hold
			monSvc->release();
		}
	} else {
		SYSLOG("ngreen", "V60M: complete — %d iterations, final HEAD=0x%x EXEC=0x%x ch=%d",
			   v60Count, rcsHead, execStatus, childCount);
		monSvc->release(); // last iteration — release the retain
	}
}

// V74: Permanent EMR + GGTT enforcer — fires every 50ms for the lifetime of the GPU.
//
// WHY: On RPL/ADL spoofed as TGL, Apple's AppleIntelTGLGraphics continuously re-writes
// RING_EMR (Error Mask Register) for RCS and BCS back to a non-full-mask value, and also
// re-maps GGTT[0] to the stolen-memory base. Both are fatal:
//   - Un-masked EMR causes the GPU error interrupt to fire on every submitted command,
//     preventing the display pipe from ever activating (all-black screen).
//   - GGTT[0] → stolen mem causes a package-wide MCE if any GPU VA-0 access occurs.
//
// HOW: Apple writes EMR via direct GT MMIO (not through the hookable WriteRegister32
// vtable), so we cannot intercept it. Instead we poll every 50ms and correct any
// deviation. The timer re-arms itself indefinitely; it stops only if MMIO becomes
// invalid (GPU teardown). Each timer instance holds a retain on the IOService and
// transfers it to the next allocation to avoid UAF.
//
// WHAT it does each tick:
//   1. Force RING_EMR(RCS) and RING_EMR(BCS) to 0xFFFFFFFF (all errors masked).
//   2. Clear ERROR_GEN6 if non-zero.
//   3. Re-enforce GGTT[0] → safe wired dummy page (V116, see below).
void Gen11::v71EmrEnforcer(thread_call_param_t param0, thread_call_param_t param1) {
	static int v71Count = 0;
	v71Count++;

	// V74 EMR-mask portion REMOVED (was: every 50ms force RING_EMR(RCS) and RING_EMR(BCS)
	// to 0xFFFFFFFF, clear ERROR_GEN6). Pair-mate to V72R/V72W/V72F. With those three
	// passthrough on the write paths but V74 still polling and rewriting EMR back to
	// all-ones from a thread, the "no blanket EMR mask" experiment was incomplete.
	// Now reads-only: log what Apple's actual EMR/ERROR values look like so we can
	// see WHICH bits Apple unmasks and WHICH errors actually fire (instead of hiding
	// them under a blanket 0xFFFFFFFF). The V116 GGTT[0] re-enforcer below is left
	// intact — that prevents real MCE on stolen-mem GVA-0 access, not a hack.
	uint32_t emrRcs = NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE));
	uint32_t emrBcs = NGreen::callback->readReg32(RING_EMR(BLT_RING_BASE));
	uint32_t err    = NGreen::callback->readReg32(ERROR_GEN6);
	// 19/10: o pânico "spinlock timeout, owner=current_thread" parou dentro deste SYSLOG; com xefix ele não é mais emitido
	static int v74Quiet = -1;
	if (v74Quiet < 0) v74Quiet = checkKernelArgument("-ngreenxefix") ? 1 : 0;
	if (!v74Quiet && (v71Count <= 3 || (err != 0 && v71Count <= 50) ||
	    ((emrRcs != 0xFFFFFFFF || emrBcs != 0xFFFFFFFF) && v71Count <= 50))) {
		SYSLOG("ngreen", "V74Ep[%d]: EMR_RCS=0x%x EMR_BCS=0x%x ERR=0x%x (V74 EMR mask removed)",
		       v71Count, emrRcs, emrBcs, err);
	}

	// V80 legacy ownership mode: one-shot plane linearization at early boot.
	// Runs only during the first 3 ticks (≤150ms after start) — this produces the
	// brief display flash that was present before the start hook was active.
	// After tick 3 WS has composited its first frame and owns those registers;
	// continuing to write them causes WS crash-loop → watchdog KP at T+120s.
	// -ngreenv80l overrides to run continuously (for isolated plane-write testing only).
	/*if (!NGreen::callback->isRealTGL && isV60MonitorEnabled() &&
	    (v71Count <= 3 || isV80LEnforcerEnabled())) {
		uint32_t planCtl = NGreen::callback->readReg32(0x70180); // PLANE_CTL
		bool planeEnabled = !!(planCtl & 0x80000000u);
		if (planeEnabled) {
			uint32_t planStrd = NGreen::callback->readReg32(0x70188); // PLANE_STRIDE
			uint32_t planSurf = NGreen::callback->readReg32(0x7019C); // PLANE_SURF
			uint32_t tiling = (planCtl >> 10) & 0x7; // bits[12:10]
			if (planSurf != 0 && tiling != 0) {
				uint32_t newCtl = planCtl & ~(0x7u << 10); // clear tiling -> linear
				uint32_t newStrd = planStrd;
				if (tiling == 1) {
					newStrd = planStrd * 8;
				} else if (tiling == 4) {
					newStrd = planStrd * 16;
				}
				NGreen::callback->writeReg32(0x70180, newCtl);
				NGreen::callback->writeReg32(0x70188, newStrd);
				NGreen::callback->writeReg32(0x7019C, planSurf);
				if (v71Count <= 20 || (v71Count % 20) == 0) {
					SYSLOG("ngreen", "V80L[%d]: tiling %u->linear CTL 0x%x->0x%x STRIDE 0x%x->0x%x SURF=0x%x",
						   v71Count, tiling, planCtl, newCtl, planStrd, newStrd, planSurf);
				}
			} else if (planSurf != 0) {
				NGreen::callback->writeReg32(0x7019C, planSurf);
				if (v71Count <= 20 || (v71Count % 20) == 0) {
					SYSLOG("ngreen", "V80L[%d]: already linear, SURF re-arm=0x%x", v71Count, planSurf);
				}
			}
		}
	}*/

	// V116: Re-enforce GGTT[0] → safe wired dummy page every 50ms.
	// Apple's TGL framebuffer driver re-writes GGTT[0] to the stolen-memory physical base
	// during its internal GTT setup. Any GPU command that dereferences VA 0x0 then hits
	// stolen memory, triggering an uncorrectable MCE (machine check exception) that kills
	// the whole package. We restore GGTT[0] to a wired zero-page (allocated at start) on
	// every enforcer tick so the stolen-mem mapping can never persist.
	// PTE flags: bits[11:0]=0x9 → present | global (TGL/ICL GTT format, no cache attrs).
	if (v116DummyPhys) {
		uint32_t wantLo = (uint32_t)(v116DummyPhys & 0xFFFFF000ULL) | 0x9;
		uint32_t cur0Lo = NGreen::callback->readReg32(GGTT_PTE_LO(0));
		if (cur0Lo != wantLo) {
			uint32_t wantHi = (uint32_t)(v116DummyPhys >> 32);
			NGreen::callback->writeReg32(GGTT_PTE_LO(0), wantLo);
			NGreen::callback->writeReg32(GGTT_PTE_HI(0), wantHi);
			NGreen::callback->writeReg32(0x101008, 0x1);  // GGTT TLB flush
			if (v71Count <= 10)
				SYSLOG("ngreen", "V116E[%d]: GGTT[0] restored (was 0x%x → 0x%x)",
					   v71Count, cur0Lo, wantLo);
		}
	}

	// V74: Re-arm FOREVER — never stop. 50ms interval.
	// Each timer holds a retain on the IOService (param0). Pass the retain to the
	// next timer so the IOService stays alive. If MMIO is gone (GPU tore down),
	// stop re-arming and release the last retain to avoid UAF.
	auto *emrSvc = static_cast<IOService *>(param0);
	if (!NGreen::callback->mmioValid()) {
		// GPU torn down — stop timer loop and release the retain we hold.
		SYSLOG("ngreen", "V74E[%d]: MMIO gone, stopping EMR enforcer", v71Count);
		emrSvc->release();
		return;
	}
	auto nextTimer = thread_call_allocate(v71EmrEnforcer, param0);
	if (nextTimer) {
		uint64_t deadline;
		clock_interval_to_deadline(50, kMillisecondScale, &deadline);
		thread_call_enter_delayed(nextTimer, deadline);
		// retain is transferred to the next timer — do NOT release here
	} else {
		// alloc failed — stop loop and release our retain
		emrSvc->release();
	}
}

// V85: Static member definitions for persistent FB mapping
IOMemoryMap *Gen11::v85PersistMap = nullptr;
uint32_t Gen11::v85SurfAddr = 0;

// V116: Safe dummy page for GGTT[0] remap (prevents stolen-mem MCE on GPU VA 0 access)
IOBufferMemoryDescriptor *Gen11::v116DummyBuf = nullptr;
uint64_t Gen11::v116DummyPhys = 0;

// V221: Becomes true when IntelAccelerator::start() returns. Used by wrapWaitForStamp
// to block CoreDisplay's stamp-3 wait until the GFX interrupt handler is installed.
volatile bool Gen11::gGfxAccelStartDone = false;

// V54: IRQ watchdog — re-enables Master IRQ if the driver disables it during init.
// Fires every 2s, up to 5 times (10s total), then stops.
void Gen11::v54IrqWatchdog(thread_call_param_t param0, thread_call_param_t) {
	static int v54WatchdogCount = 0;
	v54WatchdogCount++;
	
	uint32_t mstrIrq = NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ);
	bool enabled = !!(mstrIrq & GEN11_MASTER_IRQ);
	
	if (!enabled) {
		// Re-enable Master IRQ
		NGreen::callback->writeReg32(GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
		IODelay(100);
		uint32_t after = NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ);
		SYSLOG("ngreen", "V54W[%d]: Master IRQ was DISABLED (0x%x) — re-enabled → 0x%x",
			v54WatchdogCount, mstrIrq, after);
	} else {
		SYSLOG("ngreen", "V54W[%d]: Master IRQ OK (0x%x)", v54WatchdogCount, mstrIrq);
	}
	
	// V65: Continuously enforce RCS0 tier-1 interrupt enable during ring init window.
	// V64 proved Apple's code leaves RCS0 DISABLED in RENDER_COPY_INTR_ENABLE.
	// The ring activates between V54W[4]-V54W[5] (T+8-10s) — we must have this set BEFORE.
	{
		uint32_t rcIntrEn = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t wantBits = getV65Tier1WantBits(NGreen::callback->isRealTGL);
		bool rcsOk = !!(rcIntrEn & (1 << GEN11_RCS0));
		if (!rcsOk) {
			uint32_t newEn = rcIntrEn | wantBits;
			NGreen::callback->writeReg32(GEN11_RENDER_COPY_INTR_ENABLE, newEn);
			SYSLOG("ngreen", "V65W[%d]: RCS0 tier-1 DISABLED (0x%x) — enabled → 0x%x",
				v54WatchdogCount, rcIntrEn,
				NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE));
		} else {
			SYSLOG("ngreen", "V65W[%d]: RCS0 tier-1 OK (0x%x)", v54WatchdogCount, rcIntrEn);
		}
		// Also enforce tier-2 unmask
		uint32_t rcsMask = NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK);
		uint32_t wantUnmasked = GT_CONTEXT_SWITCH_INTERRUPT | GT_RENDER_USER_INTERRUPT;
		if (rcsMask & wantUnmasked) {
			uint32_t newMask = rcsMask & ~wantUnmasked;
			NGreen::callback->writeReg32(GEN11_RCS0_RSVD_INTR_MASK, newMask);
			SYSLOG("ngreen", "V65W[%d]: RCS mask fix 0x%x->0x%x", v54WatchdogCount, rcsMask, newMask);
		}
		// V65: Proactively clear ERROR_GEN6 to prevent error handler from killing engine
		uint32_t errNow = NGreen::callback->readReg32(ERROR_GEN6);
		if (errNow) {
			NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
			SYSLOG("ngreen", "V65W[%d]: cleared ERROR_GEN6=0x%x", v54WatchdogCount, errNow);
		}
	}
	
	// Also dump quick GPU state for diagnostics
	SYSLOG("ngreen", "V54W[%d]: RCS CTL=0x%x HEAD=0x%x TAIL=0x%x ERROR_GEN6=0x%x",
		v54WatchdogCount,
		NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE)),
		NGreen::callback->readReg32(ERROR_GEN6));
	
	// Re-arm for up to 5 iterations
	if (v54WatchdogCount < 5) {
		// Allocate a fresh timer call for the next iteration
		auto nextTimer = thread_call_allocate(v54IrqWatchdog, nullptr);
		if (nextTimer) {
			uint64_t deadline;
			clock_interval_to_deadline(2, kSecondScale, &deadline);
			thread_call_enter_delayed(nextTimer, deadline);
		}
	} else {
		SYSLOG("ngreen", "V54W: watchdog complete after %d iterations", v54WatchdogCount);
	}
}
// V44: Bundle/property logging helper — logs Metal/GL/VA bundle names after a delay.
static const char *v44SafeCString(OSString *str) {
    if (!str) {
        return "MISSING";
    }
    auto *cstr = str->getCStringNoCopy();
    return cstr ? cstr : "<null-cstr>";
}

static void v44DelayedBundleLog(thread_call_param_t p0, thread_call_param_t p1) {
    auto *svc = static_cast<IOService *>(p0);
    unsigned delayMs = (unsigned)(uintptr_t)p1;

    auto *metalProp = OSDynamicCast(OSString, svc->getProperty("MetalPluginName"));
    auto *glProp    = OSDynamicCast(OSString, svc->getProperty("IOGLBundleName"));
    auto *dvdProp   = OSDynamicCast(OSString, svc->getProperty("IODVDBundleName"));
    auto *accelBid  = OSDynamicCast(OSString, svc->getProperty("AcceleratorBundleIdentifier"));
    auto *fbBid     = OSDynamicCast(OSString, svc->getProperty("FramebufferBundleIdentifier"));
    auto *vaRIDProp = OSDynamicCast(OSNumber, svc->getProperty("IOVARendererID"));

    SYSLOG("ngreen", "V44: T+%ums MetalPlugin=%s GL=%s DVD=%s VARendID=0x%x",
           delayMs,
           v44SafeCString(metalProp),
           v44SafeCString(glProp),
           v44SafeCString(dvdProp),
           vaRIDProp ? (uint32_t)vaRIDProp->unsigned32BitValue() : 0);
    SYSLOG("ngreen", "V44: T+%ums AcceleratorBundleIdentifier=%s FramebufferBundleIdentifier=%s",
           delayMs,
           v44SafeCString(accelBid),
           v44SafeCString(fbBid));

    // Release retain acquired by scheduler helper.
    svc->release();
}

static void v44ScheduleBundleLog(void *accelInstance, unsigned delayMs) {
    auto *svc = static_cast<IOService *>(accelInstance);
    svc->retain();

    thread_call_t tc = thread_call_allocate(v44DelayedBundleLog,
                                        static_cast<thread_call_param_t>(svc));
    if (tc) {
        uint64_t deadline;
        clock_interval_to_deadline(delayMs, kMillisecondScale, &deadline);
        thread_call_enter1_delayed(tc,
                                   (thread_call_param_t)(uintptr_t)delayMs,
                                   deadline);
    } else {
        svc->release();
    }
}

int Gen11::wrapPmNotifyWrapper(unsigned int a0, unsigned int a1, unsigned long long *a2, unsigned int *freq) {
	
	/*struct intel_rps_freq_caps *caps;
	
	caps->rp0_freq *= GEN9_FREQ_SCALER;
	caps->rp1_freq *= GEN9_FREQ_SCALER;
	caps->min_freq *= GEN9_FREQ_SCALER;
	
	uint32_t mult=GEN9_FREQ_SCALER;
	uint32_t ddcc_status = 0;
	
	*freq =caps->rp1_freq;
	return 0;*/
	
	uint32_t cfreq = 0;

	FunctionCast(wrapPmNotifyWrapper, callback->orgPmNotifyWrapper)(a0, a1, a2, &cfreq);
	
	if (!callback->freq_max) {
		callback->freq_max = wrapReadRegister32(callback->framecont, GEN6_RP_STATE_CAP) & 0xFF;

	}
	
	*freq = (GEN9_FREQ_SCALER << GEN9_FREQUENCY_SHIFT) * callback->freq_max;
	return 0;
}

bool Gen11::patchRCSCheck(mach_vm_address_t& start) {
	constexpr unsigned ninsts_max {256};
	
	hde64s dis;
	
	bool found_cmp = false;
	bool found_jmp = false;

	for (size_t i = 0; i < ninsts_max; i++) {
		auto sz = Disassembler::hdeDisasm(start, &dis);

		if (dis.flags & F_ERROR) {
			break;
		}

		/* cmp byte ptr [rcx], 0 */
		if (!found_cmp && dis.opcode == 0x80 && dis.modrm_reg == 7 && dis.modrm_rm == 1)
			found_cmp = true;
		/* jnz rel32 */
		if (found_cmp && dis.opcode == 0x0f && dis.opcode2 == 0x85) {
			found_jmp = true;
			break;
		}

		start += sz;
	}
	
	if (found_jmp) {
		auto status = MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
		if (status == KERN_SUCCESS) {
			constexpr uint8_t nop6[] {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
			lilu_os_memcpy(reinterpret_cast<void*>(start), nop6, arrsize(nop6));
			MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
			return true;
		} else {
			return false;
		}
	} else {
		return false;
	}
}

/**
 * Port of i915 force wake for Gen12 (TGL/ADL/RPL).
 * Replaces IntelAccelerator::SafeForceWakeMultithreaded.
 *
 * Differences from Apple's code:
 * 1. 50 ms ACK timeouts (Apple uses 90 ms) — https://patchwork.kernel.org/patch/7057561/
 * 2. Reserve-bit fallback on primary ACK timeout — https://patchwork.kernel.org/patch/10029821/
 * 3. Correct Gen9-style 3-domain iteration matching Apple's dom bitmask
 *    (dom: bit0=Render, bit1=Media, bit2=Blitter/GT)
 *
 * NOTE: The header's regForDom/ackForDom use Gen11+ domain bitmask IDs
 * (FORCEWAKE_RENDER=1=Render, FORCEWAKE_GT=2, FORCEWAKE_MEDIA=4) which does NOT
 * match Apple's 3-bit dom (1=Render, 2=Media, 4=Blitter). We use direct register
 * mapping here to match Apple's convention, same as WEG's ForceWakeWorkaround.
 */

// Map Apple's 3-bit domain to MMIO request register (Render + GT/Blitter same Gen9-Gen12)
static uint32_t fwReqReg(unsigned d) {
	if (d == DOM_RENDER)  return FORCEWAKE_RENDER_GEN9;   // 0xa278
	if (d == DOM_BLITTER) return FORCEWAKE_BLITTER_GEN9;  // 0xa188
	return 0;  // Media uses Gen11+ per-engine registers, handled separately
}

// Map Apple's 3-bit domain to MMIO ACK register (Render + GT/Blitter same Gen9-Gen12)
static uint32_t fwAckReg(unsigned d) {
	if (d == DOM_RENDER)  return FORCEWAKE_ACK_RENDER_GEN9;   // 0x0D84
	if (d == DOM_BLITTER) return FORCEWAKE_ACK_BLITTER_GEN9;  // 0x130044
	return 0;
}

// Gen12 Media ForceWake: per-engine register pairs (VDBOX + VEBOX)
struct FwMediaEngine {
	uint32_t req;
	uint32_t ack;
	const char *name;
};
static const FwMediaEngine fwMediaEngines[] = {
	{ FORCEWAKE_MEDIA_VDBOX_GEN11(0), FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(0), "VDBOX0" },  // 0xa540 / 0x0D50
	{ FORCEWAKE_MEDIA_VEBOX_GEN11(0), FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(0), "VEBOX0" },  // 0xa560 / 0x0D70
};

void Gen11::wrapSafeForceWake(void *that, bool set, uint32_t dom) {
	forceWake(that, set, dom, 1);  // forward to our forceWake with ctx=1 (normal, non-IRQ)
}

void Gen11::forceWake(void *that, bool set, uint32_t dom, uint8_t ctx) {
	// V61: silenced per-call logging — was flooding Lilu circular buffer (384+ entries/boot)
	// preventing V60M health monitor and delayed child check entries from surviving
	
	// ── Hangcheck: dump GPU state once after stamp-timeout restart ──
	// During init, ~14 forceWake calls happen. After IOAcceleratorFamily2 submits
	// work and the stamp times out (~5s later), a burst of restart-related calls
	// occurs. Dump full RCS/BCS state once on the 30th call to capture post-hang state.
	static int fwCallCount = 0;
	static bool hangcheckDumped = false;
	fwCallCount++;
	
	static int hcQuiet = -1;  // 19/10: dezenas de SYSLOG aqui; o forcewake pode estar em interrupção
	if (hcQuiet < 0) hcQuiet = checkKernelArgument("-ngreenxefix") ? 1 : 0;
	if (!hcQuiet && !hangcheckDumped && fwCallCount == 15) {
		hangcheckDumped = true;
		SYSLOG("ngreen", "=== HANGCHECK: GPU state dump (fwCall=%d) ===", fwCallCount);
		SYSLOG("ngreen", "FBMODE hook: calls=%u bootprog=%u subst=%u argseen=%u lastret=0x%x lastmode=0x%x", gFbModeCalls,
			   gFbModeBootProg, gFbModeSubst, gFbModeArgSeen, gFbModeLastRet, gFbModeLastMode);
		SYSLOG("ngreen", "FBMODE state: tglblock=%u routes=%u icl=%u org_gcdm=0x%llx org_gpi=0x%llx org_vdm=0x%llx",
			   gTglFbBlockRuns, gTglFbRoutesDone, gIcLFbBlockRuns, (unsigned long long)gOrgGcdm,
			   (unsigned long long)gOrgGpi, (unsigned long long)gOrgVdm);
		SYSLOG("ngreen", "FBMODE patch: tried=%u ok=%u", gFbModePatchTried, gFbModePatchOk);
		SYSLOG("ngreen", "FBMODE timings: sym=0x%llx tc=%d tries=%u seen=%u online=%u already=%u built=%u done=%u ret=0x%x crtc=%ux%u",
			   (unsigned long long)gHwCrtcToTimingAddr, gFbtTc != nullptr, gFbtTries, gFbtSeen, gFbtOnline, gFbtAlready,
			   gFbtBuilt, gFbtDone, gFbtRet, gFbtH, gFbtV);
		
		// Acquire both Render + Blitter ForceWake for reliable reads
		NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 1);
		NGreen::callback->writeReg32(FORCEWAKE_BLITTER_GEN9, (1 << 16) | 1);
		IODelay(1000);
		
		SYSLOG("ngreen", "HANGCHECK ForceWake ACK: Render=0x%x Blitter=0x%x",
			NGreen::callback->readReg32(FORCEWAKE_ACK_RENDER_GEN9),
			NGreen::callback->readReg32(FORCEWAKE_ACK_BLITTER_GEN9));
		
		// RCS ring state
		SYSLOG("ngreen", "HANGCHECK RCS HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x",
			NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_START(RENDER_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK RCS ACTHD=0x%x:%08x IPEHR=0x%x",
			NGreen::callback->readReg32(RING_ACTHD_UDW(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_ACTHD(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_IPEHR(RENDER_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK RCS INSTDONE=0x%x DMA_FADD=0x%x:%08x",
			NGreen::callback->readReg32(RING_INSTDONE(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_DMA_FADD_UDW(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_DMA_FADD(RENDER_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK RCS EIR=0x%x ESR=0x%x EMR=0x%x",
			NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_ESR(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE)));
		// V203: EXECLIST_STATUS is a 64-bit context descriptor echo.
		// LRCA GGTT byte address = full64 & ~0xFFF  (bits[11:0] are context flags).
		// Must combine both words — the upper 12 bits of the GGTT address live in elsHi bits[11:0].
		// RING_MODE bit15 = GFX_RUN_LIST_ENABLE; if 0, ExecList submissions are silently dropped.
		{
			uint32_t rcsMode = NGreen::callback->readReg32(RING_MODE(RENDER_RING_BASE));
			SYSLOG("ngreen", "HANGCHECK V205: RING_MODE=0x%x GFX_RUN_LIST_ENABLE=%d",
				rcsMode, !!(rcsMode & (1u << 15)));
			uint32_t elsLo = NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE));
			uint32_t elsHi = NGreen::callback->readReg32(RENDER_RING_BASE + 0x238);
			uint32_t hws   = NGreen::callback->readReg32(RING_HWS_PGA(RENDER_RING_BASE));
			// Correct LRCA extraction: descriptor is 64-bit, LRCA = full64 & ~0xFFF.
			// Using only elsLo gives the wrong address (e.g. 0x18000 instead of 0x40018000).
			uint64_t fullDesc = ((uint64_t)elsHi << 32) | elsLo;
			uint32_t lrcaGgtt = (uint32_t)(fullDesc & ~0xFFFULL);
			SYSLOG("ngreen", "HANGCHECK RCS EXECLIST_STATUS=0x%08x:%08x lrca_ggtt=0x%x HWS_PGA=0x%x",
				elsHi, elsLo, lrcaGgtt, hws);
			// V204: Context Status Buffer — each entry records what happened to submitted contexts.
			// Entry status bits[2:0]: 1=idle, 2=active, 4=preempted, 8=element-switch, 0x10=complete.
			// wr_ptr advances after each event; rd_ptr is the software "consumed up to here" pointer.
			{
				uint32_t csp = NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
				uint32_t wrPtr = (csp >> 8) & 0x7;
				uint32_t rdPtr = csp & 0x7;
				SYSLOG("ngreen", "HANGCHECK V204: CSB CTX_STATUS_PTR=0x%x wr=%d rd=%d",
					csp, wrPtr, rdPtr);
				for (int i = 0; i < 6; i++) {
					uint32_t csbHi = NGreen::callback->readReg32(RING_CONTEXT_STATUS_BUF_HI(RENDER_RING_BASE, i));
					uint32_t csbLo = NGreen::callback->readReg32(RING_CONTEXT_STATUS_BUF(RENDER_RING_BASE, i));
					SYSLOG("ngreen", "HANGCHECK V204: CSB[%d]=%08x:%08x ctx_id=%d status=0x%x%s",
						i, csbHi, csbLo,
						(csbLo >> 8) & 0xFF,
						csbLo & 0xFF,
						(csbLo & 0x10) ? " COMPLETE" :
						(csbLo & 0x08) ? " ELEMENT_SWITCH" :
						(csbLo & 0x04) ? " PREEMPTED" :
						(csbLo & 0x02) ? " ACTIVE" :
						(csbLo & 0x01) ? " IDLE" : " (empty/unknown)");
				}
			}
			// V204: Read PTE for the HWS page — if HWS isn't mapped, CS completion writes fault.
			// Also read PTE for the legacy RING_START page (the preamble ring buffer).
			if (hws) {
				uint32_t hwsPg  = hws >> 12;
				uint32_t hwsPLo = NGreen::callback->readReg32(GGTT_PTE_LO(hwsPg));
				uint32_t hwsPHi = NGreen::callback->readReg32(GGTT_PTE_HI(hwsPg));
				SYSLOG("ngreen", "HANGCHECK V204: HWS PTE[page 0x%x]=%08x:%08x present=%d llc=%d",
					hwsPg, hwsPHi, hwsPLo, hwsPLo & 1, (hwsPLo >> 3) & 1);
			} else {
				SYSLOG("ngreen", "HANGCHECK V204: HWS_PGA=0 (status page NOT set up!)");
			}
			{
				uint32_t rStart = NGreen::callback->readReg32(RING_START(RENDER_RING_BASE));
				uint32_t rHead  = NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE));
				if (rStart) {
					uint32_t rPg  = rStart >> 12;
					uint32_t rPLo = NGreen::callback->readReg32(GGTT_PTE_LO(rPg));
					uint32_t rPHi = NGreen::callback->readReg32(GGTT_PTE_HI(rPg));
					SYSLOG("ngreen", "HANGCHECK V204: RING_START PTE[page 0x%x]=%08x:%08x present=%d llc=%d",
						rPg, rPHi, rPLo, rPLo & 1, (rPLo >> 3) & 1);
					// Dump the preamble ring: HEAD bytes already executed — these are the
					// exact commands Apple submitted. IPEHR=last command pipelined.
					NGreen::callback->setApertureIfNecessary();
					if (NGreen::callback->aperturePtr && NGreen::callback->apertureLen >= 0x2000 && (rPLo & 1)) {
						volatile uint32_t *ap = NGreen::callback->aperturePtr;
						uint32_t saveLo = NGreen::callback->readReg32(GGTT_PTE_LO(0));
						uint32_t saveHi = NGreen::callback->readReg32(GGTT_PTE_HI(0));
						NGreen::callback->writeReg32(GGTT_PTE_LO(0), rPLo);
						NGreen::callback->writeReg32(GGTT_PTE_HI(0), rPHi);
						NGreen::callback->writeReg32(0x101008, 0x1);
						uint32_t dumpDW = (rHead + 0x3F) / 4;  // round HEAD up to nearest 16 DW
						if (dumpDW < 16) dumpDW = 16;
						if (dumpDW > 64) dumpDW = 64;
						SYSLOG("ngreen", "HANGCHECK V204: preamble ring [0..0x%x] (HEAD=0x%x IPEHR=0x%x):",
							dumpDW*4 - 1, rHead,
							NGreen::callback->readReg32(RING_IPEHR(RENDER_RING_BASE)));
						for (uint32_t i = 0; i < dumpDW; i += 4)
							SYSLOG("ngreen", "HANGCHECK V204: ring[%02d-%02d] +0x%02x: %08x %08x %08x %08x",
								i, i+3, i*4, ap[i], ap[i+1], ap[i+2], ap[i+3]);
						NGreen::callback->writeReg32(GGTT_PTE_LO(0), saveLo);
						NGreen::callback->writeReg32(GGTT_PTE_HI(0), saveHi);
						NGreen::callback->writeReg32(0x101008, 0x1);
					}
				}
			}
			// V205: Dump LRCA via aperture remap — no IOMappedRead32, no PTE range issue.
			// Temporarily point GGTT page 0 at each LRCA page, read via aperturePtr[0],
			// then restore. GPU is hung so clobbering GGTT[0] momentarily is safe.
			if (lrcaGgtt) {
				uint32_t lPg0 = lrcaGgtt >> 12;
				uint32_t lPg1 = lPg0 + 1;
				uint32_t p0Lo = NGreen::callback->readReg32(GGTT_PTE_LO(lPg0));
				uint32_t p0Hi = NGreen::callback->readReg32(GGTT_PTE_HI(lPg0));
				uint32_t p1Lo = NGreen::callback->readReg32(GGTT_PTE_LO(lPg1));
				uint32_t p1Hi = NGreen::callback->readReg32(GGTT_PTE_HI(lPg1));
				SYSLOG("ngreen", "HANGCHECK V205: LRCA ggtt=0x%x pg0 PTE=%08x:%08x present=%d | pg1 PTE=%08x:%08x present=%d",
					lrcaGgtt, p0Hi, p0Lo, p0Lo & 1, p1Hi, p1Lo, p1Lo & 1);
				NGreen::callback->setApertureIfNecessary();
				if (NGreen::callback->aperturePtr && NGreen::callback->apertureLen >= 0x2000 &&
				    (p0Lo & 1) && (p1Lo & 1))
				{
					volatile uint32_t *ap = NGreen::callback->aperturePtr;
					uint32_t saveLo = NGreen::callback->readReg32(GGTT_PTE_LO(0));
					uint32_t saveHi = NGreen::callback->readReg32(GGTT_PTE_HI(0));
					// Remap GGTT[0] → LRCA page0 (PPHWSP)
					NGreen::callback->writeReg32(GGTT_PTE_LO(0), p0Lo);
					NGreen::callback->writeReg32(GGTT_PTE_HI(0), p0Hi);
					NGreen::callback->writeReg32(0x101008, 0x1);
					SYSLOG("ngreen", "HANGCHECK V205: LRCA page0 (PPHWSP) DW0..31:");
					for (int i = 0; i < 8; i++)
						SYSLOG("ngreen", "HANGCHECK V205: LRCA[%02d-%02d] %08x %08x %08x %08x",
							i*4, i*4+3, ap[i*4], ap[i*4+1], ap[i*4+2], ap[i*4+3]);
					// Remap GGTT[0] → LRCA page1 (ctx-reg-image)
					NGreen::callback->writeReg32(GGTT_PTE_LO(0), p1Lo);
					NGreen::callback->writeReg32(GGTT_PTE_HI(0), p1Hi);
					NGreen::callback->writeReg32(0x101008, 0x1);
					SYSLOG("ngreen", "HANGCHECK V205: LRCA page1 (ctx-reg-image) DW0..63:");
					for (int i = 0; i < 16; i++)
						SYSLOG("ngreen", "HANGCHECK V205: LRCA+1K[%02d-%02d] %08x %08x %08x %08x",
							i*4, i*4+3, ap[i*4], ap[i*4+1], ap[i*4+2], ap[i*4+3]);
					// Restore GGTT[0]
					NGreen::callback->writeReg32(GGTT_PTE_LO(0), saveLo);
					NGreen::callback->writeReg32(GGTT_PTE_HI(0), saveHi);
					NGreen::callback->writeReg32(0x101008, 0x1);
				} else {
					SYSLOG("ngreen", "HANGCHECK V205: remap skipped aper=%s len=0x%llx p0=%d p1=%d",
						NGreen::callback->aperturePtr ? "ok" : "null",
						(unsigned long long)NGreen::callback->apertureLen,
						p0Lo & 1, p1Lo & 1);
				}
			}
		}

		// V206: Dump preamble stamp target pages (GPU addr 0x40000000 / 0x40001200).
		// PC1 writes 0 → GGTT[0x40000]+0, PC2 writes stamp value 2 → GGTT[0x40001]+0x200.
		// If either PTE is invalid the PIPE_CONTROL write silently drops and
		// fwWaitForHardwareRegisterValue polls forever.
		{
			NGreen::callback->setApertureIfNecessary();
			auto *apcb = NGreen::callback;
			if (apcb->aperturePtr && apcb->apertureLen >= 0x1000) {
				volatile uint32_t *ap = apcb->aperturePtr;
				uint32_t saveLo = apcb->readReg32(GGTT_PTE_LO(0));
				uint32_t saveHi = apcb->readReg32(GGTT_PTE_HI(0));

				// PC1 clear target: GGTT[0x40000]
				uint32_t pc1Lo = apcb->readReg32(GGTT_PTE_LO(0x40000));
				uint32_t pc1Hi = apcb->readReg32(GGTT_PTE_HI(0x40000));
				SYSLOG("ngreen", "HANGCHECK V206: stamp PC1 GGTT[0x40000] PTE=%08x:%08x present=%d llc=%d",
					pc1Hi, pc1Lo, pc1Lo & 1, (pc1Lo >> 3) & 1);
				if (pc1Lo & 1) {
					asm volatile("wbinvd" ::: "memory");
					apcb->writeReg32(GGTT_PTE_LO(0), pc1Lo);
					apcb->writeReg32(GGTT_PTE_HI(0), pc1Hi);
					apcb->writeReg32(0x101008, 0x1);
					SYSLOG("ngreen", "HANGCHECK V206: GGTT[0x40000] DW[0..3]=%08x %08x %08x %08x",
						ap[0], ap[1], ap[2], ap[3]);
				}

				// PC2 stamp target: GGTT[0x40001] + 0x200 = DW[0x80]
				uint32_t pc2Lo = apcb->readReg32(GGTT_PTE_LO(0x40001));
				uint32_t pc2Hi = apcb->readReg32(GGTT_PTE_HI(0x40001));
				SYSLOG("ngreen", "HANGCHECK V206: stamp PC2 GGTT[0x40001] PTE=%08x:%08x present=%d llc=%d",
					pc2Hi, pc2Lo, pc2Lo & 1, (pc2Lo >> 3) & 1);
				if (pc2Lo & 1) {
					asm volatile("wbinvd" ::: "memory");
					apcb->writeReg32(GGTT_PTE_LO(0), pc2Lo);
					apcb->writeReg32(GGTT_PTE_HI(0), pc2Hi);
					apcb->writeReg32(0x101008, 0x1);
					// DW[0x80] = offset 0x200 = the stamp value written by PC2
					SYSLOG("ngreen", "HANGCHECK V206: GGTT[0x40001] DW[0x7e..0x81]=%08x %08x %08x %08x (stamp@DW[0x80]=%08x)",
						ap[0x7e], ap[0x7f], ap[0x80], ap[0x81], ap[0x80]);
				}

				apcb->writeReg32(GGTT_PTE_LO(0), saveLo);
				apcb->writeReg32(GGTT_PTE_HI(0), saveHi);
				apcb->writeReg32(0x101008, 0x1);
			} else {
				SYSLOG("ngreen", "HANGCHECK V206: aperture unavailable — skipping stamp target dump");
			}
		}

		// BCS ring state (with proper Blitter ForceWake held!)
		SYSLOG("ngreen", "HANGCHECK BCS HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x",
			NGreen::callback->readReg32(RING_HEAD(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_TAIL(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_CTL(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_START(BLT_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK BCS HWS_PGA=0x%x ACTHD=0x%x:%08x",
			NGreen::callback->readReg32(RING_HWS_PGA(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_ACTHD_UDW(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_ACTHD(BLT_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK BCS IPEHR=0x%x INSTDONE=0x%x",
			NGreen::callback->readReg32(RING_IPEHR(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_INSTDONE(BLT_RING_BASE)));
		SYSLOG("ngreen", "HANGCHECK BCS EIR=0x%x ESR=0x%x EMR=0x%x",
			NGreen::callback->readReg32(RING_EIR(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_ESR(BLT_RING_BASE)),
			NGreen::callback->readReg32(RING_EMR(BLT_RING_BASE)));
		
		// Global error state
		SYSLOG("ngreen", "HANGCHECK ERROR_GEN6=0x%x RING_FAULT=0x%x",
			NGreen::callback->readReg32(0x40A0),
			NGreen::callback->readReg32(0xCEC4));
		SYSLOG("ngreen", "HANGCHECK FAULT_TLB0=0x%x TLB1=0x%x",
			NGreen::callback->readReg32(0x4B10),
			NGreen::callback->readReg32(0x4B14));
		SYSLOG("ngreen", "HANGCHECK GT_INTR_DW0=0x%x DW1=0x%x",
			NGreen::callback->readReg32(0x190018),
			NGreen::callback->readReg32(0x19001C));
		
		// V47: RING_TIMESTAMP — check if GPU engines have processed any commands
		// Timestamp increments with GPU clock whenever the engine is active.
		// If it's 0 or unchanged from start, the engine never executed anything.
		SYSLOG("ngreen", "HANGCHECK V47: RCS RING_TIMESTAMP=0x%x BCS RING_TIMESTAMP=0x%x",
			NGreen::callback->readReg32(RENDER_RING_BASE + 0x358),
			NGreen::callback->readReg32(BLT_RING_BASE + 0x358));
		
		// V47: Re-check GFX_MSTR_IRQ — did someone disable master interrupt since start?
		uint32_t hcMstr = NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ);
		SYSLOG("ngreen", "HANGCHECK V47: GFX_MSTR_IRQ=0x%x (bit31=%d)", hcMstr, !!(hcMstr & GEN11_MASTER_IRQ));
		if (!(hcMstr & GEN11_MASTER_IRQ)) {
			SYSLOG("ngreen", "HANGCHECK V47: Master IRQ disabled! Re-enabling...");
			NGreen::callback->writeReg32(GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
		}

		// V202: GGTT PTE verification — ForceWake is held so reads are reliable.
		// GGTT[0] (stolen memory base) MUST have a valid PTE — if it shows 0 here
		// then either stolen memory was never mapped (fatal) or the base address is wrong.
		// PLANE_SURF PTE reveals whether the display engine can actually DMA the surface.
		{
			uint32_t pte0Lo = NGreen::callback->readReg32(GGTT_PTE_LO(0));
			uint32_t pte0Hi = NGreen::callback->readReg32(GGTT_PTE_HI(0));
			SYSLOG("ngreen", "HANGCHECK V202: GGTT[0] (stolen base) PTE=%08x:%08x present=%d llc=%d",
				pte0Hi, pte0Lo, pte0Lo & 1, (pte0Lo >> 3) & 1);

			// Display registers don't need GT ForceWake — read live PLANE_SURF.
			uint32_t hcSurf = NGreen::callback->readReg32(0x7019C);
			if (hcSurf) {
				uint32_t surfPg  = hcSurf >> 12;
				uint32_t surfPLo = NGreen::callback->readReg32(GGTT_PTE_LO(surfPg));
				uint32_t surfPHi = NGreen::callback->readReg32(GGTT_PTE_HI(surfPg));
				SYSLOG("ngreen", "HANGCHECK V202: PLANE_SURF=0x%x (page 0x%x) PTE=%08x:%08x present=%d llc=%d",
					hcSurf, surfPg, surfPHi, surfPLo, surfPLo & 1, (surfPLo >> 3) & 1);
			} else {
				SYSLOG("ngreen", "HANGCHECK V202: PLANE_SURF=0 (display armed to null)");
			}
		}

		// Release ForceWake
		NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 0);
		NGreen::callback->writeReg32(FORCEWAKE_BLITTER_GEN9, (1 << 16) | 0);
		
		// V42: Enumerate accelerator children at hangcheck time
		if (callback->accelInstance) {
			auto *service = static_cast<IOService *>(callback->accelInstance);
			OSIterator *iter = service->getClientIterator();
			int childCount = 0;
			if (iter) {
				OSObject *obj;
				while ((obj = iter->getNextObject())) {
					auto *child = OSDynamicCast(IOService, obj);
					if (child) {
						SYSLOG("ngreen", "HANGCHECK child[%d]: %s (busy=%d)", childCount,
							child->getName(), child->getBusyState());
						// Check if child has its own children (user clients)
						OSIterator *iter2 = child->getClientIterator();
						if (iter2) {
							int ucCount = 0;
							OSObject *obj2;
							while ((obj2 = iter2->getNextObject())) {
								auto *uc = OSDynamicCast(IOService, obj2);
								if (uc) {
									SYSLOG("ngreen", "HANGCHECK   child[%d].uc[%d]: %s", childCount, ucCount, uc->getName());
									ucCount++;
								}
							}
							iter2->release();
						}
						childCount++;
					}
				}
				iter->release();
			}
			SYSLOG("ngreen", "HANGCHECK accelerator has %d children total", childCount);
			
			// V45: Log IOService state at hangcheck time
			uint64_t hcState = service->getState();
			bool hcOpen = service->isOpen();
			SYSLOG("ngreen", "HANGCHECK V45: state=0x%llx (reg=%d match=%d pub=%d fmatch=%d inact=%d) isOpen=%d",
				   (unsigned long long)hcState,
				   !!(hcState & 0x02), !!(hcState & 0x04), !!(hcState & 0x08),
				   !!(hcState & 0x10), !!(hcState & 0x01), hcOpen);
		}
		
		// V42: Check interrupt state — did GT_INTR_DW0 re-assert since we cleared it?
		SYSLOG("ngreen", "HANGCHECK IRQ: RENDER_COPY_INTR_EN=0x%x RCS0_RSVD_MASK=0x%x",
			NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE),
			NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK));
		SYSLOG("ngreen", "HANGCHECK IRQ: GFX_MSTR_IRQ=0x%x",
			NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ));
		
		SYSLOG("ngreen", "=== HANGCHECK: dump complete ===");
	}
	
	// ctx 2: IRQ, ctx 1: normal
	uint32_t ack_exp = set << ctx;
	uint32_t mask = 1 << ctx;
	uint32_t wr = ack_exp | (1 << ctx << 16);

	// xe/fwref (19/10): contagem de referências por (domínio, ctx), como o SafeForceWakeMultithreaded da Apple.
	// Sem ela, o "soltar" de um usuário desligava o forcewake de outro no meio do trabalho: a escrita da execlist do
	// RCS, logo depois de o motor ficar ocioso (RC6), se perdia e o escalonador 5 esperava para sempre.
	// Só escreve o registrador nas transições 0->1 e 1->0. Desliga com -ngreennofwref.
	bool fwJaEscrito = false;
	static int fwRelease = -1;
	if (fwRelease < 0) fwRelease = checkKernelArgument("-ngreenfwhold") ? 0 : 1;  // 19/10: segurar sempre quebrou o boot
	static int fwRefArg = -1;
	if (fwRefArg < 0) fwRefArg = (checkKernelArgument("-ngreenxefix") && !checkKernelArgument("-ngreennofwref")) ? 1 : 0;
	static IOSimpleLock *fwLock = nullptr;
	static uint32_t fwRef[8][4];
	if (fwRefArg == 1 && !fwLock) {
		IOSimpleLock *l = IOSimpleLockAlloc();
		if (l && !__sync_bool_compare_and_swap(&fwLock, (IOSimpleLock *)nullptr, l)) IOSimpleLockFree(l);
	}
	if (fwRefArg == 1 && fwLock && ctx < 4) {
		uint32_t mudar = 0;
		// 19/10: o forcewake também é chamado em interrupção primária (panic "hwlock ... recursively" com o lock
		// comum): desliga as interrupções enquanto segura o lock.
		IOInterruptState fwIs = IOSimpleLockLockDisableInterrupt(fwLock);
		for (unsigned d = DOM_FIRST, i = 0; d <= DOM_LAST && i < 8; d <<= 1, i++) {
			if (!(dom & d)) continue;
			if (set) {
				if (fwRef[i][ctx]++ == 0) mudar |= d;
			} else if (fwRef[i][ctx] > 0) {
				// xe/fwhold (19/10): ao zerar, NÃO solta (o motor ficava em RC6 e a execlist escrita logo depois do
				// fim do CSB se perdia). Mantém o forcewake ligado; -ngreenfwrelease volta a soltar.
				if (--fwRef[i][ctx] == 0 && fwRelease) mudar |= d;
			}
		}
		// a requisição é escrita ainda dentro do lock (a ordem das escritas segue a dos contadores); só o ACK é esperado fora
		for (unsigned d = DOM_FIRST; d <= DOM_LAST; d <<= 1) {
			if (!(mudar & d)) continue;
			// 19/10: escrita direta no MMIO dentro da trava. wrapWriteRegister32 pode chamar o forcewake de novo
			// (recursão -> panic "spinlock acquisition timeout, owner: current_thread").
			if (d == DOM_MEDIA) {
				for (const auto &eng : fwMediaEngines) NGreen::callback->writeReg32(eng.req, wr);
			} else {
				NGreen::callback->writeReg32(fwReqReg(d), wr);
			}
		}
		IOSimpleLockUnlockEnableInterrupt(fwLock, fwIs);
		dom = mudar;
		if (!dom) return;
		fwJaEscrito = true;
	}

	for (unsigned d = DOM_FIRST; d <= DOM_LAST; d <<= 1) {
		if (!(dom & d)) continue;

		if (d == DOM_MEDIA) {
			// Gen12+: Media uses per-engine ForceWake (VDBOX + VEBOX), NOT Gen9 single register
			for (const auto &eng : fwMediaEngines) {
				if (!fwJaEscrito) wrapWriteRegister32(callback->framecont, eng.req, wr);
				IOPause(100);
				if (!pollRegister(eng.ack, ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS) &&
					!forceWakeWaitAckFallback(eng.req, eng.ack, ack_exp, mask) &&
					!pollRegister(eng.ack, ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS))
					xeEv('FWTO', eng.req, dom, ack_exp);  // 19/10: sem SYSLOG (pode estar em interrupção -> deadlock no lock do log)
				else
					{ /* xe (19/10): "ForceWake OK" desligado, enchia o dmesg */ }
			}
		} else {
			if (!fwJaEscrito) wrapWriteRegister32(callback->framecont, fwReqReg(d), wr);
			IOPause(100);
			if (!pollRegister(fwAckReg(d), ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS) &&
				!forceWakeWaitAckFallback(fwReqReg(d), fwAckReg(d), ack_exp, mask) &&
				!pollRegister(fwAckReg(d), ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS))
				xeEv('FWTO', fwReqReg(d), dom, ack_exp);  // 19/10: sem SYSLOG (idem)
			else
				/* xe (19/10): spam que enchia o dmesg */ if (0) DBGLOG("ngreen", "ForceWake OK domain=%s set=%d ack=0x%x", strForDom(d), set, wrapReadRegister32(callback->framecont, fwAckReg(d)));
		}
	}
	// V61: silenced — see above
}

bool Gen11::pollRegister(uint32_t reg, uint32_t val, uint32_t mask, uint32_t timeout) {
    uint64_t now = 0, deadline = 0;
    clock_interval_to_deadline(timeout, kMillisecondScale, &deadline);
    for (clock_get_uptime(&now); now < deadline; clock_get_uptime(&now)) {
        auto rd = wrapReadRegister32(callback->framecont, reg);
        if ((rd & mask) == val)
            return true;
    }
    return false;
}

bool Gen11::forceWakeWaitAckFallback(uint32_t reqReg, uint32_t ackReg, uint32_t val, uint32_t mask) {
	unsigned pass = 1;
	bool ack = false;
	auto controller = callback->framecont;
	
	do {
		pollRegister(ackReg, 0, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_ACK_TIMEOUT_MS);
		wrapWriteRegister32(controller, reqReg, fw_set(FORCEWAKE_KERNEL_FALLBACK));
		
		IODelay(10 * pass);
		pollRegister(ackReg, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_ACK_TIMEOUT_MS);

		ack = (wrapReadRegister32(controller, ackReg) & mask) == val;

		wrapWriteRegister32(controller, reqReg, fw_clear(FORCEWAKE_KERNEL_FALLBACK));
	} while (!ack && pass++ < 10);
	
	return ack;
}

void Gen11::releaseDoorbell()
{}

bool Gen11::dotrue()
{
	return true;
}

int iniin=1;
void  Gen11::readAndClearInterrupts(AppleIntel::AppleIntelBaseController *that, void *param_1)
{
	
	if (iniin){
		iniin=0;
		SYSLOG("ngreen", "readAndClearInterrupts: first call — initializing Gen11 GT interrupts");
		
		wrapWriteRegister32(callback->framecont, GEN11_GFX_MSTR_IRQ, 0);
		wrapWriteRegister32(callback->framecont,GEN11_DISPLAY_INT_CTL, 0);
		uint32_t master_ctl=wrapReadRegister32(callback->framecont,GEN11_GFX_MSTR_IRQ);
		
		//Disable RCS, BCS, VCS and VECS class engines.
		wrapWriteRegister32(callback->framecont, GEN11_RENDER_COPY_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_VCS_VECS_INTR_ENABLE,	  0);
		
		// Restore masks irqs on RCS, BCS, VCS and VECS engines.
		wrapWriteRegister32(callback->framecont, GEN11_RCS0_RSVD_INTR_MASK,	~0);
		wrapWriteRegister32(callback->framecont, GEN11_BCS_RSVD_INTR_MASK,	~0);
		wrapWriteRegister32(callback->framecont, GEN11_VCS0_VCS1_INTR_MASK,	~0);
		wrapWriteRegister32(callback->framecont, GEN11_VCS2_VCS3_INTR_MASK,	~0);
		wrapWriteRegister32(callback->framecont, GEN11_VECS0_VECS1_INTR_MASK,	~0);
		
		wrapWriteRegister32(callback->framecont, GEN11_GPM_WGBOXPERF_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_GPM_WGBOXPERF_INTR_MASK,  ~0);
		wrapWriteRegister32(callback->framecont, GEN11_GUC_SG_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_GUC_SG_INTR_MASK,  ~0);
		
		
		
		uint32_t irqs = GT_RENDER_USER_INTERRUPT;
		uint32_t guc_mask = /*intel_uc_wants_guc(&gt->uc) ? GUC_INTR_GUC2HOST :*/ 0;
		uint32_t gsc_mask = 0;
		uint32_t heci_mask = 0;
		uint32_t dmask;
		uint32_t smask;
		
		irqs |= GT_CS_MASTER_ERROR_INTERRUPT |
		GT_CONTEXT_SWITCH_INTERRUPT |
		GT_WAIT_SEMAPHORE_INTERRUPT;
		
		dmask = irqs << 16 | irqs;
		smask = irqs << 16;
		
		
		/* Enable RCS, BCS, VCS and VECS class interrupts. */
		wrapWriteRegister32(callback->framecont, GEN11_RENDER_COPY_INTR_ENABLE, dmask);
		wrapWriteRegister32(callback->framecont, GEN11_VCS_VECS_INTR_ENABLE, dmask);
		
		/* Unmask irqs on RCS, BCS, VCS and VECS engines. */
		wrapWriteRegister32(callback->framecont, GEN11_RCS0_RSVD_INTR_MASK, ~smask);
		wrapWriteRegister32(callback->framecont, GEN11_BCS_RSVD_INTR_MASK, ~smask);
		wrapWriteRegister32(callback->framecont, GEN11_VCS0_VCS1_INTR_MASK, ~dmask);
		wrapWriteRegister32(callback->framecont, GEN11_VCS2_VCS3_INTR_MASK, ~dmask);
		wrapWriteRegister32(callback->framecont, GEN11_VECS0_VECS1_INTR_MASK, ~dmask);
		
		/*
		 * RPS interrupts will get enabled/disabled on demand when RPS itself
		 * is enabled/disabled.
		 */
		
		
		wrapWriteRegister32(callback->framecont, GEN11_GPM_WGBOXPERF_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_GPM_WGBOXPERF_INTR_MASK,  ~0);
		
		/* Same thing for GuC interrupts */
		wrapWriteRegister32(callback->framecont, GEN11_GUC_SG_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_GUC_SG_INTR_MASK,  ~0);
		
		wrapWriteRegister32(callback->framecont,GEN11_DISPLAY_INT_CTL, GEN11_DISPLAY_IRQ_ENABLE);
		wrapWriteRegister32(callback->framecont, GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
	}
	
	
	FunctionCast(readAndClearInterrupts, callback->oreadAndClearInterrupts)(that,param_1);
}

void * Gen11::serviceInterrupts(void *param_1)
{
	return FunctionCast(serviceInterrupts, callback->oserviceInterrupts)(param_1);
	
}

void * Gen11::wprobe(void *that,void *param_1,int *param_2)
{
	//FunctionCast(wprobe, callback->owprobe)(that, param_1,param_2);
	//logStateInRegistry(that,0x56);
	initializeLogging(reinterpret_cast<AppleIntel::AppleIntelBaseController *>(that));
	return that;
	
}

void *contr;
bool  Gen11::tgstart(void *that,void *param_1)
{
	contr=that;
	FunctionCast(tgstart, callback->otgstart)(that, param_1);
	return true;
	
}

void Gen11::FBMemMgr_Init(void *that)
{
	ccont  = getMember<void *>(that, 0xc40);  // MMIO register access manager
	ccont2 = that;                             // AppleIntelBaseController itself

	FunctionCast(FBMemMgr_Init, callback->oFBMemMgr_Init)(that);
	
	

	/*IODeviceMemory * m= NGreen::callback->iGPU->getDeviceMemoryWithIndex(0);
	IODeviceMemory *dm;
	m->withSubRange(dm,0x4180000,0x12000);//fDSBBufferBytes = 73728, fDSBBufferBaseOffset = 68681728
	IOMemoryMap *dsb=dm->map();
	
	IODeviceMemory *dm2;
	m->withSubRange(dm2,0x4192000,0x3000);//fConnectionStatusBytes = 12288, fConnectionStatusOffset = 68755456
	IOMemoryMap *dsb2=dm2->map();*/
	
}

uint32_t Gen11::probePortMode()
{
	auto ret=FunctionCast(probePortMode, callback->oprobePortMode)();
	return ret;
};


uint32_t Gen11::wdepthFromAttribute(void *that,uint param_1)
{
	return 0x1e;
};

uint32_t Gen11::raReadRegister32(void *that,unsigned long param_1)
{
	if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return NGreen::callback->readReg32(param_1);
	
	// V60: ReadRegister32 intercept REMOVED — V59 proved hooking all register reads
	// causes display driver to loop in power-well cycling, producing 0 children.
	// Error suppression done via timer-based R/W clear (V57 approach).
	
	auto ret=FunctionCast(raReadRegister32, callback->oraReadRegister32)(that,param_1);
	return ret;
};

unsigned long Gen11::raReadRegister32b(void *that,void *param_1,unsigned long param_2)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return 0;
	//if (reinterpret_cast<volatile uint64_t*>(param_1)==nullptr) return 0;
	return  raReadRegister32(that,reinterpret_cast<uint64_t>(param_1) + param_2);
};

uint64_t Gen11::raReadRegister64(void *that,unsigned long param_1)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return 0;
	
	return FunctionCast(raReadRegister64, callback->oraReadRegister64)(that,param_1);
};
uint64_t Gen11::raReadRegister64b(void *that,void *param_1,unsigned long param_2)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return 0;
	return  raReadRegister64(that,reinterpret_cast<uint64_t>(param_1) + param_2);
};

void Gen11::radWriteRegister32(void *that,unsigned long param_1, UInt32 param_2)
{
	radWriteRegister32f( that,param_1,param_2);
};

void Gen11::radWriteRegister32f(void *that,unsigned long param_1, UInt32 param_2)
{
	//FunctionCast(radWriteRegister32f, callback->oradWriteRegister32f)( that,param_1,param_2);
};

void Gen11::raWriteRegister64(void *that,unsigned long param_1,UInt64 param_2)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return;

	if (NGreen::callback) {
		const uint32_t reg = static_cast<uint32_t>(param_1 & 0xFFFFF);
		const bool looksLikePlaneSurf =
			(reg >= 0x60000 && reg <= 0xBFFFF) &&
			((reg & 0xFFF) == 0x19C);

		if (looksLikePlaneSurf && param_2 == 0) {
			const uint32_t ctlReg = reg - 0x1C;
			const uint32_t planCtl = NGreen::callback->readReg32(ctlReg);
			if (planCtl & 0x80000000U) {
				const uint32_t currentSurf = NGreen::callback->readReg32(reg);
				if (currentSurf != 0) {
					param_2 = static_cast<UInt64>(currentSurf);
				} else {
					NGreen::callback->writeReg32(ctlReg, planCtl & ~0x80000000U);
				}
			}
		}
	}

	FunctionCast(raWriteRegister64, callback->oraWriteRegister64)(that, param_1, param_2);
};

void Gen11::raWriteRegister64b(void *that,void *param_1,unsigned long param_2,UInt64 param_3)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return;
	raWriteRegister64( that,reinterpret_cast<uint64_t>(param_1) + param_2,param_3);
};

void Gen11::setupPlanarSurfaceDBUF()
{
	//FunctionCast(setupPlanarSurfaceDBUF, callback->osetupPlanarSurfaceDBUF)();
};

void Gen11::updateDBUF(void *that,uint param_1,uint param_2,bool param_3)
{
	//setupPlanarSurfaceDBUF();
};

int Gen11::LightUpEDP(void *that,void *param_1, void *param_2,void *param_3)
{
	FunctionCast(LightUpEDP, callback->oLightUpEDP)(that,param_1,param_2,param_3);
	return 0;
};

uint8_t Gen11::disableVDDForAux(void *that)
{
	uint32_t iVar3 = raReadRegister32(ccont,0xc7200);
	if (-1 < iVar3) {
		IORegistryEntry *r= (IORegistryEntry *)getMember<long *>(that, 0xd60);
		r->setProperty("AAPL,LCD-PowerState-ON", false);
	}
	return FunctionCast(disableVDDForAux, callback->odisableVDDForAux)(that);
};

void Gen11::setPanelPowerState(void *that,bool param_1)
{
	FunctionCast(setPanelPowerState, callback->osetPanelPowerState)(that,param_1);
	
	IORegistryEntry *r= (IORegistryEntry *)getMember<long *>(that, 0xd60);
	r->setProperty("AAPL,LCD-PowerState-ON", param_1);
};

unsigned long Gen11::fastLinkTraining()
{
	
	FunctionCast(fastLinkTraining, callback->ofastLinkTraining)();
	return 1;
};

void Gen11::logStateInRegistry(void *that,uint param_1)
{
 FunctionCast(logStateInRegistry, callback->ologStateInRegistry)(that,param_1 );
}

void Gen11::initializeLogging(AppleIntel::AppleIntelBaseController *that)
{
	FunctionCast(initializeLogging, callback->oinitializeLogging)(that );
}

int Gen11::getPlatformID()
{
 return FunctionCast(getPlatformID, callback->ogetPlatformID)( );
}

uint32_t Gen11::tprobePortMode(void * that)
{
 return Genx::callback->tprobePortMode(that );
}

void  Gen11::AppleIntelPlanec1(AppleIntel::AppleIntelPlane *that)
{
	Genx::callback->AppleIntelPlanec1(that );
}

void  Gen11::AppleIntelScalerc1(AppleIntel::AppleIntelScaler *that)
{
	Genx::callback->AppleIntelScalerc1(that );
}

void * Gen11::AppleIntelScalernew(unsigned long param_1)
{
	return Genx::callback->AppleIntelScalernew(param_1 );
}

void * Gen11::AppleIntelPlanenew(unsigned long param_1)
{
	return Genx::callback->AppleIntelPlanenew(param_1 );
}

void Gen11::uupdateDBUF(void *that,uint param_1,uint param_2,bool param_3)
{
	Genx::callback->uupdateDBUF(that,param_1,param_2 );
}

bool inpwell=false;
void Gen11::PowerWellinit(void *that,void *param_1)
{
	inpwell=true;
  FunctionCast(PowerWellinit, callback->oPowerWellinit)(that,param_1 );
	inpwell=false;
}

long Gen11::getPortByDDI(uint param_1)
{
 auto ret= FunctionCast(getPortByDDI, callback->ogetPortByDDI)(param_1 );
	if (inpwell) setPortMode((void*)ret,1);
 return ret;
}

uint8_t  Gen11::setPortMode(void *that,uint32_t param_1)
{
 auto ret= FunctionCast(setPortMode, callback->osetPortMode)(that,param_1 );
	
 return ret;
}

int smo=0;

int Gen11::isConflictRegister()
{
	return -1;

}

int Gen11::wrapGetFreeJoinablePathCount(void *that)
{
	// Always return 0 — with 1/1/1 pinfo config, only pipe 0 has a path. Original
	// would call getPathByPipe(1) and (2), get NULL back, dereference [+0x3644] →
	// kernel panic at setupBootDisplay->allocateBootDisplayResources chain.
	// Returning 0 means "no joinable paths" which is correct for single-pipe.
	static int v210Count = 0;
	if (v210Count < 4) {
		v210Count++;
		SYSLOG("ngreen", "V210[%d]: getFreeJoinablePathCount → 0 (1/1/1 single-pipe safe)", v210Count);
	}
	return 0;
}

int Gen11::wrapEnableController(void *that)
{
	uint32_t fbId = getMember<uint32_t>(that, 0x1DC);
	if (fbId != 0) {
		// Short-circuit for phantom FB1/FB2: don't call original, return success
		// without doing the per-fb setup (gController ref-count bumps,
		// AppleIntelBaseController::enableController, AAPL0n,IgnoreConnection
		// property lookup, etc). Without this, the per-fb setup chain leaves
		// fb1/fb2 partially-initialized → IGAccelDisplayPipe iterates them in
		// displayModeDidChange → IOFramebuffer::getAttributeExt+0x25 NULL deref.
		// Returning 0 (kIOReturnSuccess) so callers don't error-handle.
		SYSLOG("ngreen", "V209: enableController short-circuited for fb=%u", fbId);
		return 0;
	}
	int ret = FunctionCast(wrapEnableController, callback->oEnableController)(that);
	SYSLOG("ngreen", "V209: enableController fb=0 ret=0x%x", ret);
	return ret;
}

bool Gen11::wrapAppleIntelFramebufferStart(void *that, void *provider)
{
	uint32_t fbId = getMember<uint32_t>(that, 0x1DC);
	if (fbId != 0) {
		// Refuse to start phantom FB1/FB2 — they have no display path assigned and
		// trip CoreDisplay_CreateDisplayForCGXDisplayDevice's __assert_rtn during WS init.
		// Returning false from start() means IOService::registerService is never called,
		// so CoreDisplay never sees this FB. Apple's controller still has 3 FBs internally
		// (per pinfo counts) — we just block IOFramebufferShared registration for !FB0.
		SYSLOG("ngreen", "V208: AppleIntelFramebuffer::start refused for fb=%u (only FB0 starts)",
		       fbId);
		return false;
	}
	bool ret = FunctionCast(wrapAppleIntelFramebufferStart, callback->oAppleIntelFramebufferStart)(that, provider);
	SYSLOG("ngreen", "V208: AppleIntelFramebuffer::start fb=0 ret=%d", ret);
	return ret;
}

void Gen11::injectAcceleratorPersonality(bool useTglNames)
{
	if (this->acceleratorPersonalityInjected) {
		DBGLOG("ngreen", "injectAcceleratorPersonality: already injected, skipping");
		return;
	}

	SYSLOG("ngreen", "injectAcceleratorPersonality: registering IntelAccelerator (%s) into IOCatalogue",
	       useTglNames ? "TGL" : "ICL");

	auto *dict = OSDictionary::withCapacity(24);
	if (!dict) return;

	const char *bundleId = useTglNames ? "com.xxxxx.driver.AppleIntelTGLGraphics" : "com.apple.driver.AppleIntelICLGraphics";
	const char *mtlName  = useTglNames ? "AppleIntelTGLGraphicsMTLDriver"        : "AppleIntelICLGraphicsMTLDriver";
	const char *glName   = useTglNames ? "AppleIntelTGLGraphicsGLDriver"         : "AppleIntelICLGraphicsGLDriver";
	const char *vaName   = useTglNames ? "AppleIntelTGLGraphicsVADriver"         : "AppleIntelICLGraphicsVADriver";

	// Basic matching properties
	auto *bi  = OSString::withCString(bundleId);
	auto *cls = OSString::withCString("IntelAccelerator");
	auto *mc  = OSString::withCString("IOAccelerator");
	auto *pv  = OSString::withCString("IOPCIDevice");
	auto *pcm = OSString::withCString("0x03000000&0xff000000");
	auto *ps  = OSNumber::withNumber(static_cast<unsigned long long>(1000), 32);

	dict->setObject("CFBundleIdentifier", bi);
	dict->setObject("IOClass", cls);
	dict->setObject("IOMatchCategory", mc);
	dict->setObject("IOProviderClass", pv);
	dict->setObject("IOPCIClassMatch", pcm);
	dict->setObject("IOProbeScore", ps);

	OSSafeReleaseNULL(bi);
	OSSafeReleaseNULL(cls);
	OSSafeReleaseNULL(mc);
	OSSafeReleaseNULL(pv);
	OSSafeReleaseNULL(pcm);
	OSSafeReleaseNULL(ps);

	// V44: GPU driver bundle names — required by IOAcceleratorFamily2 and WindowServer
	auto *mtl = OSString::withCString(mtlName);
	auto *gl  = OSString::withCString(glName);
	auto *dvd = OSString::withCString(vaName);
	auto *src = OSString::withCString("0.0.0.0.0");
	auto *vaCodec  = OSString::withCString("Gen10");
	auto *vaScaler = OSString::withCString("Gen10");
	auto *vaBGRA   = OSString::withCString("Gen10");
	auto *vaRendID = OSNumber::withNumber(static_cast<unsigned long long>(17301568), 32); // 0x1084000

	dict->setObject("MetalPluginName", mtl);
	dict->setObject("IOGLBundleName", gl);
	dict->setObject("IODVDBundleName", dvd);
	dict->setObject("IOSourceVersion", src);
	dict->setObject("IOGVACodec", vaCodec);
	dict->setObject("IOGVAScaler", vaScaler);
	dict->setObject("IOGVABGRAEnc", vaBGRA);
	dict->setObject("IOVARendererID", vaRendID);

	OSSafeReleaseNULL(mtl);
	OSSafeReleaseNULL(gl);
	OSSafeReleaseNULL(dvd);
	OSSafeReleaseNULL(src);
	OSSafeReleaseNULL(vaCodec);
	OSSafeReleaseNULL(vaScaler);
	OSSafeReleaseNULL(vaBGRA);
	OSSafeReleaseNULL(vaRendID);

	// IOAccelerator2D plugin type (required for 2D acceleration matching)
	auto *pluginDict = OSDictionary::withCapacity(1);
	if (pluginDict) {
		auto *pluginName = OSString::withCString("IOAccelerator2D.plugin");
		pluginDict->setObject("ACCF0000-0000-0000-0000-000a2789904e", pluginName);
		OSSafeReleaseNULL(pluginName);
		dict->setObject("IOCFPlugInTypes", pluginDict);
		pluginDict->release();
	}

	// V77: Display pipe capabilities forced to 0 to prevent WS GPU compositing crash.
	// xe/dpcaps (19/10): com o display pipe nativo (sem -ngreendp0) isso deixava TransactionsSupported=0 enquanto
	// o WindowServer usa o pipe: todo IOAccelDisplayPipeTransaction2::set_transaction_args voltava 0xe00002c7
	// (unsupported), nenhum quadro era apresentado e os stamps das filas de flip nunca fechavam (GPU restart).
	// Agora: modo nativo anuncia o mesmo que o Info.plist do TGL da Apple (as duas true); -ngreendp0 mantém 0.
	auto *dpCaps = OSDictionary::withCapacity(2);
	if (dpCaps) {
		const bool dpOff = isDisplayPipeForceDisabled();
		dpCaps->setObject("DisplayPipeSupported", dpOff ? kOSBooleanFalse : kOSBooleanTrue);
		dpCaps->setObject("TransactionsSupported", dpOff ? kOSBooleanFalse : kOSBooleanTrue);
		dict->setObject("IOAccelDisplayPipeCapabilities", dpCaps);
		dpCaps->release();
		SYSLOG("ngreen", "XE-DPCAPS: DisplayPipeSupported=%d TransactionsSupported=%d", !dpOff, !dpOff);
	}

	SYSLOG("ngreen", "injectAcceleratorPersonality: personality has %u properties", dict->getCount());

	auto *array = OSArray::withCapacity(1);
	if (array) {
		array->setObject(dict);
		if (gIOCatalogue) {
			bool ok = gIOCatalogue->addDrivers(array, true);
			SYSLOG("ngreen", "injectAcceleratorPersonality: addDrivers returned %d", ok);
			this->acceleratorPersonalityInjected = ok;
		} else {
			SYSLOG("ngreen", "injectAcceleratorPersonality: gIOCatalogue is null!");
		}
		array->release();
	}
	dict->release();
}

void  Gen11::disablePowerWellPG(AppleIntel::AppleIntelBaseController *that, uint param_1)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(disablePowerWellPG, callback->odisablePowerWellPG)(that, param_1);
}
void  Gen11::enablePowerWellPG(AppleIntel::AppleIntelBaseController *that, uint param_1)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(enablePowerWellPG, callback->oenablePowerWellPG)(that, param_1);
}

void Gen11::hwSetPowerWellStatePG(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStatePG, callback->ohwSetPowerWellStatePG)(that,param_1,param_2);
}

void Gen11::hwConfigureCustomAUX(AppleIntel::AppleIntelBaseController *that, bool param_1)
{
	SYSLOG("ngreen", "hwAUX p1=%d CE4=%d",
		(int)param_1, getMember<int>(that, 0xCE4));

	// Pure passthrough — V12 showed that the native "Custom AUX enable" logic works
	// correctly on ADL-P hardware. The 0x863xx PHY writes added in V12 broke EDID
	// (56283 µs failure). Native-only: EDID succeeded in 3663 µs on same hardware.
	if (callback->ohwConfigureCustomAUX)
		FunctionCast(hwConfigureCustomAUX, callback->ohwConfigureCustomAUX)(that, param_1);
}

int Gen11::hasExternalDispla()
{

	return 1;
}

long blti=0;

uint8_t Gen11::isPanelPowerOn()
{
	return 1;
}

uint8_t Gen11::SetupDPSSTTimings(void *that,void *param_1,void *param_2,void *param_3){
	return FunctionCast(SetupDPSSTTimings, callback->oSetupDPSSTTimings)(that,param_1,param_2,param_3);
}

uint32_t Gen11::validateDetailedTiming(void *that,void *param_1,unsigned long param_2) {
	return FunctionCast(validateDetailedTiming, callback->ovalidateDetailedTiming)(that,param_1,param_2);
}

uint8_t Gen11::maxSupportedDepths(void *param_1) {
    auto displayTimingInfo = const_cast<IODetailedTimingInformationV2 *>(reinterpret_cast<const IODetailedTimingInformationV2 *>(param_1));
    if (displayTimingInfo!=nullptr) displayTimingInfo->pixelClock = 785400000;
    auto ret= FunctionCast(maxSupportedDepths, callback->omaxSupportedDepths)(param_1);
	return ret;
}

uint8_t Gen11::validateModeDepth(void *that,void *param_1,uint param_2)
{
    auto displayTimingInfo = const_cast<IODetailedTimingInformationV2 *>(reinterpret_cast<const IODetailedTimingInformationV2 *>(param_1));
    if (displayTimingInfo!=nullptr) displayTimingInfo->pixelClock = 785400000;
	auto ret= FunctionCast(validateModeDepth, callback->ovalidateModeDepth)(that, param_1, param_2);
	return ret;
}

void Gen11::SetupTimings(void *that, void *param_1, void *param_2, void *param_3, void *param_4){
	FunctionCast(SetupTimings, callback->oSetupTimings)(that,param_1,param_2, param_3, param_4);
}

uint8_t Gen11::validateDisplayMode(void *that, int param_1,void *param_2, void *param_3){
	// xe/max2560 (-ngreenmax2560): rejeita modos acima de 2560 px de largura, para o driver
	// escolher o 2560x1600 que o autor do NootedGreen validou. Não altera o pixel clock.
	// Chama o original SEMPRE (ele inicializa o modo); só depois rejeita. Pular a chamada causou page fault
	// em getTimingInfoForDisplayMode (panic 2026-10-17 10:54).
	uint8_t ret = FunctionCast(validateDisplayMode, callback->ovalidateDisplayMode)(that,param_1,param_2,param_3);
	auto timing = reinterpret_cast<const IODetailedTimingInformationV2 *>(param_3);
	if (ret && timing && checkKernelArgument("-ngreenmax2560") && timing->horizontalActive > 2560)
		return 0;
	return ret;
}

uint32_t Gen11::getCurrentDisplayMode(void *that, int *displayMode, int *depth) {
	// xe/bootmode (-ngreenfixbootmode): com o DisplayPipe nativo, IOAccelDisplayPipe::init_framebuffer_resource
	// pergunta o modo atual do FB0 antes de o WindowServer definir qualquer modo. O FB responde
	// kIOFBBootProgrammable (0xfffffffb, fDetailedTimings=0), getPixelInformation falha (0xe00002c2), o CoreDisplay
	// marca a tela principal como offline e o WindowServer aborta em loop. Com -ngreenkeepefi o painel já roda no
	// modo nativo 3840x2400 (0x80001000), então devolvemos esse modo no lugar do boot-programmable.
	uint32_t ret = FunctionCast(getCurrentDisplayMode, callback->ogetCurrentDisplayMode)(that, displayMode, depth);
	gFbModeCalls++;
	gFbModeLastRet = ret;
	if (displayMode) gFbModeLastMode = (uint32_t)*displayMode;
	if (displayMode && *displayMode == (int)0xfffffffb) {
		gFbModeBootProg++;
		if (gFbModeBootProg <= 6)
			SYSLOG("ngreen", "FBMODE bootprog #%u: calls=%u ret=0x%x depth=%d arg=%d", gFbModeBootProg, gFbModeCalls, ret,
				   depth ? *depth : -1, (int)checkKernelArgument("-ngreenfixbootmode"));
	}
	if (checkKernelArgument("-ngreenfixbootmode")) gFbModeArgSeen = 1;
	if (ret == 0 && displayMode && *displayMode == (int)0xfffffffb && checkKernelArgument("-ngreenfixbootmode")) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			SYSLOG("ngreen", "fixbootmode: getCurrentDisplayMode boot-programmable -> 0x80001000 depth 1");
		}
		*displayMode = 0x80001000;
		if (depth) *depth = 1;
		gFbModeSubst++;
	}
	return ret;
}

void Gen11::setupDisplayTiming (void *that,void *param_1,
                         void *param_2){
    auto displayTimingInfo = const_cast<IODetailedTimingInformationV2 *>(reinterpret_cast<const IODetailedTimingInformationV2 *>(param_2));
    if (displayTimingInfo!=nullptr) displayTimingInfo->pixelClock = 785400000;
	FunctionCast(setupDisplayTiming, callback->osetupDisplayTiming)(that,param_1,param_2);
    //auto ret= FunctionCast(setupDisplayTiming, callback->osetupDisplayTiming)(that,param_1,param_2);
    //return ret;
}

unsigned long Gen11::getPixelInformation (void *that, uint param_1,int param_2,int param_3, void *param_4){
	// xe/bootmode (-ngreenfixbootmode): IOAccelDisplayPipe::init_framebuffer_resource pede o pixel info do modo
	// atual do FB0 enquanto ele ainda é kIOFBBootProgrammable (0xfffffffb, sem detailed timings) e isso falha
	// (validateDisplayMode 0xe00002c2). Com -ngreenkeepefi o painel já roda no modo nativo (0x80001000).
	static uint32_t gpiBoot = 0;
	if (param_1 == 0xfffffffbu) {
		gpiBoot++;
		const bool fix = checkKernelArgument("-ngreenfixbootmode");
		unsigned long r0 = 0;
		if (fix) {
			r0 = FunctionCast(getPixelInformation, callback->ogetPixelInformation)(that, 0x80001000u, param_2 ? param_2 : 1, param_3, param_4);
			if (gpiBoot <= 6)
				SYSLOG("ngreen", "FBMODE gpi bootprog #%u: depth=%d aperture=%d fix=%d -> 0x%lx", gpiBoot, param_2, param_3, (int)fix, r0);
			return r0;
		}
		if (gpiBoot <= 6)
			SYSLOG("ngreen", "FBMODE gpi bootprog #%u: depth=%d aperture=%d fix=0", gpiBoot, param_2, param_3);
	}
	return FunctionCast(getPixelInformation, callback->ogetPixelInformation)(that,param_1,param_2, param_3, param_4);
}

int Gen11::blit3d_supported(void *param_1,void *param_2)
{
	return 0;
}

uint8_t Gen11::isPanelPowerOn(void *that)
{
	return 1;//FunctionCast(isPanelPowerOn, callback->oisPanelPowerOn)(that);
}

void * Gen11::ExtendedContextWithOptions(void *param_1)
{
	return FunctionCast(ExtendedContextWithOptions, callback->oExtendedContextWithOptions)(param_1);
}

uint8_t Gen11::enableController(AppleIntel::AppleIntelFramebuffer *that)
{
	if (getMember<uint32_t>(that, 0x1dc)==0) getMember<uint8_t>(that, 0x1e0)=1;
	auto ret= FunctionCast(enableController, callback->oenableController)(that);
	return ret;
}


uint8_t Gen11::setDisplayMode(AppleIntel::AppleIntelFramebuffer *that, int param_1, int param_2)
{
	if (getMember<uint32_t>(that, 0x1dc)==0) getMember<uint8_t>(that, 0x1e0)=1;
	xeBootDiagDump("pre-setDisplayMode");
	auto r = FunctionCast(setDisplayMode, callback->osetDisplayMode)(that,param_1,param_2 );
	xeBootDiagDump("post-setDisplayMode");
	return r;

}

uint8_t Gen11::connectionChanged(void *that)
{
	
	auto ret= FunctionCast(connectionChanged, callback->oconnectionChanged)(that);
	//getMember<uint8_t>(that, 0x1e0)=1;
	return ret;
}

unsigned long  Gen11::allocateDisplayResources(void *that)
{
	auto ret=FunctionCast(allocateDisplayResources, callback->oallocateDisplayResources)(that);

	if (!NGreen::callback->isRealTGL) {
		// ADL-P / RPL-P / RPL-U (XE_LPD, Display v13).
		// ADL-P IS XE_LPD — same display engine, same register layout.
		// Apple's TGL driver (AppleIntelTGLGraphicsFramebuffer) does not program this
		// hardware correctly when spoofed, so we replicate icl_display_core_init here.

		//icl_display_core_init
		NGreen::callback->writeReg32(DC_STATE_EN,0);

		/* Wa_14011294188:ehl,jsl,tgl,rkl,adl-s */
		NGreen::callback->intel_de_rmw( SOUTH_DSPCLK_GATE_D, 0,
					 PCH_DPMGUNIT_CLOCK_GATE_DISABLE);

		uint32_t reg,reset_bits;
		reg = HSW_NDE_RSTWRN_OPT;
		reset_bits = RESET_PCH_HANDSHAKE_ENABLE;
		NGreen::callback->intel_de_rmw( reg, reset_bits, reset_bits);

		//intel_cdclk_init_hw(dev_priv);
		//gen12_dbuf_slices_config — XE_LPD: 4 slices (Linux syslog: dbuf slices=0xf)
		NGreen::callback->intel_de_rmw(_DBUF_CTL_S0,
					 DBUF_TRACKER_STATE_SERVICE_MASK,
					 DBUF_TRACKER_STATE_SERVICE(8));
		NGreen::callback->intel_de_rmw(_DBUF_CTL_S1,
					 DBUF_TRACKER_STATE_SERVICE_MASK,
					 DBUF_TRACKER_STATE_SERVICE(8));
		NGreen::callback->intel_de_rmw(_DBUF_CTL_S2,
					 DBUF_TRACKER_STATE_SERVICE_MASK,
					 DBUF_TRACKER_STATE_SERVICE(8));
		NGreen::callback->intel_de_rmw(_DBUF_CTL_S3,
					 DBUF_TRACKER_STATE_SERVICE_MASK,
					 DBUF_TRACKER_STATE_SERVICE(8));

		//gen9_dbuf_enable(dev_priv); — enable all 4 slices
		reg = _DBUF_CTL_S0;
		NGreen::callback->intel_de_rmw(reg, DBUF_POWER_REQUEST,
				  DBUF_POWER_REQUEST);
		NGreen::callback->readReg32(reg);
		IODelay(10);
		reg = _DBUF_CTL_S1;
		NGreen::callback->intel_de_rmw(reg, DBUF_POWER_REQUEST,
				  DBUF_POWER_REQUEST);
		NGreen::callback->readReg32(reg);
		IODelay(10);
		reg = _DBUF_CTL_S2;
		NGreen::callback->intel_de_rmw(reg, DBUF_POWER_REQUEST,
				  DBUF_POWER_REQUEST);
		NGreen::callback->readReg32(reg);
		IODelay(10);
		reg = _DBUF_CTL_S3;
		NGreen::callback->intel_de_rmw(reg, DBUF_POWER_REQUEST,
				  DBUF_POWER_REQUEST);
		NGreen::callback->readReg32(reg);
		IODelay(10);


		//icl_mbus_init
		// XE_LPD_FEATURES (Linux intel_display_device.c): .abox_mask = GENMASK(1, 0)
		// = ABOX0 (0x45038) + ABOX1 (0x45048)
		unsigned long abox_mask = GENMASK(1, 0);//DISPLAY_INFO(dev_priv)->abox_mask;
		int config, i;

		unsigned long abox_regs=abox_mask;
		uint32_t mask = MBUS_ABOX_BT_CREDIT_POOL1_MASK |
			MBUS_ABOX_BT_CREDIT_POOL2_MASK |
			MBUS_ABOX_B_CREDIT_MASK |
			MBUS_ABOX_BW_CREDIT_MASK;
		uint32_t val = MBUS_ABOX_BT_CREDIT_POOL1(16) |
			MBUS_ABOX_BT_CREDIT_POOL2(16) |
			MBUS_ABOX_B_CREDIT(1) |
			MBUS_ABOX_BW_CREDIT(1);

		for_each_set_bit(i, &abox_regs, sizeof(abox_regs))
		NGreen::callback->intel_de_rmw( MBUS_ABOX_CTL(i), mask, val);


		//tgl_bw_buddy_init
		// Linux syslog: DRAM channels=4, memory type 0x23=LPDDR5
		// tgl_buddy_page_masks: {4ch, LPDDR4, 0x38} and {4ch, LPDDR5, 0x38} — same page_mask
		// Use LPDDR4 bucket (LPDDR5 maps identically in the table)
		enum intel_dram_type type = INTEL_DRAM_LPDDR4;
		uint8_t num_channels = 4; //4ch LPDDR — tgl_buddy_page_masks → page_mask=0x38
		const struct buddy_page_mask *table=tgl_buddy_page_masks;

		for (config = 0; table[config].page_mask != 0; config++)
			if (table[config].num_channels == num_channels &&
				table[config].type == type)
				break;

		for_each_set_bit(i, &abox_mask, sizeof(abox_mask)) {
			NGreen::callback->writeReg32( BW_BUDDY_PAGE_MASK(i),
						   table[config].page_mask);

			/* Wa_22010178259:tgl,dg1,rkl,adl-s */
			NGreen::callback->intel_de_rmw( BW_BUDDY_CTL(i),
							 BW_BUDDY_TLB_REQ_TIMER_MASK,
							 BW_BUDDY_TLB_REQ_TIMER(0x8));
		}

		/* Wa_14011508470:tgl,dg1,rkl,adl-s,adl-p,dg2 */
		//if (IS_DISPLAY_VER_FULL(dev_priv, IP_VER(12, 0), IP_VER(13, 0)))
		/*NGreen::callback->intel_de_rmw( GEN11_CHICKEN_DCPR_2, 0,
					 DCPR_CLEAR_MEMSTAT_DIS | DCPR_SEND_RESP_IMM |
					 DCPR_MASK_LPMODE | DCPR_MASK_MAXLATENCY_MEMUP_CLR);*/

		/* * Display WA #1185 WaDisableDARBFClkGating:glk,icl,ehl,tgl
		 * Also known as Wa_14010480278.
		 */
		//if (IS_DISPLAY_VER(i915, 10, 12))
		//NGreen::callback->intel_de_rmw( GEN9_CLKGATE_DIS_0, 0, DARBF_GATING_DIS);

		/* Wa_14013723622 */
		NGreen::callback->intel_de_rmw( CLKREQ_POLICY, CLKREQ_POLICY_MEM_UP_OVRD, 0);

	}
	// isRealTGL: Apple's AppleIntelTGLGraphicsFramebuffer handles all of the above
	// natively (2 DBUF slices, XE_D abox_mask=GENMASK(2,1), correct DRAM config).
	// No additional writes needed.

	return ret;
}
	
void Gen11::IGScheduler5resume(void *that) {
		
		void *accelerator = getMember<void *>(that, 0x10);
		struct IGHwCsDesc *descArray = (struct IGHwCsDesc *)callback->kIGHwCsDesc;
		uint32_t mode = _MASKED_BIT_ENABLE(GEN11_GFX_DISABLE_LEGACY_MODE);

		for (int i = 0; i < 6; i++) {
			struct IGHwCsDesc *desc = &descArray[i];

			// --- FILTER: ONLY RCS AND BCS ---
			if (desc->type == kIGHwCsTypeRCS) {
				//FunctionCast(SafeForceWake, callback->oSafeForceWake)(accelerator, true, 0);
			} else if (desc->type == kIGHwCsTypeBCS) {
				//FunctionCast(SafeForceWake, callback->oSafeForceWake)(accelerator, true, 1);
			} else {
				continue;
			}

					uint32_t ringBase = desc->mmioExecListControl - 0x3c;
					
					// USE THE CORRECT STRUCT MEMBER FOR HWS
					// The dump proves mmioGlobalStatusPage holds the offset 0x2080
					uint32_t hwsRegisterOffset = desc->mmioGlobalStatusPage;
					
					// READ SHADOW REGISTER
					// We use the offset from the struct (0x2080) to index the shadow map
					uint32_t hwsAddr = getMember<uint32_t>(accelerator, 0x1240 + hwsRegisterOffset);
					
					// WRITE TO HARDWARE
					NGreen::callback->writeReg32(hwsRegisterOffset, hwsAddr);
					NGreen::callback->readReg32(hwsRegisterOffset);

					// REST OF INIT (Using explicit offsets from struct where possible)
					NGreen::callback->writeReg32(ringBase + 0x98, ~0u); // HWSTAM
					NGreen::callback->writeReg32(desc->mmioGfxMode, mode); // GFX Mode
					NGreen::callback->writeReg32(ringBase + 0x9c, _MASKED_BIT_DISABLE(STOP_RING)); // MI Mode
					
					NGreen::callback->writeReg32(desc->mmioErrorIdentity, ~0u); // EMR
					NGreen::callback->writeReg32(desc->mmioErrorMask, ~0u);    // EIR
					NGreen::callback->writeReg32(desc->mmioErrorIdentity, ~0x1); // I915_ERROR_INSTRUCTION
		}
	
	FunctionCast(IGScheduler5resume, callback->oIGScheduler5resume)(that);

}

// V212: The GPU watchdog calls isGpuIdle() after engine init to decide if the GPU is healthy.
// On RPL-P, INSTDONE bit0 is permanently 0 → original returns false → watchdog resets the GPU
// → startGraphicsEngine retry loop. startGraphicsEngine itself SUCCEEDS (return value is non-zero
// = success path); the reset is triggered entirely by this watchdog query.
bool Gen11::wrapIGScheduler5IsGpuIdle(const void *that) {
	bool idle = FunctionCast(wrapIGScheduler5IsGpuIdle, callback->oIGScheduler5IsGpuIdle)(that);
	if (!idle) {
		uint32_t instdone = NGreen::callback->readReg32(RING_INSTDONE(RENDER_RING_BASE));
		if ((instdone & ~1U) == 0xfffffffe) {
			SYSLOG("ngreen", "V212[5]: isGpuIdle override INSTDONE=0x%08x → idle", instdone);
			return true;
		}
	}
	return idle;
}

bool Gen11::wrapIGScheduler4IsGpuIdle(const void *that) {
	bool idle = FunctionCast(wrapIGScheduler4IsGpuIdle, callback->oIGScheduler4IsGpuIdle)(that);
	if (!idle) {
		uint32_t instdone = NGreen::callback->readReg32(RING_INSTDONE(RENDER_RING_BASE));
		if ((instdone & ~1U) == 0xfffffffe) {
			SYSLOG("ngreen", "V212[4]: isGpuIdle override INSTDONE=0x%08x → idle", instdone);
			return true;
		}
	}
	return idle;
}

typedef enum AGDCVendorClass {
	kAGDCVendorClassReserved,
	kAGDCVendorClassIntegratedGPU,
	kAGDCVendorClassDiscreteGPU,
	kAGDCVendorClassOtherHW,
	kAGDCVendorClassOtherSW,
	kAGDCVendorClassAppleGPUPolicyManager,
	kAGDCVendorClassAppleGPUPowerManager,
	kAGDCVendorClassGPURoot,
	kAGDCVendorClassAppleGPUWrangler,
	kAGDCVendorClassAppleMuxControl,
} AGDCVendorClass_t;

typedef struct AGDCVendorInfo {
	union {
		struct {
			UInt16 Minor;
			UInt16 Major;
		};
		UInt32 Raw;
	} Version;
	char VendorString[32];
	UInt32 VendorID;
	AGDCVendorClass_t VendorClass;
} AGDCVendorInfo_t;

uint32_t
Gen11::IntelFBClientControldoAttribute
		  (void *that,uint param_1,unsigned long *param_2,unsigned long param_3,unsigned long *param_4,
		   unsigned long *param_5,void *param_6)
{

	/*if (param_1 == 0x923) {
		return kIOReturnUnsupported;
	}*/
	
	auto ret=FunctionCast(IntelFBClientControldoAttribute, callback->oIntelFBClientControldoAttribute)(that,param_1,param_2,param_3,param_4,param_5,param_6);
	
	if (param_1 == 1)//0x2001)
	if (param_5 != (unsigned long *)0x0)
		if (0x2b < *param_5) {
			//memset(param_4,0,0x2c);
			AGDCVendorInfo *v=(AGDCVendorInfo*)param_4;
			v->Version.Raw=0;
			v->Version.Major=0;
			v->Version.Minor=0;
			v->VendorID=0x8086;
			*v->VendorString=*(char*)"INTEL";
			v->VendorClass=kAGDCVendorClassIntegratedGPU;
			//*(mach_vm_address_t*)v=callback->IntelFBClientControl11doAttribut;
		return 0;
	}
	return ret;
}

int hw=1;
int Gen11::hwSetMode
		  (AppleIntel::AppleIntelBaseController *that,
		   AppleIntel::AppleIntelFramebuffer *param_1,
		   AppleIntel::AppleIntelDisplayPath *param_2,
		   AppleIntel::CRTCParams *param_3)
{
	// On RPL-P (spoofed TGL), setPortMode ran just before us and restored TRANS_DDI_FUNC_CTL_A
	// to 0x8A000106 (4-lane eDP, matching UEFI DDI_BUF_CTL_A). paramsFbCompare inside the
	// original hwSetMode reads TRANS_DDI_FUNC_CTL_A and compares it with the target 0x8A010102
	// (2-lane DP SST). Write the correct value NOW so paramsFbCompare sees no lane-count change
	// needed and leaves DDI_BUF_CTL_A alone — preserving the UEFI-trained 4-lane eDP link.
	if (!NGreen::callback->isRealTGL) {
		NGreen::callback->writeReg32(0x60400, 0x8A000106);  // TRANS_DDI_FUNC_CTL_A
		DBGLOG("ngreen", "hwSetMode: pre-write TRANS_DDI_FUNC_CTL_A=0x8A000106 to prevent paramsFbCompare lane reprog");
	}
	auto ret= FunctionCast(hwSetMode, callback->ohwSetMode)(that, param_1, param_2, param_3);
	if (hw)
		enablePipe(that, param_1, param_2, param_3);
	hw=0;
	return ret;
}

void Gen11::enablePipe
		  (AppleIntel::AppleIntelBaseController *that,
		   AppleIntel::AppleIntelFramebuffer *param_1,
		   AppleIntel::AppleIntelDisplayPath *param_2,
		   AppleIntel::CRTCParams *param_3)
{
	return FunctionCast(enablePipe, callback->oenablePipe)(that, param_1, param_2, param_3);
}

uint8_t Gen11::beginReset(void *that)
{
	auto ret= FunctionCast(beginReset, callback->obeginReset)(that);
	
	/*static const struct intel_device_info tgl_info = {
		GEN12_FEATURES,
		PLATFORM(INTEL_TIGERLAKE),
		.platform_engine_mask =
			BIT(RCS0) | BIT(BCS0) | BIT(VECS0) | BIT(VCS0) | BIT(VCS2),
	};*/
	
	
	
	/*if (GRAPHICS_VER_FULL(dev_priv) >= IP_VER(12, 10))
		dg1_irq_reset(dev_priv);
	else if (GRAPHICS_VER(dev_priv) >= 11)
		gen11_irq_reset(dev_priv);*/
	
	//dg1_irq_reset(struct drm_i915_private *dev_priv)
	NGreen::callback->writeReg32( DG1_MSTR_TILE_INTR, 0);
	
	//gen11_irq_reset
	
	NGreen::callback->writeReg32( GEN11_GFX_MSTR_IRQ, 0);
	
	//gen11_gt_irq_reset(gt);
	// Disable RCS, BCS, VCS and VECS class engines.
	NGreen::callback->writeReg32( GEN11_RENDER_COPY_INTR_ENABLE, 0);
	NGreen::callback->writeReg32( GEN11_VCS_VECS_INTR_ENABLE,	  0);

	// Restore masks irqs on RCS, BCS, VCS and VECS engines.
	NGreen::callback->writeReg32( GEN11_RCS0_RSVD_INTR_MASK,	~0);
	NGreen::callback->writeReg32( GEN11_BCS_RSVD_INTR_MASK,	~0);
	
	NGreen::callback->writeReg32( GEN11_VCS0_VCS1_INTR_MASK,	~0);
	NGreen::callback->writeReg32( GEN11_VCS2_VCS3_INTR_MASK,	~0);
	
	//if (HAS_ENGINE(gt, VECS2) || HAS_ENGINE(gt, VECS3))
	NGreen::callback->writeReg32( GEN12_VECS2_VECS3_INTR_MASK, ~0);
	
	NGreen::callback->writeReg32( GEN11_GPM_WGBOXPERF_INTR_ENABLE, 0);
	NGreen::callback->writeReg32( GEN11_GPM_WGBOXPERF_INTR_MASK,  ~0);
	NGreen::callback->writeReg32( GEN11_GUC_SG_INTR_ENABLE, 0);
	NGreen::callback->writeReg32( GEN11_GUC_SG_INTR_MASK,  ~0);

	NGreen::callback->writeReg32( GEN11_CRYPTO_RSVD_INTR_ENABLE, 0);
	NGreen::callback->writeReg32( GEN11_CRYPTO_RSVD_INTR_MASK,  ~0);
	
	//gen11_display_irq_reset(dev_priv);
	
	NGreen::callback->writeReg32( GEN11_GFX_MSTR_IRQ, ~0);

	return ret;
}
											
void Gen11::endReset(void *that)
{
	FunctionCast(endReset, callback->oendReset)(that);
	
	//void gen11_gt_irq_postinstall(struct intel_gt *gt)
	uint32_t irqs = GT_RENDER_USER_INTERRUPT;
	uint32_t dmask,smask;
	
	dmask = irqs << 16 | irqs;
	smask = irqs << 16;
	
	
	
	// Enable RCS, BCS, VCS and VECS class interrupts.
	NGreen::callback->writeReg32( GEN11_RENDER_COPY_INTR_ENABLE, dmask);
	NGreen::callback->writeReg32( GEN11_VCS_VECS_INTR_ENABLE, dmask);

		// Unmask irqs on RCS, BCS, VCS and VECS engines.
	NGreen::callback->writeReg32( GEN11_RCS0_RSVD_INTR_MASK, ~smask);
	NGreen::callback->writeReg32( GEN11_BCS_RSVD_INTR_MASK, ~smask);

	NGreen::callback->writeReg32( GEN11_VCS0_VCS1_INTR_MASK, ~dmask);
	NGreen::callback->writeReg32( GEN11_VCS2_VCS3_INTR_MASK, ~dmask);
	
	NGreen::callback->writeReg32( GEN11_VECS0_VECS1_INTR_MASK, ~dmask);
	
		//if (HAS_ENGINE(gt, VECS2) || HAS_ENGINE(gt, VECS3))
	NGreen::callback->writeReg32( GEN12_VECS2_VECS3_INTR_MASK, ~dmask);
	
	
	//gen11_de_irq_postinstall(dev_priv);
	NGreen::callback->writeReg32( GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
	
	NGreen::callback->writeReg32( DG1_MSTR_TILE_INTR, DG1_MSTR_IRQ);
}

//SIGNATURES GFX
//ulong __thiscall IntelAccelerator::start(IntelAccelerator *this,IOService *param_1)
//undefined8 __thiscall IGAccelDevice::deviceStart(IGAccelDevice *this)
//void __thiscall IntelAccelerator::getGPUInfo(IntelAccelerator *this)
//void IntelAccelerator::getGPUInfo(void)
//void __thiscall IntelAccelerator::populateResetRegisterList(IntelAccelerator *this)
//undefined8 __thiscall IntelAccelerator::createUserGPUTask(IntelAccelerator *this)
//IGAccelTask * IGAccelTask::withOptions(IntelAccelerator *param_1)
//IGHardwareExtendedContext * __thiscall IGAccelTask::getBlit3DContext(IGAccelTask *this,bool param_1)
//undefined8 __thiscall IGHardwareExtendedContext::initWithOptions (IGHardwareExtendedContext *this,IGAccelTask *param_1, IGHardwareExtendedContextParams *param_2)
//undefined8 blit3d_init_ctx(IGHardwareBlit3DContext *param_1)
//void blit3d_initialize_scratch_space(IGAccelSysMemory *param_1)
//void __thiscall IGHardwareBlit3DContext::initialize(IGHardwareBlit3DContext *this)
//ulong __thiscall IntelAccelerator::startGraphicsEngine(IntelAccelerator *this)
//undefined8 __thiscall IntelAccelerator::stopGraphicsEngine(IntelAccelerator *this)
//void __thiscall IGAccelSegmentResourceList::initBlitUsage(IGAccelSegmentResourceList *this)
//ulong IntelAccelerator::submitBlit (blit3d_params_t *param_1,IGVector *param_2,IGAccelTask *param_3,bool param_4)
