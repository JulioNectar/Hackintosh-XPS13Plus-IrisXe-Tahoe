// AudShimDrv.h: M6 "driver mode" interface of AudShim (interface v8, AudShim 0.8.0).
// Shared by the kext (AudShim.cpp), audctl (tools/audctl.c) and the HAL plug-in
// (tools/XeSpeakers/XeSpeakers.c). Plain C, no kernel or user-space includes.
//
// A driver connection is opened with IOServiceOpen(type = AUDSHIM_DRV_CONN_TYPE). It may be opened by
// root, an admin / console user, or _coreaudiod (uid 202, the AudioServerPlugIn host). Only one driver
// connection owns the hardware at a time; the owner is the connection that last opened/started it.
// Closing that connection (or the death of its process, e.g. `killall coreaudiod`) tears everything
// down in the kext (amps muted + PS3, DMA stopped, links and DSP off, M1 state).
//
// Methods (scalar in / scalar out):
//   AUDSHIM_METHOD_DRV  in: op, a0          out: 16 x uint64 (o[0] = kDrvErr*, see below)
//   AUDSHIM_METHOD_POS  in: -               out: 8 x uint64, lock-free, cheap (real-time IO thread)
// Memory (IOConnectMapMemory64, type AUDSHIM_MEM_RING): the 64 KiB looping playback ring, read-write,
// S32_LE stereo interleaved (24 valid bits are what reaches the amps), 8192 frames. Valid while the
// driver state is READY or RUNNING; it is a different buffer after every bring-up (map it again after
// each successful OPEN/START that reports a new bring-up count in DRV_STATE o[11]).
#ifndef AUDSHIM_DRV_H
#define AUDSHIM_DRV_H

#define AUDSHIM_IFACE_VERSION 8
#define AUDSHIM_DRV_CONN_TYPE 0x58454452u   /* 'XEDR' */
#define AUDSHIM_METHOD_DRV 5
#define AUDSHIM_METHOD_POS 6
#define AUDSHIM_MEM_RING 0
#define AUDSHIM_RING_BYTES 65536u
#define AUDSHIM_RING_FRAMES 8192u
#define AUDSHIM_FRAME_BYTES 8u
#define AUDSHIM_RATE 48000u

// driver ops (AUDSHIM_METHOD_DRV in[0])
enum {
    kDrvOpState = 0,   // read-only: o[1] state o[2] fail step o[3] fail code o[4] applied gain Q16 o[5] cap Q16
                       // o[6] cap dB o[7] starts o[8] DSP xrun notifications o[9] DSP notifications o[10] host DMA
                       // errors o[11] bring-ups o[12] last bring-up ms o[13] requested gain Q16 | muted << 32
                       // o[14] b0 suspended, b1 lingering (deferred stop pending) o[15] interface version
    kDrvOpOpen = 1,    // bring the DSP/links/amps/pipeline up to READY (blocking, ~1-2 s); no-op if already up
    kDrvOpStart = 2,   // READY -> RUNNING (brings up first if needed): amps on, SoundWire stream, DMA, TRIG_START
    kDrvOpStop = 3,    // RUNNING -> READY: fade, TRIG_STOP, DMA stop, amps muted + PS3, SoundWire stream off.
                       // Deferred (boot-arg audshimlingerms, default 5000): the PGA fades to 0 and the ring is zeroed at
                       // once, the real stop follows after the linger time unless a START resumes the stream first
    kDrvOpClose = 4,   // full teardown -> IDLE (M1 state) and release ownership
    kDrvOpVolume = 5,  // a0 = gain Q16 (0x10000 = 0 dB) | muted << 32; clamped by the kext cap
};
// driver states (DRV_STATE o[1], POS o[0])
enum { kDrvStIdle = 0, kDrvStBusy = 1, kDrvStReady = 2, kDrvStRunning = 3, kDrvStFailed = 4 };
// results (o[0] of DRV ops)
enum {
    kDrvOk = 0, kDrvErrBusy = 1, kDrvErrSuspended = 2, kDrvErrFailed = 3, kDrvErrBadOp = 4,
    kDrvErrNotReady = 5, kDrvErrRetryLater = 6,
};
// AUDSHIM_METHOD_POS out: o[0] state, o[1] DPIB (bytes read by the host DMA in the ring, 0..65535),
// o[2] mach_absolute_time() at the DPIB read (midpoint), o[3] run generation (+1 at every DMA start),
// o[4] SD CTL/STS, o[5] LPIB, o[6] DSP xrun notifications, o[7] mach_absolute_time() at DMA RUN

#endif
