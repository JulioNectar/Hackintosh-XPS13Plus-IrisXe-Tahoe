//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#pragma once
//#include "kern_netdbg.hpp"
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_iokit.hpp>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/acpi/IOACPIPlatformExpert.h>

#define BIT(n) (1<< n)
#define REG_BIT(n) (1<< n)
#define   RING_FORCE_TO_NONPRIV_ACCESS_RW	(0 << 28)
#define __MASKED_FIELD(mask, value) ((mask) << 16 | (value))
#define _MASKED_FIELD(mask, value) ({ __MASKED_FIELD(mask, value); })
#define _MASKED_BIT_ENABLE(a)	({ __typeof(a) _a = (a); _MASKED_FIELD(_a, _a); })
#define _MASKED_BIT_DISABLE(a)	(_MASKED_FIELD((a), 0))


//! Hack
class AppleACPIPlatformExpert : IOACPIPlatformExpert {
	friend class NGreen;
};

struct intel_ip_version {
	UInt8 ver;
	UInt8 rel;
	UInt8 step;
};

constexpr UInt32 mmPCIE_INDEX2 = 0xE;
constexpr UInt32 mmPCIE_DATA2 = 0xF;
/*
class EXPORT PRODUCT_NAME : public IOService {
	OSDeclareDefaultStructors(PRODUCT_NAME);

	public:
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
};*/

class NGreen {
    friend class Gen11;
	friend class Genx;
	friend class DYLDPatches;

    public:
    static NGreen *callback;
    void init();
    void processPatcher(KernelPatcher &patcher);
    bool processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);
	void setRMMIOIfNecessary();
	// V201 diagnostic: BAR2 graphics aperture mapping. Lets us read scanout buffer
	// bytes through the GTT-translated CPU mapping (safe — no MCE risk like raw PA).
	void setApertureIfNecessary();
	
	static uint16_t configRead16(IORegistryEntry *service, uint32_t space, uint8_t offset);
	static uint32_t configRead32(IORegistryEntry *service, uint32_t space, uint8_t offset);
	WIOKit::t_PCIConfigRead16 orgConfigRead16 {nullptr};
	WIOKit::t_PCIConfigRead32 orgConfigRead32 {nullptr};
	
	OSMetaClass *metaClassMap[4][2] = {{nullptr}};
	mach_vm_address_t orgSafeMetaCast {0};
	static OSMetaClassBase *wrapSafeMetaCast(const OSMetaClassBase *anObject, const OSMetaClass *toMeta);
	
	static size_t wrapFunctionReturnZero();

	static bool wrapIGAccelDeviceStart(void *that);
	mach_vm_address_t orgIGAccelDeviceStart {0};
	
	mach_vm_address_t orgApplePanelSetDisplay {0};
	static bool wrapApplePanelSetDisplay(IOService *that, IODisplay *display);
	
	UInt32 stolen_size;
	uint32_t framebufferId {0};
	
	// Public MMIO register access (used by display link training, display merge, etc.)
	UInt32 readReg32(unsigned long reg) {
		if (!rmmio || !rmmioPtr) return 0;
		if (reg + sizeof(uint32_t) <= this->rmmio->getLength()) {
			return this->rmmioPtr[reg >> 2];
		} else {
			this->rmmioPtr[mmPCIE_INDEX2] = reg;
			return this->rmmioPtr[mmPCIE_DATA2];
		}
	}

	// reg = byte offset (i915 convention). rmmioPtr is uint32_t* so divide by 4.
	void writeReg32(unsigned long reg, UInt32 val) {
		if (!rmmio || !rmmioPtr) return;
		static int v93MmioLogCount = 0;

		// Safety guard: prevent enabled display planes from being armed with SURF=0.
		// On Gen11/TGL-class paths, SURF=0 may make HW fetch GGTT[0] (stolen base),
		// which can trigger package-wide MCE on some systems.
		const bool looksLikePlaneSurf =
			(reg >= 0x60000 && reg <= 0xBFFFF) &&
			((reg & 0xFFF) == 0x19C);
		if (looksLikePlaneSurf && val == 0) {
			const uint32_t ctlReg = static_cast<uint32_t>(reg - 0x1C);
			const uint32_t planCtl = readReg32(ctlReg);
			if (planCtl & 0x80000000U) {
				const uint32_t currentSurf = readReg32(reg);
				if (currentSurf != 0) {
					if (v93MmioLogCount < 24) {
						SYSLOG("ngreen", "V93M: blocked zero SURF@0x%lx in writeReg32; keeping current 0x%x", reg, currentSurf);
						v93MmioLogCount++;
					}
					val = currentSurf;
				} else {
					// Last resort: disable plane before allowing zero surface address.
					if (v93MmioLogCount < 24) {
						SYSLOG("ngreen", "V93M: forcing plane disable before zero SURF@0x%lx in writeReg32", reg);
						v93MmioLogCount++;
					}
					writeReg32(ctlReg, planCtl & ~0x80000000U);
				}
			}
		}

		if (reg + sizeof(uint32_t) <= this->rmmio->getLength()) {
			this->rmmioPtr[reg >> 2] = val;
		} else {
			this->rmmioPtr[mmPCIE_INDEX2] = reg;
			this->rmmioPtr[mmPCIE_DATA2] = val;
		}
	}
	
	UInt64 readReg64(unsigned long reg) {
		if (!rmmio || !rmmioPtr) return 0;
		if (reg * sizeof(uint64_t) < this->rmmio->getLength()) {
			return this->rmmioPtr[reg];
		} else {
			this->rmmioPtr[mmPCIE_INDEX2] = reg;
			return this->rmmioPtr[mmPCIE_DATA2];
		}
	}

	void writeReg64(unsigned long reg, UInt64 val) {
		if (!rmmio || !rmmioPtr) return;
		static int v93Mmio64LogCount = 0;
		const bool looksLikePlaneSurf =
			(reg >= 0x60000 && reg <= 0xBFFFF) &&
			((reg & 0xFFF) == 0x19C);
		if (looksLikePlaneSurf && val == 0) {
			const uint32_t ctlReg = static_cast<uint32_t>(reg - 0x1C);
			const uint32_t planCtl = readReg32(ctlReg);
			if (planCtl & 0x80000000U) {
				const uint32_t currentSurf = readReg32(reg);
				if (currentSurf != 0) {
					if (v93Mmio64LogCount < 24) {
						SYSLOG("ngreen", "V93M64: blocked zero SURF@0x%lx in writeReg64; keeping current 0x%x", reg, currentSurf);
						v93Mmio64LogCount++;
					}
					val = currentSurf;
				} else {
					if (v93Mmio64LogCount < 24) {
						SYSLOG("ngreen", "V93M64: forcing plane disable before zero SURF@0x%lx in writeReg64", reg);
						v93Mmio64LogCount++;
					}
					writeReg32(ctlReg, planCtl & ~0x80000000U);
				}
			}
		}
		if ((reg * sizeof(uint64_t)) < this->rmmio->getLength()) {
			this->rmmioPtr[reg] = val;
		} else {
			this->rmmioPtr[mmPCIE_INDEX2] = reg;
			this->rmmioPtr[mmPCIE_DATA2] = val;
		}
	}
	
	uint32_t intel_de_rmw(uint32_t reg, uint32_t clear, uint32_t set) {
		uint32_t old, val;
		old = readReg32(reg);
		val = (old & ~clear) | set;
		writeReg32(reg, val);
		return old;
	}

    private:
	
	// Returns true when MMIO mapping is live and safe to access.
	bool mmioValid() const { return rmmio != nullptr && rmmioPtr != nullptr; }
	
	void whitelist_reg_ext(uint32_t reg, uint32_t flags)
	{
		uint32_t old;
		old = readReg32(reg);
		old = old | flags;
		writeReg32(reg, old);
	}
	
	void
	whitelist_reg(uint32_t reg)
	{
		whitelist_reg_ext( reg, RING_FORCE_TO_NONPRIV_ACCESS_RW);
	}

	void wa_add(uint32_t reg, uint32_t clear, uint32_t set, uint32_t read_mask, bool masked_reg)
	{
		uint32_t old, val;
		
		if (masked_reg) {
			/* Keep the enable mask, reset the actual target bits */
			set &= ~(set >> 16);
		}

		old = readReg32(reg);
		val = (old & ~clear) | set;
		val |= read_mask;
		writeReg32(reg, val);
	}
	
	void wa_masked_en(uint32_t reg, uint32_t val)
	{
		wa_add( reg, 0, _MASKED_BIT_ENABLE(val), val, true);
	}

	void wa_masked_field_set(uint32_t reg, uint32_t mask, uint32_t val)
	{
		wa_add(reg, 0, _MASKED_FIELD(mask, val), mask, true);
	}

	void wa_write_clr_set( uint32_t reg, uint32_t clear, uint32_t set)
	{
		wa_add( reg, clear, set, clear | set, false);
	}

	void wa_mcr_add(uint32_t reg,
						   uint32_t clear, uint32_t set, uint32_t read_mask, bool masked_reg)
	{
		wa_add(reg, clear,set,read_mask,masked_reg );
	}
	
	void
	wa_mcr_masked_en(uint32_t reg, uint32_t val)
	{
		wa_mcr_add( reg, 0, _MASKED_BIT_ENABLE(val), val, true);
	}

	void
	wa_mcr_write_clr_set(uint32_t reg, uint32_t clear, uint32_t set)
	{
		wa_mcr_add( reg, clear, set, clear | set, false);
	}

	void
	wa_write(uint32_t reg, uint32_t set)
	{
		wa_write_clr_set( reg, ~0, set);
	}

	void
	wa_write_or(uint32_t reg, uint32_t set)
	{
		wa_write_clr_set( reg, set, set);
	}

	void
	wa_mcr_write_or(uint32_t reg, uint32_t set)
	{
		wa_mcr_write_clr_set( reg, set, set);
	}

	void
	wa_write_clr(uint32_t reg, uint32_t clr)
	{
		wa_write_clr_set( reg, clr, 0);
	}

	void
	wa_mcr_write_clr(uint32_t reg, uint32_t clr)
	{
		wa_mcr_write_clr_set( reg, clr, 0);
	}
	
    bool isCflDerivative = false;
    bool isJslDerivative = false;
    bool isGen9LPDerivative = false;
    bool isGen8LPDerivative = false;
    bool isRealTGL = false;  // true if CPU is genuine TGL (model 0x8C/0x8D), false if spoofed (RPL/ADL)
public:
    bool getIsRealTGL() const { return isRealTGL; }
private:
    bool dmcIsAdlp = false;    // true when ADL-P DMC blob was loaded in hwInitializeCState
    void adlpDcExit(const char *caller);
    uint32_t uefiCtl1 {0};    // UEFI-read PWR_WELL CTL1 value saved in hwInitializeCState
    uint32_t cpuModel {0};  // full display model (family_model)
    uint32_t deviceId {0};
    uint16_t revision {0};
    uint32_t pciRevision {0};
    IOPCIDevice *iGPU {nullptr};
	
	IOMemoryMap *rmmio {nullptr};
	volatile UInt32 *rmmioPtr {nullptr};

	IOMemoryMap *aperture {nullptr};
	volatile UInt32 *aperturePtr {nullptr};
	uint64_t apertureLen {0};

	// Last RCS context object seen by IGHardwareContext::withOptions.
	// V507 uses this to re-run the LRCA slot repair on each populateResetRegisterList call.
	void *lastRCSCtx {nullptr};

};

//! Change frame-buffer count >= 2 check to >= 1.
static const UInt8 kAGDPFBCountCheckOriginal[] = {0x02, 0x00, 0x00, 0x83, 0xF8, 0x02};
static const UInt8 kAGDPFBCountCheckPatched[] = {0x02, 0x00, 0x00, 0x83, 0xF8, 0x01};

//! Ditto
static const UInt8 kAGDPFBCountCheckVenturaOriginal[] = {0x41, 0x83, 0xBE, 0x14, 0x02, 0x00, 0x00, 0x02};
static const UInt8 kAGDPFBCountCheckVenturaPatched[] = {0x41, 0x83, 0xBE, 0x14, 0x02, 0x00, 0x00, 0x01};

//! Neutralise access to AGDP configuration by board identifier.
static const UInt8 kAGDPBoardIDKeyOriginal[] = "board-id";
static const UInt8 kAGDPBoardIDKeyPatched[] =  "applehax";


struct DPCDCap16 { // 16 bytes
	// DPCD Revision (DP Config Version)
	// Value: 0x10, 0x11, 0x12, 0x13, 0x14
	uint8_t revision {};

	// Maximum Link Rate
	// Value: 0x1E (HBR3) 8.1 Gbps
	//        0x14 (HBR2) 5.4 Gbps
	//        0x0C (3_24) 3.24 Gbps
	//        0x0A (HBR)  2.7 Gbps
	//        0x06 (RBR)  1.62 Gbps
	// Reference: 0x0C is used by Apple internally.
	uint8_t maxLinkRate {};

	// Maximum Number of Lanes
	// Value: 0x1 (HBR2)
	//        0x2 (HBR)
	//        0x4 (RBR)
	// Side Notes:
	// (1) Bit 7 is used to indicate whether the link is capable of enhanced framing.
	// (2) Bit 6 is used to indicate whether TPS3 is supported.
	uint8_t maxLaneCount {};

	// Maximum Downspread
	uint8_t maxDownspread {};

	// Other fields omitted in this struct
	// Detailed information can be found in the specification
	uint8_t others[12] {};
};

