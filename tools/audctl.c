// audctl: user-space tool for the AudShim kext (milestones M1/M1b/M2/M2b/M3/M4/M5/M6/M7, audctl 0.9.0).
// Intel cAVS 2.5 audio DSP, PCI 8086:51CA (RPL-P), Dell XPS 13 Plus 9320.
//
//   audctl info               PCI IDs, command/status, BAR addresses and sizes, PM state
//   audctl rd0 <off> [n]      read n dwords from BAR0 (HDA controller)
//   audctl rd4 <off> [n]      read n dwords from BAR4 (DSP); the kext gates regions (see AudShim.cpp)
//   audctl cfg <off> [n]      read n dwords of PCI config space (0..0xFFF)
//   audctl m1                 full M1 dump with decoded fields (RESEARCH.md section 4, M1)
//   audctl m1 hda             same, but stops after config space + BAR0 (never touches BAR4)
//   audctl m2 --dry           M2: print the planned write list (offsets, values, Linux file:line);
//                             nothing is armed, nothing is written
//   audctl m2 [opts]          M2: pre-checks, ARM, then DSP core 0 power-up to the ROM (writes!)
//   audctl m2 --undo          M2: back to the M1 state (stall/reset/power down core 0, cfg 0x44/0x48)
//   audctl m2 --state         M2: kext-side M2 state (read-only)
//   audctl m2b --dry          M2b: planned sequence (Linux order incl. controller reset, PPCTL, L1SEN,
//                             ICCMAX + code-loader streams with DMA buffers, BME); nothing written
//   audctl m2b [opts]         M2b: the real run (writes!), stops at ROM INIT_DONE, then tears down
//   audctl m2b --undo         M2b: full rollback (core 0 off, streams/SPIB/PPCTL/EM2/LTRP/cfg restored,
//                             BME off, DMA freed). Same kext path as `m2 --undo`.
//   audctl m2b --state        M2b: kext state + live M2b registers (read-only)
//   audctl m3 --dry [--offline] M3: parse the firmware file (SOF extended manifest, payload) and print the
//                             planned sequence; nothing armed or written
//   audctl m3 [opts]          M3: M2b + firmware download over the code-loader DMA + FW_READY (writes!),
//                             then teardown (or --leave-up: firmware left running)
//   audctl m3 --undo          M3: full rollback (same kext path as m2 --undo; also acks a pending
//                             DSP->host message and frees the firmware buffer)
//   audctl m3 --state         M3: kext state, live registers, firmware status/mailbox if core 0 is up
//   audctl m4 --dry [--offline] M4: firmware file + full planned sequence incl. SoundWire; nothing written
//   audctl m4 [opts]          M4: M3 (firmware up, FW_READY acked) + SoundWire link power-up, Cadence
//                             master start (bus reset) and enumeration (SCP_DevId read, SCP_DevNumber
//                             assignment) on every link, then teardown (or --leave-up)
//   audctl m4 --undo          M4: full rollback (links stopped like intel_stop_bus, then the M3 rollback)
//   audctl m4 --state         M4: M3 state + SoundWire kext state, shim and Cadence registers
//   audctl tplg [PATH]        offline: parse the SOF topology (.tplg) and print the IPC3 messages Linux sends for
//                             the speaker PCM (no kext access)
//   audctl m5 --dry [--offline] [--hex]  M5: M4 plan + speaker playback plan (topology messages, SoundWire stream
//                             registers, RT1316 sequence); nothing written
//   audctl m5 [opts]          M5: M4 + RT1316 init, SoundWire stream on links 1+2, SOF pipeline from the topology,
//                             host DMA playback of a ~1 kHz sine (default 2 s, -30 dBFS; caps 3 s / -12 dBFS), then
//                             the full reverse teardown; opts --seconds N --level dB --channel=both|left|right
//                             --no-play (= audctl m5a: everything except the channel enable / DMA / TRIG_START)
//   audctl m5 --undo | --state  rollback (amps muted + PS3 first) / state incl. the M5 kext state
//   audctl gen-header [OUT.h] M6 (offline): writes tools/AudShim/AudShimData.h = firmware payload + the IPC3 plan +
//                             SoundWire/RT1316 tables, compiled into the kext for its autonomous driver mode
//   audctl drv state|open|start|stop|close   M6: the kext driver mode (interface v8) through a driver connection
//   audctl drv test [--seconds=N] [--level=dB]  M6: OPEN, map the shared ring, sine, START/STOP twice, CLOSE
//                             (only when no other driver connection, i.e. the XeSpeakers HAL plug-in, owns it)
//   audctl rec [OUT.wav] [--seconds=N]  M7 (interface v9): record the internal microphones (RT714 on SoundWire link 0)
//                             to a 48 kHz / 2 ch / 32-bit WAV and print RMS / peak levels; works while the HAL plug-in
//                             owns the hardware (capture-only connection) or alone (then it brings the hardware up)
//   audctl rec --dry-run [--hex]  M7 (offline, no kext access): the planned RT714 register / SoundWire / DMA / IPC3 sequence
//
// Everything except `m2` (without --dry/--state) is read-only. (Since M1b the kext itself sets
// PCI COMMAND.MSE once at start, unless booted with -audshimnomse; `info` reports it.) The M2
// writes go through kext method 4 only, which accepts a fixed list of ops, each with fixed
// values and preconditions, and only after an ARM call on the same connection.
// The m1 dump flushes and fsync()s stdout before the first BAR4 read, so if a BAR4 read ever
// hung the machine, the config + BAR0 part is already on disk.
// Build: clang -O2 -Wall -o audctl audctl.c -framework IOKit -framework CoreFoundation
//
// Register offsets and fields cite the Linux tree in ~/xe/build/audio/linux-src (torvalds master):
//   [HR]  include/sound/hda_register.h
//   [SH]  sound/soc/sof/intel/hda.h
//   [SI]  include/linux/soundwire/sdw_intel.h
//   [CD]  drivers/soundwire/cadence_master.c
//   [CT]  sound/hda/core/controller.c, [HC] sound/soc/sof/intel/hda-ctrl.c
//   [TG]  sound/soc/sof/intel/tgl.c, [PT] sound/soc/sof/intel/pci-tgl.c
// RPL-P (8086:51CA) -> rpl_desc ([PT]:295, 252) -> tgl_chip_info ([TG]:138): cAVS 2.5, 4 cores
// ([TG]:140), only core 0 host-managed ([TG]:142), CNL IPC ([TG]:143-147), SDW shim/ALH bases
// ([TG]:152-153 -> [SI]:15-16), cAVS SoundWire link layout with ip_offset 0
// (drivers/soundwire/intel_init.c:68-70).
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <mach/mach_time.h>

static io_connect_t conn;

enum { kInfo = 0, kCfgRead = 1, kMmioRead = 2, kMseInfo = 3, kM2 = 4 };

static int aud_open(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AudShim"));
    if (!svc) { fprintf(stderr, "AudShim not loaded\n"); return -1; }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr) { fprintf(stderr, "IOServiceOpen: 0x%x\n", kr); return -1; }
    return 0;
}

static int info(uint64_t o[16]) {
    uint32_t n = 16;
    return IOConnectCallScalarMethod(conn, kInfo, NULL, 0, o, &n);
}

// Returns 0 on success; on failure *v = 0xFFFFFFFF and the IOReturn is returned.
static kern_return_t cfg(uint32_t off, uint32_t *v) {
    uint64_t in = off, out = 0; uint32_t n = 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, kCfgRead, &in, 1, &out, &n);
    *v = kr ? 0xFFFFFFFFu : (uint32_t)out;
    return kr;
}

#ifdef AUDCTL_SELFTEST   /* offline parser test only (not in the shipped binary): BAR4 reads from a fake image */
uint8_t *g_fake4;
#endif
static kern_return_t mmio(unsigned bar, uint32_t off, uint32_t *v) {
#ifdef AUDCTL_SELFTEST
    if (g_fake4 && bar == 4) { memcpy(v, g_fake4 + off, 4); return 0; }
#endif
    uint64_t in[2] = {bar, off}, out = 0; uint32_t n = 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, kMmioRead, in, 2, &out, &n);
    *v = kr ? 0xFFFFFFFFu : (uint32_t)out;
    return kr;
}

static const char *krs(kern_return_t kr) {
    switch (kr) {
    case 0: return "ok";
    case kIOReturnNotReady: return "refused: not ready (MSE off / not D0 / link or core unpowered)";
    case kIOReturnNotPermitted: return "refused: region not allowed in M1";
    case kIOReturnBadArgument: return "refused: bad offset (alignment/bounds)";
    case kIOReturnNotFound: return "BAR not mapped";
    default: return "error";
    }
}

static uint32_t F(uint32_t v, int hi, int lo) { return (v >> lo) & ((hi - lo == 31) ? 0xFFFFFFFFu : ((1u << (hi - lo + 1)) - 1)); }

// Read + print one named register; returns the value (0xFFFFFFFF on refusal) and sets *ok.
static uint32_t R(unsigned bar, uint32_t off, const char *name, int *ok) {
    uint32_t v; kern_return_t kr = mmio(bar, off, &v);
    if (ok) *ok = (kr == 0);
    if (kr) printf("  %-14s BAR%u+0x%05x = --------  (%s)\n", name, bar, off, krs(kr));
    else    printf("  %-14s BAR%u+0x%05x = %08x", name, bar, off, v);
    return v;
}
#define NL() printf("\n")

static void hexdump(unsigned which, uint32_t off, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = off + 4 * i, v; kern_return_t kr;
        kr = (which == 'c') ? cfg(o, &v) : mmio(which, o, &v);
        if (i % 4 == 0) printf("%s%05x:", i ? "\n" : "", o);
        if (kr) printf(" --------"); else printf(" %08x", v);
    }
    printf("\n");
}

// ------------------------------------------------------------------ info

static void cmd_info(void) {
    uint64_t o[16];
    if (info(o)) { fprintf(stderr, "info failed\n"); return; }
    printf("vendor:device   %04llx:%04llx\n", o[0] & 0xFFFF, o[0] >> 16);
    printf("subsystem       %04llx:%04llx\n", o[2] & 0xFFFF, o[2] >> 16);
    printf("class/rev       %06llx rev %02llx  (0x040100 = audio DSP/SOF mode, 0x040300 = legacy HDA)\n",
           o[1] >> 8, o[1] & 0xFF);
    printf("command/status  %08llx  MSE=%llu BME=%llu INTxDis=%llu\n", o[3], (o[3] >> 1) & 1, (o[3] >> 2) & 1, (o[3] >> 10) & 1);
    printf("BAR0 (HDA)      phys 0x%llx len 0x%llx  raw cfg10=%08llx cfg14=%08llx  64-bit=%s\n",
           o[4], o[5], o[8], o[9], ((o[8] >> 1) & 3) == 2 ? "yes" : "no");
    printf("BAR4 (DSP)      phys 0x%llx len 0x%llx  raw cfg20=%08llx cfg24=%08llx  64-bit=%s\n",
           o[6], o[7], o[10], o[11], ((o[10] >> 1) & 3) == 2 ? "yes" : "no");
    if (o[12]) printf("PM cap @0x%02llx    PMCSR %08llx  D%llu\n", o[12], o[13], o[13] & 3);
    printf("kext flags      BAR0 %s%s, BAR4 %s%s, MMIO reads %s, interface v%llu\n",
           (o[14] & 1) ? "mapped" : "UNMAPPED", (o[14] & 2) ? " ro" : "",
           (o[14] & 4) ? "mapped" : "UNMAPPED", (o[14] & 8) ? " ro" : "",
           (o[14] & 16) ? "allowed" : "REFUSED (MSE off or not D0)", o[15]);
    if (o[15] >= 2) {
        uint64_t m[4]; uint32_t n = 4;
        static const char *st[] = {"unknown (start did not reach it)", "already on at start: nothing written",
                                   "ENABLED by AudShim", "left off: -audshimnomse boot-arg",
                                   "write did NOT stick", "cmd read 0xFFFF: not touched"};
        if (IOConnectCallScalarMethod(conn, kMseInfo, NULL, 0, m, &n) == 0)
            printf("MSE (M1b)       %s  (cmd at start 0x%04llx -> after 0x%04llx, now 0x%04llx; BME now %llu)\n",
                   m[0] < 6 ? st[m[0]] : "?", m[1], m[2], m[3], (m[3] >> 2) & 1);
        else
            printf("MSE (M1b)       method 3 failed\n");
    } else {
        printf("MSE (M1b)       n/a: kext interface v%llu predates M1b (it never touches MSE)\n", o[15]);
    }
}

// ------------------------------------------------------------------ m1

static void m1_cfg(void) {
    uint32_t v;
    printf("\n== PCI config space ==\n");
    hexdump('c', 0, 64);
    cfg(0x04, &v);
    printf("  CMD/STS   %08x  MSE=%u BME=%u INTxDisable=%u CapList=%u\n", v, F(v,1,1), F(v,2,2), F(v,10,10), F(v,20,20));
    // PCI capability walk (standard list at 0x34)
    uint32_t p; cfg(0x34, &p); p &= 0xFC;
    for (int n = 0; p && n < 16; n++) {
        uint32_t h; cfg(p, &h);
        uint8_t id = h & 0xFF, next = (h >> 8) & 0xFC;
        if (id == 0x01) {
            uint32_t cs; cfg(p + 4, &cs);
            printf("  cap@%02x PM     PMC=%04x PMCSR=%08x -> D%u NoSoftReset=%u PME_En=%u PME_Sts=%u\n",
                   p, h >> 16, cs, F(cs,1,0), F(cs,3,3), F(cs,8,8), F(cs,15,15));
        } else if (id == 0x05) {
            uint32_t lo, hi; cfg(p + 4, &lo); cfg(p + 8, &hi);
            printf("  cap@%02x MSI    ctrl=%04x Enable=%u 64bit=%u MMC=%u MME=%u addr=%08x:%08x\n",
                   p, h >> 16, F(h,16,16), F(h,23,23), F(h,19,17), F(h,22,20), hi, lo);
        } else if (id == 0x10) {
            printf("  cap@%02x PCIe   %08x\n", p, h);
        } else if (id == 0x09) {
            printf("  cap@%02x VendorSpecific %08x\n", p, h);
        } else {
            printf("  cap@%02x id=0x%02x %08x\n", p, id, h);
        }
        p = next;
    }
    // [SH]:25-35 PCI_TCSEL/PCI_PGCTL = 0x44 (ADSPPGD b2, LSRMD b4), PCI_CGCTL = 0x48 (ADSPDCGE b1, MISCBDCGE b6)
    cfg(0x44, &v);
    printf("  cfg44 TCSEL/PGCTL byte=%02x  TCSEL[2:0]=%u ADSPPGD(b2)=%u LSRMD(b4)=%u   ([SH]:25-31)\n",
           v & 0xFF, F(v,2,0), F(v,2,2), F(v,4,4));
    cfg(0x48, &v);
    printf("  cfg48 CGCTL byte=%02x  ADSPDCGE(b1)=%u MISCBDCGE(b6)=%u   ([SH]:27,34-35)\n",
           v & 0xFF, F(v,1,1), F(v,6,6));
}

struct caps { uint32_t pp, spb, ml, gts, drsm; int nstreams; };

static void m1_bar0(struct caps *c) {
    int ok; uint32_t v;
    printf("\n== BAR0: HDA controller (global regs [HR]:14-37) ==\n");
    v = R(0, 0x00, "GCAP/VMIN/VMAJ", &ok);
    if (ok) {
        uint32_t g = v & 0xFFFF;
        c->nstreams = F(g,15,12) + F(g,11,8) + F(g,7,3);
        printf("  GCAP=%04x OSS=%u ISS=%u BSS=%u NSDO=%u 64OK=%u  VMIN=%u VMAJ=%u  ([HR]:14-21)\n",
               g, F(g,15,12), F(g,11,8), F(g,7,3), F(g,2,1), F(g,0,0), F(v,23,16), F(v,31,24));
    } else NL();
    v = R(0, 0x04, "OUTPAY/INPAY", &ok); if (ok) printf("  OUTPAY=%u INPAY=%u\n", v & 0xFFFF, v >> 16); else NL();
    v = R(0, 0x08, "GCTL", &ok);
    if (ok) printf("  CRST(b0)=%u%s FCNTRL(b1)=%u UNSOL(b8)=%u  ([HR]:24-27)\n", F(v,0,0),
                   F(v,0,0) ? "" : " (controller IN RESET)", F(v,1,1), F(v,8,8)); else NL();
    v = R(0, 0x0C, "WAKEEN/STATEST", &ok);
    if (ok) printf("  WAKEEN=%04x STATESTS=%04x (SDIN mask; bit2 = iDisp codec on TGL+)  ([HR]:28-29)\n", v & 0xFFFF, v >> 16); else NL();
    v = R(0, 0x10, "GSTS/GCAP2", &ok); if (ok) printf("  GSTS=%04x (FSTS b1=%u) GCAP2=%04x  ([HR]:30-32)\n", v & 0xFFFF, F(v,1,1), v >> 16); else NL();
    uint32_t llch = R(0, 0x14, "LLCH", &ok);
    if (ok) printf("  LLCH (first capability offset)=0x%04x  ([HR]:33)\n", llch & 0xFFFF); else { NL(); llch = 0; }
    v = R(0, 0x20, "INTCTL", &ok); if (ok) printf("  GIE(b31)=%u CIE(b30)=%u SIE[29:0]=%08x  ([HR]:36, [SH] SOF_HDA_INT_*)\n", F(v,31,31), F(v,30,30), F(v,29,0)); else NL();
    v = R(0, 0x24, "INTSTS", &ok); if (ok) printf("  GIS(b31)=%u CIS(b30)=%u SIS=%08x\n", F(v,31,31), F(v,30,30), F(v,29,0)); else NL();
    v = R(0, 0x30, "WALLCLK", &ok); NL();
    v = R(0, 0x38, "SSYNC", &ok); if (ok) printf("  stream sync mask=%08x  ([HR]:40)\n", v); else NL();
    // CORB/RIRB/IC/DPLBASE ([HR]:41-75): all plain RW/RO, status bytes are RW1C; pointer
    // registers (CORBRP, RIRBWP) only change on writes or DMA progress, never on a read.
    uint32_t lo, hi;
    lo = R(0, 0x40, "CORBLBASE", &ok); NL(); hi = R(0, 0x44, "CORBUBASE", &ok); NL();
    v = R(0, 0x48, "CORBWP/RP", &ok); if (ok) printf("  CORBWP=%02x CORBRP=%04x (RST b15=%u)\n", F(v,7,0), v >> 16, F(v,31,31)); else NL();
    v = R(0, 0x4C, "CORBCTL/STS/SZ", &ok); if (ok) printf("  CORB MEIE=%u RUN=%u  STS=%02x  SIZE=%02x\n", F(v,0,0), F(v,1,1), F(v,15,8), F(v,23,16)); else NL();
    lo = R(0, 0x50, "RIRBLBASE", &ok); NL(); hi = R(0, 0x54, "RIRBUBASE", &ok); NL();
    v = R(0, 0x58, "RIRBWP/RINTCNT", &ok); if (ok) printf("  RIRBWP=%02x RINTCNT=%04x\n", F(v,7,0), v >> 16); else NL();
    v = R(0, 0x5C, "RIRBCTL/STS/SZ", &ok); if (ok) printf("  RIRB INTCTL=%u DMA_EN=%u OIC=%u  STS=%02x  SIZE=%02x\n", F(v,0,0), F(v,1,1), F(v,2,2), F(v,15,8), F(v,23,16)); else NL();
    v = R(0, 0x68, "IRS (imm.cmd)", &ok); if (ok) printf("  ICB(b0)=%u IRV(b1)=%u  (IRV is RW1C, not read-to-clear)\n", F(v,0,0), F(v,1,1)); else NL();
    lo = R(0, 0x70, "DPLBASE", &ok); if (ok) printf("  position buffer enable(b0)=%u\n", F(lo,0,0)); else NL();
    hi = R(0, 0x74, "DPUBASE", &ok); NL();
    (void)lo; (void)hi;
    v = R(0, 0x1000, "EM1", &ok); NL();
    v = R(0, 0x1030, "EM2", &ok); if (ok) printf("  L1SEN(b13)=%u  ([SH]:313-314)\n", F(v,13,13)); else NL();
    v = R(0, 0x1048, "LTRP/D0I3C/PCE", &ok);
    if (ok) printf("  LTRP=%04x (GB[6:0]=%u) D0I3C=%02x (CIP b0=%u, I3 b2=%u) PCE=%02x  ([HR]:124-126,134, [SH]:77-81)\n",
                   v & 0xFFFF, F(v,6,0), F(v,23,16), F(v,16,16), F(v,18,18), F(v,31,24)); else NL();

    // Capability chain ([CT]:406-475, [HC]:76-125): header [31:28] ver, [27:16] id, [15:0] next ([HR]:193-197)
    printf("\n-- HDA capability chain from LLCH --\n");
    uint32_t off = llch & 0xFFFF;
    for (int n = 0; off && n <= 10; n++) {   // SOF_HDA_MAX_CAPS = 10 ([SH]:60)
        if (off & 3 || off >= 0x4000) { printf("  bad capability offset 0x%x, stop\n", off); break; }
        uint32_t h; if (mmio(0, off, &h)) { printf("  cap @0x%x unreadable\n", off); break; }
        if (h == 0xFFFFFFFF) { printf("  cap @0x%x reads all-ones, stop\n", off); break; }
        uint32_t id = F(h,27,16);
        const char *nm = id == 1 ? "GTS" : id == 2 ? "ML" : id == 3 ? "PP" : id == 4 ? "SPB(SPIB)" : id == 5 ? "DRSM" : "unknown";
        printf("  @0x%04x hdr=%08x id=%u (%s) ver=%u next=0x%04x\n", off, h, id, nm, F(h,31,28), F(h,15,0));
        if (id == 1) c->gts = off; else if (id == 2) c->ml = off; else if (id == 3) c->pp = off;
        else if (id == 4) c->spb = off; else if (id == 5) c->drsm = off;
        off = h & 0xFFFF;
    }

    if (c->pp) {   // [HR]:222-238, [SH]:69-74
        printf("\n-- PP (processing pipe, id 3) @0x%x --\n", c->pp);
        v = R(0, c->pp + 0x04, "PPCTL", &ok);
        if (ok) printf("  PIE(b31)=%u GPROCEN(b30)=%u%s PROCEN[29:0]=%08x\n", F(v,31,31), F(v,30,30),
                       F(v,30,30) ? "" : " (DSP not enabled: BAR4 may read 0/ones)", F(v,29,0)); else NL();
        v = R(0, c->pp + 0x08, "PPSTS", &ok); NL();
    }
    if (c->spb) {  // [HR]:200-211
        printf("\n-- SPB/SPIB (id 4) @0x%x --\n", c->spb);
        v = R(0, c->spb + 0x04, "SPBFCCTL", &ok); if (ok) printf("  SPIB enable mask=%08x\n", v); else NL();
        int ns = c->nstreams > 0 && c->nstreams <= 30 ? c->nstreams : 0;
        for (int i = 0; i < ns; i++) {
            uint32_t a, b; mmio(0, c->spb + 0x08 + 0x08 * i, &a); mmio(0, c->spb + 0x0C + 0x08 * i, &b);
            printf("    stream %2d SPIB=%08x MAXFIFOS=%08x\n", i, a, b);
        }
    }
    if (c->ml) {   // [HR]:259-320
        printf("\n-- ML (multi-link, id 2) @0x%x --\n", c->ml);
        uint32_t mlcd = R(0, c->ml + 0x04, "MLCD", &ok);
        // Upstream sound/hda/core/ext/controller.c (snd_hdac_ext_bus_get_ml_capabilities): num_links = MLCD + 1
        int nl = ok ? (int)(mlcd & 0xF) + 1 : 0;
        if (ok) printf("  link count = MLCD[3:0]+1 = %d\n", nl); else NL();
        for (int i = 0; i < nl && i < 8; i++) {
            uint32_t b = c->ml + 0x40 + 0x40 * i;  // AZX_ML_BASE/INTERVAL [HR]:262-263
            if (b + 0x24 > 0x4000) break;
            uint32_t lcap, lctl, losidv, lsdiid; int k = 0;
            k |= mmio(0, b + 0x00, &lcap); k |= mmio(0, b + 0x04, &lctl);
            k |= mmio(0, b + 0x08, &losidv); k |= mmio(0, b + 0x0C, &lsdiid);
            if (k) { printf("    link %d unreadable\n", i); continue; }
            printf("    link %d LCAP=%08x ALT(b28)=%u  LCTL=%08x SPA(b16)=%u CPA(b23)=%u SCF=%u  LOSIDV=%08x LSDIID=%04x\n",
                   i, lcap, F(lcap,28,28), lctl, F(lctl,16,16), F(lctl,23,23), F(lctl,3,0), losidv, lsdiid & 0xFFFF);
            if (F(lcap,28,28)) {  // extended link: LEPTR id 0x00 = SoundWire ([HR]:311-320); ACE2+ only
                uint32_t lep; mmio(0, b + 0x20, &lep);
                printf("           LEPTR=%08x id=0x%02x (%s) ver=%u ptr=0x%05x\n", lep, F(lep,31,24),
                       F(lep,31,24) == 0 ? "SoundWire" : F(lep,31,24) == 0xC0 ? "SSP" : F(lep,31,24) == 0xC1 ? "DMIC" : "?",
                       F(lep,23,20), F(lep,19,0));
            }
        }
        printf("  (cAVS 2.5: SoundWire is NOT an HDA ML extended link; it lives in BAR4 at 0x2C000. ALT=1/LEPTR\n"
               "   SoundWire links appear only on ACE 2.x, cf. [SI]:121-123 SHIM2 vs [SI]:15-20.)\n");
    }
    if (c->gts) {  // [HR]:214-219
        printf("\n-- GTS (id 1) @0x%x --\n", c->gts);
        R(0, c->gts + 0x04, "GTSCD", NULL); NL();
        R(0, c->gts + 0x0C, "GTSCTLAC", NULL); NL();
    }
    if (c->drsm) { // [HR]:323-328
        printf("\n-- DRSM (id 5) @0x%x --\n", c->drsm);
        R(0, c->drsm + 0x04, "DRSM_CTL", NULL); NL();
    }

    // Stream descriptors: SD n at 0x80 + 0x20*n ([SH]:120-137), order = ISS inputs, OSS outputs, BSS.
    if (c->nstreams > 0 && c->nstreams <= 30) {
        printf("\n-- stream descriptors (0x80 + 0x20*n) --\n");
        for (int i = 0; i < c->nstreams; i++) {
            uint32_t b = 0x80 + 0x20 * i, ctl, lpib, cbl, lvi, fmt, bl, bu;
            mmio(0, b + 0x00, &ctl); mmio(0, b + 0x04, &lpib); mmio(0, b + 0x08, &cbl);
            mmio(0, b + 0x0C, &lvi); mmio(0, b + 0x10, &fmt); mmio(0, b + 0x18, &bl); mmio(0, b + 0x1C, &bu);
            // SD regs [HR]:82-92; CTL: SRST b0, RUN b1, IOCE b2, STRIPE[17:16], DIR b19 (bidir), STRM/TAG [23:20];
            // STS (byte 3): BCIS b2, FIFOE b3, DESE b4, FIFORDY b5 (RW1C, no read side effect)
            printf("    SD%-2d CTL/STS=%08x (SRST=%u RUN=%u IOCE=%u TAG=%u STS=%02x) LPIB=%08x CBL=%08x LVI=%04x FIFOW=%04x FIFOS=%04x FMT=%04x BDL=%08x:%08x\n",
                   i, ctl, F(ctl,0,0), F(ctl,1,1), F(ctl,2,2), F(ctl,23,20), F(ctl,31,24), lpib, cbl, lvi & 0xFFFF, lvi >> 16,
                   fmt & 0xFFFF, fmt >> 16, bu, bl);
        }
    }
}

static void m1_bar4(void) {
    int ok; uint32_t v;
    printf("\n== BAR4: DSP (cAVS 2.5) ==\n");
    // ADSPCS [SH]:295, fields [SH]:352-374. Four cores ([TG]:140), only core 0 host-managed ([TG]:142).
    uint32_t cs = R(4, 0x04, "ADSPCS", &ok);
    int cpa0 = 0;
    if (ok) {
        printf("  CRST[3:0]=%x CSTALL[11:8]=%x SPA[23:16]=%x CPA[31:24]=%x\n", F(cs,3,0), F(cs,11,8), F(cs,23,16), F(cs,31,24));
        for (int k = 0; k < 4; k++)
            printf("    core %d: %s%s%s%s\n", k, F(cs,24+k,24+k) ? "POWERED" : "off",
                   F(cs,16+k,16+k) ? " SPA" : "", F(cs,k,k) ? " in-reset" : "", F(cs,8+k,8+k) ? " stalled" : "");
        cpa0 = F(cs,24,24);
    } else NL();
    v = R(4, 0x08, "ADSPIC", &ok);  if (ok) printf("  IPC(b0)=%u CL_DMA(b1)=%u  ([SH]:290,337)\n", F(v,0,0), F(v,1,1)); else NL();
    v = R(4, 0x0C, "ADSPIS", &ok);  if (ok) printf("  IPC(b0)=%u CL_DMA(b1)=%u\n", F(v,0,0), F(v,1,1)); else NL();
    v = R(4, 0x10, "ADSPIC2", &ok); if (ok) printf("  SNDW(b5)=%u  ([SH]:298,301)\n", F(v,5,5)); else NL();
    v = R(4, 0x14, "ADSPIS2", &ok); if (ok) printf("  SNDW(b5)=%u  ([SH]:299,302)\n", F(v,5,5)); else NL();

    printf("\n-- CNL-style IPC (used on cAVS 2.5; [SH]:383-410, [TG]:143-147) --\n");
    v = R(4, 0xC0, "HIPCTDR", &ok); if (ok) printf("  BUSY(b31)=%u msg=%08x  (DSP->host request)\n", F(v,31,31), v & 0x7FFFFFFF); else NL();
    v = R(4, 0xC4, "HIPCTDA", &ok); if (ok) printf("  DONE(b31)=%u msg=%08x\n", F(v,31,31), v & 0x7FFFFFFF); else NL();
    v = R(4, 0xC8, "HIPCTDD", &ok); NL();
    v = R(4, 0xD0, "HIPCIDR", &ok); if (ok) printf("  BUSY(b31)=%u msg=%08x  (host->DSP request)\n", F(v,31,31), v & 0x7FFFFFFF); else NL();
    v = R(4, 0xD4, "HIPCIDA", &ok); if (ok) printf("  DONE(b31)=%u msg=%08x\n", F(v,31,31), v & 0x7FFFFFFF); else NL();
    v = R(4, 0xD8, "HIPCIDD", &ok); NL();
    v = R(4, 0xE8, "HIPCCTL", &ok); if (ok) printf("  BUSY-IE(b0)=%u DONE-IE(b1)=%u\n", F(v,0,0), F(v,1,1)); else NL();
    printf("-- legacy SKL-layout IPC @0x40 ([SH]:305-310; NOT used on cAVS 2.5, for comparison only) --\n");
    R(4, 0x40, "HIPCT(skl)", NULL);  NL();
    R(4, 0x44, "HIPCTE(skl)", NULL); NL();
    R(4, 0x48, "HIPCI(skl)", NULL);  NL();
    R(4, 0x4C, "HIPCIE(skl)", NULL); NL();
    R(4, 0x50, "HIPCCTL(skl)", NULL); NL();

    // SoundWire shim ([SI]:15, 24-106)
    printf("\n-- SoundWire shim @BAR4+0x2C000 ([SI]:15,24-106) --\n");
    uint32_t S = 0x2C000;
    uint32_t lcap = R(4, S + 0x00, "LCAP", &ok);
    int lcount = 0;
    if (ok) { lcount = F(lcap,2,0); printf("  LCOUNT[2:0]=%d (expect 4) MLCS(b8)=%u\n", lcount, F(lcap,8,8)); } else NL();
    uint32_t lctl = R(4, S + 0x04, "LCTL", &ok);
    if (ok) printf("  SPA[3:0]=%x CPA[11:8]=%x MLCS[29:27]=%u (0 XTAL,1 cardinal,2 audio PLL)\n", F(lctl,3,0), F(lctl,11,8), F(lctl,29,27)); else { NL(); lctl = 0; }
    v = R(4, S + 0x0C, "SYNC", &ok);
    if (ok) printf("  SYNCPRD[14:0]=%u (9599 = 38.4 MHz/4 kHz-1) SYNCCPU(b15)=%u CMDSYNC[19:16]=%x SYNCGO(b24)=%u\n",
                   F(v,14,0), F(v,15,15), F(v,19,16), F(v,24,24)); else NL();
    if (lcount < 1 || lcount > 4) lcount = 4;
    for (int x = 0; x < lcount; x++) {
        uint32_t b = S + 0x60 * x, ctls, pcms, io;
        int k = mmio(4, b + 0x10, &ctls) | mmio(4, b + 0x20, &pcms) | mmio(4, b + 0x6C, &io);
        if (k) { printf("  link %d: shim regs refused\n", x); continue; }
        uint32_t ioctl = io & 0xFFFF, ctm = io >> 16;
        printf("  link %d CTLSCAP=%04x PCMSCAP=%04x (ISS=%u OSS=%u BSS=%u)\n", x, ctls & 0xFFFF, pcms & 0xFFFF,
               F(pcms,3,0), F(pcms,7,4), F(pcms,12,8));
        printf("         IOCTL=%04x MIF=%u CO=%u COE=%u DO=%u DOE=%u BKE=%u WPDD=%u CIBD=%u DIBD=%u\n", ioctl,
               F(ioctl,0,0), F(ioctl,1,1), F(ioctl,2,2), F(ioctl,3,3), F(ioctl,4,4), F(ioctl,5,5), F(ioctl,6,6), F(ioctl,8,8), F(ioctl,9,9));
        printf("         CTMCTL=%04x DACTQE=%u DODS=%u DOAIS=%u\n", ctm, F(ctm,0,0), F(ctm,1,1), F(ctm,4,3));
        int np = F(pcms,3,0) + F(pcms,7,4) + F(pcms,12,8);
        if (np > 0 && np <= 16) {  // PCMSyCHM(x,y)=0x22+0x60x+2y, PCMSyCHC(x,y)=0x42+0x60x+2y ([SI]:68-76)
            printf("         PDI:");
            for (int y = 0; y < np; y++) {
                uint32_t a, c; mmio(4, (b + 0x22 + 2 * y) & ~3u, &a); mmio(4, (b + 0x42 + 2 * y) & ~3u, &c);
                uint32_t chm = ((b + 0x22 + 2 * y) & 2) ? a >> 16 : a & 0xFFFF;
                uint32_t chc = ((b + 0x42 + 2 * y) & 2) ? c >> 16 : c & 0xFFFF;
                printf(" %d:CHM=%04x/CHC=%04x", y, chm, chc);
            }
            printf("\n");
        }
    }
    v = R(4, S + 0x190, "WAKEEN/WAKESTS", &ok);
    if (ok) printf("  WAKEEN=%04x WAKESTS=%04x  ([SI]:92-99)\n", v & 0xFFFF, v >> 16); else NL();

    // ALH ([SI]:16,109-114). Expected ids: link*16 + pdi + 3 (+2 if pdi >= 2) (drivers/soundwire/intel.c intel_pdi_alh_configure)
    printf("\n-- ALH STRMZCFG @BAR4+0x2C800 (nonzero entries of 64) --\n  ");
    int nz = 0;
    for (int id = 0; id < 64; id++) {
        uint32_t a; if (mmio(4, 0x2C800 + 4 * id, &a)) { printf("refused "); break; }
        if (a) { printf("id%02d=%08x(DMAT=%u CHN=%u) ", id, a, F(a,7,0), F(a,19,16)); nz++; }
    }
    printf("%s\n", nz ? "" : "(all zero)");

    // Cadence link registers ([CD]:30-134), only for links with LCTL.CPA set (the kext enforces this).
    printf("\n-- Cadence SoundWire masters @BAR4+0x30000+0x10000*n ([SI]:19-20, [CD]:30-130) --\n");
    static const struct { uint32_t o; const char *n; } cr[] = {
        {0x00, "MCP_CONFIG"}, {0x04, "MCP_CONTROL"}, {0x08, "MCP_CMDCTRL"}, {0x0C, "MCP_SSPSTAT"},
        {0x10, "FRAME_SHAPE"}, {0x14, "FRAME_SHAPE_IN"}, {0x18, "CONFIG_UPDATE"}, {0x1C, "PHYCTRL"},
        {0x30, "CLK_CTRL0"}, {0x38, "CLK_CTRL1"}, {0x40, "MCP_STAT"}, {0x44, "MCP_INTSTAT"},
        {0x48, "MCP_INTMASK"}, {0x50, "SLAVE_STAT"}, {0x54, "SLAVE_INTSTAT0"}, {0x58, "SLAVE_INTSTAT1"},
        {0x64, "PORT_INTSTAT"}, {0x78, "FIFOLEVEL"}, {0x7C, "FIFOSTAT"},
    };  // 0x80..0xFF (CMD/RESP FIFO) deliberately never read: reads pop responses ([CD]:133-137)
    for (int n = 0; n < lcount; n++) {
        if (!F(lctl, 8 + n, 8 + n)) { printf("  link %d: LCTL.CPA=0 (powered down) -> not read\n", n); continue; }
        printf("  link %d:\n", n);
        for (unsigned i = 0; i < sizeof cr / sizeof cr[0]; i++) {
            uint32_t a = R(4, 0x30000 + 0x10000 * n + cr[i].o, cr[i].n, &ok);
            if (!ok) continue;
            if (cr[i].o == 0x50) {
                printf("  dev:");
                for (int d = 0; d < 12; d++) {
                    static const char *st[] = {"-", "ATT", "ALERT", "rsv"};
                    printf(" %d=%s", d, st[F(a, 2*d+1, 2*d)]);
                }
            } else if (cr[i].o == 0x40) {
                printf("  ACTIVE_BANK(b20)=%u CLK_STOP(b16)=%u", F(a,20,20), F(a,16,16));
            } else if (cr[i].o == 0x04) {
                printf("  CMD_RST(b7)=%u SOFT_RST(b6)=%u HW_RST(b4)=%u CMD_ACCEPT(b1)=%u BLOCK_WAKEUP(b0)=%u", F(a,7,7), F(a,6,6), F(a,4,4), F(a,1,1), F(a,0,0));
            } else if (cr[i].o == 0x14 || cr[i].o == 0x10) {
                printf("  rows-code[7:3]=%u cols-code[2:0]=%u", F(a,7,3), F(a,2,0));
            }
            printf("\n");
        }
    }

    // SRAM window 0 ([SH]:162-175, FSR decode [SH]:195-226). Only if core 0 is powered.
    printf("\n-- SRAM window 0 (ROM/FW status) @BAR4+0x80000 --\n");
    if (!cpa0) { printf("  ADSPCS.CPA0=0 -> not read (DSP core 0 powered down)\n"); return; }
    v = R(4, 0x80000, "ROM_STATUS", &ok);
    if (ok) printf("  state[23:0]=0x%x wait[27:24]=%u module[30:28]=%u halted(b31)=%u  (1=INIT_DONE, 5=FW_ENTERED)\n",
                   F(v,23,0), F(v,27,24), F(v,30,28), F(v,31,31)); else NL();
    R(4, 0x80004, "ROM_ERROR", NULL); NL();
}

static void flush_to_disk(void) {
    fflush(stdout);
    fsync(STDOUT_FILENO);  // EINVAL on a tty/pipe is fine; on a file it pushes the dump to disk
}

static void cmd_m1(int hda_only) {
    struct caps c = {0};
    printf("AudShim M1 dump (read-only; audctl 0.8.0)\n");
    cmd_info();
    m1_cfg();
    m1_bar0(&c);
    printf("\n-- raw BAR0 0x000-0x0FF --\n");
    hexdump(0, 0x000, 64);
    flush_to_disk();
    if (hda_only) { printf("\nend of M1 dump (hda only: BAR4 not touched)\n"); return; }
    if (c.pp) {
        uint32_t ppctl;
        if (mmio(0, c.pp + 0x04, &ppctl) == 0 && !F(ppctl,30,30))
            printf("\nnote: PPCTL.GPROCEN=0. Linux never reads BAR4 before setting GPROCEN (hda.c:906-912);\n"
                   "      values below show what the DSP BAR returns while the DSP side is not enabled.\n");
    }
    m1_bar4();
    printf("\n-- raw BAR4 0x000-0x0FF --\n");
    hexdump(4, 0x000, 64);
    printf("\nend of M1 dump\n");
}

// ------------------------------------------------------------------ m2
// DSP core 0 power-up and ROM handshake (RESEARCH.md section 4, M2). Linux references:
//   [HD] sound/soc/sof/intel/hda.c      [HX] hda-ctrl.c      [DS] hda-dsp.c
//   [LD] hda-loader.c                   [TG] tgl.c           [SH] hda.h
// No DMA stream, no BME, no firmware, no interrupts. The kext enforces order and values.

enum {  // must match AudShim.cpp
    M2_STATE = 0, M2_PRECHECK = 1, M2_ARM = 2, M2_TCSEL = 3, M2_CGCTL = 4, M2_PGCTL = 5,
    M2_SPA = 6, M2_IPC = 7, M2_CRST = 8, M2_CSTALL = 9, M2_IPCDONE = 10, M2_ROMPOLL = 11,
    M2_ROLLBACK = 12, M2_FINISH = 13, M2_DISARM = 14,
    M2B_BME = 15, M2B_CTRLINIT = 16, M2B_PPCAP = 17, M2B_L1SEN = 18, M2B_DMAALLOC = 19, M2B_ICCMAX = 20,
    M2B_CLPREP = 21, M2B_SSP = 22, M2B_COREOFF = 23, M2B_INFO = 24,
    M3_FWLOAD = 25, M3_CLSTART = 26, M3_CLSTOP = 27, M3_FWREADY = 28, M3_TDACK = 29, M3_POSTFW = 30, M3_INFO = 31,
};
enum { M2R_OK = 0, M2R_TIMEOUT = 1, M2R_NOTARMED = 2, M2R_PRECOND = 3, M2R_BADOP = 4, M2R_VERIFY = 5, M2R_BUSY = 6, M2R_NOMAP = 7, M2R_NOMEM = 8, M2R_DMAERR = 9 };
#define MAGIC_RUN  0x4D32474FULL   /* 'M2GO' */
#define MAGIC_UNDO 0x554E444FULL   /* 'UNDO' */
#define MAGIC_M2B  0x4D324247ULL   /* 'M2BG' */
#define MAGIC_M3   0x4D33474FULL   /* 'M3GO' */
#define ROM_CTL_MSG 0x81004000u    /* BUSY | ROM_CONTROL | PURGE_FW | (tag 1 - 1) << 9: [LD]:87-89, [SH]:276-277 */

static const char *m2rc(uint64_t r) {
    static const char *n[] = {"ok", "TIMEOUT", "refused: not armed", "refused: precondition", "bad op",
                              "verify FAILED", "refused: another connection is armed", "no writable BAR map",
                              "out of memory", "DMA mapping error"};
    return r < 10 ? n[r] : "?";
}

static int m2call(uint64_t op, uint64_t a0, uint64_t a1, uint64_t o[16]) {
    uint64_t in[3] = {op, a0, a1}; uint32_t n = 16;
    memset(o, 0, 16 * sizeof(uint64_t));
    kern_return_t kr = IOConnectCallScalarMethod(conn, kM2, in, 3, o, &n);
    if (kr) { printf("  !! kext method 4 (M2) failed: 0x%x (kext older than 0.5.0?)\n", kr); return -1; }
    return 0;
}

static const char *fsr_state(uint32_t st) {   // [SH]:221-248
    switch (st) {
    case 0x0: return "INIT (ROM starting)";
    case 0x1: return "INIT_DONE (ROM ready for the code loader)";
    case 0x2: return "CSE_MANIFEST_LOADED";
    case 0x3: return "FW_MANIFEST_LOADED";
    case 0x4: return "FW_FW_LOADED";
    case 0x5: return "FW_ENTERED";
    case 0x6: return "VERIFY_FEATURE_MASK";
    case 0x7: return "GET_LOAD_OFFSET";
    case 0x8: return "FETCH_ROM_EXT";
    case 0x9: return "FETCH_ROM_EXT_DONE";
    case 0xf: return "BASEFW_ENTERED (SKL)";
    case 0x10: return "CSE_IMR_REQUEST";
    case 0x11: return "CSE_IMR_GRANTED";
    case 0x12: return "CSE_VALIDATE_IMAGE_REQUEST";
    case 0x13: return "CSE_IMAGE_VALIDATED";
    case 0x20: return "CSE_IPC_IFACE_INIT";
    case 0x21: return "CSE_IPC_RESET_PHASE_1";
    case 0x22: return "CSE_IPC_OPERATIONAL_ENTRY";
    case 0x23: return "CSE_IPC_OPERATIONAL";
    case 0x24: return "CSE_IPC_DOWN";
    default: return "unknown";
    }
}
static const char *fsr_wait(uint32_t w) {     // [SH]:204-209
    static const char *n[] = {"none", "WAIT_FOR_IPC_BUSY", "WAIT_FOR_IPC_DONE", "WAIT_FOR_CACHE_INVALIDATION",
                              "WAIT_FOR_LP_SRAM_OFF", "WAIT_FOR_DMA_BUFFER_FULL", "WAIT_FOR_CSE_CSR"};
    return w < 7 ? n[w] : "?";
}
static const char *fsr_module(uint32_t m) {   // [SH]:212-217
    static const char *n[] = {"ROM", "ROM_BYP", "BASE_FW", "LP_BOOT", "BRNGUP", "ROM_EXT", "?", "?"};
    return n[m & 7];
}
static const char *rom_error(uint32_t e) {    // [SH]:259-274
    switch (e) {
    case 0: return "none";
    case 40: return "CSE_ERROR";
    case 41: return "CSE_WRONG_RESPONSE";
    case 42: return "IMR_TO_SMALL";
    case 43: return "BASE_FW_NOT_FOUND";
    case 44: return "CSE_VALIDATION_FAILED";
    case 45: return "IPC_FATAL_ERROR";
    case 46: return "L2_CACHE_ERROR";
    case 47: return "LOAD_OFFSET_TO_SMALL";
    case 50: return "API_PTR_INVALID";
    case 51: return "BASEFW_INCOMPAT";
    case 0x4c4c4e55: return "NULL_FW_ENTRY";
    }
    switch (e & 0xFFFFF000u) {
    case 0xBEE00000: return "UNHANDLED_INTERRUPT";
    case 0xECC00000: return "MEMORY_HOLE_ECC";
    case 0xCAFE0000: return "KERNEL_EXCEPTION";
    case 0xBEEF0000: return "USER_EXCEPTION";
    case 0xDECAF000: return "UNEXPECTED_RESET";
    }
    return "unknown";
}
static void print_fsr(const char *tag, uint32_t s) {
    printf("%s0x%08x  state[23:0]=0x%x %s | wait[27:24]=%u %s | module[30:28]=%u %s | halted(b31)=%u\n",
           tag, s, s & 0xFFFFFF, fsr_state(s & 0xFFFFFF), (s >> 24) & 0xF, fsr_wait((s >> 24) & 0xF),
           (s >> 28) & 7, fsr_module((s >> 28) & 7), s >> 31);
}
static void print_adspcs(const char *tag, uint32_t cs) {
    printf("%s0x%08x  CRST=%x CSTALL=%x SPA=%x CPA=%x  core0: %s%s%s%s\n", tag, cs, cs & 0xF, (cs >> 8) & 0xF,
           (cs >> 16) & 0xF, (cs >> 24) & 0xF, (cs >> 24) & 1 ? "POWERED" : "off", (cs >> 16) & 1 ? " SPA" : "",
           cs & 1 ? " in-reset" : " out-of-reset", (cs >> 8) & 1 ? " stalled" : " RUNNING");
}

// --- pre-checks (read-only, kext method 4 op 1)
static int g_pre_m2b = 0;   // 1: also run the kext's M2b pre-checks (SPIB, stream layout/idle)
static uint32_t m2_precheck(uint64_t p[16], int verbose) {
    if (m2call(M2_PRECHECK, g_pre_m2b, 0, p)) return 0xFFFFFFFFu;
    uint32_t f = (uint32_t)p[11];
    if (!verbose) return f;
#define CK(bit, txt, ...) printf("  [%s] " txt "\n", (f & (bit)) ? "FAIL" : " ok ", ##__VA_ARGS__)
    printf("pre-checks (read-only):\n");
    CK(1u << 0, "PCI id 0x%08llx == 0x51ca8086", p[1]);
    CK(1u << 1, "power state D%llu (PMCSR 0x%08llx) == D0", p[3] & 3, p[3]);
    CK(1u << 2, "command 0x%04llx MSE=1", p[2] & 0xFFFF);
    CK(1u << 3, "command BME=%llu (must stay 0: no DMA in M2)", (p[2] >> 2) & 1);
    CK(1u << 7, "BAR0/BAR4 mapped in the kext");
    if (f & ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 7))) return f;
    CK(1u << 4, "GCTL 0x%08llx CRST=1 (controller out of reset)  [HD]:Linux leaves it 1 after init_chip", p[4]);
    CK(1u << 8, "PP capability found @0x%llx", p[12]);
    CK(1u << 5, "PPCTL 0x%08llx GPROCEN=1 (DSP side enabled; Linux: hda-ctrl.c ppcap_enable)", p[5]);
    CK(1u << 6, "ADSPCS 0x%08llx: core0 SPA=0 CPA=0 CRST=1 CSTALL=1 (M1 state; [HD]:428-436 would power down otherwise)", p[6]);
    CK(1u << 9, "cfg44 0x%02llx ADSPPGD=0, cfg48 0x%02llx ADSPDCGE=1 (gating at BIOS/M1 defaults)", p[7], p[8]);
    if (g_pre_m2b) {
        CK(1u << 10, "SPIB capability found (SPIB cap @0x%llx)", p[15] & 0xFFFF);
        CK(1u << 11, "GCAP ISS=%llu OSS=%llu usable; SD0 (ICCMAX) and SD%llu (code loader) idle: RUN=0 SRST=0 BDL=0",
           (p[15] >> 16) & 0xF, (p[15] >> 20) & 0xF, (p[15] >> 16) & 0xF);
    }
    printf("  [info] HIPCIDR 0x%08llx (BUSY=%llu)  HIPCIDA 0x%08llx (DONE=%llu)  HIPCTDR 0x%08llx\n",
           p[9], (p[9] >> 31) & 1, p[10], (p[10] >> 31) & 1, p[13]);
#undef CK
    return f;
}

// --- plan (also used by --dry)
static void m2_plan(uint32_t c44, uint32_t c48, uint32_t cs, const char *ipcmode, unsigned romms, int live) {
    uint32_t a44 = c44 & ~7u, a48 = c48 & ~2u, b44 = a44 | 4u;
    uint32_t cs1 = cs | (1u << 16), cs2 = cs1 | (1u << 24), cs3 = cs2 & ~1u, cs4 = cs3 & ~(1u << 8);
    printf("M2 planned writes (%s values; ipc=%s, ROM poll %u ms). Every write is RMW of the listed bits only.\n",
           live ? "live" : "M1-dump", ipcmode, romms);
    printf("  #  target              off      before      -> after        Linux reference\n");
    if (c44 & 7)
        printf("  W1 cfg byte TCSEL[2:0]=0  0x44     0x%02x        -> 0x%02x         hda.c:903 (probe; skipped when already 0)\n", c44, a44);
    else
        printf("  W1 cfg byte TCSEL[2:0]=0  0x44     0x%02x        -> (no write: already 0)  hda.c:903\n", c44);
    printf("  W2 cfg byte CGCTL.ADSPDCGE=0 0x48  0x%02x        -> 0x%02x         hda.c:439 -> hda-ctrl.c:169-170\n", c48, a48);
    printf("     (skipped on purpose: BAR0 EM2.L1SEN=0, hda-ctrl.c:173-176, only matters for host DMA)\n");
    printf("  W3 cfg byte PGCTL.ADSPPGD=1  0x44  0x%02x        -> 0x%02x         hda.c:439 -> hda-ctrl.c:179-180\n", a44, b44);
    printf("  W4 BAR4 ADSPCS SPA0=1     0x00004  0x%08x  -> 0x%08x   hda-loader.c:77 -> hda-dsp.c:295-297\n", cs, cs1);
    printf("     poll CPA0 (b24)=1, 50 ms (HDA_DSP_RESET_TIMEOUT_US hda.h:282)    expect 0x%08x  hda-dsp.c:300-305\n", cs2);
    printf("     (skipped on purpose: SSP SSC1 CBP/CFP writes, hda-loader.c:84,34-49; SSP unused on this machine)\n");
    if (!strcmp(ipcmode, "first"))
        printf("  W5 BAR4 HIPCIDR           0x000d0  (any)       -> 0x%08x   hda-loader.c:87-91 (Linux order: before core run)\n", ROM_CTL_MSG);
    printf("  W6 BAR4 ADSPCS CRST0=0    0x00004  0x%08x  -> 0x%08x   hda-loader.c:94 -> hda-dsp.c:253,176-179\n", cs2, cs3);
    printf("     poll CRST0=0, 50 ms                                               hda-dsp.c:182-187\n");
    printf("  W7 BAR4 ADSPCS CSTALL0=0  0x00004  0x%08x  -> 0x%08x   hda-dsp.c:259-262; check enabled 264-265,222-246\n", cs3, cs4);
    if (!strcmp(ipcmode, "first")) {
        printf("     poll HIPCIDA (0xd4) DONE(b31)=1, 500 ms (HDA_DSP_INIT_TIMEOUT_US hda.h:284)  hda-loader.c:104-109\n");
        printf("  W8 BAR4 HIPCIDA |= DONE   0x000d4  (reply)     -> reply|0x80000000  hda-loader.c:120-123 (write-1-to-clear)\n");
    }
    printf("     poll ROM status BAR4+0x80000 state[23:0]==1 (INIT_DONE), %u ms (Linux: 300, tgl.c:149)  hda-loader.c:148-153, hda.h:170,195-199\n", romms);
    if (!strcmp(ipcmode, "auto")) {
        printf("     ONLY IF the ROM did not report INIT_DONE, is not halted and has no ROM error\n"
               "     (state INIT=0 or wait=WAIT_FOR_IPC_BUSY), send the cl_dsp_init ROM message:\n");
        printf("  W5 BAR4 HIPCIDR           0x000d0  (any)       -> 0x%08x   hda-loader.c:87-91 (BUSY|ROM_CONTROL|PURGE_FW|tag1)\n", ROM_CTL_MSG);
        printf("     poll HIPCIDA DONE(b31)=1, 500 ms                                 hda-loader.c:104-109\n");
        printf("  W8 BAR4 HIPCIDA |= DONE   0x000d4  (reply)     -> reply|0x80000000  hda-loader.c:120-123\n");
        printf("     poll ROM status for INIT_DONE again, %u ms                     hda-loader.c:148-153\n", romms);
    }
    if (!strcmp(ipcmode, "never"))
        printf("     (ipc=never: the ROM_CONTROL message is never sent)\n");
    printf("  then: read-only report (ADSPCS, IPC, ROM status/error, SRAM window 0, SoundWire LCAP/LCTL)\n");
    printf("  success: core 0 is LEFT at the ROM (unless --teardown); run `audctl m2 --undo` afterwards\n");
    printf("rollback (on any failure, on --undo, or by the kext if audctl dies mid-sequence):\n");
    printf("  R0 BAR4 HIPCIDA |= DONE   0x000d4  only if a ROM reply is pending       hda-loader.c:120-123\n");
    printf("  R1 BAR4 ADSPCS CSTALL0=1  0x00004  -> e.g. 0x%08x                    hda-loader.c:178 -> hda-dsp.c:212-215\n", cs3 | (1u << 8));
    printf("  R2 BAR4 ADSPCS CRST0=1    0x00004  -> e.g. 0x%08x, poll 50 ms         hda-dsp.c:136-147\n", cs2);
    printf("  R3 BAR4 ADSPCS SPA0=0     0x00004  -> 0x%08x, poll CPA0=0 50 ms       hda-dsp.c:334-341 (HDA_DSP_PD_TIMEOUT hda.h:281)\n", cs);
    printf("  R4 cfg byte 0x44 bits[2:0] back to 0x%02x (ADSPPGD=%u)                    hda-ctrl.c:179-180 (enable, hda.c:471)\n", c44 & 7, (c44 >> 2) & 1);
    printf("  R5 cfg byte 0x48 bit1 back to %u (ADSPDCGE)                              hda-ctrl.c:169-170 (enable, hda.c:471)\n", (c48 >> 1) & 1);
    printf("Nothing else is written: no BAR0 write, no BME, no stream/BDL/SPIB, no INTCTL/ADSPIC/HIPCCTL, no SoundWire.\n");
}

static void m2_report(int full) {
    uint32_t v; int ok;
    printf("\n-- M2 final state --\n");
    v = R(4, 0x04, "ADSPCS", &ok); if (ok) print_adspcs("  ", v); else NL();
    int cpa0 = ok && (v >> 24) & 1;
    R(4, 0x0C, "ADSPIS", NULL); NL();
    R(4, 0x14, "ADSPIS2", NULL); NL();
    v = R(4, 0xC0, "HIPCTDR", &ok); if (ok) printf("  BUSY=%u msg=0x%08x\n", v >> 31, v & 0x7FFFFFFF); else NL();
    v = R(4, 0xC4, "HIPCTDA", &ok); NL();
    v = R(4, 0xC8, "HIPCTDD", &ok); NL();
    v = R(4, 0xD0, "HIPCIDR", &ok); if (ok) printf("  BUSY=%u msg=0x%08x\n", v >> 31, v & 0x7FFFFFFF); else NL();
    v = R(4, 0xD4, "HIPCIDA", &ok); if (ok) printf("  DONE=%u msg=0x%08x\n", v >> 31, v & 0x7FFFFFFF); else NL();
    v = R(4, 0xD8, "HIPCIDD", &ok); NL();
    v = R(4, 0xE8, "HIPCCTL", &ok); NL();
    if (cpa0) {
        v = R(4, 0x80000, "ROM_STATUS", &ok); if (ok) print_fsr("  ", v); else NL();
        v = R(4, 0x80004, "ROM_ERROR", &ok); if (ok) printf("  %s (%u)\n", rom_error(v), v); else NL();
        if (full) { printf("  SRAM window 0 (BAR4+0x80000), first 0x40 bytes:\n"); hexdump(4, 0x80000, 16); }
    } else {
        printf("  ROM_STATUS     not read (core 0 has no power: CPA0=0)\n");
    }
    v = R(4, 0x2C000, "SDW LCAP", &ok);
    if (ok) printf("  LCOUNT[2:0]=%u (expect 4) MLCS(b8)=%u\n", v & 7, (v >> 8) & 1); else NL();
    v = R(4, 0x2C004, "SDW LCTL", &ok);
    if (ok) printf("  SPA[3:0]=%x CPA[11:8]=%x\n", v & 0xF, (v >> 8) & 0xF); else NL();
    R(4, 0x2C00C, "SDW SYNC", NULL); NL();
    if (full) {
        for (int x = 0; x < 4; x++) {
            uint32_t a, b;
            if (mmio(4, 0x2C010 + 0x60 * x, &a) | mmio(4, 0x2C020 + 0x60 * x, &b)) continue;
            printf("  SDW link %d CTLSCAP=%04x PCMSCAP=%04x (ISS=%u OSS=%u BSS=%u)\n", x, a & 0xFFFF, b & 0xFFFF,
                   b & 0xF, (b >> 4) & 0xF, (b >> 8) & 0x1F);
        }
    }
    uint32_t c44, c48; cfg(0x44, &c44); cfg(0x48, &c48);
    printf("  cfg44 byte 0x%02x (ADSPPGD=%u)  cfg48 byte 0x%02x (ADSPDCGE=%u)\n", c44 & 0xFF, (c44 >> 2) & 1, c48 & 0xFF, (c48 >> 1) & 1);
    uint32_t cmd; cfg(0x04, &cmd);
    printf("  PCI cmd/sts 0x%08x  MSE=%u BME=%u\n", cmd, (cmd >> 1) & 1, (cmd >> 2) & 1);
}

static void print_rom_poll(const char *what, uint64_t o[16]) {
    printf("     %s: %s after ~%llu us (reached=%llu)\n", what, m2rc(o[0]), o[3], o[4]);
    for (uint64_t i = 0; i < o[5] && i < 4; i++) {
        char tag[48]; snprintf(tag, sizeof tag, "       t=%6llu us  ", o[7 + 2 * i]);
        print_fsr(tag, (uint32_t)o[6 + 2 * i]);
    }
    print_fsr("       final      ", (uint32_t)o[1]);
    if (o[2] != 0xFFFFFFFFull) printf("       ROM_ERROR  0x%08llx %s\n", o[2], rom_error((uint32_t)o[2]));
    if (o[15] & 1) printf("       stopped early: HIPCTDR.BUSY already set (0x%08llx): the firmware is already talking\n", o[14]);
    if (o[15] & 2) printf("       !! core 0 lost power (CPA0=0) during the poll\n");
}

static int m2_rollback(const char *why) {
    uint64_t o[16];
    printf("\n== rollback (%s) ==\n", why);
    if (m2call(M2_ROLLBACK, 0, 0, o)) return -1;
    printf("  result %s, %llu write(s)\n", m2rc(o[0]), o[8]);
    print_adspcs("  ADSPCS before  ", (uint32_t)o[1]);
    print_adspcs("  ADSPCS after   ", (uint32_t)o[2]);
    printf("  cfg44 0x%02llx -> 0x%02llx   cfg48 0x%02llx -> 0x%02llx%s\n", o[3] >> 8, o[3] & 0xFF, o[4] >> 8, o[4] & 0xFF,
           (o[5] & 8) ? "  (no snapshot in the kext: restored to M1 defaults ADSPPGD=0, ADSPDCGE=1)" : "");
    if (o[5] & 4) printf("  a pending ROM reply in HIPCIDA was acknowledged\n");
    if (o[5] & 1) printf("  !! CRST0 did not read back 1 within 50 ms\n");
    if (o[5] & 2) printf("  !! CPA0 did not drop within 50 ms (core 0 still powered; cfg left with gating disabled)\n");
    printf("  HIPCIDR 0x%08llx HIPCIDA 0x%08llx\n", o[6], o[7]);
    if (o[5] & 0x10) printf("  !! a host stream did not stop (RUN stayed 1)\n");
    if (o[5] & 0x20) printf("  !! DSP still powered: BAR0 registers, BME and DMA were NOT restored yet (re-run --undo)\n");
    if (o[5] & 0x40) printf("  !! a stream reset (SRST) timed out\n");
    if (o[5] & 0x80) printf("  !! PCI command (BME/INTx) restore did not verify\n");
    if (o[5] & 0x100) printf("  !! PPCTL or LTRP did not read back the snapshot\n");
    if (o[5] & 0x200) printf("  a pending DSP->host message (HIPCTDR.BUSY) was acknowledged before the core was stopped\n");
    if (o[15] >> 32) {   // M5 (kext 0.7.0): host DMA stop, amps mute + PS3, master port channels off, before M4
        uint32_t m5 = (uint32_t)(o[15] >> 32), nw = m5 & 0xFFFF, rc = m5 >> 16;
        printf("  M5 part (first): %u write(s): playback DMA RUN=0, RT1316 FU21 mute + PDE23/27/22 PS3 on links 1/2, DP2 CH_EN=0%s%s%s\n", nw,
               (rc & 1) ? "\n  !! the playback DMA did not stop" : "", (rc & 2) ? "\n  !! an amp write failed (the link power-down that follows silences the amps anyway)" : "",
               (rc & 4) ? "\n  (DSP already unpowered: nothing to stop)" : "");
    }
    if (o[15] & 0xFFFFFFFFull) {   // M4 (kext 0.6.0): SoundWire links stopped first (intel_stop_bus, intel_bus_common.c:205-238)
        uint32_t nw = (uint32_t)(o[15] & 0xFFFF), rc = (uint32_t)((o[15] >> 16) & 0xFFFF);
        printf("  SoundWire part (first, intel_stop_bus): %u write(s): interrupt masks 0, IOCTL master IP -> glue,\n"
               "    LCTL.SPA=0 (link power down, intel.c:451-498), WAKEEN off / WAKESTS cleared (intel.c:302-326)%s%s\n", nw,
               (rc & 1) ? "\n  !! LCTL.CPA did not drop within 50 ms" : "",
               (rc & 2) ? "\n  (DSP already unpowered: the SoundWire shim lost power with it, nothing to stop)" : "");
    }
    if (o[12]) printf("  M2b part: PPCTL 0x%08llx EM2 0x%08llx (L1SEN=%llu) LTRP byte 0x%02llx PCI cmd 0x%04llx (BME=%llu INTxDis=%llu)\n"
                      "            SD(CL) CTL 0x%08llx SPBFCCTL 0x%08llx; DMA buffers freed\n",
                      o[9], o[10], (o[10] >> 13) & 1, o[11], o[12], (o[12] >> 2) & 1, (o[12] >> 10) & 1, o[13], o[14]);
    int m1 = ((uint32_t)o[2] & 0xFF0F0101u) == 0x00000101u;  // core0: CPA=SPA=0, CRST=1, CSTALL=1
    printf("  core 0 back to the M1 state: %s\n", m1 ? "yes" : "NO");
    return m1 ? 0 : -1;
}


static int cmd_m2(int argc, char **argv) {
    int dry = 0, undo = 0, state = 0, teardown = 0, offline = 0;
    const char *ipc = "auto"; unsigned romms = 300;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dry")) dry = 1;
        else if (!strcmp(argv[i], "--undo")) undo = 1;
        else if (!strcmp(argv[i], "--state")) state = 1;
        else if (!strcmp(argv[i], "--teardown")) teardown = 1;
        else if (!strcmp(argv[i], "--offline")) offline = 1;   // --dry without opening the kext
        else if (!strncmp(argv[i], "--ipc=", 6) && (!strcmp(argv[i] + 6, "auto") || !strcmp(argv[i] + 6, "first") || !strcmp(argv[i] + 6, "never"))) ipc = argv[i] + 6;
        else if (!strncmp(argv[i], "--rom-ms=", 9)) { romms = (unsigned)strtoul(argv[i] + 9, NULL, 0); if (romms < 50) romms = 50; if (romms > 1000) romms = 1000; }
        else { fprintf(stderr, "m2: unknown option %s\n", argv[i]); return 2; }
    }
    if (dry + undo + state > 1) { fprintf(stderr, "m2: --dry, --undo and --state are exclusive\n"); return 2; }

    if (dry) {
        printf("audctl 0.9.0  m2 --dry: NOTHING is armed or written.\n");
        uint32_t c44 = 0x00, c48 = 0xFF, cs = 0x00000F0F; int live = 0;
        if (offline) printf("(--offline: kext not opened, using the M1 dump values)\n");
        else if (aud_open() == 0) {
            uint64_t o[16]; uint32_t n = 16;
            if (IOConnectCallScalarMethod(conn, kInfo, NULL, 0, o, &n) == 0 && o[15] >= 3) {
                uint64_t p[16];
                uint32_t f = m2_precheck(p, 1);
                if (f != 0xFFFFFFFFu && !(f & 0x87)) { c44 = (uint32_t)p[7]; c48 = (uint32_t)p[8]; cs = (uint32_t)p[6]; live = 1; }
            } else printf("(loaded kext is interface v%llu, not 0.3.0: using the M1 dump values)\n", n ? o[15] : 0);
        } else printf("(kext not reachable: using the M1 dump values)\n");
        m2_plan(c44, c48, cs, ipc, romms, live);
        return 0;
    }

    if (aud_open()) return 1;
    uint64_t o[16], p[16];
    { uint32_t n = 16; if (IOConnectCallScalarMethod(conn, kInfo, NULL, 0, o, &n) || o[15] < 3) {
        fprintf(stderr, "m2 needs AudShim >= 0.3.0 (interface v3); loaded kext reports v%llu\n", o[15]); return 1; } }

    if (state) {
        if (m2call(M2_STATE, 0, 0, o)) return 1;
        static const char *ph[] = {"idle", "armed", "WRITING (sequence half-done)", "LEFT UP (core 0 at ROM, needs --undo)"};
        printf("M2 kext state: phase=%s owner=%s undo-arm=%llu cfg-snapshot=%s ipcSent=%llu writable-BAR4-map=%llu\n",
               o[1] < 4 ? ph[o[1]] : "?", o[2] == 1 ? "this" : o[2] == 2 ? "OTHER connection" : "none", o[3],
               (o[4] >> 16) ? "yes" : "no", o[5], o[6]);
        if (o[4] >> 16) printf("  saved cfg44 0x%02llx cfg48 0x%02llx\n", o[4] & 0xFF, (o[4] >> 8) & 0xFF);
        printf("  mode: M2b=%llu M3=%llu  progress flags 0x%04llx (b0 BME b1 ctrl b2 ppcap b3 L1 b4 DMA b5 ICCMAX b6 CL b7 LTRP\n"
               "        b8 SSP b9 UNSOL b10 FW-buffer b11 CL-running b12 CL-cleanup-done b13 post_fw_run)\n", o[7] & 1, (o[7] >> 1) & 1, o[8]);
        m2_precheck(p, 1);
        m2_report(0);
        IOServiceClose(conn);
        return 0;
    }

    if (undo) {
        printf("audctl 0.9.0  m2 --undo: return DSP core 0 and cfg 0x44/0x48 to the M1 state\n");
        if (m2call(M2_ARM, MAGIC_UNDO, 0, o)) return 1;
        if (o[0] != M2R_OK) { printf("  ARM(undo) %s (fail mask 0x%llx)\n", m2rc(o[0]), o[11]); m2_precheck(p, 1); return 1; }
        { uint64_t q[16]; if (m2call(M2B_INFO, 2, 0, q) == 0 && q[11])
            printf("  kext is in M2b mode (flags 0x%llx): the rollback also restores BAR0 regs, BME and frees DMA\n", q[12]); }
        printf("  armed (undo-only: the kext accepts ROLLBACK and nothing else on this connection)\n");
        int r = m2_rollback("--undo");
        m2call(M2_DISARM, 0, 0, o);
        m2_report(0);
        IOServiceClose(conn);
        printf("\nUNDO %s\n", r == 0 ? "OK: M1 state" : "INCOMPLETE: see above");
        return r ? 1 : 0;
    }

    // ---------------- the real M2 run
    printf("audctl 0.9.0  m2 (ipc=%s, ROM poll %u ms%s)\n", ipc, romms, teardown ? ", teardown at end" : "");
    uint32_t f = m2_precheck(p, 1);
    if (f == 0xFFFFFFFFu) return 1;
    if (f) {
        printf("\nABORT: pre-checks failed (mask 0x%x). Nothing was written.%s\n", f,
               (f & ((1u << 6) | (1u << 9))) ? "\n  core 0 / gating not in the M1 state: run `audctl m2 --undo` first." : "");
        return 1;
    }
    uint32_t c44 = (uint32_t)p[7], c48 = (uint32_t)p[8], cs = (uint32_t)p[6];
    printf("\n");
    m2_plan(c44, c48, cs, ipc, romms, 1);
    fflush(stdout); fsync(STDOUT_FILENO);

    printf("\n== ARM ==\n");
    if (m2call(M2_ARM, MAGIC_RUN, 0, o)) return 1;
    if (o[0] != M2R_OK) { printf("  ARM %s (fail mask 0x%llx, phase %llu). Nothing was written.\n", m2rc(o[0]), o[11], o[15]); return 1; }
    printf("  armed on this connection (cfg44 0x%02x / cfg48 0x%02x snapshotted in the kext)\n", c44, c48);

    const char *failstep = NULL;
    int ipc_sent = 0, rom_ok = 0;
#define STEP(name) printf("\n%s\n", name); fflush(stdout); fsync(STDOUT_FILENO)
#define CHECK(stepname) if (o[0] != M2R_OK) { failstep = stepname; goto fail; }

    STEP("[W1] cfg 0x44 TCSEL[2:0]=0   (hda.c:903)");
    if (m2call(M2_TCSEL, 0, 0, o)) { failstep = "W1"; goto fail; }
    printf("  before 0x%02llx after 0x%02llx  %s  %s\n", o[1], o[2], o[3] ? "written" : "no write (already 0)", m2rc(o[0]));
    CHECK("W1 TCSEL");

    STEP("[W2] cfg 0x48 CGCTL.ADSPDCGE(b1)=0   (hda.c:439 -> hda-ctrl.c:169-170)");
    if (m2call(M2_CGCTL, 0, 0, o)) { failstep = "W2"; goto fail; }
    printf("  before 0x%02llx after 0x%02llx  %s  %s\n", o[1], o[2], o[3] ? "written" : "no write", m2rc(o[0]));
    CHECK("W2 CGCTL");

    STEP("[W3] cfg 0x44 PGCTL.ADSPPGD(b2)=1   (hda.c:439 -> hda-ctrl.c:179-180)");
    if (m2call(M2_PGCTL, 0, 0, o)) { failstep = "W3"; goto fail; }
    printf("  before 0x%02llx after 0x%02llx  %s  %s\n", o[1], o[2], o[3] ? "written" : "no write", m2rc(o[0]));
    CHECK("W3 PGCTL");

    STEP("[W4] ADSPCS SPA0=1, poll CPA0 50 ms   (hda-loader.c:77 -> hda-dsp.c:295-305)");
    if (m2call(M2_SPA, 0, 0, o)) { failstep = "W4"; goto fail; }
    print_adspcs("  before ", (uint32_t)o[1]);
    print_adspcs("  after  ", (uint32_t)o[2]);
    printf("  CPA0 after ~%llu us: %s\n", o[3], m2rc(o[0]));
    CHECK("W4 SPA0/CPA0");

    if (!strcmp(ipc, "first")) {
        STEP("[W5] HIPCIDR = 0x81004000 ROM_CONTROL|PURGE_FW, tag 1   (hda-loader.c:87-91, Linux order)");
        if (m2call(M2_IPC, 0, 0, o)) { failstep = "W5"; goto fail; }
        printf("  HIPCIDR before 0x%08llx after 0x%08llx  HIPCIDA 0x%08llx  %s\n", o[1], o[2], o[4], m2rc(o[0]));
        CHECK("W5 IPC");
        ipc_sent = 1;
    }

    STEP("[W6] ADSPCS CRST0=0 (core 0 still stalled), poll 50 ms   (hda-dsp.c:176-187)");
    if (m2call(M2_CRST, 0, 0, o)) { failstep = "W6"; goto fail; }
    print_adspcs("  before ", (uint32_t)o[1]);
    print_adspcs("  after  ", (uint32_t)o[2]);
    printf("  CRST0 clear after ~%llu us: %s\n", o[3], m2rc(o[0]));
    CHECK("W6 CRST0");

    STEP("[W7] ADSPCS CSTALL0=0 (core 0 runs its ROM)   (hda-dsp.c:259-265)");
    if (m2call(M2_CSTALL, 0, 0, o)) { failstep = "W7"; goto fail; }
    print_adspcs("  before ", (uint32_t)o[1]);
    print_adspcs("  after  ", (uint32_t)o[2]);
    printf("  core 0 enabled check (CPA0 SPA0 !CRST0 !CSTALL0): %s\n", m2rc(o[0]));
    CHECK("W7 CSTALL0");

    if (ipc_sent) {
        STEP("[poll] HIPCIDA.DONE, 500 ms; [W8] ack DONE   (hda-loader.c:104-123)");
        if (m2call(M2_IPCDONE, 500, 0, o)) { failstep = "W8"; goto fail; }
        printf("  HIPCIDA seen 0x%08llx after ~%llu us -> after ack 0x%08llx; HIPCIDR 0x%08llx  %s\n", o[1], o[3], o[2], o[5], m2rc(o[0]));
        CHECK("IPC DONE (ROM did not answer ROM_CONTROL)");
    }

    STEP("[poll] ROM status BAR4+0x80000 == INIT_DONE   (hda-loader.c:148-153)");
    if (m2call(M2_ROMPOLL, 1, romms, o)) { failstep = "ROM poll"; goto fail; }
    print_rom_poll("ROM poll", o);
    rom_ok = (o[0] == M2R_OK);

    if (!rom_ok && !strcmp(ipc, "auto") && !ipc_sent && o[2] != 0xFFFFFFFFull) {
        uint32_t s = (uint32_t)o[1], e = (uint32_t)o[2];
        int waits_ipc = ((s & 0xFFFFFF) == 0) || (((s >> 24) & 0xF) == 1);
        if (!(s >> 31) && e == 0 && waits_ipc) {
            printf("\n  ROM did not report INIT_DONE by itself and looks idle/waiting (state 0x%x wait %u):\n"
                   "  it needs the cl_dsp_init ROM_CONTROL message -> sending it now.\n", s & 0xFFFFFF, (s >> 24) & 0xF);
            STEP("[W5] HIPCIDR = 0x81004000 ROM_CONTROL|PURGE_FW, tag 1   (hda-loader.c:87-91)");
            if (m2call(M2_IPC, 0, 0, o)) { failstep = "W5"; goto fail; }
            printf("  HIPCIDR before 0x%08llx after 0x%08llx  HIPCIDA 0x%08llx  %s\n", o[1], o[2], o[4], m2rc(o[0]));
            CHECK("W5 IPC");
            ipc_sent = 1;
            STEP("[poll] HIPCIDA.DONE, 500 ms; [W8] ack DONE   (hda-loader.c:104-123)");
            if (m2call(M2_IPCDONE, 500, 0, o)) { failstep = "W8"; goto fail; }
            printf("  HIPCIDA seen 0x%08llx after ~%llu us -> after ack 0x%08llx; HIPCIDR 0x%08llx  %s\n", o[1], o[3], o[2], o[5], m2rc(o[0]));
            int done_ok = (o[0] == M2R_OK);
            STEP("[poll] ROM status == INIT_DONE (after IPC)   (hda-loader.c:148-153)");
            if (m2call(M2_ROMPOLL, 1, romms, o)) { failstep = "ROM poll 2"; goto fail; }
            print_rom_poll("ROM poll", o);
            rom_ok = (o[0] == M2R_OK);
            if (!done_ok && !rom_ok) { failstep = "IPC DONE + ROM INIT_DONE (after ROM_CONTROL)"; goto fail; }
        } else {
            printf("\n  ROM did not reach INIT_DONE and is %s: not sending the IPC.\n",
                   (s >> 31) ? "HALTED" : e ? "reporting a ROM error" : "in an unexpected state");
        }
    }
    if (!rom_ok) { failstep = "ROM INIT_DONE"; goto fail; }

    m2_report(1);
    if (teardown) {
        int r = m2_rollback("--teardown requested");
        printf("\nM2 RESULT: SUCCESS (ROM INIT_DONE%s), then torn down: %s\n",
               ipc_sent ? " after ROM_CONTROL IPC" : ", no IPC needed", r == 0 ? "M1 state" : "TEARDOWN INCOMPLETE");
    } else {
        m2call(M2_FINISH, 0, 0, o);
        printf("\nM2 RESULT: SUCCESS (ROM INIT_DONE%s). Core 0 is LEFT at the ROM (finish: %s).\n"
               "  Dump it with `audctl m1`, then run `audctl m2 --undo` (before sleep/shutdown).\n",
               ipc_sent ? " after ROM_CONTROL IPC" : ", no IPC needed", m2rc(o[0]));
    }
    IOServiceClose(conn);
    return 0;

fail:
    printf("\n!! M2 FAILED at %s (%s)\n", failstep, m2rc(o[0]));
    m2_report(1);   // state before rollback (ROM status still readable while CPA0=1)
    m2_rollback("failure path");
    m2_report(0);
    IOServiceClose(conn);
    printf("\nM2 RESULT: FAILURE at %s; rolled back.\n", failstep);
    return 1;
#undef STEP
#undef CHECK
}

// ------------------------------------------------------------------ m2b / m3
// M2b: the M2 ROM handshake with the Linux prerequisites that M2 skipped. Linux references:
//   [HD] hda.c  [HX] hda-ctrl.c  [DS] hda-dsp.c  [LD] hda-loader.c  [ST] hda-stream.c  [TG] tgl.c
//   [CN] cnl.c  [I3] sound/soc/sof/ipc3.c  [IL] sound/soc/sof/ipc3-loader.c  [LO] sound/soc/sof/loader.c
//   [CO] sound/soc/sof/core.c  [IN] include/sound/sof/info.h  [EM] include/sound/sof/ext_manifest.h
//   [EH] sound/soc/sof/intel/ext_manifest.h  [AB] include/uapi/sound/sof/abi.h  [TR] include/sound/sof/trace.h
// Order = Linux first boot on RPL-P (tgl ops: run = hda_dsp_cl_boot_firmware_iccmax, [TG]:129):
//   probe: BME [HD]:891, TCSEL [HD]:903, init_chip [HD]:635 -> [HX]:186-292, PPCTL [HD]:912-913
//   pre_fw_run [HD]:422-440: ADSPDCGE=0, EM2.L1SEN=0, ADSPPGD=1
//   ICCMAX capture stream started [LD]:284-296 -> [ST]:484-555 (LTRP guardband 95)
//   CL playback stream prepared [LD]:376-380 -> [ST]:563-755 + SPIB [ST]:187-214
//   cl_dsp_init x up to 3 [LD]:393-405 -> [LD]:66-182 (SPA, SSP, ROM_CONTROL, CRST, CSTALL, DONE, INIT_DONE)
// M2b stops at ROM INIT_DONE. M3 continues exactly like Linux:
//   payload = .ri minus the SOF extended manifest ([LO]:48-51, [IL]:112-231), copied into the CL buffer
//   ([LD]:375-395); hda_cl_copy_fw [LD]:228-274: CL DMA start, FW_ENTERED (3 s), CL DMA stop;
//   hda_cl_cleanup [LD]:461; ICCMAX cleanup + LTRP [LD]:305-317; wait FW_READY ([LO]:150-170,
//   boot_timeout 2000 ms [CO]:65,690-691) = HIPCTDR.BUSY ([CN]:154-184), message at the uplink
//   mailbox 0x81000 ([HD]:916, [I3]:1049-1075, 817-858); ack = cnl_ipc_host_done [CN]:201-216.

struct bootopt {
    int attempts, ctrl, icc, l1, ssp, pie, leave, m3, post;
    unsigned romms, readyms;
    const char *fw;
    int m4; unsigned links, attachms;   // M4: ACPI link mask (default 0xF), dev0 attach wait
    int m5, noplay, chan, hex;          // M5: speaker playback; --no-play = M5a; channel 0 both 1 left 2 right
    double level, secs;                 // M5: sine level (dBFS, -60..-12) and duration (s, 0.5..3)
    const char *tplg;                   // M5: topology file
};

// ---- firmware file (M3)
struct fwfile {
    uint8_t *data; size_t size;      // whole .ri file
    uint32_t off;                     // payload_offset = extended manifest full_size ([LO]:48-51)
    uint32_t psize, hash;             // payload size and FNV-1a 32
    int have_ver; uint16_t vmaj, vmin, vmic, vbuild; char vtag[7]; uint32_t vabi;
    int nwin; uint32_t win[16][6];    // manifest windows: hdr.size, type, id, flags, size, offset ([IN]:95-110)
};

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
static void cstr(char *dst, const uint8_t *src, size_t n) {   // fixed-size field -> printable C string
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : '?';
    dst[i] = 0;
}
static const char *region_name(uint32_t t) {   // enum sof_ipc_region [IN]:80-88; names as in [I3]:676-726
    static const char *n[] = {"DOWNBOX (outbox, host->DSP)", "UPBOX (inbox, DSP->host)", "TRACE (etrace)",
                              "DEBUG", "STREAM", "REGS", "EXCEPTION"};
    return t < 7 ? n[t] : "?";
}
// window offset = SRAM_WINDOW_OFFSET(id) + elem.offset (hda-ipc.c:396-399, hda.h:162, [I3]:666-726)
static uint32_t win_bar4(const uint32_t *w) { return 0x80000u + w[2] * 0x20000u + w[5]; }
static void print_windows(const char *pfx, int n, uint32_t w[][6]) {
    for (int i = 0; i < n; i++)
        printf("%stype %u %-28s id %u flags 0x%x size 0x%05x offset 0x%05x -> BAR4+0x%05x\n", pfx, w[i][1],
               region_name(w[i][1]), w[i][2], w[i][3], w[i][4], w[i][5], win_bar4(w[i]));
}
static uint32_t find_window(int n, uint32_t w[][6], uint32_t type, uint32_t *size) {
    for (int i = 0; i < n; i++) if (w[i][1] == type) { if (size) *size = w[i][4]; return win_bar4(w[i]); }
    if (size) *size = 0;
    return 0;
}

static void print_fwver(const char *pfx, const uint8_t *v) {   // struct sof_ipc_fw_version [IN]:44-59
    char date[13], tim[11], tag[7];
    cstr(date, v + 12, 12); cstr(tim, v + 24, 10); cstr(tag, v + 34, 6);
    uint32_t abi = le32(v + 40);
    printf("%sversion %u.%u.%u build %u tag '%s' date '%s' time '%s' ABI %u.%u.%u (0x%08x) src_hash 0x%08x\n",
           pfx, le16(v + 4), le16(v + 6), le16(v + 8), le16(v + 10), tag, date, tim,
           (abi >> 24) & 0xff, (abi >> 12) & 0xfff, abi & 0xfff, abi, le32(v + 44));
}

// snd_sof_load_firmware_raw ([LO]:17-66) + sof_ipc3_fw_parse_ext_man ([IL]:112-231), read-only.
static int fw_parse(struct fwfile *f, const char *path, int verbose) {
    memset(f, 0, sizeof *f);
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("  !! cannot open firmware file %s\n", path); return -1; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (sz < 4096 || sz > 4 * 1024 * 1024) { printf("  !! firmware file size %ld out of range\n", sz); fclose(fp); return -1; }
    f->data = malloc((size_t)sz); f->size = (size_t)sz;
    if (!f->data || fread(f->data, 1, f->size, fp) != f->size) { printf("  !! read failed\n"); fclose(fp); return -1; }
    fclose(fp);
    if (verbose) printf("firmware file %s: %zu bytes, FNV-1a 0x%08x\n", path, f->size, fnv1a(f->data, f->size));
    // ipc3_fw_ext_man_size [IL]:112-136
    uint32_t magic = le32(f->data), full = le32(f->data + 4), hsz = le32(f->data + 8), hver = le32(f->data + 12);
    if (magic != 0x6e614d58u) {   // SOF_EXT_MAN_MAGIC_NUMBER [EM]:25
        if (verbose) printf("  no SOF extended manifest (magic 0x%08x): payload = whole file ([LO]:52-54)\n", magic);
        f->off = 0;
    } else {
        long remaining = (long)full - (long)hsz;                                  // [IL]:150-152
        if (remaining < 0 || remaining > (long)f->size || full > f->size) { printf("  !! extended manifest sizes inconsistent\n"); return -1; }
        if ((hver & 0xff000000u) != 0x01000000u) {                              // [IL]:160-166, [EM]:34-39
            printf("  !! extended manifest version 0x%08x incompatible with 1.0.0\n", hver); return -1;
        }
        if (verbose) printf("  SOF extended manifest 'XMan' ([EM]:25-56): full_size %u header_size %u version %u.%u.%u\n"
                            "    -> Linux drops it: payload_offset = %u ([LO]:48-51, [IL]:231)\n",
                            full, hsz, hver >> 24, (hver >> 12) & 0xfff, hver & 0xfff, full);
        size_t p = hsz;
        while (remaining > 8) {                                                   // [IL]:171-224
            uint32_t type = le32(f->data + p), esz = le32(f->data + p + 4);
            if (esz < 8 || esz > (uint32_t)remaining) { printf("  !! invalid sof_ext_man element size %u (type %u)\n", esz, type); return -1; }
            const uint8_t *e = f->data + p + 8;
            switch (type) {
            case 0:   // SOF_EXT_MAN_ELEM_FW_VERSION [EM]:78-83 -> ipc3_fw_ext_man_get_version [IL]:14-25
                if (esz >= 8 + 64) {
                    f->have_ver = 1; f->vmaj = le16(e + 4); f->vmin = le16(e + 6); f->vmic = le16(e + 8); f->vbuild = le16(e + 10);
                    cstr(f->vtag, e + 34, 6); f->vabi = le32(e + 40);
                    if (verbose) { print_fwver("    FW_VERSION    ", e); printf("                  flags 0x%08x\n", esz >= 8 + 64 ? le32(e + 60) : 0); }
                }
                break;
            case 1: { // SOF_EXT_MAN_ELEM_WINDOW [EM]:86-90 -> ipc3_fw_ext_man_get_windows
                uint32_t nw = esz >= 8 + 16 ? le32(e + 12) : 0;
                if (nw > 16) nw = 16;
                for (uint32_t i = 0; i < nw && 8 + 16 + 24 * (i + 1) <= esz; i++)
                    for (int k = 0; k < 6; k++) f->win[i][k] = le32(e + 16 + 24 * i + 4 * k);
                f->nwin = (int)nw;
                if (verbose) { printf("    WINDOW        %u windows ([IN]:95-110; BAR4 = 0x80000 + id*0x20000 + offset, hda.h:162):\n", nw);
                               print_windows("                  ", f->nwin, f->win); }
                break;
            }
            case 2:   // SOF_EXT_MAN_ELEM_CC_VERSION [IN]:112-124
                if (verbose && esz >= 8 + 92) {
                    char nm[17], op[5], ds[33]; cstr(nm, e + 40, 16); cstr(op, e + 56, 4); cstr(ds, e + 60, 32);
                    printf("    CC_VERSION    compiler %s %u.%u.%u %s, optimisation %s\n", nm, le32(e + 12), le32(e + 16), le32(e + 20), ds, op);
                }
                break;
            case 3: if (verbose) printf("    PROBE_INFO    (not parsed by Linux either, [IL]:193-195)\n"); break;
            case 4: if (verbose && esz >= 8 + 16) { uint32_t a = le32(e + 12);
                        printf("    DBG_ABI       debug ABI %u.%u.%u\n", a >> 24, (a >> 12) & 0xfff, a & 0xfff); } break;
            case 5:   // SOF_EXT_MAN_ELEM_CONFIG_DATA [EM]:105-124
                if (verbose) for (uint32_t i = 8; i + 8 <= esz; i += 8) {
                    uint32_t t = le32(f->data + p + i), v = le32(f->data + p + i + 4);
                    if (t) printf("    CONFIG_DATA   %s = %u\n", t == 1 ? "IPC_MSG_SIZE" : t == 2 ? "MEMORY_USAGE_SCAN" : "token?", v);
                }
                break;
            case 6:   // SOF_EXT_MAN_ELEM_PLATFORM_CONFIG_DATA -> hda_dsp_ext_man_get_cavs_config_data [LD]:604-641, [EH]:21-34
                if (verbose) for (uint32_t i = 8; i + 8 <= esz; i += 8) {
                    uint32_t t = le32(f->data + p + i), v = le32(f->data + p + i + 4);
                    if (t == 1) printf("    PLATFORM_CFG  CAVS_LPRO = %u -> FW clock config %s\n", v, v ? "LPRO" : "HPRO");
                    else if (t) printf("    PLATFORM_CFG  %s = 0x%x\n", t == 2 ? "OUTBOX_SIZE" : t == 3 ? "INBOX_SIZE" : "token?", v);
                }
                break;
            default: if (verbose) printf("    unknown sof_ext_man element type %u size %u\n", type, esz);
            }
            remaining -= esz; p += esz;
        }
        if (remaining) { printf("  !! sof_ext_man header is inconsistent ([IL]:226-229)\n"); return -1; }
        f->off = full;
    }
    if (f->size <= f->off) { printf("  !! firmware size must be greater than firmware offset ([LD]:366-369)\n"); return -1; }
    f->psize = (uint32_t)(f->size - f->off);
    f->hash = fnv1a(f->data + f->off, f->psize);
    const uint8_t *pl = f->data + f->off;
    int cpd = !memcmp(pl, "$CPD", 4);
    if (verbose) {
        printf("  payload (DMA image): file[%u..%zu) = %u bytes (%u pages%s), FNV-1a 0x%08x, starts '%c%c%c%c' %s\n",
               f->off, f->size, f->psize, (f->psize + 4095) / 4096, f->psize % 4096 ? ", last one partial" : "", f->hash,
               pl[0] >= 32 && pl[0] < 127 ? pl[0] : '.', pl[1] >= 32 && pl[1] < 127 ? pl[1] : '.',
               pl[2] >= 32 && pl[2] < 127 ? pl[2] : '.', pl[3] >= 32 && pl[3] < 127 ? pl[3] : '.',
               cpd ? "(CSE code partition directory: Intel-signed image, verified by CSE/ROM)" : "(!! not $CPD)");
        if (f->have_ver)
            printf("  firmware ABI %u.%u.%u vs Linux kernel ABI 3.23.1 ([AB]:30-32): %s ([I3]:787-792, [AB]:54-57)\n",
                   f->vabi >> 24, (f->vabi >> 12) & 0xfff, f->vabi & 0xfff, (f->vabi >> 24) == 3 ? "compatible (same major)" : "INCOMPATIBLE");
    }
    if (!cpd) { printf("  !! payload does not start with $CPD: wrong file? (the kext refuses it too)\n"); return -1; }
    if (f->psize & 3 || f->psize > 1024 * 1024) { printf("  !! payload size %u not usable (multiple of 4, <= 1 MiB)\n", f->psize); return -1; }
    if (f->have_ver && (f->vabi >> 24) != 3) { printf("  !! firmware ABI major is not 3 (IPC3): Linux would refuse it\n"); return -1; }
    return 0;
}

static void m4_plan(unsigned links, unsigned attachms);
struct m5plan;
static void m5_plan_print(const struct bootopt *op, const struct m5plan *pl, int hex);
struct tplg;
static int pl_build(const struct tplg *tp, struct m5plan *pl, int verbose);
static struct m5plan *g_m5plan;
static void boot_plan(const struct bootopt *op, uint32_t iss, uint32_t oss, uint32_t spb, uint32_t pp, int live,
                      const struct fwfile *f) {
    uint32_t cl = iss, sdI = 0x80, sdC = 0x80 + 0x20 * cl;
    const char *M = op->m3 ? "M3" : "M2b";
    printf("%s planned sequence (%s layout: ISS=%u OSS=%u -> ICCMAX = SD0 @0x%x tag 1, CL = SD%u @0x%x tag 1;\n"
           "  PP @0x%x, SPIB @0x%x). attempts=%d, ROM poll %u ms%s%s\n", M, live ? "live" : "M1-dump", iss, oss, sdI, cl, sdC,
           pp, spb, op->attempts, op->romms, op->m3 ? "" : "", op->leave ? ", LEAVE UP at the end" : ", full teardown at the end");
    printf("  S0  ARM('%s'): kext snapshots PCI cmd, GCTL, PPCTL, EM2, LTRP, SPBFCCTL, SPIB(CL), cfg44/48\n", op->m3 ? "M3GO" : "M2BG");
    printf("  S1  PCI cmd |= BME(b2) | INTxDisable(b10)                   hda.c:891 pci_set_master (INTx off: we poll)\n");
    printf("  S2  cfg 0x44 TCSEL[2:0]=0 (only if non-zero)                hda.c:903\n");
    if (op->ctrl)
        printf("  S3  controller init: cfg48 MISCBDCGE=0, WAKESTS clear, GCTL.CRST=0 (poll 100 ms), 1 ms,\n"
               "      GCTL.CRST=1 (poll), 1.2 ms, GCTL.UNSOL=1, SD STS clear, WAKESTS clear, MISCBDCGE back\n"
               "                                                               hda-ctrl.c:186-292 (no INTCTL/RIRB/posbuf)\n");
    else printf("  S3  (skipped: --no-ctrl-reset)\n");
    printf("  S4  PPCTL |= GPROCEN%s                                        hda.c:912-913\n", op->pie ? " | PIE" : "");
    printf("  S5  pre_fw_run: cfg48 ADSPDCGE=0, %scfg44 ADSPPGD=1          hda.c:422-440 -> hda-ctrl.c:163-184\n",
           op->l1 ? "EM2(0x1030) L1SEN(b13)=0, " : "(EM2.L1SEN skipped), ");
    printf("  S6  DMA: 5 pages (CL BDL, ICCMAX BDL, 8 KiB CL data%s, 4 KiB ICCMAX data), IODMACommand through\n"
           "      the PCI device's mapper; device addresses (IOVA) logged next to physical\n", op->m3 ? " (unused in M3)" : "");
    if (op->m3) {
        printf("  S6b firmware payload: %u bytes (file minus %u-byte SOF extended manifest), copied by the kext from\n"
               "      this process into a page-aligned <4 GiB IOBufferMemoryDescriptor (%u pages, not necessarily\n"
               "      contiguous), FNV-1a 0x%08x checked, IODMACommand segments recorded  loader.c:48-51, hda-loader.c:375-395\n",
               f ? f->psize : 0, f ? f->off : 0, f ? (f->psize + 4095) / 4096 : 0, f ? f->hash : 0);
    }
    if (op->icc)
        printf("  S7  ICCMAX SD0: BDLP=0, BDL(2x2 KiB), BDLP, CBL 4096, LVI 1, PPCTL PROCEN0, LTRP=(LTRP&~0x3f)|95,\n"
               "      RUN=1                                                    hda-loader.c:284-296, hda-stream.c:484-555\n");
    else printf("  S7  (skipped: --no-iccmax)\n");
    if (op->m3)
        printf("  S8  CL SD%u: PPCTL PROCEN%u, stop/clear, SRST cycle, BDLP=0, tag 1, CBL %u, FMT 0x40, LVI = BDLEs-1,\n"
               "      BDLP, SD int-enable bits; SPBFCCTL bit%u, SPIB=%u. BDL = hda_dsp_stream_setup_bdl: one BDLE per\n"
               "      contiguous chunk, or 2 halves if the buffer is one chunk     hda-stream.c:112-185, 563-755, 187-214\n",
               cl, cl, f ? f->psize : 0, cl, f ? f->psize : 0);
    else
        printf("  S8  CL SD%u: PPCTL PROCEN%u, stop/clear, SRST cycle, BDLP=0, tag 1, CBL 8192, FMT 0x40, LVI 1,\n"
               "      BDLP, SD int-enable bits; SPBFCCTL bit%u, SPIB=8192        hda-stream.c:563-755, 187-214\n", cl, cl, cl);
    printf("  attempt loop (max %d, Linux HDA_FW_BOOT_ATTEMPTS=3, hda-loader.c:393-405):\n", op->attempts);
    printf("  S9  ADSPCS SPA0=1, poll CPA0 50 ms                          hda-dsp.c:280-326\n");
    printf("  S10 SSP0-2 SSC1 %s   hda-loader.c:34-49,84\n",
           op->ssp ? "|= 0x03000000 (CBP/CFP), real write (--ssp)  " : "read/recorded only, NO write (= the M2b run)");
    printf("  S11 HIPCIDR = 0x81004000 (BUSY|ROM_CONTROL|PURGE_FW|(tag1-1)<<9) hda-loader.c:87-91\n");
    printf("  S12 CRST0=0 (poll), CSTALL0=0, enabled check                hda-dsp.c:248-277\n");
    printf("  S13 poll HIPCIDA.DONE 500 ms, ack                           hda-loader.c:104-123\n");
    printf("  S14 poll ROM status BAR4+0x80000 == INIT_DONE, %u ms         hda-loader.c:148-153\n", op->romms);
    printf("      on failure: core 0 stall/reset/power-down, next attempt  hda-loader.c:178\n");
    if (op->m3) {
        printf("  S15 CL SD%u CTL |= RUN|IOCE|FEIE|DEIE, poll RUN 300 us (INTCTL not touched)   hda-loader.c:237 -> 185-215\n", cl);
        printf("  S16 poll ROM status == FW_ENTERED (5), 3000 ms (HDA_DSP_BASEFW_TIMEOUT_US)  hda-loader.c:245-249\n");
        printf("  S17 CL stop (CTL &= ~(RUN|int), poll RUN=0 300 us, STS clear) hda-loader.c:264 -> hda-stream.c:463-480;\n"
               "      CL cleanup: SPIB=0, SPBFCCTL bit off, BDLP=0, CTL=0      hda-loader.c:461 -> hda-stream.c:1333-1375;\n"
               "      ICCMAX cleanup: RUN=0, EM2.L1SEN=1 (stream put), BDLP=0, CTL=0; LTRP GB bits 5:0 restored\n"
               "                                                               hda-loader.c:305-317, hda-stream.c:314-326;\n"
               "      firmware DMA buffer freed\n");
        printf("  S18 poll HIPCTDR.BUSY (FW_READY), %u ms (boot_timeout, core.c:65)  loader.c:150-160, cnl.c:154\n", op->readyms);
        printf("  S19 read the uplink mailbox BAR4+0x81000: sof_ipc_cmd_hdr, sof_ipc_fw_ready (108 bytes), ext data\n"
               "      (windows / cc / probe / user ABI), ABI check, mailbox windows  hda.c:916, ipc3.c:1049-1075, 817-858, 585-745, 774-809\n");
        printf("  S20 ack: HIPCTDR |= BUSY, then HIPCTDA |= DONE           cnl.c:184 -> 201-216\n");
        if (op->m4) m4_plan(op->links, op->attachms);
        if (op->m5 && g_m5plan) m5_plan_print(op, g_m5plan, op->hex);
        if (op->post)
            printf("  S21 post_fw_run: cfg48 ADSPDCGE=1, EM2.L1SEN=1, cfg44 ADSPPGD=0  hda.c:471 -> hda-ctrl.c:163-184 (--post-fw-run)\n");
        else
            printf("  S21 (post_fw_run gating re-enable skipped; --post-fw-run enables it%s)\n",
                   op->m4 ? ", after the SoundWire startup like hda.c:450 -> 471" : ". SoundWire startup hda.c:450 is M4");
    }
    printf("  S%s report (ROM/FW status, SRAM window 0, IPC, ADSPIS, SDW LCAP/LCTL, stream regs)\n", op->m3 ? "22" : "15");
    printf("  S%s %s\n", op->m3 ? "23" : "16",
           op->leave ? (op->m4 ? "LEAVE UP: firmware RUNNING, SoundWire links up, BME on; run `audctl m4 --undo` before sleep/shutdown"
                       : op->m3 ? "LEAVE UP: firmware RUNNING on core 0, BME on; run `audctl m3 --undo` before sleep/shutdown"
                               : "LEAVE UP: core 0 at the ROM, streams/BME/DMA kept; run `audctl m2b --undo` afterwards")
                     : "teardown = rollback below");
    if (op->m4)
        printf("rollback first stops the SoundWire links like intel_stop_bus (intel_bus_common.c:205-238): per started link\n"
               "  SLAVE_INTMASK0/1 = INTMASK = 0 (cadence_master.c:1209-1270), IOCTL master IP -> glue (BKE|COE, MIF=0,\n"
               "  intel.c:237-255); then LCTL &= ~SPA(link mask), wait CPA=0 (intel.c:471-492); WAKEEN off, WAKESTS w1c\n"
               "  (intel.c:316-323). The Cadence IPs are power-gated by SPA=0 (their state is lost, cf. intel_bus_common.c:110-113).\n");
    printf("rollback (failure, teardown, --undo, or by the kext if audctl dies): ICCMAX/CL RUN=0 -> SSP restore (only if\n"
           "  written) -> HIPCIDA/HIPCTDR pending ack -> cores with SPA set: CSTALL/CRST/SPA=0 -> cfg44/48 restore ->\n"
           "  SPIB/SPBFCCTL restore -> SRST + zero SD0/SD%u regs -> PPCTL, LTRP, EM2.L1SEN, GCTL.UNSOL restore -> BME/INTx\n"
           "  restore -> DMA (BDL pages%s) unmapped and freed\n", cl, op->m3 ? " + firmware buffer" : "");
    printf("Not done (deviations from Linux): INTCTL/ADSPIC/HIPCCTL/PPCTL.PIE-driven interrupts are never serviced\n"
           "  (INTx disabled), no DPLBASE position buffer, no codec/CORB/RIRB%s.\n",
           op->m4 ? ", no ADSPIC2.SNDW (hda_sdw_int_enable): SoundWire is polled; no sdw_initialize_slave / codec init\n"
                    "  (only SCP_DevId reads and SCP_DevNumber writes reach the peripherals), no topology, no trace DMA"
           : op->m3 ? ", no SoundWire startup, no topology, no trace DMA" : ", no firmware, CL stream never started");
}

static void m2b_regs(const char *title) {
    uint64_t q[16];
    if (m2call(M2B_INFO, 0, 0, q) || q[0] != M2R_OK) { printf("  (M2b register read failed)\n"); return; }
    uint32_t cl = (uint32_t)(q[3] >> 24) & 0xFF;
    printf("  -- %s --\n", title);
    printf("  flags 0x%04llx  GCTL 0x%08llx  PCI cmd 0x%04llx (BME=%llu INTxDis=%llu)  PPCTL 0x%08llx  EM2 0x%08llx (L1SEN=%llu)  LTRP 0x%08llx (GB byte 0x%02llx)\n",
           q[1], q[14] >> 32, q[5], (q[5] >> 2) & 1, (q[5] >> 10) & 1, q[11], q[14] & 0xFFFFFFFF, (q[14] >> 13) & 1,
           q[15] & 0xFFFFFFFF, q[15] & 0xFF);
    printf("  SD0 (ICCMAX) CTL/STS 0x%08llx CBL 0x%llx BDL 0x%016llx LPIB 0x%llx\n", q[6] & 0xFFFFFFFF, q[6] >> 32, q[8], q[15] >> 32);
    printf("  SD%u (CL)    CTL/STS 0x%08llx (tag %llu RUN %llu) CBL 0x%llx LVI 0x%llx FIFOS 0x%llx FMT 0x%llx BDL 0x%016llx\n",
           cl, q[7] & 0xFFFFFFFF, (q[7] >> 20) & 0xF, (q[7] >> 1) & 1, q[7] >> 32, q[9] & 0xFFFF, (q[9] >> 32) & 0xFFFF, q[9] >> 48, q[10]);
    printf("  SPBFCCTL 0x%08llx SPIB(CL) 0x%08llx\n", q[12], q[13]);
}

static int m2b_step(const char *title) { printf("\n%s\n", title); fflush(stdout); fsync(STDOUT_FILENO); return 0; }

// Read n bytes (multiple of 4) of BAR4 through the read-only MMIO method (kext allows 0x80000+ only
// while CPA0=1). Returns 0 on success.
static int rd4buf(uint32_t off, uint8_t *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t v;
        if (mmio(4, off + i, &v)) return -1;
        buf[i] = (uint8_t)v; buf[i + 1] = (uint8_t)(v >> 8); buf[i + 2] = (uint8_t)(v >> 16); buf[i + 3] = (uint8_t)(v >> 24);
    }
    return 0;
}

static void fw_status_regs(void) {   // SRAM window 0 "registers" hda.h:169-175
    uint8_t b[16];
    if (rd4buf(0x80000, b, 16)) { printf("  SRAM window 0 not readable (core 0 unpowered?)\n"); return; }
    print_fsr("  BAR4+0x80000 ROM/FW status   ", le32(b));
    printf("  BAR4+0x80004 ROM_ERROR/FW_STATUS 0x%08x (%s)  +0x8 FW_TRACEP 0x%08x  +0xc FW_END 0x%08x\n",
           le32(b + 4), rom_error(le32(b + 4)), le32(b + 8), le32(b + 12));
}

struct fwready { uint16_t maj, min, mic, build; char tag[7]; uint32_t abi; uint64_t flags; };

static const char *panic_name(uint32_t code) {   // [TR]:81-95
    static const char *n[] = {"MEM", "WORK", "IPC", "ARCH", "PLATFORM", "TASK", "EXCEPTION", "DEADLOCK", "STACK",
                              "IDLE", "WFI", "ASSERT"};
    return code < 12 ? n[code] : "?";
}

// S19: the FW_READY message. Returns 0 ok, -1 read error, -2 panic, -3 not FW_READY, -4 READY_FAILED (ABI).
static int m3_fw_ready(uint32_t tdr, uint32_t tdd, const struct fwfile *f, struct fwready *r) {
    memset(r, 0, sizeof *r);
    printf("  HIPCTDR 0x%08x: BUSY=%u msg=0x%08x   HIPCTDD 0x%08x (msg_ext)   ([CN]:154-157, hda.h:405-413)\n",
           tdr, tdr >> 31, tdr & 0x7FFFFFFF, tdd);
    if ((tdr & 0x0ffff000u) == 0x0dead000u) {   // SOF_IPC_PANIC_MAGIC / _MASK [TR]:81-82, [CN]:162-179
        uint32_t po = (tdd & 0xFFFFFF) + 0x80000;  // HDA_DSP_PANIC_OFFSET hda.h:166-167
        printf("  !! FIRMWARE PANIC 0x%08x = SOF_IPC_PANIC_%s; panic data at BAR4+0x%05x (HDA_DSP_PANIC_OFFSET):\n",
               tdr & 0x7FFFFFFF, panic_name(tdr & 0xFFF), po);
        if (po >= 0x80000 && po < 0xFFF00) hexdump(4, po, 64);
        uint32_t esz, eo = find_window(f->nwin, (uint32_t (*)[6])f->win, 6, &esz);
        if (eo) { printf("  exception window BAR4+0x%05x (first 0x100 bytes):\n", eo); hexdump(4, eo, 64); }
        return -2;
    }
    uint8_t m[4096];
    if (rd4buf(0x81000, m, 8)) { printf("  !! uplink mailbox BAR4+0x81000 not readable\n"); return -1; }
    uint32_t hsize = le32(m), hcmd = le32(m + 4);
    printf("  mailbox BAR4+0x81000 (dsp_box = HDA_DSP_MBOX_UPLINK_OFFSET, hda.c:916, hda.h:177):\n"
           "    sof_ipc_cmd_hdr size %u cmd 0x%08x -> GLB type 0x%x%s   ([I3]:1049-1075, header.h:33-49)\n",
           hsize, hcmd, hcmd >> 28, (hcmd & 0xF0000000u) == 0x70000000u ? " = SOF_IPC_FW_READY" : "");
    if (hsize < 8 || hsize > 384)   // SOF_IPC_MSG_MAX_SIZE header.h:134, [I3]:1062-1065
        printf("  !! header size %u outside 8..384: Linux drops this message ('The received message size is invalid')\n", hsize);
    if ((hcmd & 0xF0000000u) != 0x70000000u) {
        printf("  !! first message is not FW_READY\n");
        hexdump(4, 0x81000, 32);
        return -3;
    }
    if (rd4buf(0x81000, m, 108)) { printf("  !! fw_ready read failed\n"); return -1; }
    // struct sof_ipc_fw_ready [IN]:61-75 (packed, 108 bytes)
    const uint8_t *v = m + 24;
    r->maj = le16(v + 4); r->min = le16(v + 6); r->mic = le16(v + 8); r->build = le16(v + 10);
    cstr(r->tag, v + 34, 6); r->abi = le32(v + 40);
    r->flags = le32(m + 84) | ((uint64_t)le32(m + 88) << 32);
    printf("    sof_ipc_fw_ready ([IN]:61-75):\n");
    printf("      dspbox_offset 0x%08x hostbox_offset 0x%08x dspbox_size 0x%x hostbox_size 0x%x\n",
           le32(m + 8), le32(m + 12), le32(m + 16), le32(m + 20));
    print_fwver("      ", v);
    printf("      flags 0x%016llx: BUILD=%u LOCKS=%u LOCKSV=%u GDB=%u D3_PERSISTENT=%u   ([IN]:24-28)\n",
           (unsigned long long)r->flags, (unsigned)(r->flags & 1), (unsigned)(r->flags >> 1) & 1,
           (unsigned)(r->flags >> 2) & 1, (unsigned)(r->flags >> 3) & 1, (unsigned)(r->flags >> 4) & 1);
    // sof_ipc3_validate_fw_version [I3]:774-809 (the Linux log lines, verbatim format)
    printf("    Linux would log: \"Firmware info: version %u:%u:%u-%s\"\n", r->maj, r->min, r->mic, r->tag);
    printf("    Linux would log: \"Firmware: ABI %u:%u:%u Kernel ABI 3:23:1\"\n", r->abi >> 24, (r->abi >> 12) & 0xfff, r->abi & 0xfff);
    if (r->flags & 1) {
        char date[13], tim[11]; cstr(date, v + 12, 12); cstr(tim, v + 24, 10);
        printf("    Linux would log: \"Firmware debug build %u on %s-%s - options: GDB %s, lock debug %s, lock vdebug %s\"\n",
               r->build, date, tim, (r->flags & 8) ? "enabled" : "disabled", (r->flags & 2) ? "enabled" : "disabled",
               (r->flags & 4) ? "enabled" : "disabled");
    }
    if (f->have_ver)
        printf("    vs extended manifest: %u.%u.%u-%s ABI 0x%08x -> %s\n", f->vmaj, f->vmin, f->vmic, f->vtag, f->vabi,
               (f->vmaj == r->maj && f->vmin == r->min && f->vmic == r->mic && f->vabi == r->abi) ? "same" : "DIFFERENT");
    // ipc3_fw_parse_ext_data [I3]:585-645: chain of sof_ipc_ext_data_hdr while hdr.cmd == FW_READY
    uint32_t xo = 0x81000 + 108, nw = 0, w[16][6];
    printf("    extended data after fw_ready ([I3]:585-645):\n");
    for (int k = 0; k < 16; k++) {
        uint8_t h[12];
        if (xo + 12 > 0x82000 || rd4buf(xo, h, 12)) break;
        uint32_t esz = le32(h), ecmd = le32(h + 4), etype = le32(h + 8);
        if (ecmd != 0x70000000u) { if (!k) printf("      (none: next header cmd 0x%08x)\n", ecmd); break; }
        if (esz < 12 || xo + esz > 0x82000 || esz > sizeof m) { printf("      !! bad ext header size %u\n", esz); break; }
        if (rd4buf(xo, m, (esz + 3) & ~3u)) break;
        switch (etype) {
        case 1: {   // SOF_IPC_EXT_WINDOW -> sof_ipc3_get_ext_windows [I3]:517-537
            nw = le32(m + 12); if (nw > 16) nw = 16;
            for (uint32_t i = 0; i < nw && 16 + 24 * (i + 1) <= esz; i++)
                for (int q = 0; q < 6; q++) w[i][q] = le32(m + 16 + 24 * i + 4 * q);
            printf("      EXT_WINDOW: %u windows\n", nw);
            print_windows("        ", (int)nw, w);
            if (f->nwin) {
                int same = (int)nw == f->nwin;
                for (uint32_t i = 0; same && i < nw; i++) for (int q = 1; q < 6; q++) if (w[i][q] != f->win[i][q]) same = 0;
                printf("        vs extended manifest windows: %s\n", same ? "identical" :
                       "MISMATCH (Linux: 'mismatch between window descriptor from extended manifest and mailbox')");
            }
            break;
        }
        case 2: if (esz >= 92) { char nm[17], op[5], ds[33]; cstr(nm, m + 40, 16); cstr(op, m + 56, 4); cstr(ds, m + 60, 32);
                    printf("      EXT_CC_INFO: compiler %s %u.%u.%u%s, optimisation %s\n", nm, le32(m + 12), le32(m + 16), le32(m + 20), ds, op); }
                break;
        case 3: if (esz >= 20) printf("      EXT_PROBE_INFO: probe_points_max %u injection_dmas_max %u\n", le32(m + 12), le32(m + 16)); break;
        case 4: if (esz >= 16) { uint32_t a = le32(m + 12); printf("      EXT_USER_ABI_INFO: debug ABI %u.%u.%u\n", a >> 24, (a >> 12) & 0xfff, a & 0xfff); } break;
        default: printf("      unknown ext header type %u size 0x%x\n", etype, esz);
        }
        xo += esz;
    }
    // ipc3_get_windows [I3]:647-761: Linux uses the manifest window descriptor (info_window) if present
    int un = f->nwin ? f->nwin : (int)nw;
    uint32_t (*uw)[6] = f->nwin ? (uint32_t (*)[6])f->win : w;
    if (un) {
        uint32_t s_in, s_out, s_st, s_db;
        uint32_t in = find_window(un, uw, 1, &s_in), out = find_window(un, uw, 0, &s_out);
        uint32_t st = find_window(un, uw, 4, &s_st), db = find_window(un, uw, 3, &s_db);
        uint32_t ex = find_window(un, uw, 6, NULL), tr = find_window(un, uw, 2, NULL), rg = find_window(un, uw, 5, NULL);
        printf("    mailboxes for M4 (ipc3_get_windows [I3]:739-749):\n"
               "      dsp_box (inbox, DSP->host)  BAR4+0x%05x size 0x%x\n"
               "      host_box (outbox, host->DSP) BAR4+0x%05x size 0x%x\n"
               "      stream_box BAR4+0x%05x size 0x%x   debug_box BAR4+0x%05x size 0x%x\n"
               "      exception BAR4+0x%05x   etrace BAR4+0x%05x   regs BAR4+0x%05x\n",
               in, s_in, out, s_out, st, s_st, db, s_db, ex, tr, rg);
        if (!s_in || !s_out) printf("    !! Illegal mailbox window ([I3]:734-737)\n");
    }
    if ((r->abi >> 24) != 3) {   // SOF_ABI_VERSION_INCOMPATIBLE [AB]:54-57, [I3]:787-792
        printf("  !! incompatible FW ABI version (major %u, kernel 3): Linux sets SOF_FW_BOOT_READY_FAILED\n", r->abi >> 24);
        return -4;
    }
    return 0;
}

// ------------------------------------------------------------------ m4
// SoundWire bring-up and enumeration on top of M3 (firmware RUNNING, FW_READY acked, no IPC sent:
// Linux starts the links in hda_dsp_post_fw_run straight after FW_READY, [HD]:443-450, [LO]:150-171).
// Linux references (~/xe/build/audio/linux-src, torvalds master):
//   [IS] drivers/soundwire/intel.c       [IB] drivers/soundwire/intel_bus_common.c
//   [IA] drivers/soundwire/intel_auxdevice.c  [II] drivers/soundwire/intel_init.c
//   [CM] drivers/soundwire/cadence_master.c  [BU] drivers/soundwire/bus.c  [MD] drivers/soundwire/mipi_disco.c
//   [SR] include/linux/soundwire/sdw_registers.h  [SA] sound/hda/core/intel-sdw-acpi.c  [DS] hda-dsp.c
//   [ST] (here) drivers/soundwire/stream.c (row/col tables)
// Values from the live DSDT (SNDW LNK0-3, ~/xe/build/brightkeys2/live/DSDT-live.dsl:9184-9680):
//   intel-sdw-ip-clock 38.4 MHz -> mclk 19.2 MHz ([IA]:175-183); clock-frequencies-supported {4.8 MHz}
//   -> max_clk_freq 4.8 MHz ([MD]:81-112) -> curr_dr_freq 9.6 MHz ([BU]:160-161); frame 48 kHz, 50 rows,
//   4 cols; command-error-threshold 16 ([MD]:151-152); master-count 4 and intel-quirk-mask 0 on all links
//   -> link_mask 0xF ([SA]:84-129). Peripherals: SWD0/1/2 _ADR (RESEARCH.md 1.2).
#define MAGIC_M4 0x4D34474FULL   /* 'M4GO' */
enum { M4_STATE = 32, M4_POWERUP = 33, M4_SHIMINIT = 34, M4_SOFTRESET = 35, M4_SYNCARM = 36, M4_CDNSINIT = 37,
       M4_COMMIT = 38, M4_INTEN = 39, M4_EXITRESET = 40, M4_STATPOLL = 41, M4_XFER = 42 };
enum { XR_OK = 0, XR_IGNORED = 1, XR_NACK = 2, XR_TIMEOUT = 3, XR_KEXT = 4 };

struct acpi_peri { int link; uint64_t adr; const char *acpi, *name, *shortname; };
static const struct acpi_peri g_peri[] = {   // DSDT SWD0/SWD1/SWD2 (_ADR includes the link in [51:48])
    {0, 0x000030025D071401ULL, "SWD0", "RT714 (SDCA mic array codec, rt715-sdca)", "RT714"},
    {1, 0x000131025D131601ULL, "SWD1", "RT1316 (SDCA smart amp, left speaker)", "RT1316"},
    {2, 0x000230025D131601ULL, "SWD2", "RT1316 (SDCA smart amp, right speaker)", "RT1316"},
};

static const char *part_name(uint32_t mfg, uint32_t part) {
    if (mfg == 0x025D && part == 0x1316) return "RT1316";
    if (mfg == 0x025D && part == 0x0714) return "RT714";
    if (mfg == 0x025D && part == 0x0715) return "RT715";
    return "unknown";
}
static uint64_t devid_addr(const uint8_t b[6]) {   // [BU]:856-863
    return (uint64_t)b[5] | ((uint64_t)b[4] << 8) | ((uint64_t)b[3] << 16) | ((uint64_t)b[2] << 24) |
           ((uint64_t)b[1] << 32) | ((uint64_t)b[0] << 40);
}
// sdw_extract_slave_id ([BU]:797-811): version [47:44], unique [43:40], mfg [39:24], part [23:8], class [7:0]
static void print_devid(const char *pfx, uint64_t a) {
    printf("%sSDW Slave Addr: %llx -> version %llu (SoundWire 1.%llu), unique_id 0x%llx, mfg_id 0x%04llx (%s), part_id 0x%04llx (%s), class_id 0x%02llx (%s)\n",
           pfx, (unsigned long long)a, (a >> 44) & 0xF, ((a >> 44) & 0xF) ? ((a >> 44) & 0xF) - 1 : 0, (a >> 40) & 0xF,
           (a >> 24) & 0xFFFF, ((a >> 24) & 0xFFFF) == 0x025D ? "Realtek" : "?", (a >> 8) & 0xFFFF,
           part_name((uint32_t)((a >> 24) & 0xFFFF), (uint32_t)((a >> 8) & 0xFFFF)), a & 0xFF, (a & 0xFF) == 1 ? "SDCA" : "-");
    printf("%sLinux would log (dev_dbg [BU]:807-809): \"SDW Slave class_id 0x%02llx, mfg_id 0x%04llx, part_id 0x%04llx, unique_id 0x%llx, version 0x%llx\"\n",
           pfx, a & 0xFF, (a >> 24) & 0xFFFF, (a >> 8) & 0xFFFF, (a >> 40) & 0xF, (a >> 44) & 0xF);
}
static const char *slv_state(uint32_t s) { static const char *n[] = {"-", "ATTACHED", "ALERT", "RESERVED"}; return n[s & 3]; }
static void print_slvstat(const char *pfx, uint32_t v) {   // MCP_SLAVE_STAT, 2 bits per device 0..11 ([CM]:1015-1022)
    printf("%sSLAVE_STAT 0x%08x:", pfx, v);
    int any = 0;
    for (int d = 0; d < 12; d++) if ((v >> (2 * d)) & 3) { printf(" dev%d=%s", d, slv_state(v >> (2 * d))); any = 1; }
    printf("%s\n", any ? "" : " (no device reports on the bus)");
}

static void cdns_dump(int l) {   // read-only through the generic MMIO method (FIFO window +0x80..0xFF is never read)
    static const struct { uint32_t o; const char *n; } cr[] = {
        {0x00, "MCP_CONFIG"}, {0x04, "MCP_CONTROL"}, {0x08, "MCP_CMDCTRL"}, {0x0C, "MCP_SSPSTAT"},
        {0x10, "FRAME_SHAPE"}, {0x14, "FRAME_SHAPE_IN"}, {0x18, "CONFIG_UPDATE"}, {0x1C, "PHYCTRL"},
        {0x20, "SSP_CTRL0"}, {0x28, "SSP_CTRL1"}, {0x30, "CLK_CTRL0"}, {0x38, "CLK_CTRL1"}, {0x40, "MCP_STAT"},
        {0x44, "MCP_INTSTAT"}, {0x48, "MCP_INTMASK"}, {0x50, "SLAVE_STAT"}, {0x54, "SLAVE_INTSTAT0"},
        {0x58, "SLAVE_INTSTAT1"}, {0x5C, "SLAVE_INTMASK0"}, {0x60, "SLAVE_INTMASK1"}, {0x64, "PORT_INTSTAT"},
        {0x78, "FIFOLEVEL"}, {0x7C, "FIFOSTAT"},
    };
    printf("  -- Cadence link %d @BAR4+0x%05x ([CM]:30-150) --\n", l, 0x30000 + 0x10000 * l);
    for (unsigned i = 0; i < sizeof cr / sizeof cr[0]; i++) {
        int ok; uint32_t a = R(4, 0x30000 + 0x10000 * l + cr[i].o, cr[i].n, &ok);
        if (!ok) continue;
        if (cr[i].o == 0x50) { printf("\n"); print_slvstat("                  ", a); continue; }
        if (cr[i].o == 0x04) printf("  CMD_RST=%u SOFT_RST=%u SW_RST=%u HW_RST=%u CLK_STOP_CLR=%u CMD_ACCEPT=%u BLOCK_WAKEUP=%u",
                                     F(a,7,7), F(a,6,6), F(a,5,5), F(a,4,4), F(a,2,2), F(a,1,1), F(a,0,0));
        else if (cr[i].o == 0x00) printf("  MMASTER=%u BUS_REL=%u SNIFFER=%u CMD=%u OP=%u", F(a,7,7), F(a,6,6), F(a,5,5), F(a,3,3), F(a,2,0));
        else if (cr[i].o == 0x40) printf("  ACTIVE_BANK=%u CLK_STOP=%u", F(a,20,20), F(a,16,16));
        else if (cr[i].o == 0x10 || cr[i].o == 0x14) printf("  rows-idx=%u cols-idx=%u", F(a,7,3), F(a,2,0));
        else if (cr[i].o == 0x30 || cr[i].o == 0x38) printf("  MCLKD=%u", F(a,7,0));
        else if (cr[i].o == 0x7C) printf("  RX_FIFO_AVAIL=%u", F(a,5,0));
        printf("\n");
    }
}

static void shim_dump(const char *title) {
    int ok; uint32_t v;
    printf("  -- SoundWire shim (%s) --\n", title);
    v = R(4, 0x2C000, "LCAP", &ok); if (ok) printf("  LCOUNT=%u MLCS=%u\n", F(v,2,0), F(v,8,8)); else NL();
    v = R(4, 0x2C004, "LCTL", &ok); if (ok) printf("  SPA[3:0]=%x CPA[11:8]=%x\n", F(v,3,0), F(v,11,8)); else NL();
    v = R(4, 0x2C00C, "SYNC", &ok);
    if (ok) printf("  SYNCPRD=%u SYNCCPU=%u CMDSYNC=%x SYNCGO=%u\n", F(v,14,0), F(v,15,15), F(v,19,16), F(v,24,24)); else NL();
    for (int x = 0; x < 4; x++) {
        uint32_t io; if (mmio(4, 0x2C06C + 0x60 * x, &io)) continue;
        uint32_t i = io & 0xFFFF, c = io >> 16;
        printf("  link %d IOCTL=%04x (MIF=%u CO=%u COE=%u DO=%u DOE=%u BKE=%u WPDD=%u) CTMCTL=%04x (DACTQE=%u DODS=%u DOAIS=%u)\n",
               x, i, F(i,0,0), F(i,1,1), F(i,2,2), F(i,3,3), F(i,4,4), F(i,5,5), F(i,6,6), c, F(c,0,0), F(c,1,1), F(c,4,3));
    }
    v = R(4, 0x2C190, "WAKEEN/WAKESTS", &ok); if (ok) printf("  WAKEEN=%04x WAKESTS=%04x\n", v & 0xFFFF, v >> 16); else NL();
    v = R(4, 0x14, "ADSPIS2", &ok); if (ok) printf("  SNDW(b5)=%u (status only; ADSPIC2.SNDW is never enabled)\n", F(v,5,5)); else NL();
}

static void m4_plan(unsigned links, unsigned attachms) {
    printf("  ---- M4: SoundWire (no IPC first: [HD]:443-450 runs right after FW_READY, [LO]:150-171) ----\n");
    printf("  S30 read-only: LCAP.LCOUNT >= ACPI master-count 4 ([DS]:1262-1283), LCTL SPA/CPA = 0, SYNC, IOCTL/CTMCTL,\n"
           "      WAKEEN/WAKESTS (first boot: hda_sdw_process_wakeen is a no-op, [IA]:523-527)\n");
    printf("  S31 intel_link_power_up, first link ([IS]:337-449): SYNC = SYNCPRD 9599 (38.4 MHz/4 kHz-1, [IS]:367-381)\n"
           "      | SYNCCPU; LCTL |= SPA 0x%x (ACPI link_mask, [SA]:113-129); wait CPA 0x%x and SYNCCPU=0 (10 x 100 us,\n"
           "      [IS]:23-38); read CTLSCAP/PCMSCAP (intel_pdi_init [IS]:551-567)\n", links, links << 8);
    printf("  per link in 0x%x, ascending (sdw_intel_startup [II]:297-306 -> intel_link_startup [IA]:383-487, multi_link):\n", links);
    printf("  S32 intel_shim_init ([IS]:258-289): IOCTL = BKE, +WPDD, +DO, +DOE (10-15 us apart), glue->master IP\n"
           "      ([IS]:207-234: DOE=0, DO=0, MIF=1, BKE=COE=0) -> IOCTL = MIF|WPDD; CTMCTL DOAIS=1|DACTQE|DODS\n");
    printf("  S33 intel_start_bus ([IB]:12-73):\n"
           "      a sdw_cdns_soft_reset ([CM]:1406-1425): CONTROL |= SOFT_RST, CONFIG_UPDATE (self-clear), SOFT_RST=0\n"
           "      b intel_shim_sync_arm ([IS]:500-513): SYNC |= CMDSYNC(link)\n"
           "      c sdw_cdns_init ([CM]:1431-1491): CLK_CTRL0/1 MCLKD = 19.2 MHz*2/9.6 MHz-1 = 3, FRAME_SHAPE_INIT 0x09\n"
           "        (rows 50 idx 1, cols 9.6 MHz/48 kHz/50 = 4 idx 1, [ST]:29-34), SSP_CTRL0/1 = 48000/4000 = 12\n"
           "        ([CM]:1353-1400); CONTROL |= CMD_RST, |= CMD_ACCEPT, BLOCK_WAKEUP=0; CONFIG BUS_REL=0; CONFIG OP=normal\n"
           "        CMD=0 SNIFFER=0 MMASTER=1\n"
           "      d CONFIG_UPDATE=1 ([CM]:292-296), SYNC |= SYNCGO ([IS]:515-531), wait CONFIG_UPDATE=0 ([CM]:303-310)\n"
           "      e sdw_cdns_enable_interrupt ([CM]:1209-1270): SLAVE_INTSTAT0/1 w1c, SLAVE_INTMASK0/1 = ~0, INTMASK =\n"
           "        SLAVE|CTRL_CLASH|DATA_CLASH|PARITY|RX_WL|IRQ (no interrupt reaches the CPU: ADSPIC2.SNDW and INTx off)\n"
           "      f sdw_cdns_exit_reset ([CM]:1172-1184): CONTROL |= HW_RST (bus reset), CONFIG_UPDATE; self-clearing\n"
           "        check after 1 ms, HW_RST polled 10 x 1 ms ([CM]:1116-1165, INTEL_MASTER_RESET_ITERATIONS)\n");
    printf("  S34 enumeration per link (cdns attach work [IB]:69, [CM]:1006-1030 -> sdw_handle_slave_status [BU]:1881+):\n"
           "      poll SLAVE_STAT dev0 ATTACHED, up to %u ms; then sdw_program_device_num ([BU]:827-912): read\n"
           "      SCP_DevId_0..5 (0x50-0x55, [SR]:128-133) of device 0 through the command FIFO (FIFOLEVEL = 6, 6 read\n"
           "      commands, poll RX_WL/FIFOSTAT 500 ms, read the responses, INTSTAT RX_WL w1c: [CM]:602-675, 920-944);\n"
           "      IGNORED (no ACK) = 'No more devices'; else decode, assign the first free device number 1..11\n"
           "      ([IA]:265-279, [BU]:115-124) by writing SCP_DevNumber (0x46, [SR]:107) of device 0; repeat (max 24).\n"
           "      Retries per message: err_threshold 16 ([BU]:234-250). Then wait for the new number to report ATTACHED\n"
           "      and read its DevId again (read-only check). Nothing else is written to any peripheral (no\n"
           "      sdw_initialize_slave, no SCP_IntMask, no codec register, no amp power-up).\n", attachms);
    printf("  S35 report: shim + Cadence registers per link, devices found per link vs the DSDT\n");
}

// One sdw_transfer: do_transfer retries up to err_threshold (16) times unless OK or IGNORED ([BU]:234-250).
static int sdw_xfer(int link, int cmd, int dev, int reg, int cnt, int data, uint8_t *buf, int *tries, uint64_t o[16], int verbose) {
    int res = XR_TIMEOUT;
    for (int i = 0; i <= 16; i++) {
        uint64_t a0 = (uint64_t)link | ((uint64_t)cnt << 8) | ((uint64_t)cmd << 16) | ((uint64_t)dev << 24) | ((uint64_t)reg << 32);
        if (m2call(M4_XFER, a0, (uint64_t)data, o)) return XR_KEXT;
        if (o[0] != M2R_OK) { printf("    !! FIFO message refused by the kext (%s)\n", m2rc(o[0])); return XR_KEXT; }
        res = (int)o[1];
        if (tries) *tries = i + 1;
        if (verbose || res != XR_OK) {
            static const char *rn[] = {"OK", "IGNORED (no ACK)", "NACK", "TIMEOUT (500 ms)"};
            printf("    %s dev %d reg 0x%02x x%d%s: %s after %llu us, %llu response(s):", cmd == 2 ? "READ " : "WRITE", dev, reg, cnt,
                   cmd == 3 ? " (SCP_DevNumber)" : "", rn[res & 3], o[14], o[2]);
            for (uint64_t k = 0; k < o[2] && k < 10; k++) printf(" %08llx", o[3 + k]);
            printf("  [INTSTAT %08llx FIFOSTAT %08llx]\n", o[13], o[15]);
        }
        if (res == XR_OK) {
            if (buf) for (int k = 0; k < cnt; k++) buf[k] = (uint8_t)((o[3 + k] >> 8) & 0xFF);   // RESP_RDATA [15:8]
            return XR_OK;
        }
        if (res == XR_IGNORED) return XR_IGNORED;
        printf("    retry %d/16 (do_transfer, [BU]:240-247)\n", i + 1);
    }
    return res;
}

struct m4dev { uint64_t adr; int num; int attached; int verified; };
struct m4link { int ran, attach0, ndev, err; struct m4dev d[12]; };

// S30-S35. Returns 0 on success (all three DSDT peripherals enumerated and verified), 1 when the
// sequence completed but the result differs from the DSDT, -1 on an error (caller rolls back).
static int m4_sdw(const struct bootopt *op, char *summary, size_t sn, const char **failstep, uint64_t o[16]) {
    struct m4link L[4];
    memset(L, 0, sizeof L);
    uint32_t v;

    m2b_step("[S30] SoundWire pre-checks (read-only)   (hda.c:231-250, hda-dsp.c:1262-1283, hda-loader.c:335)");
    shim_dump("before power-up");
    v = 0; mmio(4, 0x2C000, &v);
    printf("  LCAP.LCOUNT %u vs ACPI mipi-sdw-master-count 4: %s\n", v & 7, (v & 7) >= 4 ? "ok" : "FAIL ('BIOS master count is larger than hardware capabilities')");
    if ((v & 7) < 4) { *failstep = "S30 LCAP.LCOUNT"; o[0] = M2R_PRECOND; return -1; }
    v = 0; mmio(4, 0x2C004, &v);
    if (v & 0xF0F) { printf("  !! LCTL SPA/CPA already set (0x%08x): someone else powered the links\n", v); *failstep = "S30 LCTL not idle"; o[0] = M2R_PRECOND; return -1; }

    m2b_step("[S31] intel_link_power_up (first link): SYNCPRD/SYNCCPU, LCTL.SPA, CPA   (intel.c:337-449)");
    if (m2call(M4_POWERUP, op->links, 0, o)) { *failstep = "S31"; return -1; }
    printf("  LCAP 0x%08llx  LCTL 0x%08llx -> 0x%08llx (CPA after ~%llu us)  SYNC 0x%08llx -> 0x%08llx (SYNCCPU=0 after ~%llu us)  %s\n",
           o[1], o[2], o[4], o[5], o[3], o[6], o[7], m2rc(o[0]));
    printf("  LCTL final 0x%08llx  (SPA=%llx CPA=%llx)\n", o[8], o[8] & 0xF, (o[8] >> 8) & 0xF);
    for (int k = 0; k < 4; k++) {
        uint32_t c = (uint32_t)o[10 + k], pc = c >> 16;
        printf("  link %d CTLSCAP=%04x PCMSCAP=%04x (ISS=%u OSS=%u BSS=%u)  (intel_pdi_init, intel.c:551-567)\n", k, c & 0xFFFF, pc,
               F(pc,3,0), F(pc,7,4), F(pc,12,8));
    }
    if (o[0] == M2R_TIMEOUT) printf("  !! CPA did not follow SPA within 1 ms. Linux: 'Failed to power up link' (intel.c:416-420)\n");
    if (o[0] == M2R_VERIFY) printf("  !! SYNCCPU did not clear. Linux: 'Failed to set SHIM_SYNC' (intel.c:422-429)\n");
    if (o[0] != M2R_OK) { *failstep = "S31 link power-up"; return -1; }

    for (int l = 0; l < 4; l++) {
        if (!(op->links & (1u << l))) continue;
        char t[160];
        snprintf(t, sizeof t, "[S32.%d] link %d: intel_shim_init + glue -> master IP   (intel.c:258-289, 207-234)", l, l);
        m2b_step(t);
        if (m2call(M4_SHIMINIT, (uint64_t)l, 0, o)) { *failstep = "S32"; return -1; }
        printf("  IOCTL 0x%04llx -> after init 0x%04llx -> after handoff 0x%04llx (expect MIF|WPDD = 0x0041)  CTMCTL 0x%04llx -> 0x%04llx (expect 0x000b)  %s\n",
               o[1] & 0xFFFF, o[2], o[3], o[1] >> 16, o[4], m2rc(o[0]));
        if (o[0] == M2R_VERIFY) printf("  (read-back differs from the written value; Linux does not verify these; continuing)\n");
        else if (o[0] != M2R_OK) { *failstep = "S32 shim init"; return -1; }

        snprintf(t, sizeof t, "[S33.%d] link %d: intel_start_bus   (intel_bus_common.c:12-73)", l, l); m2b_step(t);
        if (m2call(M4_SOFTRESET, (uint64_t)l, 0, o)) { *failstep = "S33a"; return -1; }
        printf("  a soft reset ([CM]:1406-1425): CONTROL 0x%08llx -> 0x%08llx, CONFIG_UPDATE last 0x%llx (~%llu us), SOFT_RST clear (~%llu us); MCP_STAT 0x%08llx MCP_CONFIG 0x%08llx  %s\n",
               o[1], o[4], o[2], o[3], o[7], o[5], o[6], m2rc(o[0]));
        if (o[0] != M2R_OK) {
            printf("  !! Linux: '%s' (%s)\n", o[0] == M2R_TIMEOUT ? "Config update timedout" : "Soft Reset timed out",
                   o[0] == M2R_TIMEOUT ? "cadence_master.c:282-283" : "cadence_master.c:1421-1422");
            *failstep = "S33a soft reset"; return -1;
        }
        if (m2call(M4_SYNCARM, (uint64_t)l, 0, o)) { *failstep = "S33b"; return -1; }
        printf("  b sync arm ([IS]:500-513): SYNC 0x%08llx -> 0x%08llx  %s\n", o[1], o[2], m2rc(o[0]));
        if (o[0] != M2R_OK) { *failstep = "S33b sync arm"; return -1; }
        if (m2call(M4_CDNSINIT, (uint64_t)l, 0, o)) { *failstep = "S33c"; return -1; }
        printf("  c sdw_cdns_init ([CM]:1431-1491): CLK_CTRL0 0x%08llx CLK_CTRL1 0x%08llx (MCLKD %llu) FRAME_SHAPE_INIT 0x%02llx SSP_CTRL0/1 %llu/%llu\n"
               "    CONTROL 0x%08llx (before CMD_RST 0x%08llx) MCP_CONFIG 0x%08llx; msg_count = FIFOLEVEL %llu  %s\n",
               o[4], o[5], o[4] & 0xFF, o[6], o[7], o[8], o[9], o[1], o[10], o[3], m2rc(o[0]));
        if (o[0] != M2R_OK) { *failstep = "S33c cdns init"; return -1; }
        if (m2call(M4_COMMIT, (uint64_t)l, 0, o)) { *failstep = "S33d"; return -1; }
        printf("  d CONFIG_UPDATE=1 (reads 0x%llx), SYNC 0x%08llx -> SYNCGO 0x%08llx -> now 0x%08llx; CONFIG_UPDATE 0x%llx after ~%llu us; CONTROL 0x%08llx CONFIG 0x%08llx  %s\n",
               o[2], o[3], o[4], o[7], o[5], o[6], o[8], o[9], m2rc(o[0]));
        if (o[0] != M2R_OK) { printf("  !! Linux: 'CONFIG_UPDATE BIT still set' (intel_bus_common.c:48-52)\n"); *failstep = "S33d config update / sync go"; return -1; }
        if (m2call(M4_INTEN, (uint64_t)l, 0, o)) { *failstep = "S33e"; return -1; }
        printf("  e interrupt masks ([CM]:1209-1270): SLAVE_INTSTAT0/1 were 0x%08llx/0x%08llx (cleared); SLAVE_INTMASK0/1 0x%08llx/0x%08llx INTMASK 0x%08llx; INTSTAT 0x%08llx ADSPIS2 0x%08llx  %s\n",
               o[1], o[2], o[3], o[4], o[5], o[6], o[7], m2rc(o[0]));
        if (o[0] != M2R_OK) { *failstep = "S33e interrupt masks"; return -1; }
        if (m2call(M4_EXITRESET, (uint64_t)l, 0, o)) { *failstep = "S33f"; return -1; }
        printf("  f exit reset ([CM]:1172-1184, 1116-1165): CONFIG_UPDATE last 0x%llx (~%llu us); after 1 ms CONTROL 0x%08llx CONFIG_UPDATE 0x%llx;\n"
               "    HW_RST %s (CONTROL 0x%08llx, %llu extra ms); MCP_STAT 0x%08llx INTSTAT 0x%08llx FIFOSTAT 0x%08llx SLAVE_INTSTAT0/1 0x%08llx/0x%08llx  %s\n",
               o[1], o[2], o[3], o[4], (o[5] & 0x10) ? "STILL SET" : "self-cleared", o[5], o[6], o[7], o[9], o[10], o[11], o[12], m2rc(o[0]));
        print_slvstat("    ", (uint32_t)o[8]);
        if (o[13]) printf("  !! self-clearing check (sdw_cdns_check_self_clearing_bits): mask 0x%llx (b0 SW_RST b1 CMD_RST b2 SOFT_RST b3 CLK_STOP_CLR b4 CONFIG_UPDATE b5 HW_RST); Linux only logs this\n", o[13]);
        if (o[0] == M2R_TIMEOUT) { printf("  !! Linux: 'unable to exit bus reset sequence' (intel_bus_common.c:60-64)\n"); *failstep = "S33f exit reset"; return -1; }
        L[l].ran = 1;
    }

    for (int l = 0; l < 4; l++) {
        if (!L[l].ran) continue;
        char t[200];
        snprintf(t, sizeof t, "[S34.%d] link %d: enumeration   (cadence_master.c:1006-1030 -> bus.c:1881-1925, 827-912)", l, l);
        m2b_step(t);
        int exp = -1;
        for (unsigned k = 0; k < sizeof g_peri / sizeof g_peri[0]; k++) if (g_peri[k].link == l) exp = (int)k;
        if (exp >= 0) printf("  DSDT expects %s _ADR 0x%016llx: %s\n", g_peri[exp].acpi, (unsigned long long)g_peri[exp].adr, g_peri[exp].name);
        else printf("  DSDT describes no peripheral on link %d (enabled in ACPI, unused): expect nothing\n", l);
        if (m2call(M4_STATPOLL, (uint64_t)l | (0ULL << 8) | (1ULL << 16), op->attachms, o)) { *failstep = "S34 poll"; return -1; }
        printf("  waited ~%llu us for dev0 ATTACHED: %s;  MCP_STAT 0x%08llx INTSTAT 0x%08llx SLAVE_INTSTAT0/1 0x%08llx/0x%08llx\n",
               o[2], o[0] == M2R_OK ? "attached" : "nothing", o[6], o[3], o[4], o[5]);
        print_slvstat("  ", (uint32_t)o[1]);
        if (o[0] != M2R_OK) { L[l].attach0 = 0; continue; }
        L[l].attach0 = 1;
        // sdw_program_device_num: do { DevId read on #0; assign } while (ret == 0 && count < 24)
        uint32_t used = 0;   // bus->assigned for device numbers 1..11 on this link
        for (int count = 0; count < 24; count++) {
            uint8_t id[6] = {0}; int tries = 0;
            printf("  [%d] read SCP_DevId_0..5 of device 0 ([BU]:837-846)\n", count);
            int r = sdw_xfer(l, 2, 0, 0x50, 6, 0, id, &tries, o, 1);
            if (r == XR_IGNORED) { printf("    -> IGNORED: Linux 'No more devices to enumerate' ([BU]:841-844)\n"); break; }
            if (r != XR_OK) { printf("    !! DEVID read failed: Linux 'DEVID read fail' ([BU]:846-849)\n"); L[l].err = 1; break; }
            uint64_t a = devid_addr(id);
            printf("    DevId bytes %02x %02x %02x %02x %02x %02x\n", id[0], id[1], id[2], id[3], id[4], id[5]);
            print_devid("    ", a);
            if (exp >= 0) {
                uint64_t e = g_peri[exp].adr & 0xFFFFFFFFFFFFULL;
                int same = ((a ^ e) & 0x0000FFFFFFFFFFULL) == 0, uniq = ((a >> 40) & 0xF) == ((e >> 40) & 0xF);
                printf("    vs %s: mfg/part/class %s, unique_id %s (0x%llx vs 0x%llx), version %s\n", g_peri[exp].acpi, same ? "MATCH" : "DIFFER",
                       uniq ? "match" : "differs", (a >> 40) & 0xF, (e >> 40) & 0xF, ((a >> 44) & 0xF) == ((e >> 44) & 0xF) ? "match" : "differs");
            } else printf("    !! no DSDT entry on this link: Linux would add it anyway and log 'Slave Entry not found' ([BU]:891-903)\n");
            int num = 0;
            for (int n = 1; n <= 11; n++) if (!(used & (1u << n))) { num = n; break; }
            if (!num) { printf("    !! no free device number ('Get dev_num failed', [BU]:763-767)\n"); L[l].err = 1; break; }
            printf("  [%d] assign device number %d: write SCP_DevNumber (0x46) of device 0 = %d   (sdw_assign_device_num [BU]:752-795)\n", count, num, num);
            r = sdw_xfer(l, 3, 0, 0x46, 1, num, NULL, &tries, o, 1);
            if (r != XR_OK) { printf("    !! Linux: 'Program device_num %d failed' ([BU]:779-784)\n", num); L[l].err = 1; break; }
            used |= 1u << num;
            if (L[l].ndev < 12) { L[l].d[L[l].ndev].adr = a; L[l].d[L[l].ndev].num = num; L[l].ndev++; }
        }
        // the new numbers report ATTACHED (bus.c:880-883 comment); then read-only DevId check on the new number
        for (int k = 0; k < L[l].ndev; k++) {
            struct m4dev *d = &L[l].d[k];
            if (m2call(M4_STATPOLL, (uint64_t)l | ((uint64_t)d->num << 8) | (1ULL << 16), 200, o)) { *failstep = "S34 poll2"; return -1; }
            d->attached = (o[0] == M2R_OK);
            printf("  device %d: %s after ~%llu us   ", d->num, d->attached ? "ATTACHED" : "NOT attached", o[2]);
            print_slvstat("", (uint32_t)o[1]);
            if (!d->attached) continue;
            uint8_t id[6] = {0}; int tries = 0;
            printf("  device %d: read SCP_DevId_0..5 on the new number (read-only check)\n", d->num);
            int r = sdw_xfer(l, 2, d->num, 0x50, 6, 0, id, &tries, o, 1);
            d->verified = (r == XR_OK && devid_addr(id) == d->adr);
            printf("    -> %s (0x%012llx)\n", r != XR_OK ? "read failed" : d->verified ? "same ID: enumerated" : "DIFFERENT ID", (unsigned long long)devid_addr(id));
        }
        // acknowledge the slave status like cdns_update_slave_status_work (cadence_master.c:1047-1060), read-only otherwise
        if (m2call(M4_STATPOLL, (uint64_t)l | (0ULL << 8) | (0ULL << 16) | (1ULL << 24), 50, o) == 0) {
            printf("  final: ");
            print_slvstat("", (uint32_t)o[1]);
            printf("         INTSTAT 0x%08llx SLAVE_INTSTAT0/1 0x%08llx/0x%08llx (acknowledged, w1c)\n", o[3], o[4], o[5]);
        }
    }

    m2b_step("[S35] SoundWire report");
    shim_dump("after enumeration");
    for (int l = 0; l < 4; l++) if (L[l].ran) cdns_dump(l);
    int good = 1;
    size_t p = 0;
    summary[0] = 0;
    for (int l = 0; l < 4; l++) {
        if (!(op->links & (1u << l))) continue;
        int exp = -1;
        for (unsigned k = 0; k < sizeof g_peri / sizeof g_peri[0]; k++) if (g_peri[k].link == l) exp = (int)k;
        printf("  link %d: ", l);
        if (!L[l].ndev) printf("no peripheral%s\n", exp >= 0 ? "  !! the DSDT expects one" : " (as expected)");
        int hit = 0;
        for (int k = 0; k < L[l].ndev; k++) {
            struct m4dev *d = &L[l].d[k];
            const char *nm = part_name((uint32_t)((d->adr >> 24) & 0xFFFF), (uint32_t)((d->adr >> 8) & 0xFFFF));
            printf("%sdev %d %s 0x%012llx (mfg 0x%04llx part 0x%04llx class 0x%02llx unique %llu v%llu) %s%s\n", k ? "          " : "",
                   d->num, nm, (unsigned long long)d->adr, (d->adr >> 24) & 0xFFFF, (d->adr >> 8) & 0xFFFF, d->adr & 0xFF,
                   (d->adr >> 40) & 0xF, (d->adr >> 44) & 0xF, d->attached ? "attached" : "NOT attached", d->verified ? ", verified" : ", NOT verified");
            if (exp >= 0 && ((d->adr ^ g_peri[exp].adr) & 0x0000FFFFFFFFFFULL) == 0 && d->verified) hit = 1;
            if (p < sn) p += (size_t)snprintf(summary + p, sn - p, "%slink%d: %s dev%d unique %llu id 0x%012llx", p ? ", " : "", l, nm, d->num,
                                              (d->adr >> 40) & 0xF, (unsigned long long)d->adr);
        }
        if (exp >= 0 && !hit) good = 0;
        if (exp < 0 && L[l].ndev) good = 0;
        if (L[l].err) good = 0;
    }
    if (!p) snprintf(summary, sn, "no peripheral enumerated");
    return good ? 0 : 1;
}

// ------------------------------------------------------------------ m5: topology parser + IPC3 message plan
// User-space port of what Linux does with sof-rpl-rt1316-l12-rt714-l0.tplg for the "Speaker" PCM (pcm 2).
// References (~/xe/build/audio/linux-src, torvalds master):
//   [AS]  include/uapi/sound/asoc.h (ALSA topology binary)    [TK]  include/uapi/sound/sof/tokens.h
//   [STP] sound/soc/soc-topology.c   [SDP] sound/soc/soc-dapm.c   [TP] sound/soc/sof/topology.c
//   [I3T] sound/soc/sof/ipc3-topology.c   [I3C] sound/soc/sof/ipc3-control.c   [I3P] sound/soc/sof/ipc3-pcm.c
//   [SA2] sound/soc/sof/sof-audio.c   [SP] sound/soc/sof/pcm.c   [HS] include/sound/sof/header.h
//   [TH] include/sound/sof/topology.h [DH] include/sound/sof/dai.h [STH] include/sound/sof/stream.h
//   [CH] include/sound/sof/control.h  [SDH] sound/soc/sof/intel/hda.c (sdw_params_stream)
//   [SIC] drivers/soundwire/intel.c
// Struct sizes were checked against the Linux headers with clang: comp 28, comp_config 36, host 76, volume 84,
// process 100, dai 80, buffer 44, pipe_new 48, ready/free/stream 12, connect 16, dai_config 216 (alh @52),
// pcm_params 108, ctrl_data 92, abi_hdr 32.

#define TP_NAME 44
struct tp_tuple { uint32_t tok, type, val; char str[TP_NAME]; uint8_t uuid[16]; };
struct tp_kctl {
    uint32_t type;                  // SND_SOC_TPLG_TYPE_MIXER 1 / BYTES 2 / ENUM 3
    char name[TP_NAME];
    uint32_t info, access, tlv_type;
    int32_t min, max, nch, tlv_min, tlv_step, tlv_mute;
    uint8_t priv[1024]; uint32_t privsz, max_size;
};
struct tp_widget {
    uint32_t id; char name[TP_NAME], sname[TP_NAME];
    uint32_t index, comp_id;
    int ntup; struct tp_tuple tup[48];
    int nk; struct tp_kctl k[4];
};
struct tp_route { char src[TP_NAME], sink[TP_NAME], ctl[TP_NAME]; uint32_t index; };
struct tp_pcm { char name[TP_NAME], dai[TP_NAME]; uint32_t pcm_id, dai_id, pb, cap; char sname[2][TP_NAME]; uint64_t fmts[2]; uint32_t caps[2][12]; };
struct tp_link { uint32_t id; char name[TP_NAME]; uint32_t fmt, nhw; int ntup; struct tp_tuple tup[16]; };
struct tplg {
    int nw, nr, np, nl;
    struct tp_widget w[96]; struct tp_route r[160]; struct tp_pcm p[16]; struct tp_link l[16];
    uint32_t man[6]; uint32_t next_comp_id; size_t size;
};

enum { W_INPUT = 0, W_OUTPUT = 1, W_MUX = 2, W_MIXER = 3, W_PGA = 4, W_OUTDRV = 5, W_ADC = 6, W_DAC = 7, W_SWITCH = 8,
       W_AIF_IN = 11, W_AIF_OUT = 12, W_DAI_IN = 13, W_DAI_OUT = 14, W_BUFFER = 16, W_SCHED = 17, W_EFFECT = 18 };
static const char *wid_name(uint32_t id) {   // [AS] SND_SOC_TPLG_DAPM_*
    static const char *n[] = {"input", "output", "mux", "mixer", "pga", "out_drv", "adc", "dac", "switch", "pre", "post",
                              "aif_in", "aif_out", "dai_in", "dai_out", "dai_link", "buffer", "scheduler", "effect", "siggen",
                              "src", "asrc", "encoder", "decoder"};
    return id < 24 ? n[id] : "?";
}

static void tp_str(char *d, const uint8_t *s) { memcpy(d, s, TP_NAME); d[TP_NAME - 1] = 0; }

// SOF vendor tuple arrays in a private data block ([AS] snd_soc_tplg_vendor_array, [TP] sof_parse_tokens)
static int tp_tuples(const uint8_t *p, uint32_t sz, struct tp_tuple *t, int max) {
    int n = 0;
    uint32_t o = 0;
    while (o + 12 <= sz) {
        uint32_t asz = le32(p + o), aty = le32(p + o + 4), ne = le32(p + o + 8);
        if (asz < 12 || o + asz > sz) break;
        uint32_t q = o + 12;
        for (uint32_t i = 0; i < ne && n < max; i++) {
            memset(&t[n], 0, sizeof t[n]);
            t[n].type = aty;
            if (aty == 0) { if (q + 20 > o + asz) break; t[n].tok = le32(p + q); memcpy(t[n].uuid, p + q + 4, 16); q += 20; }
            else if (aty == 1) { if (q + 48 > o + asz) break; t[n].tok = le32(p + q); tp_str(t[n].str, p + q + 4); q += 48; }
            else { if (q + 8 > o + asz) break; t[n].tok = le32(p + q); t[n].val = le32(p + q + 4); q += 8; }
            n++;
        }
        o += asz;
    }
    return n;
}
static const struct tp_tuple *tp_find(const struct tp_tuple *t, int n, uint32_t tok) {
    for (int i = 0; i < n; i++) if (t[i].tok == tok) return &t[i];
    return NULL;
}
static uint32_t tp_u32(const struct tp_widget *w, uint32_t tok, uint32_t def) { const struct tp_tuple *x = tp_find(w->tup, w->ntup, tok); return x ? x->val : def; }
static const char *tp_s(const struct tp_widget *w, uint32_t tok) { const struct tp_tuple *x = tp_find(w->tup, w->ntup, tok); return x ? x->str : NULL; }

// [TP] get_token_comp_format: "s16le" -> S16_LE 0, "s24le" -> S24_4LE 1, "s32le" -> S32_LE 2, "float" -> 3
static uint32_t tp_fmt(const char *s) {
    if (!s) return 0;
    if (!strcmp(s, "s24le")) return 1;
    if (!strcmp(s, "s32le")) return 2;
    if (!strcmp(s, "float")) return 3;
    return 0;
}
static const char *fmt_name(uint32_t f) { static const char *n[] = {"S16_LE", "S24_4LE", "S32_LE", "FLOAT"}; return f < 4 ? n[f] : "?"; }

static int tplg_load(struct tplg *tp, const char *path, int verbose) {
    memset(tp, 0, sizeof *tp);
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("  !! cannot open topology %s\n", path); return -1; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (sz < 36 || sz > 4 * 1024 * 1024) { fclose(fp); printf("  !! topology size %ld out of range\n", sz); return -1; }
    uint8_t *d = malloc((size_t)sz);
    if (!d || fread(d, 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); free(d); return -1; }
    fclose(fp);
    tp->size = (size_t)sz;
    if (verbose) printf("topology %s: %ld bytes, FNV-1a 0x%08x\n", path, sz, fnv1a(d, (size_t)sz));
    size_t off = 0;
    int bad = 0;
    while (off + 36 <= (size_t)sz && !bad) {   // [AS] snd_soc_tplg_hdr, [STP] soc_tplg_load_header
        uint32_t magic = le32(d + off), abi = le32(d + off + 4), type = le32(d + off + 12), hsz = le32(d + off + 16);
        uint32_t psz = le32(d + off + 24), index = le32(d + off + 28), count = le32(d + off + 32);
        if (magic != 0x41536F43u) { printf("  !! bad block magic 0x%08x at 0x%zx\n", magic, off); bad = 1; break; }
        if (abi < 5) { printf("  !! topology ABI %u < 5\n", abi); bad = 1; break; }
        size_t p = off + hsz, end = p + psz;
        if (end > (size_t)sz) { printf("  !! block at 0x%zx overruns the file\n", off); bad = 1; break; }
        for (uint32_t e = 0; e < count && !bad; e++) {
            if (type == 5) {   // SND_SOC_TPLG_TYPE_DAPM_WIDGET [AS] snd_soc_tplg_dapm_widget (132 bytes)
                if (tp->nw >= 96 || p + 132 > end) { bad = 1; break; }
                struct tp_widget *w = &tp->w[tp->nw];
                memset(w, 0, sizeof *w);
                w->id = le32(d + p + 4); tp_str(w->name, d + p + 8); tp_str(w->sname, d + p + 52);
                w->index = index;
                uint32_t nk = le32(d + p + 124), ps = le32(d + p + 128);
                if (p + 132 + ps > end) { bad = 1; break; }
                w->ntup = tp_tuples(d + p + 132, ps, w->tup, 48);
                size_t q = p + 132 + ps;
                for (uint32_t k = 0; k < nk; k++) {   // kcontrols follow the widget ([STP] soc_tplg_dapm_widget_create)
                    if (q + 204 > end) { bad = 1; break; }
                    struct tp_kctl kc; memset(&kc, 0, sizeof kc);
                    kc.type = le32(d + q + 4); tp_str(kc.name, d + q + 8); kc.access = le32(d + q + 52); kc.info = le32(d + q + 64);
                    kc.tlv_type = le32(d + q + 72); kc.tlv_min = (int32_t)le32(d + q + 76); kc.tlv_step = (int32_t)le32(d + q + 80);
                    kc.tlv_mute = (int32_t)le32(d + q + 84);
                    size_t poff, adv;
                    if (kc.type == 1) {        // snd_soc_tplg_mixer_control (360)
                        kc.min = (int32_t)le32(d + q + 208); kc.max = (int32_t)le32(d + q + 212); kc.nch = (int32_t)le32(d + q + 224);
                        poff = 356;
                    } else if (kc.type == 2) { // snd_soc_tplg_bytes_control (240)
                        kc.max_size = le32(d + q + 208); poff = 236;
                    } else if (kc.type == 3) { // snd_soc_tplg_enum_control (1764)
                        kc.nch = (int32_t)le32(d + q + 208); kc.max = (int32_t)le32(d + q + 340); poff = 1760;
                    } else { printf("  !! unknown kcontrol type %u in widget %s\n", kc.type, w->name); bad = 1; break; }
                    if (q + poff + 4 > end) { bad = 1; break; }
                    kc.privsz = le32(d + q + poff);
                    adv = poff + 4 + kc.privsz;
                    if (q + adv > end || kc.privsz > sizeof kc.priv) { bad = 1; break; }
                    memcpy(kc.priv, d + q + poff + 4, kc.privsz);
                    if (w->nk < 4) w->k[w->nk++] = kc;
                    q += adv;
                }
                // [TP] sof_widget_ready: swidget->comp_id = sdev->next_comp_id++ for every widget, load order
                w->comp_id = tp->next_comp_id++;
                tp->nw++;
                p = q;
            } else if (type == 4) {   // DAPM_GRAPH: snd_soc_tplg_dapm_graph_elem (132)
                if (tp->nr >= 160 || p + 132 > end) { bad = 1; break; }
                struct tp_route *r = &tp->r[tp->nr++];
                tp_str(r->sink, d + p); tp_str(r->ctl, d + p + 44); tp_str(r->src, d + p + 88); r->index = index;
                p += 132;
            } else if (type == 7) {   // PCM: snd_soc_tplg_pcm (912 + priv)
                if (tp->np >= 16 || p + 912 > end) { bad = 1; break; }
                struct tp_pcm *c = &tp->p[tp->np++];
                tp_str(c->name, d + p + 4); tp_str(c->dai, d + p + 48);
                c->pcm_id = le32(d + p + 92); c->dai_id = le32(d + p + 96); c->pb = le32(d + p + 100); c->cap = le32(d + p + 104);
                for (int k = 0; k < 2; k++) {
                    size_t cb = p + 692 + 104 * (size_t)k;
                    tp_str(c->sname[k], d + cb + 4);
                    c->fmts[k] = le32(d + cb + 48) | ((uint64_t)le32(d + cb + 52) << 32);
                    for (int j = 0; j < 12; j++) c->caps[k][j] = le32(d + cb + 56 + 4 * j);
                }
                p += le32(d + p) + le32(d + p + 908);
            } else if (type == 6 || type == 10) {   // DAI_LINK / BACKEND_LINK: snd_soc_tplg_link_config (1656 + priv)
                if (tp->nl >= 16 || p + 1656 > end) { bad = 1; break; }
                struct tp_link *l = &tp->l[tp->nl++];
                l->id = le32(d + p + 4); tp_str(l->name, d + p + 8);
                l->fmt = le32(d + p + 676 + 8); l->nhw = le32(d + p + 1636);
                uint32_t ps = le32(d + p + 1652);
                if (p + 1656 + ps > end) { bad = 1; break; }
                l->ntup = tp_tuples(d + p + 1656, ps, l->tup, 16);
                p += le32(d + p) + ps;
            } else if (type == 8) {   // MANIFEST
                for (int k = 0; k < 6; k++) tp->man[k] = le32(d + p + 4 + 4 * k);
                p += le32(d + p) + le32(d + p + 108);
            } else {   // controls / DAI / others are not needed for the speaker path
                p = end;
                break;
            }
            if (p > end) { bad = 1; break; }
        }
        off = end;
    }
    free(d);
    if (bad) { printf("  !! topology parse error\n"); return -1; }
    if (verbose)
        printf("  manifest: %u controls, %u widgets, %u routes, %u PCMs, %u DAI links ([AS] snd_soc_tplg_manifest)\n"
               "  parsed: %d widgets (comp ids 0..%u in load order, [TP]:1447), %d routes, %d PCMs, %d BE links\n",
               tp->man[0], tp->man[1], tp->man[2], tp->man[3], tp->man[4], tp->nw, tp->next_comp_id - 1, tp->nr, tp->np, tp->nl);
    return 0;
}

static int tp_widget_by_name(const struct tplg *tp, const char *n) {
    for (int i = 0; i < tp->nw; i++) if (!strcmp(tp->w[i].name, n)) return i;
    return -1;
}
static int tp_sched_of(const struct tplg *tp, uint32_t index) {   // [TP] sof_set_widget_pipeline: same pipeline_id
    for (int i = 0; i < tp->nw; i++) if (tp->w[i].id == W_SCHED && tp->w[i].index == index) return i;
    return -1;
}

static void tp_print_tuples(const struct tp_tuple *t, int n) {
    for (int i = 0; i < n; i++) {
        if (t[i].type == 0) {
            printf(" %u=", t[i].tok);
            for (int k = 0; k < 16; k++) printf("%02x", t[i].uuid[k]);
        } else if (t[i].type == 1) printf(" %u='%s'", t[i].tok, t[i].str);
        else printf(" %u=%u", t[i].tok, t[i].val);
    }
}

static void tplg_dump(const struct tplg *tp) {
    printf("\n-- widgets (block index = pipeline id; comp id = Linux next_comp_id order) --\n");
    for (int i = 0; i < tp->nw; i++) {
        const struct tp_widget *w = &tp->w[i];
        printf("  comp %2u pipe %u %-9s %-24s sname '%s'  tuples:", w->comp_id, w->index, wid_name(w->id), w->name, w->sname);
        tp_print_tuples(w->tup, w->ntup);
        printf("\n");
        for (int k = 0; k < w->nk; k++) {
            const struct tp_kctl *c = &w->k[k];
            printf("      kcontrol %s '%s' info %u", c->type == 1 ? "mixer" : c->type == 2 ? "bytes" : "enum", c->name, c->info);
            if (c->type == 1) printf(" min %d max %d ch %d tlv type %u min %d step %d mute %d", c->min, c->max, c->nch, c->tlv_type,
                                     c->tlv_min, c->tlv_step, c->tlv_mute);
            if (c->type == 2) printf(" max_size %u priv %u bytes", c->max_size, c->privsz);
            printf("\n");
        }
    }
    printf("-- routes (source -> sink) --\n");
    for (int i = 0; i < tp->nr; i++) printf("  [%u] %s -> %s%s%s\n", tp->r[i].index, tp->r[i].src, tp->r[i].sink,
                                            tp->r[i].ctl[0] ? " via " : "", tp->r[i].ctl);
    printf("-- PCMs --\n");
    for (int i = 0; i < tp->np; i++) {
        const struct tp_pcm *c = &tp->p[i];
        printf("  pcm %u '%s' dai '%s' playback %u capture %u", c->pcm_id, c->name, c->dai, c->pb, c->cap);
        for (int k = 0; k < 2; k++) if (c->sname[k][0])
            printf("  %s '%s' formats 0x%llx rate %u-%u ch %u-%u periods %u-%u period %u-%u buffer %u-%u", k ? "capture" : "playback",
                   c->sname[k], (unsigned long long)c->fmts[k], c->caps[k][1], c->caps[k][2], c->caps[k][3], c->caps[k][4], c->caps[k][5],
                   c->caps[k][6], c->caps[k][7], c->caps[k][8], c->caps[k][9], c->caps[k][10]);
        printf("\n");
    }
    printf("-- BE links --\n");
    for (int i = 0; i < tp->nl; i++) {
        printf("  link %u '%s' fmt %u hw_configs %u tuples:", tp->l[i].id, tp->l[i].name, tp->l[i].fmt, tp->l[i].nhw);
        tp_print_tuples(tp->l[i].tup, tp->l[i].ntup);
        printf("\n");
    }
}

// ---- volume table ([TP]:177-262 vol_shift_64 / vol_pow32 / vol_compute_gain, VOLUME_FWL 16 sof-audio.h:62)
static uint32_t vol_shift_64(uint64_t i, uint32_t x) {
    if (x > 32) x = 32;
    if (x == 0) return (uint32_t)i;
    return (uint32_t)(((i >> (x - 1)) + 1) >> 1);
}
static uint32_t vol_pow32(uint32_t a, int exp, uint32_t fwl) {
    uint32_t power = 1u << fwl;
    if (exp == 0) return power;
    int iter = exp < 0 ? -exp : exp;
    for (int i = 0; i < iter; i++) power = vol_shift_64((uint64_t)power * a, fwl);
    if (exp > 0) return power;
    return (uint32_t)(((uint64_t)1 << (fwl << 1)) / power);
}
static uint32_t vol_compute_gain(uint32_t value, const int *tlv) {   // tlv: min (dB), step (0.01 dB), mute
    if (value == 0 && tlv[2]) return 0;
    int dB = tlv[0] + (int)(value * (uint32_t)tlv[1]) / 100;
    uint32_t g = vol_pow32(73533, dB, 16);                    // VOL_TWENTIETH_ROOT_OF_TEN
    int f_step = tlv[1] - (tlv[1] / 100);
    if (f_step == 50 && (value & 1)) g = vol_shift_64((uint64_t)g * 69419, 16);   // VOL_FORTIETH_ROOT_OF_TEN
    return g;
}

// ---- IPC3 message plan
#define IPC_GLB_TPLG 0x30000000u
#define IPC_GLB_COMP 0x50000000u
#define IPC_GLB_STREAM 0x60000000u
#define IPC_GLB_DAI 0x80000000u
#define IPC_COMP_NEW 0x00010000u
#define IPC_COMP_FREE 0x00020000u
#define IPC_COMP_CONNECT 0x00030000u
#define IPC_PIPE_NEW 0x00100000u
#define IPC_PIPE_FREE 0x00110000u
#define IPC_PIPE_COMPLETE 0x00130000u
#define IPC_BUFFER_NEW 0x00200000u
#define IPC_BUFFER_FREE 0x00210000u
#define IPC_SET_VALUE 0x00010000u
#define IPC_SET_DATA 0x00030000u
#define IPC_DAI_CONFIG 0x00010000u
#define IPC_PCM_PARAMS 0x00010000u
#define IPC_PCM_FREE 0x00030000u
#define IPC_TRIG_START 0x00040000u
#define IPC_TRIG_STOP 0x00050000u
#define VOL_ZERO_DB 0x10000u          // BIT(VOLUME_FWL) [I3T] sof_ipc3_control_load_volume
#define DMA_CHAN_INVALID 0xFFFFFFFFu

struct ipcmsg { uint8_t b[384]; uint32_t len; char what[200]; };
struct m5dai { int w; uint32_t link, pdi, tplg_index, dai_index, alh_id, rate, ch, fmt, type; char wname[TP_NAME]; };
struct m5plan {
    int n; struct ipcmsg m[48];       // widget set-up, routes, pipeline complete ([SA2] sof_widget_list_setup)
    int nf; struct ipcmsg f[32];      // widget free ([SA2] sof_widget_list_free)
    struct ipcmsg pcm_params, pcm_free, trig_start, trig_stop, vol_mute, vol_0db;
    uint32_t host_comp, pga_comp, demux_comp; int host_w, pga_w;
    int ndai; struct m5dai dai[2];
    uint32_t vol_max, vol_min, vol_ch; int tlv[3];
    int order[32], norder;            // DFS pre-order (widget indices)
    char pcm_name[TP_NAME]; uint32_t pcm_id;
    int cap;                          // M7: 1 = microphone (capture) plan
};

static void put32(uint8_t *b, uint32_t o, uint32_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); b[o + 2] = (uint8_t)(v >> 16); b[o + 3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *b, uint32_t o, uint32_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }
static struct ipcmsg *pl_new(struct ipcmsg *m, uint32_t len, uint32_t cmd, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static struct ipcmsg *pl_new(struct ipcmsg *m, uint32_t len, uint32_t cmd, const char *fmt, ...) {
    memset(m, 0, sizeof *m);
    m->len = len; put32(m->b, 0, len); put32(m->b, 4, cmd);
    va_list ap; va_start(ap, fmt); vsnprintf(m->what, sizeof m->what, fmt, ap); va_end(ap);
    return m;
}

static const char *ipc_cmd_name(uint32_t cmd) {   // [HS]:43-122
    switch (cmd) {
    case IPC_GLB_TPLG | IPC_COMP_NEW: return "TPLG_COMP_NEW";
    case IPC_GLB_TPLG | IPC_COMP_FREE: return "TPLG_COMP_FREE";
    case IPC_GLB_TPLG | IPC_COMP_CONNECT: return "TPLG_COMP_CONNECT";
    case IPC_GLB_TPLG | IPC_PIPE_NEW: return "TPLG_PIPE_NEW";
    case IPC_GLB_TPLG | IPC_PIPE_FREE: return "TPLG_PIPE_FREE";
    case IPC_GLB_TPLG | IPC_PIPE_COMPLETE: return "TPLG_PIPE_COMPLETE";
    case IPC_GLB_TPLG | IPC_BUFFER_NEW: return "TPLG_BUFFER_NEW";
    case IPC_GLB_TPLG | IPC_BUFFER_FREE: return "TPLG_BUFFER_FREE";
    case IPC_GLB_COMP | IPC_SET_VALUE: return "COMP_SET_VALUE";
    case IPC_GLB_COMP | IPC_SET_DATA: return "COMP_SET_DATA";
    case IPC_GLB_DAI | IPC_DAI_CONFIG: return "DAI_CONFIG";
    case IPC_GLB_STREAM | IPC_PCM_PARAMS: return "STREAM_PCM_PARAMS";
    case IPC_GLB_STREAM | IPC_PCM_FREE: return "STREAM_PCM_FREE";
    case IPC_GLB_STREAM | IPC_TRIG_START: return "STREAM_TRIG_START";
    case IPC_GLB_STREAM | IPC_TRIG_STOP: return "STREAM_TRIG_STOP";
    case IPC_GLB_STREAM | 0x00090000u: return "STREAM_TRIG_XRUN";
    case IPC_GLB_STREAM | 0x000a0000u: return "STREAM_POSITION";
    case 0x10000000u: return "GLB_REPLY";
    case 0x70000000u: return "FW_READY";
    }
    return "?";
}

// sof_comp_alloc ([I3T]:367-399): header + optional 16-byte UUID extension at the end
static void pl_comp(struct ipcmsg *m, const struct tp_widget *w, uint32_t base, uint32_t type) {
    const struct tp_tuple *u = tp_find(w->tup, w->ntup, 405);   // SOF_TKN_COMP_UUID
    int has_uuid = 0;
    if (u) for (int k = 0; k < 16; k++) if (u->uuid[k]) has_uuid = 1;
    uint32_t total = base + (has_uuid ? 16 : 0);
    m->len = total; put32(m->b, 0, total); put32(m->b, 4, IPC_GLB_TPLG | IPC_COMP_NEW);
    put32(m->b, 8, w->comp_id); put32(m->b, 12, type); put32(m->b, 16, w->index); put32(m->b, 20, tp_u32(w, 404, 0));
    if (has_uuid) { put32(m->b, 24, 16); memcpy(m->b + base, u->uuid, 16); }
}
// sof_ipc_comp_config at +28 from SOF_COMP_TOKENS ([I3T]:163-171): periods_sink, periods_source, frame_fmt
static void pl_config(struct ipcmsg *m, const struct tp_widget *w) {
    put32(m->b, 28, 36);
    put32(m->b, 36, tp_u32(w, 400, 0)); put32(m->b, 40, tp_u32(w, 401, 0));
    put32(m->b, 48, tp_fmt(tp_s(w, 402)));
}

// cap = 0: the speaker PCM ('Speaker', playback): the walk starts at its host (AIF_IN) widget.
// cap = 1 (M7): the microphone PCM ('Microphone', capture): Linux starts the walk at the DAI_OUT widget whose path
// reaches the PCM's host (AIF_OUT) widget (sof_walk_widgets_in_order, sof-audio.c:684-692) and sets the widgets up
// source to sink (sof_set_up_widgets_in_path, sof-audio.c:599-667).
static int pl_build_dir(const struct tplg *tp, struct m5plan *pl, int verbose, int cap) {
    memset(pl, 0, sizeof *pl);
    pl->host_w = pl->pga_w = -1;
    pl->cap = cap;
    const char *pname = cap ? "Microphone" : "Speaker";
    // 1. the PCM and its host widget ([SP] / [TP] spcm, stream name match)
    const struct tp_pcm *pc = NULL;
    for (int i = 0; i < tp->np; i++) if ((cap ? tp->p[i].cap : tp->p[i].pb) && !strcmp(tp->p[i].name, pname)) pc = &tp->p[i];
    if (!pc) { printf("  !! no %s PCM named '%s' in the topology\n", cap ? "capture" : "playback", pname); return -1; }
    snprintf(pl->pcm_name, sizeof pl->pcm_name, "%s", pc->name); pl->pcm_id = pc->pcm_id;
    int hw = -1;
    for (int i = 0; i < tp->nw; i++)
        if (tp->w[i].id == (cap ? W_AIF_OUT : W_AIF_IN) && !strcmp(tp->w[i].sname, pc->sname[cap ? 1 : 0])) hw = i;
    if (hw < 0) { printf("  !! no %s widget with stream name '%s'\n", cap ? "AIF_OUT" : "AIF_IN", pc->sname[cap ? 1 : 0]); return -1; }
    pl->host_w = hw; pl->host_comp = tp->w[hw].comp_id;
    int start = hw;
    if (cap) {   // the DAI_OUT widget the host widget is reachable from
        start = -1;
        for (int d = 0; d < tp->nw && start < 0; d++) {
            if (tp->w[d].id != W_DAI_OUT) continue;
            int st[96], n = 0, vis[96] = {0};
            st[n++] = d;
            while (n) {
                int x = st[--n];
                if (vis[x]) continue;
                vis[x] = 1;
                if (x == hw) { start = d; break; }
                for (int r = 0; r < tp->nr; r++) {
                    if (strcmp(tp->r[r].src, tp->w[x].name)) continue;
                    int s2 = tp_widget_by_name(tp, tp->r[r].sink);
                    if (s2 >= 0 && !vis[s2] && n < 96) st[n++] = s2;
                }
            }
        }
        if (start < 0) { printf("  !! no DAI_OUT widget reaches %s\n", tp->w[hw].name); return -1; }
    }
    // 2. DFS pre-order from the host widget. DAPM keeps a widget's out-edges in reverse insertion order
    //    (list_add in dapm_add_path, [SDP]:693-696), so the route added last is walked first.
    int stack[32], sp = 0, seen[96] = {0};
    stack[sp++] = start;
    while (sp && pl->norder < 32) {
        int wi = stack[--sp];
        if (seen[wi]) continue;
        seen[wi] = 1;
        pl->order[pl->norder++] = wi;
        // push sinks so that the first edge in DAPM order is popped first: push in insertion order
        for (int r = 0; r < tp->nr; r++) {
            if (strcmp(tp->r[r].src, tp->w[wi].name)) continue;
            int s = tp_widget_by_name(tp, tp->r[r].sink);
            if (s >= 0 && !seen[s] && sp < 32) stack[sp++] = s;
        }
    }
    // 3. per-widget IPC ([SA2]:144-248 sof_widget_setup_unlocked + [I3T]:2288-2327 sof_ipc3_widget_setup)
    int pipe_up[96] = {0}, pipe_use[96] = {0};
    for (int oi = 0; oi < pl->norder; oi++) {
        const struct tp_widget *w = &tp->w[pl->order[oi]];
        int sch = tp_sched_of(tp, w->index);
        if (sch < 0) { printf("  !! no scheduler widget for pipeline %u (%s)\n", w->index, w->name); return -1; }
        const struct tp_widget *s = &tp->w[sch];
        if (!pipe_up[sch]) {   // dynamic pipeline: the scheduler widget is set up first ([SA2]:170-180)
            int sched = tp_widget_by_name(tp, s->sname);
            if (sched < 0) { printf("  !! scheduler %s refers to non existent widget %s ([I3T]:546-551)\n", s->name, s->sname); return -1; }
            if (tp_u32(s, 206, 0) != 1) printf("  note: %s is not a dynamic pipeline (token 206)\n", s->name);
            struct ipcmsg *m = pl_new(&pl->m[pl->n++], 48, IPC_GLB_TPLG | IPC_PIPE_NEW,
                "PIPE_NEW %s: comp %u pipeline %u sched comp %u (%s) core %u period %u us prio %u mips %u frames %u time_domain %u  [I3T]:524-597",
                s->name, s->comp_id, s->index, tp->w[sched].comp_id, s->sname, tp_u32(s, 203, 0), tp_u32(s, 200, 0), tp_u32(s, 201, 0),
                tp_u32(s, 202, 0), tp_u32(s, 204, 0), tp_u32(s, 205, 0));
            put32(m->b, 8, s->comp_id); put32(m->b, 12, s->index); put32(m->b, 16, tp->w[sched].comp_id);
            put32(m->b, 20, tp_u32(s, 203, 0)); put32(m->b, 24, tp_u32(s, 200, 0)); put32(m->b, 28, tp_u32(s, 201, 0));
            put32(m->b, 32, tp_u32(s, 202, 0)); put32(m->b, 36, tp_u32(s, 204, 0)); put32(m->b, 44, tp_u32(s, 205, 0));
            pipe_up[sch] = 1;
        }
        pipe_use[sch]++;
        struct ipcmsg *m = &pl->m[pl->n];
        memset(m, 0, sizeof *m);
        switch (w->id) {
        case W_AIF_IN: case W_AIF_OUT:   // sof_ipc3_widget_setup_comp_host [I3T]:408-450
            pl_comp(m, w, 76, 1);  pl_config(m, w);
            put32(m->b, 64, w->id == W_AIF_OUT ? 1 : 0); put32(m->b, 72, tp_u32(w, 353, 0));
            snprintf(m->what, sizeof m->what, "COMP_NEW %s: host comp %u pipeline %u dir %s periods sink %u src %u fmt %s  [I3T]:408-450",
                     w->name, w->comp_id, w->index, w->id == W_AIF_OUT ? "capture" : "playback", tp_u32(w, 400, 0), tp_u32(w, 401, 0),
                     fmt_name(tp_fmt(tp_s(w, 402))));
            pl->n++;
            break;
        case W_BUFFER:   // sof_ipc3_widget_setup_comp_buffer [I3T]:599-632
            m->len = 44; put32(m->b, 0, 44); put32(m->b, 4, IPC_GLB_TPLG | IPC_BUFFER_NEW);
            put32(m->b, 8, w->comp_id); put32(m->b, 12, 12); put32(m->b, 16, w->index); put32(m->b, 20, tp_u32(w, 404, 0));
            put32(m->b, 28, tp_u32(w, 100, 0)); put32(m->b, 32, tp_u32(w, 101, 0)); put32(m->b, 36, tp_u32(w, 102, 0));
            snprintf(m->what, sizeof m->what, "BUFFER_NEW %s: comp %u pipeline %u size %u caps 0x%x flags 0x%x  [I3T]:599-632",
                     w->name, w->comp_id, w->index, tp_u32(w, 100, 0), tp_u32(w, 101, 0), tp_u32(w, 102, 0));
            pl->n++;
            break;
        case W_PGA: {   // sof_ipc3_widget_setup_comp_pga [I3T]:757-811 (+ volume table [TP] / [I3C]:740-755)
            pl_comp(m, w, 84, 5); pl_config(m, w);
            const struct tp_kctl *kc = NULL;
            for (int k = 0; k < w->nk; k++) if (w->k[k].type == 1 && w->k[k].max > 1) kc = &w->k[k];
            uint32_t ch = 0, vmin = 0, vmax = 0;
            if (kc && kc->tlv_type == 1) {   // DB_SCALE: [TP]:149-168 get_tlv_data (min/100, step, mute)
                pl->tlv[0] = kc->tlv_min / 100; pl->tlv[1] = kc->tlv_step & 0xFFFF; pl->tlv[2] = kc->tlv_mute ? 1 : 0;
                vmin = vol_compute_gain((uint32_t)kc->min, pl->tlv); vmax = vol_compute_gain((uint32_t)kc->max, pl->tlv); ch = (uint32_t)kc->nch;
            }
            put32(m->b, 64, ch); put32(m->b, 68, vmin); put32(m->b, 72, vmax);
            put32(m->b, 76, tp_u32(w, 250, 0)); put32(m->b, 80, tp_u32(w, 251, 0));
            snprintf(m->what, sizeof m->what, "COMP_NEW %s: volume comp %u ch %u min 0x%x max 0x%x ramp %u initial_ramp %u ms fmt %s  [I3T]:757-811",
                     w->name, w->comp_id, ch, vmin, vmax, tp_u32(w, 250, 0), tp_u32(w, 251, 0), fmt_name(tp_fmt(tp_s(w, 402))));
            pl->n++;
            pl->pga_w = pl->order[oi]; pl->pga_comp = w->comp_id; pl->vol_min = vmin; pl->vol_max = vmax; pl->vol_ch = ch;
            if (kc && pl->n + 2 > 46) { printf("  !! too many messages\n"); return -1; }
            if (kc) {   // sof_ipc3_widget_kcontrol_setup [I3C]:703-738 -> SET_VALUE with the cached value (0 dB, [I3T]:1866-1896)
                struct ipcmsg *v = pl_new(&pl->m[pl->n++], 92 + 8 * ch, IPC_GLB_COMP | IPC_SET_VALUE,
                    "COMP_SET_VALUE %s: '%s' %u ch = VOL_ZERO_DB 0x%x (kcontrol restore)  [I3C]:14-120, 703-738", w->name, kc->name, ch, VOL_ZERO_DB);
                put32(v->b, 12, w->comp_id); put32(v->b, 16, 1); put32(v->b, 20, 0); put32(v->b, 24, 0); put32(v->b, 56, ch);
                for (uint32_t c = 0; c < ch; c++) { put32(v->b, 92 + 8 * c, c); put32(v->b, 96 + 8 * c, VOL_ZERO_DB); }
                // the same message with other values is used for the fade-out (mute) and 0 dB
                pl->vol_0db = *v;
                pl->vol_mute = *v;
                for (uint32_t c = 0; c < ch; c++) put32(pl->vol_mute.b, 96 + 8 * c, 0);
                snprintf(pl->vol_mute.what, sizeof pl->vol_mute.what, "COMP_SET_VALUE %s: volume 0 (mute, ramped) before stop (M5 fade-out, not Linux)", w->name);
            }
            // M7: a switch kcontrol on the same PGA ('5 Master Capture Switch', max 1). Linux loads it with cmd SWITCH
            // and value 0 ([I3T] sof_ipc3_control_load_volume) and restores that at widget set-up; alsactl / UCM then
            // turn it on. We send the "on" state right away (SOF volume.c: value != 0 = unmute). Playback PGAs of this
            // topology have no switch, so the speaker plan is unchanged.
            for (int k = 0; k < w->nk; k++) {
                const struct tp_kctl *sw = &w->k[k];
                if (sw->type != 1 || sw->max != 1) continue;
                uint32_t sch = (uint32_t)sw->nch;
                if (sch < 1 || sch > 8 || pl->n > 46) { printf("  !! switch '%s' unusable\n", sw->name); return -1; }
                struct ipcmsg *v = pl_new(&pl->m[pl->n++], 92 + 8 * sch, IPC_GLB_COMP | IPC_SET_VALUE,
                    "COMP_SET_VALUE %s: '%s' %u ch = 1 (SWITCH on; Linux restores 0, alsactl/UCM then set 1)  [I3C]:14-120", w->name, sw->name, sch);
                put32(v->b, 12, w->comp_id); put32(v->b, 16, 1); put32(v->b, 20, 2); put32(v->b, 24, 0); put32(v->b, 56, sch);
                for (uint32_t c = 0; c < sch; c++) { put32(v->b, 92 + 8 * c, c); put32(v->b, 96 + 8 * c, 1); }
            }
            break;
        }
        case W_EFFECT: {   // sof_widget_update_ipc_comp_process + sof_process_load [I3T]:895-1012
            const char *pt = tp_s(w, 900);
            // [I3T]:40-50 sof_process[] -> enum sof_comp_type (SOF topology.h): MUX 7, EQ_IIR 13, EQ_FIR 14, DEMUX 18
            uint32_t ctype = !pt ? 0 : !strcmp(pt, "DEMUX") ? 18 : !strcmp(pt, "MUX") ? 7 : !strcmp(pt, "EQIIR") ? 13 :
                             !strcmp(pt, "EQFIR") ? 14 : 0;
            if (!ctype) { printf("  !! effect %s of type '%s' is not supported by M5\n", w->name, pt ? pt : "?"); return -1; }
            const struct tp_kctl *kc = NULL;
            for (int k = 0; k < w->nk; k++) if (w->k[k].type == 2) kc = &w->k[k];
            uint32_t dsz = 0;
            if (kc) {   // sof_ipc3_control_load_bytes [I3T]:1803-1864: priv = sof_abi_hdr + data
                if (kc->privsz < 32 || le32(kc->priv) != 0x00464F53u || le32(kc->priv + 8) + 32 != kc->privsz) {
                    printf("  !! bytes control '%s' has a bad SOF ABI header\n", kc->name); return -1;
                }
                dsz = le32(kc->priv + 8);
            }
            pl_comp(m, w, 100 + dsz, ctype); pl_config(m, w);
            put32(m->b, 64, dsz);   // process->size; process->type stays 0 (only config is parsed into it, [I3T]:925-957)
            if (dsz) memcpy(m->b + 100, kc->priv + 32, dsz);
            snprintf(m->what, sizeof m->what, "COMP_NEW %s: %s (process) comp %u with %u bytes of '%s' config  [I3T]:895-976",
                     w->name, pt, w->comp_id, dsz, kc ? kc->name : "-");
            pl->n++;
            pl->demux_comp = w->comp_id;
            if (kc) {   // kcontrol restore: SET_DATA with sof_abi_hdr + data ([I3C]:56-90)
                struct ipcmsg *v = pl_new(&pl->m[pl->n++], 92 + kc->privsz, IPC_GLB_COMP | IPC_SET_DATA,
                    "COMP_SET_DATA %s: '%s' %u bytes (abi 0x%08x)  [I3C]:14-120, 703-738", w->name, kc->name, dsz, le32(kc->priv + 12));
                put32(v->b, 12, w->comp_id); put32(v->b, 16, 5); put32(v->b, 20, 3); put32(v->b, 24, 0); put32(v->b, 56, dsz);
                memcpy(v->b + 92, kc->priv, kc->privsz);
            }
            break;
        }
        case W_DAI_IN: case W_DAI_OUT: {   // sof_ipc3_widget_setup_comp_dai [I3T]:1600-1752
            const char *dt = tp_s(w, 154);
            uint32_t idx = tp_u32(w, 155, 0);
            if (!dt || strcmp(dt, "ALH") || idx < 2) { printf("  !! DAI widget %s is not an ALH pin >= 2\n", w->name); return -1; }
            if (pl->ndai >= 2) { printf("  !! more than two DAI widgets on the speaker path\n"); return -1; }
            pl_comp(m, w, 80, 2); pl_config(m, w);
            put32(m->b, 64, tp_u32(w, 156, 0)); put32(m->b, 68, idx - 2); put32(m->b, 72, 4);   // INTEL_ALH_DAI_INDEX_BASE 2
            snprintf(m->what, sizeof m->what, "COMP_NEW %s: dai comp %u pipeline %u ALH dai_index %u (tplg %u - 2) dir %u periods src %u fmt %s  [I3T]:1600-1660",
                     w->name, w->comp_id, w->index, idx - 2, idx, tp_u32(w, 156, 0), tp_u32(w, 401, 0), fmt_name(tp_fmt(tp_s(w, 402))));
            pl->n++;
            // BE link with the DAI's stream name ([I3T]:1662-1714 + sof_link_alh_load 1574-1598)
            const struct tp_link *lk = NULL;
            for (int i = 0; i < tp->nl; i++) if (!strcmp(tp->l[i].name, w->sname)) lk = &tp->l[i];
            if (!lk) { printf("  !! no BE link named '%s' for %s\n", w->sname, w->name); return -1; }
            const struct tp_tuple *lr = tp_find(lk->tup, lk->ntup, 1400), *lc = tp_find(lk->tup, lk->ntup, 1401);
            struct m5dai *dd = &pl->dai[pl->ndai++];
            dd->w = pl->order[oi]; snprintf(dd->wname, sizeof dd->wname, "%s", w->name);
            dd->tplg_index = idx; dd->link = idx >> 8; dd->pdi = idx & 0xFF;
            // sdw_params_stream: dai_index = link_id << 8 | cpu dai id (= PDI) ([SDH]:61-78); minus base 2
            dd->dai_index = ((dd->link << 8) | dd->pdi) - 2;
            // intel_pdi_alh_configure: ALH id = link * 16 + pdi + 3 (+2 if pdi >= 2) ([SIC]:656-675)
            dd->alh_id = dd->link * 16 + dd->pdi + 3 + (dd->pdi >= 2 ? 2 : 0);
            dd->rate = lr ? lr->val : 0; dd->ch = lc ? lc->val : 0; dd->fmt = lk->fmt; dd->type = 4;
            struct ipcmsg *c = pl_new(&pl->m[pl->n++], 216, IPC_GLB_DAI | IPC_DAI_CONFIG,
                "DAI_CONFIG %s: ALH dai_index 0x%x (link %u PDI %u) flags HW_PARAMS, rate %u ch %u stream_id %u  [I3T]:2186-2286, [SDH]:61-78",
                w->name, dd->dai_index, dd->link, dd->pdi, dd->rate, dd->ch, dd->alh_id);
            put32(c->b, 8, 4); put32(c->b, 12, dd->dai_index); put16(c->b, 16, dd->fmt); c->b[19] = 1;   // SOF_DAI_CONFIG_FLAGS_HW_PARAMS
            put32(c->b, 56, dd->alh_id); put32(c->b, 60, dd->rate); put32(c->b, 64, dd->ch);
            break;
        }
        default:
            printf("  !! widget %s of type %s is not supported on the M5 path\n", w->name, wid_name(w->id));
            return -1;
        }
        if (pl->n > 40) { printf("  !! too many messages\n"); return -1; }
    }
    // 4. connections ([SA2]:328-431 sof_setup_pipeline_connections -> [I3T]:1780-1801): widget list order,
    //    out-edges in DAPM order (last added first)
    int inlist[96] = {0};
    for (int oi = 0; oi < pl->norder; oi++) inlist[pl->order[oi]] = 1;
    for (int oi = 0; oi < pl->norder; oi++) {
        const struct tp_widget *w = &tp->w[pl->order[oi]];
        for (int r = tp->nr - 1; r >= 0; r--) {
            if (strcmp(tp->r[r].src, w->name)) continue;
            int s = tp_widget_by_name(tp, tp->r[r].sink);
            if (s < 0 || !inlist[s]) continue;
            struct ipcmsg *m = pl_new(&pl->m[pl->n++], 16, IPC_GLB_TPLG | IPC_COMP_CONNECT,
                                      "COMP_CONNECT %s (%u) -> %s (%u)  [I3T]:1780-1801", w->name, w->comp_id, tp->w[s].name, tp->w[s].comp_id);
            put32(m->b, 8, w->comp_id); put32(m->b, 12, tp->w[s].comp_id);
        }
    }
    // 5. pipeline complete, in widget list order ([SA2]:787-823 -> [I3T]:2131-2149)
    int done[96] = {0};
    for (int oi = 0; oi < pl->norder; oi++) {
        int sch = tp_sched_of(tp, tp->w[pl->order[oi]].index);
        if (done[sch]) continue;
        done[sch] = 1;
        struct ipcmsg *m = pl_new(&pl->m[pl->n++], 12, IPC_GLB_TPLG | IPC_PIPE_COMPLETE,
                                  "PIPE_COMPLETE %s (comp %u)  [I3T]:2131-2149", tp->w[sch].name, tp->w[sch].comp_id);
        put32(m->b, 8, tp->w[sch].comp_id);
    }
    // 6. free ([SA2]:549-597 sof_free_widgets_in_path, 59-135 sof_widget_free_unlocked, [I3T]:2151-2184)
    for (int oi = 0; oi < pl->norder; oi++) {
        const struct tp_widget *w = &tp->w[pl->order[oi]];
        int sch = tp_sched_of(tp, w->index);
        if (w->id == W_DAI_IN || w->id == W_DAI_OUT) {
            const struct m5dai *dd = NULL;
            for (int k = 0; k < pl->ndai; k++) if (pl->dai[k].w == pl->order[oi]) dd = &pl->dai[k];
            struct ipcmsg *c = pl_new(&pl->f[pl->nf++], 216, IPC_GLB_DAI | IPC_DAI_CONFIG,
                "DAI_CONFIG %s: ALH dai_index 0x%x flags HW_FREE stream_id DMA_CHAN_INVALID  [SA2]:80-92, [I3T]:2186-2286", w->name, dd->dai_index);
            put32(c->b, 8, 4); put32(c->b, 12, dd->dai_index); put16(c->b, 16, dd->fmt); c->b[19] = 2;   // SOF_DAI_CONFIG_FLAGS_HW_FREE
            put32(c->b, 56, DMA_CHAN_INVALID); put32(c->b, 60, dd->rate); put32(c->b, 64, dd->ch);
        }
        uint32_t fc = w->id == W_BUFFER ? IPC_BUFFER_FREE : IPC_COMP_FREE;
        struct ipcmsg *m = pl_new(&pl->f[pl->nf++], 12, IPC_GLB_TPLG | fc, "%s %s (comp %u)  [I3T]:2151-2184",
                                  fc == IPC_BUFFER_FREE ? "BUFFER_FREE" : "COMP_FREE", w->name, w->comp_id);
        put32(m->b, 8, w->comp_id);
        if (--pipe_use[sch] == 0) {
            struct ipcmsg *p = pl_new(&pl->f[pl->nf++], 12, IPC_GLB_TPLG | IPC_PIPE_FREE,
                "PIPE_FREE %s (comp %u): last widget of the pipeline freed  [SA2]:120-130", tp->w[sch].name, tp->w[sch].comp_id);
            put32(p->b, 8, tp->w[sch].comp_id);
        }
        if (pl->nf > 30) { printf("  !! too many free messages\n"); return -1; }
    }
    // 7. stream messages ([I3P]:16-160)
    struct ipcmsg *m = pl_new(&pl->pcm_params, 108, IPC_GLB_STREAM | IPC_PCM_PARAMS,
        "STREAM_PCM_PARAMS host comp %u: %s, 48000 Hz, 2 ch, S32_LE (4/4 bytes), interleaved, buffer 65536 B / 16 pages, period 4096 B, "
        "stream tag 1, no_stream_position 1 (page table address patched by the kext)  [I3P]:39-143", pl->host_comp, cap ? "capture" : "playback");
    put32(m->b, 8, pl->host_comp);
    put32(m->b, 24, 84);                       // params.hdr.size = sizeof(pcm.params)
    put32(m->b, 36, 16); put32(m->b, 40, 65536);   // buffer.pages = PFN_UP(dma_bytes), buffer.size
    put32(m->b, 56, cap ? 1 : 0);              // direction: SOF_IPC_STREAM_PLAYBACK 0 / CAPTURE 1 (stream.h)
    put32(m->b, 60, 2);                        // SOF_IPC_FRAME_S32_LE
    put32(m->b, 64, 0);                        // SOF_IPC_BUFFER_INTERLEAVED
    put32(m->b, 68, 48000);
    put16(m->b, 72, 1); put16(m->b, 74, 2); put16(m->b, 76, 4); put16(m->b, 78, 4);   // tag, channels, valid, container bytes
    put32(m->b, 80, 4096);                     // host_period_bytes
    put16(m->b, 84, 1);                        // no_stream_position (hda no_ipc_position, ABI >= 3.10)
    m = pl_new(&pl->pcm_free, 12, IPC_GLB_STREAM | IPC_PCM_FREE, "STREAM_PCM_FREE host comp %u  [I3P]:16-37", pl->host_comp);
    put32(m->b, 8, pl->host_comp);
    m = pl_new(&pl->trig_start, 12, IPC_GLB_STREAM | IPC_TRIG_START, "STREAM_TRIG_START host comp %u  [I3P]:145-180", pl->host_comp);
    put32(m->b, 8, pl->host_comp);
    m = pl_new(&pl->trig_stop, 12, IPC_GLB_STREAM | IPC_TRIG_STOP, "STREAM_TRIG_STOP host comp %u  [I3P]:145-180", pl->host_comp);
    put32(m->b, 8, pl->host_comp);
    if (cap) {   // M7 sanity: exactly the RT714 pin (SDW0 Pin2: link 0, PDI 2), 48 kHz, 2 ch, a PGA on the path
        if (pl->ndai != 1 || pl->dai[0].link != 0 || pl->dai[0].pdi != 2 || pl->dai[0].rate != 48000 || pl->dai[0].ch != 2) {
            printf("  !! microphone path: expected 1 ALH DAI on link 0 PDI 2 at 48 kHz / 2 ch (found %d)\n", pl->ndai);
            return -1;
        }
        if (pl->pga_w < 0) { printf("  !! no PGA on the microphone path\n"); return -1; }
        if (verbose) {
            printf("  microphone path (DFS from '%s' to '%s', DAPM edge order):", tp->w[start].name, tp->w[hw].name);
            for (int oi = 0; oi < pl->norder; oi++) printf(" %s%s", oi ? "-> " : "", tp->w[pl->order[oi]].name);
            printf("\n");
        }
        return 0;
    }
    // sanity: exactly the two RT1316 ALH pins
    if (pl->ndai != 2) { printf("  !! expected 2 ALH DAIs on the speaker path, found %d\n", pl->ndai); return -1; }
    for (int k = 0; k < 2; k++) {
        const struct m5dai *dd = &pl->dai[k];
        if ((dd->link != 1 && dd->link != 2) || dd->pdi != 2 || dd->rate != 48000 || dd->ch != 2) {
            printf("  !! DAI %s: link %u PDI %u rate %u ch %u (M5 supports links 1/2, PDI 2, 48 kHz, 2 ch)\n", dd->wname, dd->link,
                   dd->pdi, dd->rate, dd->ch);
            return -1;
        }
    }
    if (pl->pga_w < 0) { printf("  !! no PGA on the speaker path\n"); return -1; }
    if (verbose) {
        printf("  speaker path (DFS from '%s', DAPM edge order):", tp->w[hw].name);
        for (int oi = 0; oi < pl->norder; oi++) printf(" %s%s", oi ? "-> " : "", tp->w[pl->order[oi]].name);
        printf("\n");
    }
    return 0;
}

static int pl_build(const struct tplg *tp, struct m5plan *pl, int verbose) { return pl_build_dir(tp, pl, verbose, 0); }

static void pl_hex(const struct ipcmsg *m, const char *pfx) {
    for (uint32_t i = 0; i < m->len; i += 4) {
        if (i % 32 == 0) printf("%s%s%03x:", i ? "\n" : "", pfx, i);
        printf(" %08x", le32(m->b + i));
    }
    printf("\n");
}
static void pl_print(const struct m5plan *pl, int hex) {
    printf("  IPC3 set-up sequence (FE prepare: sof_widget_list_setup [SA2]:760-838, then PCM params [SP]:315-375):\n");
    for (int i = 0; i < pl->n; i++) {
        printf("   %2d %-18s %3u B  %s\n", i + 1, ipc_cmd_name(le32(pl->m[i].b + 4)), pl->m[i].len, pl->m[i].what);
        if (hex) pl_hex(&pl->m[i], "        ");
    }
    printf("   %2d %-18s %3u B  %s\n", pl->n + 1, "STREAM_PCM_PARAMS", pl->pcm_params.len, pl->pcm_params.what);
    if (hex) pl_hex(&pl->pcm_params, "        ");
    printf("  start: %s; stop: %s, then %s\n", pl->trig_start.what, pl->trig_stop.what, pl->pcm_free.what);
    printf("  IPC3 free sequence (FE hw_free: sof_widget_list_free [SA2]:840-861):\n");
    for (int i = 0; i < pl->nf; i++) {
        printf("   %2d %-18s %3u B  %s\n", i + 1, ipc_cmd_name(le32(pl->f[i].b + 4)), pl->f[i].len, pl->f[i].what);
        if (hex) pl_hex(&pl->f[i], "        ");
    }
}

// ------------------------------------------------------------------ m5: run time
// Linux references (besides the topology ones above):
//   [ST2] drivers/soundwire/stream.c   [GBA] drivers/soundwire/generic_bandwidth_allocation.c
//   [CM]  drivers/soundwire/cadence_master.c   [IS] drivers/soundwire/intel.c   [IB] intel_bus_common.c
//   [BU]  drivers/soundwire/bus.c   [SR] include/linux/soundwire/sdw_registers.h   [SI] sdw_intel.h
//   [RT]  sound/soc/codecs/rt1316-sdw.c (+ rt1316-sdw.h)   [SU] sound/soc/sdw_utils/soc_sdw_utils.c
//   [UCM] alsa-ucm-conf ucm2/sof-soundwire/rt1316.conf     [HST] sound/soc/sof/intel/hda-stream.c
//   [HP]  sound/soc/sof/intel/hda-pcm.c   [CN] sound/soc/sof/intel/cnl.c   [SPC] sound/soc/soc-pcm.c
#define MAGIC_M5 0x4D35474FULL   /* 'M5GO' */
enum { M5_STATE = 43, M5_IPC = 44, M5_SALLOC = 45, M5_STREAM = 46, M5_REGW = 47, M5_XFER = 48, M5_BANK = 49 };

// SDW_SDCA_CTL(fun, ent, ctl, ch) [SR]:335-342
static uint32_t sdca(uint32_t fun, uint32_t ent, uint32_t ctl, uint32_t ch) {
    return (1u << 30) | ((fun & 7) << 22) | ((ent & 0x40) << 15) | ((ent & 0x3F) << 7) | ((ctl & 0x30) << 15) |
           ((ctl & 0xF) << 3) | ((ch & 0x38) << 12) | (ch & 7);
}
// rt1316-sdw.h: FUNC_NUM_SMART_AMP 4; PDE23 0x31 PDE27 0x32 PDE22 0x33 XU24 0x24 FU21 0x03 UDMPU21 0x02
#define RT_PDE23 sdca(4, 0x31, 0x01, 0)
#define RT_PDE27 sdca(4, 0x32, 0x01, 0)
#define RT_PDE22 sdca(4, 0x33, 0x01, 0)
#define RT_XU24_BYPASS sdca(4, 0x24, 0x01, 0)
#define RT_FU21_MUTE_L sdca(4, 0x03, 0x01, 1)
#define RT_FU21_MUTE_R sdca(4, 0x03, 0x01, 2)
#define RT_UDMPU21_CLUSTER sdca(4, 0x02, 0x10, 0)
#define RT_ACT_PS(ent) sdca(4, ent, 0x10, 0)   /* SDCA PDE ACTUAL_PS (not used by the Linux driver; read-only check) */

// [RT]:71-129 rt1316_blind_write (applied after the 0xc000 = 0x02 software reset, [RT]:287-290)
static const struct { uint32_t reg; uint8_t val; } rt1316_blind[] = {
    {0xc710, 0x17}, {0xc711, 0x80}, {0xc712, 0x26}, {0xc713, 0x06}, {0xc714, 0x80}, {0xc715, 0x06}, {0xc702, 0x0a},
    {0xc703, 0x0a}, {0xc001, 0x45}, {0xc003, 0x00}, {0xc004, 0x11}, {0xc005, 0x00}, {0xc006, 0x00}, {0xc106, 0x00},
    {0xc007, 0x11}, {0xc008, 0x11}, {0xc009, 0x00},
    {0x2f0a, 0x00}, {0xd101, 0xf0}, {0xd103, 0x9b}, {0x2f36, 0x8e}, {0x3206, 0x80}, {0x3211, 0x0b}, {0x3216, 0x06},
    {0xc614, 0x20}, {0xc615, 0x0a}, {0xc616, 0x02}, {0xc617, 0x00}, {0xc60b, 0x10}, {0xc60e, 0x05}, {0xc102, 0x00},
    {0xc090, 0xb0}, {0xc00f, 0x01}, {0xc09c, 0x7b},
    {0xc602, 0x07}, {0xc603, 0x07}, {0xc0a3, 0x71}, {0xc00b, 0x30}, {0xc093, 0x80}, {0xc09d, 0x80}, {0xc0b0, 0x77},
    {0xc010, 0xa5}, {0xc050, 0x83}, {0x2f55, 0x03}, {0x3217, 0xb5}, {0x3202, 0x02},
    {0x41001208 /* SDW_SDCA_CTL(SMART_AMP, XU24, BYPASS, 0) */, 0x00},
    {0x2232, 0x80}, {0xc0b0, 0x77}, {0xc011, 0x00}, {0xc020, 0x00}, {0xc023, 0x00}, {0x3101, 0x00}, {0x3004, 0xa0},
    {0x3005, 0xb1}, {0xc007, 0x11}, {0xc008, 0x11}, {0xc009, 0x00}, {0xc022, 0xd6}, {0xc025, 0xd6},
    {0xd001, 0x03}, {0xd002, 0xbf}, {0xd003, 0x03}, {0xd004, 0xbf},
};

struct m5st {   // what has been done (for the graceful unwind on failure)
    int amp_init, ucm_on, ports, dma, sprep, bank_prep, pde_on, ipc_setup, params, enabled, started, ipc_n;
    int xruns, notes, cur_bank, nbank;
    uint32_t posn_offset;
};

static const char *xr_name(uint64_t r) { static const char *n[] = {"ok", "IGNORED (no ACK)", "NACK", "TIMEOUT"}; return r < 4 ? n[r] : "?"; }

// One RT1316 access with do_transfer's retry rule (err_threshold 16, [BU]:234-250); IGNORED is an error here
// (regmap would return -ENODATA). Prints one line per access.
static int rt_xfer(int link, int cmd, uint32_t addr, int cnt, uint32_t data, uint8_t *buf, const char *what, int verbose) {
    uint64_t o[16];
    for (int i = 0; i <= 16; i++) {
        uint64_t a0 = (uint64_t)link | ((uint64_t)cnt << 8) | ((uint64_t)cmd << 16) | (1ULL << 24) | ((uint64_t)(data & 0xFF) << 32);
        if (m2call(M5_XFER, a0, addr, o)) return -1;
        if (o[0] != M2R_OK) { printf("    !! link %d %s 0x%08x refused by the kext (%s)\n", link, cmd == 3 ? "write" : "read", addr, m2rc(o[0])); return -1; }
        if (o[1] == 0) {
            if (buf) for (int k = 0; k < cnt; k++) buf[k] = (uint8_t)((o[3 + k] >> 8) & 0xFF);
            if (verbose) {
                if (cmd == 3) printf("    L%d W %08x = %02x  %-44s ok (%llu us%s)\n", link, addr, data & 0xFF, what, o[14],
                                     (o[10] >> 32) ? ", paged" : "");
                else {
                    printf("    L%d R %08x x%d =", link, addr, cnt);
                    for (int k = 0; k < cnt; k++) printf(" %02llx", (o[3 + k] >> 8) & 0xFF);
                    printf("  %-40s ok (%llu us%s)\n", what, o[14], (o[10] >> 32) ? ", paged" : "");
                }
            }
            return 0;
        }
        printf("    L%d %s %08x: %s after %llu us, responses %llu (%08llx %08llx) page resp %016llx INTSTAT %08llx FIFOSTAT %08llx  [%s]\n",
               link, cmd == 3 ? "W" : "R", addr, xr_name(o[1]), o[14], o[2], o[3], o[4], o[9], o[13], o[15], what);
        if (o[1] == 1) return -1;   // IGNORED: no retry ([BU]:242-244)
        printf("    retry %d/16 (do_transfer [BU]:240-247)\n", i + 1);
    }
    return -1;
}
static int rt_w(int link, uint32_t addr, uint32_t v, const char *what) { return rt_xfer(link, 3, addr, 1, v, NULL, what, 1); }
static int rt_r(int link, uint32_t addr, uint8_t *v, const char *what) { return rt_xfer(link, 2, addr, 1, 0, v, what, 1); }
// sdw_update_no_pm = read + write ((v & ~mask) | val) ([BU] sdw_update_no_pm)
static int rt_upd(int link, uint32_t addr, uint8_t mask, uint8_t val, const char *what) {
    uint8_t v = 0;
    if (rt_xfer(link, 2, addr, 1, 0, &v, what, 0)) return -1;
    uint8_t n = (uint8_t)((v & ~mask) | val);
    printf("    L%d U %08x: read %02x -> write %02x (mask %02x)\n", link, addr, v, n, mask);
    return rt_w(link, addr, n, what);
}

static int regw(uint32_t off, int width, uint32_t val, const char *what) {
    uint64_t o[16];
    if (m2call(M5_REGW, (uint64_t)off | ((uint64_t)width << 32), val, o)) return -1;
    if (o[0] != M2R_OK) { printf("    !! BAR4+0x%05x write refused (%s)\n", off, m2rc(o[0])); return -1; }
    printf("    BAR4+0x%05x %-46s 0x%0*llx -> 0x%0*llx\n", off, what, width / 4, o[1], width / 4, o[2]);
    return 0;
}
static uint32_t rd4v(uint32_t off) { uint32_t v = 0xFFFFFFFFu; mmio(4, off, &v); return v; }
static uint32_t cdb(int l) { return 0x30000u + 0x10000u * (uint32_t)l; }

// multi-link bank switch on links 1+2 (kext op 49 = do_bank_switch [ST2]:857-965)
static int m5_bank(struct m5st *st, int bank, const char *why) {
    uint64_t o[16];
    printf("  bank switch -> bank %d (%s): CMDSYNC(1,2), deferred broadcast SCP_FrameCtrl_B%d (0x%02x) = 0x09 with SSP tag,"
           " SYNCGO  [ST2]:754-855, [IB]:243-280\n", bank, why, bank, bank ? 0x70 : 0x60);
    if (m2call(M5_BANK, 0x6, (uint64_t)bank, o)) return -1;
    if (o[0] == M2R_BADOP || o[0] == M2R_PRECOND) { printf("    !! refused (%s)\n", m2rc(o[0])); return -1; }
    printf("    SYNC 0x%08llx -> armed 0x%08llx -> after SYNCGO 0x%08llx -> end 0x%08llx; MCP_STAT before L1 0x%08llx L2 0x%08llx\n",
           o[1], o[3], o[4], o[11], o[2] & 0xFFFFFFFF, o[2] >> 32);
    int ok = (o[0] == M2R_OK);
    for (int l = 1; l <= 2; l++) {
        uint64_t resp = o[5 + 3 * (l - 1)], tm = o[6 + 3 * (l - 1)], stat = o[7 + 3 * (l - 1)];
        int act = (int)((stat >> 20) & 1);
        printf("    link %d: %s after %llu us, response 0x%08llx (%s), MCP_STAT 0x%08llx ACTIVE_BANK=%d%s\n", l,
               (tm >> 32) ? "response" : "TIMEOUT", tm & 0xFFFFFFFF, resp & 0xFFFFFFFF, (resp & 1) ? "ACK" : (resp & 2) ? "NACK" : "no ACK",
               stat, act, act == bank ? "" : "  !! not the requested bank (Linux does not check this; continuing)");
        if ((tm >> 32) && !(resp & 1))
            printf("    !! no ACK for the FrameCtrl write (Linux only logs this: cdns_fill_msg_resp, cadence_master.c:564-600)\n");
    }
    if (!ok) { printf("    !! bank switch failed (Linux: 'Controller Timed out on bank switch' / 'multi link bank switch failed')\n"); return -1; }
    st->cur_bank = bank;
    st->nbank++;
    return 0;
}

// Transport/port parameters for one link (sdw_compute_params [GBA]:661-685 with this bus: 1 stream,
// 1 master port (PDI 2) + 1 slave port (DP1), 48 kHz, 2 ch, 24 bit (BE format S24_LE from the DAI comp,
// [I3P] sof_ipc3_pcm_dai_link_fixup), curr_dr_freq 9.6 MHz, 50 x 4 frame)
struct sdwxp { uint32_t si, hstart, hstop, offset1, offset2, bps, ch, chmask; };
static void m5_compute(struct sdwxp *x, int verbose) {
    uint32_t dr = 9600000, rate = 48000, col = 4, bps = 24, ch = 2;
    uint32_t full_bw = dr / rate, payload = bps * ch, hwidth = (col * payload + full_bw - 1) / full_bw;
    x->si = dr / rate; x->hstop = col - 1; x->hstart = x->hstop - hwidth + 1; x->offset1 = 1; x->offset2 = 0;
    x->bps = bps; x->ch = ch; x->chmask = (1u << ch) - 1;
    if (verbose)
        printf("  transport params ([GBA]:33-325): bandwidth %u bit/s, curr_dr_freq %u -> sample_interval %u, full_bw %u,"
               " payload_bw %u, hwidth %u, hstop %u, hstart %u, block offset %u, offset2 %u; port bps %u, ch_mask 0x%x,"
               " flow ISOCH, data mode NORMAL, BGC 1 (0), BPM per port (0), lane 0\n",
               rate * ch * bps, dr, x->si, full_bw, payload, hwidth, x->hstop, x->hstart, x->offset1, x->offset2, bps, x->chmask);
}

// sdw_program_params(bus) for one link and one bank ([ST2]:666-752): SCP_BusClock_Scale (SDCA), slave DP1
// ([ST2]:65-252), master port 2 (cdns_transport_params + cdns_port_params [CM]:1521-1648), cdns_bus_conf ([CM]:1494-1519)
static int m5_program(int l, int bank, const struct sdwxp *x) {
    char w[96];
    printf("  link %d: program params, bank %d\n", l, bank);
    if (rt_w(l, bank ? 0x72 : 0x62, 3, bank ? "SCP_BusClock_Scale_B1 = 3 (19.2/4.8 MHz)" : "SCP_BusClock_Scale_B0 = 3 (19.2/4.8 MHz)")) return -1;
    if (rt_upd(l, 0x102, 0x0F, 0x00, "DP1_PortCtrl: data mode 0, flow mode 0")) return -1;
    if (rt_w(l, 0x103, x->bps - 1, "DP1_BlockCtrl1: word length - 1 = 23")) return -1;
    snprintf(w, sizeof w, "DP1_SampleCtrl1_B%d = (si - 1) & 0xff", bank);
    if (rt_w(l, bank ? 0x132 : 0x122, (x->si - 1) & 0xFF, w)) return -1;
    snprintf(w, sizeof w, "DP1_OffsetCtrl1_B%d = block offset", bank);
    if (rt_w(l, bank ? 0x134 : 0x124, x->offset1, w)) return -1;
    snprintf(w, sizeof w, "DP1_OffsetCtrl2_B%d", bank);
    if (rt_w(l, bank ? 0x135 : 0x125, x->offset2, w)) return -1;
    snprintf(w, sizeof w, "DP1_BlockCtrl3_B%d = blk_pkg_mode per port", bank);
    if (rt_w(l, bank ? 0x137 : 0x127, 0, w)) return -1;
    snprintf(w, sizeof w, "DP1_SampleCtrl2_B%d = (si - 1) >> 8", bank);
    if (rt_w(l, bank ? 0x133 : 0x123, (x->si - 1) >> 8, w)) return -1;
    snprintf(w, sizeof w, "DP1_HCtrl_B%d = hstart << 4 | hstop", bank);
    if (rt_w(l, bank ? 0x136 : 0x126, (x->hstart << 4) | x->hstop, w)) return -1;
    uint32_t b = cdb(l), cfg = bank ? 0x218 : 0x200;
    uint32_t c = rd4v(b + cfg);
    c &= ~((3u << 16) | (1u << 18));                       // BGC = 0 (SDW_BLK_GRP_CNT_1), BPM = 0 (per port)
    snprintf(w, sizeof w, "CDNS DP2 B%d CONFIG: BGC 0, BPM 0", bank);
    if (regw(b + cfg, 32, c, w)) return -1;
    snprintf(w, sizeof w, "CDNS DP2 B%d OFFSET_CTRL = offset1 | offset2 << 8", bank);
    if (regw(b + cfg + 0x0C, 32, x->offset1 | (x->offset2 << 8), w)) return -1;
    snprintf(w, sizeof w, "CDNS DP2 B%d HCTRL = hstart << 4 | hstop", bank);
    if (regw(b + cfg + 0x10, 32, (x->hstart << 4) | x->hstop, w)) return -1;
    snprintf(w, sizeof w, "CDNS DP2 B%d SAMPLE_CTRL = si - 1", bank);
    if (regw(b + cfg + 0x08, 32, x->si - 1, w)) return -1;
    c = rd4v(b + cfg);
    c = (c & ~((0x1Fu << 8) | 0xFu)) | ((x->bps - 1) << 8);   // WL = bps-1, PORT_DAT 0, PORT_FLOW 0
    snprintf(w, sizeof w, "CDNS DP2 B%d CONFIG: WL 23, flow 0, data 0", bank);
    if (regw(b + cfg, 32, c, w)) return -1;
    uint32_t ck = rd4v(b + (bank ? 0x38 : 0x30));
    snprintf(w, sizeof w, "CDNS CLK_CTRL%d MCLKD = 19.2*2/9.6 - 1 = 3", bank);
    if (regw(b + (bank ? 0x38 : 0x30), 32, (ck & ~0xFFu) | 3, w)) return -1;
    return 0;
}
// sdw_enable_disable_ports ([ST2]:319-415): slave DP1 ChannelEn, master DPN CH_EN
static int m5_ports(int l, int bank, int en, const struct sdwxp *x) {
    char w[96];
    snprintf(w, sizeof w, "DP1_ChannelEn_B%d = %s", bank, en ? "ch_mask" : "0");
    if (rt_w(l, bank ? 0x130 : 0x120, en ? x->chmask : 0, w)) return -1;
    snprintf(w, sizeof w, "CDNS DP2 B%d CH_EN = %s", bank, en ? "ch_mask" : "0");
    return regw(cdb(l) + (bank ? 0x21C : 0x204), 32, en ? x->chmask : 0, w);
}

// DSP->host messages (notifications) while M5 runs: read the uplink mailbox, ack (cnl_ipc_irq_thread [CN]:152-184)
static int m5_notify(struct m5st *st, const struct fwfile *f) {
    uint32_t tdr = 0, tdd = 0;
    if (mmio(4, 0xC0, &tdr) || !(tdr >> 31)) return 0;
    mmio(4, 0xC8, &tdd);
    if ((tdr & 0x0ffff000u) == 0x0dead000u) {
        uint32_t po = (tdd & 0xFFFFFF) + 0x80000;
        printf("  !! FIRMWARE PANIC 0x%08x (SOF_IPC_PANIC_%s), panic data at BAR4+0x%05x:\n", tdr & 0x7FFFFFFF, panic_name(tdr & 0xFFF), po);
        if (po >= 0x80000 && po < 0xFFF00) hexdump(4, po, 32);
        uint32_t esz, eo = find_window(f->nwin, (uint32_t (*)[6])f->win, 6, &esz);
        if (eo) { printf("  exception window BAR4+0x%05x:\n", eo); hexdump(4, eo, 32); }
        return -2;
    }
    uint8_t h[24] = {0};
    rd4buf(0x81000, h, 24);
    uint32_t cmd = le32(h + 4);
    st->notes++;
    if (cmd == (IPC_GLB_STREAM | 0x00090000u)) st->xruns++;
    printf("  DSP->host message: HIPCTDR 0x%08x, mailbox size %u cmd 0x%08x = %s, words %08x %08x %08x %08x\n", tdr, le32(h),
           cmd, ipc_cmd_name(cmd), le32(h + 8), le32(h + 12), le32(h + 16), le32(h + 20));
    uint64_t o[16];
    if (m2call(M3_TDACK, 0, 0, o) == 0) printf("    acked (HIPCTDR 0x%08llx -> 0x%08llx, HIPCTDA |= DONE)\n", o[1], o[2]);
    return 1;
}

// One IPC3 message (kext op 44 = cnl_ipc_send_msg + ipc3_wait_tx_done + sof_ipc3_get_reply)
static int m5_ipc(struct m5st *st, const struct fwfile *f, const struct ipcmsg *m, uint64_t o[16]) {
    uint32_t cmd = le32(m->b + 4);
    st->ipc_n++;
    printf("  IPC %2d %-18s %3u B  %s\n", st->ipc_n, ipc_cmd_name(cmd), m->len, m->what);
    if (m2call(M5_IPC, (uint64_t)(uintptr_t)m->b, m->len, o)) return -1;
    if (o[0] != M2R_OK) {
        printf("    !! %s (cmd 0x%08llx len %llu / HIPCIDR 0x%08llx HIPCIDA 0x%08llx)%s\n", m2rc(o[0]), o[1], o[2], o[1], o[2],
               o[0] == M2R_TIMEOUT ? "\n    Linux: 'ipc tx timed out' -> snd_sof_handle_fw_exception (ipc3.c:296-302)" : "");
        if (o[0] == M2R_TIMEOUT) { fw_status_regs(); m5_notify(st, f); }
        return -1;
    }
    int32_t err = (int32_t)(o[2] >> 32);
    uint32_t rsz = (uint32_t)(o[2] & 0xFFFFFFFF);
    printf("    reply cmd 0x%08llx (%s) size %u error %d after %llu us\n", o[1], ipc_cmd_name((uint32_t)o[1]), rsz, err, o[3] & 0xFFFFFFFF);
    if (err < 0) {
        printf("    !! the firmware rejected it (Linux: 'ipc tx error for %#x ... %d', ipc3.c:305-309)\n", cmd, err);
        o[0] = M2R_VERIFY;
        return -1;
    }
    if ((o[3] >> 32) & 0x80000000u) { if (m5_notify(st, f) == -2) return -1; }
    return 0;
}

// The sine: an integer number of cycles in the 64 KiB ring (8192 frames) so that the looping ring is
// seamless: k = round(1000 * 8192 / 48000) = 171 cycles -> 171 * 48000 / 8192 = 1001.953 Hz.
static void m5_sine(int32_t *buf, double level_db, int chan, double *freq, int32_t *peak) {
    const int frames = 65536 / 8, k = 171;
    double a = pow(10.0, level_db / 20.0) * 2147483647.0;
    *freq = (double)k * 48000.0 / frames;
    *peak = (int32_t)a;
    for (int i = 0; i < frames; i++) {
        int32_t s = (int32_t)lrint(a * sin(2.0 * M_PI * k * i / frames));
        buf[2 * i] = (chan == 2) ? 0 : s;       // left  -> rt1316-1 (link 1) via RX Channel Select 'L,L' [UCM]
        buf[2 * i + 1] = (chan == 1) ? 0 : s;   // right -> rt1316-2 (link 2) via 'R,R'
    }
}

static void m5_plan_print(const struct bootopt *op, const struct m5plan *pl, int hex) {
    struct sdwxp x; m5_compute(&x, 0);
    printf("  ---- M5: speaker playback (after S35; Linux order: BE hw_params, FE hw_params, BE prepare + DAPM, FE prepare,\n"
           "       BE trigger, FE trigger; stop and free in reverse; [SPC]/[SP]/[SU]) ----\n");
    printf("  S40 topology %s -> speaker path + IPC3 messages (user-space port of [TP]/[I3T]/[I3C]/[I3P])\n", op->tplg);
    printf("  S41 RT1316 L1/L2 sdw_initialize_slave ([BU]:1380-1490): SCP_BusClock_Base (0x4D) = 1 (19.2 MHz), Scale_B0/B1\n"
           "      (0x62/0x72) = 3; SCP_INT1 bus-clash clear if set (CLEAR_INITIAL_CLASH quirk); SCP_IntMask1 |= BUS_CLASH|PARITY\n");
    printf("  S42 RT1316 L1/L2 rt1316_io_init ([RT]:267-306): 0xc000 = 0x02 (SW reset), %zu blind writes ([RT]:71-129), read-back\n",
           sizeof rt1316_blind / sizeof rt1316_blind[0]);
    printf("  S43 UCM 'Speaker' enable ([UCM]): rt1316-1 RX Channel Select 'L,L' (UDMPU21 cluster 0x%08x = 1), rt1316-2 'R,R' (= 5);\n"
           "      DAC Switch on (FU21 MUTE L 0x%08x / R 0x%08x = 0)\n", RT_UDMPU21_CLUSTER, RT_FU21_MUTE_L, RT_FU21_MUTE_R);
    printf("  S44 BE hw_params per cpu DAI (intel_hw_params [IS]:720-785): SDW%u/SDW%u Pin2: PCMSyCHM(l,2) = STREAM alh id | HCHN 1,\n"
           "      ALH STRMZCFG(alh id) DMAT 3 CHN 1 ([IS]:625-675), Cadence PORTCTRL(2) TX, PDI_CONFIG(2) = 2 | ch 0x3 | SOFT_RESET\n"
           "      ([CM]:1926-1953); ALH ids %u and %u go into the DAI_CONFIG messages ([SDH]:61-78)\n",
           1u, 2u, pl->dai[pl->dai[0].link == 1 ? 0 : 1].alh_id, pl->dai[pl->dai[0].link == 1 ? 1 : 0].alh_id);
    printf("  S45 FE hw_params: ring 64 KiB + BDL (16 x 4096, IOC) + SOF page table (kext op 45); host stream SD[ISS] tag 1: EM2.L1SEN=0,\n"
           "      PPCTL PROCEN, reset, tag, CBL 65536, FMT 0x0041, LVI 15, BDL, int bits, SPIB off ([HST]:563-755, [HP]:101-151)\n");
    printf("  S46 BE prepare = sdw_prepare_stream ([ST2]:1474-1552) on links 1+2: program params in bank 1, bank switch to 1\n");
    m5_compute(&x, 1);
    printf("  S47 DAPM stream start ([RT]:329-370 CLASS D POST_PMU): PDE23/PDE27/PDE22 REQ_PS = PS0 on both amps; ACTUAL_PS read (info)\n");
    printf("  S48 FE prepare: %d topology messages + PCM_PARAMS (each reply must have error 0)\n", pl->n);
    pl_print(pl, hex);
    if (op->noplay) {
        printf("  (--no-play / M5a: S49-S51 are skipped: no channel enable, no DMA start, no TRIG_START; nothing is played)\n");
    } else {
        printf("  S49 trigger START (DPCM POST: BE first, [SPC]:2513-2560 dpcm_fe_dai_do_trigger): sdw_enable_stream ([ST2]:1597-1638) = params in\n"
               "      bank 0 + DP1/DP2 channels on in bank 0 + bank switch to 0; then FE: host DMA RUN, then STREAM_TRIG_START ([SP]:385-495)\n");
        printf("  S50 play %.1f s: %.2f Hz sine at %.1f dBFS (%s), S32_LE stereo looping ring; poll LPIB/SD STS/HIPCTDR every 20 ms\n",
               op->secs, 171.0 * 48000.0 / 8192.0, op->level, op->chan == 0 ? "both channels" : op->chan == 1 ? "left only" : "right only");
        printf("  S51 stop: PGA volume 0 (ramped, M5 fade-out), 80 ms; FE first: STREAM_TRIG_STOP, DMA stop, STREAM_PCM_FREE, stream\n"
               "      hw_free (reset_hw_params_during_stop, [I3P]:436-445); BE: sdw_disable_stream ([ST2]:1672-1726)\n");
    }
    printf("  S52 FE hw_free: IPC3 free sequence above; BE hw_free: sdw_deprepare_stream ([ST2]:1759-1831): params bank 0, bank switch 0\n");
    printf("  S53 DAPM stop + UCM disable: PDE23/27/22 = PS3, FU21 L/R mute = 1 (no 5 s pmdown delay)\n");
    printf("  S54 report, then the M4/M3 teardown (links off, DSP off) -> M1 state\n");
    printf("  M5 rollback (failure, --undo, audctl killed, kext stop): host DMA RUN=0 -> both amps FU21 mute + PDE PS3 (FIFO, best\n"
           "      effort) -> master DP2 CH_EN=0 both banks -> M4 rollback (links off) -> M3 rollback (DSP off, BME off, DMA freed)\n");
}

// S40-S54. Returns 0 success, -1 failure (the caller then runs the kext rollback, which also mutes the amps).
// On a failure after the stream started, the host DMA and the pipeline are stopped here first.
static int m5_run(const struct bootopt *op, const struct fwfile *f, struct m5plan *pl, const char **failstep,
                  uint64_t o[16], char *summary, size_t sn) {
    struct m5st st; memset(&st, 0, sizeof st);
    struct sdwxp x; m5_compute(&x, 0);
    char t[160];
    int amp_link[2] = {1, 2};

    // ---------------- S40 (already parsed; re-print the key facts)
    m2b_step("[S40] topology -> speaker path   (topology.c, ipc3-topology.c, sof-audio.c)");
    printf("  PCM '%s' (pcm %u), host comp %u, PGA comp %u (volume table min 0x%x max 0x%x, %u ch), DEMUX comp %u\n",
           pl->pcm_name, pl->pcm_id, pl->host_comp, pl->pga_comp, pl->vol_min, pl->vol_max, pl->vol_ch, pl->demux_comp);
    for (int k = 0; k < 2; k++)
        printf("  DAI %s: ALH tplg index %u -> link %u PDI %u (cpu DAI 'SDW%u Pin%u'), FW dai_index 0x%x, ALH stream id %u, %u Hz %u ch\n",
               pl->dai[k].wname, pl->dai[k].tplg_index, pl->dai[k].link, pl->dai[k].pdi, pl->dai[k].link, pl->dai[k].pdi,
               pl->dai[k].dai_index, pl->dai[k].alh_id, pl->dai[k].rate, pl->dai[k].ch);
    // bank state before anything
    for (int l = 1; l <= 2; l++) {
        uint32_t s = rd4v(cdb(l) + 0x40);
        printf("  link %d MCP_STAT 0x%08x ACTIVE_BANK=%u (Linux bus params: curr_bank 0, next_bank 1 after start)\n", l, s, (s >> 20) & 1);
        if ((s >> 20) & 1) { *failstep = "S40 unexpected active bank"; o[0] = M2R_PRECOND; return -1; }
    }

    // ---------------- S41 sdw_initialize_slave
    for (int i = 0; i < 2; i++) {
        int l = amp_link[i];
        snprintf(t, sizeof t, "[S41.%d] link %d RT1316 dev 1: sdw_initialize_slave   (bus.c:1419-1490)", l, l); m2b_step(t);
        st.amp_init = 1;
        if (rt_w(l, 0x4D, 1, "SCP_BusClock_Base = 19.2 MHz (sdw_slave_get_scale_index)") ||
            rt_w(l, 0x62, 3, "SCP_BusClock_Scale_B0 = 3 (scale 4: 19.2/4.8 MHz, index+1)") ||
            rt_w(l, 0x72, 3, "SCP_BusClock_Scale_B1 = 3")) { *failstep = "S41 bus clock base/scale"; return -1; }
        uint8_t v = 0;
        if (rt_r(l, 0x40, &v, "SCP_Int1 (CLEAR_INITIAL_CLASH quirk, bus.c:1430-1446)")) { *failstep = "S41 SCP_INT1 read"; return -1; }
        if (v & 0x02) { if (rt_w(l, 0x40, 0x02, "SCP_Int1 BUS_CLASH w1c ('Bus clash detected before INT mask is enabled')")) { *failstep = "S41 clash clear"; return -1; } }
        printf("    (PARITY clear skipped: RT1316 sets SDW_SLAVE_QUIRKS_INVALID_INITIAL_PARITY, rt1316-sdw.c:205)\n");
        if (rt_upd(l, 0x41, 0x03, 0x03, "SCP_IntMask1 |= BUS_CLASH|PARITY (scp_int1_mask)")) { *failstep = "S41 SCP_INTMASK1"; return -1; }
        printf("    (no DP0 properties on RT1316: DP0 interrupt mask not touched, bus.c:1486-1487)\n");
    }

    // ---------------- S42 rt1316_io_init
    for (int i = 0; i < 2; i++) {
        int l = amp_link[i];
        snprintf(t, sizeof t, "[S42.%d] link %d RT1316: rt1316_io_init (SW reset + blind writes)   (rt1316-sdw.c:267-306, 71-129)", l, l); m2b_step(t);
        if (rt_w(l, 0xc000, 0x02, "SW reset")) { *failstep = "S42 SW reset"; return -1; }
        for (size_t k = 0; k < sizeof rt1316_blind / sizeof rt1316_blind[0]; k++) {
            snprintf(t, sizeof t, "blind write %zu/%zu", k + 1, sizeof rt1316_blind / sizeof rt1316_blind[0]);
            if (rt_w(l, rt1316_blind[k].reg, rt1316_blind[k].val, t)) { *failstep = "S42 blind write"; return -1; }
        }
        static const uint32_t chk[] = {0xc710, 0xc711, 0xc712, 0xc001, 0xc010, 0xd101, 0xd103, 0x2f0a, 0x2f36};
        int mism = 0;
        for (size_t k = 0; k < sizeof chk / sizeof chk[0]; k++) {
            uint8_t v = 0;
            if (rt_xfer(l, 2, chk[k], 1, 0, &v, "read-back", 0)) { printf("    read-back 0x%04x failed\n", chk[k]); mism++; continue; }
            uint8_t want = 0;
            for (size_t j = 0; j < sizeof rt1316_blind / sizeof rt1316_blind[0]; j++) if (rt1316_blind[j].reg == chk[k]) want = rt1316_blind[j].val;
            printf("    read-back 0x%04x = %02x (written %02x)%s\n", chk[k], v, want, v == want ? "" : "  (differs; volatile/self-updating bits are possible)");
        }
        uint8_t xb = 0xFF;
        rt_r(l, RT_XU24_BYPASS, &xb, "XU24 bypass (blind write 0x00)");
        printf("    %d read-back difference(s) (informational; Linux does not read back)\n", mism);
    }

    // ---------------- S43 UCM Speaker enable
    m2b_step("[S43] UCM 'Speaker' EnableSequence: RX Channel Select + DAC Switch   (ucm2/sof-soundwire/rt1316.conf)");
    for (int i = 0; i < 2; i++) {
        int l = amp_link[i];
        if (rt_w(l, RT_UDMPU21_CLUSTER, l == 1 ? 1 : 5, l == 1 ? "rt1316-1 RX Channel Select = 'L,L' (index 1)" : "rt1316-2 RX Channel Select = 'R,R' (index 5)") ||
            rt_w(l, RT_FU21_MUTE_L, 0, "DAC Switch on: FU21 MUTE CH_L = 0") ||
            rt_w(l, RT_FU21_MUTE_R, 0, "DAC Switch on: FU21 MUTE CH_R = 0")) { *failstep = "S43 UCM controls"; return -1; }
        st.ucm_on = 1;
    }

    // ---------------- S44 BE hw_params: Intel PDI/shim/ALH/Cadence port
    m2b_step("[S44] BE hw_params: intel_hw_params per cpu DAI (SDW1 Pin2, SDW2 Pin2)   (intel.c:720-785, 625-675; cadence_master.c:1926-1953)");
    for (int k = 0; k < 2; k++) {   // cpu DAI order of the BE link: SDW1 Pin2, SDW2 Pin2 ([SPC]:1252-1290)
        const struct m5dai *dd = &pl->dai[pl->dai[0].link == (uint32_t)(k + 1) ? 0 : 1];
        uint32_t l = dd->link, pdi = dd->pdi;
        printf("  link %u PDI %u: ch %u, TX (playback), ALH id %u, sdw_stream_add_master port %u ch_mask 0x%x (no register access)\n",
               l, pdi, dd->ch, dd->alh_id, pdi, x.chmask);
        st.ports = 1;
        if (regw(0x2C000 + 0x22 + 0x60 * l + 2 * pdi, 16, (dd->alh_id << 8) | ((dd->ch - 1) << 4) | 0, "PCMSyCHM: STREAM=alh id, HCHN=ch-1, LCHN 0, DIR TX")) { *failstep = "S44 PCMSyCHM"; return -1; }
        uint32_t a = rd4v(0x2C800 + 4 * dd->alh_id);
        a = (a & ~0xFFu & ~(0xFu << 16)) | 0x3u | ((dd->ch - 1) << 16);
        if (regw(0x2C800 + 4 * dd->alh_id, 32, a, "ALH STRMZCFG: DMAT 3, CHN ch-1")) { *failstep = "S44 ALH STRMZCFG"; return -1; }
        uint32_t pc = rd4v(cdb((int)l) + 0x130 + pdi * 0x80);
        pc &= ~((1u << 7) | (1u << 1) | (1u << 16));
        if (regw(cdb((int)l) + 0x130 + pdi * 0x80, 32, pc, "CDNS PORTCTRL(2): DIRN 0 (TX), not bulk")) { *failstep = "S44 PORTCTRL"; return -1; }
        if (regw(cdb((int)l) + 0x1100 + pdi * 16, 32, pdi | (1u << 24) | (x.chmask << 8), "CDNS PDI_CONFIG(2): port 2 | ch 0x3 | SOFT_RESET")) { *failstep = "S44 PDI_CONFIG"; return -1; }
        printf("    intel_params_stream -> sdw_params_stream -> DAI config stored for %s: dai_index 0x%x stream_id %u (sent at S48)\n",
               dd->wname, dd->dai_index, dd->alh_id);
    }

    // ---------------- S45 FE hw_params: ring + host stream
    m2b_step("[S45] FE hw_params: playback ring + host stream SD[ISS] tag 1   (hda-pcm.c:101-151, hda-stream.c:563-755)");
    {
        int32_t *ring = calloc(1, 65536);
        double fr = 0; int32_t pk = 0;
        if (!ring) { *failstep = "S45 malloc"; o[0] = M2R_NOMEM; return -1; }
        if (op->noplay) memset(ring, 0, 65536);
        else m5_sine(ring, op->level, op->chan, &fr, &pk);
        printf("  ring: %s, %d frames S32_LE stereo, FNV-1a 0x%08x%s\n",
               op->noplay ? "silence (--no-play)" : "sine", 8192, fnv1a((uint8_t *)ring, 65536), "");
        if (!op->noplay) printf("  sine: 171 cycles per 8192 frames = %.3f Hz, peak %d (%.1f dBFS), channels %s\n", fr, pk, op->level,
                                op->chan == 0 ? "L+R" : op->chan == 1 ? "L only (link 1 amp)" : "R only (link 2 amp)");
        int r = m2call(M5_SALLOC, (uint64_t)(uintptr_t)ring, 65536, o);
        uint32_t want = fnv1a((uint8_t *)ring, 65536);
        free(ring);
        if (r) { *failstep = "S45 ring"; return -1; }
        printf("  ring IOVA 0x%llx phys 0x%llx (%s), BDL at 0x%llx, page table at 0x%llx [first PFNs %012llx], kernel FNV-1a 0x%08llx %s  %s\n",
               o[1], o[2], o[3] ? "IOVA == phys" : "remapped", o[5], o[6], o[7], o[4], (uint32_t)o[4] == want ? "MATCH" : "MISMATCH", m2rc(o[0]));
        if (o[0] != M2R_OK || (uint32_t)o[4] != want) { *failstep = "S45 ring allocation"; if (!o[0]) o[0] = M2R_VERIFY; return -1; }
        st.dma = 1;
        if (m2call(M5_STREAM, 0, 0, o)) { *failstep = "S45 stream"; return -1; }
        st.sprep = 1;
        printf("  SD%llu CTL/STS 0x%08llx (tag %llu) CBL %llu LVI/FIFOW 0x%08llx FIFOS/FMT 0x%08llx BDL 0x%llx PPCTL 0x%08llx EM2 0x%08llx (L1SEN %llu)\n"
               "  SPBFCCTL 0x%08llx  %s\n", o[9], o[1], (o[1] >> 20) & 0xF, o[2], o[3], o[4], o[5], o[6], o[7], (o[7] >> 13) & 1, o[10], m2rc(o[0]));
        if (o[0] != M2R_OK) { *failstep = "S45 host stream hw_params"; return -1; }
    }

    // ---------------- S46 BE prepare
    m2b_step("[S46] BE prepare: sdw_prepare_stream on links 1+2   (stream.c:1474-1552, 666-752)");
    m5_compute(&x, 1);
    for (int i = 0; i < 2; i++) if (m5_program(amp_link[i], 1, &x)) { *failstep = "S46 program params (bank 1)"; return -1; }
    if (m5_bank(&st, 1, "prepare")) { *failstep = "S46 bank switch to 1"; o[0] = M2R_TIMEOUT; return -1; }
    st.bank_prep = 1;
    printf("  port prepare ([ST2]:417-618): RT1316 DP1 has simple_ch_prep_sm, no impl-def interrupts and no port_prep op; the\n"
           "  Cadence master has no dpn_port_prep op -> nothing on the bus\n");

    // ---------------- S47 DAPM start: amp power
    m2b_step("[S47] DAPM stream start: RT1316 CLASS D POST_PMU -> PDE23/27/22 PS0   (rt1316-sdw.c:329-370)");
    for (int i = 0; i < 2; i++) {
        int l = amp_link[i];
        st.pde_on = 1;
        if (rt_w(l, RT_PDE23, 0, "PDE23 REQUESTED_PS = PS0") || rt_w(l, RT_PDE27, 0, "PDE27 REQUESTED_PS = PS0") ||
            rt_w(l, RT_PDE22, 0, "PDE22 REQUESTED_PS = PS0")) { *failstep = "S47 PDE PS0"; return -1; }
    }
    for (int i = 0; i < 2; i++) {
        int l = amp_link[i];
        uint8_t a = 0xFF, b = 0xFF, c = 0xFF;
        for (int tries = 0; tries < 10; tries++) {   // informational: SDCA ACTUAL_PS (control 0x10), up to ~100 ms
            if (rt_xfer(l, 2, RT_ACT_PS(0x31), 1, 0, &a, "", 0) || rt_xfer(l, 2, RT_ACT_PS(0x32), 1, 0, &b, "", 0) ||
                rt_xfer(l, 2, RT_ACT_PS(0x33), 1, 0, &c, "", 0)) { a = b = c = 0xEE; break; }
            if (!a && !b && !c) break;
            usleep(10000);
        }
        printf("    link %d ACTUAL_PS PDE23 %02x PDE27 %02x PDE22 %02x (0 = PS0; 0xee = not readable; Linux never reads these)\n", l, a, b, c);
    }

    // ---------------- S48 FE prepare: topology + PCM params
    m2b_step("[S48] FE prepare: sof_widget_list_setup + STREAM_PCM_PARAMS   (sof-audio.c:760-838, pcm.c:315-375, ipc3-pcm.c:39-143)");
    for (int i = 0; i < pl->n; i++) {
        st.ipc_setup = 1;
        if (m5_ipc(&st, f, &pl->m[i], o)) { *failstep = "S48 topology IPC"; return -1; }
    }
    if (m5_ipc(&st, f, &pl->pcm_params, o)) { *failstep = "S48 PCM_PARAMS"; return -1; }
    st.params = 1;
    st.posn_offset = (uint32_t)o[6];
    printf("    sof_ipc_pcm_params_reply: comp_id %llu posn_offset 0x%x (stream mailbox BAR4+0xc1000 + offset)\n", o[5] >> 32, st.posn_offset);

    double played = 0; uint64_t moved = 0; int lpib_bad = 0;
    if (!op->noplay) {
        // ---------------- S49 trigger START: BE then FE
        m2b_step("[S49] trigger START: BE sdw_enable_stream, then FE host DMA RUN + STREAM_TRIG_START   (stream.c:1597-1638, pcm.c:385-495)");
        for (int i = 0; i < 2; i++) {
            if (m5_program(amp_link[i], 0, &x)) { *failstep = "S49 program params (bank 0)"; return -1; }
            if (m5_ports(amp_link[i], 0, 1, &x)) { *failstep = "S49 enable ports"; return -1; }
        }
        if (m5_bank(&st, 0, "enable")) { *failstep = "S49 bank switch to 0"; o[0] = M2R_TIMEOUT; return -1; }
        st.enabled = 1;
        if (m2call(M5_STREAM, 1, 0, o)) { *failstep = "S49 DMA"; return -1; }
        printf("  host DMA: SD CTL 0x%08llx -> 0x%08llx (RUN after ~%llu us) LPIB 0x%llx  %s\n", o[1], o[2], o[3], o[4], m2rc(o[0]));
        if (o[0] != M2R_OK) { *failstep = "S49 host DMA start"; goto stop_dma; }
        st.started = 1;
        if (m5_ipc(&st, f, &pl->trig_start, o)) { *failstep = "S49 TRIG_START"; goto stop_dma; }

        // ---------------- S50 play
        snprintf(t, sizeof t, "[S50] playing %.1f s: listen for a ~1 kHz tone   (position = DPIB like hda_dsp_stream_get_position, hda-stream.c:1075-1150)", op->secs);
        m2b_step(t);
        uint32_t sidx = 7, prev = 0, lp = 0, dp = 0, sts = 0;
        { uint64_t q[16]; if (m2call(M2B_INFO, 0, 0, q) == 0) sidx = (uint32_t)((q[3] >> 24) & 0xFF); }
        uint32_t sd = 0x80 + 0x20 * sidx, dpib = 0x1084 + 0x20 * sidx;   // AZX_REG_VS_SDXDPIB_XBASE/XINTERVAL hda_register.h:129-130
        mmio(0, dpib, &prev);
        struct timespec t0, tn; clock_gettime(CLOCK_MONOTONIC, &t0);
        double nextp = 0;
        for (;;) {
            usleep(20000);
            clock_gettime(CLOCK_MONOTONIC, &tn);
            double el = (double)(tn.tv_sec - t0.tv_sec) + (double)(tn.tv_nsec - t0.tv_nsec) / 1e9;
            uint32_t ctl = 0;
            mmio(0, sd + 4, &lp); mmio(0, dpib, &dp); mmio(0, sd, &ctl);
            sts = ctl >> 24;
            if (dp >= 65536) lpib_bad++;
            moved += (uint32_t)((dp - prev) & 0xFFFF); prev = dp;
            int nr = m5_notify(&st, f);
            if (nr == -2) { *failstep = "S50 firmware panic"; o[0] = M2R_VERIFY; goto stop_dma; }
            if (el >= nextp) {
                printf("  t=%5.2f s DPIB 0x%05x LPIB 0x%05x moved %8llu B (%.3f s of audio, %.0f B/s) SD CTL 0x%08x STS 0x%02x (BCIS %u FIFOE %u DESE %u) xruns %d\n",
                       el, dp, lp, (unsigned long long)moved, (double)moved / 384000.0, el > 0 ? (double)moved / el : 0, ctl, sts,
                       (sts >> 2) & 1, (sts >> 3) & 1, (sts >> 4) & 1, st.xruns);
                nextp += 0.25;
            }
            if (sts & 0x18) { printf("  !! SD STS FIFOE/DESE set: host DMA error\n"); *failstep = "S50 host DMA error"; o[0] = M2R_DMAERR; goto stop_dma; }
            if (el >= op->secs) { played = el; break; }
        }
        if (st.posn_offset < 0x1000) {
            uint8_t pb[48];
            if (rd4buf(0xc1000 + st.posn_offset, pb, 48) == 0)
                printf("  stream mailbox posn (BAR4+0x%05x): host_posn 0x%llx dai_posn 0x%llx (FW updates these only on request/periods; informational)\n",
                       0xc1000 + st.posn_offset, (unsigned long long)(le32(pb + 20) | ((uint64_t)le32(pb + 24) << 32)),
                       (unsigned long long)(le32(pb + 28) | ((uint64_t)le32(pb + 32) << 32)));
        }

        // ---------------- S51 stop
        m2b_step("[S51] stop: fade-out, FE STREAM_TRIG_STOP + DMA stop + PCM_FREE + stream hw_free, BE sdw_disable_stream");
        if (pl->vol_mute.len && m5_ipc(&st, f, &pl->vol_mute, o)) printf("  (fade-out volume message failed; continuing with the stop)\n");
        usleep(80000);
        if (m5_ipc(&st, f, &pl->trig_stop, o)) { *failstep = "S51 TRIG_STOP"; goto stop_dma; }
        if (m2call(M5_STREAM, 2, 0, o) || o[0] != M2R_OK) { printf("  !! host DMA stop: %s\n", m2rc(o[0])); *failstep = "S51 DMA stop"; return -1; }
        printf("  host DMA stopped: SD CTL 0x%08llx -> 0x%08llx, LPIB at stop 0x%llx\n", o[1], o[2], o[4]);
        st.started = 0;
        if (m5_ipc(&st, f, &pl->pcm_free, o)) { *failstep = "S51 PCM_FREE"; return -1; }
        st.params = 0;
        if (m2call(M5_STREAM, 3, 0, o) || o[0] != M2R_OK) { *failstep = "S51 stream hw_free"; return -1; }
        printf("  stream hw_free: SD CTL 0x%08llx, PPCTL 0x%08llx, EM2 0x%08llx (L1SEN %llu)\n", o[1], o[2], o[3], (o[3] >> 13) & 1);
        st.sprep = 0;
        for (int i = 0; i < 2; i++) if (m5_ports(amp_link[i], 1, 0, &x)) { *failstep = "S51 disable ports (bank 1)"; return -1; }
        for (int i = 0; i < 2; i++) if (m5_program(amp_link[i], 1, &x)) { *failstep = "S51 program params (bank 1)"; return -1; }
        if (m5_bank(&st, 1, "disable")) { *failstep = "S51 bank switch to 1"; o[0] = M2R_TIMEOUT; return -1; }
        for (int i = 0; i < 2; i++) if (m5_ports(amp_link[i], 0, 0, &x)) { *failstep = "S51 disable ports (bank 0)"; return -1; }
        st.enabled = 0;
    } else {
        m2b_step("[S49-S51] skipped (--no-play / M5a): nothing was enabled or started");
    }

    // ---------------- S52 hw_free
    m2b_step("[S52] FE hw_free: widget free sequence; BE hw_free: sdw_deprepare_stream   (sof-audio.c:840-861, stream.c:1759-1831)");
    if (st.params) { if (m5_ipc(&st, f, &pl->pcm_free, o)) { *failstep = "S52 PCM_FREE"; return -1; } st.params = 0; }
    if (st.sprep) {
        if (m2call(M5_STREAM, 3, 0, o) || o[0] != M2R_OK) { *failstep = "S52 stream hw_free"; return -1; }
        st.sprep = 0;
    }
    for (int i = 0; i < pl->nf; i++) if (m5_ipc(&st, f, &pl->f[i], o)) { *failstep = "S52 widget free IPC"; return -1; }
    st.ipc_setup = 0;
    for (int i = 0; i < 2; i++) if (m5_program(amp_link[i], 0, &x)) { *failstep = "S52 program params (bank 0)"; return -1; }
    if (m5_bank(&st, 0, "deprepare")) { *failstep = "S52 bank switch to 0"; o[0] = M2R_TIMEOUT; return -1; }
    st.bank_prep = 0;

    // ---------------- S53 amps off
    m2b_step("[S53] DAPM stop + UCM disable: PDE23/27/22 PS3, DAC Switch off   (rt1316-sdw.c:352-367, rt1316.conf DisableSequence)");
    for (int i = 0; i < 2; i++) {
        int l = amp_link[i];
        if (rt_w(l, RT_PDE23, 3, "PDE23 REQUESTED_PS = PS3") || rt_w(l, RT_PDE27, 3, "PDE27 REQUESTED_PS = PS3") ||
            rt_w(l, RT_PDE22, 3, "PDE22 REQUESTED_PS = PS3") || rt_w(l, RT_FU21_MUTE_L, 1, "DAC Switch off: FU21 MUTE CH_L = 1") ||
            rt_w(l, RT_FU21_MUTE_R, 1, "DAC Switch off: FU21 MUTE CH_R = 1")) { *failstep = "S53 amp off"; return -1; }
    }
    st.pde_on = 0; st.ucm_on = 0;

    m2b_step("[S54] M5 report");
    uint64_t q[16];
    if (m2call(M5_STATE, 0, 0, q) == 0)
        printf("  kext: flags 0x%llx, %llu IPC(s), last error %d; SD CTL 0x%08llx LPIB 0x%llx; HIPCIDR/IDA 0x%016llx HIPCTDR 0x%08llx; MCP_STAT L1 0x%08llx L2 0x%08llx\n",
               q[1] & 0xFFFF, q[2] & 0xFFFFFFFF, (int32_t)(q[2] >> 32), q[4], q[5], q[12], q[13], q[14] & 0xFFFFFFFF, q[14] >> 32);
    if (op->noplay)
        snprintf(summary, sn, "M5a: %d IPC(s) all error 0, RT1316 x2 initialised and powered PS0 -> PS3, %d bank switches, no audio played",
                 st.ipc_n, st.nbank);
    else {
        double secs = (double)moved / 384000.0;
        snprintf(summary, sn, "played %.1f s of a %.2f Hz sine at %.1f dBFS on %s; host DMA moved %llu bytes = %.2f s at 48 kHz/2ch/32-bit"
                 " (%.0f%% of real time), %d xrun notification(s), %d IPC(s) all error 0%s",
                 played, 171.0 * 48000.0 / 8192.0, op->level, op->chan == 0 ? "both speakers" : op->chan == 1 ? "the left speaker" : "the right speaker",
                 (unsigned long long)moved, secs, played > 0 ? 100.0 * secs / played : 0, st.xruns, st.ipc_n, lpib_bad ? ", DPIB out of range seen" : "");
        if (played <= 0 || secs < 0.8 * played || secs > 1.2 * played) {
            printf("  !! the host DMA did not move data at the 48 kHz rate (%.2f s of audio in %.2f s): the DSP pipeline is not consuming\n", secs, played);
            return 1;
        }
        if (st.xruns) return 1;
    }
    return 0;

stop_dma:
    // failure while the stream may be running: stop it the Linux way as far as possible (FE first), then fail
    printf("\n  !! failure during playback: stopping (TRIG_STOP, DMA stop); the rollback mutes the amps and powers everything down\n");
    if (st.started) { uint64_t r[16]; m5_ipc(&st, f, &pl->trig_stop, r); }
    { uint64_t r[16]; m2call(M5_STREAM, 2, 0, r); printf("  host DMA stop: %s\n", m2rc(r[0])); }
    return -1;
}

static int cmd_boot(int argc, char **argv, int mode) {   // mode 0 m2b, 1 m3, 2 m4, 3 m5, 4 m5a (= m5 --no-play)
    char defpath[1024];
    const char *home = getenv("HOME");
    snprintf(defpath, sizeof defpath, "%s/xe/build/audio/fw/use/sof-rpl.ri", home ? home : "");
    int m3 = mode >= 1, m4 = mode >= 2, m5 = mode >= 3;
    char tdef[1024];
    snprintf(tdef, sizeof tdef, "%s/xe/build/audio/fw/use/sof-rpl-rt1316-l12-rt714-l0.tplg", home ? home : "");
    struct bootopt op = {3, 1, 1, 1, 0, 1, 0, m3, 0, 300, 2000, defpath, m4, 0xF, 1000, m5, mode == 4, 0, 0, -30.0, 2.0, tdef};
    const char *cmd = m5 ? (mode == 4 ? "m5a" : "m5") : m4 ? "m4" : m3 ? "m3" : "m2b";
    const char *MN = m5 ? "M5" : m4 ? "M4" : m3 ? "M3" : "M2b";
    static struct tplg tpbuf;
    static struct m5plan plbuf;
    int dry = 0, undo = 0, state = 0, offline = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dry")) dry = 1;
        else if (!strcmp(argv[i], "--undo")) undo = 1;
        else if (!strcmp(argv[i], "--state")) state = 1;
        else if (!strcmp(argv[i], "--offline")) offline = 1;
        else if (!strcmp(argv[i], "--leave-up")) op.leave = 1;
        else if (!strcmp(argv[i], "--no-ctrl-reset")) op.ctrl = 0;
        else if (!strcmp(argv[i], "--no-iccmax")) op.icc = 0;
        else if (!strcmp(argv[i], "--no-l1sen")) op.l1 = 0;
        else if (!strcmp(argv[i], "--no-ssp")) op.ssp = 0;            // kept for compatibility (default)
        else if (!strcmp(argv[i], "--ssp")) op.ssp = 1;
        else if (!strcmp(argv[i], "--no-pie")) op.pie = 0;
        else if (m3 && !strcmp(argv[i], "--post-fw-run")) op.post = 1;
        else if (m3 && !strncmp(argv[i], "--fw=", 5)) op.fw = argv[i] + 5;
        else if (m3 && !strncmp(argv[i], "--ready-ms=", 11)) { op.readyms = (unsigned)strtoul(argv[i] + 11, NULL, 0); if (op.readyms < 100) op.readyms = 100; if (op.readyms > 5000) op.readyms = 5000; }
        else if (m4 && !strncmp(argv[i], "--links=", 8)) { op.links = (unsigned)strtoul(argv[i] + 8, NULL, 0) & 0xF; if (!op.links) { fprintf(stderr, "m4: --links must be 0x1..0xF\n"); return 2; } }
        else if (m4 && !strncmp(argv[i], "--attach-ms=", 12)) { op.attachms = (unsigned)strtoul(argv[i] + 12, NULL, 0); if (op.attachms < 100) op.attachms = 100; if (op.attachms > 2000) op.attachms = 2000; }
        else if (m5 && (!strcmp(argv[i], "--seconds") || !strncmp(argv[i], "--seconds=", 10))) {
            const char *v = argv[i][9] == '=' ? argv[i] + 10 : (i + 1 < argc ? argv[++i] : "");
            op.secs = strtod(v, NULL);
            if (!(op.secs >= 0.5 && op.secs <= 3.0)) { fprintf(stderr, "%s: --seconds must be 0.5..3 (safety cap; first runs: 2)\n", cmd); return 2; } }
        else if (m5 && (!strcmp(argv[i], "--level") || !strncmp(argv[i], "--level=", 8))) {
            const char *v = argv[i][7] == '=' ? argv[i] + 8 : (i + 1 < argc ? argv[++i] : "");
            op.level = strtod(v, NULL);
            if (!(op.level >= -60.0 && op.level <= -12.0)) { fprintf(stderr, "%s: --level must be -60..-12 dBFS (never louder than -12)\n", cmd); return 2; } }
        else if (m5 && !strncmp(argv[i], "--channel=", 10)) {
            const char *v = argv[i] + 10;
            op.chan = !strcmp(v, "both") ? 0 : !strcmp(v, "left") ? 1 : !strcmp(v, "right") ? 2 : -1;
            if (op.chan < 0) { fprintf(stderr, "%s: --channel=both|left|right\n", cmd); return 2; } }
        else if (m5 && !strcmp(argv[i], "--no-play")) op.noplay = 1;
        else if (m5 && !strcmp(argv[i], "--hex")) op.hex = 1;
        else if (m5 && !strncmp(argv[i], "--tplg=", 7)) op.tplg = argv[i] + 7;
        else if (!strncmp(argv[i], "--attempts=", 11)) { op.attempts = atoi(argv[i] + 11); if (op.attempts < 1) op.attempts = 1; if (op.attempts > 3) op.attempts = 3; }
        else if (!strncmp(argv[i], "--rom-ms=", 9)) { op.romms = (unsigned)strtoul(argv[i] + 9, NULL, 0); if (op.romms < 50) op.romms = 50; if (op.romms > 1000) op.romms = 1000; }
        else { fprintf(stderr, "%s: unknown option %s\n", cmd, argv[i]); return 2; }
    }
    if (dry + undo + state > 1) { fprintf(stderr, "%s: --dry, --undo and --state are exclusive\n", cmd); return 2; }
    if (m5 && op.leave) { fprintf(stderr, "%s: --leave-up is not supported (M5 always tears down; amps end muted in PS3)\n", cmd); return 2; }
    if (undo) { char *a[] = {"--undo"}; return cmd_m2(1, a); }   // same kext rollback (M2b/M3/M4/M5-aware)
    g_pre_m2b = 1;
    unsigned need = m5 ? 7 : m4 ? 6 : m3 ? 5 : 4;


    struct fwfile fw;
    memset(&fw, 0, sizeof fw);
    uint64_t o[16], p[16];
    if (dry) {
        printf("audctl 0.9.0  %s --dry: NOTHING is armed or written.\n", cmd);
        if (m3) { printf("\n"); if (fw_parse(&fw, op.fw, 1)) printf("  (firmware file problem: a real run would abort here)\n"); }
        if (m5) {
            printf("\n");
            if (tplg_load(&tpbuf, op.tplg, 1) || pl_build(&tpbuf, &plbuf, 1)) printf("  (topology problem: a real run would abort here)\n");
            else g_m5plan = &plbuf;
        }
        uint32_t iss = 7, oss = 9, spb = 0x700, pp = 0x800; int live = 0;
        if (!offline && aud_open() == 0) {
            uint32_t n = 16;
            if (IOConnectCallScalarMethod(conn, kInfo, NULL, 0, o, &n) == 0 && o[15] >= need) {
                printf("\n");
                uint32_t f = m2_precheck(p, 1);
                if (f != 0xFFFFFFFFu && !(f & 0x87)) {
                    iss = (p[15] >> 16) & 0xF; oss = (p[15] >> 20) & 0xF; spb = p[15] & 0xFFFF; pp = (uint32_t)p[12]; live = 1;
                }
            } else printf("(loaded kext is interface v%llu, %s needs v%u (AudShim 0.%u.0): using the M1 dump values)\n", o[15], cmd, need, need);
        } else printf("(kext not opened: using the M1 dump values)\n");
        printf("\n");
        boot_plan(&op, iss, oss, spb, pp, live, m3 ? &fw : NULL);
        return 0;
    }

    if (aud_open()) return 1;
    { uint32_t n = 16; if (IOConnectCallScalarMethod(conn, kInfo, NULL, 0, o, &n) || o[15] < need) {
        fprintf(stderr, "%s needs AudShim 0.%u.0 (interface v%u); loaded kext reports v%llu\n", cmd, need, need, o[15]); return 1; } }

    if (state) {
        char *a[] = {"--state"};
        IOServiceClose(conn);
        cmd_m2(1, a);
        if (aud_open()) return 1;
        uint64_t q[16];
        if (m2call(M2B_INFO, 2, 0, q) == 0)
            printf("M2b mode %llu flags 0x%llx; snapshots: cmd 0x%04llx GCTL 0x%08llx PPCTL 0x%08llx EM2 0x%08llx LTRP 0x%02llx SPBFCCTL 0x%08llx SPIB 0x%08llx\n",
                   q[11], q[12], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
        m2b_regs("live M2b registers");
        if (m4 && m2call(M4_STATE, 0, 0, q) == 0) {
            static const char *stp[] = {"-", "shim", "soft-reset", "sync-armed", "cdns-init", "committed", "int-masks", "RUNNING"};
            printf("M4: mode %llu flags 0x%llx link_mask 0x%llx shim_mask 0x%llx steps:", (q[1] >> 24) & 1, q[1] & 0xFF, (q[1] >> 8) & 0xF, (q[1] >> 16) & 0xF);
            for (int k = 0; k < 4; k++) printf(" link%d=%s", k, stp[((q[2] >> (8 * k)) & 0xFF) < 8 ? ((q[2] >> (8 * k)) & 0xFF) : 0]);
            printf("\n");
        }
        if (m5 && m2call(M5_STATE, 0, 0, q) == 0 && q[0] == M2R_OK)
            printf("M5: mode %llu flags 0x%02llx (b0 IPC b1 ring b2 stream-prep b3 RUN b4 ports b5 amps b6 L1SEN-cleared b7 bank), %llu IPC(s),\n"
                   "    last cmd 0x%08llx error %d; ring IOVA 0x%llx phys 0x%llx; SD CTL 0x%08llx LPIB 0x%llx CBL 0x%llx BDL 0x%llx;\n"
                   "    EM2 0x%08llx PPCTL 0x%08llx; HIPCIDR/IDA 0x%016llx HIPCTDR 0x%08llx; MCP_STAT L1 0x%08llx L2 0x%08llx\n",
                   (q[1] >> 16) & 1, q[1] & 0xFF, q[2] & 0xFFFFFFFF, q[3], (int32_t)(q[2] >> 32), q[8], q[9], q[4], q[5], q[6], q[7],
                   q[10], q[11], q[12], q[13], q[14] & 0xFFFFFFFF, q[14] >> 32);
        if (m3 && m2call(M3_INFO, 0, 0, q) == 0)
            printf("M3: mode %llu, firmware buffer %s (%llu bytes, %llu segs, %llu BDLEs, FNV-1a 0x%08llx)\n",
                   (q[5] >> 2) & 1, (q[5] & 2) ? "allocated" : "none", q[1], q[2], q[3], q[4]);
        uint32_t cs;
        if (m3 && mmio(4, 0x04, &cs) == 0 && (cs >> 24) & 1) {
            uint32_t tdr = 0, tdd = 0; mmio(4, 0xC0, &tdr); mmio(4, 0xC8, &tdd);
            printf("-- firmware side (core 0 powered) --\n");
            fw_status_regs();
            printf("  HIPCTDR 0x%08x (BUSY=%u) HIPCTDD 0x%08x\n  uplink mailbox BAR4+0x81000:\n", tdr, tdr >> 31, tdd);
            hexdump(4, 0x81000, 32);
            if (m4) {
                uint32_t lc = 0;
                shim_dump("live");
                if (mmio(4, 0x2C004, &lc) == 0) for (int l = 0; l < 4; l++) if ((lc >> (8 + l)) & 1) cdns_dump(l);
            }
        }
        IOServiceClose(conn);
        return 0;
    }

    // ---------------- the real run
    printf("audctl 0.9.0  %s (attempts %d, ROM poll %u ms, ctrl-reset %s, iccmax %s, L1SEN %s, SSP %s, PIE %s%s%s, %s)\n",
           cmd, op.attempts, op.romms, op.ctrl ? "on" : "OFF", op.icc ? "on" : "OFF", op.l1 ? "on" : "OFF",
           op.ssp ? "WRITE" : "record-only", op.pie ? "on" : "OFF", m3 ? ", FW_READY wait " : "",
           m3 ? (op.post ? "+ post_fw_run" : "") : "", op.leave ? "leave-up" : "teardown");
    if (m4) printf("  SoundWire: link mask 0x%x, dev0 attach wait %u ms\n", op.links, op.attachms);
    if (m5) printf("  M5: %s; topology %s\n", op.noplay ? "M5a (--no-play): pipeline + amps set up and torn down, no audio" : "playback", op.tplg);
    if (m5 && !op.noplay) printf("  M5: %.1f s of a ~1 kHz sine at %.1f dBFS on %s (max 3 s / -12 dBFS)\n", op.secs, op.level,
                                 op.chan == 0 ? "both speakers" : op.chan == 1 ? "the left speaker only" : "the right speaker only");
    if (m3) {
        printf("\n");
        if (fw_parse(&fw, op.fw, 1)) { printf("\nABORT: firmware file unusable. Nothing was written.\n"); return 1; }
        printf("  FW_READY wait %u ms\n", op.readyms);
    }
    if (m5) {   // S40 happens before anything is armed: a bad topology aborts with nothing written
        printf("\n");
        if (tplg_load(&tpbuf, op.tplg, 1) || pl_build(&tpbuf, &plbuf, 1)) {
            printf("\nABORT: topology unusable for the speaker path. Nothing was written.\n");
            return 1;
        }
        g_m5plan = &plbuf;
    }
    printf("\n");
    uint32_t f = m2_precheck(p, 1);
    if (f == 0xFFFFFFFFu) return 1;
    if (f) {
        printf("\nABORT: pre-checks failed (mask 0x%x). Nothing was written.%s\n", f,
               (f & ((1u << 3) | (1u << 6) | (1u << 9) | (1u << 11))) ? "\n  A previous run may be left up: run `audctl m3 --undo` first." : "");
        return 1;
    }
    uint32_t iss = (p[15] >> 16) & 0xF, oss = (p[15] >> 20) & 0xF;
    printf("\n");
    boot_plan(&op, iss, oss, (uint32_t)(p[15] & 0xFFFF), (uint32_t)p[12], 1, m3 ? &fw : NULL);
    fflush(stdout); fsync(STDOUT_FILENO);

    printf("\n== ARM (%s) ==\n", MN);
    if (m2call(M2_ARM, m5 ? MAGIC_M5 : m4 ? MAGIC_M4 : m3 ? MAGIC_M3 : MAGIC_M2B, 0, o)) return 1;
    if (o[0] != M2R_OK) { printf("  ARM %s (fail mask 0x%llx, phase %llu). Nothing was written.\n", m2rc(o[0]), o[11], o[15]); return 1; }
    printf("  armed on this connection (snapshots taken in the kext)\n");
    m2b_regs("registers at ARM");

    const char *failstep = NULL;
    int rom_ok = 0, attempt = 0, ready_ok = 0, m4bad = 0, m5bad = 0;
    struct fwready fr;
    memset(&fr, 0, sizeof fr);
    char result[600] = "";
#define CHK(name) if (o[0] != M2R_OK) { failstep = name; goto fail; }
#define CALL(opc, a0, name) if (m2call(opc, a0, 0, o)) { failstep = name; goto fail; }

    m2b_step("[S1] PCI COMMAND |= BME | INTxDisable   (hda.c:891)");
    CALL(M2B_BME, 1, "S1");
    printf("  cmd 0x%04llx -> 0x%04llx  %s\n", o[1], o[2], m2rc(o[0])); CHK("S1 BME");

    m2b_step("[S2] cfg 0x44 TCSEL[2:0]=0   (hda.c:903)");
    CALL(M2_TCSEL, 0, "S2");
    printf("  before 0x%02llx after 0x%02llx  %s  %s\n", o[1], o[2], o[3] ? "written" : "no write (already 0)", m2rc(o[0])); CHK("S2 TCSEL");

    if (op.ctrl) {
        m2b_step("[S3] controller reset cycle (hda_dsp_ctrl_init_chip, hda-ctrl.c:186-292)");
        CALL(M2B_CTRLINIT, 0, "S3");
        printf("  GCTL 0x%08llx -> 0x%08llx  (CRST=0 seen after ~%llu us, CRST=1 after ~%llu us)  %s\n", o[1], o[5], o[3], o[4], m2rc(o[0]));
        printf("  PPCTL 0x%08llx -> 0x%08llx%s\n", o[2], o[6], ((o[6] >> 30) & 1) ? "" : "  (GPROCEN cleared by the reset; S4 sets it again)");
        printf("  STATESTS after reset 0x%04llx (bit2 = iDisp codec; 0 = display-audio link unpowered, irrelevant for the DSP)\n", o[7]);
        printf("  EM2 0x%08llx  LTRP 0x%08llx  ADSPCS 0x%08llx  ML link0 LCTL 0x%08llx link1 LCTL 0x%08llx  SPBFCCTL 0x%08llx SD(CL) 0x%08llx\n",
               o[8], o[9], o[10], o[11], o[12], o[13], o[14]);
        CHK("S3 controller reset");
    }

    m2b_step("[S4] PPCTL |= GPROCEN (| PIE)   (hda.c:912-913)");
    CALL(M2B_PPCAP, op.pie, "S4");
    printf("  PPCTL 0x%08llx -> 0x%08llx  %s\n", o[1], o[2], m2rc(o[0])); CHK("S4 PPCTL");

    m2b_step("[S5] pre_fw_run: ADSPDCGE=0, L1SEN=0, ADSPPGD=1   (hda.c:422-440 -> hda-ctrl.c:163-184)");
    CALL(M2_CGCTL, 0, "S5a"); printf("  cfg48 0x%02llx -> 0x%02llx  %s\n", o[1], o[2], m2rc(o[0])); CHK("S5 CGCTL");
    if (op.l1) { CALL(M2B_L1SEN, 0, "S5b"); printf("  EM2 0x%08llx -> 0x%08llx (L1SEN %llu)  %s\n", o[1], o[2], (o[2] >> 13) & 1, m2rc(o[0])); CHK("S5 L1SEN"); }
    CALL(M2_PGCTL, 0, "S5c"); printf("  cfg44 0x%02llx -> 0x%02llx  %s\n", o[1], o[2], m2rc(o[0])); CHK("S5 PGCTL");

    m2b_step("[S6] DMA buffer + IODMACommand (device mapper)");
    CALL(M2B_DMAALLOC, 0, "S6");
    if (o[0] == M2R_OK) {
        static const char *pg[] = {"CL BDL", "ICCMAX BDL", "CL data 0", "CL data 1", "ICCMAX data"};
        for (int i = 0; i < 5; i++) printf("  page %d %-12s IOVA 0x%012llx  phys 0x%012llx\n", i, pg[i], o[1 + i], o[6 + i]);
        printf("  %llu segment(s); %s\n", o[11], o[12] ? "IOVA == phys for all pages (no VT-d remap active: AppleVTD is off on this machine,\n"
                                                         "  so the DSP DMA sees physical addresses directly; this is the M2b-run result)"
                                                       : "remapped by the IOMMU");
    } else printf("  %s (IOReturn 0x%llx, segs %llu, offset %llu)\n", m2rc(o[0]), o[1], o[2], o[3]);
    CHK("S6 DMA");

    if (m3) {
        m2b_step("[S6b] firmware payload -> kernel DMA buffer   (loader.c:48-51, hda-loader.c:375-395, hda-stream.c:1282)");
        if (m2call(M3_FWLOAD, (uint64_t)(uintptr_t)(fw.data + fw.off), fw.psize, o)) { failstep = "S6b"; goto fail; }
        if (o[0] == M2R_OK) {
            printf("  %llu bytes copied, first dword 0x%08llx ('$CPD'), buffer %llu bytes, FNV-1a kernel 0x%08llx vs file 0x%08x: %s\n",
                   o[1], o[7], o[8], o[6], fw.hash, (uint32_t)o[6] == fw.hash ? "MATCH" : "MISMATCH");
            printf("  %llu DMA segment(s), %llu..%llu bytes each; seg0 IOVA 0x%llx phys 0x%llx; %s\n", o[2], o[10], o[9], o[3], o[4],
                   o[5] ? "IOVA == phys" : "remapped");
            if ((uint32_t)o[6] != fw.hash) { o[0] = M2R_VERIFY; }
        } else printf("  %s (detail 0x%llx 0x%llx 0x%llx; flags 0x%llx)\n", m2rc(o[0]), o[1], o[2], o[3], o[15]);
        CHK("S6b firmware copy");
    }

    if (op.icc) {
        m2b_step("[S7] ICCMAX capture stream SD0, started   (hda-loader.c:284-296, hda-stream.c:484-555)");
        CALL(M2B_ICCMAX, 0, "S7");
        printf("  SD0 CTL/STS 0x%08llx LPIB 0x%llx CBL 0x%llx LVI 0x%llx BDL 0x%016llx PPCTL 0x%08llx LTRP 0x%08llx  %s\n",
               o[1], o[2], o[3], o[7] & 0xFFFF, o[6], o[4], o[5], m2rc(o[0]));
        CHK("S7 ICCMAX");
    }

    m2b_step(m3 ? "[S8] code-loader stream (tag 1) with the firmware BDL + SPIB   (hda-stream.c:112-185, 563-755, 187-214)"
                : "[S8] code-loader stream (tag 1) + SPIB   (hda-stream.c:563-755, 187-214)");
    CALL(M2B_CLPREP, m3 ? 1 : 0, "S8");
    printf("  SD CTL/STS 0x%08llx (tag %llu) CBL 0x%llx LVI/FIFOW 0x%08llx FIFOS/FMT 0x%08llx BDL 0x%016llx\n"
           "  PPCTL 0x%08llx SPBFCCTL 0x%08llx SPIB 0x%08llx FIFOS %llu  BDLEs %llu (CBL target %llu)  %s%s\n",
           o[1], (o[1] >> 20) & 0xF, o[2], o[3], o[4], o[5], o[6], o[7] & 0xFFFFFFFF, o[7] >> 32, o[8], o[10], o[11], m2rc(o[0]),
           (o[9] & 0x40) ? " (stream reset timeout)" : "");
    if (m3 && o[0] == M2R_VERIFY && o[10] > 0x7fffffffULL) printf("  !! BDL build failed (%lld: -1 too many entries, -2 hole/misaligned)\n", (long long)o[10]);
    CHK("S8 CL stream");
    if (m3) {
        uint64_t q[16];
        if (m2call(M3_INFO, 0, 0, q) == 0 && q[0] == M2R_OK) {
            uint64_t nb = q[3], tot = 0;
            printf("  BDL (%llu entries; Linux period rule: %s):\n", nb, nb == 2 ? "one contiguous chunk -> two halves" : "one BDLE per contiguous chunk");
            for (uint64_t base = 0; base < nb; base += 5) {
                uint64_t r[16];
                if (m2call(M3_INFO, base, 0, r) || r[0] != M2R_OK) break;
                for (uint64_t k = 0; k < 5 && base + k < nb; k++) {
                    uint64_t len = r[7 + 2 * k] & 0xFFFFFFFF, ioc = r[7 + 2 * k] >> 32;
                    tot += len;
                    if (base + k < 3 || base + k + 3 > nb)
                        printf("    BDLE %3llu addr 0x%012llx len 0x%06llx ioc %llu\n", base + k, r[6 + 2 * k], len, ioc);
                    else if (base + k == 3) printf("    ...\n");
                }
            }
            printf("  BDL total %llu bytes (= payload %u: %s)\n", tot, fw.psize, tot == fw.psize ? "ok" : "MISMATCH");
            if (tot != fw.psize) { o[0] = M2R_VERIFY; failstep = "S8 BDL total"; goto fail; }
        }
    }
    m2b_regs("registers before the DSP is powered");

    for (attempt = 1; attempt <= op.attempts && !rom_ok; attempt++) {
        printf("\n==== boot attempt %d/%d (cl_dsp_init, hda-loader.c:66-182) ====\n", attempt, op.attempts);
        m2b_step("[S9] ADSPCS SPA0=1, poll CPA0   (hda-dsp.c:280-326)");
        CALL(M2_SPA, 0, "S9");
        print_adspcs("  before ", (uint32_t)o[1]); print_adspcs("  after  ", (uint32_t)o[2]);
        printf("  CPA0 after ~%llu us: %s\n", o[3], m2rc(o[0])); CHK("S9 SPA0/CPA0");
        m2b_step(op.ssp ? "[S10] SSP0-2 SSC1 |= CBP/CFP (real write, --ssp)   (hda-loader.c:34-49)"
                        : "[S10] SSP0-2 SSC1 recorded, no write (as in the successful M2b run)   (hda-loader.c:34-49)");
        CALL(M2B_SSP, op.ssp ? 1 : 0, "S10");
        for (int i = 0; i < 3; i++) printf("  SSP%d SSC1 0x%08llx -> 0x%08llx\n", i, o[1 + i], o[4 + i]);
        CHK("S10 SSP");
        m2b_step("[S11] HIPCIDR = 0x81004000 ROM_CONTROL|PURGE_FW, CL tag 1   (hda-loader.c:87-91)");
        CALL(M2_IPC, 0, "S11");
        printf("  HIPCIDR before 0x%08llx after 0x%08llx  HIPCIDA 0x%08llx  %s\n", o[1], o[2], o[4], m2rc(o[0])); CHK("S11 IPC");
        m2b_step("[S12] CRST0=0, CSTALL0=0   (hda-dsp.c:248-277)");
        CALL(M2_CRST, 0, "S12a");
        print_adspcs("  after CRST0=0   ", (uint32_t)o[2]); printf("  (~%llu us) %s\n", o[3], m2rc(o[0])); CHK("S12 CRST0");
        CALL(M2_CSTALL, 0, "S12b");
        print_adspcs("  after CSTALL0=0 ", (uint32_t)o[2]); printf("  enabled check: %s\n", m2rc(o[0])); CHK("S12 CSTALL0");
        m2b_step("[S13] poll HIPCIDA.DONE 500 ms, ack   (hda-loader.c:104-123)");
        CALL(M2_IPCDONE, 500, "S13");
        printf("  HIPCIDA seen 0x%08llx after ~%llu us -> after ack 0x%08llx; HIPCIDR 0x%08llx  %s\n", o[1], o[3], o[2], o[5], m2rc(o[0]));
        int done_ok = (o[0] == M2R_OK);
        m2b_step(done_ok ? "[S14] poll ROM status == INIT_DONE   (hda-loader.c:148-153)"
                         : "[S14] (no DONE) short ROM status sample for the record, 50 ms");
        if (m2call(M2_ROMPOLL, 1, done_ok ? op.romms : 50, o)) { failstep = "S14"; goto fail; }
        print_rom_poll("ROM poll", o);
        rom_ok = done_ok && (o[0] == M2R_OK);
        if (rom_ok) break;
        printf("\n  attempt %d failed (%s) -> state dump, then core 0 reset/power-down (hda-loader.c:165-178)\n",
               attempt, done_ok ? "no INIT_DONE" : "no DONE");
        m2_report(1);
        m2b_regs("stream/controller registers at the failure");
        CALL(M2B_COREOFF, 0, "core-off");
        print_adspcs("  core-off: ADSPCS ", (uint32_t)o[2]); printf("  HIPCIDR 0x%08llx HIPCIDA 0x%08llx  %s\n", o[3], o[4], m2rc(o[0]));
        CHK("core-off between attempts");
    }
    if (!rom_ok) { failstep = "ROM handshake (all attempts)"; goto fail_nodump; }

    if (m3) {
        m2b_step("[S15] code-loader DMA start (hda_cl_trigger START)   (hda-loader.c:237 -> 185-215)");
        CALL(M3_CLSTART, 0, "S15");
        printf("  CL SD CTL 0x%08llx -> 0x%08llx (RUN seen after ~%llu us)  LPIB 0x%llx -> 0x%llx  ROM status 0x%08llx  %s\n",
               o[1], o[2], o[3], o[6], o[4], o[5], m2rc(o[0]));
        CHK("S15 CL DMA start");

        m2b_step("[S16] poll ROM status == FW_ENTERED, 3000 ms   (hda-loader.c:243-262)");
        if (m2call(M2_ROMPOLL, 5, 3000, o)) { failstep = "S16"; goto fail; }
        print_rom_poll("FW_ENTERED poll", o);
        int entered = (o[0] == M2R_OK) || (o[15] & 1);
        uint64_t rs = o[1], re = o[2];

        m2b_step("[S17] CL DMA stop + cleanup, ICCMAX cleanup, LTRP   (hda-loader.c:264, 461, 305-317; hda-stream.c:463-480, 1333-1375)");
        CALL(M3_CLSTOP, 0, "S17");
        printf("  at stop: LPIB 0x%llx (%llu of %u bytes read by the DMA), SD STS 0x%02llx (BCIS=%llu FIFOE=%llu DESE=%llu)\n",
               o[2], o[2], fw.psize, o[10], (o[10] >> 2) & 1, (o[10] >> 3) & 1, (o[10] >> 4) & 1);
        printf("  CL CTL after stop 0x%08llx, after cleanup 0x%08llx; SPBFCCTL 0x%08llx SPIB 0x%08llx\n", o[1], o[5], o[3], o[4]);
        printf("  ICCMAX CTL after cleanup 0x%08llx  EM2 0x%08llx (L1SEN %llu)  LTRP 0x%08llx (GB byte 0x%02llx)  firmware buffer %s  %s%s\n",
               o[6], o[7], (o[7] >> 13) & 1, o[8], o[8] & 0xFF, o[11] ? "freed" : "KEPT (stream did not stop)", m2rc(o[0]),
               (o[9] & 2) ? " (ICCMAX RUN did not read 0 within 300 us)" : "");
        if (o[0] != M2R_OK && o[0] != M2R_VERIFY) { failstep = "S17 CL DMA stop"; goto fail; }
        if (!entered) {
            printf("\n  !! FW_ENTERED not reached (final 0x%08llx, error 0x%08llx %s). Linux: 'timeout with rom_status_reg'\n"
                   "     -> 'Firmware download failed' -> boot fails (hda-loader.c:256-259, 449-453).\n", rs, re, rom_error((uint32_t)re));
            failstep = "S16 FW_ENTERED"; o[0] = M2R_TIMEOUT; goto fail;
        }

        snprintf(result, sizeof result, "FW_READY wait %u ms", op.readyms);
        m2b_step("[S18] wait for the firmware's first message: HIPCTDR.BUSY   (loader.c:150-160, cnl.c:154-184)");
        CALL(M3_FWREADY, op.readyms, "S18");
        printf("  %s after ~%llu us: HIPCTDR 0x%08llx HIPCTDD 0x%08llx HIPCTDA 0x%08llx | HIPCIDR 0x%08llx HIPCIDA 0x%08llx\n"
               "  ADSPCS 0x%08llx ADSPIS 0x%08llx%s\n",
               o[0] == M2R_OK ? "message" : "NO message", o[3], o[1], o[2], o[4], o[5], o[6], o[9], o[10],
               (o[11] & 2) ? "  !! core 0 lost power" : "");
        print_fsr("  ROM/FW status ", (uint32_t)o[7]);
        if (o[0] != M2R_OK) {
            printf("  !! no FW_READY within %u ms. Linux: 'Firmware boot failure due to timeout' (loader.c:157-163)\n", op.readyms);
            failstep = "S18 FW_READY timeout"; goto fail;
        }
        uint32_t tdr = (uint32_t)o[1], tdd = (uint32_t)o[2];

        m2b_step("[S19] FW_READY message from the uplink mailbox   (ipc3.c:1049-1075 -> 817-858)");
        int rr = m3_fw_ready(tdr, tdd, &fw, &fr);
        ready_ok = (rr == 0);

        m2b_step("[S20] ack: HIPCTDR |= BUSY, HIPCTDA |= DONE   (cnl.c:184 -> 201-216)");
        CALL(M3_TDACK, 0, "S20");
        printf("  HIPCTDR 0x%08llx -> 0x%08llx   HIPCTDA 0x%08llx -> 0x%08llx  %s\n", o[1], o[2], o[3], o[4], m2rc(o[0]));
        CHK("S20 ack");
        if (!ready_ok) {
            failstep = rr == -2 ? "S19 firmware PANIC" : rr == -3 ? "S19 first message is not FW_READY"
                     : rr == -4 ? "S19 FW_READY failed (ABI)" : "S19 mailbox read";
            o[0] = M2R_VERIFY; goto fail;
        }
        // anything else queued right behind FW_READY? (read-only, short)
        if (m2call(M3_FWREADY, 0, 0, o) == 0 && o[0] == M2R_OK)
            printf("  note: another DSP->host message is already pending (HIPCTDR 0x%08llx); left for M4 (rollback acks it)\n", o[1]);

        if (m4) {   // hda_dsp_post_fw_run order: hda_sdw_startup first (hda.c:450), gating after (hda.c:471)
            char sum[512];
            int r4 = m4_sdw(&op, sum, sizeof sum, &failstep, o);
            if (r4 < 0) goto fail;
            if (m2call(M3_FWREADY, 0, 0, o) == 0 && o[0] == M2R_OK)
                printf("  note: a DSP->host message is pending (HIPCTDR 0x%08llx); not handled in M4 (rollback acks it)\n", o[1]);
            snprintf(result, sizeof result, "%s", sum);
            m4bad = (r4 != 0);
            if (m5) {
                if (m4bad) { failstep = "S35 enumeration differs from the DSDT (M5 needs RT1316 dev 1 on links 1 and 2)"; o[0] = M2R_VERIFY; goto fail; }
                char s5[512];
                int r5 = m5_run(&op, &fw, &plbuf, &failstep, o, s5, sizeof s5);
                if (r5 < 0) goto fail;
                m5bad = (r5 != 0);
                snprintf(result, sizeof result, "%s", s5);
            }
        }
        if (op.post) {
            m2b_step("[S21] post_fw_run: clock/power gating back on   (hda.c:471 -> hda-ctrl.c:163-184)");
            CALL(M3_POSTFW, 0, "S21");
            printf("  cfg48 0x%02llx -> 0x%02llx  EM2 0x%08llx (L1SEN %llu)  cfg44 0x%02llx -> 0x%02llx  ADSPCS 0x%08llx  %s\n",
                   o[1] >> 8, o[1] & 0xFF, o[2], (o[2] >> 13) & 1, o[3] >> 8, o[3] & 0xFF, o[4], m2rc(o[0]));
            CHK("S21 post_fw_run");
        }
        if (!m4)
            snprintf(result, sizeof result, "FW_READY: SOF firmware %u.%u.%u-%s build %u, ABI %u.%u.%u, flags 0x%llx, ROM attempt %d",
                     fr.maj, fr.min, fr.mic, fr.tag, fr.build, fr.abi >> 24, (fr.abi >> 12) & 0xfff, fr.abi & 0xfff,
                     (unsigned long long)fr.flags, attempt);
        else
            printf("\n  firmware: SOF %u.%u.%u-%s build %u, ABI %u.%u.%u (ROM attempt %d)\n", fr.maj, fr.min, fr.mic, fr.tag, fr.build,
                   fr.abi >> 24, (fr.abi >> 12) & 0xfff, fr.abi & 0xfff, attempt);
    }

    m2b_step(m3 ? "[S22] report" : "[S15] report");
    m2_report(1);
    if (m3) fw_status_regs();
    m2b_regs(m3 ? "registers with the firmware running" : "registers at ROM INIT_DONE");
    if (op.leave) {
        m2call(M2_FINISH, 0, 0, o);
        if (m4)
            printf("\nM4 RESULT: %s (%s). LEFT UP (finish: %s): firmware RUNNING, SoundWire links 0x%x powered and\n"
                   "  running (bus clock 4.8 MHz, 48 kHz frames), devices keep their numbers. BME on.\n"
                   "  Run `audctl m4 --undo` BEFORE sleep/shutdown/kext unload.\n",
                   m4bad ? "INCOMPLETE (enumeration differs from the DSDT)" : "SUCCESS", result, m2rc(o[0]), op.links);
        else if (m3)
            printf("\nM3 RESULT: SUCCESS (%s). LEFT UP (finish: %s): the firmware is RUNNING on core 0, BME on.\n"
                   "  Run `audctl m3 --undo` BEFORE sleep/shutdown/kext unload (a sleep with the DSP up is untested).\n",
                   result, m2rc(o[0]));
        else
            printf("\nM2b RESULT: SUCCESS (ROM INIT_DONE on attempt %d). LEFT UP (finish: %s): core 0 at the ROM,\n"
                   "  ICCMAX running, CL stream prepared, BME on. Dump with `audctl m1`, then `audctl m2b --undo` (before sleep).\n",
                   attempt, m2rc(o[0]));
        IOServiceClose(conn);
        free(fw.data);
        return 0;
    }
    { int r = m2_rollback("teardown after success");
      m2_report(0);
      IOServiceClose(conn);
      free(fw.data);
      if (m5) {
          printf("\nM5 RESULT: %s (%s), then torn down: %s\n", m5bad ? "FAILURE (see the S50/S54 notes)" : "SUCCESS", result,
                 r == 0 ? "M1 state" : "TEARDOWN INCOMPLETE (run audctl m5 --undo)");
          if (!op.noplay && !m5bad) printf("  -> did you hear a ~1 kHz tone from %s? Please note it next to this log.\n",
                                          op.chan == 0 ? "both speakers" : op.chan == 1 ? "the left speaker" : "the right speaker");
          return m5bad ? 1 : 0;
      }
      if (m4) printf("\nM4 RESULT: %s (%s), then torn down: %s\n", m4bad ? "FAILURE (enumeration differs from the DSDT)" : "SUCCESS",
                     result, r == 0 ? "M1 state" : "TEARDOWN INCOMPLETE (run audctl m4 --undo)");
      else if (m3) printf("\nM3 RESULT: SUCCESS (%s), then torn down: %s\n", result, r == 0 ? "M1 state" : "TEARDOWN INCOMPLETE (run audctl m3 --undo)");
      else printf("\nM2b RESULT: SUCCESS (ROM INIT_DONE on attempt %d), then torn down: %s\n", attempt, r == 0 ? "M1 state" : "TEARDOWN INCOMPLETE (run audctl m2b --undo)");
      return (m4 && m4bad) ? 1 : 0; }

fail:
    printf("\n!! %s FAILED at %s (%s)\n", MN, failstep, m2rc(o[0]));
    m2_report(1);
    if (m3) { uint32_t cs; if (mmio(4, 0x04, &cs) == 0 && (cs >> 24) & 1) {
        fw_status_regs();
        printf("  uplink mailbox BAR4+0x81000 (first 0x40 bytes):\n"); hexdump(4, 0x81000, 16);
        uint32_t esz, eo = find_window(fw.nwin, fw.win, 6, &esz);
        if (eo) { printf("  exception window BAR4+0x%05x (first 0x40 bytes):\n", eo); hexdump(4, eo, 16); }
        if (m4) {
            uint32_t lc = 0;
            shim_dump("at the failure");
            if (mmio(4, 0x2C004, &lc) == 0) for (int l = 0; l < 4; l++) if ((lc >> (8 + l)) & 1) cdns_dump(l);
        } } }
    m2b_regs("registers at the failure");
fail_nodump:
    if (!failstep) failstep = "?";
    { int r = m2_rollback("failure path");
      m2_report(0);
      m2b_regs("registers after rollback");
      IOServiceClose(conn);
      free(fw.data);
      printf("\n%s RESULT: FAILURE at %s; rolled back%s.\n", MN, failstep,
             r == 0 ? " (M1 state)" : m5 ? " INCOMPLETE: run `audctl m5 --undo`" : m4 ? " INCOMPLETE: run `audctl m4 --undo`" : m3 ? " INCOMPLETE: run `audctl m3 --undo`" : " INCOMPLETE: run `audctl m2b --undo`"); }
    return 1;
#undef CHK
#undef CALL
}

// ------------------------------------------------------------------ M7: RT714 microphones (capture)
// References (besides the M5 ones):
//   [R7]  sound/soc/codecs/rt715-sdca.c (RT714 = rt715-sdca, part 0x714, SDCA function 2 = MIC_ARRAY)
//   [R7S] sound/soc/codecs/rt715-sdca-sdw.c (source ports 0x50 = DP4 + DP6, simple_ch_prep_sm, paging)
//   [R7H] sound/soc/codecs/rt715-sdca.h      [MBQ] drivers/base/regmap/regmap-sdw-mbq.c
//   [UCM7] alsa-ucm-conf ucm2/codecs/rt715-sdca/init.conf + ucm2/sof-soundwire/rt715-sdca.conf
//   [SU]  sound/soc/sdw_utils/soc_sdw_utils.c: rt714 -> dai "rt715-sdca-aif2" (DP4), SOC_SDW_DMIC_DAI_ID
// Copies of the files used are in ~/xe/build/mic/ref/.
//
// The sequence is a table so that the kext (driver mode, AudShimData.h) and `audctl rec --dry-run` use the same bytes.
// kinds: W8 = sdw_write; U8 = sdw_update (read + write (v & ~mask) | val); C8 = read, write `mask` back if any of
// its bits is set (w1c); WQ = 16-bit MBQ write (MSB to reg | 0x2000 first, then LSB to reg, [MBQ]:75-92); UQ = MBQ
// read (LSB at reg, MSB at reg | 0x2000, [MBQ]:128-151) + MBQ write of (v & ~mask) | val; RQ = MBQ read, kept as the
// hardware version (v & 0xf). cond 1/2: only when hw_ver == 0 / 1 ([R7] rt715_sdca_io_init).
// phases: 1 = attach (sdw_initialize_slave + rt715_sdca_io_init + UCM BootSequence), 2 = BE hw_params,
//         3 = power up (UCM EnableSequence + DAPM PDE23_24 POST_PMU), 4 = power down (PRE_PMD + UCM DisableSequence)
enum { MK_W8 = 1, MK_U8 = 2, MK_C8 = 3, MK_WQ = 4, MK_UQ = 5, MK_RQ = 6 };
struct micop { uint8_t phase, kind, cond; uint32_t reg; uint16_t mask, val; const char *what; };
#define FUN_MIC 2
// SDW_SDCA_CTL as a constant expression ([SR]:335-342, same as sdca())
#define SDCA(fun, ent, ctl, ch) ((1u << 30) | (((fun) & 7u) << 22) | (((ent) & 0x40u) << 15) | (((ent) & 0x3Fu) << 7) | \
                                 (((ctl) & 0x30u) << 15) | (((ctl) & 0xFu) << 3) | (((ch) & 0x38u) << 12) | ((ch) & 7u))
static const struct micop mic_ops[] = {
    {1, MK_W8, 0, 0x4D, 0, 1, "SCP_BusClock_Base = 1 (sdw_slave_set_frequency, bus.c:1377-1417)"},
    {1, MK_W8, 0, 0x62, 0, 3, "SCP_BusClock_Scale_B0 = 3"},
    {1, MK_W8, 0, 0x72, 0, 3, "SCP_BusClock_Scale_B1 = 3"},
    {1, MK_C8, 0, 0x40, 0x02, 0, "SCP_Int1: clear BUS_CLASH if set (bus.c:1430-1447)"},
    {1, MK_U8, 0, 0x41, 0x00, 0, "SCP_IntMask1 |= scp_int1_mask (0 for rt715-sdca: read + same write, bus.c:1475-1483)"},
    {1, MK_RQ, 0, 0x2000000, 0, 0, "VENDOR_REG(0x20) PRODUCT_NUM: hw_ver = value & 0xf ([R7] io_init)"},
    {1, MK_W8, 0, SDCA(FUN_MIC, 0x10, 0x01, 0), 0, 1, "SDCA CX_CLK_SEL (ent 0x10 ctl 1) = 1: external clock"},
    {1, MK_UQ, 1, 0x2000036, 0x54, 0x54, "AD_FUNC_EN |= 0x54 (hw_ver 0: GPIO4/5/6 as DMIC3/4)"},
    {1, MK_UQ, 2, 0x2000036, 0x55, 0x55, "AD_FUNC_EN |= 0x55 (hw_ver 1)"},
    {1, MK_UQ, 2, 0x2000037, 0x40, 0x40, "REV_1 |= 0x40 (hw_ver 1)"},
    {1, MK_UQ, 0, 0x2000044, 0x01, 0x01, "DFLL_VAD |= 1: DFLL calibration trigger"},
    {1, MK_W8, 0, SDCA(FUN_MIC, 0x05, 0x10, 0), 0, 2, "SDCA SMPU_TRIG_ST_EN (ent 0x05 ctl 0x10) = 2: trigger mode VAD"},
    {1, MK_U8, 0, 0x5E, 0x01, 0x01, "RT715_INT_MASK (SDCA IntMask3) |= 1: SMPU-1"},
    {1, MK_U8, 0, SDCA(FUN_MIC, 0x0A, 0x01, 1), 0x01, 0x01, "FU0A (ADC7/27) mute ch1 = 1 ([UCM7] 'FU0A Capture Switch' 0)"},
    {1, MK_U8, 0, SDCA(FUN_MIC, 0x0A, 0x01, 2), 0x01, 0x01, "FU0A mute ch2 = 1"},
    {1, MK_UQ, 0, 0x6100000, 0xF000, 0x6000, "HDA_LEGACY_MUX_CTL1: ADC 22 Mux = DMIC3 ([UCM7])"},
    {1, MK_UQ, 0, 0x6100000, 0x0F00, 0x0700, "HDA_LEGACY_MUX_CTL1: ADC 23 Mux = DMIC4 ([UCM7])"},
    {1, MK_U8, 0, SDCA(FUN_MIC, 0x02, 0x01, 1), 0x01, 0x00, "FU02 (ADC8/9) unmute ch1 ([UCM7] 'FU02 Capture Switch' 1)"},
    {1, MK_U8, 0, SDCA(FUN_MIC, 0x02, 0x01, 2), 0x01, 0x00, "FU02 unmute ch2"},
    {1, MK_U8, 0, SDCA(FUN_MIC, 0x02, 0x01, 3), 0x01, 0x00, "FU02 unmute ch3"},
    {1, MK_U8, 0, SDCA(FUN_MIC, 0x02, 0x01, 4), 0x01, 0x00, "FU02 unmute ch4"},
    {1, MK_WQ, 0, SDCA(FUN_MIC, 0x02, 0x02, 1), 0, 0x0000, "FU02 volume ch1 = 0x0000 = 0 dB ([UCM7] 'FU02 Capture Volume' 47)"},
    {1, MK_WQ, 0, SDCA(FUN_MIC, 0x02, 0x02, 2), 0, 0x0000, "FU02 volume ch2 = 0 dB"},
    {1, MK_WQ, 0, SDCA(FUN_MIC, 0x02, 0x02, 3), 0, 0x0000, "FU02 volume ch3 = 0 dB"},
    {1, MK_WQ, 0, SDCA(FUN_MIC, 0x02, 0x02, 4), 0, 0x0000, "FU02 volume ch4 = 0 dB"},
    {2, MK_WQ, 0, 0x2000039, 0, 0xaf00, "SDW_INPUT_SEL = 0xaf00 (rt715_sdca_pcm_hw_params, AIF2 = DP4)"},
    {2, MK_W8, 0, SDCA(FUN_MIC, 0x01, 0x10, 0), 0, 0x09, "SDCA CS_FREQ_IND (ent 0x01 ctl 0x10) = 0x9: 48 kHz"},
    {3, MK_U8, 0, SDCA(FUN_MIC, 0x02, 0x01, 1), 0x01, 0x00, "FU02 unmute ch1 ([UCM7] EnableSequence)"},
    {3, MK_U8, 0, SDCA(FUN_MIC, 0x02, 0x01, 2), 0x01, 0x00, "FU02 unmute ch2"},
    {3, MK_U8, 0, SDCA(FUN_MIC, 0x02, 0x01, 3), 0x01, 0x00, "FU02 unmute ch3"},
    {3, MK_U8, 0, SDCA(FUN_MIC, 0x02, 0x01, 4), 0x01, 0x00, "FU02 unmute ch4"},
    {3, MK_W8, 0, SDCA(FUN_MIC, 0x18, 0x01, 0), 0, 0x00, "SDCA PDE23_24 REQ_POWER (ent 0x18 ctl 1) = PS0 (DAPM POST_PMU)"},
    {4, MK_W8, 0, SDCA(FUN_MIC, 0x18, 0x01, 0), 0, 0x03, "SDCA PDE23_24 REQ_POWER = PS3 (DAPM PRE_PMD)"},
    {4, MK_U8, 0, SDCA(FUN_MIC, 0x02, 0x01, 1), 0x01, 0x01, "FU02 mute ch1 ([UCM7] DisableSequence)"},
    {4, MK_U8, 0, SDCA(FUN_MIC, 0x02, 0x01, 2), 0x01, 0x01, "FU02 mute ch2"},
    {4, MK_U8, 0, SDCA(FUN_MIC, 0x02, 0x01, 3), 0x01, 0x01, "FU02 mute ch3"},
    {4, MK_U8, 0, SDCA(FUN_MIC, 0x02, 0x01, 4), 0x01, 0x01, "FU02 mute ch4"},
};
#define NMICOPS ((int)(sizeof mic_ops / sizeof mic_ops[0]))
static uint32_t mic_reg(int i) { return mic_ops[i].reg; }
static const char *mic_kind(int k) {
    static const char *n[] = {"?", "W8", "U8", "C8", "WQ", "UQ", "RQ"};
    return k < 7 ? n[k] : "?";
}
// how a 32-bit address goes out on the command FIFO (cdns_program_scp_addr [CD]:677-768, SCP_AddrPage1/2)
static void mic_addr(char *b, size_t n, uint32_t a) {
    if (a < 0x8000) snprintf(b, n, "reg 0x%04x", a);
    else snprintf(b, n, "page1 0x%02x page2 0x%02x reg 0x%04x", (a >> 23) & 0xFF, (a >> 15) & 0xFF, (a & 0x7FFF) | 0x8000);
}
static void mic_print_phase(int phase, int dev) {
    char ab[64], ab2[64];
    for (int i = 0; i < NMICOPS; i++) {
        const struct micop *m = &mic_ops[i];
        if (m->phase != phase) continue;
        uint32_t a = mic_reg(i);
        mic_addr(ab, sizeof ab, a);
        const char *cond = m->cond == 1 ? " [only if hw_ver == 0]" : m->cond == 2 ? " [only if hw_ver == 1]" : "";
        switch (m->kind) {
        case MK_W8: printf("    L0 dev %d W  0x%08x = 0x%02x          (%s)  %s%s\n", dev, a, m->val, ab, m->what, cond); break;
        case MK_U8: printf("    L0 dev %d RW 0x%08x &~0x%02x |0x%02x    (%s)  %s%s\n", dev, a, m->mask, m->val, ab, m->what, cond); break;
        case MK_C8: printf("    L0 dev %d R  0x%08x, W 0x%02x if set  (%s)  %s%s\n", dev, a, m->mask, ab, m->what, cond); break;
        case MK_WQ: case MK_UQ: case MK_RQ:
            mic_addr(ab2, sizeof ab2, a | 0x2000);
            if (m->kind == MK_RQ) printf("    L0 dev %d R  0x%08x (LSB, %s) + 0x%08x (MSB, %s)  %s\n", dev, a, ab, a | 0x2000, ab2, m->what);
            else printf("    L0 dev %d %s 0x%08x %s0x%04x: %sW 0x%08x = MSB (%s), W 0x%08x = LSB  %s%s\n", dev,
                        m->kind == MK_UQ ? "RMW" : "W  ", a, m->kind == MK_UQ ? "mask " : "", m->kind == MK_UQ ? m->mask : m->val,
                        m->kind == MK_UQ ? "read LSB+MSB, " : "", a | 0x2000, ab2, a, m->what, cond);
            break;
        }
    }
}

// RT714 DP4 (slave) port registers, SDW_DPN_* with n = 4 ([SR]): base 0x400
#define MIC_DP 4u
static uint32_t dpn(uint32_t off) { return MIC_DP * 0x100 + off; }

// The microphone capture plan: what the kext driver mode (AudShim 0.9.0, interface v9) does for CAP_START / CAP_STOP,
// in the Linux order (BE hw_params, FE hw_params, BE prepare (sdw_prepare_stream), DAPM power-up, BE trigger
// (sdw_enable_stream), FE trigger (DMA RUN, TRIG_START); stop in reverse; [SPC]/[SP]/[ST2]).
static void mic_plan_print(const struct m5plan *cp, int hex) {
    struct sdwxp x; m5_compute(&x, 0);
    const struct m5dai *d = &cp->dai[0];
    uint32_t l = d->link, pdi = d->pdi, cb = 0x30000u + 0x10000u * l;
    printf("== audctl rec plan (DRY RUN: nothing below is executed; the kext does it on CAP_START / CAP_STOP) ==\n\n");
    printf("0. hardware up (only if no driver connection has it up yet): the M6 bring-up (M3 firmware, M4 links 0-3 + enumeration,\n"
           "   M5 speaker set-up). Enumeration must find RT714 = 025D:0714 on link 0 (DSDT SWD0 _ADR 0x000030025D071401);\n"
           "   the kext records its device number (expected 1).\n\n");
    printf("1. RT714 attach-time init, once per bring-up (sdw_initialize_slave, rt715_sdca_io_init, UCM BootSequence):\n");
    mic_print_phase(1, 1);
    printf("\n2. SOF capture pipeline %s (pcm %u '%s'), once per bring-up: widget set-up in the Linux order\n", cp->dai[0].wname, cp->pcm_id, cp->pcm_name);
    for (int i = 0; i < cp->n; i++) {
        printf("   IPC %2d %-18s %3u B  %s\n", i + 1, ipc_cmd_name(le32(cp->m[i].b + 4)), cp->m[i].len, cp->m[i].what);
        if (hex) pl_hex(&cp->m[i], "        ");
    }
    printf("\n3. BE hw_params (intel_hw_params -> intel_pdi_shim_configure / intel_pdi_alh_configure / sdw_cdns_config_stream,\n"
           "   [IS]:625-675, [CD] sdw_cdns_config_stream) for %s: link %u PDI %u, ALH stream %u, RX (capture):\n", d->wname, l, pdi, d->alh_id);
    printf("    BAR4+0x%05x (16) PCMSyCHM(%u,%u) = 0x%04x  (STREAM %u << 8 | HCHN %u << 4 | LCHN 0 | DIR(RX) bit 15)\n",
           0x2C000 + 0x22 + 0x60 * l + 2 * pdi, l, pdi, (d->alh_id << 8) | ((d->ch - 1) << 4) | 0x8000, d->alh_id, d->ch - 1);
    printf("    BAR4+0x%05x (32) ALH STRMZCFG(%u): DMAT = 3, CHN = %u (read-modify-write)\n", 0x2C800 + 4 * d->alh_id, d->alh_id, d->ch - 1);
    printf("    BAR4+0x%05x (32) CDNS PORTCTRL(%u): DIRN = 1 (RX), TEST_FAILED 0, BULK 0 (read-modify-write)\n", cb + 0x130 + pdi * 0x80, pdi);
    printf("    BAR4+0x%05x (32) CDNS PDI_CONFIG(%u) = 0x%08x (port %u | ch mask 0x%x << 8 | SOFT_RESET bit 24)\n", cb + 0x1100 + pdi * 16, pdi,
           pdi | (1u << 24) | (x.chmask << 8), pdi, x.chmask);
    printf("   RT714 hw_params (port DP%u, [R7] rt715_sdca_pcm_hw_params):\n", MIC_DP);
    mic_print_phase(2, 1);
    printf("\n4. FE hw_params: host capture stream SD0 (first input stream, tag 1; hda_dsp_stream_hw_params [HST]:563-755):\n"
           "    EM2.L1SEN = 0 if set; PPCTL |= PROCEN(0); SD0 CTL RUN/int off, STS clear, SRST cycle, BDLPL/U = 0,\n"
           "    SD0 CTL tag = 1, CBL = 65536, FORMAT = 0x0041 (48 kHz, 32 bit, 2 ch), LVI = 15, BDLPL/U = ring BDL (16 x 4096 B, IOC),\n"
           "    SD0 CTL int-enable bits, SPIB(0) off. Ring: 64 KiB contiguous < 4 GiB + BDL + SOF page table (kext memory).\n");
    printf("   IPC %-18s %3u B  %s\n", "STREAM_PCM_PARAMS", cp->pcm_params.len, cp->pcm_params.what);
    if (hex) pl_hex(&cp->pcm_params, "        ");
    printf("\n5. BE prepare = sdw_prepare_stream on link %u ([ST2]:666-752): program bank 1, then bank switch to 1\n", l);
    for (int bank = 1; bank >= 0; bank--) {
        if (bank == 0) printf("\n6. DAPM power-up, then BE trigger START = sdw_enable_stream: program bank 0 + enable ports, bank switch to 0\n");
        if (bank == 0) mic_print_phase(3, 1);
        printf("    L0 dev 1 W  0x%04x = 3       SCP_BusClock_Scale_B%d\n", bank ? 0x72 : 0x62, bank);
        printf("    L0 dev 1 RW 0x%04x &~0x0f    DP%u_PortCtrl: data mode 0, flow mode 0\n", dpn(0x02), MIC_DP);
        printf("    L0 dev 1 W  0x%04x = %u      DP%u_BlockCtrl1: word length - 1\n", dpn(0x03), x.bps - 1, MIC_DP);
        printf("    L0 dev 1 W  0x%04x = 0x%02x    DP%u_SampleCtrl1_B%d = (si %u - 1) & 0xff\n", dpn(bank ? 0x32 : 0x22), (x.si - 1) & 0xFF, MIC_DP, bank, x.si);
        printf("    L0 dev 1 W  0x%04x = %u       DP%u_OffsetCtrl1_B%d\n", dpn(bank ? 0x34 : 0x24), x.offset1, MIC_DP, bank);
        printf("    L0 dev 1 W  0x%04x = %u       DP%u_OffsetCtrl2_B%d\n", dpn(bank ? 0x35 : 0x25), x.offset2, MIC_DP, bank);
        printf("    L0 dev 1 W  0x%04x = 0       DP%u_BlockCtrl3_B%d\n", dpn(bank ? 0x37 : 0x27), MIC_DP, bank);
        printf("    L0 dev 1 W  0x%04x = %u       DP%u_SampleCtrl2_B%d = (si - 1) >> 8\n", dpn(bank ? 0x33 : 0x23), (x.si - 1) >> 8, MIC_DP, bank);
        printf("    L0 dev 1 W  0x%04x = 0x%02x    DP%u_HCtrl_B%d = hstart %u << 4 | hstop %u\n", dpn(bank ? 0x36 : 0x26), (x.hstart << 4) | x.hstop, MIC_DP, bank, x.hstart, x.hstop);
        uint32_t cfg = bank ? 0x218 : 0x200;
        printf("    BAR4+0x%05x CDNS DP%u B%d CONFIG: BGC 0, BPM 0, then WL %u, flow 0, data 0\n", cb + cfg, pdi, bank, x.bps - 1);
        printf("    BAR4+0x%05x CDNS DP%u B%d OFFSET_CTRL = 0x%x\n", cb + cfg + 0x0C, pdi, bank, x.offset1 | (x.offset2 << 8));
        printf("    BAR4+0x%05x CDNS DP%u B%d HCTRL = 0x%02x\n", cb + cfg + 0x10, pdi, bank, (x.hstart << 4) | x.hstop);
        printf("    BAR4+0x%05x CDNS DP%u B%d SAMPLE_CTRL = %u\n", cb + cfg + 0x08, pdi, bank, x.si - 1);
        printf("    BAR4+0x%05x CDNS CLK_CTRL%d MCLKD = 3\n", cb + (bank ? 0x38 : 0x30), bank);
        if (bank == 0) {
            printf("    L0 dev 1 W  0x%04x = 0x%x     DP%u_ChannelEn_B0 = ch mask\n", dpn(0x20), x.chmask, MIC_DP);
            printf("    BAR4+0x%05x CDNS DP%u B0 CH_EN = 0x%x\n", cb + 0x204, pdi, x.chmask);
        }
        printf("    bank switch (kext op 49, link mask 0x%x): SYNC.CMDSYNC(%u), deferred broadcast SCP_FrameCtrl_B%d (0x%02x) = 0x09 with the\n"
               "      SSP tag, SYNCGO, response polled (3 s)\n", 1u << l, l, bank, bank ? 0x70 : 0x60);
    }
    printf("\n7. FE trigger START: SD0 CTL RUN = 1 (host DMA, DSP -> memory), then\n");
    printf("   IPC %-18s %3u B  %s\n", "STREAM_TRIG_START", cp->trig_start.len, cp->trig_start.what);
    printf("   capture runs: the HAL / audctl read the 64 KiB ring behind DPIB (BAR0+0x%x = SD0 DPIB), method 7 (lock-free)\n", 0x1084);
    printf("\n8. CAP_STOP (reverse): IPC %s; SD0 RUN = 0; IPC %s; SD0 hw_free (SRST, PPCTL PROCEN(0) = 0);\n", cp->trig_stop.what, cp->pcm_free.what);
    mic_print_phase(4, 1);
    printf("    disable: DP%u_ChannelEn_B1 = 0, CDNS CH_EN B1 = 0, program bank 1, bank switch 1; DP%u_ChannelEn_B0 = 0, CH_EN B0 = 0,\n"
           "    program bank 0, bank switch 0 (sdw_disable_stream + sdw_deprepare_stream). The capture widgets stay set up until\n"
           "    the hardware is torn down; then (best effort, before the DSP is powered off):\n", MIC_DP, MIC_DP);
    for (int i = 0; i < cp->nf; i++) printf("   IPC %2d %-18s %3u B  %s\n", i + 1, ipc_cmd_name(le32(cp->f[i].b + 4)), cp->f[i].len, cp->f[i].what);
    printf("\n9. rollback on any failure / client death: SD0 RUN = 0, RT714 PS3 + FU02 muted, link 0 port 2 CH_EN = 0 (both banks),\n"
           "   then the M5/M4/M3/M2b rollback (all writes through the same kext whitelists).\n");
}

// ------------------------------------------------------------------ M6: gen-header (static data for the kext's driver mode)
// Writes a C header with everything the kext needs to run the M3 -> M5 sequence by itself (AudShim 0.8.0
// "driver mode"): the firmware payload (the .ri file minus its SOF extended manifest), the IPC3 messages that
// pl_build() generates from the topology (set-up, PCM params/free, trigger, free, volume template), the speaker
// DAI facts, the SoundWire transport parameters (m5_compute) and the RT1316 init table. Both paths stay: audctl
// m5 keeps driving the kext op by op; the kext driver mode replays exactly these bytes.
static void gh_escape(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) { if (*s == '"' || *s == '\\') fputc('\\', f); fputc((unsigned char)*s >= 32 && (unsigned char)*s < 127 ? *s : '?', f); }
    fputc('"', f);
}
static void gh_msg(FILE *f, const char *name, const struct ipcmsg *m) {
    fprintf(f, "static const uint32_t %s[%u] = {", name, (m->len + 3) / 4);
    for (uint32_t i = 0; i < m->len; i += 4) fprintf(f, "%s0x%08x", i ? (i % 32 ? ", " : ",\n    ") : "\n    ", le32(m->b + i));
    fprintf(f, "\n};\n");
}
static int cmd_genheader(int argc, char **argv) {
    char defpath[1024], tdef[1024];
    const char *home = getenv("HOME");
    snprintf(defpath, sizeof defpath, "%s/xe/build/audio/fw/use/sof-rpl.ri", home ? home : "");
    snprintf(tdef, sizeof tdef, "%s/xe/build/audio/fw/use/sof-rpl-rt1316-l12-rt714-l0.tplg", home ? home : "");
    const char *out = "AudShimData.h", *fwp = defpath, *tpp = tdef;
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "--fw=", 5)) fwp = argv[i] + 5;
        else if (!strncmp(argv[i], "--tplg=", 7)) tpp = argv[i] + 7;
        else out = argv[i];
    }
    static struct fwfile fw; static struct tplg tp; static struct m5plan pl, cp;
    if (fw_parse(&fw, fwp, 0)) { fprintf(stderr, "gen-header: firmware %s unusable\n", fwp); return 1; }
    if (tplg_load(&tp, tpp, 0) || pl_build(&tp, &pl, 0)) { fprintf(stderr, "gen-header: topology %s unusable\n", tpp); return 1; }
    if (pl_build_dir(&tp, &cp, 0, 1)) { fprintf(stderr, "gen-header: no usable microphone (capture) path in %s\n", tpp); return 1; }
    if (!pl.vol_0db.len || pl.vol_ch < 1 || pl.vol_ch > 8) { fprintf(stderr, "gen-header: no PGA volume control\n"); return 1; }
    struct sdwxp x; m5_compute(&x, 0);
    FILE *f = fopen(out, "w");
    if (!f) { perror(out); return 1; }
    uint32_t tfnv = 0;
    { FILE *t = fopen(tpp, "rb"); if (t) { static uint8_t b[1 << 20]; size_t n = fread(b, 1, sizeof b, t); fclose(t); tfnv = fnv1a(b, n); } }
    fprintf(f, "// AudShimData.h: GENERATED by `audctl gen-header` (audctl 0.9.0). Do not edit.\n"
               "// firmware: %s (%zu bytes, payload %u bytes at offset %u, payload FNV-1a 0x%08x)\n"
               "//   Intel SOF firmware, redistributable under LICENCE.Intel (~/xe/build/audio/fw/use/LICENCE.Intel)\n"
               "// topology: %s (FNV-1a 0x%08x), speaker PCM '%s' (pcm %u)\n"
               "// Consumed by AudShim.cpp (driver mode, M6). Regenerate after changing either file:\n"
               "//   audctl gen-header tools/AudShim/AudShimData.h\n"
               "#ifndef AUDSHIM_DATA_H\n#define AUDSHIM_DATA_H\n\n"
               "struct AudDrvMsg { uint32_t len; const uint32_t *w; const char *what; };\n"
               "struct AudDrvDai { uint32_t link, pdi, alh_id, ch, dai_index; };\n"
               "struct AudDrvReg { uint32_t reg; uint8_t val; };\n\n",
            fwp, fw.size, fw.psize, fw.off, fw.hash, tpp, tfnv, pl.pcm_name, pl.pcm_id);
    // IPC messages
    char nm[64];
    for (int i = 0; i < pl.n; i++) { snprintf(nm, sizeof nm, "kDrvSetup%d", i); gh_msg(f, nm, &pl.m[i]); }
    for (int i = 0; i < pl.nf; i++) { snprintf(nm, sizeof nm, "kDrvFree%d", i); gh_msg(f, nm, &pl.f[i]); }
    gh_msg(f, "kDrvPcmParamsW", &pl.pcm_params); gh_msg(f, "kDrvPcmFreeW", &pl.pcm_free);
    gh_msg(f, "kDrvTrigStartW", &pl.trig_start); gh_msg(f, "kDrvTrigStopW", &pl.trig_stop);
    gh_msg(f, "kDrvVolumeW", &pl.vol_0db);
    fprintf(f, "\n// widget set-up + routes + pipeline complete (FE prepare minus PCM_PARAMS), in the Linux order\n");
    fprintf(f, "static const AudDrvMsg kDrvSetup[%d] = {\n", pl.n);
    for (int i = 0; i < pl.n; i++) { fprintf(f, "    {%u, kDrvSetup%d, ", pl.m[i].len, i); gh_escape(f, pl.m[i].what); fprintf(f, "},\n"); }
    fprintf(f, "};\n// widget free (FE hw_free)\nstatic const AudDrvMsg kDrvFree[%d] = {\n", pl.nf);
    for (int i = 0; i < pl.nf; i++) { fprintf(f, "    {%u, kDrvFree%d, ", pl.f[i].len, i); gh_escape(f, pl.f[i].what); fprintf(f, "},\n"); }
    fprintf(f, "};\n");
    fprintf(f, "static const AudDrvMsg kDrvPcmParams = {%u, kDrvPcmParamsW, \"STREAM_PCM_PARAMS\"};\n", pl.pcm_params.len);
    fprintf(f, "static const AudDrvMsg kDrvPcmFree = {%u, kDrvPcmFreeW, \"STREAM_PCM_FREE\"};\n", pl.pcm_free.len);
    fprintf(f, "static const AudDrvMsg kDrvTrigStart = {%u, kDrvTrigStartW, \"STREAM_TRIG_START\"};\n", pl.trig_start.len);
    fprintf(f, "static const AudDrvMsg kDrvTrigStop = {%u, kDrvTrigStopW, \"STREAM_TRIG_STOP\"};\n", pl.trig_stop.len);
    fprintf(f, "// COMP_SET_VALUE template for the PGA: channel c value at byte 96 + 8 * c\n");
    fprintf(f, "static const AudDrvMsg kDrvVolume = {%u, kDrvVolumeW, \"COMP_SET_VALUE PGA\"};\n", pl.vol_0db.len);
    fprintf(f, "static const uint32_t kDrvPgaComp = %u, kDrvVolCh = %u, kDrvVolMin = 0x%x, kDrvVolMax = 0x%x, kDrvHostComp = %u;\n",
            pl.pga_comp, pl.vol_ch, pl.vol_min, pl.vol_max, pl.host_comp);
    // DAIs in BE cpu-DAI order: link 1 first
    fprintf(f, "\n// speaker DAIs (BE hw_params order: SDW1 Pin2, SDW2 Pin2)\nstatic const AudDrvDai kDrvDai[2] = {\n");
    for (int k = 0; k < 2; k++) {
        const struct m5dai *dd = &pl.dai[pl.dai[0].link == (uint32_t)(k + 1) ? 0 : 1];
        fprintf(f, "    {%u, %u, %u, %u, 0x%x},   // %s\n", dd->link, dd->pdi, dd->alh_id, dd->ch, dd->dai_index, dd->wname);
    }
    fprintf(f, "};\n\n// SoundWire transport/port parameters (m5_compute, generic_bandwidth_allocation.c)\n");
    fprintf(f, "static const uint32_t kDrvXpSi = %u, kDrvXpHstart = %u, kDrvXpHstop = %u, kDrvXpOff1 = %u, kDrvXpOff2 = %u,\n"
               "                      kDrvXpBps = %u, kDrvXpChMask = 0x%x;\n", x.si, x.hstart, x.hstop, x.offset1, x.offset2, x.bps, x.chmask);
    fprintf(f, "\n// rt1316_io_init blind writes (rt1316-sdw.c:71-129), after 0xc000 = 0x02\nstatic const AudDrvReg kDrvRtBlind[%zu] = {\n",
            sizeof rt1316_blind / sizeof rt1316_blind[0]);
    for (size_t k = 0; k < sizeof rt1316_blind / sizeof rt1316_blind[0]; k++)
        fprintf(f, "%s{0x%x, 0x%02x},%s", k % 6 ? " " : "    ", rt1316_blind[k].reg, rt1316_blind[k].val, (k % 6 == 5) ? "\n" : "");
    fprintf(f, "\n};\n");
    // ---- M7 (AudShim 0.9.0): microphone capture (pcm 'Microphone', RT714 on link 0)
    fprintf(f, "\n// ==== M7: microphone capture ('%s', pcm %u), generated from the same topology ====\n#define AUDSHIM_DATA_CAPTURE 1\n",
            cp.pcm_name, cp.pcm_id);
    for (int i = 0; i < cp.n; i++) { snprintf(nm, sizeof nm, "kDrvCapSetup%d", i); gh_msg(f, nm, &cp.m[i]); }
    for (int i = 0; i < cp.nf; i++) { snprintf(nm, sizeof nm, "kDrvCapFree%d", i); gh_msg(f, nm, &cp.f[i]); }
    gh_msg(f, "kDrvCapPcmParamsW", &cp.pcm_params); gh_msg(f, "kDrvCapPcmFreeW", &cp.pcm_free);
    gh_msg(f, "kDrvCapTrigStartW", &cp.trig_start); gh_msg(f, "kDrvCapTrigStopW", &cp.trig_stop);
    fprintf(f, "static const AudDrvMsg kDrvCapSetup[%d] = {\n", cp.n);
    for (int i = 0; i < cp.n; i++) { fprintf(f, "    {%u, kDrvCapSetup%d, ", cp.m[i].len, i); gh_escape(f, cp.m[i].what); fprintf(f, "},\n"); }
    fprintf(f, "};\nstatic const AudDrvMsg kDrvCapFree[%d] = {\n", cp.nf);
    for (int i = 0; i < cp.nf; i++) { fprintf(f, "    {%u, kDrvCapFree%d, ", cp.f[i].len, i); gh_escape(f, cp.f[i].what); fprintf(f, "},\n"); }
    fprintf(f, "};\n");
    fprintf(f, "static const AudDrvMsg kDrvCapPcmParams = {%u, kDrvCapPcmParamsW, \"STREAM_PCM_PARAMS (capture)\"};\n", cp.pcm_params.len);
    fprintf(f, "static const AudDrvMsg kDrvCapPcmFree = {%u, kDrvCapPcmFreeW, \"STREAM_PCM_FREE (capture)\"};\n", cp.pcm_free.len);
    fprintf(f, "static const AudDrvMsg kDrvCapTrigStart = {%u, kDrvCapTrigStartW, \"STREAM_TRIG_START (capture)\"};\n", cp.trig_start.len);
    fprintf(f, "static const AudDrvMsg kDrvCapTrigStop = {%u, kDrvCapTrigStopW, \"STREAM_TRIG_STOP (capture)\"};\n", cp.trig_stop.len);
    fprintf(f, "static const uint32_t kDrvCapHostComp = %u, kDrvCapPgaComp = %u, kDrvCapVolCh = %u;\n", cp.host_comp, cp.pga_comp, cp.vol_ch);
    fprintf(f, "// microphone DAI (SDW0 Pin2)\nstatic const AudDrvDai kDrvCapDai = {%u, %u, %u, %u, 0x%x};   // %s\n", cp.dai[0].link,
            cp.dai[0].pdi, cp.dai[0].alh_id, cp.dai[0].ch, cp.dai[0].dai_index, cp.dai[0].wname);
    fprintf(f, "// RT714 (rt715-sdca) data port of the capture DAI (rt715-sdca-aif2 = DP4); transport parameters = kDrvXp* (same\n"
               "// 48 kHz / 2 ch / 24 bit single-stream link)\nstatic const uint32_t kDrvMicPort = %u;\n", MIC_DP);
    fprintf(f, "// RT714 register sequence (audctl mic_ops[]): phase 1 attach, 2 BE hw_params, 3 power up, 4 power down;\n"
               "// kind 1 W8 2 U8 3 C8 4 WQ (MBQ 16-bit write) 5 UQ (MBQ read-modify-write) 6 RQ (MBQ read = hw_ver); cond 1/2: hw_ver 0/1\n"
               "struct AudDrvMicOp { uint8_t phase, kind, cond; uint32_t reg; uint16_t mask, val; };\n"
               "static const AudDrvMicOp kDrvMicOps[%d] = {\n", NMICOPS);
    for (int i = 0; i < NMICOPS; i++)
        fprintf(f, "    {%u, %u, %u, 0x%08x, 0x%04x, 0x%04x},   // %s %s\n", mic_ops[i].phase, mic_ops[i].kind, mic_ops[i].cond, mic_reg(i),
                mic_ops[i].mask, mic_ops[i].val, mic_kind(mic_ops[i].kind), mic_ops[i].what);
    fprintf(f, "};\n");
    fprintf(f, "\n// firmware payload (DMA image, starts with $CPD)\nstatic const uint32_t kDrvFwLen = %u, kDrvFwHash = 0x%08x;\n", fw.psize, fw.hash);
    fprintf(f, "static const uint8_t kDrvFw[%u] __attribute__((aligned(16))) = {\n", fw.psize);
    const uint8_t *p = fw.data + fw.off;
    for (uint32_t i = 0; i < fw.psize; i++) fprintf(f, "%s0x%02x,%s", i % 16 ? "" : "    ", p[i], i % 16 == 15 ? "\n" : "");
    fprintf(f, "};\n\n#endif\n");
    fclose(f);
    printf("wrote %s: %d set-up + %d free IPC messages, PGA comp %u (%u ch), DAIs ALH %u/%u, %zu RT1316 writes,\n"
           "  firmware payload %u bytes FNV-1a 0x%08x\n", out, pl.n, pl.nf, pl.pga_comp, pl.vol_ch, pl.dai[0].alh_id, pl.dai[1].alh_id,
           sizeof rt1316_blind / sizeof rt1316_blind[0], fw.psize, fw.hash);
    printf("  M7 capture: %d set-up + %d free IPC messages, host comp %u, PGA comp %u, ALH %u (link %u PDI %u), %d RT714 ops\n",
           cp.n, cp.nf, cp.host_comp, cp.pga_comp, cp.dai[0].alh_id, cp.dai[0].link, cp.dai[0].pdi, NMICOPS);
    free(fw.data);
    return 0;
}

// ------------------------------------------------------------------ M6: drv (kext driver mode, test from the terminal)
#include "AudShim/AudShimDrv.h"
static io_connect_t dconn;
static int drv_open_conn(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AudShim"));
    if (!svc) { fprintf(stderr, "AudShim not loaded\n"); return -1; }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), AUDSHIM_DRV_CONN_TYPE, &dconn);
    IOObjectRelease(svc);
    if (kr) { fprintf(stderr, "IOServiceOpen(driver connection): 0x%x (needs AudShim 0.8.0)\n", kr); return -1; }
    return 0;
}
static int drv_call(uint64_t op, uint64_t a0, uint64_t o[16]) {
    uint64_t in[2] = {op, a0}; uint32_t n = 16;
    memset(o, 0, 16 * sizeof o[0]);
    kern_return_t kr = IOConnectCallScalarMethod(dconn, AUDSHIM_METHOD_DRV, in, 2, o, &n);
    if (kr) { fprintf(stderr, "driver method: 0x%x\n", kr); return -1; }
    return 0;
}
static const char *drv_st(uint64_t s) { static const char *n[] = {"IDLE", "BUSY", "READY", "RUNNING", "FAILED"}; return s < 5 ? n[s] : "?"; }
static const char *drv_rc(uint64_t r) {
    static const char *n[] = {"ok", "busy (another driver connection owns the hardware, e.g. coreaudiod)", "suspended (sleep/shutdown)",
                              "FAILED (see kernel log: AudShim DRV)", "bad op", "not ready", "retry later (recent failure)"};
    return r < 7 ? n[r] : "?";
}
static void cap_print_state(const uint64_t o[16]);
static void drv_print_state(void) {
    uint64_t o[16];
    if (drv_call(kDrvOpState, 0, o)) return;
    if (o[15] >= 9) {   // M7
        uint64_t c[16];
        uint64_t in[2] = {kDrvOpCapState, 0}; uint32_t n = 16;
        if (!IOConnectCallScalarMethod(dconn, AUDSHIM_METHOD_DRV, in, 2, c, &n)) cap_print_state(c);
    }
    printf("state %s, last failure step %llu code %llu; gain applied 0x%llx (cap 0x%llx = -%llu dB), requested 0x%llx%s\n"
           "starts %llu, bring-ups %llu (last %llu ms), DSP notifications %llu (xruns %llu), host DMA errors %llu, suspended %llu,%s iface v%llu\n",
           drv_st(o[1]), o[2], o[3], o[4], o[5], o[6], o[13] & 0xFFFFFFFF, (o[13] >> 32) ? " (muted)" : "", o[7], o[11], o[12], o[9], o[8],
           o[10], o[14] & 1, (o[14] & 2) ? " LINGERING (deferred stop pending)," : "", o[15]);
}
static int cmd_drv(int argc, char **argv) {
    const char *c = argc > 0 ? argv[0] : "state";
    if (drv_open_conn()) return 1;
    uint64_t o[16];
    int rc = 0;
    if (!strcmp(c, "state")) drv_print_state();
    else if (!strcmp(c, "open") || !strcmp(c, "start") || !strcmp(c, "stop") || !strcmp(c, "close")) {
        // NOTE: this connection owns the hardware only while this process lives; when audctl exits the kext tears
        // everything down again (client death). "open"/"start" here are therefore only useful as quick checks.
        uint64_t op = !strcmp(c, "open") ? kDrvOpOpen : !strcmp(c, "start") ? kDrvOpStart : !strcmp(c, "stop") ? kDrvOpStop : kDrvOpClose;
        struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        if (drv_call(op, 0, o)) return 1;
        clock_gettime(CLOCK_MONOTONIC, &b);
        printf("%s: %s (%.0f ms)\n", c, drv_rc(o[0]), (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6);
        drv_print_state();
        rc = o[0] ? 1 : 0;
    } else if (!strcmp(c, "test")) {
        // end-to-end check of the driver mode without coreaudiod: OPEN, map the ring, write a ~1 kHz sine (looping, like
        // M5), START, watch the position for N seconds, STOP, START/STOP a second time, CLOSE.
        double secs = 2.0, level = -30.0;
        for (int i = 1; i < argc; i++) {
            if (!strncmp(argv[i], "--seconds=", 10)) secs = strtod(argv[i] + 10, NULL);
            else if (!strncmp(argv[i], "--level=", 8)) level = strtod(argv[i] + 8, NULL);
        }
        if (!(secs >= 0.5 && secs <= 5)) { fprintf(stderr, "--seconds 0.5..5\n"); return 2; }
        if (!(level >= -60 && level <= -12)) { fprintf(stderr, "--level -60..-12 dBFS\n"); return 2; }
        printf("audctl 0.9.0 drv test: %.1f s of a ~1 kHz sine at %.1f dBFS through the kext driver mode, 3 times:\n"
               "  round 1 = first START, round 2 = resume 0.3 s after STOP (deferred stop pending), round 3 = START 6 s later\n"
               "  (after the deferred stop really stopped the stream: amps PS3, DMA off, widgets kept)\n", secs, level);
        struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        if (drv_call(kDrvOpOpen, 0, o)) return 1;
        clock_gettime(CLOCK_MONOTONIC, &b);
        printf("OPEN: %s (%.0f ms)\n", drv_rc(o[0]), (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6);
        drv_print_state();
        if (o[0]) return 1;
        // PGA to 0 dB (the kext clamps to its cap); the sine level sets the loudness
        if (drv_call(kDrvOpVolume, 0x10000, o)) return 1;
        for (int round = 1; round <= 3; round++) {
            mach_vm_address_t addr = 0; mach_vm_size_t size = 0;
            kern_return_t kr = IOConnectMapMemory64(dconn, AUDSHIM_MEM_RING, mach_task_self(), &addr, &size, kIOMapAnywhere);
            if (kr || size < AUDSHIM_RING_BYTES) { fprintf(stderr, "map ring: 0x%x size %llu\n", kr, (unsigned long long)size); rc = 1; break; }
            if (drv_call(kDrvOpStart, 0, o)) { rc = 1; break; }
            printf("START %d: %s\n", round, drv_rc(o[0]));
            if (o[0]) { drv_print_state(); rc = 1; break; }
            // the kext zeroes the ring at START; fill it now (the DMA plays ~6 ms of silence first)
            double fr; int32_t pk;
            m5_sine((int32_t *)(uintptr_t)addr, level, 0, &fr, &pk);
            uint64_t p[8]; uint32_t n = 8, prev = 0; uint64_t moved = 0;
            mach_timebase_info_data_t tb; mach_timebase_info(&tb);
            uint64_t t0 = 0, tl = 0;
            double el = 0;
            while (el < secs) {
                usleep(20000);
                n = 8;
                if (IOConnectCallScalarMethod(dconn, AUDSHIM_METHOD_POS, NULL, 0, p, &n)) { fprintf(stderr, "pos failed\n"); rc = 1; break; }
                if (!t0) { t0 = p[2]; prev = (uint32_t)p[1]; tl = p[2]; continue; }   // measure from the first sample
                moved += (uint32_t)((p[1] - prev) & 0xFFFF); prev = (uint32_t)p[1];
                el = (double)(p[2] - t0) * tb.numer / tb.denom / 1e9;
                if (p[2] - tl > 250000000ull * tb.denom / tb.numer) {
                    printf("  t=%5.2f s state %s DPIB 0x%05llx LPIB 0x%05llx moved %llu B (%.0f B/s) SD 0x%08llx xruns %llu gen %llu\n", el,
                           drv_st(p[0]), p[1], p[5], (unsigned long long)moved, el > 0 ? moved / el : 0, p[4], p[6], p[3]);
                    tl = p[2];
                }
                if (p[0] != kDrvStRunning) { printf("  !! state %s\n", drv_st(p[0])); rc = 1; break; }
            }
            if (!rc && (el <= 0 || moved < 0.8 * 384000.0 * el || moved > 1.2 * 384000.0 * el)) {
                printf("  !! the host DMA did not move at the 48 kHz rate (%.2f s of audio in %.2f s)\n", moved / 384000.0, el);
                rc = 1;
            }
            if (drv_call(kDrvOpStop, 0, o)) { rc = 1; break; }
            printf("STOP %d: %s; moved %llu B = %.2f s of audio in %.2f s\n", round, drv_rc(o[0]), (unsigned long long)moved, moved / 384000.0, el);
            IOConnectUnmapMemory64(dconn, AUDSHIM_MEM_RING, mach_task_self(), addr);
            if (o[0] || rc) { rc = 1; break; }
            if (round == 1) usleep(300000);
            else if (round == 2) { printf("  waiting 6 s for the deferred stop...\n"); sleep(6); drv_print_state(); }
        }
        drv_call(kDrvOpClose, 0, o);
        printf("CLOSE: %s\n", drv_rc(o[0]));
        drv_print_state();
        printf("\nDRV TEST RESULT: %s\n", rc ? "FAILURE (see `log show --last 5m --predicate 'eventMessage CONTAINS \"AudShim\"'`)" :
                                             "SUCCESS -> did you hear the ~1 kHz tone three times from both speakers?");
    } else { fprintf(stderr, "drv: state | open | start | stop | close | test [--seconds=N] [--level=dB]\n"); rc = 2; }
    IOServiceClose(dconn);
    return rc;
}

// ------------------------------------------------------------------ M7: rec (microphones -> WAV)
static const char *cap_st(uint64_t s) { static const char *n[] = {"OFF", "RUNNING", "FAILED"}; return s < 3 ? n[s] : "?"; }
static void cap_print_state(const uint64_t o[16]) {
    printf("capture %s (driver %s), last failure step %llu code 0x%llx, starts %llu, capture DMA errors %llu, RT714 dev %llu "
           "(hw_ver %s%llu), init %llu, widgets %llu, ring gen %llu, owner %s, DSP xruns %llu, iface v%llu\n",
           cap_st(o[2]), drv_st(o[1]), o[3], o[4], o[5], o[6], o[7], o[12] == 0xFF ? "?" : "", o[12] == 0xFF ? 0 : o[12], o[10], o[14],
           o[8], o[9] == 1 ? "this connection" : o[9] == 2 ? "another connection" : "none", o[13], o[15]);
}
static void wav_hdr(FILE *f, uint32_t frames) {
    uint32_t data = frames * 8, v;
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    uint16_t fmt = 1, ch = 2, bits = 32, align = 8; uint32_t rate = 48000, bps = 48000 * 8;
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&bps, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}
static double dbfs(double x) { return x > 0 ? 20.0 * log10(x) : -200.0; }
static int cmd_rec(int argc, char **argv) {
    double secs = 5.0;
    int dry = 0, hex = 0;
    const char *out = "mic.wav", *tpath = NULL;
    char tdef[1024]; const char *home = getenv("HOME");
    snprintf(tdef, sizeof tdef, "%s/xe/build/audio/fw/use/sof-rpl-rt1316-l12-rt714-l0.tplg", home ? home : "");
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "--seconds=", 10)) secs = strtod(argv[i] + 10, NULL);
        else if (!strcmp(argv[i], "--dry-run") || !strcmp(argv[i], "--dry")) dry = 1;
        else if (!strcmp(argv[i], "--hex")) hex = 1;
        else if (!strncmp(argv[i], "--out=", 6)) out = argv[i] + 6;
        else if (!strncmp(argv[i], "--tplg=", 7)) tpath = argv[i] + 7;
        else if (argv[i][0] != '-') out = argv[i];
        else { fprintf(stderr, "rec: unknown option %s\n", argv[i]); return 2; }
    }
    if (!(secs >= 0.5 && secs <= 60)) { fprintf(stderr, "--seconds 0.5..60\n"); return 2; }
    if (dry) {   // offline: no kext access at all
        static struct tplg tp; static struct m5plan cp;
        printf("audctl 0.9.0 rec --dry-run: %.1f s from the internal microphones to %s would run the sequence below.\n", secs, out);
        if (tplg_load(&tp, tpath ? tpath : tdef, 1) || pl_build_dir(&tp, &cp, 1, 1)) return 1;
        printf("\n");
        mic_plan_print(&cp, hex);
        printf("\nthen: read the ring behind DPIB every 20 ms for %.1f s, CAP_STOP, close (the kext tears down if this connection owned\n"
               "the hardware), write %s (RIFF/WAVE PCM, 48000 Hz, 2 ch, 32 bit) and print the RMS / peak level per channel.\n", secs, out);
        return 0;
    }
    if (drv_open_conn()) return 1;
    uint64_t o[16];
    if (drv_call(kDrvOpState, 0, o)) return 1;
    if (o[15] < 9) { fprintf(stderr, "the loaded AudShim is interface v%llu: rec needs v9 (AudShim 0.9.0)\n", o[15]); return 1; }
    printf("audctl 0.9.0 rec: %.1f s from the internal microphones (RT714, SOF pipeline 5) -> %s\n", secs, out);
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    if (drv_call(kDrvOpCapStart, 0, o)) return 1;
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("CAP_START: %s (%.0f ms)\n", drv_rc(o[0]), (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6);
    cap_print_state(o);
    if (o[0] || o[2] != 1) {
        printf("REC RESULT: FAIL start (step %llu code 0x%llx; kernel log: AudShim DRV)\n", o[3], o[4]);
        IOServiceClose(dconn);
        return 1;
    }
    mach_vm_address_t addr = 0; mach_vm_size_t size = 0;
    kern_return_t kr = IOConnectMapMemory64(dconn, AUDSHIM_MEM_CRING, mach_task_self(), &addr, &size, kIOMapAnywhere | kIOMapReadOnly);
    if (kr || size < AUDSHIM_RING_BYTES) {
        fprintf(stderr, "map capture ring: 0x%x size %llu\n", kr, (unsigned long long)size);
        drv_call(kDrvOpCapStop, 0, o); IOServiceClose(dconn);
        printf("REC RESULT: FAIL map\n");
        return 1;
    }
    const volatile int32_t *ring = (const volatile int32_t *)(uintptr_t)addr;
    uint32_t want = (uint32_t)(secs * 48000.0);
    int32_t *buf = calloc((size_t)want + 8192, 8);
    if (!buf) { drv_call(kDrvOpCapStop, 0, o); IOServiceClose(dconn); return 1; }
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t p[8]; uint32_t n, got = 0, prev = 0, laps_lost = 0;
    uint64_t t0 = 0, tl = 0, moved = 0;
    int rc = 0, first = 1;
    double el = 0;
    while (got < want) {
        usleep(20000);
        n = 8;
        if (IOConnectCallScalarMethod(dconn, AUDSHIM_METHOD_CPOS, NULL, 0, p, &n)) { fprintf(stderr, "capture position failed\n"); rc = 1; break; }
        if (p[0] != 1) { printf("  !! capture stopped (state %llu)\n", p[0]); rc = 1; break; }
        uint32_t d = (uint32_t)(p[1] % AUDSHIM_RING_BYTES) & ~7u;
        if (first) { first = 0; t0 = tl = p[2]; prev = d; continue; }   // start reading from here (older data is pre-roll)
        uint32_t nb = (d - prev) & (AUDSHIM_RING_BYTES - 1);
        if (nb > AUDSHIM_RING_BYTES - 8192) laps_lost++;   // > 7/8 of the ring in 20 ms would mean we fell behind
        for (uint32_t k = 0; k < nb / 8 && got < want; k++) {
            uint32_t fr = ((prev / 8) + k) % AUDSHIM_RING_FRAMES;
            buf[2 * got] = ring[2 * fr]; buf[2 * got + 1] = ring[2 * fr + 1];
            got++;
        }
        moved += nb; prev = d;
        el = (double)(p[2] - t0) * tb.numer / tb.denom / 1e9;
        if (p[2] - tl > 1000000000ull * tb.denom / tb.numer) {
            printf("  t=%5.2f s DPIB 0x%05llx LPIB 0x%05llx moved %llu B (%.0f B/s) SD 0x%08llx xruns %llu frames %u\n", el, p[1], p[5],
                   (unsigned long long)moved, el > 0 ? moved / el : 0, p[4], p[6], got);
            tl = p[2];
        }
        if (el > secs + 3) { printf("  !! timeout: only %u of %u frames in %.1f s\n", got, want, el); rc = 1; break; }
    }
    drv_call(kDrvOpCapStop, 0, o);
    printf("CAP_STOP: %s\n", drv_rc(o[0]));
    cap_print_state(o);
    IOConnectUnmapMemory64(dconn, AUDSHIM_MEM_CRING, mach_task_self(), addr);
    IOServiceClose(dconn);
    double rate = el > 0 ? moved / 8.0 / el : 0;
    // levels (24 valid bits in a 32-bit container: dBFS relative to INT32 full scale)
    double ss[2] = {0, 0}, mean[2] = {0, 0}; int32_t pk[2] = {0, 0}; uint32_t zeros = 0, same = 0;
    for (uint32_t i = 0; i < got; i++) for (int c = 0; c < 2; c++) mean[c] += buf[2 * i + c];
    for (int c = 0; c < 2; c++) mean[c] = got ? mean[c] / got : 0;
    for (uint32_t i = 0; i < got; i++) {
        for (int c = 0; c < 2; c++) {
            double x = (buf[2 * i + c] - mean[c]) / 2147483648.0;
            ss[c] += x * x;
            int32_t ax = buf[2 * i + c] == INT32_MIN ? INT32_MAX : abs(buf[2 * i + c]);
            if (ax > pk[c]) pk[c] = ax;
        }
        if (!buf[2 * i] && !buf[2 * i + 1]) zeros++;
        if (i && buf[2 * i] == buf[2 * i - 2] && buf[2 * i + 1] == buf[2 * i - 1]) same++;
    }
    double rms[2], rmsdb[2];
    for (int c = 0; c < 2; c++) { rms[c] = got ? sqrt(ss[c] / got) : 0; rmsdb[c] = dbfs(rms[c]); }
    FILE *f = fopen(out, "wb");
    if (f) { wav_hdr(f, got); fwrite(buf, 8, got, f); fclose(f); }
    else { perror(out); rc = 1; }
    free(buf);
    printf("\nwrote %s: %u frames (%.2f s), DMA rate %.0f frames/s, DC L %.0f R %.0f\n", out, got, got / 48000.0, rate, mean[0], mean[1]);
    printf("level: L RMS %.1f dBFS peak %.1f dBFS | R RMS %.1f dBFS peak %.1f dBFS | all-zero frames %u, repeated frames %u, ring overruns %u\n",
           rmsdb[0], dbfs(pk[0] / 2147483648.0), rmsdb[1], dbfs(pk[1] / 2147483648.0), zeros, same, laps_lost);
    int dma_ok = rate > 0.8 * 48000 && rate < 1.2 * 48000 && got >= want;
    printf("REC RESULT: %s frames=%u rate=%.0f rms_l_db=%.1f rms_r_db=%.1f peak_db=%.1f zero_frames=%u\n",
           rc || !dma_ok ? "FAIL" : "OK", got, rate, rmsdb[0], rmsdb[1], dbfs((pk[0] > pk[1] ? pk[0] : pk[1]) / 2147483648.0), zeros);
    return (rc || !dma_ok) ? 1 : 0;
}

// ------------------------------------------------------------------ main

static uint32_t num(const char *s) { return (uint32_t)strtoul(s, NULL, 0); }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: audctl info | rd0 <off> [n] | rd4 <off> [n] | cfg <off> [n] | m1 [hda]\n"
                        "       audctl m2 --dry [--offline] | m2 [--ipc=auto|first|never] [--rom-ms=N] [--teardown] | m2 --undo | m2 --state\n"
                        "       audctl m2b --dry [--offline] | m2b [--attempts=1..3] [--rom-ms=N] [--leave-up] [--no-ctrl-reset]\n"
                        "                  [--no-iccmax] [--no-l1sen] [--ssp] [--no-pie] | m2b --undo | m2b --state\n"
                        "       audctl m3 --dry [--offline] [--fw=PATH] | m3 [--fw=PATH] [--attempts=1..3] [--rom-ms=N]\n"
                        "                  [--ready-ms=N] [--leave-up] [--post-fw-run] [--ssp] [--no-ctrl-reset] [--no-iccmax]\n"
                        "                  [--no-l1sen] [--no-pie] | m3 --undo | m3 --state\n"
                        "       audctl m4 --dry [--offline] [--fw=PATH] | m4 [m3 options] [--links=0x1..0xF] [--attach-ms=100..2000]\n"
                        "                  [--leave-up] | m4 --undo | m4 --state\n"
                        "       audctl m5 --dry [--offline] [--hex] | m5 [m4 options] [--seconds N (0.5..3, default 2)] [--level dB\n"
                        "                  (-60..-12, default -30)] [--channel=both|left|right] [--no-play] [--tplg=PATH] | m5 --undo | m5 --state\n"
                        "       audctl m5a [...]    = m5 --no-play (M5a: topology/pipeline/amp set-up and teardown, no audio)\n"
                        "       audctl tplg [PATH] [--hex] [--plan]   topology dump + generated IPC3 messages (offline)\n"
                        "       audctl gen-header [OUT.h] [--fw=PATH] [--tplg=PATH]   M6: static data header for the kext driver mode\n"
                        "       audctl drv state | open | start | stop | close | test [--seconds=N] [--level=dB]   M6: kext driver mode\n"
                        "       audctl rec [OUT.wav] [--seconds=N (0.5..60, default 5)] [--dry-run [--hex] [--tplg=PATH]]   M7: microphones\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!strcmp(argv[1], "m2")) return cmd_m2(argc - 2, argv + 2);
    if (!strcmp(argv[1], "m2b")) return cmd_boot(argc - 2, argv + 2, 0);
    if (!strcmp(argv[1], "m3")) return cmd_boot(argc - 2, argv + 2, 1);
    if (!strcmp(argv[1], "m4")) return cmd_boot(argc - 2, argv + 2, 2);
    if (!strcmp(argv[1], "m5")) return cmd_boot(argc - 2, argv + 2, 3);
    if (!strcmp(argv[1], "m5a")) return cmd_boot(argc - 2, argv + 2, 4);
    if (!strcmp(argv[1], "gen-header")) return cmd_genheader(argc - 2, argv + 2);   // offline (M6)
    if (!strcmp(argv[1], "drv")) return cmd_drv(argc - 2, argv + 2);                 // M6 driver mode
    if (!strcmp(argv[1], "rec")) return cmd_rec(argc - 2, argv + 2);                 // M7 microphones
    if (!strcmp(argv[1], "tplg")) {   // offline: topology dump + the generated IPC3 plan (no kext needed)
        static struct tplg tp; static struct m5plan pl;
        char tdef[1024]; const char *home = getenv("HOME"), *path = NULL; int hex = 0, quiet = 0;
        snprintf(tdef, sizeof tdef, "%s/xe/build/audio/fw/use/sof-rpl-rt1316-l12-rt714-l0.tplg", home ? home : "");
        for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--hex")) hex = 1; else if (!strcmp(argv[i], "--plan")) quiet = 1; else path = argv[i]; }
        printf("audctl 0.9.0  tplg: ALSA/SOF topology parser + IPC3 plans for the speaker and microphone PCMs (read-only, no kext access)\n\n");
        if (tplg_load(&tp, path ? path : tdef, 1)) return 1;
        if (!quiet) tplg_dump(&tp);
        printf("\n-- speaker playback plan (what Linux sends for 'Speaker', pcm 2) --\n");
        if (pl_build(&tp, &pl, 1)) return 1;
        for (int k = 0; k < 2; k++)
            printf("  %s: link %u PDI %u ALH stream id %u, FW dai_index 0x%x, %u Hz %u ch fmt %u\n", pl.dai[k].wname, pl.dai[k].link,
                   pl.dai[k].pdi, pl.dai[k].alh_id, pl.dai[k].dai_index, pl.dai[k].rate, pl.dai[k].ch, pl.dai[k].fmt);
        pl_print(&pl, hex);
        static struct m5plan cp;   // M7
        printf("\n-- microphone capture plan (what Linux sends for 'Microphone', pcm 4) --\n");
        if (pl_build_dir(&tp, &cp, 1, 1)) return 1;
        printf("  %s: link %u PDI %u ALH stream id %u, FW dai_index 0x%x, %u Hz %u ch fmt %u\n", cp.dai[0].wname, cp.dai[0].link,
               cp.dai[0].pdi, cp.dai[0].alh_id, cp.dai[0].dai_index, cp.dai[0].rate, cp.dai[0].ch, cp.dai[0].fmt);
        pl_print(&cp, hex);
        return 0;
    }
    if (aud_open()) return 1;
    const char *c = argv[1];
    uint32_t off = argc > 2 ? num(argv[2]) : 0, n = argc > 3 ? num(argv[3]) : 1;
    if (n == 0 || n > 0x40000) n = 1;
    if (!strcmp(c, "info")) cmd_info();
    else if (!strcmp(c, "rd0") && argc > 2) hexdump(0, off, n);
    else if (!strcmp(c, "rd4") && argc > 2) hexdump(4, off, n);
    else if (!strcmp(c, "cfg") && argc > 2) hexdump('c', off, n);
    else if (!strcmp(c, "m1")) cmd_m1(argc > 2 && !strcmp(argv[2], "hda"));
    else { fprintf(stderr, "unknown command\n"); return 2; }
    IOServiceClose(conn);
    return 0;
}
