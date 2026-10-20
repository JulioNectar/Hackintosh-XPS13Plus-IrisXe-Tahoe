//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.

#include "kern_green.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>

static NGreen nb;

static const char *bootargDebug = "-NGreenDebug";


PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
	nullptr,
	0,
	&bootargDebug,
	1,
	nullptr,
	0,
	KernelVersion::Ventura,
	KernelVersion::Tahoe,
	[]() { nb.init(); },
};

// LILU_CUSTOM_IOKIT_INIT=1
/*
 OSDefineMetaClassAndStructors(PRODUCT_NAME, IOService);

 IOService *PRODUCT_NAME::probe(IOService *provider, SInt32 *score) {
	 setProperty("VersionInfo", kextVersion);
	 auto service = IOService::probe(provider, score);
	 return ADDPR(startSuccess) ? service : nullptr;
 }

 bool PRODUCT_NAME::start(IOService *provider) {
	 if (!IOService::start(provider)) {
		 SYSLOG("init", "Failed to start the parent");
		 return false;
	 }

	 if (!(lilu.getRunMode() & LiluAPI::RunningInstallerRecovery) && ADDPR(startSuccess)) {
		 auto *prop = OSDynamicCast(OSArray, this->getProperty("Drivers"));
		 if (!prop) {
			 SYSLOG("init", "Failed to get Drivers property");
			 return false;
		 }
		 auto *propCopy = prop->copyCollection();
		 if (!propCopy) {
			 SYSLOG("init", "Failed to copy Drivers property");
			 return false;
		 }
		 auto *drivers = OSDynamicCast(OSArray, propCopy);
		 if (!drivers) {
			 SYSLOG("init", "Failed to cast Drivers property");
			 OSSafeReleaseNULL(propCopy);
			 return false;
		 }
		 if (!gIOCatalogue->addDrivers(drivers)) {
			 SYSLOG("init", "Failed to add drivers");
			 OSSafeReleaseNULL(drivers);
			 return false;
		 }
		 OSSafeReleaseNULL(drivers);
	 }

	 return ADDPR(startSuccess);
 }*/
