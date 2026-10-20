//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#ifndef kern_genx_hpp
#define kern_genx_hpp
#include "kern_green.hpp"
#include "kern_patcherplus.hpp"
#include <Headers/kern_util.hpp>



class Genx {
	friend class Gen11;

private:
	
	static bool start(void *that,void  *param_1);
	mach_vm_address_t ostart {};
	
	static void * wprobe(void *that,void *param_1,int *param_2);
	mach_vm_address_t owprobe {};
	
	static uint32_t validateDisplayMode(void *that,int param_1,void **param_2,void **param_3);
	mach_vm_address_t ovalidateDisplayMode {};
	
	static IOReturn wrapICLReadAUX(void *that, uint32_t address, void *buffer, uint32_t length);
	mach_vm_address_t orgICLReadAUX {};
	
	static uint32_t wrapReadRegister32(void *controller, uint32_t address);
	mach_vm_address_t owrapReadRegister32 {};
	
	static void sanitizeCDClockFrequency(void *that);
	static uint32_t wrapProbeCDClockFrequency(void *that);
	
	static void  readAndClearInterrupts(void *that,void *param_1);
	mach_vm_address_t oreadAndClearInterrupts {};
	
	uint32_t (*orgProbeCDClockFrequency)(void *) {nullptr};
	void (*orgDisableCDClock)(void *) {nullptr};
	void (*orgSetCDClockFrequency)(void *, unsigned long long) {nullptr};
	
	static void dovoid();
	static IOReturn wrapFBClientDoAttribute(void *fbclient, uint32_t attribute, unsigned long *unk1, unsigned long unk2, unsigned long *unk3, unsigned long *unk4, void *externalMethodArguments);
	mach_vm_address_t orgFBClientDoAttribute {};
	
	static void * AppleIntelScalernew(unsigned long param_1);
	mach_vm_address_t oAppleIntelScalernew {};
	
	static void * AppleIntelPlanenew(unsigned long param_1);
	mach_vm_address_t oAppleIntelPlanenew {};
	
	mach_vm_address_t ZN15AppleIntelPlaneC1Ev {};
	mach_vm_address_t ZN16AppleIntelScalerC1Ev {};
	
	mach_vm_address_t ZN16AppleIntelScaler10gMetaClassE {};
	mach_vm_address_t ZN15AppleIntelPlane10gMetaClassE {};
	
	static uint32_t hwSaveNVRAM();
	
	static int getPlatformID();
	mach_vm_address_t ogetPlatformID {};
	
	static uint8_t  AppleIntelPlaneinit(void *that,uint8_t param_1);
	mach_vm_address_t oAppleIntelPlaneinit {};
	static unsigned long  AppleIntelScalerinit(void *that,uint8_t param_1);
	mach_vm_address_t oAppleIntelScalerinit {};
	
	static void prepareToEnterWake();
	static void prepareToExitWake();
	static void prepareToExitSleep();
	static void prepareToEnterSleep();
	
	static void initForPM();
	mach_vm_address_t oinitForPM {};
	
	static unsigned long allocateDisplayResources();
	mach_vm_address_t oallocateDisplayResources {};
	
	
	mach_vm_address_t oframeBufferNotificationcallback {};
	
	static void setPanelPowerState(void *that,bool param_1);
	
	static uint32_t CallBackAGDC(void *that,uint32_t param_1,unsigned long param_2, uint param_3);
	mach_vm_address_t oCallBackAGDC {};
	
	static uint8_t isGPUSubSamplingSupportedForTiming(void *that,void *param_1);
	mach_vm_address_t oisGPUSubSamplingSupportedForTiming {};
	
	static unsigned long fastLinkTraining();
	mach_vm_address_t ofastLinkTraining {};
	
	static int LightUpEDP(void *that,void *param_1, void *param_2,void *param_3);
	mach_vm_address_t oLightUpEDP {};
	
	
public:

	void init();
	static Genx *callback;
	static uint32_t tprobePortMode(void * that);
	static void uupdateDBUF(void *that,uint param_1,uint param_2);
	static void AppleIntelPlanec1(void *that);
	static void AppleIntelScalerc1(void *that);
	bool processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);

private:
	mach_vm_address_t otprobePortMode {};
	mach_vm_address_t ouupdateDBUF {};
	
};

#endif /* kern_gen8_hpp */
