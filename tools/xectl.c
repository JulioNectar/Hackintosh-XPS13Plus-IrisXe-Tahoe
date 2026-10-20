// xectl: userspace bring-up tool for the Intel Xe iGPU via the XeShim kext.
//   xectl info                 PCI/BAR info
//   xectl regs                 named register dump (display, GT, fuses)
//   xectl rd <off> [n]         read n dwords of MMIO
//   xectl wr <off> <val>       write one MMIO dword (bring-up only)
//   xectl gtt <idx> [n]        read GGTT PTEs
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "xe.h"

static io_connect_t conn;
static volatile uint8_t *bar0;
static uint64_t bar0Len;

int xe_open(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("XeShim"));
    if (!svc) { fprintf(stderr, "XeShim not loaded\n"); return -1; }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr) { fprintf(stderr, "IOServiceOpen: 0x%x\n", kr); return -1; }
    mach_vm_address_t a = 0; mach_vm_size_t s = 0;
    kr = IOConnectMapMemory64(conn, 0, mach_task_self(), &a, &s, kIOMapAnywhere);
    if (kr) { fprintf(stderr, "map BAR0: 0x%x\n", kr); return -1; }
    bar0 = (volatile uint8_t *)a; bar0Len = s;
    return 0;
}

uint32_t xe_rd(uint32_t off) { return off + 4 <= bar0Len ? *(volatile uint32_t *)(bar0 + off) : 0xDEADBEEF; }
void xe_wr(uint32_t off, uint32_t v) { if (off + 4 <= bar0Len) *(volatile uint32_t *)(bar0 + off) = v; }
uint64_t xe_gtt_rd(uint32_t idx) { return *(volatile uint64_t *)(bar0 + XE_GGTT_OFFSET + (uint64_t)idx * 8); }
void xe_gtt_wr(uint32_t idx, uint64_t pte) { *(volatile uint64_t *)(bar0 + XE_GGTT_OFFSET + (uint64_t)idx * 8) = pte; }

int xe_info(uint64_t out[16]) {
    uint32_t n = 16;
    return IOConnectCallScalarMethod(conn, 0, NULL, 0, out, &n);
}

int xe_dma_alloc(uint64_t size, struct xe_dma *d) {
    uint64_t in[2] = {size, 0}, out[3]; uint32_t n = 3;
    kern_return_t kr = IOConnectCallScalarMethod(conn, 1, in, 2, out, &n);
    if (kr) return kr;
    d->handle = out[0]; d->bus = out[1]; d->size = out[2];
    mach_vm_address_t a = 0; mach_vm_size_t s = 0;
    kr = IOConnectMapMemory64(conn, 0x100 + (uint32_t)d->handle, mach_task_self(), &a, &s, kIOMapAnywhere);
    if (kr) return kr;
    d->cpu = (void *)a;
    return 0;
}

int xe_dma_free(struct xe_dma *d) {
    if (!d->cpu) return -1;
    IOConnectUnmapMemory64(conn, 0x100 + (uint32_t)d->handle, mach_task_self(), (mach_vm_address_t)d->cpu);
    uint64_t in[1] = { d->handle };
    kern_return_t kr = IOConnectCallScalarMethod(conn, 2, in, 1, NULL, NULL);
    d->cpu = NULL;
    return kr;
}

static const struct { const char *n; uint32_t o; } kRegs[] = {
    {"HTOTAL_A", 0x60000}, {"VTOTAL_A", 0x6000C}, {"PIPESRC_A", 0x6001C}, {"PIPECONF_A", 0x70008},
    {"PLANE_CTL_1_A", 0x70180}, {"PLANE_STRIDE_1_A", 0x70188}, {"PLANE_POS_1_A", 0x7018C},
    {"PLANE_SIZE_1_A", 0x70190}, {"PLANE_SURF_1_A", 0x7019C},
    {"PS_WIN_POS_1_A", 0x68170}, {"PS_WIN_SZ_1_A", 0x68174}, {"PS_CTRL_1_A", 0x68180},
    {"BLC_PWM_PCH_CTL1", 0xC8250}, {"BLC_PWM_PCH_CTL2", 0xC8254},
    {"BLC_PWM_CTL_DDI", 0x48250}, {"BLC_PWM_FREQ", 0x48254}, {"BLC_PWM_DUTY", 0x48258},
    {"DDI_BUF_CTL_A", 0x64000}, {"TRANS_DDI_FUNC_CTL_A", 0x60400},
    {"PWR_WELL_CTL2", 0x45404}, {"DC_STATE_EN", 0x45504},
    {"FORCEWAKE_ACK_RENDER", 0x0D84}, {"FORCEWAKE_RENDER", 0xA278}, {"FORCEWAKE_ACK_GT", 0x130044},
    {"RCS_RING_TAIL", 0x2030}, {"RCS_RING_HEAD", 0x2034}, {"RCS_RING_START", 0x2038},
    {"RCS_RING_CTL", 0x203C}, {"RCS_MODE", 0x229C}, {"RCS_EXECLIST_STATUS", 0x2234},
    {"BCS_RING_HEAD", 0x22034}, {"BCS_MODE", 0x2229C},
    {"GEN11_EU_DISABLE", 0x9134}, {"GEN11_GT_SLICE_ENABLE", 0x9138}, {"GEN12_GT_DSS_ENABLE", 0x913C},
    {"GEN6_RPSTAT1", 0xA01C}, {"GEN6_RC_STATE", 0x138104}, {"GEN6_GT_CORE_STATUS", 0x138060},
    {"GUC_STATUS", 0xC000}, {"GEN12_GSMBASE", 0x108100}, {"GEN12_DSMBASE", 0x1080C0},
};


#include <unistd.h>
// Forcewake: Gen12 has separate GT and RENDER domains (masked-bit registers).
#define FW_GT 0xA188u
#define FW_GT_ACK 0x130044u
#define FW_RENDER 0xA278u
#define FW_RENDER_ACK 0x0D84u
#define FW_BCS_ACK 0x130044u
static int fw_wait(uint32_t ack, int on) {
    for (int i = 0; i < 1000; i++) {
        if (((xe_rd(ack) & 1) != 0) == on) return 0;
        usleep(50);
    }
    return -1;
}
int xe_forcewake(int on) {
    uint32_t v = on ? 0x00010001u : 0x00010000u;
    xe_wr(FW_GT, v);
    int a = fw_wait(FW_GT_ACK, on);
    xe_wr(FW_RENDER, v);
    int b = fw_wait(FW_RENDER_ACK, on);
    if (a || b) fprintf(stderr, "forcewake %s timeout: gt=%d render=%d (ack gt %08x render %08x)\n",
                        on ? "get" : "put", a, b, xe_rd(FW_GT_ACK), xe_rd(FW_RENDER_ACK));
    return a || b;
}

#ifndef XECTL_NO_MAIN
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: xectl info|regs|gt|rd|wr|gtt ...\n"); return 2; }
    if (xe_open()) return 1;
    const char *cmd = argv[1];
    if (!strcmp(cmd, "info")) {
        uint64_t o[16];
        if (xe_info(o)) return 1;
        printf("vendor/device %08llx rev/class %08llx\nBAR0 %#llx len %#llx\nBAR2 %#llx len %#llx\n"
               "GGC %08llx BDSM %08llx CMD %08llx\nmapped BAR0 len %#llx\n",
               o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8], bar0Len);
    } else if (!strcmp(cmd, "regs")) {
        for (size_t i = 0; i < sizeof(kRegs) / sizeof(kRegs[0]); i++)
            printf("%-22s %06x = %08x\n", kRegs[i].n, kRegs[i].o, xe_rd(kRegs[i].o));
    } else if (!strcmp(cmd, "gt")) {
        xe_forcewake(1);
        for (size_t i = 0; i < sizeof(kRegs) / sizeof(kRegs[0]); i++)
            if (kRegs[i].o < 0x40000 || kRegs[i].o >= 0x100000)
                printf("%-22s %06x = %08x\n", kRegs[i].n, kRegs[i].o, xe_rd(kRegs[i].o));
        xe_forcewake(0);
    } else if (!strcmp(cmd, "rdw") && argc >= 3) {  // read with forcewake held
        xe_forcewake(1);
        uint32_t off = (uint32_t)strtoul(argv[2], 0, 16);
        int n = argc > 3 ? atoi(argv[3]) : 1;
        for (int i = 0; i < n; i++) printf("%06x = %08x\n", off + 4 * i, xe_rd(off + 4 * i));
        xe_forcewake(0);
    } else if (!strcmp(cmd, "px") && argc >= 4) {  // read scanout pixel via BAR2 aperture
        mach_vm_address_t a = 0; mach_vm_size_t sz = 0;
        if (IOConnectMapMemory64(conn, 1, mach_task_self(), &a, &sz, kIOMapAnywhere)) return 1;
        uint32_t surf = xe_rd(0x7019C) & ~0xFFFu, pitch = (xe_rd(0x70188) & 0x7FF) * 64;
        for (int i = 2; i + 1 < argc; i += 2) {
            int x = atoi(argv[i]), y = atoi(argv[i + 1]);
            printf("(%d,%d) = %08x\n", x, y, *(volatile uint32_t *)(a + surf + (uint64_t)y * pitch + (uint64_t)x * 4));
        }
    } else if (!strcmp(cmd, "arm") && argc >= 3) {  // XeShim v0.2+: toggle xe-icl-accel marker
        uint64_t v = (uint64_t)atoi(argv[2]);
        kern_return_t kr = IOConnectCallScalarMethod(conn, 4, &v, 1, NULL, NULL);
        printf("arm %llu -> 0x%x\n", v, kr);
    } else if (!strcmp(cmd, "fb") && argc >= 3) {  // XeShim v0.3+: 1 = XeFB takes display, 0 = give back
        uint64_t v = (uint64_t)atoi(argv[2]);
        kern_return_t kr = IOConnectCallScalarMethod(conn, 5, &v, 1, NULL, NULL);
        printf("fb %llu -> 0x%x\n", v, kr);
    } else if (!strcmp(cmd, "rd") && argc >= 3) {
        uint32_t off = (uint32_t)strtoul(argv[2], 0, 16);
        int n = argc > 3 ? atoi(argv[3]) : 1;
        for (int i = 0; i < n; i++) printf("%06x = %08x\n", off + 4 * i, xe_rd(off + 4 * i));
    } else if (!strcmp(cmd, "wr") && argc >= 4) {
        uint32_t off = (uint32_t)strtoul(argv[2], 0, 16), v = (uint32_t)strtoul(argv[3], 0, 16);
        printf("%06x: %08x -> ", off, xe_rd(off));
        xe_wr(off, v);
        printf("%08x\n", xe_rd(off));
    } else if (!strcmp(cmd, "wrw") && argc >= 4) {  // write with forcewake held (GT registers in RC6)
        uint32_t off = (uint32_t)strtoul(argv[2], 0, 16), v = (uint32_t)strtoul(argv[3], 0, 16);
        xe_forcewake(1);
        printf("%06x: %08x -> ", off, xe_rd(off));
        xe_wr(off, v);
        printf("%08x\n", xe_rd(off));
        xe_forcewake(0);
    } else if (!strcmp(cmd, "gtt") && argc >= 3) {
        uint32_t idx = (uint32_t)strtoul(argv[2], 0, 0);
        int n = argc > 3 ? atoi(argv[3]) : 1;
        for (int i = 0; i < n; i++) printf("pte[%u] = %016llx\n", idx + i, xe_gtt_rd(idx + i));
    } else {
        fprintf(stderr, "bad command\n"); return 2;
    }
    return 0;
}
#endif
