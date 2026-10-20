//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_genx.hpp"
#include "kern_gen11.hpp"
#include <Headers/kern_api.hpp>


static const char *pathG11FB0 = "/Library/Extensions/AppleIntelICLLPGraphicsFramebuffer.kext/Contents/MacOS/"
                               "AppleIntelICLLPGraphicsFramebuffer";

static KernelPatcher::KextInfo kextG11FB0 {"com.xxxxx.driver.AppleIntelICLLPGraphicsFramebuffer", &pathG11FB0, 1, {}, {},
    KernelPatcher::KextInfo::Unloaded};


Genx *Genx::callback = nullptr;

void Genx::init() {
	callback = this;
    lilu.onKextLoadForce(&kextG11FB0);

}



bool Genx::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {

    if (kextG11FB0.loadIndex == index) {
		NGreen::callback->setRMMIOIfNecessary();
		
		
		SolveRequestPlus solveRequests[] = {
			

			/*{"__ZN15ApppeIntelPlane10gMetaClassE", NGreen::callback->metaClassMap[0][0]},
			{"__ZN16ApppeIntelScaler10gMetaClassE", NGreen::callback->metaClassMap[1][0]},
			{"__ZN21ApppeIntelDisplayPath10gMetaClassE", NGreen::callback->metaClassMap[2][0]},
			{"__ZN30ApppeIntelDisplaySimController10gMetaClassE", NGreen::callback->metaClassMap[3][0]},*/
			
			/*{"__ZN14ApppeIntelPort10gMetaClassE", NGreen::callback->metaClassMap[0][1]},
			{"__ZN17ApppeIntelPortHAL10gMetaClassE", NGreen::callback->metaClassMap[1][1]},
			{"__ZN22ApppeIntelPortHALDiags10gMetaClassE", NGreen::callback->metaClassMap[2][1]},
			{"__ZN19ApppeIntelPowerWell10gMetaClassE", NGreen::callback->metaClassMap[3][1]},*/
			
			{"__ZN24AppleIntelBaseController14disableCDClockEv", this->orgDisableCDClock},
			{"__ZN24AppleIntelBaseController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
			
			//{"__ZN16ApppeIntelScaler13disableScalerEb",this->disableScaler0},
			//__ZN26AppleIntelFramebufferDiags10gMetaClassE
			
			//{"__ZN16AppleIntelScaler10gMetaClassE",this->ZN16AppleIntelScaler10gMetaClassE},
			//{"__ZN15AppleIntelPlane10gMetaClassE",this->ZN15AppleIntelPlane10gMetaClassE},
			//{"__ZN21AppleIntelFramebuffer31frameBufferNotificationcallbackEP8OSObjectPvP13IOFramebufferiS2_",this->oframeBufferNotificationcallback},
			
		};
		PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "ngreen",	"genx Failed to resolve symbols");
		
		
		RouteRequestPlus requests[] = {
			
			//{"__ZN24AppleIntelBaseController16setupBootDisplayEv",hwSaveNVRAM},
			
		//	{"__ZN21AppleIntelFramebuffer24allocateDisplayResourcesEv",allocateDisplayResources,this->oallocateDisplayResources},
			
			{"__ZN24AppleIntelBaseController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
			{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",wrapReadRegister32,	this->owrapReadRegister32},
		
			/*{"__ZN15AppleIntelPlane10updateDBUFEjj",uupdateDBUF,this->ouupdateDBUF},
			
			{"__ZN15AppleIntelPlanenwEm",AppleIntelPlanenew,this->oAppleIntelPlanenew},
			{"__ZN16AppleIntelScalernwEm",AppleIntelScalernew,this->oAppleIntelScalernew},
			
			{"__ZN15AppleIntelPlaneC1Ev",AppleIntelPlanec1,this->ZN15AppleIntelPlaneC1Ev},
			{"__ZN16AppleIntelScalerC1Ev",AppleIntelScalerc1, this->ZN16AppleIntelScalerC1Ev},
			
			{"__ZN16AppleIntelScaler4initE10IGScalerID", AppleIntelScalerinit,this->oAppleIntelScalerinit},
			{"__ZN15AppleIntelPlane4initE9IGPlaneID", AppleIntelPlaneinit,this->oAppleIntelPlaneinit},*/
			
			//{"__ZN17AppleIntelPortHAL13probePortModeEv",tprobePortMode,	this->otprobePortMode},
			
			//{"__ZN24AppleIntelBaseController13getPlatformIDEv",getPlatformID,this->ogetPlatformID},
			
			/*{"__ZN31AppleIntelFramebufferController5startEP9IOService",start,	this->ostart},
			{"__ZN24AppleIntelBaseController5probeEP9IOServicePi",wprobe,	this->owprobe},
			{"_IntelLog",dovoid},
			{"_IntelLogDecrementStackLevel",dovoid},
			{"_IntelLogIncrementStackLevel",dovoid},
			{"_IntelLogEnabled",hwSaveNVRAM},*/
			
			//{"__ZN13IOFramebuffer37addFramebufferNotificationWithOptionsEPFiP8OSObjectPvPS_iS2_ES1_S2_jij",addFramebufferNotificationWithOptions,	this->oaddFramebufferNotificationWithOptions},

			//{"__ZN21AppleIntelFramebuffer4initEP24AppleIntelBaseControllerj",AppleIntelFramebufferinit,	this->oAppleIntelFramebufferinit},
			
			
			//{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments",wrapFBClientDoAttribute,	this->orgFBClientDoAttribute},
			
			{"__ZN24AppleIntelBaseController11hwSaveNVRAMEv",hwSaveNVRAM},
			
			{"__ZN21AppleIntelFramebuffer18prepareToEnterWakeEv",prepareToEnterWake},
			{"__ZN21AppleIntelFramebuffer17prepareToExitWakeEv",prepareToExitWake},
			
			{"__ZN21AppleIntelFramebuffer18prepareToExitSleepEv",prepareToExitSleep},
			{"__ZN21AppleIntelFramebuffer19prepareToEnterSleepEv",prepareToEnterSleep},
			
			
			//{"__ZN24AppleIntelBaseController19getTranscoderOffsetEP14AppleIntelPortj",hwSaveNVRAM},
			
			//{"__ZN21AppleIntelFramebuffer14disableDisplayEb",dovoid},
			//{"__ZN24AppleIntelBaseController14disableDisplayEP21AppleIntelFramebufferP21AppleIntelDisplayPath",dovoid},
			
			//{"__ZN21AppleIntelFramebuffer9initForPMEv",initForPM,this->oinitForPM},
			
			/*{"__ZN14AppleIntelPort16fastLinkTrainingEv",fastLinkTraining,	this->ofastLinkTraining},
			{"__ZN24AppleIntelBaseController10LightUpEDPEP21AppleIntelFramebufferP21AppleIntelDisplayPathPK29IODetailedTimingInformationV2",LightUpEDP,	this->oLightUpEDP},
			{"__ZN21AppleIntelFramebuffer18setPanelPowerStateEb",setPanelPowerState},
			{"__ZN24AppleIntelBaseController15hwSetPanelPowerEj",dovoid},*/
			
			//{"__ZN21AppleIntelFramebuffer19validateDisplayModeEiPPKNS_15ModeDescriptionEPPK29IODetailedTimingInformationV2",validateDisplayMode,	this->ovalidateDisplayMode},
			
			
			//{"__ZN24AppleIntelBaseController13GetLinkConfigEP16AGDCLinkConfig_tS1_",hwSaveNVRAM},
			//{"__ZN21AppleIntelFramebuffer19getPixelInformationEiiiP18IOPixelInformation",hwSaveNVRAM},
			
			
			//{"__ZN24AppleIntelBaseController34isGPUSubSamplingSupportedForTimingEPK29IODetailedTimingInformationV2",isGPUSubSamplingSupportedForTiming,	this->oisGPUSubSamplingSupportedForTiming},
			
			//{"__ZN14AppleIntelPort7readAUXEjPvj",wrapICLReadAUX,	this->orgICLReadAUX},
			
			//{"__ZN24AppleIntelBaseController12CallBackAGDCE31kAGDCRegisterLinkControlEvent_tmj",CallBackAGDC,	this->oCallBackAGDC},
			{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments",wrapFBClientDoAttribute,	this->orgFBClientDoAttribute},
			
			//{"__ZN21AppleIntelFramebuffer14disableDisplayEb",dovoid},
			
			//{"__ZN24AppleIntelBaseController14disableDisplayEP21AppleIntelFramebufferP21AppleIntelDisplayPath",hwSaveNVRAM},
			
			//{"__ZN14AppleIntelPort14getBuiltInPortEv",hwSaveNVRAM},
			
			
			
			
		};
		PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","genx Failed to route symbols");
		
		//connectors
		static const uint8_t f1[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 
			0x02, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0xC1, 0x02, 0x00, 0x00,
			0x03, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0xC1, 0x02, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
		
		static const uint8_t r1[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x03, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

		//hwreg
		static const uint8_t f2[]= {0x74, 0x62, 0x48, 0xff, 0x05, 0x13, 0x71, 0x10, 0x00, 0xbf, 0x04, 0x00, 0x00, 0x00, 0xbe, 0x10, 0x00, 0x00, 0x00};
		static const uint8_t r2[]= {0xeb, 0x62, 0x48, 0xff, 0x05, 0x13, 0x71, 0x10, 0x00, 0xbf, 0x04, 0x00, 0x00, 0x00, 0xbe, 0x10, 0x00, 0x00, 0x00};
		
		//getPathByPipe log
		static const uint8_t f3[]= {0x74, 0x36, 0x48, 0xff, 0x05, 0x52, 0xdc, 0x09, 0x00, 0x44, 0x89, 0x3c, 0x24, 0x48, 0x8d, 0x15, 0xc2, 0x04, 0x04, 0x00, 0x4c, 0x8d, 0x05, 0x9f, 0x06, 0x04, 0x00};
		static const uint8_t r3[]= {0xeb, 0x36, 0x48, 0xff, 0x05, 0x52, 0xdc, 0x09, 0x00, 0x44, 0x89, 0x3c, 0x24, 0x48, 0x8d, 0x15, 0xc2, 0x04, 0x04, 0x00, 0x4c, 0x8d, 0x05, 0x9f, 0x06, 0x04, 0x00};
		
		//getBuiltInPor
		static const uint8_t f4[]= {0x48, 0x89, 0x05, 0x44, 0xbf, 0x16, 0x00, 0x48, 0x8b, 0x83, 0x40, 0x04, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x75, 0x0a};
		static const uint8_t r4[]= {0x48, 0x89, 0x05, 0x44, 0xbf, 0x16, 0x00, 0x48, 0x8b, 0x83, 0x40, 0x04, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0xeb, 0x0a};
		//probeBootPipe
		static const uint8_t f4b[]= {0xbe, 0x00, 0xf4, 0x06, 0x00, 0x48, 0x89, 0xdf, 0xff, 0x90, 0xb0, 0x08, 0x00, 0x00, 0x41, 0x89, 0xc6, 0x48, 0x8b, 0x03, 0xbe, 0x00, 0x04, 0x06, 0x00};
		static const uint8_t r4b[]= {0xbe, 0x00, 0x04, 0x06, 0x00, 0x48, 0x89, 0xdf, 0xff, 0x90, 0xb0, 0x08, 0x00, 0x00, 0x41, 0x89, 0xc6, 0x48, 0x8b, 0x03, 0xbe, 0x00, 0xf4, 0x06, 0x00};
		
		

		
		//osinfo  fMaxSliceCount 2 fMaxEUCount 8 subslices 8 ?
		static const uint8_t f6[]= {0x49, 0xbe, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x35, 0xa3, 0x28, 0x12, 0x00, 0xb8, 0x08, 0x00, 0x00, 0x00};
		static const uint8_t r6[]= {0x49, 0xbe, 0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x35, 0xa3, 0x28, 0x12, 0x00, 0xb8, 0x05, 0x00, 0x00, 0x00};
		//flags
		static const uint8_t f6b[]= {0xb8, 0x04, 0xe3, 0x00, 0x00, 0x89, 0x05, 0xf3, 0x28, 0x12, 0x00, 0x89, 0x1d, 0xf5, 0x28, 0x12, 0x00, 0x4c, 0x89, 0x3d, 0xf6, 0x28, 0x12, 0x00};
		static const uint8_t r6b[]= {0xb8, 0x05, 0x08, 0x00, 0x00, 0x89, 0x05, 0xf3, 0x28, 0x12, 0x00, 0x89, 0x1d, 0xf5, 0x28, 0x12, 0x00, 0x4c, 0x89, 0x3d, 0xf6, 0x28, 0x12, 0x00};
		/*fInfoHasLid                  : 1
		fInfoPipeCount               : 3
		fInfoPortCount               : 3
		fInfoFramebufferCount        : 3*/
		static const uint8_t f6c[]= {0x4c, 0x89, 0x2d, 0x90, 0x28, 0x12, 0x00, 0xb8, 0x01, 0x03, 0x03, 0x03};
		static const uint8_t r6c[]= {0x4c, 0x89, 0x2d, 0x90, 0x28, 0x12, 0x00, 0xb8, 0x01, 0x04, 0x02, 0x02};
		
		//pixel
		static const uint8_t f7[]= {0x48, 0xf7, 0xe1, 0x48, 0xc1, 0xea, 0x08, 0x48, 0x69, 0xc2, 0x50, 0xc3, 0x00, 0x00, 0x49, 0x89, 0x45, 0x28};
		static const uint8_t r7[]= {0x48, 0xf7, 0xe1, 0x48, 0xc1, 0xea, 0x08, 0x48, 0x31, 0xc0, 0x90, 0x90, 0x90, 0x90, 0x49, 0x89, 0x45, 0x28};
		
		static const uint8_t f7b[]= {0x48, 0x89, 0xd8, 0x31, 0xd2, 0x48, 0xf7, 0xf1, 0x49, 0x89, 0x45, 0x28, 0x45, 0x84, 0xe4, 0x74, 0x2b};
		static const uint8_t r7b[]= {0x48, 0x89, 0xd8, 0x31, 0xd2, 0x48, 0x31, 0xc0, 0x49, 0x89, 0x45, 0x28, 0x45, 0x84, 0xe4, 0xeb, 0x2b};
		
		static const uint8_t f7c[]= {0x49, 0x0f, 0xaf, 0xc4, 0x89, 0xd9, 0x48, 0x0f, 0xaf, 0xc8, 0x49, 0x89, 0x4d, 0x28};
		static const uint8_t r7c[]= {0x49, 0x0f, 0xaf, 0xc4, 0x89, 0xd9, 0x48, 0x31, 0xc9, 0x90, 0x49, 0x89, 0x4d, 0x28};
		
		//pipe a
		static const uint8_t f8[]= {0x48, 0xff, 0x05, 0x88, 0xd5, 0x10, 0x00, 0x41, 0xc1, 0xe7, 0x0c, 0x48, 0x8b, 0x80, 0x40, 0x04, 0x00, 0x00, 0x83, 0x78, 0x08, 0x00, 0x0f, 0x84, 0x7b, 0x01, 0x00, 0x00};
		static const uint8_t r8[]= {0x48, 0xff, 0x05, 0x88, 0xd5, 0x10, 0x00, 0x41, 0xc1, 0xe7, 0x0c, 0x48, 0x8b, 0x80, 0x40, 0x04, 0x00, 0x00, 0x83, 0x78, 0x08, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		static const uint8_t f8b[]= {0xe9, 0x69, 0x04, 0x00, 0x00, 0x48, 0xff, 0x05, 0x84, 0x3c, 0x0a, 0x00, 0xbe, 0x00, 0xf0, 0x06, 0x00};
		static const uint8_t r8b[]= {0xe9, 0x69, 0x04, 0x00, 0x00, 0x48, 0xff, 0x05, 0x84, 0x3c, 0x0a, 0x00, 0xbe, 0x00, 0x00, 0x06, 0x00};
		
		static const uint8_t f8c[]= {0xeb, 0x0c, 0x48, 0xff, 0x05, 0x5e, 0x3c, 0x0a, 0x00, 0xbe, 0x0c, 0xf0, 0x06, 0x00};
		static const uint8_t r8c[]= {0xeb, 0x0c, 0x48, 0xff, 0x05, 0x5e, 0x3c, 0x0a, 0x00, 0xbe, 0x0c, 0x00, 0x06, 0x00};
		
		//cpu id
		static const uint8_t f9[]= {0x02, 0x00, 0x5c, 0x8a};
		static const uint8_t r9[]= {0x00, 0x00, 0x49, 0x9a};
		//mem
		static const uint8_t f9b[]= {0x0f, 0x94, 0xc0, 0xb9, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x80, 0x00};
		static const uint8_t r9b[]= {0x0f, 0x94, 0xc0, 0xb9, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x40, 0x00};
		//cdclock
		static const uint8_t f9c[]= {0x48, 0xc7, 0x83, 0xd8, 0x4a, 0x00, 0x00, 0x00, 0x2d, 0x31, 0x01, 0x48, 0xc7, 0x83, 0xe0, 0x4a, 0x00, 0x00, 0x00, 0x54, 0xea, 0x2a, 0xc6, 0x83, 0x2d, 0x4d, 0x00, 0x00, 0x00};
		static const uint8_t r9c[]= {0x48, 0xc7, 0x83, 0xd8, 0x4a, 0x00, 0x00, 0x00, 0xa3, 0x02, 0x00, 0x48, 0xc7, 0x83, 0xe0, 0x4a, 0x00, 0x00, 0x00, 0xf6, 0x09, 0x00, 0xc6, 0x83, 0x2d, 0x4d, 0x00, 0x00, 0x00};
		
		
		//ext disp
		static const uint8_t f10[]= {0x48, 0xff, 0x05, 0xc1, 0x42, 0x15, 0x00, 0x41, 0x83, 0xbd, 0xdc, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xf2, 0x00, 0x00, 0x00};
		static const uint8_t r10[]= {0x48, 0xff, 0x05, 0xc1, 0x42, 0x15, 0x00, 0x41, 0x83, 0xbd, 0xdc, 0x01, 0x00, 0x00, 0x00, 0x48, 0xe9, 0xf2, 0x00, 0x00, 0x00};
		
		static const uint8_t f10b[]= {0x0f, 0xb6, 0x87, 0x2a, 0x0e, 0x00, 0x00, 0x48, 0x01, 0x05, 0xa6, 0x41, 0x15, 0x00, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0xe2, 0xfe, 0xff, 0xff};
		static const uint8_t r10b[]= {0x0f, 0xb6, 0x87, 0x2a, 0x0e, 0x00, 0x00, 0x48, 0x01, 0x05, 0xa6, 0x41, 0x15, 0x00, 0x48, 0x85, 0xc0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		static const uint8_t f10c[]= {0x75, 0x40, 0x48, 0xff, 0x05, 0xbd, 0x45, 0x15, 0x00, 0x41, 0x80, 0xbd, 0xe0, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xdb, 0x00, 0x00, 0x00};
		static const uint8_t r10c[]= {0x90, 0x90, 0x48, 0xff, 0x05, 0xbd, 0x45, 0x15, 0x00, 0x41, 0x80, 0xbd, 0xe0, 0x01, 0x00, 0x00, 0x00, 0x48, 0xe9, 0xdb, 0x00, 0x00, 0x00};
		
		
		LookupPatchPlus const patches[] = {
			
			{&kextG11FB0, f2, r2, arrsize(f2),	1},
			
			//{&kextG11FB0, f1, r1, arrsize(f1),	1},
			
			{&kextG11FB0, f3, r3, arrsize(f3),	1},
			
			//{&kextG11FB0, f4, r4, arrsize(f4),	1},
			//{&kextG11FB0, f4b, r4b, arrsize(f4b),	1},
			
			//{&kextG11FB0, f6, r6, arrsize(f6),	1},
			//{&kextG11FB0, f6b, r6b, arrsize(f6b),	1},
			//{&kextG11FB0, f6c, r6c, arrsize(f6c),	1},
			
			/*{&kextG11FB0, f7, r7, arrsize(f7),	1},
			{&kextG11FB0, f7b, r7b, arrsize(f7b),	1},
			{&kextG11FB0, f7c, r7c, arrsize(f7c),	1},*/
			
			//{&kextG11FB0, f8, r8, arrsize(f8),	1},
			//{&kextG11FB0, f8b, r8b, arrsize(f8b),	1},
			//{&kextG11FB0, f8c, r8c, arrsize(f8c),	1},
			
			{&kextG11FB0, f9, r9, arrsize(f9),	1},
			//{&kextG11FB0, f9b, r9b, arrsize(f9b),	1},
			//{&kextG11FB0, f9c, r9c, arrsize(f9c),	1},
			
			/*{&kextG11FB0, f10, r10, arrsize(f10),	1},
			{&kextG11FB0, f10b, r10b, arrsize(f10b),	1},
			{&kextG11FB0, f10c, r10c, arrsize(f10c),	1},*/

			
		};

		PANIC_COND(!LookupPatchPlus::applyAll(patcher, patches , address, size), "ngreen", "kextG11FB0 Failed to apply patches!");

			auto startAddress = patcher.solveSymbol(index, "__ZN24AppleIntelBaseController13FBMemMgr_InitEv", address, size);
			if (startAddress){
				hde64s handle;
				uint64_t shllAddr = 0, andlAddr = 0;
				uint32_t shllSize = 0, andlSize = 0;
				uint32_t shllDstr = 0, andlDstr = 0;
				
				uint8_t movl[] = {0x41, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x90};
				uint8_t nops[] = {0x90, 0x90, 0x90, 0x90};
				
				for (auto index = 0; index < 64; index += 1) {
					auto size = Disassembler::hdeDisasm(startAddress, &handle);
					
					if (handle.flags & F_ERROR) {
						break;
					}
					
					// Instruction: shll $0x11, %???
					// 3 bytes long if DSTReg < %r8d, otherwise 4 bytes long
					if (handle.opcode == 0xC1 && handle.imm.imm8 == 0x11) {
						shllAddr = startAddress;
						shllSize = handle.len;
						shllDstr = (handle.rex_b << 3) | handle.modrm_rm;
					}
					
					// Instruction: andl $0xFE000000, %???
					// 5 bytes long if DSTReg is %eax; 6 bytes long if DSTReg < %r8d; otherwise 7 bytes long.
					if ((handle.opcode == 0x25 || handle.opcode == 0x81) && handle.imm.imm32 == 0xFE000000) {
						andlAddr = startAddress;
						andlSize = handle.len;
						andlDstr = (handle.rex_b << 3) | handle.modrm_rm;
					}
					
					// Guard: Calculate and apply the binary patch if we have found both instructions
					if (shllAddr && andlAddr) {
						// Update the `movl` instruction with the actual amount of DVMT preallocated memory
						*reinterpret_cast<uint32_t*>(movl + 2) = NGreen::callback->stolen_size;
						SYSLOG("ngreen", "FBMemMgr_Init: patching stolen_size=0x%x", NGreen::callback->stolen_size);
						
						// Update the `movl` instruction with the actual destination register
						// Find the actual starting point of the patch and the number of bytes to patch
						uint8_t* patchStart;
						uint32_t patchSize;
						if (andlDstr >= 8) {
							// %r8d, %r9d, ..., %r15d
							movl[1] += (andlDstr - 8);
							patchStart = movl;
							patchSize = 7;
						} else {
							// %eax, %ecx, ..., %edi
							movl[1] += andlDstr;
							patchStart = (movl + 1);
							patchSize = andlDstr == 0 ?  5 : 6;
						}
						
						// Guard: Prepare to apply the binary patch
						if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
						}
						
						// Replace `shll` with `nop`s
						// The number of nops is determined by the actual instruction length
						lilu_os_memcpy(reinterpret_cast<void*>(shllAddr), nops, shllSize);
						
						// Replace `andl` with `movl`
						// The patch contents and size are determined by the destination register of `andl`
						lilu_os_memcpy(reinterpret_cast<void*>(andlAddr), patchStart, patchSize);
						
						// Finished applying the binary patch
						MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
					}
					
					startAddress += size;
				}
			}
		
		
        DBGLOG("ngreen", "Loaded ApppeIntelICLLPGraphicsFramebuffer!");
        return true;
		
    }

    return false;
}


IOReturn Genx::wrapICLReadAUX(void *that, uint32_t address, void *buffer, uint32_t length) {

	IOReturn retVal =	FunctionCast(wrapICLReadAUX, callback->orgICLReadAUX)(that,address, buffer, length );

	if (address != 0x0000 && address != 0x2200)	return retVal;

	auto caps = reinterpret_cast<DPCDCap16*>(buffer);

	if (caps->revision < 0x03) {
		caps->maxLinkRate=0;
	}

	return retVal;
}

uint32_t Genx::hwSaveNVRAM()
{
	return 0;

}

bool Genx::start(void *that,void  *param_1)
{
	return false;

}
void * Genx::wprobe(void *that,void *param_1,int *param_2)
{
	return that;
}

void Genx::dovoid()
{
	
	
}
IOReturn Genx::wrapFBClientDoAttribute(void *fbclient, uint32_t attribute, unsigned long *unk1, unsigned long unk2, unsigned long *unk3, unsigned long *unk4 , void *externalMethodArguments) {
	if (attribute == 0x923) {
		return kIOReturnUnsupported;
	}
	
	return FunctionCast(wrapFBClientDoAttribute, callback->orgFBClientDoAttribute)(fbclient, attribute, unk1, unk2, unk3, unk4,  externalMethodArguments);
}

uint8_t  Genx::AppleIntelPlaneinit(void *that,uint8_t param_1)
{
	return FunctionCast(AppleIntelPlaneinit, callback->oAppleIntelPlaneinit)(that,param_1 );
}
unsigned long  Genx::AppleIntelScalerinit(void *that,uint8_t param_1)
{
	return FunctionCast(AppleIntelScalerinit, callback->oAppleIntelScalerinit)(that,param_1 );
}

uint32_t Genx::tprobePortMode(void * that)
{
	return FunctionCast(tprobePortMode, callback->otprobePortMode)(that );
}

int Genx::getPlatformID()
{
 return Gen11::callback->getPlatformID();
}

void Genx::prepareToEnterWake()
{
 //FunctionCast(prepareToEnterWake, Gen11::callback->oprepareToEnterWake)( );
}

void Genx::prepareToExitWake()
{
	//initForPM();
 //FunctionCast(prepareToExitWake, Gen11::callback->oprepareToExitWake)( );
}
void Genx::prepareToExitSleep()
{
	//initForPM();
 //FunctionCast(prepareToExitSleep, Gen11::callback->oprepareToExitSleep)( );
}
void Genx::prepareToEnterSleep()
{
 //FunctionCast(prepareToEnterSleep, Gen11::callback->oprepareToEnterSleep)( );
}

void  Genx::AppleIntelPlanec1(void *that)
{
 FunctionCast(AppleIntelPlanec1, callback->ZN15AppleIntelPlaneC1Ev)(that );
}

void  Genx::AppleIntelScalerc1(void *that)
{
	FunctionCast(AppleIntelScalerc1, callback->ZN16AppleIntelScalerC1Ev)(that );
}

void * Genx::AppleIntelScalernew(unsigned long param_1)
{
	return FunctionCast(AppleIntelScalernew, callback->oAppleIntelScalernew)(param_1 );
}
void * Genx::AppleIntelPlanenew(unsigned long param_1)
{
	return FunctionCast(AppleIntelPlanenew, callback->oAppleIntelPlanenew)(param_1 );
}

void Genx::uupdateDBUF(void *that,uint param_1,uint param_2)
{
	FunctionCast(uupdateDBUF, callback->ouupdateDBUF)(that,param_1,param_2 );
}

unsigned long Genx::allocateDisplayResources()
{
	return FunctionCast(allocateDisplayResources, callback->oallocateDisplayResources)( );
}

void Genx::initForPM()
{
	FunctionCast(initForPM, callback->oinitForPM)( );
}


void Genx::setPanelPowerState(void *that,bool param_1)
{
	IOFramebuffer *f=(IOFramebuffer*)that;
	if (f) f->setProperty("AAPL,LCD-PowerState-ON", param_1);
}

uint32_t Genx::CallBackAGDC(void *that,uint32_t param_1,unsigned long param_2, uint param_3)
{
	//auto ret=FunctionCast(CallBackAGDC, callback->oCallBackAGDC)(that ,param_1,param_2,param_3);
	
	return 1;
}


uint8_t Genx::isGPUSubSamplingSupportedForTiming(void *that,void *param_1)
{
	auto ret=FunctionCast(isGPUSubSamplingSupportedForTiming, callback->oisGPUSubSamplingSupportedForTiming)(that ,param_1);
	return 1;
}

unsigned long Genx::fastLinkTraining()
{
	
	FunctionCast(fastLinkTraining, callback->ofastLinkTraining)();
	return 1;
};
int Genx::LightUpEDP(void *that,void *param_1, void *param_2,void *param_3)
{
	FunctionCast(LightUpEDP, callback->oLightUpEDP)(that,param_1,param_2,param_3);
	return 0;
};


uint32_t Genx::validateDisplayMode(void *that,int param_1,void **param_2,void **param_3)
{
	auto ret=FunctionCast(validateDisplayMode, callback->ovalidateDisplayMode)(that ,param_1,param_2,param_3);
	if (ret==0xe00002c2) return 0;
	return ret;
}

void Genx::sanitizeCDClockFrequency(void *that) {

	//auto referenceFrequency = callback->wrapReadRegister32(that, SKL_DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK;
	auto referenceFrequency = callback->wrapReadRegister32(that, ICL_REG_DSSM) >> 29;
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
	
	callback->orgDisableCDClock(that);
	
	callback->orgSetCDClockFrequency(that, newPLLFrequency);

}

uint32_t Genx::wrapProbeCDClockFrequency(void *that) {

	auto cdclk = callback->wrapReadRegister32(that, ICL_REG_CDCLK_CTL) & CDCLK_FREQ_DECIMAL_MASK;

	
	if (cdclk < ICL_CDCLK_DEC_FREQ_THRESHOLD) {
		sanitizeCDClockFrequency(that);
	}
	
	auto retVal = callback->orgProbeCDClockFrequency(that);
	return retVal;
}

uint32_t Genx::wrapReadRegister32(void *controller, uint32_t address) {
	uint32_t retVal = FunctionCast(wrapReadRegister32, callback->owrapReadRegister32)(controller,address);

	return retVal;
}
