//! Copyright © 2022-2023 ChefKiss Inc. Licensed under the Thou Shalt Not Profit License version 1.5.
//! See LICENSE for details.

#include "DYLDPatches.hpp"
#include "kern_green.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <IOKit/IODeviceTreeSupport.h>

DYLDPatches *DYLDPatches::callback = nullptr;
void DYLDPatches::init() { callback = this; }

static bool shouldForceFullMetalPath() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenfullmtldyld", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}
	if (checkKernelArgument("-ngreenfullmtldyld")) {
		return true;
	}
	// Legacy: unified arg still accepted as fallback.
	if (PE_parse_boot_argn("ngreenfullmtl", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}
	return checkKernelArgument("-ngreenfullmtl");
}

void DYLDPatches::processPatcher(KernelPatcher &patcher) {

    auto *entry = IORegistryEntry::fromPath("/", gIODTPlane);
    if (entry) {
        DBGLOG("DYLD", "Setting hwgva-id to iMacPro1,1");
        entry->setProperty("hwgva-id", const_cast<char *>(kHwGvaId), arrsize(kHwGvaId));
        OSSafeReleaseNULL(entry);
    }

    KernelPatcher::RouteRequest request {"_cs_validate_page", wrapCsValidatePage, this->orgCsValidatePage};

    SYSLOG_COND(!patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1), "DYLD",
        "Failed to route kernel symbols");
}

void DYLDPatches::wrapCsValidatePage(vnode *vp, memory_object_t pager, memory_object_offset_t page_offset,
    const void *data, int *validated_p, int *tainted_p, int *nx_p) {
    FunctionCast(wrapCsValidatePage, callback->orgCsValidatePage)(vp, pager, page_offset, data, validated_p, tainted_p,
        nx_p);

    char path[PATH_MAX];
    int pathlen = PATH_MAX;
    if (vn_getpath(vp, path, &pathlen) != 0) { return; }

	// One-shot visibility logs for real bundle/image validation events.
	// If these lines appear, the corresponding bundle image was actually seen by cs_validate_page.
	static bool loggedTglMtl = false;
	static bool loggedTglGl = false;
	static bool loggedTglVa = false;
	if (!loggedTglMtl && strstr(path, "AppleIntelTGLGraphicsMTLDriver.bundle")) {
		loggedTglMtl = true;
		SYSLOG("DYLD", "MTL_BUNDLE_SEEN: %s", path);
	}
	if (!loggedTglGl && strstr(path, "AppleIntelTGLGraphicsGLDriver.bundle")) {
		loggedTglGl = true;
		SYSLOG("DYLD", "GL_BUNDLE_SEEN: %s", path);
	}
	if (!loggedTglVa && strstr(path, "AppleIntelTGLGraphicsVADriver.bundle")) {
		loggedTglVa = true;
		SYSLOG("DYLD", "VA_BUNDLE_SEEN: %s", path);
	}

    if (!UserPatcher::matchSharedCachePath(path)) {
        if (LIKELY(strncmp(path, kCoreLSKDMSEPath, arrsize(kCoreLSKDMSEPath))) ||
            LIKELY(strncmp(path, kCoreLSKDPath, arrsize(kCoreLSKDPath)))) {
            return;
        }
        const DYLDPatch patch = {kCoreLSKDOriginal, kCoreLSKDPatched, "CoreLSKD streaming CPUID to Haswell"};
        patch.apply(const_cast<void *>(data), PAGE_SIZE);
        return;
    }

    if (UNLIKELY(KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE, kVideoToolboxDRMModelOriginal,
            arrsize(kVideoToolboxDRMModelOriginal), BaseDeviceInfo::get().modelIdentifier, 20))) {
        DBGLOG("DYLD", "Applied 'VideoToolbox DRM model check' patch");
    }

    const DYLDPatch patches[] = {
        {kAGVABoardIdOriginal, kAGVABoardIdPatched, "iMacPro1,1 spoof (AppleGVA)"},
		{kHEVCEncBoardIdOriginal, kHEVCEncBoardIdPatched, "iMacPro1,1 spoof (AppleGVAHEVCEncoder)"},
    };
    DYLDPatch::applyAll(patches, const_cast<void *>(data), PAGE_SIZE);
	
	// ── V50: GPU bundle search path redirect ──
	// Metal calls gpu_bundle_find_trusted() in libsystem_sandbox.dylib to locate
	// GPU plugin bundles. This function searches exactly two directories:
	//   1. /Library/GPUBundles      (checked first)
	//   2. /System/Library/Extensions  (checked second)
	// using format "%s/%s.bundle" to construct paths.
	//
	// Apple never made a Mac with Tiger Lake — no TGL Metal driver exists in either
	// of those directories. The TGL driver is at /Library/Extensions/ (user-installed).
	//
	// Fix: patch the first search path in libsystem_sandbox's __cstring:
	//   "/Library/GPUBundles\0"  (20 bytes) → "/Library/Extensions\0" (20 bytes)
	// This makes gpu_bundle_find_trusted search /Library/Extensions/ first,
	// where the TGL driver bundle actually exists.
	// /System/Library/Extensions stays as the fallback for system GPU bundles.
	//
	// Alternative: manually copy the TGL bundle:
	//   sudo mkdir -p /Library/GPUBundles
	//   sudo cp -R /Library/Extensions/AppleIntelTGLGraphicsMTLDriver.bundle /Library/GPUBundles/
	static const uint8_t gpuPathFind[] = {
		0x2F, 0x4C, 0x69, 0x62, 0x72, 0x61, 0x72, 0x79, // /Library
		0x2F, 0x47, 0x50, 0x55, 0x42, 0x75, 0x6E, 0x64, // /GPUBund
		0x6C, 0x65, 0x73, 0x00,                           // les\0
	};
	static const uint8_t gpuPathRepl[] = {
		0x2F, 0x4C, 0x69, 0x62, 0x72, 0x61, 0x72, 0x79, // /Library
		0x2F, 0x45, 0x78, 0x74, 0x65, 0x6E, 0x73, 0x69, // /Extensi
		0x6F, 0x6E, 0x73, 0x00,                           // ons\0
	};

	/* Sandbox: WindowServer deny(1) file-map-executable 
	/Library/Extensions/AppleIntelTGLGraphicsMTLDriver.bundle — 
	Sandbox blocks WS from mapping the Metal driver as executable. 
	That's a secondary issue (separate from the kernel-side getBlit3DContext).

	if (UNLIKELY(KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE,
			gpuPathFind, arrsize(gpuPathFind), gpuPathRepl, arrsize(gpuPathRepl)))) {
		SYSLOG("DYLD", "V50: Patched gpu_bundle_find_trusted: /Library/GPUBundles -> /Library/Extensions");
	}*/
	
	// V50: ICL Metal driver device-ID bypass (mask-based, build-portable).
	// The ICL driver (in shared cache) checks device_id:vendor_id against
	// 0x8A5C8086/0x8A5D8086, then calls a hw-cap fallback check.
	// Patch: change jne to jmp so the hw-cap check always succeeds.
	// This is a fallback — if the TGL driver loads, this won't be needed.
	static const uint8_t f2find[] = {
		0x81, 0xFF, 0x86, 0x80, 0x5C, 0x8A,  // cmp edi, 0x8A5C8086
		0x74, 0x00,                            // je +XX (wildcard offset)
		0x81, 0xFF, 0x86, 0x80, 0x5D, 0x8A,  // cmp edi, 0x8A5D8086
		0x74, 0x00,                            // je +XX (wildcard offset)
		0xE8, 0x00, 0x00, 0x00, 0x00,         // call +XXXX (wildcard offset)
		0x84, 0xC0,                            // test al, al
		0x75, 0x00,                            // jne +XX → change to EB (jmp)
	};
	static const uint8_t f2mask[] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // cmp exact
		0xFF, 0x00,                            // je opcode exact, offset wildcard
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // cmp exact
		0xFF, 0x00,                            // je opcode exact, offset wildcard
		0xFF, 0x00, 0x00, 0x00, 0x00,         // call opcode exact, offset wildcard
		0xFF, 0xFF,                            // test exact
		0xFF, 0x00,                            // jne opcode exact, offset wildcard
	};
	static const uint8_t f2repl[] = {
		0x81, 0xFF, 0x86, 0x80, 0x5C, 0x8A,  // unchanged
		0x74, 0x00,                            // unchanged
		0x81, 0xFF, 0x86, 0x80, 0x5D, 0x8A,  // unchanged
		0x74, 0x00,                            // unchanged
		0xE8, 0x00, 0x00, 0x00, 0x00,         // unchanged
		0x84, 0xC0,                            // unchanged
		0xEB, 0x00,                            // jne→jmp (0x75→0xEB)
	};
	static const uint8_t f2rmask[] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // don't touch
		0x00, 0x00,                            // don't touch
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // don't touch
		0x00, 0x00,                            // don't touch
		0x00, 0x00, 0x00, 0x00, 0x00,         // don't touch
		0x00, 0x00,                            // don't touch
		0xFF, 0x00,                            // CHANGE byte 23 only (0x75→0xEB)
	};
	if (UNLIKELY(KernelPatcher::findAndReplaceWithMask(const_cast<void *>(data), PAGE_SIZE,
			f2find, f2mask, f2repl, f2rmask, 1, 0))) {
		SYSLOG("DYLD", "V50: Applied ICL Metal device-ID bypass (f2, mask-based)");
	}
	
	// Stage-3 Metal (hardcoded): assertion bypass + RunFullDisplayPipe NULL-guard
	// + GetMTLTexture/CQ stubs. AccessComplete is live (not skipped).

	//CoreDisplay_CreateDisplayForCGXDisplayDevice: NOP jne to __assert_rtn (Sonoma 14.7.1)
	static const uint8_t f3b_sonoma[] = {0x75, 0x0A, 0xE8, 0x79, 0x03, 0x0B, 0x00, 0xE9, 0xE6, 0xF6, 0xFF, 0xFF, 0x83, 0xBD, 0xC0, 0xFE, 0xFF, 0xFF, 0x00, 0x48, 0x8D, 0x05, 0xF2, 0x76, 0x0C, 0x00};
	static const uint8_t r3b_sonoma[] = {0x90, 0x90, 0xE8, 0x79, 0x03, 0x0B, 0x00, 0xE9, 0xE6, 0xF6, 0xFF, 0xFF, 0x83, 0xBD, 0xC0, 0xFE, 0xFF, 0xFF, 0x00, 0x48, 0x8D, 0x05, 0xF2, 0x76, 0x0C, 0x00};

	//CoreDisplay::DisplaySurface::GetMTLTexture - return NULL (Sonoma 14.7.1)
	static const uint8_t f_getmtltex_sonoma[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x68, 0x01, 0x00, 0x00, 0x49, 0x89, 0xf6, 0x49, 0x89, 0xff};
	static const uint8_t r_getmtltex_sonoma[] = {0x31, 0xC0, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
	
	//CoreDisplay::MetalDevice::GetMTLCommandQueue() const - return NULL (Sonoma 14.7.1)
	//Called from AccessComplete with rdi=NULL (NULL MetalDevice), crashes at mov rax,[rdi+8] (+30).
	static const uint8_t f_getmtlcq_sonoma[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x05, 0xe9, 0x29, 0x88, 0x3f, 0x48, 0x8b, 0x00};
	static const uint8_t r_getmtlcq_sonoma[] = {0x31, 0xc0, 0xc3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

	//CoreDisplay::DisplayPipe::RunFullDisplayPipe - NULL vcall guard at entry (Sonoma 14.7.1)
	//test rdi,rdi; jz +6 instead of mov rax,[rdi]; call [rax+0x28] — skips crash when rdi==NULL.
	static const uint8_t f_runfdp_guard_sonoma[] = {0x49, 0x8b, 0xbe, 0x88, 0x08, 0x00, 0x00, 0x48, 0x8b, 0x07, 0xff, 0x50, 0x28};
	static const uint8_t r_runfdp_guard_sonoma[] = {0x49, 0x8b, 0xbe, 0x88, 0x08, 0x00, 0x00, 0x48, 0x85, 0xff, 0x74, 0x06, 0x90};

	// V60: RunFullDisplayPipe isRemovable crash guard (Sonoma 14.7.1)
	// At RunFullDisplayPipe+2103, objc_msgSend is called as [device isRemovable].
	// On spoofed RPL/ADL the receiver (from rbp-0x490) is an __NSCFNumber, not a real
	// display device — causing NSInvalidArgumentException / WindowServer crash loop.
	// Fix: replace the 6-byte call with mov al,1 (returns 1 = removable).
	// The jne at +2110 jumps when al≠0 (isRemovable=true) to skip the Assert block.
	// Returning 0 (xor eax,eax) caused the jne to NOT be taken → CoreDisplay::Assert fires.
	// Returning 1 causes jne to be taken → Assert at fn+2156 (imgOff 0xC5FA6) is skipped.
	// Pattern is unique (1 match) at offset +0xC5F5A in CoreDisplay 14.7.1 __TEXT.
	static const uint8_t f_isrm_guard_sonoma[] = {
		0x48,0x8b,0x35,0xc7,0xf3,0x07,0x3e,  // mov rsi,[rip+...] (isRemovable selector)
		0x48,0x8b,0xbd,0x70,0xfb,0xff,0xff,  // mov rdi,[rbp-0x490] (receiver = NSNumber)
		0xff,0x15,0xa2,0xa8,0x8d,0x3f,       // call [rip+...] (objc_msgSend)
		0x84,0xc0                             // test al, al
	};
	static const uint8_t r_isrm_guard_sonoma[] = {
		0x48,0x8b,0x35,0xc7,0xf3,0x07,0x3e,  // (keep: selector load)
		0x48,0x8b,0xbd,0x70,0xfb,0xff,0xff,  // (keep: receiver load)
		0xb0,0x01,0x90,0x90,0x90,0x90,       // mov al,1; nop×4 (return 1 → jne taken → skip Assert)
		0x84,0xc0                             // (keep: test al,al)
	};

	// V187: CoreDisplay::DisplaySurface::AccessComplete crash guard (Sonoma 14.7.1).
	// Current WindowServer recycle loop crashes at:
	//   std::__hash_table<...>::find + 0x4
	//   CoreDisplay::DisplaySurface::AccessComplete + 1928
	// with KERN_INVALID_ADDRESS at 0x80 on spoofed RPL/ADL.
	// Short-circuit AccessComplete to avoid dereferencing invalid internal state.
	// Preserve real TGL behavior by applying only on !isRealTGL.
	static const uint8_t f_accesscomplete_guard_sonoma[] = {
		0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
		0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,
		0xd8,0x01,0x00,0x00,0x48,0x8b,0x05,0xc1,
		0xaa,0x8f,0x3f,0x48,0x8b,0x00,0x48,0x89
	};
	static const uint8_t r_accesscomplete_guard_sonoma[] = {
		0x31,0xc0,0xc3,0x90,0x90,0x90,0x90,0x90,
		0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
		0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
		0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90
	};

	// V188: Narrow guard for the active Sonoma WindowServer crash site.
	// Crash frame:
	//   std::__hash_table<id<MTLRenderPipelineState>...>::find + 0x4
	// with rdi=0x78/0x80-like garbage, causing mov r9,[rdi+8] fault.
	// Strategy: short-circuit this specific hash-table find specialization to
	// return a null/end iterator, letting AccessComplete continue through its
	// existing fallback path instead of stubbing the whole function.
	static const uint8_t f_hashfind_mtlps_guard_sonoma[] = {
		0x55,0x48,0x89,0xe5,0x4c,0x8b,0x4f,0x08,
		0x4d,0x85,0xc9,0x0f,0x84,0xcc,0x00,0x00,
		0x00,0x4c,0x89,0xc8,0x48,0xd1,0xe8,0x48,
		0xb9,0x55,0x55,0x55,0x55,0x55,0x55,0x55
	};
	static const uint8_t r_hashfind_mtlps_guard_sonoma[] = {
		0x31,0xc0,0xc3,0x90,0x90,0x90,0x90,0x90,
		0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
		0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
		0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90
	};

	// V189: Smart AccessComplete patch — MINIMAL variant (Sonoma 14.7.1).
	// Earlier full-signal-path variant (jumping to loc_7FF8022E40E0) caused full system
	// deadlock: it called _CGRegionCreateEmptyRegion / CFTypePtr::operator= before the
	// lock dec, and those CG calls deadlocked against WS-held locks. WS state stayed at
	// 0x3 (no degrade) but system froze on Apple-logo flash.
	//
	// New strategy: jump from prologue +0x2F directly to loc_7FF8022E4150 — the absolute
	// minimal completion-signal block:
	//   mov rax, [r15+0x178]                         ; surface backing
	//   test rax, rax
	//   jz short loc_7FF8022E4163
	//   lock dec dword ptr [rax+0x170]               ; ← KERNEL WAKEUP (the only signal)
	//   mov qword ptr [r15+0x178], 0                 ; clear backing
	//   <canary check + ret>
	// No CG calls, no CFTypePtr, no region housekeeping. Just the atomic dec that wakes
	// the kernel-side waiter, then return.
	//
	// Find pattern (16 bytes at function offset +0x2F):
	//   mov rdi, [rdi+0x178]      48 8B BF 78 01 00 00
	//   test rdi, rdi              48 85 FF
	//   jz loc_7FF8022E40B8        0F 84 15 22 00 00     (rel offset 0x2215)
	// Replace with:
	//   jmp loc_7FF8022E4150       E9 B8 22 00 00        (rel offset 0x22B8)
	//   + 11 NOPs
	static const uint8_t f_accesscomplete_v189_sonoma[] = {
		0x48,0x8B,0xBF,0x78,0x01,0x00,0x00,
		0x48,0x85,0xFF,
		0x0F,0x84,0x15,0x22,0x00,0x00
	};
	static const uint8_t r_accesscomplete_v189_sonoma[] = {
		0xE9,0xB8,0x22,0x00,0x00,
		0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90
	};

	if (getKernelVersion() >= KernelVersion::Ventura) {
		const bool isRealTGL = NGreen::callback && NGreen::callback->isRealTGL;
		const bool forceFullMTL = shouldForceFullMetalPath();
		//const bool applyCoreDisplaySafety = isRealTGL || forceFullMTL || !isRealTGL;
		{
			const bool fullMTLActive = isRealTGL || forceFullMTL;
			static bool loggedFullMTL = false;
			if (!loggedFullMTL) {
				loggedFullMTL = true;
				SYSLOG("DYLD", "FULL_MTL_ACTIVE=%d (isRealTGL=%d forceFullMTL=%d)",
				       fullMTLActive, isRealTGL, forceFullMTL);
			}
		}
		// COMMENTED OUT (2026-05-09): the gate `applyCoreDisplaySafety = isRealTGL || forceFullMTL || !isRealTGL`
		// is a tautology (A || B || !A == true), so these two patches were always being applied
		// regardless of mode. Disabling the branch entirely as a diagnostic to see whether the
		// CoreDisplay assertion bypass and RunFullDisplayPipe NULL guard are actually needed
		// alongside the V189 signal-path patch. Re-enable if WS/cursor init regresses.
		/*
		if (applyCoreDisplaySafety) {
			const DYLDPatch assertionPatch[] = {
				{f3b_sonoma, r3b_sonoma, "CoreDisplay assertion bypass (Sonoma)"},
			};
			DYLDPatch::applyAll(assertionPatch, const_cast<void *>(data), PAGE_SIZE);

			const DYLDPatch fdpGuardPatch[] = {
				{f_runfdp_guard_sonoma, r_runfdp_guard_sonoma, "RunFullDisplayPipe NULL vcall guard (Sonoma)"},
			};
			DYLDPatch::applyAll(fdpGuardPatch, const_cast<void *>(data), PAGE_SIZE);
		}
		*/

		if (!isRealTGL && !forceFullMTL) {
			// For spoofed (non-TGL) hardware the CoreDisplay MetalDevice is never
			// validly constructed — its internal mtlDevice field holds an
			// NSTaggedPointerString rather than a real MTLDevice object.
			// Stubbing both accessors to NULL prevents objc_msgSend from
			// dispatching Metal methods onto the garbage pointer.
			//
			// Gated on !forceFullMTL: the whole point of -ngreenfullmtl on
			// spoofed hardware is to drive the real Metal path (see
			// kern_gen11.cpp:7346 and the !wegCoexist || forceFullMTL branches
			// around kern_gen11.cpp:1075). Stubbing GetMTLTexture /
			// GetMTLCommandQueue and truncating AccessComplete here would
			// neuter the very accessors that path needs, so when forceFullMTL
			// is set we skip these patches and let the real Metal accessors
			// run. Trade-off: loses the RunFullDisplayPipe isRemovable crash
			// guard under forceFullMTL — re-add it (with its own gate) if RFDP
			// hits a NULL vcall on spoofed hardware in that mode.
			// DISABLED (2026-05-16): GFX pipeline is now robust enough that MetalDevice may
			// be validly constructed. Stubbing these to NULL blocks real Metal compositing
			// while other WS code still tries to drive the MTL path → MTLCompilerService
			// abort loop. Re-enable only if objc_msgSend crashes on a NULL mtlDevice.
			/*
			const DYLDPatch getMtlTextureSafetyPatch[] = {
				{f_getmtltex_sonoma, r_getmtltex_sonoma, "GetMTLTexture return NULL (Sonoma)"},
			};
			DYLDPatch::applyAll(getMtlTextureSafetyPatch, const_cast<void *>(data), PAGE_SIZE);

			const DYLDPatch getMtlCommandQueueSafetyPatch[] = {
				{f_getmtlcq_sonoma, r_getmtlcq_sonoma, "GetMTLCommandQueue return NULL (Sonoma)"},
			};
			DYLDPatch::applyAll(getMtlCommandQueueSafetyPatch, const_cast<void *>(data), PAGE_SIZE);
			*/

			const DYLDPatch isRemovableGuardPatch[] = {
				{f_isrm_guard_sonoma, r_isrm_guard_sonoma, "RunFullDisplayPipe isRemovable crash guard (Sonoma)"},
			};
			DYLDPatch::applyAll(isRemovableGuardPatch, const_cast<void *>(data), PAGE_SIZE);

			// V187 vs V189 selection.
			// -ngreenv189 (opt-in): apply the smart signal-path patch instead of V187.
			//   V189 lets the prologue run, then jumps from +0x2F to loc_7FF8022E40E0
			//   (the completion-signal path: lock dec on surface->[+0x178]+0x170).
			//   This unblocks the WS 234 ms timeout while still avoiding the Metal crash.
			// Default: V187 (full-stub, safe but never signals — Apple flash freezes).
			static bool loggedV189Decision = false;
			const bool wantV189 = checkKernelArgument("-ngreenv189");
			if (!loggedV189Decision) {
				loggedV189Decision = true;
				SYSLOG("DYLD", "V189: -ngreenv189 detected=%d", wantV189);
			}
			if (wantV189) {
				if (UNLIKELY(KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE,
						f_accesscomplete_v189_sonoma, sizeof(f_accesscomplete_v189_sonoma),
						r_accesscomplete_v189_sonoma, sizeof(r_accesscomplete_v189_sonoma)))) {
					SYSLOG("DYLD", "V189: AccessComplete signal-path jump applied");
				}
			} else {
				if (UNLIKELY(KernelPatcher::findAndReplace(const_cast<void *>(data), PAGE_SIZE,
						f_accesscomplete_guard_sonoma, sizeof(f_accesscomplete_guard_sonoma),
						r_accesscomplete_guard_sonoma, sizeof(r_accesscomplete_guard_sonoma)))) {
					SYSLOG("DYLD", "V187: AccessComplete crash guard applied");
				}
			}

			// V188: narrower hash-find guard — caused black screen regression, debug only.
			if (checkKernelArgument("-ngreenV188htfind")) {
				const DYLDPatch hashFindGuardPatch[] = {
					{f_hashfind_mtlps_guard_sonoma, r_hashfind_mtlps_guard_sonoma, "MTLRenderPipelineState hash::find crash guard V188 (Sonoma)"},
				};
				DYLDPatch::applyAll(hashFindGuardPatch, const_cast<void *>(data), PAGE_SIZE);
			}
		}
	}
}
