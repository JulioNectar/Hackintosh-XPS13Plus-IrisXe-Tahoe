//  XE-PM: CPU performance and idle power profile for hybrid Alder Lake / Raptor Lake (and Tiger Lake) under XCPM.
//  Opt-in with -ngreenpm. Everything is applied 20 s after the patcher loads (system up), re-checked at 60 s.
//
//  1. HWP units (hybrid P-cores). XCPM writes IA32_HWP_REQUEST (0x774) max/desired in 100 MHz ratio units (max = the
//     TURBO_RATIO_LIMIT value, 50 on an i7-1360P). On hybrid P-cores one HWP performance unit is ~78.7 MHz (Linux
//     intel_pstate hybrid scaling factor 78741; HWP_CAPABILITIES highest = 64 for 5.0 GHz), so max 50 caps them at
//     ~3.9 GHz. The static XCPM routine at _xcpm_enable_pstate_ctrs+0x1190 (macOS 26.7.1 25G241) takes the target
//     multiplier and writes the local CPU's request; it is routed, and after Apple's write the request is rescaled on
//     cores whose highest perf exceeds the requested max (max -> highest, desired -> desired * highest / max). The
//     route is installed only after the prologue and the write site match byte for byte.
//  2. EPP. XCPM's frequency vector asks EPP 0x80. On ADL/RPL mobile parts that clips one-core turbo (Intel, Linux 5.17
//     intel_pstate: EPP <= 102 needed). The profile writes EPP 0 (ngreenpmepp=N to choose, sysctl debug.xepm.epp).
//  3. Energy-efficient turbo off (MSR_POWER_CTL 0x1FC bit 19).
//  4. XCPM idle table (_xcpm_cstate_table): for the spoofed CPU it has C8 (0x40) and C10 (0x60) disabled and C3 (0x10,
//     not an ADL/RPL/TGL state) enabled, so cores stop at C6 or fall back to C1. C8/C10 are enabled and C3 disabled,
//     only if the three entries carry exactly those hints.
//  5. D3hot for PCI functions macOS has no driver for (IPU camera, GNA, ISH, DPTF participant, Thunderbolt/USB4 DMA),
//     by device id, only when nothing is attached (or only a client that never matched).
//  Measured on a Dell XPS 13 Plus 9320 (i7-1360P): Geekbench 7 single-core 1269 -> 1697, multi-core 5859 -> 6148,
//  one-thread clock 3.3 -> 4.3-4.85 GHz, idle package ~4.6 -> ~3.3 W.
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>
#include <sys/sysctl.h>

extern "C" void mp_rendezvous_no_intrs(void (*fn)(void *), void *arg);

static inline uint64_t xpRdmsr(uint32_t m) { uint32_t lo, hi; asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return ((uint64_t)hi << 32) | lo; }
static inline void xpWrmsr(uint32_t m, uint64_t v) { asm volatile("wrmsr" : : "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }

static bool gXpOn = false;
static int gXpHwp = 0, gXpEpp = -1;                 // sysctl debug.xepm.hwp / .epp (root)
static int gXpEet = 0, gXpCst = 0, gXpD3 = 0, gXpRoute = 0;   // status
static mach_vm_address_t gXpOrgSetMult = 0, gXpCstPtr = 0;

// ─── 1/2: XCPM HWP request ───────────────────────────────────────────────────────────────────────────────────────
static uint64_t xpSetMult(uint64_t mult, uint64_t a1, uint64_t a2, uint64_t a3) {   // only edi is used by Apple
	const uint64_t r = FunctionCast(xpSetMult, gXpOrgSetMult)(mult, a1, a2, a3);
	// XCPM calls this before it enables HWP; the HWP MSRs #GP until IA32_PM_ENABLE bit 0 is set.
	if (gXpHwp && (xpRdmsr(0x770) & 1)) {
		const uint64_t v = xpRdmsr(0x774);
		if (v & (1ULL << 42)) return r;   // package control: leave it alone
		const uint32_t hi = (uint32_t)(xpRdmsr(0x771) & 0xff), max = (uint32_t)(v >> 8) & 0xff, des = (uint32_t)(v >> 16) & 0xff;
		uint64_t nv = v;
		if (max && hi > max && hi < 0xff) {
			uint32_t d = des ? (des * hi + max / 2) / max : 0;
			if (d > hi) d = hi;
			nv = (nv & ~0xFFFF00ULL) | ((uint64_t)hi << 8) | ((uint64_t)d << 16);
		}
		if (gXpEpp >= 0) nv = (nv & ~0xFF000000ULL) | ((uint64_t)gXpEpp << 24);
		if (nv != v) xpWrmsr(0x774, nv);
	}
	return r;
}

// ─── 3: energy-efficient turbo ───────────────────────────────────────────────────────────────────────────────────
static void xpEetOne(void *) {
	const uint64_t v = xpRdmsr(0x1FC);
	if (!(v & (1ULL << 19))) xpWrmsr(0x1FC, v | (1ULL << 19));
}

// ─── 4: XCPM idle table ──────────────────────────────────────────────────────────────────────────────────────────
static int xpCstFix() {
	uint8_t *tab = gXpCstPtr ? *reinterpret_cast<uint8_t **>(gXpCstPtr) : nullptr;
	if (!tab) return 2;
	auto hint = [&](int i) { return *reinterpret_cast<volatile uint32_t *>(tab + i * 24 + 4); };
	auto en = [&](int i) { return reinterpret_cast<volatile uint32_t *>(tab + i * 24 + 8); };
	if (hint(0) != 0x60 || hint(2) != 0x40 || hint(6) != 0x10) return 3;
	if (*en(0) == 1 && *en(2) == 1 && *en(6) == 0) return 1;
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) return 4;
	*en(0) = 1; *en(2) = 1; *en(6) = 0;
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	return 1;
}

// ─── 5: D3hot for driverless functions ───────────────────────────────────────────────────────────────────────────
static int xpD3() {
	static const uint16_t ids[] = {
		0xa75d, 0x465d, 0x9a19,           // IPU6 camera (RPL-P, ADL-P, TGL)
		0xa74f, 0x464f, 0x9a11,           // GNA
		0x51fc, 0x54fc, 0xa0fc,           // ISH (ADP-P, ADP-N, TGP-LP)
		0xa71d, 0x461d, 0x9a03,           // DPTF processor participant
		0xa73e, 0xa76d, 0x463e, 0x466d, 0x9a1b, 0x9a1d,   // Thunderbolt 4 / USB4 DMA (macOS has no host driver here)
	};
	int n = 0;
	OSIterator *it = IOService::getMatchingServices(IOService::serviceMatching("IOPCIDevice"));
	if (!it) return 0;
	while (auto *o = it->getNextObject()) {
		auto *d = OSDynamicCast(IOPCIDevice, o);
		if (!d) continue;
		if (IOService *cl = d->getClient())   // e.g. a USBToolBox instance on the TBT DMA that never matched
			if (cl->getState() & (kIOServiceMatchedState | kIOServiceRegisteredState)) continue;
		const uint32_t id = d->configRead32(0);
		if ((id & 0xffff) != 0x8086) continue;
		bool want = false;
		for (uint16_t w : ids) if (w == (id >> 16)) want = true;
		uint8_t pm = 0;
		if (!want || !d->findPCICapability(kIOPCIPowerManagementCapability, &pm) || !pm) continue;
		const uint16_t cs = d->configRead16(pm + 4);
		if ((cs & 3) != 3) { d->configWrite16(pm + 4, (uint16_t)(cs | 3)); n++; }
	}
	it->release();
	return n;
}

static void xpApply(thread_call_param_t, thread_call_param_t) {
	gXpCst = xpCstFix();
	gXpD3 += xpD3();
	if (!gXpEet) { mp_rendezvous_no_intrs(xpEetOne, nullptr); gXpEet = (int)((xpRdmsr(0x1FC) >> 19) & 1); }
	if (gXpOrgSetMult) gXpHwp = 1;   // takes effect at XCPM's next request on each CPU
	IOLog("XeNG PM: profile on: HWP rescale %d (route %d), EPP %d, EE turbo off %d, C8/C10 %d, D3 %d\n",
	      gXpHwp, gXpRoute, gXpEpp, gXpEet, gXpCst, gXpD3);
}

SYSCTL_NODE(_debug, OID_AUTO, xepm, CTLFLAG_RW, 0, "NootedGreen XE-PM");
SYSCTL_INT(_debug_xepm, OID_AUTO, hwp, CTLFLAG_RW, &gXpHwp, 0, "HWP rescale on hybrid P-cores");
SYSCTL_INT(_debug_xepm, OID_AUTO, epp, CTLFLAG_RW, &gXpEpp, 0, "EPP override, -1 = XCPM's");
SYSCTL_INT(_debug_xepm, OID_AUTO, eet_off, CTLFLAG_RD, &gXpEet, 0, "");
SYSCTL_INT(_debug_xepm, OID_AUTO, cst, CTLFLAG_RD, &gXpCst, 0, "1 = C8/C10 on");
SYSCTL_INT(_debug_xepm, OID_AUTO, d3, CTLFLAG_RD, &gXpD3, 0, "");
SYSCTL_INT(_debug_xepm, OID_AUTO, route, CTLFLAG_RD, &gXpRoute, 0, "1 = HWP route installed");

void xePmInstall(KernelPatcher &patcher) {
	if (!checkKernelArgument("-ngreenpm")) return;
	gXpOn = true;
	int e = 0;
	gXpEpp = (PE_parse_boot_argn("ngreenpmepp", &e, sizeof(e)) && e >= 0 && e <= 255) ? e : 0;
	gXpCstPtr = patcher.solveSymbol(KernelPatcher::KernelID, "_xcpm_cstate_table");
	patcher.clearError();
	const mach_vm_address_t base = patcher.solveSymbol(KernelPatcher::KernelID, "_xcpm_enable_pstate_ctrs");
	patcher.clearError();
	if (base) {
		const mach_vm_address_t fn = base + 0x1190;
		static const uint8_t pro[] = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x50, 0x89, 0xFB};   // push rbp; mov rbp,rsp; push rbx; push rax; mov ebx,edi
		static const uint8_t wr[] = {0x89, 0xF8, 0xC1, 0xEA, 0x18, 0xB9, 0x74, 0x07, 0x00, 0x00, 0x0F, 0x30};   // +0x168: ...; mov ecx,0x774; wrmsr
		auto *p = reinterpret_cast<const uint8_t *>(fn);
		bool ok = true;
		for (size_t i = 0; i < sizeof(pro); i++) if (p[i] != pro[i]) ok = false;
		for (size_t i = 0; i < sizeof(wr); i++) if (p[0x168 + i] != wr[i]) ok = false;
		if (ok) {
			gXpOrgSetMult = patcher.routeFunction(fn, reinterpret_cast<mach_vm_address_t>(xpSetMult), true);
			if (patcher.getError() != KernelPatcher::Error::NoError) { gXpOrgSetMult = 0; patcher.clearError(); }
		}
		gXpRoute = gXpOrgSetMult ? 1 : (ok ? 5 : 3);
	} else gXpRoute = 2;
	sysctl_register_oid(&sysctl__debug_xepm);
	sysctl_register_oid(&sysctl__debug_xepm_hwp);
	sysctl_register_oid(&sysctl__debug_xepm_epp);
	sysctl_register_oid(&sysctl__debug_xepm_eet_off);
	sysctl_register_oid(&sysctl__debug_xepm_cst);
	sysctl_register_oid(&sysctl__debug_xepm_d3);
	sysctl_register_oid(&sysctl__debug_xepm_route);
	uint64_t dl;
	thread_call_t t20 = thread_call_allocate(xpApply, nullptr), t60 = thread_call_allocate(xpApply, nullptr);
	clock_interval_to_deadline(20, kSecondScale, &dl);
	if (t20) thread_call_enter_delayed(t20, dl);
	clock_interval_to_deadline(60, kSecondScale, &dl);
	if (t60) thread_call_enter_delayed(t60, dl);
	IOLog("XeNG PM: -ngreenpm, HWP route %d, idle table %s; profile in 20 s\n", gXpRoute, gXpCstPtr ? "found" : "missing");
}
