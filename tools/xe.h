// Shared helpers for userspace Xe bring-up tools (backed by XeShim).
#pragma once
#include <stdint.h>

#define XE_GGTT_OFFSET 0x800000u  // GGTT PTEs live in the upper half of BAR0

struct xe_dma {
    uint64_t handle;
    uint64_t bus;   // physical/bus address for the GPU
    uint64_t size;
    void *cpu;      // write-combined CPU mapping
};

int xe_open(void);
int xe_info(uint64_t out[16]);
uint32_t xe_rd(uint32_t off);
void xe_wr(uint32_t off, uint32_t v);
uint64_t xe_gtt_rd(uint32_t idx);
void xe_gtt_wr(uint32_t idx, uint64_t pte);
int xe_dma_alloc(uint64_t size, struct xe_dma *d);
int xe_dma_free(struct xe_dma *d);   // unmap from the task and release the kernel buffer
