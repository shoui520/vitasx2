// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Gif_Unit.h"
#include "MTGS.h"

namespace MTGS
{
	static Threading::ThreadHandle s_thread_handle;

	const Threading::ThreadHandle& GetThreadHandle()
	{
		return s_thread_handle;
	}

	bool IsOpen()
	{
		return false;
	}

	void StartThread()
	{
	}

	void ShutdownThread()
	{
	}

	void PresentCurrentFrame()
	{
	}

	void WaitGS(bool syncRegs, bool weakWait, bool isMTVU)
	{
	}

	void ResetGS(bool hardware_reset)
	{
	}

	bool WaitForOpen()
	{
		return true;
	}

	void WaitForClose()
	{
	}

	void Freeze(FreezeAction mode, FreezeData& data)
	{
		data.retval = 0;
	}

	int GetCurrentVsyncQueueSize()
	{
		return 0;
	}

	void PostVsyncStart(bool registers_written)
	{
	}

	void InitAndReadFIFO(u8* mem, u32 qwc)
	{
	}

	void RunOnGSThread(AsyncCallType func)
	{
		if (func)
			func();
	}

	void GameChanged()
	{
	}

	void ApplySettings()
	{
	}

	void ResizeDisplayWindow(u32 width, u32 height, float scale)
	{
	}

	void UpdateDisplayWindow()
	{
	}

	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
	{
	}

	void UpdateVSyncMode()
	{
	}

	void SetSoftwareRendering(bool software, GSInterlaceMode interlace, bool display_message)
	{
	}

	void ToggleSoftwareRendering()
	{
	}

	bool SaveMemorySnapshot(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
		u32* width, u32* height, std::vector<u32>* pixels)
	{
		return false;
	}

	void SetRunIdle(bool enabled)
	{
	}
} // namespace MTGS

void Gif_AddGSPacketMTVU(GS_Packet& gsPack, GIF_PATH path)
{
	gifUnit.gifPath[path].readAmount.fetch_add(gsPack.size);
}

void Gif_AddCompletedGSPacket(GS_Packet& gsPack, GIF_PATH path)
{
	gifUnit.gifPath[path].readAmount.fetch_add(gsPack.size);
}

void Gif_AddBlankGSPacket(u32 size, GIF_PATH path)
{
	gifUnit.gifPath[path].readAmount.fetch_add(size);
}

void Gif_MTGS_Wait(bool isMTVU)
{
}
