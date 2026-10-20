	
//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#ifndef kern_gen11_hpp
#define kern_gen11_hpp
#include "kern_green.hpp"
#include "Firmware.hpp"
#include "kern_patcherplus.hpp"
#include "AppleIntelParams.hpp"
#include <Headers/kern_util.hpp>
#include <IOKit/IOBufferMemoryDescriptor.h>

// ─── Platform info / connector structures (from NootedBlue reference) ────────
// Note: ConnectorType enum is defined later in this file (below line 1100).

struct PACKED VoltageConfig {
	uint64_t pad;
	uint8_t *voltageData;
};

struct PACKED ConnectorEntry {
	uint32_t index;
	uint32_t busId;
	uint32_t pipe;
	uint32_t pad;
	uint32_t type;
	uint32_t flags;
};

struct PACKED PlatformInfo {
	uint32_t fInfoPlatformID;
	uint32_t fPchType;
	uint64_t *fModelNameAddr;
	uint8_t  fMobile;
	uint8_t  fPipeCount;
	uint8_t  fInfoPortCount;
	uint8_t  fInfoFramebufferCount;
	uint32_t fInfoFramebufferMemorySize;
	uint32_t fInfoFBCompressionMemorySize;
	uint32_t fUnifiedMemorySize;
	struct ConnectorEntry connectors[0x9];
	uint32_t fInfoFlags;
	uint32_t pad0;
	struct VoltageConfig voltages[0x9];
	uint32_t cameliav;
	uint32_t MCLK;
	uint32_t VCLK;
	uint32_t TCONfbindex;
	uint32_t BeaconLoc;
	uint32_t MinFrmTim;
	uint32_t MaxFrmTim;
	uint32_t fInfoDynamicFBCThreshold;
	uint32_t fVideoTurboFreq;
	uint32_t fSliceCount;
	uint32_t fmaxEuCount;
	uint32_t fsubslices;
};

// Framebuffer flags (fInfoFlags / boot flags) used in platform info patching
enum FramebufferFlags2 : uint32_t {
	FB_FLAG_AVOID_FAST_LINK_TRAINING     = 0x1,
	FB_FLAG_ENABLE_BACKLIGHT_REG_CONTROL = 0x2,
	FB_FLAG_FRAMEBUFFER_COMPRESSION      = 0x4,
	FB_FLAG_ENABLE_SLICE_FEATURES        = 0x8,
	FB_FLAG_DYNAMIC_FBC_ENABLE           = 0x10,
	FB_FLAG_USE_VIDEO_TURBO              = 0x20,
	FB_FLAG_FORCE_POWER_ALWAYS_CONNECTED = 0x40,
	FB_FLAG_DISABLE_HIGH_BITRATE_MODE2   = 0x80,
	FB_FLAG_BOOST_PIXEL_FREQUENCY_LIMIT  = 0x100,
	FB_FLAG_LIMIT_4K_SOURCE_SIZE         = 0x200,
	FB_FLAG_ALTERNATE_PWM_INCREMENT1     = 0x400,
	FB_FLAG_ALTERNATE_PWM_INCREMENT2     = 0x800,
	FB_FLAG_DISABLE_FEATURE_IPS          = 0x1000,
	FB_FLAG_ENABLE_DITHERING             = 0x2000,
	FB_FLAG_ALLOW_CONNECTOR_RECOVER      = 0x4000,
	FB_FLAG_DISABLE_PIPE_SCRAMBLE        = 0x8000,
	FB_FLAG_disable3d                    = 0x10000,
	FB_FLAG_ENABLE_HDMI_AUDIO            = 0x20000,
	FB_FLAG_DISABLE_GFMP_PFM             = 0x40000,
	FB_FLAG_ENABLE_PSR                   = 0x80000,
	FB_FLAG_ENABLE_PSR2                  = 0x100000,
	FB_FLAG_ENABLE_DYNAMIC_CDCLK         = 0x200000,
	FB_FLAG_SUPPORT_4K_60HZ              = 0x400000,
	FB_FLAG_SUPPORT_5K_SOURCE_SIZE       = 0x800000,
};

// ─── DMC (Display Microcontroller) firmware structures ──────────────────────
// Used to parse Intel DMC firmware blobs for display power management
#define DMC_DEFAULT_FW_OFFSET		0xFFFFFFFF
#define PACKAGE_MAX_FW_INFO_ENTRIES	20
#define PACKAGE_V2_MAX_FW_INFO_ENTRIES	32
#define DMC_V1_MAX_MMIO_COUNT		8
#define DMC_V3_MAX_MMIO_COUNT		20
#define DMC_V1_MMIO_START_RANGE		0x80000

// CSS = Code Signing Structure — firmware blob header for signature verification
struct PACKED intel_css_header
{
	uint32_t module_type;
	uint32_t header_len;
	uint32_t header_ver;
	uint32_t module_id;
	uint32_t module_vendor;
	uint32_t date;
	uint32_t size;
	uint32_t key_size;
	uint32_t modulus_size;
	uint32_t exponent_size;
	uint32_t reserved1[0xc];
	uint32_t version;
	uint32_t reserved2[0x8];
	uint32_t kernel_header_info;
};

// Wrapper header for multiple DMC firmware entries (multi-pipe support)
struct PACKED intel_package_header
{
	uint8_t header_len;
	uint8_t header_ver;
	uint8_t reserved[0xa];
	uint32_t num_entries;
};

// Per-pipe firmware descriptor: stepping/substepping + offset into binary
struct PACKED intel_fw_info
{
	uint8_t reserved1;
	uint8_t dmc_id;
	char stepping;
	char substepping;
	uint32_t offset;
	uint32_t reserved2;
};

// Base DMC header — common fields for v1 and v3 variants
struct PACKED intel_dmc_header_base {
	uint32_t signature;
	uint8_t header_len;
	uint8_t header_ver;
	uint16_t dmcc_ver;
	uint32_t project;
	uint32_t fw_size;
	uint32_t fw_version;
};

// DMC v1: ICL/TGL — fixed MMIO save/restore slots (up to 8)
struct PACKED intel_dmc_header_v1 {
	struct intel_dmc_header_base base;
	uint32_t mmio_count;
	uint32_t mmioaddr[DMC_V1_MAX_MMIO_COUNT];
	uint32_t mmiodata[DMC_V1_MAX_MMIO_COUNT];
	char dfile[32];
	uint32_t reserved1[2];
};

// DMC v3: ADL+ — range-based MMIO + up to 20 save/restore slots
struct PACKED intel_dmc_header_v3 {
	struct intel_dmc_header_base base;
	uint32_t start_mmioaddr;
	uint32_t reserved[9];
	char dfile[32];
	uint32_t mmio_count;
	uint32_t mmioaddr[DMC_V3_MAX_MMIO_COUNT];
	uint32_t mmiodata[DMC_V3_MAX_MMIO_COUNT];
};

// ─── Workaround registers (WA) ──────────────────────────────────────────────
// Hardware workaround register offsets and bits, mostly from i915 Linux driver.
// PSR = Panel Self Refresh interrupt mask/status
#define _PSR_IMR_A				0x60814
#define _PSR_IIR_A				0x60818
// DG1 tile-level master interrupt (reused for GT interrupt routing on Gen12+)
#define DG1_MSTR_TILE_INTR		(0x190008)
#define   DG1_MSTR_IRQ			REG_BIT(31)
#define   DG1_MSTR_TILE(t)		REG_BIT(t)
#define  DBUF_POWER_REQUEST			REG_BIT(31)
// South display clock gating + display chicken register workarounds
#define SOUTH_DSPCLK_GATE_D	(0xc2020)
#define GEN11_CHICKEN_DCPR_2	(0x46434)
#define  PCH_DPMGUNIT_CLOCK_GATE_DISABLE (1 << 15)
#define   DCPR_MASK_MAXLATENCY_MEMUP_CLR	(1 << 27)
#define   DCPR_MASK_LPMODE			(1 << 26)
#define   DCPR_SEND_RESP_IMM			(1 << 25)
#define   DCPR_CLEAR_MEMSTAT_DIS		(1 << 24)
#define GEN9_CLKGATE_DIS_0		(0x46530)
#define   DARBF_GATING_DIS		(1 << 27)
#define DC_STATE_EN			(0x45504)
#define CLKREQ_POLICY			(0x101038)
#define  CLKREQ_POLICY_MEM_UP_OVRD	(1 << 1)
#define GEN11_COMMON_SLICE_CHICKEN3		(0x7304)
#define   GEN12_DISABLE_CPS_AWARE_COLOR_PIPE	(1 << 9)
// Command Streamer chicken bits — GPU preemption granularity control
#define GEN8_CS_CHICKEN1			(0x2580)
#define   GEN9_PREEMPT_3D_OBJECT_LEVEL		(1 << 0)
#define   GEN9_PREEMPT_GPGPU_LEVEL(hi, lo)	(((hi) << 2) | ((lo) << 1))
#define   GEN9_PREEMPT_GPGPU_MID_THREAD_LEVEL	GEN9_PREEMPT_GPGPU_LEVEL(0, 0)
#define   GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL	GEN9_PREEMPT_GPGPU_LEVEL(0, 1)
#define   GEN9_PREEMPT_GPGPU_COMMAND_LEVEL	GEN9_PREEMPT_GPGPU_LEVEL(1, 0)
#define   GEN9_PREEMPT_GPGPU_LEVEL_MASK		GEN9_PREEMPT_GPGPU_LEVEL(1, 1)
#define   GEN8_MCR_SLICE(slice)			(((slice) & 3) << 26)
#define   GEN8_MCR_SLICE_MASK			GEN8_MCR_SLICE(3)
#define   GEN8_MCR_SUBSLICE(subslice)		(((subslice) & 3) << 24)
#define   GEN8_MCR_SUBSLICE_MASK		GEN8_MCR_SUBSLICE(3)
// MCR = Multicast/Replicated register — selects which slice/subslice to target
#define GEN8_MCR_SELECTOR			(0xfdc)
#define PS_INVOCATION_COUNT			(0x2348)
// Force-to-non-privileged access — allows userspace to access certain MMIO regs
#define   RING_FORCE_TO_NONPRIV_ACCESS_RD	(1 << 28)
#define   RING_FORCE_TO_NONPRIV_ACCESS_WR	(2 << 28)
#define   RING_FORCE_TO_NONPRIV_ACCESS_INVALID	(3 << 28)
#define   RING_FORCE_TO_NONPRIV_ACCESS_MASK	(3 << 28)
#define   RING_FORCE_TO_NONPRIV_RANGE_1		(0 << 0)     /* CFL+ & Gen11+ */
#define   RING_FORCE_TO_NONPRIV_RANGE_4		(1 << 0)
#define GEN7_COMMON_SLICE_CHICKEN1		(0x7010)
#define HIZ_CHICKEN				(0x7018)
#define GEN7_FF_SLICE_CS_CHICKEN1		(0x20e0)
#define   GEN9_FFSC_PERCTX_PREEMPT_CTRL		(1 << 14)
#define _PICK_EVEN(__index, __a, __b) ((__a) + (__index) * ((__b) - (__a)))
// BW Buddy — TLB bandwidth allocation for memory access patterns
#define _BW_BUDDY0_CTL			0x45130
#define _BW_BUDDY1_CTL			0x45140
#define BW_BUDDY_CTL(x)			(_PICK_EVEN(x, \
							 _BW_BUDDY0_CTL, \
							 _BW_BUDDY1_CTL))
#define _BW_BUDDY0_PAGE_MASK		0x45134
#define _BW_BUDDY1_PAGE_MASK		0x45144
#define BW_BUDDY_PAGE_MASK(x)		(_PICK_EVEN(x, \
							 _BW_BUDDY0_PAGE_MASK, \
							 _BW_BUDDY1_PAGE_MASK))
#define   BW_BUDDY_DISABLE		( 1 << 31)
#define   BW_BUDDY_TLB_REQ_TIMER_MASK	REG_GENMASK(21, 16)
#define   BW_BUDDY_TLB_REQ_TIMER(x)	REG_FIELD_PREP(BW_BUDDY_TLB_REQ_TIMER_MASK, x)
// FF_MODE2 — Fixed Function mode control (geometry/tessellation timers)
#define GEN12_FF_MODE2				(0x6604)
#define XEHP_FF_MODE2				(0x6604)
#define   FF_MODE2_GS_TIMER_MASK		REG_GENMASK(31, 24)
#define   FF_MODE2_GS_TIMER_224			REG_FIELD_PREP(FF_MODE2_GS_TIMER_MASK, 224)
#define   FF_MODE2_TDS_TIMER_MASK		REG_GENMASK(23, 16)
#define   FF_MODE2_TDS_TIMER_128		REG_FIELD_PREP(FF_MODE2_TDS_TIMER_MASK, 4)
// Miscellaneous clock/power gating controls
#define GEN7_MISCCPCTL				(0x9424)
#define   GEN12_DOP_CLOCK_GATE_RENDER_ENABLE	(1 << 1)
#define GEN9_CS_DEBUG_MODE1			(0x20ec)
#define   FF_DOP_CLOCK_GATE_DISABLE		(1 << 1)
#define GEN8_ROW_CHICKEN2			(0xe4f4)
#define   GEN12_DISABLE_READ_SUPPRESSION	(1 << 15)
#define   GEN12_DISABLE_EARLY_READ		(1 << 14)
#define GEN7_FF_THREAD_MODE		(0x20a0)
#define   GEN7_FF_SCHED_MASK		0x0077070
#define   GEN8_FF_DS_REF_CNT_FFME	(1 << 19)
#define   GEN12_FF_TESSELATION_DOP_GATE_DISABLE BIT(19)
#define GEN10_SAMPLER_MODE			(0xe18c)
#define   ENABLE_SMALLPL			(1 << 15)
#define   GEN12_PUSH_CONST_DEREF_HOLD_DIS	(1 << 8)
#define GEN9_ROW_CHICKEN4			(0xe48c)
#define   GEN12_DISABLE_TDL_PUSH		(1 << 9)
#define RING_PSMI_CTL(base)			((base) + 0x50)
#define   GEN12_WAIT_FOR_EVENT_POWER_DOWN_DISABLE (1 << 7)
#define   GEN8_RC_SEMA_IDLE_MSG_DISABLE		(1 << 12)
#define _PICK_EVEN_2RANGES(__index, __c_index, __a, __b, __c, __d)		\
				   _PICK_EVEN((__index) - (__c_index), __c, __d)
// MBUS Arbiter Box — memory bus bandwidth credit allocation
#define _MBUS_ABOX0_CTL			0x45038
#define _MBUS_ABOX1_CTL			0x45048
#define _MBUS_ABOX2_CTL			0x4504C
#define MBUS_ABOX_CTL(x)							\
	(_PICK_EVEN_2RANGES(x, 2,						\
				 _MBUS_ABOX0_CTL, _MBUS_ABOX1_CTL,		\
				 _MBUS_ABOX2_CTL, _MBUS_ABOX2_CTL))
#define MBUS_ABOX_BW_CREDIT_MASK	(3 << 20)
#define MBUS_ABOX_BW_CREDIT(x)		((x) << 20)
#define MBUS_ABOX_B_CREDIT_MASK		(0xF << 16)
#define MBUS_ABOX_B_CREDIT(x)		((x) << 16)
#define MBUS_ABOX_BT_CREDIT_POOL2_MASK	(0x1F << 8)
#define MBUS_ABOX_BT_CREDIT_POOL2(x)	((x) << 8)
#define MBUS_ABOX_BT_CREDIT_POOL1_MASK	(0x1F << 0)
#define MBUS_ABOX_BT_CREDIT_POOL1(x)	((x) << 0)
// MOCS (Memory Object Control State) — cache policy overrides per ring
#define RING_CMD_CCTL(base)			((base) + 0xc4)
#define CMD_CCTL_WRITE_OVERRIDE_MASK REG_GENMASK(13, 7)
#define CMD_CCTL_READ_OVERRIDE_MASK REG_GENMASK(6, 0)
#define CMD_CCTL_MOCS_MASK (CMD_CCTL_WRITE_OVERRIDE_MASK | \
				CMD_CCTL_READ_OVERRIDE_MASK)
#define CMD_CCTL_MOCS_OVERRIDE(write, read)				      \
		(REG_FIELD_PREP(CMD_CCTL_WRITE_OVERRIDE_MASK, (write) << 1) | \
		 REG_FIELD_PREP(CMD_CCTL_READ_OVERRIDE_MASK, (read) << 1))
#define BLIT_CCTL(base)				((base) + 0x204)
#define   BLIT_CCTL_DST_MOCS_MASK		REG_GENMASK(14, 8)
#define   BLIT_CCTL_SRC_MOCS_MASK		REG_GENMASK(6, 0)
#define   BLIT_CCTL_MASK (BLIT_CCTL_DST_MOCS_MASK | \
			  BLIT_CCTL_SRC_MOCS_MASK)
#define   BLIT_CCTL_MOCS(dst, src)				       \
		(REG_FIELD_PREP(BLIT_CCTL_DST_MOCS_MASK, (dst) << 1) | \
		 REG_FIELD_PREP(BLIT_CCTL_SRC_MOCS_MASK, (src) << 1))
// DBUF — Display Buffer control, one per slice (manages display BW)
#define _DBUF_CTL_S0				0x45008
#define _DBUF_CTL_S1				0x44FE8
#define _DBUF_CTL_S2				0x44300
#define _DBUF_CTL_S3				0x44304
#define  DBUF_TRACKER_STATE_SERVICE_MASK	REG_GENMASK(23, 19)
#define  DBUF_TRACKER_STATE_SERVICE(x)		REG_FIELD_PREP(DBUF_TRACKER_STATE_SERVICE_MASK, x)
//end workar

// ─── GT fuse/topology registers ─────────────────────────────────────────────
// Read at boot to determine which EUs/slices/subslices are enabled
#define   IECPUNIT_CLKGATE_DIS			REG_BIT(22)
#define VDBOX_CGCTL3F10(base)			((base) + 0x3f10)
#define GEN11_GT_VEBOX_VDBOX_DISABLE		(0x9140)
#define GEN11_EU_DISABLE			(0x9134)
#define GEN11_GT_SLICE_ENABLE			(0x9138)
#define GEN11_GT_SUBSLICE_DISABLE		(0x913c)
#define RPM_CONFIG0				(0xd00)
#define   GEN11_GT_VDBOX_DISABLE_MASK		0xff
#define   GEN11_GT_VEBOX_DISABLE_SHIFT		0x10
#define   GEN11_GT_VEBOX_DISABLE_MASK		(0x0f << GEN11_GT_VEBOX_DISABLE_SHIFT)

#define   I915_ERROR_INSTRUCTION			(1 << 0)
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + 1)
// ─── Ring buffer MMIO (per-engine, offset from ring base) ───────────────────
#define RING_START(base)			((base) + 0x38)
#define RING_CTL(base)				((base) + 0x3c)
#define RING_EIR(base)				((base) + 0xb0)
#define RING_EMR(base)				((base) + 0xb4)
#define RING_TAIL(base)				((base) + 0x30)
#define RING_HEAD(base)				((base) + 0x34)
#define RING_HWS_PGA(base)			((base) + 0x80)
#define RING_HWSTAM(base)			((base) + 0x98)
#define RING_MI_MODE(base)			((base) + 0x9c)
#define RING_MODE_GEN7(base)		((base) + 0x29c)
#define RING_ACTHD(base)			((base) + 0x74)
#define RING_ACTHD_UDW(base)		((base) + 0x5c)
#define RING_IPEHR(base)			((base) + 0x68)
#define RING_IPEIR(base)			((base) + 0x64)
#define RING_INSTDONE(base)			((base) + 0x6c)
#define RING_ESR(base)				((base) + 0xb8)
#define RING_CTX_SIZE(base)			((base) + 0x1a0) /* context size */
#define RING_CCID(base)				((base) + 0x180) /* context control ID */
#define RING_CTX_CTRL(base)			((base) + 0x244) /* context control */
/* RING_CTX_CTRL bit definitions (GEN8+/GEN12) */
#define CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT	(1 << 0)  /* bit 0: inhibit context restore on context switch */
#define CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT	(1 << 2)  /* bit 2: inhibit context save on preemption */
#define CTX_CTRL_INHIBIT_SYN_CTX_SWITCH	(1 << 3)  /* bit 3: force immediate (async) context switch */
/* Writing bits 0+2+3 = 0x0D forces context abandon: no restore, no save, immediate switch.
 * This is the i915 engine-quiesce sequence used before __intel_gt_disable(). */
#define CTX_CTRL_FORCE_ABANDON \
	(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT | CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT | CTX_CTRL_INHIBIT_SYN_CTX_SWITCH)
#define RING_ELSP(base)				((base) + 0x230) /* ExecList Submission Port */
#define RING_EXECLIST_STATUS(base)	((base) + 0x234)
#define RING_CONTEXT_STATUS_PTR(base) ((base) + 0x3a0)
#define RING_MODE(base)				((base) + 0x29c)
#define RING_FAULT_REG(base)		((base) + 0x150)
#define RING_DMA_FADD(base)			((base) + 0x78)
#define RING_DMA_FADD_UDW(base)		((base) + 0x60)
#define RING_INSTPM(base)			((base) + 0xC0)
#define RING_CONTEXT_STATUS_BUF(base, idx)    ((base) + 0x370 + (idx) * 8)
#define RING_CONTEXT_STATUS_BUF_HI(base, idx) ((base) + 0x374 + (idx) * 8)

// Global error/fault registers
#define ERROR_GEN6				0x40A0
#define GEN12_RING_FAULT_REG	0xCEC4
#define GEN8_FAULT_TLB_DATA0	0x4B10
#define GEN8_FAULT_TLB_DATA1	0x4B14

// Engine reset registers (Gen6+)
#define GEN6_GDRST				0x941c
#define   GEN6_GRDOM_FULL		(1 << 0)
#define   GEN6_GRDOM_RENDER		(1 << 1)
#define   GEN6_GRDOM_MEDIA		(1 << 2)
#define   GEN6_GRDOM_BLT		(1 << 3)

// Per-engine reset control (Gen12+)
#define RING_RESET_CTL(base)		((base) + 0xd0)
#define   RESET_CTL_REQUEST_RESET	(1 << 0)
#define   RESET_CTL_READY_TO_RESET	(1 << 1)
#define   RESET_CTL_CAT_ERROR		(1 << 2)

// GGTT PTE base within BAR0 (Gen8+: 8MB into MMIO BAR, each PTE is 8 bytes)
#define GEN8_GGTT_PTE_BASE		0x800000
#define GGTT_PTE_LO(page)		(GEN8_GGTT_PTE_BASE + (page) * 8)
#define GGTT_PTE_HI(page)		(GEN8_GGTT_PTE_BASE + (page) * 8 + 4)
#define   GEN11_GFX_DISABLE_LEGACY_MODE		(1 << 3)
#define   STOP_RING				REG_BIT(8)
// Engine ring base addresses (RCS=render, BCS=blitter, VCS/BSD=video, VECS=video enhance)
#define RENDER_RING_BASE	0x02000
#define GEN12_COMPUTE0_RING_BASE	0x1a000
#define BLT_RING_BASE		0x22000
#define GEN11_BSD_RING_BASE	0x1c0000
#define GEN11_BSD3_RING_BASE	0x1d0000
#define GEN11_VEBOX_RING_BASE	0x1c8000

// ─── CDCLK (Core Display Clock) ─────────────────────────────────────────────
static constexpr uint32_t ICL_REG_CDCLK_CTL = 0x46000;
// DSSM = Display Subsystem Status/Mode — contains PLL reference clock info
static constexpr uint32_t ICL_REG_DSSM = 0x51004;


enum ICLReferenceClockFrequency {
	
	// 24 MHz
	ICL_REF_CLOCK_FREQ_24_0 = 0x0,
	
	// 19.2 MHz
	ICL_REF_CLOCK_FREQ_19_2 = 0x1,
	
	// 38.4 MHz
	ICL_REF_CLOCK_FREQ_38_4 = 0x2
};


enum ICLCoreDisplayClockDecimalFrequency {
	
	// 172.8 MHz
	ICL_CDCLK_FREQ_172_8 = 0x158,
	
	// 180 MHz
	ICL_CDCLK_FREQ_180_0 = 0x166,
	
	// 192 MHz
	ICL_CDCLK_FREQ_192_0 = 0x17E,
	
	// 307.2 MHz
	ICL_CDCLK_FREQ_307_2 = 0x264,
	
	// 312 MHz
	ICL_CDCLK_FREQ_312_0 = 0x26E,
	
	// 552 MHz
	ICL_CDCLK_FREQ_552_0 = 0x44E,
	
	// 556.8 MHz
	ICL_CDCLK_FREQ_556_8 = 0x458,
	
	// 648 MHz
	ICL_CDCLK_FREQ_648_0 = 0x50E,
	
	// 652.8 MHz
	ICL_CDCLK_FREQ_652_8 = 0x518
};

static constexpr uint32_t ICL_CDCLK_DEC_FREQ_THRESHOLD = ICL_CDCLK_FREQ_648_0;


// PLL output = refclk × ratio (all produce ~1296 MHz)
static constexpr uint32_t ICL_CDCLK_PLL_FREQ_REF_24_0 = 24000000 * 54;
static constexpr uint32_t ICL_CDCLK_PLL_FREQ_REF_19_2 = 19200000 * 68;
static constexpr uint32_t ICL_CDCLK_PLL_FREQ_REF_38_4 = 38400000 * 34;

//#define  BXT_CDCLK_CD2X_DIV_SEL_MASK	REG_GENMASK(23, 22)
#define  BXT_CDCLK_CD2X_DIV_SEL_MASK	(3 << 22)
#define  BXT_CDCLK_CD2X_DIV_SEL_1	(0 << 22)
#define  BXT_CDCLK_CD2X_DIV_SEL_1_5	(1 << 22)
#define  BXT_CDCLK_CD2X_DIV_SEL_2	(2 << 22)
#define  BXT_CDCLK_CD2X_DIV_SEL_4	(3 << 22)
#define  BXT_CDCLK_CD2X_PIPE(pipe)	((pipe) << 20)
#define  CDCLK_DIVMUX_CD_OVERRIDE	(1 << 19)
#define  BXT_CDCLK_CD2X_PIPE_NONE	BXT_CDCLK_CD2X_PIPE(3)
#define  ICL_CDCLK_CD2X_PIPE_NONE	(7 << 19)
#define  BXT_CDCLK_SSA_PRECHARGE_ENABLE	(1 << 16)
#define  CDCLK_FREQ_DECIMAL_MASK	(0x7ff)
#define SKL_DSSM				(0x51004)
#define ICL_DSSM_CDCLK_PLL_REFCLK_MASK		(7 << 29)
#define ICL_DSSM_CDCLK_PLL_REFCLK_24MHz		(0 << 29)
#define ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz	(1 << 29)
#define ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz	(2 << 29)
#define BXT_DE_PLL_ENABLE		(0x46070)
#define   BXT_DE_PLL_PLL_ENABLE		(1 << 31)
#define   BXT_DE_PLL_LOCK		(1 << 30)
#define   BXT_DE_PLL_FREQ_REQ		(1 << 23)
#define   BXT_DE_PLL_FREQ_REQ_ACK	(1 << 22)
#define   ICL_CDCLK_PLL_RATIO(x)	(x)
#define   ICL_CDCLK_PLL_RATIO_MASK	0xff
#define CDCLK_CTL			(0x46000)

#define DIV_ROUND_CLOSEST(x, divisor)(			\
{							\
__typeof(x) __x = x;				\
__typeof(divisor) __d = divisor;			\
	(((__typeof(x))-1) > 0 ||				\
	 ((__typeof(divisor))-1) > 0 || (__x) > 0) ?	\
		(((__x) + ((__d) / 2)) / (__d)) :	\
		(((__x) - ((__d) / 2)) / (__d));	\
}							\
)

// ─── Interrupt registers ────────────────────────────────────────────────────
#define SOUTH_CHICKEN1		(0xc2000)
#define GEN8_MASTER_IRQ		(0x44200)  // same offset as GEN11_DISPLAY_INT_CTL

// Hotplug detection registers (PCH south bridge side)
#define PCH_PORT_HOTPLUG		(0xc4030)
#define SHOTPLUG_CTL_DDI		(0xc4030)  // DDI = Digital Display Interface
#define SHOTPLUG_CTL_TC			(0xc4034)  // TC  = Type-C
#define SHPD_FILTER_CNT			(0xc4038)

// SDE = South Display Engine interrupt mask/identity/enable
#define SDEIMR (0xc4004)
#define SDEIIR (0xc4008)
#define SDEIER (0xc400c)

// DE = Display Engine; HPD = Hot Plug Detect; ISR/IMR/IIR/IER = status/mask/identity/enable
#define GEN11_DE_HPD_ISR		(0x44470)
#define GEN11_DE_HPD_IMR		(0x44474)
#define GEN11_DE_HPD_IIR		(0x44478)
#define GEN11_DE_HPD_IER		(0x4447c)

#define GEN8_DE_MISC_ISR (0x44460)
#define GEN8_DE_MISC_IMR  (0x44464)
#define GEN8_DE_MISC_IIR  (0x44468)
#define GEN8_DE_MISC_IER  (0x4446c)

#define GEN8_DE_PIPE_ISR_A  (0x44400)
#define GEN8_DE_PIPE_IMR_A  (0x44404)
#define GEN8_DE_PIPE_IIR_A  (0x44408)
#define GEN8_DE_PIPE_IER_A  (0x4440c)

#define GEN8_DE_PIPE_ISR_B  (0x44410)
#define GEN8_DE_PIPE_IMR_B  (0x44414)
#define GEN8_DE_PIPE_IIR_B  (0x44418)
#define GEN8_DE_PIPE_IER_B  (0x4441c)

#define GEN8_DE_PIPE_ISR_C  (0x44420)
#define GEN8_DE_PIPE_IMR_C  (0x44424)
#define GEN8_DE_PIPE_IIR_C  (0x44428)
#define GEN8_DE_PIPE_IER_C  (0x4442c)



// ─── GPU frequency / power management ───────────────────────────────────────
// MCHBAR mirror: memory controller hub BAR mapped into GT MMIO space
constexpr uint32_t MCHBAR_MIRROR_BASE_SNB = 0x140000;
// RP_STATE_CAP: contains RP0 (max), RP1 (efficient), RPn (min) frequency caps
constexpr uint32_t GEN6_RP_STATE_CAP = MCHBAR_MIRROR_BASE_SNB + 0x5998;

constexpr uint32_t GEN9_FREQUENCY_SHIFT = 23;
constexpr uint32_t GEN9_FREQ_SCALER  = 3;

// ─── Forcewake ──────────────────────────────────────────────────────────────
// Forcewake keeps GT power domains awake for MMIO access.
// Without it, reads return 0xFFFFFFFF from sleeping domains.
constexpr uint32_t FORCEWAKE_KERNEL_FALLBACK = 1 << 15;
constexpr uint32_t FORCEWAKE_ACK_TIMEOUT_MS = 50;

// Request registers — write here to wake a domain
constexpr uint32_t FORCEWAKE_MEDIA_GEN9 = 0xa270;
constexpr uint32_t FORCEWAKE_RENDER_GEN9 = 0xa278;
constexpr uint32_t FORCEWAKE_BLITTER_GEN9 = 0xa188;

// Acknowledge registers — poll these to confirm domain is awake
constexpr uint32_t FORCEWAKE_ACK_MEDIA_GEN9 = 0x0D88;
constexpr uint32_t FORCEWAKE_ACK_RENDER_GEN9 = 0x0D84;
constexpr uint32_t FORCEWAKE_ACK_BLITTER_GEN9 = 0x130044;

#define FORCEWAKE_MEDIA_VDBOX_GEN11(n)		(0xa540 + (n) * 4)
#define FORCEWAKE_MEDIA_VEBOX_GEN11(n)		(0xa560 + (n) * 4)
#define FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(n)	(0xd50 + (n) * 4)
#define FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(n)	(0xd70 + (n) * 4)
#define FORCEWAKE_GT_GEN9			(0xa188)
#define FORCEWAKE_ACK_GT_GEN9			(0x130044)
#define FORCEWAKE_REQ_GSC			(0xa618)
#define FORCEWAKE_ACK_GSC			(0xdf8)

static inline unsigned long find_first_bit(const unsigned long *addr, unsigned long size) {
	unsigned long val = *addr;
	if (!val || size == 0) return size;
	return __builtin_ctzl(val);
}

static inline unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
	if (offset >= size) return size;
	unsigned long val = *addr & (~0UL << offset);
	if (!val) return size;
	return __builtin_ctzl(val);
}

#define for_each_set_bit(bit, addr, size) \
	for ((bit) = find_first_bit((addr), (size));		\
		 (bit) < (size);					\
		 (bit) = find_next_bit((addr), (size), (bit) + 1))

#define __bf_shf(x) (__builtin_ffsll(x) - 1)
#define REG_FIELD_PREP(__mask, __val) \
((uint32_t)((__typeof(__mask))(__val) << __bf_shf(__mask)) & (__mask))

// Apple's command streamer type enum (matches IGAccel internal enum)
enum IGHwCsType
{
	kIGHwCsTypeRCS,     //  0 — Render Command Streamer
	kIGHwCsTypeCCS,     //  1 — Compute Command Streamer (Gen12.5+)
	kIGHwCsTypeBCS,     //  2 — Blitter Command Streamer
	kIGHwCsTypeVCS0,    //  3 — Video Command Streamer 0
	kIGHwCsTypeVCS2,    //  4 — Video Command Streamer 2
	kIGHwCsTypeVECS0,   //  5 — Video Enhancement CS 0
};

// Apple's per-engine descriptor — MMIO offsets for ExecList, context, status, forcewake
struct IGHwCsDesc {
	IGHwCsType   type;
	uint32_t     csMask;
	const char * name;
	const char * label;
	uint32_t     mmioExecListSubmitPort;
	uint32_t     mmioExecListSubmitQueue;
	uint32_t     mmioExecListControl;
	uint32_t     mmioExecListStatus;
	uint32_t     mmioContextStatusPointer;
	uint32_t     mmioContextStatusBuffer;
	uint32_t     mmioContextStatusPort;
	uint32_t     mmioContextStatusFifoStatus;
	uint32_t     mmioGlobalStatusPage;
	uint32_t     mmioGfxMode;
	uint32_t     mmioResetCtrl;
	uint32_t     mmioErrorIdentity;
	uint32_t     mmioErrorMask;
	uint32_t     mmioTimeStamp;
	uint32_t     mmioGpr0;
	uint32_t     fuseMask;
	uint32_t     contextSizeBytes;
	int32_t      stampIndexRangeMin;
	int32_t      stampIndexRangeMax;
	int32_t      stampIndexRangeSize;
	IOSelect     contextSwitchInterruptType;
	IOSelect     flushNotifyInterruptType;
	IOSelect     errorInterruptType;
	uint32_t     mmioForcewakeReq;
	uint32_t     mmioForcewakeAck;
};


// BW Buddy page masks — TLB request size depends on DRAM type & channel count
struct buddy_page_mask {
	uint32_t page_mask;
	uint8_t type;
	uint8_t num_channels;
};
enum intel_dram_type {
	INTEL_DRAM_UNKNOWN,
	INTEL_DRAM_DDR3,
	INTEL_DRAM_DDR4,
	INTEL_DRAM_LPDDR3,
	INTEL_DRAM_LPDDR4,
	INTEL_DRAM_DDR5,
	INTEL_DRAM_LPDDR5,
	INTEL_DRAM_GDDR,
} ;

static const struct buddy_page_mask tgl_buddy_page_masks[] = {
	{ .num_channels = 1, .type = INTEL_DRAM_DDR4,   .page_mask = 0xF },
	{ .num_channels = 1, .type = INTEL_DRAM_DDR5,	.page_mask = 0xF },
	{ .num_channels = 2, .type = INTEL_DRAM_LPDDR4, .page_mask = 0x1C },
	{ .num_channels = 2, .type = INTEL_DRAM_LPDDR5, .page_mask = 0x1C },
	{ .num_channels = 2, .type = INTEL_DRAM_DDR4,   .page_mask = 0x1F },
	{ .num_channels = 2, .type = INTEL_DRAM_DDR5,   .page_mask = 0x1E },
	{ .num_channels = 4, .type = INTEL_DRAM_LPDDR4, .page_mask = 0x38 },
	{ .num_channels = 4, .type = INTEL_DRAM_LPDDR5, .page_mask = 0x38 },
	{}
};

#define GENMASK(high, low) \
	(((0xFFFFFFFF) << (low)) & (0xFFFFFFFF >> (32 - 1 - (high))))

#define REG_GENMASK(__high, __low)	GENMASK(__high, __low)


// Legacy 3-bit forcewake domain bitmask (Gen9 style — before per-engine domains)
enum FORCEWAKE_DOM_BITS : unsigned {
	DOM_RENDER = 0b001,
	DOM_MEDIA = 0b010,
	DOM_BLITTER = 0b100,
	DOM_LAST = DOM_BLITTER,
	DOM_FIRST = DOM_RENDER
};
/*
enum forcewake_domain_id {
	FW_DOMAIN_ID_RENDER = 0,
	FW_DOMAIN_ID_BLITTER,
	FW_DOMAIN_ID_MEDIA,
	FW_DOMAIN_ID_MEDIA_VDBOX0,
	FW_DOMAIN_ID_MEDIA_VDBOX1,
	FW_DOMAIN_ID_MEDIA_VDBOX2,
	FW_DOMAIN_ID_MEDIA_VDBOX3,
	FW_DOMAIN_ID_MEDIA_VEBOX0,
	FW_DOMAIN_ID_MEDIA_VEBOX1,

	FW_DOMAIN_ID_COUNT
};*/

// Gen11+ forcewake domain IDs — separate domain per media engine instance
enum forcewake_domain_id {
	FW_DOMAIN_ID_RENDER = 0,
	FW_DOMAIN_ID_GT,        /* also includes blitter engine */
	FW_DOMAIN_ID_MEDIA,
	FW_DOMAIN_ID_MEDIA_VDBOX0,
	FW_DOMAIN_ID_MEDIA_VDBOX1,
	FW_DOMAIN_ID_MEDIA_VDBOX2,
	FW_DOMAIN_ID_MEDIA_VDBOX3,
	FW_DOMAIN_ID_MEDIA_VDBOX4,
	FW_DOMAIN_ID_MEDIA_VDBOX5,
	FW_DOMAIN_ID_MEDIA_VDBOX6,
	FW_DOMAIN_ID_MEDIA_VDBOX7,
	FW_DOMAIN_ID_MEDIA_VEBOX0,
	FW_DOMAIN_ID_MEDIA_VEBOX1,
	FW_DOMAIN_ID_MEDIA_VEBOX2,
	FW_DOMAIN_ID_MEDIA_VEBOX3,
	FW_DOMAIN_ID_GSC,

	FW_DOMAIN_ID_COUNT
};
#define BIT(n) (1<< n)
enum forcewake_domains {
	FORCEWAKE_RENDER	= BIT(FW_DOMAIN_ID_RENDER),
	FORCEWAKE_GT		= BIT(FW_DOMAIN_ID_GT),
	FORCEWAKE_MEDIA		= BIT(FW_DOMAIN_ID_MEDIA),
	FORCEWAKE_MEDIA_VDBOX0	= BIT(FW_DOMAIN_ID_MEDIA_VDBOX0),
	FORCEWAKE_MEDIA_VDBOX1	= BIT(FW_DOMAIN_ID_MEDIA_VDBOX1),
	FORCEWAKE_MEDIA_VDBOX2	= BIT(FW_DOMAIN_ID_MEDIA_VDBOX2),
	FORCEWAKE_MEDIA_VDBOX3	= BIT(FW_DOMAIN_ID_MEDIA_VDBOX3),
	FORCEWAKE_MEDIA_VDBOX4	= BIT(FW_DOMAIN_ID_MEDIA_VDBOX4),
	FORCEWAKE_MEDIA_VDBOX5	= BIT(FW_DOMAIN_ID_MEDIA_VDBOX5),
	FORCEWAKE_MEDIA_VDBOX6	= BIT(FW_DOMAIN_ID_MEDIA_VDBOX6),
	FORCEWAKE_MEDIA_VDBOX7	= BIT(FW_DOMAIN_ID_MEDIA_VDBOX7),
	FORCEWAKE_MEDIA_VEBOX0	= BIT(FW_DOMAIN_ID_MEDIA_VEBOX0),
	FORCEWAKE_MEDIA_VEBOX1	= BIT(FW_DOMAIN_ID_MEDIA_VEBOX1),
	FORCEWAKE_MEDIA_VEBOX2	= BIT(FW_DOMAIN_ID_MEDIA_VEBOX2),
	FORCEWAKE_MEDIA_VEBOX3	= BIT(FW_DOMAIN_ID_MEDIA_VEBOX3),
	FORCEWAKE_GSC		= BIT(FW_DOMAIN_ID_GSC),

	FORCEWAKE_ALL = BIT(FW_DOMAIN_ID_COUNT) - 1,
};






// Engine instance IDs (i915 convention)
enum intel_engine_id {
	RCS0 = 0,  // Render
	BCS0,
	BCS1,
	BCS2,
	BCS3,
	BCS4,
	BCS5,
	BCS6,
	BCS7,
	BCS8,
#define _BCS(n) (BCS0 + (n))
	VCS0,
	VCS1,
	VCS2,
	VCS3,
	VCS4,
	VCS5,
	VCS6,
	VCS7,
#define _VCS(n) (VCS0 + (n))
	VECS0,
	VECS1,
	VECS2,
	VECS3,
#define _VECS(n) (VECS0 + (n))
	CCS0,
	CCS1,
	CCS2,
	CCS3,
#define _CCS(n) (CCS0 + (n))
	GSC0,
	I915_NUM_ENGINES
#define INVALID_ENGINE ((enum intel_engine_id)-1)
};
#define __HAS_ENGINE(engine_mask, id) ((engine_mask) & BIT(id))

// Registers below 0x40000 or above 0x116000 need forcewake to be held
#define NEEDS_FORCE_WAKE(reg) ({ \
	u32 __reg = (reg); \
	__reg < 0x40000 || __reg >= 0x116000; \
})

#define GEN_FW_RANGE(s, e, d) \
	{ .start = (s), .end = (e), .domains = (d) }

// ─── GT workaround registers ────────────────────────────────────────────────
#define GEN11_GACB_PERF_CTRL			(0x4b80)
#define   GEN11_HASH_CTRL_MASK			(0x3 << 12 | 0xf << 0)
#define   GEN11_HASH_CTRL_BIT0			(1 << 0)
#define   GEN11_HASH_CTRL_BIT4			(1 << 12)
#define GEN11_LSN_UNSLCVC			(0xb43c)
#define   GEN11_LSN_UNSLCVC_GAFS_HALF_CL2_MAXALLOC	(1 << 9)
#define   GEN11_LSN_UNSLCVC_GAFS_HALF_SF_MAXALLOC	(1 << 7)
#define GEN8_GAMW_ECO_DEV_RW_IA			(0x4080)
#define   GAMW_ECO_ENABLE_64K_IPS_FIELD		0xF
#define   GAMW_ECO_DEV_CTX_RELOAD_DISABLE	(1 << 7)
#define GAMT_CHKN_BIT_REG			(0x4ab8)
#define   GAMT_CHKN_DISABLE_L3_COH_PIPE		(1 << 31)
#define   GAMT_CHKN_DISABLE_DYNAMIC_CREDIT_SHARING	(1 << 28)
#define   GAMT_CHKN_DISABLE_I2M_CYCLE_ON_WR_PORT	(1 << 24)
#define UNSLICE_UNIT_LEVEL_CLKGATE2		(0x94e4)
#define   VSUNIT_CLKGATE_DIS_TGL		BIT(19)
#define   PSDUNIT_CLKGATE_DIS			BIT(5)
#define UNSLICE_UNIT_LEVEL_CLKGATE		(0x9434)
#define   VFUNIT_CLKGATE_DIS			BIT(20)
#define   CG3DDISCFEG_CLKGATE_DIS		BIT(17) /* DG2 */
#define   GAMEDIA_CLKGATE_DIS			BIT(11)
#define   HSUNIT_CLKGATE_DIS			BIT(8)
#define   VSUNIT_CLKGATE_DIS			BIT(3)
#define GEN10_DFR_RATIO_EN_AND_CHICKEN		(0x9550)
#define   DFR_DISABLE				(1 << 9)
// DSS = Dual Sub-Slice clock gating (Gen11+ EU topology)
#define GEN11_SUBSLICE_UNIT_LEVEL_CLKGATE	(0x9524)
#define   DSS_ROUTER_CLKGATE_DIS		BIT(28)
#define   GWUNIT_CLKGATE_DIS			BIT(16)

// Reset handshake — coordinates GT/PCH reset sequencing
#define HSW_NDE_RSTWRN_OPT	(0x46408)
#define  MTL_RESET_PICA_HANDSHAKE_EN	BIT(6)
#define  RESET_PCH_HANDSHAKE_ENABLE	BIT(4)

// Masked register write helpers — upper 16 bits = mask, lower 16 bits = value
#define _MASKED_FIELD(mask, value) ((mask) << 16 | (value))
#define _MASKED_BIT_ENABLE(a)	(_MASKED_FIELD((a), a))
#define _MASKED_BIT_DISABLE(a)	(_MASKED_FIELD((a), 0))
#define GEN9_GAMT_ECO_REG_RW_IA (0x4ab0)
#define   GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS	(1 << 18)


struct intel_forcewake_range {
	uint32_t start;
	uint32_t end;

	enum forcewake_domains domains;
};

// Gen11 forcewake range table — maps MMIO ranges to their required power domain
const struct intel_forcewake_range __gen11_fw_ranges[] = {
	//GEN_FW_RANGE(0x0, 0x1fff, 0), /* uncore range */
	GEN_FW_RANGE(0x2000, 0x26ff, FORCEWAKE_RENDER),
	GEN_FW_RANGE(0x2700, 0x2fff, FORCEWAKE_GT),
	GEN_FW_RANGE(0x3000, 0x3fff, FORCEWAKE_RENDER),
	GEN_FW_RANGE(0x4000, 0x51ff, FORCEWAKE_GT),
	GEN_FW_RANGE(0x5200, 0x7fff, FORCEWAKE_RENDER),
	GEN_FW_RANGE(0x8000, 0x813f, FORCEWAKE_GT),
	GEN_FW_RANGE(0x8140, 0x815f, FORCEWAKE_RENDER),
	GEN_FW_RANGE(0x8160, 0x82ff, FORCEWAKE_GT),
	GEN_FW_RANGE(0x8300, 0x84ff, FORCEWAKE_RENDER),
	GEN_FW_RANGE(0x8500, 0x87ff, FORCEWAKE_GT),
	//GEN_FW_RANGE(0x8800, 0x8bff, 0),
	GEN_FW_RANGE(0x8c00, 0x8cff, FORCEWAKE_RENDER),
	GEN_FW_RANGE(0x8d00, 0x94cf, FORCEWAKE_GT),
	GEN_FW_RANGE(0x94d0, 0x955f, FORCEWAKE_RENDER),
	//GEN_FW_RANGE(0x9560, 0x95ff, 0),
	GEN_FW_RANGE(0x9600, 0xafff, FORCEWAKE_GT),
	GEN_FW_RANGE(0xb000, 0xb47f, FORCEWAKE_RENDER),
	GEN_FW_RANGE(0xb480, 0xdeff, FORCEWAKE_GT),
	GEN_FW_RANGE(0xdf00, 0xe8ff, FORCEWAKE_RENDER),
	GEN_FW_RANGE(0xe900, 0x16dff, FORCEWAKE_GT),
	GEN_FW_RANGE(0x16e00, 0x19fff, FORCEWAKE_RENDER),
	GEN_FW_RANGE(0x1a000, 0x23fff, FORCEWAKE_GT),
	//GEN_FW_RANGE(0x24000, 0x2407f, 0),
	GEN_FW_RANGE(0x24080, 0x2417f, FORCEWAKE_GT),
	GEN_FW_RANGE(0x24180, 0x242ff, FORCEWAKE_RENDER),
	GEN_FW_RANGE(0x24300, 0x243ff, FORCEWAKE_GT),
	GEN_FW_RANGE(0x24400, 0x24fff, FORCEWAKE_RENDER),
	GEN_FW_RANGE(0x25000, 0x3ffff, FORCEWAKE_GT),
	//GEN_FW_RANGE(0x40000, 0x1bffff, 0),
	GEN_FW_RANGE(0x1c0000, 0x1c3fff, FORCEWAKE_MEDIA_VDBOX0),
	//GEN_FW_RANGE(0x1c4000, 0x1c7fff, 0),
	GEN_FW_RANGE(0x1c8000, 0x1cffff, FORCEWAKE_MEDIA_VEBOX0),
	GEN_FW_RANGE(0x1d0000, 0x1d3fff, FORCEWAKE_MEDIA_VDBOX2),
	//GEN_FW_RANGE(0x1d4000, 0x1dbfff, 0)
};
// eDRAM capability register (Haswell/Broadwell, not present on ICL+)
#define  HSW_EDRAM_CAP				(0x120010)
#define    EDRAM_NUM_BANKS(cap)			(((cap) >> 1) & 0xf)
#define    EDRAM_WAYS_IDX(cap)			(((cap) >> 5) & 0x7)
#define    EDRAM_SETS_IDX(cap)			(((cap) >> 8) & 0x3)
#define   FORCEWAKE_KERNEL			BIT(0)


// ─── GT interrupt registers ─────────────────────────────────────────────────
// DW0/DW1 identify which engine fired; identity regs carry interrupt details
#define GEN11_GT_INTR_DW0		(0x190018)
#define  GEN11_CSME			(31)
#define  GEN11_GUNIT			(28)
#define  GEN11_GUC			(25)
#define  GEN11_WDPERF			(20)
#define  GEN11_KCR			(19)
#define  GEN11_GTPM			(16)
#define  GEN11_BCS			(15)
#define  GEN11_RCS0			(0)

#define GEN11_GT_INTR_DW1		(0x19001c)
#define  GEN11_VECS(x)			(31 - (x))
#define  GEN11_VCS(x)			(x)

#define GEN11_GT_INTR_DW(x)		(0x190018 + ((x) * 4))

#define GEN11_INTR_IDENTITY_REG0	(0x190060)
#define GEN11_INTR_IDENTITY_REG1	(0x190064)
#define  GEN11_INTR_DATA_VALID		(1 << 31)
//#define  GEN11_INTR_ENGINE_CLASS(x)	(((x) & GENMASK(18, 16)) >> 16)
//#define  GEN11_INTR_ENGINE_INSTANCE(x)	(((x) & GENMASK(25, 20)) >> 20)
#define  GEN11_INTR_ENGINE_INTR(x)	((x) & 0xffff)
/* irq instances for OTHER_CLASS */
#define OTHER_GUC_INSTANCE	0
#define OTHER_GTPM_INSTANCE	1

#define GEN11_INTR_IDENTITY_REG(x)	(0x190060 + ((x) * 4))

#define GEN11_IIR_REG0_SELECTOR		(0x190070)
#define GEN11_IIR_REG1_SELECTOR		(0x190074)

#define GEN11_IIR_REG_SELECTOR(x)	(0x190070 + ((x) * 4))

// Per-engine interrupt enable/mask registers
#define GEN11_RENDER_COPY_INTR_ENABLE	(0x190030)
#define GEN11_VCS_VECS_INTR_ENABLE	(0x190034)
#define GEN11_GUC_SG_INTR_ENABLE	(0x190038)
#define GEN11_GPM_WGBOXPERF_INTR_ENABLE	(0x19003c)
#define GEN11_CRYPTO_RSVD_INTR_ENABLE	(0x190040)
#define GEN11_GUNIT_CSME_INTR_ENABLE	(0x190044)

#define GEN11_RCS0_RSVD_INTR_MASK	(0x190090)
#define GEN11_BCS_RSVD_INTR_MASK	(0x1900a0)
#define GEN11_VCS0_VCS1_INTR_MASK	(0x1900a8)
#define GEN11_VCS2_VCS3_INTR_MASK	(0x1900ac)
#define GEN12_VCS4_VCS5_INTR_MASK	(0x1900b0)
#define GEN12_VCS6_VCS7_INTR_MASK	(0x1900b4)
#define GEN11_VECS0_VECS1_INTR_MASK	(0x1900d0)
#define GEN12_VECS2_VECS3_INTR_MASK	(0x1900d4)
#define GEN11_GUC_SG_INTR_MASK		(0x1900e8)
#define GEN11_GPM_WGBOXPERF_INTR_MASK	(0x1900ec)
#define GEN11_CRYPTO_RSVD_INTR_MASK	(0x1900f0)
#define GEN11_GUNIT_CSME_INTR_MASK	(0x1900f4)

// GT interrupt status bits — per-engine interrupt cause flags
#define GT_BLT_FLUSHDW_NOTIFY_INTERRUPT		(1 << 26)
#define GT_BLT_CS_ERROR_INTERRUPT		(1 << 25)
#define GT_BLT_USER_INTERRUPT			(1 << 22)
#define GT_BSD_CS_ERROR_INTERRUPT		(1 << 15)
#define GT_BSD_USER_INTERRUPT			(1 << 12)
#define GT_RENDER_L3_PARITY_ERROR_INTERRUPT_S1	(1 << 11) /* hsw+; rsvd on snb, ivb, vlv */
#define GT_CONTEXT_SWITCH_INTERRUPT		(1 <<  8)
#define GT_RENDER_L3_PARITY_ERROR_INTERRUPT	(1 <<  5) /* !snb */
#define GT_RENDER_PIPECTL_NOTIFY_INTERRUPT	(1 <<  4)
#define GT_RENDER_CS_MASTER_ERROR_INTERRUPT	(1 <<  3)
#define GT_RENDER_SYNC_STATUS_INTERRUPT		(1 <<  2)
#define GT_RENDER_DEBUG_INTERRUPT		(1 <<  1)
#define GT_RENDER_USER_INTERRUPT		(1 <<  0)

// Master interrupt control — top-level IRQ enable/routing
#define GEN11_GFX_MSTR_IRQ		(0x190010)
#define  GEN11_MASTER_IRQ		(1 << 31)
#define  GEN11_PCU_IRQ			(1 << 30)
#define  GEN11_GU_MISC_IRQ		(1 << 29)
#define  GEN11_DISPLAY_IRQ		(1 << 16)
#define  GEN11_GT_DW_IRQ(x)		(1 << (x))
#define  GEN11_GT_DW1_IRQ		(1 << 1)
#define  GEN11_GT_DW0_IRQ		(1 << 0)

// Display interrupt control — enables display engine IRQ routing to CPU
#define GEN11_DISPLAY_INT_CTL		(0x44200)
#define  GEN11_DISPLAY_IRQ_ENABLE	(1 << 31)
#define  GEN11_AUDIO_CODEC_IRQ		(1 << 24)
#define  GEN11_DE_PCH_IRQ		(1 << 23)
#define  GEN11_DE_MISC_IRQ		(1 << 22)
#define  GEN11_DE_HPD_IRQ		(1 << 21)
#define  GEN11_DE_PORT_IRQ		(1 << 20)
#define  GEN11_DE_PIPE_C		(1 << 18)
#define  GEN11_DE_PIPE_B		(1 << 17)
#define  GEN11_DE_PIPE_A		(1 << 16)

#define GT_CS_MASTER_ERROR_INTERRUPT		(3)
#define GT_WAIT_SEMAPHORE_INTERRUPT		(11)

enum ack_type {
	ACK_CLEAR = 0,
	ACK_SET
};

// RPS = Render Performance State frequency caps (read from RP_STATE_CAP)
struct intel_rps_freq_caps {
	uint8_t rp0_freq;   // RP0 — max turbo frequency
	uint8_t rp1_freq;   // RP1 — efficient/nominal frequency
	uint8_t min_freq;   // RPn — minimum frequency
};

// Map forcewake domain bitmask → MMIO request register
constexpr uint32_t regForDom(unsigned d) {
	
	
	if (d == FORCEWAKE_GT)
		return FORCEWAKE_GT_GEN9;
	if (d == FORCEWAKE_RENDER)
		return FORCEWAKE_RENDER_GEN9;
	
		
	if (d == FORCEWAKE_MEDIA_VDBOX0)
			return FORCEWAKE_MEDIA_VDBOX_GEN11(0);
	if (d == FORCEWAKE_MEDIA_VDBOX1)
			return FORCEWAKE_MEDIA_VDBOX_GEN11(1);
	if (d == FORCEWAKE_MEDIA_VDBOX2)
			return FORCEWAKE_MEDIA_VDBOX_GEN11(2);
	if (d == FORCEWAKE_MEDIA_VDBOX3)
			return FORCEWAKE_MEDIA_VDBOX_GEN11(3);
	if (d == FORCEWAKE_MEDIA_VDBOX4)
			return FORCEWAKE_MEDIA_VDBOX_GEN11(4);
	if (d == FORCEWAKE_MEDIA_VDBOX5)
			return FORCEWAKE_MEDIA_VDBOX_GEN11(5);
	if (d == FORCEWAKE_MEDIA_VDBOX6)
			return FORCEWAKE_MEDIA_VDBOX_GEN11(6);
	if (d == FORCEWAKE_MEDIA_VDBOX7)
			return FORCEWAKE_MEDIA_VDBOX_GEN11(7);


	if (d == FORCEWAKE_MEDIA_VEBOX0)
			return FORCEWAKE_MEDIA_VEBOX_GEN11(0);
	if (d == FORCEWAKE_MEDIA_VEBOX1)
			return FORCEWAKE_MEDIA_VEBOX_GEN11(1);
	if (d == FORCEWAKE_MEDIA_VEBOX2)
			return FORCEWAKE_MEDIA_VEBOX_GEN11(2);
	if (d == FORCEWAKE_MEDIA_VEBOX3)
			return FORCEWAKE_MEDIA_VEBOX_GEN11(3);
	
	if (d == FORCEWAKE_GSC)
		return FORCEWAKE_REQ_GSC;

	assertf(false, "Unknown force wake domain %d", d);
	return 0;
}

// Map forcewake domain bitmask → MMIO acknowledge register
constexpr uint32_t ackForDom(unsigned d) {
	if (d == FORCEWAKE_GT)
		return FORCEWAKE_ACK_GT_GEN9;
	if (d == FORCEWAKE_RENDER)
		return FORCEWAKE_ACK_RENDER_GEN9;
	
	if (d == FORCEWAKE_MEDIA_VDBOX0)
			return FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(0);
	if (d == FORCEWAKE_MEDIA_VDBOX1)
			return FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(1);
	if (d == FORCEWAKE_MEDIA_VDBOX2)
			return FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(2);
	if (d == FORCEWAKE_MEDIA_VDBOX3)
			return FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(3);
	if (d == FORCEWAKE_MEDIA_VDBOX4)
			return FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(4);
	if (d == FORCEWAKE_MEDIA_VDBOX5)
			return FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(5);
	if (d == FORCEWAKE_MEDIA_VDBOX6)
			return FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(6);
	if (d == FORCEWAKE_MEDIA_VDBOX7)
			return FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(7);


	if (d == FORCEWAKE_MEDIA_VEBOX0)
			return FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(0);
	if (d == FORCEWAKE_MEDIA_VEBOX1)
			return FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(1);
	if (d == FORCEWAKE_MEDIA_VEBOX2)
			return FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(2);
	if (d == FORCEWAKE_MEDIA_VEBOX3)
			return FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(3);
	
	if (d == FORCEWAKE_GSC)
		return FORCEWAKE_ACK_GSC;
	
	assertf(false, "Unknown force wake domain %d", d);
	return 0;
}

constexpr const char* const strForDom(unsigned d) {
	if (d == DOM_RENDER)
		return "Render";
	if (d == DOM_MEDIA)
		return "Media";
	if (d == DOM_BLITTER)
		return "Blitter";
	return "(unk)";
}

// Forcewake set/clear helpers using masked register write convention
constexpr uint32_t masked_field(uint32_t mask, uint32_t value) {
	return (mask << 16) | value;
}
constexpr uint32_t fw_set(uint32_t v) {
	return masked_field(v, v);
}
constexpr uint32_t fw_clear(uint32_t v) {
	return masked_field(v, 0);
}


enum ConnectorType : uint32_t {
	ConnectorZero       = 0x0,
	ConnectorDummy      = 0x1,   /* Always used as dummy, seems to sometimes work as VGA */
	ConnectorLVDS       = 0x2,   /* Just like on AMD LVDS is used for eDP */
	ConnectorDigitalDVI = 0x4,   /* This is not eDP despite a common misbelief */
	ConnectorSVID       = 0x8,
	ConnectorVGA        = 0x10,
	ConnectorDP         = 0x400,
	ConnectorHDMI       = 0x800,
	ConnectorAnalogDVI  = 0x2000
};

/* I can see very few mentioned in the code (0x1, 0x8, 0x40), though connectors themselves define way more! */

union ConnectorFlags {
	struct ConnectorFlagBits {
		/* Bits 1, 2, 8 are mentioned in AppleIntelFramebufferController::GetGPUCapability */
		/* Lets apperture memory to be not required AppleIntelFramebuffer::isApertureMemoryRequired */
		uint8_t CNAlterAppertureRequirements :1;  /* 0x1 */
		uint8_t CNUnknownFlag_2              :1;  /* 0x2 */
		uint8_t CNUnknownFlag_4              :1;  /* 0x4 */
		/* Normally set for LVDS displays (i.e. built-in displays) */
		uint8_t CNConnectorAlwaysConnected   :1;  /* 0x8 */
		/* AppleIntelFramebuffer::maxSupportedDepths checks this and returns 2 IODisplayModeInformation::maxDepthIndex ?? */
		uint8_t CNUnknownFlag_10             :1;  /* 0x10 */
		uint8_t CNUnknownFlag_20             :1;  /* 0x20 */
		/* Disable blit translation table? AppleIntelFramebufferController::ConfigureBufferTranslation */
		uint8_t CNDisableBlitTranslationTable:1;  /* 0x40 */
		/* Used in AppleIntelFramebufferController::setPowerWellState */
		/* Activates MISC IO power well (SKL_DISP_PW_MISC_IO) */
		uint8_t CNUseMiscIoPowerWell         :1;  /* 0x80 */
		/* Used in AppleIntelFramebufferController::setPowerWellState */
		/* Activates Power Well 2 usage (SKL_PW_CTL_IDX_PW_2) */
		/* May help with HDMI audio configuration issues */
		/* REF: https://github.com/acidanthera/bugtracker/issues/1189 */
		uint8_t CNUsePowerWell2              :1;  /* 0x100 */
		uint8_t CNUnknownFlag_200            :1;  /* 0x200 */
		uint8_t CNUnknownFlag_400            :1;  /* 0x400 */
		/* Sets fAvailableLaneCount to 30 instead of 20 when specified */
		uint8_t CNIncreaseLaneCount          :1;  /* 0x800 */
		uint8_t CNUnknownFlag_1000           :1;  /* 0x1000 */
		uint8_t CNUnknownFlag_2000           :1;  /* 0x2000 */
		uint8_t CNUnknownFlag_4000           :1;  /* 0x4000 */
		uint8_t CNUnknownFlag_8000           :1;  /* 0x8000 */
		uint16_t CNUnknownZeroFlags;
	} bits;
	uint32_t value;
};

struct PACKED ConnectorInfo {
	/* Watch out, this is really messy (see AppleIntelFramebufferController::MapFBToPort).
	 * I am not fully sure why this exists, and recommend setting index to array index (i.e. the sequential number from 0).
	 *
	 * The only accepted values are 0, 1, 2, 3, and -1 (0xFF). When index is equal to array index the logic is simple:
	 * Port with index    0    is always considered built-in (of LVDS type) regardless of any other values.
	 * Ports with indexes 1~3  are checked against type, HDMI will allow the use of digital audio, otherwise DP is assumed.
	 * Port with index    0xFF is ignored and skipped.
	 *
	 * When index != array index port type will be read from connector[index].type.
	 * Say, we have 2 active ports:
	 * 0 - [1]     busId 4 type LVDS
	 * 1 - [2]     busId 5 type DP
	 * 2 - [3]     busId 6 type HDMI
	 * 3 - [-1]    busId 0 type Dummy
	 * This will result in 2 framebuffers which types will be shifted:
	 * 0 - busId 4 type DP
	 * 1 - busId 5 type HDMI
	 * In fact BusId values are also read as connector[index].busId, but are later mapped back via
	 * AppleIntelFramebufferController::getGMBusIDfromPort by looking up a connector with the specified index.
	 * The lookup will stop as soon as a special marker connector (-1) is found. To illustrate, if we have 2 active ports:
	 * 0 - [1]     busId 4 type LVDS
	 * 1 - [2]     busId 5 type DP
	 * 2 - [-1]    busId 6 type HDMI
	 * 3 - [-1]    busId 0 type Dummy
	 * The result will be 2 framebuffers which types and the second busId will be shifted:
	 * 0 - busId 4 type DP
	 * 1 - busId 6 type HDMI
	 * It is also used for port-number calculation.
	 * - LVDS displays (more precisely, displays with CNConnectorAlwaysConnected flag set) get port-number 0.
	 * - Other displays go through index - port-number mapping: 1 - 5, 2 - 6, 3 - 7, or fallback to 0.
	 */
	int8_t index;
	/* Proven by AppleIntelFramebufferController::MapFBToPort, by a call to AppleIntelFramebufferController::getGMBusIDfromPort.
	 * This is GMBUS (Graphic Management Bus) ID described in https://01.org/sites/default/files/documentation/intel-gfx-prm-osrc-hsw-display_0.pdf.
	 * The use could be found in Intel Linux Graphics Driver source code:
	 * https://github.com/torvalds/linux/blob/6481d5ed076e69db83ca75e751ad492a6fb669a7/drivers/gpu/drm/i915/intel_i2c.c#L43
	 * https://github.com/torvalds/linux/blob/605dc7761d2701f73c17183649de0e3044609817/drivers/gpu/drm/i915/i915_reg.h#L3053
	 * However, it should be noted that Apple identifiers are slightly different from Linux driver.
	 * In Linux 0 means disabled, however, for Apple it has some special meaning and is used for internal display.
	 * Other than that the values are the same:
	 * - GMBUS_PIN_DPC    (4)  HDMIC
	 * - GMBUS_PIN_DPB    (5)  SDVO, HDMIB
	 * - GMBUS_PIN_DPD    (6)  HDMID
	 * - GMBUS_PIN_VGADDC (2)  VGA until Broadwell inclusive.
	 * So basically you could use 4, 5, 6 for arbitrary HDMI or DisplayPort displays.
	 * Since 5 supports SDVO (https://en.wikipedia.org/wiki/Serial_Digital_Video_Out), it may also be used to support DVI displays.
	 * Starting with Skylake VGA works via SDVO too (instead of a dedicated GMBUS_PIN_VGADDC id).
	 */
	uint8_t busId;
	/* Appears to be used for grouping ports just like Piker says, but I cannot find the usage. */
	uint8_t pipe;
	uint8_t pad;
	ConnectorType type;
	/* These are connector flags, they have nothing to do with delays regardless of what Piker says.
	 * I tried to describe some in ConnectorFlags.
	 */
	ConnectorFlags flags;
};

// ICL+ connector info — widened to 32-bit fields vs 8-bit in older gens
struct PACKED ConnectorInfoICL {
	uint32_t index;
	uint32_t busId;
	uint32_t pipe;
	uint32_t pad;
	ConnectorType type;
	ConnectorFlags flags;
};
struct PACKED FramebufferCNLCurrents {
	uint32_t value1;
	uint32_t pad;
	uint64_t valu2;
};
// FramebufferICLLP — ICL Low-Power framebuffer descriptor (6 connectors, used by ICL FB kext)
struct PACKED FramebufferICLLP {
	uint32_t framebufferId;
	/* Unclear what values really are, yet 4 stands for non-LP chipset.
	 * See AppleIntelFramebufferController::start.
	 */
	uint32_t fPchType;
	uint64_t fModelNameAddr;
	/* While it is hard to be sure, because having 0 here results in online=true returned by
	 * AppleIntelFramebuffer::GetOnlineInfo, after all it appears to be the case, and the unused
	 * so-called mobile framebufers are simply set to fail-safe defaults.
	 * For some reason it is often called fDisabled...
	 */
	uint8_t  fMobile;
	uint8_t  fPipeCount;
	uint8_t  fPortCount;
	uint8_t  fFBMemoryCount;
	/* This one is per framebuffer fStolenMemorySize * fFBMemoryCount */
	uint32_t fStolenMemorySize;
	/* This is for boot framebuffer from what I can understand */
	uint32_t fFramebufferMemorySize;
	uint32_t fUnifiedMemorySize;
	ConnectorInfoICL connectors[6];
	/* Flags are quite different in ICL now */
	union { uint32_t value; } flags;
	uint32_t unk2;
	FramebufferCNLCurrents currents[3];
	uint32_t unk3[2];
	uint32_t camelliaVersion;
	uint32_t unk4[3];
	uint32_t fNumTransactionsThreshold;
	/* Defaults to 14, used when UseVideoTurbo bit is set */
	uint32_t fVideoTurboFreq;
	uint32_t fSliceCount;
	uint32_t fEuCount;
	uint32_t unk5;
	uint8_t unk6;
	uint8_t pad[3];
};

// FramebufferICL — TGL framebuffer descriptor (3 connectors, different layout from ICLLP)
struct PACKED FramebufferICL {

	uint32_t framebufferId;

	uint32_t fPchType;
	uint64_t fModelNameAddr;

	uint8_t  fMobile;
	uint8_t  fPipeCount;
	uint8_t  fPortCount;
	uint8_t  fFBMemoryCount;

	uint32_t fStolenMemorySize;
	uint32_t fFramebufferMemorySize;
	uint32_t fUnifiedMemorySize;
	
	ConnectorInfoICL connectors[3];//144 bytes
	uint64_t flags;
	uint64_t empty0;
	
	uint64_t combo1;
	uint64_t empty1;
	uint64_t combo2;
	uint64_t empty2;
	uint64_t combo3;
	uint64_t field1;
	uint32_t camelliaVersion;
	uint32_t fNumTransactionsThreshold;
	uint32_t fVideoTurboFreq;
	uint32_t empty3;
	uint32_t empty4;
	uint32_t empty5;
	uint32_t slice;
	uint32_t eu;
	uint32_t subslice;
	uint32_t empty6;
	
};

// Platform Controller Hub identification — determines south display engine variant
enum intel_pch {
	PCH_NOP = -1,	/* PCH without south display */
	PCH_NONE = 0,	/* No PCH present */
	PCH_IBX,	/* Ibexpeak PCH */
	PCH_CPT,	/* Cougarpoint/Pantherpoint PCH */
	PCH_LPT,	/* Lynxpoint/Wildcatpoint PCH */
	PCH_SPT,        /* Sunrisepoint/Kaby Lake PCH */
	PCH_CNP,        /* Cannon/Comet Lake PCH */
	PCH_ICP,	/* Ice Lake/Jasper Lake PCH */
	PCH_TGP,	/* Tiger Lake/Mule Creek Canyon PCH */
	PCH_ADP,	/* Alder Lake PCH */

	/* Fake PCHs, functionality handled on the same PCI dev */
	PCH_DG1 = 1024,
	PCH_DG2,
	PCH_MTL,
	PCH_LNL,
};



// ═══════════════════════════════════════════════════════════════════════════
// Gen11 — Lilu route/patch class for ICL/TGL framebuffer + accelerator kexts.
// Each "static ... / mach_vm_address_t o..." pair is a Lilu function route:
//   static method = our wrapper, mach_vm_address_t = saved original pointer.
// ═══════════════════════════════════════════════════════════════════════════
class Gen11 {
	friend class Genx;

private:

	// ── GuC (Graphics micro-Controller) firmware ──
	static unsigned long loadGuCBinary(void *that);  // route: intercept GuC FW load
	mach_vm_address_t oloadGuCBinary {};

	static UInt8 wrapLoadGuCBinary(void *that);  // hooks GuC binary loading into accel
	mach_vm_address_t orgLoadGuCBinary {};

	static IOReturn wrapConnectionProbe();  // display hot-plug connection probe
	mach_vm_address_t orgConnectionProbe {};

	// ── Display timing / mode validation ──
	static uint8_t SetupDPSSTTimings(void *that,void *param_1,void *param_2,void *param_3);
	mach_vm_address_t oSetupDPSSTTimings {};

	static uint32_t validateDetailedTiming(void *that,void *param_1,unsigned long param_2);
	mach_vm_address_t ovalidateDetailedTiming{};

	static void SetupTimings(void *that, void *param_1, void *param_2, void *param_3, void *param_4);
	mach_vm_address_t oSetupTimings{};

	static uint8_t validateDisplayMode(void *that, int param_1, void *param_2, void *param_3);
	mach_vm_address_t ovalidateDisplayMode{};

	static uint32_t getCurrentDisplayMode(void *that, int *displayMode, int *depth);
	mach_vm_address_t ogetCurrentDisplayMode{};

	static void setupDisplayTiming(void *that, void *param_1, void *param_2);
	mach_vm_address_t osetupDisplayTiming{};

	static unsigned long getPixelInformation(void *that, unsigned int param_1, int param_2, int param_3, void *param_4);
	mach_vm_address_t ogetPixelInformation{};

	static uint8_t maxSupportedDepths(void *param_1);
	mach_vm_address_t omaxSupportedDepths{};
	
	// ── Accelerator firmware & scheduler ──
	static bool wrapLoadFirmware(void *that);       // GuC/HuC firmware load wrapper
	static bool wrapInitSchedControl(void *that);    // scheduler control init
	static void *wrapIgBufferWithOptions(void *accelTask, void* size, unsigned int type, unsigned int flags);
	static UInt64 wrapIgBufferGetGpuVirtualAddress(void *that);
	
	static int wrapPmNotifyWrapper(unsigned int a0, unsigned int a1, unsigned long long *a2, unsigned int *freq);  // GPU freq change notification
	mach_vm_address_t orgPmNotifyWrapper {};
	
	// ── Accelerator start & forcewake ──
	static unsigned long start(void *that,void  *param_1);   // IntelAccelerator::start wrapper
	static void v54IrqWatchdog(thread_call_param_t, thread_call_param_t);  // V54: IRQ watchdog
	static void v60GpuHealthMonitor(thread_call_param_t, thread_call_param_t);  // V60: active ERROR_GEN6 suppression + monitor
	static void v71EmrEnforcer(thread_call_param_t, thread_call_param_t);  // V71: high-freq EMR mask + ERROR clear (50ms)
	static IOMemoryMap *v85PersistMap;   // V85: persistent FB page 0 mapping for 50ms fill
	static uint32_t v85SurfAddr;         // V85: cached PLANE_SURF address
	static IOBufferMemoryDescriptor *v116DummyBuf;  // V116: safe dummy page for GGTT[0] remap
	static uint64_t v116DummyPhys;                  // V116: physical address of dummy page
	mach_vm_address_t ostart {};

	static uint8_t deviceStart(void *that);   // V111: force IGAccelDevice::deviceStart true on RPL
	mach_vm_address_t odeviceStart {};

	static void *createUserGPUTask(void *that);  // V132: fallback when per-user task creation returns null on spoofed RPL
	mach_vm_address_t ocreateUserGPUTask {};

	static void *igAccelTaskWithOptions(void *that);  // V132: cache successful task allocations for null-task fallback
	mach_vm_address_t oigAccelTaskWithOptions {};
	
	static unsigned long submitBlit(void *that, void *param_1, void *param_2, void *param_3, bool param_4);
	mach_vm_address_t osubmitBlit {};
	
	static bool patchRCSCheck(mach_vm_address_t& start);  // bypass RCS engine check
	static void forceWake(void *that, bool set, uint32_t dom, uint8_t ctx);  // custom forcewake
	mach_vm_address_t oforceWake {};
	static void wrapSafeForceWake(void *that, bool set, uint32_t dom);       // SafeForceWake wrapper
	mach_vm_address_t oSafeForceWake {};
	static bool pollRegister(uint32_t reg, uint32_t val, uint32_t mask, uint32_t timeout);
	static bool forceWakeWaitAckFallback(uint32_t reqReg, uint32_t ackReg, uint32_t val, uint32_t mask);
	
	static void * serviceInterrupts(void *param_1);  // GT interrupt handler
	mach_vm_address_t oserviceInterrupts {};
	
	static int isConflictRegister();   // stub: returns 0 (no conflict)
	
	static void releaseDoorbell();     // stub: GuC doorbell release
	
	void *framecont;  // cached framebuffer controller pointer
	void *accelInstance {nullptr};  // V42: saved IntelAccelerator instance for child enumeration
	
	// Saved original function pointers for accelerator
	mach_vm_address_t orgSubmitExecList {};    // ExecList submission (command dispatch)
	mach_vm_address_t orgLoadFirmware {};      // FW load original
	mach_vm_address_t orgInitSchedControl {};  // scheduler init original
	mach_vm_address_t orgIgBufferWithOptions {};
	mach_vm_address_t orgIgBufferGetGpuVirtualAddress {};
	
	mach_vm_address_t _gSysctlVariables {};
	
	// ── GuC firmware patching state ──
	uint32_t freq_max {0};                      // max GPU frequency (from RP_STATE_CAP)
	uint8_t *gKmGen9GuCBinary {nullptr};        // pointer to GuC binary in kext __DATA
	uint8_t *signaturePointer {nullptr};        // CSS header signature location
	uint32_t *firmwareSizePointer {nullptr};    // pointer to FW size field
	uint8_t *dummyFirmwareBuffer {nullptr};     // dummy FW for initial load
	uint8_t *realFirmwareBuffer {nullptr};      // real patched FW buffer
	uint32_t realBinarySize {};                 // actual binary size
	
	static uint8_t validateModeDepth(void *that,void *param_1,uint param_2);
	mach_vm_address_t ovalidateModeDepth {};
	
	
	bool performingFirmwareLoad {false};  // guards re-entrant FW load
	
	static int handleLinkIntegrityCheck();  // stub: returns 0 (link OK)
	
	// ── Power management ──
	static void wrapSystemWillSleep(void *that);
	static void wrapSystemDidWake(void *that);
	

	static void setPanelPowerState(void *that,bool param_1);
	mach_vm_address_t osetPanelPowerState {};
	
	static void  getGPUInfo(void *that);     // patches topology for TGL HW (TGL offsets); void per Ghidra
	mach_vm_address_t ogetGPUInfo {};

	static void  getGPUInfoICL(void *that);  // patches topology for ICL HW (ICL offsets); void per Ghidra
	mach_vm_address_t ogetGPUInfoICL {};
	
	static unsigned long fastLinkTraining();
	mach_vm_address_t ofastLinkTraining {};
	
	// FB client attribute handler — IOAccel/display property dispatch
	static IOReturn wrapFBClientDoAttribute(void *fbclient, uint32_t attribute, unsigned long *unk1, unsigned long unk2, unsigned long *unk3, unsigned long *unk4, void *externalMethodArguments);
	mach_vm_address_t orgFBClientDoAttribute {};
	
	// PAVP = Protected Audio Video Path (DRM session management)
	static IOReturn wrapPavpSessionCallback(void *intelAccelerator, int32_t sessionCommand, uint32_t sessionAppId, uint32_t *a4, bool flag);
	mach_vm_address_t orgPavpSessionCallback {};
	
	// CSR = DMC firmware patch data pointers (stepping-specific)
	const uint8_t *_CSR_PATCH_B0plus;  // B0+ stepping
	const uint8_t *_CSR_PATCH_AX;      // AX stepping
	
	// ── MMIO register access wrappers (can intercept/log/redirect) ──
	static void wrapWriteRegister32(void *controller, uint32_t address, uint32_t value);
	mach_vm_address_t owrapWriteRegister32 {};
	
	static uint32_t wrapReadRegister32(void *controller, uint32_t address);
	mach_vm_address_t owrapReadRegister32 {};
	
	// Sleep/wake transition hooks
	static void prepareToExitWake(AppleIntel::AppleIntelFramebuffer *that);
	mach_vm_address_t oprepareToExitWake {};
	static void prepareToExitSleep(AppleIntel::AppleIntelFramebuffer *that);
	mach_vm_address_t oprepareToExitSleep {};
	static void prepareToEnterSleep(AppleIntel::AppleIntelFramebuffer *that);
	mach_vm_address_t oprepareToEnterSleep {};
	
	static void hwInitializeCState(AppleIntel::AppleIntelBaseController *that);
	mach_vm_address_t ohwInitializeCState {};

	bool isICLFB {false};   // true when loaded under AppleIntelICLLPGraphicsFramebuffer
	bool tglFBLoaded {false};  // true when TGL FB processed — skip ICL FB if set
	bool tglHWLoaded {false};  // true when TGL HW processed — skip ICL HW if set

	// V131: Cached fallback contexts for spoofed RPL path to prevent NULL task submission
	void *v131CachedBlit3DCtx {nullptr};  // Fallback 3D context when creation fails
	void *v131CachedBlit2DCtx {nullptr};  // Fallback 2D context when creation fails
	void *v132CachedTask {nullptr};       // Fallback IGAccelTask when per-user task creation fails

	static void hwConfigureCustomAUX(AppleIntel::AppleIntelBaseController *that, bool param_1);
	mach_vm_address_t ohwConfigureCustomAUX {};
	
	static void FastWriteRegister32(AppleIntel::AppleIntelBaseController *that, unsigned long param_1, uint32_t param_2);
	mach_vm_address_t oFastWriteRegister32 {};
	
	mach_vm_address_t gPlatformInformationList {};

	static void     initPlatformWorkarounds(AppleIntel::AppleIntelBaseController *that);
	mach_vm_address_t oinitPlatformWorkarounds {};

	static uint64_t getOSInformation(AppleIntel::AppleIntelBaseController *that);
	mach_vm_address_t ogetOSInformation {};
	
	static uint8_t setDisplayMode(AppleIntel::AppleIntelFramebuffer *that, int param_1, int param_2);
	mach_vm_address_t osetDisplayMode {};
	
	static uint8_t hwRegsNeedUpdate
			  (AppleIntel::AppleIntelBaseController *that,
			   AppleIntel::AppleIntelFramebuffer *param_1,
			   AppleIntel::AppleIntelDisplayPath *param_2,
			   AppleIntel::CRTCParams *param_3,
			   const IODetailedTimingInformationV2 *param_4,
			   AppleIntel::SCALERPARAMS *param_5);
	mach_vm_address_t ohwRegsNeedUpdate {};

	// Force eDP lane count to match the HW-trained count from DDI_BUF_CTL_A.
	static void computeLaneCount(AppleIntel::AppleIntelBaseController *that, const IODetailedTimingInformationV2 *timing, unsigned int linkRate, unsigned int bpp, unsigned int *laneCount);
	mach_vm_address_t ocomputeLaneCount {};

	// setupOptimalLaneCount caps computeLaneCount's result to DPCD MAX_LANE_COUNT.
	// On RPL the panel reports MAX_LANE_COUNT=2 but UEFI trained 4 lanes, so we
	// override the cached optimal to match DDI_BUF_CTL_A instead.
	static void setupOptimalLaneCount(AppleIntel::AppleIntelBaseController *that, const IODetailedTimingInformationV2 *timing, unsigned int bpp);
	mach_vm_address_t osetupOptimalLaneCount {};

	// V96: Force display online — WEG's force-online (FOD) hooks getDisplayStatus which
	// does NOT exist in the TGL framebuffer kext, so it fails with "err 2" at boot.
	// The TGL FB uses getOnlineInfo instead; hook it here to unconditionally report online.
	static void getOnlineInfo(AppleIntel::AppleIntelFramebuffer *that, AppleIntel::AppleIntelDisplayPath *displayPath, unsigned char *online, unsigned char *changed);
	mach_vm_address_t ogetOnlineInfo {};

	// Path B: Force AppleIntelFramebuffer::isApertureMemoryRequired() to return true under
	// dp0 mode so setupScanoutMemory never migrates from aperture to non-aperture memory
	// when WindowServer transitions fWSAAState 0→3.  Returns the original value otherwise.
	static bool wrapIsApertureMemoryRequired(AppleIntel::AppleIntelFramebuffer *that);
	mach_vm_address_t oIsApertureMemoryRequired {};

	// Path C: Hook AppleIntelFramebuffer::setAttribute(IOSelect, uintptr_t).
	// On dp0 + !isRealTGL: when WindowServer writes kIOWindowServerActiveAttribute
	// ('wsrv' = 0x77737276) with value 0x1 (degrade), coerce to 0x3 (stay-active) before
	// calling original — preserves kernel-tracked fWSAAState so the driver keeps treating
	// WS as fully active.  All other attribute writes pass through unchanged.
	static IOReturn wrapSetAttribute(void *that, uint32_t attr, uintptr_t value);
	mach_vm_address_t oSetAttribute {};
	
	static void blit3d_submit_rectlist(void *param_1,void *param_2,void *param_3);
	mach_vm_address_t oblit3d_submit_rectlist {};
	
	static IOReturn wrapICLReadAUX(void *that, uint32_t address, void *buffer, uint32_t length);
	mach_vm_address_t orgICLReadAUX {};
	
	static int blit3d_supported();
	
	static int getPlatformID();
	mach_vm_address_t ogetPlatformID {};
	
	static void logStateInRegistry(void *that,uint param_1);
	mach_vm_address_t ologStateInRegistry {};
	
	// AppleIntelFramebuffer::init(AppleIntelBaseController*, uint pipeIndex)
	static uint32_t AppleIntelFramebufferinit(AppleIntel::AppleIntelFramebuffer *frame,
	                                          AppleIntel::AppleIntelBaseController *cont,
	                                          uint32_t pipeIndex);
	mach_vm_address_t oAppleIntelFramebufferinit {};

	// AppleIntelPlane::init(IGPlaneID pipeIndex)
	static uint64_t AppleIntelPlaneinit(AppleIntel::AppleIntelPlane *that, uint32_t pipeIndex);
	mach_vm_address_t oAppleIntelPlaneinit {};

	// AppleIntelScaler::init(IGScalerID pipeIndex)
	static uint64_t AppleIntelScalerinit(AppleIntel::AppleIntelScaler *that, uint32_t pipeIndex);
	mach_vm_address_t oAppleIntelScalerinit {};

	// AppleIntelScaler::disableScaler(bool)
	static void  disableScaler(AppleIntel::AppleIntelScaler *that, bool disable);
	mach_vm_address_t odisableScaler {};

	// AppleIntelPlane::enablePlane(bool)
	static void  enablePlane(AppleIntel::AppleIntelPlane *that, bool enable);
	mach_vm_address_t oenablePlane {};

	// AppleIntelScaler::programPipeScaler(AppleIntelDisplayPath*)
	static void programPipeScaler(AppleIntel::AppleIntelScaler *that, AppleIntel::AppleIntelDisplayPath *displayPath);
	mach_vm_address_t oprogramPipeScaler {};

	// V400: AppleIntelScaler::setupPipeScaler(AppleIntelDisplayPath *, CRTCParams *)
	// Read-only logging hook — dumps pipe scaler fields after Apple computes them
	// (PIPE_SRCSZ vs PS_PS_WIN_SZ tells us whether downscaling is happening).
	// AppleIntelScaler::setupPipeScaler(AppleIntelDisplayPath*, CRTCParams*)
	static void setupPipeScaler(AppleIntel::AppleIntelScaler *that, AppleIntel::AppleIntelDisplayPath *path,
	                            AppleIntel::CRTCParams *params);
	mach_vm_address_t osetupPipeScaler {};

	// V401: AppleIntelBaseController::paramsSurfCompare(CRTCParams *, CRTCParams *, PLANEPARAMS *, PLANEPARAMS *)
	// Read-only diagnostic. Fires on every flip — Apple uses this to decide whether
	// plane registers need reprogramming. Logs PLANE_CTL tiling bits, STRIDE, SURF.
	// AppleIntelBaseController::paramsSurfCompare(CRTCParams*, CRTCParams*, PLANEPARAMS*, PLANEPARAMS*)
	static bool paramsSurfCompare(AppleIntel::AppleIntelBaseController *that,
	                              AppleIntel::CRTCParams *p1, AppleIntel::CRTCParams *p2,
	                              AppleIntel::PLANEPARAMS *pl1, AppleIntel::PLANEPARAMS *pl2);
	mach_vm_address_t oparamsSurfCompare {};

	// V402: AppleIntelBaseController::setupDSCEngineParams(AppleIntelFramebuffer *, CRTCParams *, AppleIntelDisplayPath *, IODetailedTimingInformationV2 *)
	// Read-only diagnostic. Linux confirms DSC=off on our panel; this hook lets us
	// see if Apple still configures DSC despite Info.plist DSCSupport=0.
	// AppleIntelBaseController::setupDSCEngineParams(AppleIntelFramebuffer*, CRTCParams*, AppleIntelDisplayPath*, IODetailedTimingInformationV2*)
	static void setupDSCEngineParams(AppleIntel::AppleIntelBaseController *that,
	                                 AppleIntel::AppleIntelFramebuffer *fb,
	                                 AppleIntel::CRTCParams *params,
	                                 AppleIntel::AppleIntelDisplayPath *path,
	                                 IODetailedTimingInformationV2 *timing);
	mach_vm_address_t osetupDSCEngineParams {};

	// V403: AppleIntelBaseController::SetupParams(AppleIntelFramebuffer *, AppleIntelDisplayPath *, CRTCParams *, IODetailedTimingInformationV2 const *)
	// The MASTER CRTCParams builder — runs once per modeset before any consumer.
	// After this returns CRTCParams is fully populated. Read-only logger: snapshot
	// every key field so we can see the source-of-truth values BEFORE setupPipeScaler /
	// setupDSCEngineParams / hwRegsNeedUpdate / hwSetMode consume them.
	// AppleIntelBaseController::SetupParams(AppleIntelFramebuffer*, AppleIntelDisplayPath*, CRTCParams*, const IODetailedTimingInformationV2*)
	static void setupParams(AppleIntel::AppleIntelBaseController *that,
	                        AppleIntel::AppleIntelFramebuffer *fb,
	                        AppleIntel::AppleIntelDisplayPath *path,
	                        AppleIntel::CRTCParams *params,
	                        const IODetailedTimingInformationV2 *timing);
	mach_vm_address_t osetupParams {};

	// V404: AppleIntelBaseController::setupPipeWatermarks(AppleIntelFramebuffer *, AppleIntelDisplayPath *, CRTCParams *)
	// Called from inside SetupParams BEFORE setupPipeScaler. Per-pipe DBUF/watermark
	// allocator. Prime suspect for setting PIPE_SEAM_EXCESS=0x1 since seam-joining
	// affects DBUF distribution. Read-only logger: pre/post PIPE_SEAM_EXCESS.
	// AppleIntelBaseController::setupPipeWatermarks(AppleIntelFramebuffer*, AppleIntelDisplayPath*, CRTCParams*)
	static void setupPipeWatermarks(AppleIntel::AppleIntelBaseController *that,
	                                AppleIntel::AppleIntelFramebuffer *fb,
	                                AppleIntel::AppleIntelDisplayPath *path,
	                                AppleIntel::CRTCParams *params);
	mach_vm_address_t osetupPipeWatermarks {};

	// V405: AppleIntelPlane::configureColorPipeLine(FlipTransactionArgs*, bool)
	// Dispatches 8/10/12/12SEG gamma pipeline from a BPC selector in FlipTransactionArgs.
	// Logs GAMMA_MODE (0x4A480) and PIPE_MISC (0x70030) pre/post to confirm correct
	// Display 13 values on ADL-P.  Gated on !isRealTGL.
	static void configureColorPipeLine(AppleIntel::AppleIntelPlane *that, AppleIntel::FlipTransactionArgs *flipArgs, bool param_2);
	mach_vm_address_t oConfigureColorPipeLine {};

	// V406: AppleIntelPlane::configurePlane(FlipTransactionArgs*)
	// Origin-level linear tiling fix: patch FlipTransactionArgs+0x3c tiling enum to 2
	// (neither X-tiled=0 nor Y-tiled=1) so Apple computes PLANE_CTL bits[12:10]=000
	// (linear) and PLANE_STRIDE = pitch_bytes/512 = 0x14. Physical pages are CPU-written
	// linearly; forcing this here ensures the display engine fetches scanlines correctly.
	static void configurePlane(AppleIntel::AppleIntelPlane *that, AppleIntel::FlipTransactionArgs *flipArgs);
	mach_vm_address_t oConfigurePlane {};

	static void  disablePowerWellPG(AppleIntel::AppleIntelBaseController *that, uint param_1);
	mach_vm_address_t odisablePowerWellPG {};

	static void  enablePowerWellPG(AppleIntel::AppleIntelBaseController *that, uint param_1);
	mach_vm_address_t oenablePowerWellPG {};
	
	static void hwSetPowerWellStatePG(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2);
	mach_vm_address_t ohwSetPowerWellStatePG {};

	// V182: hwSetPowerWellStatePGE — enables PW_1/PW_2 (display power gates).
	// Previously no-op'd via releaseDoorbell; now callthrough with 0x78=ccont
	// fixup (same pattern as DDI/Aux). Linux confirms PW_1+PW_2 must be up
	// before eDP AUX/PHY-A can be initialized. cold.1-.12 remain no-op to
	// silence Apple's assert-on-timeout cold paths.
	static void hwSetPowerWellStatePGE(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2);
	mach_vm_address_t ohwSetPowerWellStatePGE {};
	
	static void hwSetPowerWellStateDDI(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2);
	mach_vm_address_t ohwSetPowerWellStateDDI {};
	
	static void hwSetPowerWellStateAux(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2);
	mach_vm_address_t ohwSetPowerWellStateAux {};
	
	
	
	static void AppleIntelPowerWellinit(AppleIntel::AppleIntelPowerWell *that, AppleIntel::AppleIntelBaseController *param_1);
	mach_vm_address_t oAppleIntelPowerWellinit {};
	
	static int hasExternalDispla();
	
	static void enableDisplayEngine(AppleIntel::AppleIntelBaseController *that);
	mach_vm_address_t oenableDisplayEngine {};
	
	static void disableDisplayEngine(AppleIntel::AppleIntelBaseController *that);
	mach_vm_address_t odisableDisplayEngine {};
	

	static uint8_t enableController(AppleIntel::AppleIntelFramebuffer *that);
	mach_vm_address_t oenableController {};
	
	static void initializeLogging(AppleIntel::AppleIntelBaseController *that);
	mach_vm_address_t oinitializeLogging {};
	
	// ── CDCLK management ──
	static void sanitizeCDClockFrequency(AppleIntel::AppleIntelBaseController *that);
	static uint32_t wrapProbeCDClockFrequency(AppleIntel::AppleIntelBaseController *that);
	
	static void  readAndClearInterrupts(AppleIntel::AppleIntelBaseController *that, void *param_1);
	mach_vm_address_t oreadAndClearInterrupts {};
	
	static void initCDClock(AppleIntel::AppleIntelBaseController *that);
	mach_vm_address_t oinitCDClock {};
	
	static void setCDClockFrequencyOnHotplug(AppleIntel::AppleIntelBaseController *that);
	mach_vm_address_t osetCDClockFrequencyOnHotplug {};
	
	static void disableCDClock(AppleIntel::AppleIntelBaseController *that);
	mach_vm_address_t odisableCDClock {};
	
	uint32_t (*orgProbeCDClockFrequency)(void *) {nullptr};
	void (*orgDisableCDClock)(void *) {nullptr};
	void (*orgSetCDClockFrequency)(void *, unsigned long long) {nullptr};
	
	static void * wprobe(void *that,void *param_1,int *param_2);  // IOService::probe wrapper
	mach_vm_address_t owprobe {};
	
	static bool  tgstart(void *that,void *param_1);  // TGL-specific start
	mach_vm_address_t otgstart {};
	
	static int hwSetMode
			  (AppleIntel::AppleIntelBaseController *that,
			   AppleIntel::AppleIntelFramebuffer *param_1,
			   AppleIntel::AppleIntelDisplayPath *param_2,
			   AppleIntel::CRTCParams *param_3);
	mach_vm_address_t ohwSetMode {};
	
	static void enablePipe
			  (AppleIntel::AppleIntelBaseController *that,
			   AppleIntel::AppleIntelFramebuffer *param_1,
			   AppleIntel::AppleIntelDisplayPath *param_2,
			   AppleIntel::CRTCParams *param_3);
	mach_vm_address_t oenablePipe {};
	
	static uint8_t beginReset(void *that);
    mach_vm_address_t obeginReset {};
												
	static void endReset(void *that);
	mach_vm_address_t oendReset {};
	
	mach_vm_address_t IntelFBClientControl11doAttribut {};

	static uint32_t probePortMode();  // detect DDI port signaling mode (DP/HDMI/eDP)
	mach_vm_address_t oprobePortMode {};
	
	static uint32_t wdepthFromAttribute(void *that,uint param_1);
	
	// ── Register access (ra = register access) ──
	static uint32_t raReadRegister32(void *that,unsigned long param_1);
	mach_vm_address_t oraReadRegister32 {};
	
	static unsigned long raReadRegister32b(void *that,void *param_1,unsigned long param_2);
	mach_vm_address_t oraReadRegister32b {};
	
	static uint64_t raReadRegister64(void *that,unsigned long param_1);
	mach_vm_address_t oraReadRegister64 {};
	
	static uint64_t raReadRegister64b(void *that,void *param_1,unsigned long param_2);
	
	static void radWriteRegister32(void *that,unsigned long param_1, UInt32 param_2);
	mach_vm_address_t oradWriteRegister32 {};
	static void radWriteRegister32f(void *that,unsigned long param_1, UInt32 param_2);
	mach_vm_address_t oradWriteRegister32f {};
	
	static void raWriteRegister32(void *that,unsigned long param_1, UInt32 param_2);
	mach_vm_address_t oraWriteRegister32 {};
	
	static void raWriteRegister32b(void *that,void *param_1,unsigned long param_2, UInt32 param_3);
	
	static void raWriteRegister32f(void *that,unsigned long param_1, UInt32 param_2);
	mach_vm_address_t oraWriteRegister32f {};
	
	static void raWriteRegister64(void *that,unsigned long param_1,UInt64 param_2);
	mach_vm_address_t oraWriteRegister64 {};
	
	static void raWriteRegister64b(void *that,void *param_1,unsigned long param_2,UInt64 param_3);
	

	// ── Display buffer & memory management ──
	static void setupPlanarSurfaceDBUF();  // DBUF allocation for planar (NV12/P010) surfaces
	mach_vm_address_t osetupPlanarSurfaceDBUF {};
	
	static void updateDBUF(void *that,uint param_1,uint param_2,bool param_3);
	
	static void FBMemMgr_Init(void *that);
	mach_vm_address_t oFBMemMgr_Init {};
	
	static int blit3d_supported(void *param_1,void *param_2);
	mach_vm_address_t oblit3d_supported {};
	
	static int LightUpEDP(void *that,void *param_1, void *param_2,void *param_3);  // eDP panel power-on
	mach_vm_address_t oLightUpEDP {};
	
	// Saved vtable/class pointers for display subsystem objects
	mach_vm_address_t PowerWell {};       // AppleIntelPowerWell class
	mach_vm_address_t PortHAL {};         // port hardware abstraction layer
	mach_vm_address_t PortHALDiags {};    // port diagnostics
	mach_vm_address_t AppleIntelPort {};  // port object

	// ── 3D Blit engine (GPU-accelerated blitting via 3D pipeline) ──
	static void IGHardwareBlit3DContextinitialize(void *that);
	mach_vm_address_t oIGHardwareBlit3DContextinitialize {};
	static void IGAccelResourcefree(void *that);
	mach_vm_address_t oIGAccelResourcefree {};
	static void wrapSetGTFrequencyMMIO(void *that, uint32_t val);
	mach_vm_address_t oSetGTFrequencyMMIO {};
	
	static bool  initHardwareCaps(void *this_ptr);  // query HW capabilities
	mach_vm_address_t oinitHardwareCaps {};
	
	static void * IGMappedBuffergetMemory(void *that);
	mach_vm_address_t oIGMappedBuffergetMemory {};
	
	static void *  IGHardwareBlit3DContextoperatornew(unsigned long size);
	mach_vm_address_t oIGHardwareBlit3DContextoperatornew {};
	
	
	static uint64_t blit3d_init_ctx(void *that);
	mach_vm_address_t oblit3d_init_ctx {};
	
	static void blit3d_initialize_scratch_space(void *that);
	mach_vm_address_t oblit3d_initialize_scratch_space {};

	// V508: Base class IGHardwareContext::withOptions hook — logs ctx+0xb8 and dumps LRCA page1
	// immediately after initWithOptions runs (wbinvd flushes LLC→DRAM for accurate aperture read).
	static void *IGHardwareContextwithOptions(void *task, const void *params, uint8_t arg);
	mach_vm_address_t oIGHardwareContextwithOptions {};

	// V509: Base class IGHardwareContext::initWithOptions hook — CPU-side LRCA page1 repair.
	// restoreFromSafeImage returns true on RPL (skips g_cInitGfxRingContextRCS memcpy), leaving
	// DW1 (MI_LRI header) as 0x00ffffff.  We write the minimal Gen12 ring context LRI block so
	// hardware can restore ring state on ExecList context-restore.
	static uint64_t IGHardwareContextinitWithOptions(void *that, void *task, const void *params, uint8_t arg);
	mach_vm_address_t oIGHardwareContextinitWithOptions {};

	// Extended GPU context init — sets up additional context state (PPGTT, aux tables)
	static uint64_t IGHardwareExtendedContextinitWithOptions
			  (void *that,void *param_1,
			   void *param_2);
	mach_vm_address_t oIGHardwareExtendedContextinitWithOptions {};
	
	// Pointers to extended context parameter tables (per-context-type)
	mach_vm_address_t ExtendedCtxParams {};
	mach_vm_address_t Blit2DExtendedCtxParams {};
	mach_vm_address_t Blit3DExtendedCtxParams {};
	
	static uint8_t isPanelPowerOn();
	
	static uint8_t setupAdditionalDataStructs();
	mach_vm_address_t osetupAdditionalDataStructs {};
	
	// Mangled C++ symbol addresses for Plane/Scaler constructors and metaclasses
	mach_vm_address_t ZN15AppleIntelPlaneC1Ev {};          // AppleIntelPlane::AppleIntelPlane()
	mach_vm_address_t ZN16AppleIntelScalerC1Ev {};         // AppleIntelScaler::AppleIntelScaler()
	mach_vm_address_t ZN16AppleIntelScaler10gMetaClassE {}; // AppleIntelScaler::gMetaClass
	mach_vm_address_t ZN15AppleIntelPlane10gMetaClassE {};  // AppleIntelPlane::gMetaClass
	
	static unsigned long  allocateDisplayResources(void *that);
	mach_vm_address_t oallocateDisplayResources {};
	
	
	static void * getBlit2DContext(void *that,bool param_1);
	mach_vm_address_t ogetBlit2DContext {};

	static void * getDepthResolveContext(void *that,bool param_1);
	mach_vm_address_t ogetDepthResolveContext {};

	static void * getColorResolveContext(void *that,bool param_1);
	mach_vm_address_t ogetColorResolveContext {};
	
	static void * ExtendedContextWithOptions(void *param_1);
	mach_vm_address_t oExtendedContextWithOptions {};
	
	static void * getBlit3DContext(void *that,bool param_1);
	mach_vm_address_t ogetBlit3DContext {};
	
	static void  AppleIntelPlanec1(AppleIntel::AppleIntelPlane *that);
	static void  AppleIntelScalerc1(AppleIntel::AppleIntelScaler *that);
	
	static void * AppleIntelScalernew(unsigned long param_1);
	mach_vm_address_t oAppleIntelScalernew {};
	
	// FB controller start — wraps original, adds registerService() for accelerator matching
	static bool AppleIntelBaseControllerstart(AppleIntel::AppleIntelBaseController *that, IOService *param_1);
	mach_vm_address_t oAppleIntelBaseControllerstart {};

	// V201 diagnostic: read first bytes of the scanout buffer right after hwSetupMemory
	// returns. Tells us whether something filled the buffer (wallpaper) or it's still
	// zeros from the IOBufferMemoryDescriptor allocation.
	static int wrapHwSetupMemory(AppleIntel::AppleIntelBaseController *that, AppleIntel::AppleIntelFramebuffer *fb, AppleIntel::AppleIntelDisplayPath *displayPath, AppleIntel::CRTCParams *params, bool isAperture);
	mach_vm_address_t ohwSetupMemory {};

	// V208: AppleIntelFramebuffer::start() override that refuses fbId != 0. Hides
	// phantom FB1/FB2 (path-not-assigned) from IOFramebufferShared so CoreDisplay's
	// CreateDisplayForCGXDisplayDevice doesn't enumerate them and assert. Apple's
	// internal pipe/port/FB counts stay 3/3/3 (kept in pinfo) — only the IOService
	// registration is blocked. fbId is at +0x1DC (we already use this offset in V96).
	static bool wrapAppleIntelFramebufferStart(void *that, void *provider);
	mach_vm_address_t oAppleIntelFramebufferStart {};

	// V209: AppleIntelFramebuffer::enableController() short-circuit for fbId != 0.
	// Pairs with V208 — without this, fb1/fb2 still get tracked by IGAccelDisplayPipe
	// (in HW kext) and a later displayModeDidChange iteration crashes in
	// IOFramebuffer::getAttributeExt + 0x25 NULL deref. Returning kIOReturnUnsupported
	// for non-FB0 marks them as "not enabled" so IGAccelDisplayPipe skips them.
	static int wrapEnableController(void *that);
	mach_vm_address_t oEnableController {};

	// V210: AppleIntelDisplayPath::getFreeJoinablePathCount() unconditionally returns 0.
	// Original calls getPathByPipe(1) and getPathByPipe(2) and dereferences [+0x3644]
	// on the result — but with single-pipe (1/1/1) pinfo config those return NULL →
	// crash during setupBootDisplay → allocateBootDisplayResources →
	// commitResourceConfigurationSet → allocateDisplayResources. With 1/1/1 we want
	// "no joinable paths available" = return 0. Enables 1/1/1 to actually boot.
	static int wrapGetFreeJoinablePathCount(void *that);
	mach_vm_address_t oGetFreeJoinablePathCount {};

	// IntelAccelerator personality registration in IOCatalogue. Lives in the HW-kext
	// path (ICL or TGL Graphics, not Framebuffer) — must run before the FBController's
	// registerService() so IOKit can match IntelAccelerator. Idempotent.
	void injectAcceleratorPersonality(bool useTglNames);
	bool acceleratorPersonalityInjected {false};
	
	
	static void * AppleIntelPlanenew(unsigned long param_1);
	mach_vm_address_t oAppleIntelPlanenew {};
	
	static void uupdateDBUF(void *that,uint param_1,uint param_2,bool param_3);
		
	static long getPortByDDI(uint param_1);
	mach_vm_address_t ogetPortByDDI {};

	static void AppleIntelScalerupdateRegisterCache(AppleIntel::AppleIntelScaler *that);
	mach_vm_address_t oAppleIntelScalerupdateRegisterCache {};

	static void AppleIntelPlaneupdateRegisterCache(AppleIntel::AppleIntelPlane *that);
	mach_vm_address_t oAppleIntelPlaneupdateRegisterCache {};

	static void PowerWellinit(void *that,void *param_1);
	mach_vm_address_t oPowerWellinit {};
	
	static uint8_t  setPortMode(void *that,uint32_t param_1);
	mach_vm_address_t osetPortMode {};

	// ── Additional accelerator hooks ──
	static void  setAsyncSliceCount(void *that,uint32_t configRaw);  // GPU slice power gating
	mach_vm_address_t osetAsyncSliceCount {};

		static uint32_t
	IntelFBClientControldoAttribute
			  (void *that,uint param_1,unsigned long *param_2,unsigned long param_3,unsigned long *param_4,
			   unsigned long *param_5,void *param_6);
	mach_vm_address_t oIntelFBClientControldoAttribute {};

	static unsigned long resetGraphicsEngine(void *that,void *param_1);  // GT engine reset
	mach_vm_address_t oresetGraphicsEngine {};

	static void applyPreStartEngineWorkarounds(int callCount); // only error/EMR clear — safe before ring init
	static void applyPreStopEngineWorkarounds(int callCount);  // full GT WAs + BCS drain — before stop only
	static unsigned long stopGraphicsEngine(void *that);    // V505: dump ring preamble before ring is stopped
	mach_vm_address_t ostopGraphicsEngine {};

	static unsigned long startGraphicsEngine(void *that);  // V163: clear PERCTX_PREEMPT_CTRL before first context snapshot
	mach_vm_address_t ostartGraphicsEngine {};

	static void xeLrcaRepair(const char *tag);
	static void populateResetRegisterList(void *that);  // V164: clear bit 14 before snapshot into replay list
	mach_vm_address_t opopulateResetRegisterList {};

	static void  IGScheduler5resume(void *that);  // GPU command scheduler resume
	mach_vm_address_t oIGScheduler5resume {};

	// V212: Hook isGpuIdle (watchdog query) — GPU watchdog calls this post-startup.
	// If INSTDONE bit0 stuck (0xfffffffe), it returns false → watchdog declares hang →
	// GPU reset → startGraphicsEngine retry loop. Fix: return true for RPL-P stuck pattern.
	static bool wrapIGScheduler5IsGpuIdle(const void *that);
	mach_vm_address_t oIGScheduler5IsGpuIdle {};
	static bool wrapIGScheduler4IsGpuIdle(const void *that);
	mach_vm_address_t oIGScheduler4IsGpuIdle {};

	static uint8_t connectionChanged(void *that);
	mach_vm_address_t oconnectionChanged {};

	static uint8_t isPanelPowerOn(void *that);
	mach_vm_address_t oisPanelPowerOn {};

	static uint32_t  IGAccelSegmentResourceListprepare(void *that);  // GPU memory segment setup
	mach_vm_address_t oIGAccelSegmentResourceListprepare {};

	static uint32_t beginCoalescedSegment(void *that);  // V124: guard [member+0xb8] null deref
	mach_vm_address_t obeginCoalescedSegment {};

	static uint8_t barrierSubmission(void *queue, void *accelerator, void *cmdDesc,
	                                void *event, uint16_t count, const uint16_t *list);
	mach_vm_address_t obarrierSubmission {};
	
	static void  markBlitUsage(void *that);
	mach_vm_address_t omarkBlitUsage {};
	
	static void  initBlitUsage(void *that);
	mach_vm_address_t oinitBlitUsage {};

	mach_vm_address_t kIGHwCsDesc {};  // pointer to engine descriptor table

	// WOPCM = Write-Once Protected Content Memory — GuC/HuC FW region config
	static void checkWOPCMSettings(void *that,unsigned long param_1,void *param_2);
	mach_vm_address_t ocheckWOPCMSettings {};

	static bool dotrue();  // stub: always returns true

	static uint8_t disableVDDForAux(void *that);
	mach_vm_address_t odisableVDDForAux {};

	static void prepareToEnterWake(AppleIntel::AppleIntelFramebuffer *that);
	mach_vm_address_t oprepareToEnterWake {};
	
public:
	// xe/fwsubmit: acesso público ao forcewake do NootedGreen (para ganchos livres)
	static void xeForceWakePub(void *that, bool set, uint32_t dom, uint8_t ctx) { forceWake(that, set, dom, ctx); }


	static volatile bool gGfxAccelStartDone;  // V221: set true after IntelAccelerator::start() returns
	// Resolved from IOAcceleratorFamily2 by NGreen::processKext — needed by blit3d scratch init.
	mach_vm_address_t oIOAF2_lockForCPUAccess {};
	mach_vm_address_t oIOAF2_unlockForCPUAccess {};

	void init();  // register kextInfos with Lilu
	static Gen11 *callback;  // singleton for Lilu static callbacks
	bool processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
	
	// Direct MMIO access helpers (bypass the kext's register methods)
	static void tWriteRegister32(unsigned long a, unsigned int b);
	static void tWriteRegister64(void volatile* a, unsigned long b, unsigned long long c);
	static unsigned int tReadRegister32(unsigned long a);
	static unsigned long long tReadRegister64(void volatile* a, unsigned long b);
	static uint64_t tgetPMTNow();              // read GT timestamp
	static bool thwSetupDSBMemory();           // DSB = Display State Buffer
	static uint32_t tprobePortMode(void * that);
	
};

#endif /* kern_gen8_hpp */
