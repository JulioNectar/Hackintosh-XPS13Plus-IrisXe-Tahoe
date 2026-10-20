// XeShim: minimal, stable kernel shim that exposes the Intel Xe iGPU to a
// userspace bring-up tool, so driver logic can be iterated without reboots
// (same idea as Linux UIO / Asahi's userspace GPU bring-up).
//
// The shim itself never touches GPU registers. It offers:
//   - mapping of BAR0 (MMIO + GGTT) and BAR2 (aperture) into the client
//   - physically contiguous DMA buffers (with their bus addresses)
//   - PCI config reads
// Only the console user may open it.

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/c++/OSArray.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>

static void terminateFramebuffers(IOPCIDevice *pci, bool victimIsXeFB);

enum {
    kXeShimInfo = 0,       // out: 16 x uint64 (see fillInfo)
    kXeShimAllocDMA = 1,   // in: size, cacheMode -> out: handle, busAddr, size
    kXeShimFreeDMA = 2,    // in: handle
    kXeShimCfgRead32 = 3,  // in: offset -> out: value
    kXeShimArmICL = 4,     // in: 1 = mark GPU with xe-icl-accel and rematch, 0 = clear mark
    kXeShimFBSwap = 5,     // in: 1 = XeFB takes the display (IONDRV framebuffers terminated), 0 = give it back
    kXeShimMethodCount
};

enum {
    kXeShimMemBAR0 = 0,
    kXeShimMemBAR2 = 1,
    kXeShimMemDMABase = 0x100,  // + handle
};

static const int kMaxDMA = 64;

class XeShim : public IOService {
    OSDeclareDefaultStructors(XeShim);
    thread_call_t armCall = nullptr;
    static void armThunk(thread_call_param_t self, thread_call_param_t unused);
public:
    IOPCIDevice *pci = nullptr;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void armAtBoot(void);
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           OSDictionary *properties, IOUserClient **handler) override;
};

class XeShimUserClient : public IOUserClient {
    OSDeclareDefaultStructors(XeShimUserClient);
    XeShim *shim = nullptr;
    task_t task = nullptr;
    IOBufferMemoryDescriptor *dma[kMaxDMA] = {};
    IOLock *lock = nullptr;

    static const IOExternalMethodDispatch sMethods[kXeShimMethodCount];
    static IOReturn sInfo(OSObject *t, void *, IOExternalMethodArguments *a);
    static IOReturn sAlloc(OSObject *t, void *, IOExternalMethodArguments *a);
    static IOReturn sFree(OSObject *t, void *, IOExternalMethodArguments *a);
    static IOReturn sCfgRead(OSObject *t, void *, IOExternalMethodArguments *a);
    static IOReturn sArmICL(OSObject *t, void *, IOExternalMethodArguments *a);
    static IOReturn sFBSwap(OSObject *t, void *, IOExternalMethodArguments *a);
    void freeAll();
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                 IOMemoryDescriptor **memory) override;
};

OSDefineMetaClassAndStructors(XeShim, IOService);
OSDefineMetaClassAndStructors(XeShimUserClient, IOUserClient);

// ---------------------------------------------------------------- XeShim

bool XeShim::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;
    setProperty("IOUserClientClass", "XeShimUserClient");
    IOLog("XeShim: attached to %08x\n", pci->configRead32(0));
    registerService();
    // Hand the display to XeFB at boot, before WindowServer starts (a userspace arm came too late).
    // Only with the xefb=1 boot-arg, so the kext can be loaded at boot with the swap disabled.
    int xefbArm = 0;
    bool armRequested = PE_parse_boot_argn("xefb", &xefbArm, sizeof(xefbArm)) && xefbArm;
    IOLog("XeShim: boot arm %s\n", armRequested ? "requested" : "off (no xefb=1 boot-arg)");
    armCall = armRequested ? thread_call_allocate(&XeShim::armThunk, this) : nullptr;
    if (armCall) {
        uint64_t deadline;
        clock_interval_to_deadline(4000, kMillisecondScale, &deadline);
        thread_call_enter_delayed(armCall, deadline);
    }
    return true;
}

void XeShim::stop(IOService *provider) {
    if (armCall) {
        thread_call_cancel(armCall);
        armCall = nullptr;
    }
    IOService::stop(provider);
}

void XeShim::armThunk(thread_call_param_t self, thread_call_param_t) {
    ((XeShim *)self)->armAtBoot();
}

// Same as the xe-fb=1 path of the user client (method 5), run from the kernel at boot.
void XeShim::armAtBoot(void) {
    IOLog("XeShim: boot arm, marking xe-fb\n");
    pci->setProperty("xe-fb", kOSBooleanTrue);
    terminateFramebuffers(pci, false);
    pci->registerService();
    IOLog("XeShim: boot arm done\n");
}

IOReturn XeShim::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                               OSDictionary *properties, IOUserClient **handler) {
    if (IOUserClient::clientHasPrivilege(owningTask, kIOClientPrivilegeLocalUser) != kIOReturnSuccess &&
        IOUserClient::clientHasPrivilege(owningTask, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    XeShimUserClient *uc = OSTypeAlloc(XeShimUserClient);
    if (!uc) return kIOReturnNoMemory;
    if (!uc->initWithTask(owningTask, securityID, type) || !uc->attach(this) || !uc->start(this)) {
        uc->detach(this);
        uc->release();
        return kIOReturnError;
    }
    *handler = uc;
    return kIOReturnSuccess;
}

// ------------------------------------------------------- XeShimUserClient

const IOExternalMethodDispatch XeShimUserClient::sMethods[kXeShimMethodCount] = {
    {sInfo, 0, 0, 16, 0},
    {sAlloc, 2, 0, 3, 0},
    {sFree, 1, 0, 0, 0},
    {sCfgRead, 1, 0, 1, 0},
    {sArmICL, 1, 0, 0, 0},
    {sFBSwap, 1, 0, 0, 0},
};

bool XeShimUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type) {
    if (!IOUserClient::initWithTask(owningTask, securityID, type)) return false;
    task = owningTask;
    lock = IOLockAlloc();
    return lock != nullptr;
}

bool XeShimUserClient::start(IOService *provider) {
    shim = OSDynamicCast(XeShim, provider);
    return shim && IOUserClient::start(provider);
}

void XeShimUserClient::freeAll() {
    for (int i = 0; i < kMaxDMA; i++) {
        if (dma[i]) {
            dma[i]->complete();
            dma[i]->release();
            dma[i] = nullptr;
        }
    }
}

IOReturn XeShimUserClient::clientClose() {
    // Note: the GPU may still reference these pages if the client left work
    // queued; the bring-up tool must idle the engines before exiting.
    IOLockLock(lock);
    freeAll();
    IOLockUnlock(lock);
    terminate();
    return kIOReturnSuccess;
}

void XeShimUserClient::free() {
    if (lock) {
        freeAll();
        IOLockFree(lock);
        lock = nullptr;
    }
    IOUserClient::free();
}

IOReturn XeShimUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                          IOExternalMethodDispatch *dispatch, OSObject *target,
                                          void *reference) {
    if (selector >= kXeShimMethodCount) return kIOReturnUnsupported;
    return IOUserClient::externalMethod(selector, args,
                                        (IOExternalMethodDispatch *)&sMethods[selector], this, nullptr);
}

static uint64_t barInfo(IOPCIDevice *pci, uint8_t reg, uint64_t *len) {
    IODeviceMemory *m = pci->getDeviceMemoryWithRegister(reg);
    *len = m ? m->getLength() : 0;
    return m ? m->getPhysicalAddress() : 0;
}

IOReturn XeShimUserClient::sInfo(OSObject *t, void *, IOExternalMethodArguments *a) {
    XeShimUserClient *self = (XeShimUserClient *)t;
    IOPCIDevice *pci = self->shim->pci;
    uint64_t *o = a->scalarOutput;
    for (int i = 0; i < 16; i++) o[i] = 0;
    o[0] = pci->configRead32(0x00);
    o[1] = pci->configRead32(0x08);
    o[2] = barInfo(pci, kIOPCIConfigBaseAddress0, &o[3]);
    o[4] = barInfo(pci, kIOPCIConfigBaseAddress2, &o[5]);
    o[6] = pci->configRead32(0x50);  // GGC
    o[7] = pci->configRead32(0x5C);  // BDSM
    o[8] = pci->configRead32(0x04);
    a->scalarOutputCount = 16;
    return kIOReturnSuccess;
}

IOReturn XeShimUserClient::sAlloc(OSObject *t, void *, IOExternalMethodArguments *a) {
    XeShimUserClient *self = (XeShimUserClient *)t;
    uint64_t size = round_page(a->scalarInput[0]);
    if (size == 0 || size > (64ULL << 20)) return kIOReturnBadArgument;

    IOLockLock(self->lock);
    int h = -1;
    for (int i = 0; i < kMaxDMA; i++) if (!self->dma[i]) { h = i; break; }
    if (h < 0) { IOLockUnlock(self->lock); return kIOReturnNoResources; }

    // 39-bit DMA mask: the Xe-LP GGTT/PPGTT address space.
    IOBufferMemoryDescriptor *bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryKernelUserShared,
        size, 0x0000007FFFFFF000ULL);
    if (!bmd || bmd->prepare() != kIOReturnSuccess) {
        if (bmd) bmd->release();
        IOLockUnlock(self->lock);
        return kIOReturnNoMemory;
    }
    bzero(bmd->getBytesNoCopy(), size);
    IOByteCount segLen = 0;
    addr64_t phys = bmd->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
    self->dma[h] = bmd;
    IOLockUnlock(self->lock);

    a->scalarOutput[0] = (uint64_t)h;
    a->scalarOutput[1] = phys;
    a->scalarOutput[2] = size;
    a->scalarOutputCount = 3;
    return kIOReturnSuccess;
}

IOReturn XeShimUserClient::sFree(OSObject *t, void *, IOExternalMethodArguments *a) {
    XeShimUserClient *self = (XeShimUserClient *)t;
    uint64_t h = a->scalarInput[0];
    if (h >= kMaxDMA) return kIOReturnBadArgument;
    IOLockLock(self->lock);
    IOBufferMemoryDescriptor *bmd = self->dma[h];
    self->dma[h] = nullptr;
    IOLockUnlock(self->lock);
    if (!bmd) return kIOReturnBadArgument;
    bmd->complete();
    bmd->release();
    return kIOReturnSuccess;
}

IOReturn XeShimUserClient::sCfgRead(OSObject *t, void *, IOExternalMethodArguments *a) {
    XeShimUserClient *self = (XeShimUserClient *)t;
    uint64_t off = a->scalarInput[0];
    if (off > 0xFFC || (off & 3)) return kIOReturnBadArgument;
    a->scalarOutput[0] = self->shim->pci->configRead32((uint8_t)off);
    a->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

// The only matching-related capability: toggle a fixed marker property on the
// GPU nub. The XeICLBridge codeless kext only matches when it is present, so
// the experimental Apple accelerator never attaches at boot.
IOReturn XeShimUserClient::sArmICL(OSObject *t, void *, IOExternalMethodArguments *a) {
    XeShimUserClient *self = (XeShimUserClient *)t;
    IOPCIDevice *pci = self->shim->pci;
    if (a->scalarInput[0]) {
        pci->setProperty("xe-icl-accel", kOSBooleanTrue);
        IOLog("XeShim: xe-icl-accel armed, rematching\n");
        pci->registerService();
    } else {
        pci->removeProperty("xe-icl-accel");
        IOLog("XeShim: xe-icl-accel cleared\n");
    }
    return kIOReturnSuccess;
}

// Stage 1 display takeover. Mark the GPU xe-fb=true, terminate every IOFramebuffer under it
// that is not XeFB (Apple's IONDRVFramebuffer), then rematch so XeFB's personality starts.
// Argument 0 reverses it: terminate XeFB, clear the marker, rematch so IONDRV comes back.
// victimIsXeFB: false = terminate every framebuffer except XeFB (takeover),
//               true  = terminate only XeFB (give the display back).
static void terminateFramebuffers(IOPCIDevice *pci, bool victimIsXeFB) {
    OSArray *victims = OSArray::withCapacity(4);
    IORegistryIterator *it = IORegistryIterator::iterateOver(pci, gIOServicePlane, kIORegistryIterateRecursively);
    if (it) {
        IORegistryEntry *e;
        while ((e = it->getNextObject())) {
            if (!OSDynamicCast(IOFramebuffer, e)) continue;
            bool isXeFB = strcmp(e->getMetaClass()->getClassName(), "XeFB") == 0;
            if (isXeFB == victimIsXeFB) victims->setObject(e);
        }
        it->release();
    }
    for (unsigned i = 0; i < victims->getCount(); i++) {
        IOService *fb = OSDynamicCast(IOService, victims->getObject(i));
        IOLog("XeShim: terminating %s\n", fb->getMetaClass()->getClassName());
        fb->terminate(kIOServiceRequired | kIOServiceSynchronous);
    }
    victims->release();
}

IOReturn XeShimUserClient::sFBSwap(OSObject *t, void *, IOExternalMethodArguments *a) {
    XeShimUserClient *self = (XeShimUserClient *)t;
    IOPCIDevice *pci = self->shim->pci;
    if (a->scalarInput[0]) {
        pci->setProperty("xe-fb", kOSBooleanTrue);
        terminateFramebuffers(pci, false);
        IOLog("XeShim: xe-fb armed, rematching\n");
        pci->registerService();
    } else {
        terminateFramebuffers(pci, true);
        pci->removeProperty("xe-fb");
        IOLog("XeShim: xe-fb cleared, handing display back\n");
        pci->registerService();
    }
    return kIOReturnSuccess;
}

IOReturn XeShimUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                               IOMemoryDescriptor **memory) {
    IOMemoryDescriptor *md = nullptr;
    IOPCIDevice *pci = shim->pci;
    if (type == kXeShimMemBAR0 || type == kXeShimMemBAR2) {
        IODeviceMemory *dm = pci->getDeviceMemoryWithRegister(
            type == kXeShimMemBAR0 ? kIOPCIConfigBaseAddress0 : kIOPCIConfigBaseAddress2);
        if (!dm) return kIOReturnNotFound;
        dm->retain();
        md = dm;
        *options = (type == kXeShimMemBAR0 ? kIOMapInhibitCache : kIOMapWriteCombineCache);
    } else if (type >= kXeShimMemDMABase && type < kXeShimMemDMABase + kMaxDMA) {
        IOLockLock(lock);
        md = dma[type - kXeShimMemDMABase];
        if (md) md->retain();
        IOLockUnlock(lock);
        if (!md) return kIOReturnNotFound;
        *options = kIOMapWriteCombineCache;
    } else {
        return kIOReturnBadArgument;
    }
    *memory = md;  // the caller consumes this reference
    return kIOReturnSuccess;
}
