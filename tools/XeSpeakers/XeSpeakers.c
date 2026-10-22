// XeSpeakers: AudioServerPlugIn (HAL driver) for the internal speakers of the Dell XPS 13 Plus 9320 on macOS
// (Intel cAVS 2.5 DSP + SoundWire + 2 x RT1316), driven by the AudShim kext's "driver mode" (AudShim 0.8.0,
// interface v8, see tools/AudShim/AudShimDrv.h). Milestone M6.
//
// One output device, "Alto-falantes internos": 48 kHz, 2 channels, Float32 for the HAL; the plug-in converts to
// S32_LE (the DSP pipeline takes S32; 24 valid bits reach the amps) and writes it straight into the kext's 64 KiB
// looping DMA ring (8192 frames), mapped read-write into this process with IOConnectMapMemory64.
//   - IO start/stop -> kext START/STOP (amps, SoundWire stream, host DMA, SOF trigger; ramps in the DSP PGA).
//   - Timing: GetZeroTimeStamp anchors one ring lap = 8192 frames. "Now" is the host DMA read position (DPIB),
//     read lock-free from the kext together with mach_absolute_time(); a first-order phase loop keeps the host-time
//     model on the DMA (the DSP clock is the 38.4 MHz crystal, not the host clock).
//   - Volume / mute controls -> SOF PGA gain (Q16) in the kext, which enforces a hard cap (default -6 dB).
//     Level persists through the host storage; first use starts at 30 %.
//   - After writing each IO cycle the next 2048 frames of the ring are zeroed, so a stalled IO thread produces a
//     short silence instead of a looping stale buffer.
// The kext brings the hardware up when this plug-in opens its driver connection (background thread at
// Initialize) and tears everything down when the connection closes (also when coreaudiod or this helper dies).
//
// 0.9.0 (M7, needs AudShim 0.9.0 = interface v9 for the microphones; with a v8 kext only the speakers appear, as before):
// a second, separate device "Microfone interno" (input only, 48 kHz, 2 ch, Float32) for the RT714 microphones. Its own
// IO start/stop -> kext CAP_START / CAP_STOP; the 64 KiB capture ring is mapped read-only (AUDSHIM_MEM_CRING); timing
// anchors on the capture DMA write position (method 7, same phase loop as the speakers); ReadInput converts S32 -> Float32
// with a software input gain (0..+30 dB, volume control in the input scope, default +12 dB, persisted). The speaker device,
// its object IDs and its behaviour are unchanged.
//
// Build: tools/XeSpeakers/build.sh (clang, no Xcode, ad-hoc signed). Install: /Library/Audio/Plug-Ins/HAL.
// Own code (no BlackHole source copied); the object model follows Apple's AudioServerPlugIn.h documentation.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <string.h>
#include <os/log.h>
#include "../AudShim/AudShimDrv.h"

#define XS_VERSION "0.9.0"
#define kDeviceUID "XeSpeakers_InternalSpeakers_UID"
#define kModelUID "XeSpeakers_RT1316x2_Model"
#define kDeviceName "Alto-falantes internos"
#define kManufacturer "Xe (AudShim/SOF)"
#define kMicUID "XeSpeakers_InternalMic_UID"
#define kMicModelUID "XeSpeakers_RT714_Model"
#define kMicName "Microfone interno"

enum {
    kObjPlugIn = kAudioObjectPlugInObject,   // 1
    kObjDevice = 2,
    kObjStream = 3,
    kObjVolume = 4,
    kObjMute = 5,
    kObjMicDevice = 6,     // 0.9.0: microphones (input only)
    kObjMicStream = 7,
    kObjMicVolume = 8,     // software input gain
};

static const Float64 kRate = 48000.0;
static const UInt32 kChannels = 2;
static const UInt32 kRingFrames = AUDSHIM_RING_FRAMES;      // 8192
static const UInt32 kSafetyOffset = 256;                    // frames ahead of the DMA read position (DPIB)
static const UInt32 kDeviceLatency = 144;                   // DSP host buffer (2 x 1 ms) + pipeline, approx.
static const UInt32 kClearAhead = 2048;                     // frames zeroed after each IO cycle (at most)
static const Float32 kMinDb = -64.0f;                       // TLV minimum of the SOF PGA ('Master Playback Volume')
static const Float32 kDefaultScalar = 0.30f;
static const UInt32 kMicSafetyOffset = 256;                 // frames behind the capture DMA write position (DPIB)
static const UInt32 kMicLatency = 96;                       // DSP capture pipeline (2 x 1 ms periods), approx.
static const Float32 kMicMaxDb = 30.0f;                     // software gain range 0..+30 dB (like the SOF capture PGA max)
static const Float32 kMicDefaultScalar = 0.40f;             // +12 dB

static os_log_t gLog;
static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;   // connection, ring mapping, IO state, controls

static io_connect_t gConn = IO_OBJECT_NULL;
static bool gDevicePublished = false;
static volatile int32_t *gRing = NULL;       // mapped ring (S32 stereo interleaved)
static mach_vm_address_t gRingAddr = 0;
static uint64_t gRingBringUp = 0;            // kext bring-up count the mapping belongs to
static UInt32 gIOCount = 0;
static Float32 gMaxDb = -6.0f;               // = -cap dB reported by the kext
static Float32 gScalar = kDefaultScalar;
static bool gMute = false;
// microphones (0.9.0)
static bool gMicPublished = false;           // kext interface >= 9
static volatile int32_t *gCRing = NULL;      // mapped capture ring (read-only)
static mach_vm_address_t gCRingAddr = 0;
static uint64_t gCRingGen = 0;               // kext capture ring generation the mapping belongs to
static UInt32 gMicIOCount = 0;
static Float32 gMicScalar = kMicDefaultScalar;
static volatile Float32 gMicGain = 3.98f;    // linear, from gMicScalar

// clock model (IO thread only, reset in StartIO)
static Float64 gTicksPerFrame = 0;           // nominal host ticks per frame
static Float64 gAnchorHost = 0;              // model: host time of unwrapped frame 0
static bool gAnchored = false;
static uint64_t gRunGen = 0;
static UInt64 gSeed = 1;
static Float64 gTicksPerSec = 1e9;
// the same model for the microphone device, anchored on the capture DMA (separate IO thread)
static Float64 gMicAnchorHost = 0;
static bool gMicAnchored = false;
static uint64_t gMicRunGen = 0;
static UInt64 gMicSeed = 1;

// ------------------------------------------------------------------ kext helpers
static kern_return_t kextDrv(uint64_t op, uint64_t a0, uint64_t o[16]) {
    uint64_t in[2] = {op, a0};
    uint32_t n = 16;
    memset(o, 0, 16 * sizeof o[0]);
    if (gConn == IO_OBJECT_NULL) return kIOReturnNotOpen;
    return IOConnectCallScalarMethod(gConn, AUDSHIM_METHOD_DRV, in, 2, o, &n);
}

static Float32 scalarToDb(Float32 s) {
    if (s <= 0.0f) return kMinDb;
    Float32 db = gMaxDb + 40.0f * log10f(s);      // amplitude ~ scalar^2: an audio taper
    return db < kMinDb ? kMinDb : db > gMaxDb ? gMaxDb : db;
}
static Float32 dbToScalar(Float32 db) {
    if (db <= kMinDb) return 0.0f;
    if (db >= gMaxDb) return 1.0f;
    return powf(10.0f, (db - gMaxDb) / 40.0f);
}

// gLock held
static void sendVolume(void) {
    uint64_t o[16];
    uint32_t q16 = 0;
    if (gScalar > 0.0f) {
        double g = pow(10.0, scalarToDb(gScalar) / 20.0) * 65536.0;
        q16 = g > 65536.0 ? 65536u : (uint32_t)g;
    }
    kern_return_t kr = kextDrv(kDrvOpVolume, (uint64_t)q16 | ((uint64_t)(gMute ? 1 : 0) << 32), o);
    if (kr || o[0]) os_log(gLog, "XeSpeakers: volume -> kext: 0x%x / %llu", kr, o[0]);
}

static Float32 micScalarToDb(Float32 s) { s = s < 0 ? 0 : s > 1 ? 1 : s; return kMicMaxDb * s; }
static Float32 micDbToScalar(Float32 db) { return db <= 0 ? 0.0f : db >= kMicMaxDb ? 1.0f : db / kMicMaxDb; }
static void micGainUpdate(void) { gMicGain = powf(10.0f, micScalarToDb(gMicScalar) / 20.0f); }

static void saveControls(void) {
    if (!gHost) return;
    CFNumberRef mg = CFNumberCreate(NULL, kCFNumberFloat32Type, &gMicScalar);
    if (mg) { gHost->WriteToStorage(gHost, CFSTR("micgain"), mg); CFRelease(mg); }
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberFloat32Type, &gScalar);
    if (v) { gHost->WriteToStorage(gHost, CFSTR("volume"), v); CFRelease(v); }
    SInt32 m = gMute ? 1 : 0;
    CFNumberRef mm = CFNumberCreate(NULL, kCFNumberSInt32Type, &m);
    if (mm) { gHost->WriteToStorage(gHost, CFSTR("mute"), mm); CFRelease(mm); }
}

static void loadControls(void) {
    CFPropertyListRef p = NULL;
    if (gHost && gHost->CopyFromStorage(gHost, CFSTR("volume"), &p) == 0 && p) {
        if (CFGetTypeID(p) == CFNumberGetTypeID()) {
            Float32 v; CFNumberGetValue((CFNumberRef)p, kCFNumberFloat32Type, &v);
            if (v >= 0.0f && v <= 1.0f) gScalar = v;
        }
        CFRelease(p);
    }
    p = NULL;
    if (gHost && gHost->CopyFromStorage(gHost, CFSTR("mute"), &p) == 0 && p) {
        if (CFGetTypeID(p) == CFNumberGetTypeID()) { SInt32 m; CFNumberGetValue((CFNumberRef)p, kCFNumberSInt32Type, &m); gMute = m != 0; }
        CFRelease(p);
    }
    p = NULL;
    if (gHost && gHost->CopyFromStorage(gHost, CFSTR("micgain"), &p) == 0 && p) {
        if (CFGetTypeID(p) == CFNumberGetTypeID()) {
            Float32 v; CFNumberGetValue((CFNumberRef)p, kCFNumberFloat32Type, &v);
            if (v >= 0.0f && v <= 1.0f) gMicScalar = v;
        }
        CFRelease(p);
    }
    micGainUpdate();
}

// gLock held: map the ring if the kext brought the hardware up again since the last mapping
static bool mapRing(uint64_t bringUps) {
    if (gRing && bringUps == gRingBringUp) return true;
    if (gRingAddr) { IOConnectUnmapMemory64(gConn, AUDSHIM_MEM_RING, mach_task_self(), gRingAddr); gRingAddr = 0; gRing = NULL; }
    mach_vm_address_t a = 0; mach_vm_size_t sz = 0;
    kern_return_t kr = IOConnectMapMemory64(gConn, AUDSHIM_MEM_RING, mach_task_self(), &a, &sz, kIOMapAnywhere);
    if (kr || sz < AUDSHIM_RING_BYTES) { os_log_error(gLog, "XeSpeakers: ring map failed 0x%x (size %llu)", kr, (unsigned long long)sz); return false; }
    gRingAddr = a; gRing = (volatile int32_t *)(uintptr_t)a; gRingBringUp = bringUps;
    return true;
}

// gLock held: map the capture ring if the kext allocated a new one since the last mapping
static bool mapCRing(uint64_t gen) {
    if (gCRing && gen == gCRingGen) return true;
    if (gCRingAddr) { IOConnectUnmapMemory64(gConn, AUDSHIM_MEM_CRING, mach_task_self(), gCRingAddr); gCRingAddr = 0; gCRing = NULL; }
    mach_vm_address_t a = 0; mach_vm_size_t sz = 0;
    kern_return_t kr = IOConnectMapMemory64(gConn, AUDSHIM_MEM_CRING, mach_task_self(), &a, &sz, kIOMapAnywhere | kIOMapReadOnly);
    if (kr || sz < AUDSHIM_RING_BYTES) { os_log_error(gLog, "XeSpeakers: capture ring map failed 0x%x (size %llu)", kr, (unsigned long long)sz); return false; }
    gCRingAddr = a; gCRing = (volatile int32_t *)(uintptr_t)a; gCRingGen = gen;
    return true;
}

static void *openThread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&gLock);
    uint64_t o[16];
    kern_return_t kr = kextDrv(kDrvOpOpen, 0, o);
    if (!kr && !o[0]) {
        if (o[5] && o[6] <= 20) gMaxDb = -(Float32)o[6];
        mapRing(o[11]);
        sendVolume();
        os_log(gLog, "XeSpeakers: hardware READY (bring-up %llu ms, cap -%llu dB)", o[12], o[6]);
    } else {
        os_log_error(gLog, "XeSpeakers: kext OPEN failed 0x%x / %llu (step %llu code 0x%llx); START will retry", kr, o[0], o[2], o[3]);
    }
    pthread_mutex_unlock(&gLock);
    return NULL;
}

// ------------------------------------------------------------------ IUnknown / factory
static HRESULT QueryInterface(void *drv, REFIID iid, LPVOID *out);
static ULONG AddRef(void *drv);
static ULONG Release(void *drv);
static OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host);
static OSStatus CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
static OSStatus DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus PerformConfigChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static OSStatus AbortConfigChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static Boolean HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
                                    const void *, UInt32 *);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
                                const void *, UInt32, UInt32 *, void *);
static OSStatus SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
                                const void *, UInt32, const void *);
static OSStatus StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
static OSStatus DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32,
                              const AudioServerPlugInIOCycleInfo *, void *, void *);
static OSStatus EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);

static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformConfigChange, AbortConfigChange, HasProperty, IsPropertySettable, GetPropertyDataSize, GetPropertyData,
    SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation,
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;
static _Atomic UInt32 gRefCount = 1;

__attribute__((visibility("default"))) void *XeSpeakers_Create(CFAllocatorRef alloc, CFUUIDRef type);
void *XeSpeakers_Create(CFAllocatorRef alloc, CFUUIDRef type) {
    (void)alloc;
    return CFEqual(type, kAudioServerPlugInTypeUUID) ? gDriverRef : NULL;
}

static HRESULT QueryInterface(void *drv, REFIID iid, LPVOID *out) {
    if (drv != gDriverRef || !out) return kAudioHardwareBadObjectError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (req) {
        if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
            atomic_fetch_add(&gRefCount, 1);
            *out = gDriverRef;
            r = S_OK;
        }
        CFRelease(req);
    }
    return r;
}
static ULONG AddRef(void *drv) { if (drv != gDriverRef) return 0; return atomic_fetch_add(&gRefCount, 1) + 1; }
static ULONG Release(void *drv) { if (drv != gDriverRef) return 0; UInt32 c = atomic_load(&gRefCount); if (c > 0) c = atomic_fetch_sub(&gRefCount, 1) - 1; return c; }

static OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host) {
    if (drv != gDriverRef) return kAudioHardwareBadObjectError;
    gLog = os_log_create("local.xe.XeSpeakers", "driver");
    gHost = host;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    gTicksPerSec = 1e9 * (Float64)tb.denom / (Float64)tb.numer;
    gTicksPerFrame = gTicksPerSec / kRate;
    loadControls();
    io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("AudShim"));
    if (!svc) { os_log_error(gLog, "XeSpeakers " XS_VERSION ": AudShim kext not loaded: no device"); return kAudioHardwareNoError; }
    CFTypeRef v = IORegistryEntryCreateCFProperty(svc, CFSTR("AudShimInterface"), NULL, 0);
    SInt32 iface = 0;
    if (v) { if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberSInt32Type, &iface); CFRelease(v); }
    if (iface < AUDSHIM_IFACE_MIN_SPEAKERS) {
        os_log_error(gLog, "XeSpeakers " XS_VERSION ": AudShim interface v%d, need v%d (AudShim 0.8.0): no device", (int)iface, AUDSHIM_IFACE_MIN_SPEAKERS);
        IOObjectRelease(svc);
        return kAudioHardwareNoError;
    }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), AUDSHIM_DRV_CONN_TYPE, &gConn);
    IOObjectRelease(svc);
    if (kr) { os_log_error(gLog, "XeSpeakers " XS_VERSION ": IOServiceOpen 0x%x (sandbox/privilege?): no device", kr); gConn = IO_OBJECT_NULL; return kAudioHardwareNoError; }
    gDevicePublished = true;
    gMicPublished = iface >= 9;   // the microphones need AudShim 0.9.0
    os_log(gLog, "XeSpeakers " XS_VERSION ": driver connection open (kext interface v%d, microphones %{public}s), bringing the "
           "hardware up in the background", (int)iface, gMicPublished ? "yes" : "no (needs v9)");
    pthread_t t;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &at, openThread, NULL);
    pthread_attr_destroy(&at);
    return kAudioHardwareNoError;
}

static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *out) {
    (void)d; (void)desc; (void)ci; (void)out; return kAudioHardwareUnsupportedOperationError;
}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID dev) { (void)d; (void)dev; return kAudioHardwareUnsupportedOperationError; }
static bool isDev(AudioObjectID dev) { return dev == kObjDevice || (gMicPublished && dev == kObjMicDevice); }
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci) {
    (void)ci; return (d == gDriverRef && isDev(dev)) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci) {
    (void)ci; return (d == gDriverRef && isDev(dev)) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}
static OSStatus PerformConfigChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 a, void *b) { (void)d; (void)dev; (void)a; (void)b; return kAudioHardwareNoError; }
static OSStatus AbortConfigChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 a, void *b) { (void)d; (void)dev; (void)a; (void)b; return kAudioHardwareNoError; }

// ------------------------------------------------------------------ properties
static bool objectExists(AudioObjectID o) {
    if (o == kObjPlugIn) return true;
    if (gMicPublished && (o == kObjMicDevice || o == kObjMicStream || o == kObjMicVolume)) return true;
    return gDevicePublished && (o == kObjDevice || o == kObjStream || o == kObjVolume || o == kObjMute);
}

static AudioStreamBasicDescription streamFormat(void) {
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * kChannels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * kChannels;
    f.mChannelsPerFrame = kChannels;
    f.mBitsPerChannel = 32;
    return f;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a) {
    (void)pid;
    if (d != gDriverRef || !a || !objectExists(obj)) return false;
    switch (obj) {
    case kObjPlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice: case kAudioPlugInPropertyResourceBundle:
        case kAudioPlugInPropertyBoxList: case kAudioPlugInPropertyTranslateUIDToBox:
        case kAudioPlugInPropertyClockDeviceList: case kAudioPlugInPropertyTranslateUIDToClockDevice:
            return true;
        }
        return false;
    case kObjDevice:
    case kObjMicDevice:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices: case kAudioDevicePropertyClockDomain: case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning: case kAudioObjectPropertyControlList:
        case kAudioDevicePropertyNominalSampleRate: case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden: case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyClockIsStable:
        case kAudioDevicePropertyStreams:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency: case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
            // every scope: the HAL queries these for input and output while adding a device; an "unknown property"
            // answer there aborts the add ("Caught exception trying to add device"). Input scope reports 0 / none.
            return true;
        }
        return false;
    case kObjStream:
    case kObjMicStream:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        }
        return false;
    case kObjVolume:
    case kObjMicVolume:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioControlPropertyScope: case kAudioControlPropertyElement:
        case kAudioLevelControlPropertyScalarValue: case kAudioLevelControlPropertyDecibelValue:
        case kAudioLevelControlPropertyDecibelRange: case kAudioLevelControlPropertyConvertScalarToDecibels:
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            return true;
        }
        return false;
    case kObjMute:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioControlPropertyScope: case kAudioControlPropertyElement:
        case kAudioBooleanControlPropertyValue:
            return true;
        }
        return false;
    }
    return false;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    if (!out) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    *out = false;
    if ((obj == kObjDevice || obj == kObjMicDevice) && a->mSelector == kAudioDevicePropertyNominalSampleRate) *out = true;   // only 48 kHz accepted
    if ((obj == kObjStream || obj == kObjMicStream) &&
        (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat ||
         a->mSelector == kAudioStreamPropertyIsActive)) *out = true;
    if ((obj == kObjVolume || obj == kObjMicVolume) &&
        (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue)) *out = true;
    if (obj == kObjMute && a->mSelector == kAudioBooleanControlPropertyValue) *out = true;
    return kAudioHardwareNoError;
}

// Fills (or sizes, when out == NULL) the property. Returns the byte count or < 0 for "unknown".
static OSStatus getProp(AudioObjectID obj, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata,
                        UInt32 inSize, UInt32 *outSize, void *out) {
#define RET(T, v) do { if (out) { if (inSize < sizeof(T)) return kAudioHardwareBadPropertySizeError; *(T *)out = (v); } *outSize = sizeof(T); return 0; } while (0)
#define RETCF(s) do { if (out) { if (inSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError; *(CFStringRef *)out = CFSTR(s); } *outSize = sizeof(CFStringRef); return 0; } while (0)
    AudioObjectID list[4];
    UInt32 n = 0;
    switch (obj) {
    case kObjPlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RET(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: RET(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: RETCF(kManufacturer);
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            if (gDevicePublished) list[n++] = kObjDevice;
            if (gMicPublished) list[n++] = kObjMicDevice;
            goto retlist;
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            AudioObjectID r = kAudioObjectUnknown;
            if (qsize == sizeof(CFStringRef) && qdata && gDevicePublished &&
                CFStringCompare(*(CFStringRef *)qdata, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo) r = kObjDevice;
            if (qsize == sizeof(CFStringRef) && qdata && gMicPublished &&
                CFStringCompare(*(CFStringRef *)qdata, CFSTR(kMicUID), 0) == kCFCompareEqualTo) r = kObjMicDevice;
            RET(AudioObjectID, r);
        }
        case kAudioPlugInPropertyResourceBundle: RETCF("");
        case kAudioPlugInPropertyBoxList: case kAudioPlugInPropertyClockDeviceList: goto retlist;   // none
        case kAudioPlugInPropertyTranslateUIDToBox: case kAudioPlugInPropertyTranslateUIDToClockDevice:
            RET(AudioObjectID, kAudioObjectUnknown);
        }
        break;
    case kObjDevice:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RET(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: RET(AudioObjectID, kObjPlugIn);
        case kAudioObjectPropertyName: RETCF(kDeviceName);
        case kAudioObjectPropertyManufacturer: RETCF(kManufacturer);
        case kAudioDevicePropertyDeviceUID: RETCF(kDeviceUID);
        case kAudioDevicePropertyModelUID: RETCF(kModelUID);
        case kAudioDevicePropertyTransportType: RET(UInt32, kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyClockDomain: RET(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: RET(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: RET(UInt32, gIOCount > 0 ? 1 : 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: RET(UInt32, a->mScope == kAudioObjectPropertyScopeInput ? 0 : 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: RET(UInt32, a->mScope == kAudioObjectPropertyScopeInput ? 0 : 1);
        case kAudioDevicePropertyLatency: RET(UInt32, a->mScope == kAudioObjectPropertyScopeInput ? 0 : kDeviceLatency);
        case kAudioDevicePropertySafetyOffset: RET(UInt32, a->mScope == kAudioObjectPropertyScopeInput ? 0 : kSafetyOffset);
        case kAudioDevicePropertyNominalSampleRate: RET(Float64, kRate);
        case kAudioDevicePropertyIsHidden: RET(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: RET(UInt32, kRingFrames);
        case kAudioDevicePropertyClockIsStable: RET(UInt32, 1);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange r = {kRate, kRate};
            RET(AudioValueRange, r);
        }
        case kAudioDevicePropertyRelatedDevices: list[n++] = kObjDevice; goto retlist;
        case kAudioObjectPropertyOwnedObjects:
            if (a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeOutput) {
                list[n++] = kObjStream; list[n++] = kObjVolume; list[n++] = kObjMute;
            }
            goto retlist;
        case kAudioDevicePropertyStreams:
            if (a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeOutput) list[n++] = kObjStream;
            goto retlist;
        case kAudioObjectPropertyControlList: list[n++] = kObjVolume; list[n++] = kObjMute; goto retlist;
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            UInt32 ch[2] = {1, 2};
            if (out) { if (inSize < sizeof ch) return kAudioHardwareBadPropertySizeError; memcpy(out, ch, sizeof ch); }
            *outSize = sizeof ch; return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 sz = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription));
            if (out) {
                if (inSize < sz) return kAudioHardwareBadPropertySizeError;
                AudioChannelLayout *l = (AudioChannelLayout *)out;
                memset(l, 0, sz);
                l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
                l->mNumberChannelDescriptions = kChannels;
                l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
                l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            }
            *outSize = sz; return 0;
        }
        }
        break;
    case kObjMicDevice: {
        // the microphone device: everything lives in the INPUT scope (the output scope reports 0 / none, the mirror
        // image of the speaker device, so that the HAL's all-scope queries while adding the device succeed)
        bool in = a->mScope == kAudioObjectPropertyScopeInput, glob = a->mScope == kAudioObjectPropertyScopeGlobal;
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RET(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: RET(AudioObjectID, kObjPlugIn);
        case kAudioObjectPropertyName: RETCF(kMicName);
        case kAudioObjectPropertyManufacturer: RETCF(kManufacturer);
        case kAudioDevicePropertyDeviceUID: RETCF(kMicUID);
        case kAudioDevicePropertyModelUID: RETCF(kMicModelUID);
        case kAudioDevicePropertyTransportType: RET(UInt32, kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyClockDomain: RET(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: RET(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: RET(UInt32, gMicIOCount > 0 ? 1 : 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: RET(UInt32, a->mScope == kAudioObjectPropertyScopeOutput ? 0 : 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: RET(UInt32, 0);
        case kAudioDevicePropertyLatency: RET(UInt32, in ? kMicLatency : 0);
        case kAudioDevicePropertySafetyOffset: RET(UInt32, in ? kMicSafetyOffset : 0);
        case kAudioDevicePropertyNominalSampleRate: RET(Float64, kRate);
        case kAudioDevicePropertyIsHidden: RET(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: RET(UInt32, kRingFrames);
        case kAudioDevicePropertyClockIsStable: RET(UInt32, 1);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange r = {kRate, kRate};
            RET(AudioValueRange, r);
        }
        case kAudioDevicePropertyRelatedDevices: list[n++] = kObjMicDevice; goto retlist;
        case kAudioObjectPropertyOwnedObjects:
            if (glob || in) { list[n++] = kObjMicStream; list[n++] = kObjMicVolume; }
            goto retlist;
        case kAudioDevicePropertyStreams:
            if (glob || in) list[n++] = kObjMicStream;
            goto retlist;
        case kAudioObjectPropertyControlList: list[n++] = kObjMicVolume; goto retlist;
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            UInt32 ch[2] = {1, 2};
            if (out) { if (inSize < sizeof ch) return kAudioHardwareBadPropertySizeError; memcpy(out, ch, sizeof ch); }
            *outSize = sizeof ch; return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 sz = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription));
            if (out) {
                if (inSize < sz) return kAudioHardwareBadPropertySizeError;
                AudioChannelLayout *l = (AudioChannelLayout *)out;
                memset(l, 0, sz);
                l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
                l->mNumberChannelDescriptions = kChannels;
                l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
                l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            }
            *outSize = sz; return 0;
        }
        }
        break;
    }
    case kObjMicStream:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RET(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: RET(AudioObjectID, kObjMicDevice);
        case kAudioObjectPropertyOwnedObjects: goto retlist;
        case kAudioStreamPropertyIsActive: RET(UInt32, 1);
        case kAudioStreamPropertyDirection: RET(UInt32, 1);   // input
        case kAudioStreamPropertyTerminalType: RET(UInt32, kAudioStreamTerminalTypeMicrophone);
        case kAudioStreamPropertyStartingChannel: RET(UInt32, 1);
        case kAudioStreamPropertyLatency: RET(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: RET(AudioStreamBasicDescription, streamFormat());
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r;
            r.mFormat = streamFormat(); r.mSampleRateRange.mMinimum = kRate; r.mSampleRateRange.mMaximum = kRate;
            RET(AudioStreamRangedDescription, r);
        }
        }
        break;
    case kObjMicVolume:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: RET(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: RET(AudioObjectID, kObjMicDevice);
        case kAudioObjectPropertyOwnedObjects: goto retlist;
        case kAudioControlPropertyScope: RET(AudioObjectPropertyScope, kAudioObjectPropertyScopeInput);
        case kAudioControlPropertyElement: RET(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: RET(Float32, gMicScalar);
        case kAudioLevelControlPropertyDecibelValue: RET(Float32, micScalarToDb(gMicScalar));
        case kAudioLevelControlPropertyDecibelRange: {
            AudioValueRange r = {0.0, kMicMaxDb};
            RET(AudioValueRange, r);
        }
        case kAudioLevelControlPropertyConvertScalarToDecibels: {
            Float32 v = (out && inSize >= sizeof(Float32)) ? *(Float32 *)out : 0.0f;
            RET(Float32, micScalarToDb(v));
        }
        case kAudioLevelControlPropertyConvertDecibelsToScalar: {
            Float32 v = (out && inSize >= sizeof(Float32)) ? *(Float32 *)out : 0.0f;
            RET(Float32, micDbToScalar(v));
        }
        }
        break;
    case kObjStream:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: RET(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: RET(AudioObjectID, kObjDevice);
        case kAudioObjectPropertyOwnedObjects: goto retlist;
        case kAudioStreamPropertyIsActive: RET(UInt32, 1);
        case kAudioStreamPropertyDirection: RET(UInt32, 0);   // output
        case kAudioStreamPropertyTerminalType: RET(UInt32, kAudioStreamTerminalTypeSpeaker);
        case kAudioStreamPropertyStartingChannel: RET(UInt32, 1);
        case kAudioStreamPropertyLatency: RET(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: RET(AudioStreamBasicDescription, streamFormat());
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r;
            r.mFormat = streamFormat(); r.mSampleRateRange.mMinimum = kRate; r.mSampleRateRange.mMaximum = kRate;
            RET(AudioStreamRangedDescription, r);
        }
        }
        break;
    case kObjVolume:
    case kObjMute:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: RET(AudioClassID, obj == kObjVolume ? kAudioLevelControlClassID : kAudioBooleanControlClassID);
        case kAudioObjectPropertyClass: RET(AudioClassID, obj == kObjVolume ? kAudioVolumeControlClassID : kAudioMuteControlClassID);
        case kAudioObjectPropertyOwner: RET(AudioObjectID, kObjDevice);
        case kAudioObjectPropertyOwnedObjects: goto retlist;
        case kAudioControlPropertyScope: RET(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: RET(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioBooleanControlPropertyValue: if (obj != kObjMute) break; RET(UInt32, gMute ? 1 : 0);
        case kAudioLevelControlPropertyScalarValue: if (obj != kObjVolume) break; RET(Float32, gScalar);
        case kAudioLevelControlPropertyDecibelValue: if (obj != kObjVolume) break; RET(Float32, scalarToDb(gScalar));
        case kAudioLevelControlPropertyDecibelRange: {
            if (obj != kObjVolume) break;
            AudioValueRange r = {kMinDb, gMaxDb};
            RET(AudioValueRange, r);
        }
        case kAudioLevelControlPropertyConvertScalarToDecibels: {
            if (obj != kObjVolume) break;
            Float32 v = (out && inSize >= sizeof(Float32)) ? *(Float32 *)out : 0.0f;
            v = v < 0 ? 0 : v > 1 ? 1 : v;
            RET(Float32, scalarToDb(v));
        }
        case kAudioLevelControlPropertyConvertDecibelsToScalar: {
            if (obj != kObjVolume) break;
            Float32 v = (out && inSize >= sizeof(Float32)) ? *(Float32 *)out : kMinDb;
            RET(Float32, dbToScalar(v));
        }
        }
        break;
    }
    return kAudioHardwareUnknownPropertyError;
retlist: {
        UInt32 bytes = n * (UInt32)sizeof(AudioObjectID);
        if (out) {
            UInt32 fit = inSize / (UInt32)sizeof(AudioObjectID);
            if (fit < n) n = fit;
            memcpy(out, list, n * sizeof(AudioObjectID));
            bytes = n * (UInt32)sizeof(AudioObjectID);
        }
        *outSize = bytes;
        return 0;
    }
#undef RET
#undef RETCF
}

static void logUnknown(const char *what, AudioObjectID obj, const AudioObjectPropertyAddress *a) {
    static _Atomic int n = 0;
    if (!a || atomic_fetch_add(&n, 1) >= 200) return;
    UInt32 s = a->mSelector, sc = a->mScope;
    os_log(gLog, "XeSpeakers: %{public}s: unknown property obj %u '%c%c%c%c' scope '%c%c%c%c' element %u", what, obj,
           (char)(s >> 24), (char)(s >> 16), (char)(s >> 8), (char)s, (char)(sc >> 24), (char)(sc >> 16), (char)(sc >> 8), (char)sc,
           a->mElement);
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qsize, const void *qdata, UInt32 *outSize) {
    if (!outSize) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(d, obj, pid, a)) { logUnknown("GetPropertyDataSize", obj, a); return kAudioHardwareUnknownPropertyError; }
    return getProp(obj, a, qsize, qdata, 0, outSize, NULL);
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qsize, const void *qdata, UInt32 inSize, UInt32 *outSize, void *out) {
    if (!outSize || !out) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(d, obj, pid, a)) { logUnknown("GetPropertyData", obj, a); return kAudioHardwareUnknownPropertyError; }
    return getProp(obj, a, qsize, qdata, inSize, outSize, out);
}

static void notify(AudioObjectID obj, AudioObjectPropertySelector sel1, AudioObjectPropertySelector sel2) {
    AudioObjectPropertyAddress a[2] = {
        {sel1, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        {sel2, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
    };
    if (gHost) gHost->PropertiesChanged(gHost, obj, sel2 ? 2 : 1, a);
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qsize, const void *qdata, UInt32 inSize, const void *data) {
    (void)qsize; (void)qdata;
    if (!data) return kAudioHardwareIllegalOperationError;
    if (!HasProperty(d, obj, pid, a)) return kAudioHardwareUnknownPropertyError;
    if ((obj == kObjDevice || obj == kObjMicDevice) && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return *(const Float64 *)data == kRate ? kAudioHardwareNoError : kAudioDeviceUnsupportedFormatError;
    }
    if ((obj == kObjStream || obj == kObjMicStream) &&
        (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (inSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription *f = data;
        AudioStreamBasicDescription w = streamFormat();
        return (f->mSampleRate == w.mSampleRate && f->mFormatID == w.mFormatID && f->mChannelsPerFrame == w.mChannelsPerFrame &&
                f->mBitsPerChannel == w.mBitsPerChannel && (f->mFormatFlags & kAudioFormatFlagIsFloat))
                   ? kAudioHardwareNoError : kAudioDeviceUnsupportedFormatError;
    }
    if ((obj == kObjStream || obj == kObjMicStream) && a->mSelector == kAudioStreamPropertyIsActive) return kAudioHardwareNoError;
    if (obj == kObjMicVolume && (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue)) {
        if (inSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
        Float32 v = *(const Float32 *)data;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue) v = micDbToScalar(v);
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        pthread_mutex_lock(&gLock);
        bool changed = v != gMicScalar;
        gMicScalar = v;
        if (changed) { micGainUpdate(); saveControls(); }
        pthread_mutex_unlock(&gLock);
        if (changed) notify(kObjMicVolume, kAudioLevelControlPropertyScalarValue, kAudioLevelControlPropertyDecibelValue);
        return kAudioHardwareNoError;
    }
    if (obj == kObjVolume && (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue)) {
        if (inSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
        Float32 v = *(const Float32 *)data;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue) v = dbToScalar(v);
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        pthread_mutex_lock(&gLock);
        bool changed = v != gScalar;
        gScalar = v;
        if (changed) { sendVolume(); saveControls(); }
        pthread_mutex_unlock(&gLock);
        if (changed) notify(kObjVolume, kAudioLevelControlPropertyScalarValue, kAudioLevelControlPropertyDecibelValue);
        return kAudioHardwareNoError;
    }
    if (obj == kObjMute && a->mSelector == kAudioBooleanControlPropertyValue) {
        if (inSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        bool m = *(const UInt32 *)data != 0;
        pthread_mutex_lock(&gLock);
        bool changed = m != gMute;
        gMute = m;
        if (changed) { sendVolume(); saveControls(); }
        pthread_mutex_unlock(&gLock);
        if (changed) notify(kObjMute, kAudioBooleanControlPropertyValue, 0);
        return kAudioHardwareNoError;
    }
    return kAudioHardwareUnknownPropertyError;
}

// ------------------------------------------------------------------ IO
static OSStatus micStartIO(void);
static OSStatus micStopIO(void);
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client) {
    (void)client;
    if (d == gDriverRef && gMicPublished && dev == kObjMicDevice) return micStartIO();
    if (d != gDriverRef || dev != kObjDevice || !gDevicePublished) return kAudioHardwareBadObjectError;
    OSStatus r = kAudioHardwareNoError;
    pthread_mutex_lock(&gLock);
    if (gIOCount == 0) {
        uint64_t o[16];
        kern_return_t kr = kextDrv(kDrvOpStart, 0, o);
        if (kr || o[0] || o[1] != kDrvStRunning) {
            os_log_error(gLog, "XeSpeakers: START failed 0x%x / %llu (state %llu, step %llu code 0x%llx)", kr, o[0], o[1], o[2], o[3]);
            r = kAudioHardwareNotRunningError;
        } else if (!mapRing(o[11])) {
            kextDrv(kDrvOpStop, 0, o);
            r = kAudioHardwareNotRunningError;
        } else {
            if (o[5] && o[6] <= 20) gMaxDb = -(Float32)o[6];
            // (no volume IPC here: START already ramped the PGA to the last value sent with sendVolume)
            gAnchored = false;
            gSeed++;
        }
    }
    if (r == kAudioHardwareNoError) gIOCount++;
    pthread_mutex_unlock(&gLock);
    return r;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client) {
    (void)client;
    if (d == gDriverRef && gMicPublished && dev == kObjMicDevice) return micStopIO();
    if (d != gDriverRef || dev != kObjDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    if (gIOCount > 0 && --gIOCount == 0) {
        uint64_t o[16];
        kern_return_t kr = kextDrv(kDrvOpStop, 0, o);
        if (kr || o[0]) os_log_error(gLog, "XeSpeakers: STOP 0x%x / %llu", kr, o[0]);
    }
    pthread_mutex_unlock(&gLock);
    return kAudioHardwareNoError;
}

static bool kextPos(uint64_t p[8]) {
    uint32_t n = 8;
    return IOConnectCallScalarMethod(gConn, AUDSHIM_METHOD_POS, NULL, 0, p, &n) == KERN_SUCCESS;
}
#ifndef XS_POS
#define XS_POS kextPos   // the offline self-test substitutes a simulated DMA
#endif
#ifndef XS_NOW
#define XS_NOW mach_absolute_time   // ... and a simulated host clock
#endif

// DMA position -> host-time model (IO thread). DPIB is where the host DMA has read up to in the ring.
static void clockUpdate(void) {
    uint64_t p[8];
    if (!XS_POS(p) || p[0] != kDrvStRunning) return;
    Float64 t = (Float64)p[2];
    Float64 inRing = (Float64)((uint32_t)p[1] % AUDSHIM_RING_BYTES / AUDSHIM_FRAME_BYTES);
    if (!gAnchored || p[3] != gRunGen) {
        gAnchorHost = t - inRing * gTicksPerFrame;
        gRunGen = p[3];
        gAnchored = true;
        return;
    }
    Float64 pred = (t - gAnchorHost) / gTicksPerFrame;
    Float64 F = floor(pred / kRingFrames) * kRingFrames + inRing;
    if (F - pred > kRingFrames / 2) F -= kRingFrames;
    else if (pred - F > kRingFrames / 2) F += kRingFrames;
    Float64 err = (t - gAnchorHost) - F * gTicksPerFrame;   // > 0: the DMA is later than the model
    if (fabs(err) > 0.02 * gTicksPerSec) {                  // > 20 ms off: re-anchor (discontinuity)
        gAnchorHost = t - F * gTicksPerFrame;
        gSeed++;
        os_log(gLog, "XeSpeakers: clock re-anchored (%.1f ms off)", err / gTicksPerSec * 1e3);
    } else {
        gAnchorHost += err * 0.05;
    }
}

static OSStatus micZeroTimeStamp(Float64 *st, UInt64 *ht, UInt64 *seed);
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, Float64 *st, UInt64 *ht, UInt64 *seed) {
    (void)client;
    if (d == gDriverRef && gMicPublished && dev == kObjMicDevice && st && ht && seed) return micZeroTimeStamp(st, ht, seed);
    if (d != gDriverRef || dev != kObjDevice || !st || !ht || !seed) return kAudioHardwareBadObjectError;
    clockUpdate();
    if (!gAnchored) { gAnchorHost = (Float64)XS_NOW(); gAnchored = true; gRunGen = (uint64_t)-1; }
    Float64 now = (Float64)XS_NOW();
    Float64 cur = (now - gAnchorHost) / gTicksPerFrame;
    Float64 laps = cur > 0 ? floor(cur / kRingFrames) : 0;
    *st = laps * kRingFrames;
    *ht = (UInt64)(gAnchorHost + laps * kRingFrames * gTicksPerFrame);
    *seed = gSeed;
    return kAudioHardwareNoError;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace) {
    (void)client;
    if (d != gDriverRef || !isDev(dev)) return kAudioHardwareBadObjectError;
    if (will) *will = (dev == kObjMicDevice) ? (op == kAudioServerPlugInIOOperationReadInput) : (op == kAudioServerPlugInIOOperationWriteMix);
    if (inPlace) *inPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 c, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *ci) {
    (void)c; (void)op; (void)n; (void)ci;
    return (d == gDriverRef && isDev(dev)) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 c, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *ci) {
    (void)c; (void)op; (void)n; (void)ci;
    return (d == gDriverRef && isDev(dev)) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

static inline int32_t toS32(Float32 x) {
    if (!(x == x)) return 0;   // NaN
    if (x >= 1.0f) return INT32_MAX;
    if (x <= -1.0f) return -INT32_MAX;
    return (int32_t)((double)x * 2147483647.0);
}

static OSStatus micReadInput(UInt32 frames, const AudioServerPlugInIOCycleInfo *ci, void *main);
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client, UInt32 op,
                              UInt32 frames, const AudioServerPlugInIOCycleInfo *ci, void *main, void *secondary) {
    (void)client; (void)secondary;
    if (d == gDriverRef && gMicPublished && dev == kObjMicDevice && stream == kObjMicStream)
        return op == kAudioServerPlugInIOOperationReadInput ? micReadInput(frames, ci, main) : kAudioHardwareNoError;
    if (d != gDriverRef || dev != kObjDevice || stream != kObjStream) return kAudioHardwareBadObjectError;
    if (op != kAudioServerPlugInIOOperationWriteMix || !main || !ci) return kAudioHardwareNoError;
    volatile int32_t *ring = gRing;
    if (!ring || frames > kRingFrames / 2) return kAudioHardwareNoError;
    const Float32 *src = (const Float32 *)main;
    Float64 t = ci->mOutputTime.mSampleTime;
    if (t < 0) return kAudioHardwareNoError;
    uint64_t pos = (uint64_t)t % kRingFrames;
    for (UInt32 i = 0; i < frames; i++) {
        volatile int32_t *f = ring + 2 * ((pos + i) % kRingFrames);
        f[0] = toS32(src[2 * i]);
        f[1] = toS32(src[2 * i + 1]);
    }
    // stale-data guard (see header). Written-but-unplayed data spans at most ~2 IO buffers + the safety offset
    // behind the end of this write; the cleared span must stay clear of it (it wraps around the ring).
    int64_t room = (int64_t)kRingFrames - 2 * (int64_t)frames - (int64_t)kSafetyOffset - 1024;
    UInt32 clr = room <= 0 ? 0 : room > (int64_t)kClearAhead ? kClearAhead : (UInt32)room;
    for (UInt32 i = 0; i < clr; i++) {
        volatile int32_t *f = ring + 2 * ((pos + frames + i) % kRingFrames);
        f[0] = 0; f[1] = 0;
    }
    return kAudioHardwareNoError;
}

// ------------------------------------------------------------------ microphones (0.9.0)
static OSStatus micStartIO(void) {
    OSStatus r = kAudioHardwareNoError;
    pthread_mutex_lock(&gLock);
    if (gMicIOCount == 0) {
        uint64_t o[16];
        kern_return_t kr = kextDrv(kDrvOpCapStart, 0, o);
        if (kr || o[0] || o[2] != kCapStRunning) {
            os_log_error(gLog, "XeSpeakers: CAP_START failed 0x%x / %llu (capture state %llu, step %llu code 0x%llx, RT714 dev %llu)",
                         kr, o[0], o[2], o[3], o[4], o[7]);
            r = kAudioHardwareNotRunningError;
        } else if (!mapCRing(o[8])) {
            kextDrv(kDrvOpCapStop, 0, o);
            r = kAudioHardwareNotRunningError;
        } else {
            gMicAnchored = false;
            gMicSeed++;
            os_log(gLog, "XeSpeakers: microphones running (capture start #%llu, gain +%.1f dB)", o[5], micScalarToDb(gMicScalar));
        }
    }
    if (r == kAudioHardwareNoError) gMicIOCount++;
    pthread_mutex_unlock(&gLock);
    return r;
}

static OSStatus micStopIO(void) {
    pthread_mutex_lock(&gLock);
    if (gMicIOCount > 0 && --gMicIOCount == 0) {
        uint64_t o[16];
        kern_return_t kr = kextDrv(kDrvOpCapStop, 0, o);
        if (kr || o[0]) os_log_error(gLog, "XeSpeakers: CAP_STOP 0x%x / %llu", kr, o[0]);
    }
    pthread_mutex_unlock(&gLock);
    return kAudioHardwareNoError;
}

static bool kextCPos(uint64_t p[8]) {
    uint32_t n = 8;
    return IOConnectCallScalarMethod(gConn, AUDSHIM_METHOD_CPOS, NULL, 0, p, &n) == KERN_SUCCESS;
}
#ifndef XS_CPOS
#define XS_CPOS kextCPos   // the offline self-test substitutes a simulated capture DMA
#endif

// capture DMA position -> host-time model (IO thread). DPIB is where the host DMA has written up to in the ring: the
// frames before it are valid, so "now" in the model is the write position; the HAL reads mInputTime <= now - safety offset.
static void micClockUpdate(void) {
    uint64_t p[8];
    if (!XS_CPOS(p) || p[0] != 1) return;
    Float64 t = (Float64)p[2];
    Float64 inRing = (Float64)((uint32_t)p[1] % AUDSHIM_RING_BYTES / AUDSHIM_FRAME_BYTES);
    if (!gMicAnchored || p[3] != gMicRunGen) {
        gMicAnchorHost = t - inRing * gTicksPerFrame;
        gMicRunGen = p[3];
        gMicAnchored = true;
        return;
    }
    Float64 pred = (t - gMicAnchorHost) / gTicksPerFrame;
    Float64 F = floor(pred / kRingFrames) * kRingFrames + inRing;
    if (F - pred > kRingFrames / 2) F -= kRingFrames;
    else if (pred - F > kRingFrames / 2) F += kRingFrames;
    Float64 err = (t - gMicAnchorHost) - F * gTicksPerFrame;
    if (fabs(err) > 0.02 * gTicksPerSec) {
        gMicAnchorHost = t - F * gTicksPerFrame;
        gMicSeed++;
        os_log(gLog, "XeSpeakers: microphone clock re-anchored (%.1f ms off)", err / gTicksPerSec * 1e3);
    } else {
        gMicAnchorHost += err * 0.05;
    }
}

static OSStatus micZeroTimeStamp(Float64 *st, UInt64 *ht, UInt64 *seed) {
    micClockUpdate();
    if (!gMicAnchored) { gMicAnchorHost = (Float64)XS_NOW(); gMicAnchored = true; gMicRunGen = (uint64_t)-1; }
    Float64 now = (Float64)XS_NOW();
    Float64 cur = (now - gMicAnchorHost) / gTicksPerFrame;
    Float64 laps = cur > 0 ? floor(cur / kRingFrames) : 0;
    *st = laps * kRingFrames;
    *ht = (UInt64)(gMicAnchorHost + laps * kRingFrames * gTicksPerFrame);
    *seed = gMicSeed;
    return kAudioHardwareNoError;
}

// ReadInput: frames [mInputTime, mInputTime + frames) from the capture ring, S32 -> Float32 with the software gain.
static OSStatus micReadInput(UInt32 frames, const AudioServerPlugInIOCycleInfo *ci, void *main) {
    if (!main || !ci) return kAudioHardwareNoError;
    Float32 *dst = (Float32 *)main;
    volatile int32_t *ring = gCRing;
    Float64 t = ci->mInputTime.mSampleTime;
    if (!ring || frames > kRingFrames / 2 || t < 0) { memset(dst, 0, (size_t)frames * kChannels * sizeof(Float32)); return kAudioHardwareNoError; }
    const Float32 g = gMicGain * (Float32)(1.0 / 2147483648.0);
    uint64_t pos = (uint64_t)t % kRingFrames;
    for (UInt32 i = 0; i < frames; i++) {
        volatile int32_t *f = ring + 2 * ((pos + i) % kRingFrames);
        Float32 l = (Float32)f[0] * g, r = (Float32)f[1] * g;
        dst[2 * i] = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l;
        dst[2 * i + 1] = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
    }
    return kAudioHardwareNoError;
}
