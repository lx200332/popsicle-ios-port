/*
 * Minimal device-tree platform expert for Redmi Note 10 Pro (sweet).
 * Uses XNU's normal platform, power-domain and nub initialization. Hardware
 * interrupt and power-management services are supplied by separate drivers.
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#if defined(PANDORA)
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOKitKeys.h>
#include <sys/proc.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPM.h>
#endif

class SweetPlatformExpert : public IODTPlatformExpert {
	OSDeclareDefaultStructors(SweetPlatformExpert);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
#if defined(PANDORA)
	long getGMTTimeOfDay(void) APPLE_KEXT_OVERRIDE;
#endif
	void processTopLevel(IORegistryEntry *root) APPLE_KEXT_OVERRIDE;
	const char *deleteList(void) APPLE_KEXT_OVERRIDE;
	const char *excludeList(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetPlatformExpert, IODTPlatformExpert);

bool
SweetPlatformExpert::start(IOService *provider)
{
	IOLog("SWEET: starting device-tree platform expert\n");
	if (!IODTPlatformExpert::start(provider)) {
		return false;
	}
	registerService();
#if defined(PANDORA)
	/* 0210: what an iPhone's RTC driver publishes; IOKitInitializeTime waits up to 30 s for it. */
	publishResource("IORTC", this);
#endif
	IOLog("SWEET: platform expert registered\n");
	return true;
}

#if defined(PANDORA)
extern "C" int pandora_rtc_read(uint32_t *secs);   /* bsd/dev/fuxi_keys.c */

/*
 * 0214: what an iPhone's RTC driver answers: the wall clock in seconds. The PMK8850's RTC counter plus
 * boot-arg pandora_rtc_offset (Android's offset, measured by tools/boot_ios.sh). Without an offset the
 * counter alone, but only when it already looks like a date (2020 or later); else 0 (1970) as before.
 */
long
SweetPlatformExpert::getGMTTimeOfDay(void)
{
	uint32_t raw = 0;
	long long offset = 0;
	const int r = pandora_rtc_read(&raw);
	const bool have = PE_parse_boot_argn("pandora_rtc_offset", &offset, sizeof(offset));
	long t = 0;
	if (r == 0 && have) {
		t = (long)raw + (long)offset;
	} else if (r == 0 && raw >= 1577836800u) {
		t = (long)raw;
	}
	IOLog("PANDORA RTC: counter %u (read %d), offset %lld%s -> %ld\n", raw, r, offset, have ? "" : " (none)", t);
	return t;
}
#endif

const char *
SweetPlatformExpert::deleteList(void)
{
	return "()";
}

const char *
SweetPlatformExpert::excludeList(void)
{
#if defined(PANDORA)
	/* /defaults gets a nub: the original AppleImage4 (tools/mkprelink.py) matches it. */
	return "('chosen', 'cpus', 'options')";
#else
	return "('chosen', 'defaults', 'cpus', 'options')";
#endif
}

#if defined(PANDORA)
extern bool sweet_gic_create(IOService *platform, IORegistryEntry *node);   /* SweetGIC.cpp (0216) */
#endif

void
SweetPlatformExpert::processTopLevel(IORegistryEntry *root)
{
	/*
	 * The stock implementation requires an Apple NVRAM /options node.
	 * SWEET has no NVRAM controller; publish real CPU/device nodes without
	 * inventing one. createNubs adds the topology's logical CPU/cluster IDs.
	 */
	IORegistryEntry *cpus = root->childFromPath("cpus", gIODTPlane);
	if (cpus != NULL) {
		createNubs(this, IODTFindMatchingEntries(cpus, kIODTExclusive, NULL));
		cpus->release();
	}
	createNubs(this, IODTFindMatchingEntries(root, kIODTExclusive, excludeList()));

	/*
	 * No AppleARMIO driver publishes the /arm-io children: do it here so
	 * built-in drivers (the display, AppleCLCD) can match them. The GIC is
	 * driven by the platform code and gets no nub. The nubs hang off this
	 * platform expert: IOPlatformDevice::compareName asks its provider.
	 */
	IORegistryEntry *armio = root->childFromPath("arm-io", gIODTPlane);
	if (armio != NULL) {
#if defined(PANDORA)
		/* 0216: the GIC as IOKit's interrupt controller for the nubs' "interrupts" (still no nub of its own). */
		IORegistryEntry *gic = armio->childFromPath("gic", gIODTPlane);
		if (gic != NULL) {
			(void)sweet_gic_create(this, gic);
			gic->release();
		}
#endif
		createNubs(this, IODTFindMatchingEntries(armio, kIODTExclusive, "('gic')"));
		armio->release();
	}
}

/*
 * hw.target / hw.product come from IODTPlatformExpert: the first root
 * "compatible" string and "model" (D22AP / iPhone10,3 in the ADT), which
 * is what the iPhone userland (MobileGestalt) expects.
 */
#if defined(PANDORA)
/*
 * 0314: Apple's FairPlay driver by its class names, refusing every call. The real one
 * (com.apple.driver.FairPlayIOKit) works with keys this SoC does not have; without any, fairplayd exits at
 * start and the requests to its ports (locationd's FairPlay signer among them) wait for good. With this one
 * fairplayd runs and answers them with its own errors. The first 64 calls are logged.
 */
class com_apple_driver_FairPlayIOKit : public IOService {
	OSDeclareDefaultStructors(com_apple_driver_FairPlayIOKit);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(com_apple_driver_FairPlayIOKit, IOService);

bool
com_apple_driver_FairPlayIOKit::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "com_apple_driver_FairPlayIOKitUserClient");
	registerService();
	IOLog("PANDORA FAIRPLAY: driver registered (every call answers unsupported)\n");
	return true;
}

class com_apple_driver_FairPlayIOKitUserClient : public IOUserClient {
	OSDeclareDefaultStructors(com_apple_driver_FairPlayIOKitUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(com_apple_driver_FairPlayIOKitUserClient, IOUserClient);

IOReturn
com_apple_driver_FairPlayIOKitUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
com_apple_driver_FairPlayIOKitUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	static uint32_t logged;
	if (logged < 64) {
		logged++;
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("PANDORA FAIRPLAY: %s sel %u in %u/%u out %u/%u -> unsupported\n", name, selector,
		    args->scalarInputCount, args->structureInputDescriptor != NULL ?
		    (uint32_t)args->structureInputDescriptor->getLength() : args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputDescriptor != NULL ?
		    (uint32_t)args->structureOutputDescriptor->getLength() : args->structureOutputSize);
	}
	args->scalarOutputCount = 0;
	return kIOReturnUnsupported;
}
#if defined(PANDORA)
/*
 * Thermal level for the host power-management domain.
 *
 * bsd/dev/pandora_thermal.c reads the board thermistors and decides the level
 * (100 = warning, 110 = trap, 0 = normal), but it is C and cannot reach
 * IOPMrootDomain -- the IOService vtable has no C entry point, and the event
 * has to be an OSSymbol. This is the small bridge it calls instead. Without
 * it the level only reached a static counter and thermalmonitord never heard
 * anything, so nothing ever reduced the load (the phone ran at 100+ C).
 */
extern "C" void
sweet_thermal_publish(int level)
{
	IOPMrootDomain *root = IOService::getPMRootDomain();

	if (root == NULL) {
		return;
	}
	const OSSymbol *event = OSSymbol::withCString(kIOPMThermalLevelWarningKey);
	if (event == NULL) {
		return;
	}
	if (level != kIOPMThermalLevelNormal) {
		root->systemPowerEventOccurred(event, (uint32_t)level);
	} else {
		root->systemPowerEventOccurred(event, (uint32_t)kIOPMThermalLevelNormal);
	}
	event->release();
}
#endif /* PANDORA */
#endif /* PANDORA */
#endif /* SWEET */
