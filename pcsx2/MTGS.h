// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS.h"

#include "common/Threading.h"

#include <functional>

/////////////////////////////////////////////////////////////////////////////
// MTGS Threaded Class Declaration

// Uncomment this to enable the MTGS debug stack, which tracks to ensure reads
// and writes stay synchronized.  Warning: the debug stack is VERY slow.
//#define RINGBUF_DEBUG_STACK

namespace MTGS
{
	using AsyncCallType = std::function<void()>;

	enum class Command : u32
	{
		GIFPath1,
		GIFPath2,
		GIFPath3,
		VSync,
		Freeze,
		Reset, // issues a GSreset() command.
		SoftReset, // issues a soft reset for the GIF
		GSPacket,
		MTVUGSPacket,
		GpuVuDraw,
		InitAndReadFIFO,
		AsyncCall,
	};

	struct FreezeData
	{
		freezeData* fdata;
		s32 retval; // value returned from the call, valid only after an mtgsWaitGS()
	};

	const Threading::ThreadHandle& GetThreadHandle();
	bool IsOpen();

	/// Starts the thread, if it hasn't already been started.
	void StartThread();

	/// Fully stops the thread, closing in the process if needed.
	void ShutdownThread();

	/// Re-presents the current frame. Call when things like window resizes happen to re-display
	/// the current frame with the correct proportions. Should only be called from the CPU thread.
	void PresentCurrentFrame();

	/// Waits for the GS to empty out the entire ring buffer contents.
	void WaitGS(bool syncRegs = true, bool weakWait = false, bool isMTVU = false);
	void ResetGS(bool hardware_reset);

	bool WaitForOpen();
	void WaitForClose();
	void Freeze(FreezeAction mode, FreezeData& data);

	int GetCurrentVsyncQueueSize();
	void PostVsyncStart(bool registers_written);
	void InitAndReadFIFO(u8* mem, u32 qwc);

	void RunOnGSThread(AsyncCallType func);
	void GameChanged();
	void ApplySettings();
	void ResizeDisplayWindow(u32 width, u32 height, float scale);
	void UpdateDisplayWindow();
	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle);
	void UpdateVSyncMode();
	void SetSoftwareRendering(bool software, GSInterlaceMode interlace, bool display_message = true);
	void ToggleSoftwareRendering();
	bool SaveMemorySnapshot(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
		u32* width, u32* height, std::vector<u32>* pixels);
	void SetRunIdle(bool enabled);

	// Size of the ringbuffer as a power of 2 -- size is a multiple of simd128s.
	// Vita keeps GIF payloads in Gif_Path and queues only their offset/size, so
	// PCSX2's desktop 8 MiB ring would waste scarce LPDDR and also inflate the
	// MTVU packet queues which key their capacity from this constant. 16K
	// command qwords retain room for several measured packet-heavy frames while
	// consuming 256 KiB.
#if defined(VITASX2_VITA)
	static const uint RingBufferSizeFactor = 14;
#else
	static const uint RingBufferSizeFactor = 19;
#endif

	// size of the ringbuffer in simd128's.
	static const uint RingBufferSize = 1 << RingBufferSizeFactor;

	// Mask to apply to ring buffer indices to wrap the pointer from end to
	// start (the wrapping is what makes it a ringbuffer, yo!)
	static const uint RingBufferMask = RingBufferSize - 1;
}
