// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Gif_Unit.h"
#include "Config.h"
#include "DebugTools/GsTrace.h"
#include "GS.h"
#include "GS/GSPerfMon.h"
#include "GS/GSState.h"
#include "MTGS.h"
#include "MTVU.h"
#include "vita/VitaGsMailbox.h"

#include <memory>

Pcsx2Config::GSOptions GSConfig;

GSRendererType GSGetCurrentRenderer()
{
	return GSRendererType::SW;
}

bool GSIsHardwareRenderer()
{
	return false;
}

namespace MTGS
{
	static Threading::ThreadHandle s_thread_handle;

	class VitaHeadlessGsState final : public GSState
	{
	public:
		void Draw() override {}
	};

	static std::unique_ptr<VitaHeadlessGsState> s_gs;

	static u8 GsTraceSourceForGifPath(GIF_PATH path)
	{
		switch (path)
		{
			case GIF_PATH_1: return Pcsx2Trace::GsTraceSourcePath1;
			case GIF_PATH_2: return Pcsx2Trace::GsTraceSourcePath2;
			case GIF_PATH_3: return Pcsx2Trace::GsTraceSourcePath3;
			default: return Pcsx2Trace::GsTraceSourcePath3;
		}
	}

	static bool EnsureGsOpen()
	{
		if (s_gs)
			return true;

		GSConfig = EmuConfig.GS;
		GSConfig.Renderer = GSRendererType::SW;
		GSConfig.UserHacks_GPUTargetCLUTMode = GSGPUTargetCLUTMode::Disabled;

		s_gs = std::make_unique<VitaHeadlessGsState>();
		s_gs->SetRegsMem(g_RealGSMem);
		s_gs->Reset(true);
		g_perfmon.Reset();
		return true;
	}

	static void TransferCompletedPacket(GS_Packet& gsPack, GIF_PATH path)
	{
		Gif_Path& gif_path = gifUnit.gifPath[path];
		gif_path.readAmount.fetch_add(gsPack.size, std::memory_order_acq_rel);

		if (EnsureGsOpen())
		{
			const Pcsx2Trace::ScopedGsTraceSourceOverride trace_source(GsTraceSourceForGifPath(path));
			s_gs->Transfer<3>(&gif_path.buffer[gsPack.offset], gsPack.size / 16);
		}

		gif_path.readAmount.fetch_sub(gsPack.size, std::memory_order_acq_rel);
	}

	static void TransferCompletedMtvUPath1Packet()
	{
		if (!vu1Thread.semaXGkick.TryWait())
			vu1Thread.semaXGkick.Wait();

		Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
		GS_Packet gsPack = path.GetGSPacketMTVU();

		if (EnsureGsOpen() && gsPack.size)
		{
			const Pcsx2Trace::ScopedGsTraceSourceOverride trace_source(Pcsx2Trace::GsTraceSourcePath1);
			s_gs->Transfer<3>(&path.buffer[gsPack.offset], gsPack.size / 16);
		}

		path.readAmount.fetch_sub(gsPack.size + gsPack.readAmount, std::memory_order_acq_rel);
		path.PopGSPacketMTVU();
	}

	const Threading::ThreadHandle& GetThreadHandle()
	{
		return s_thread_handle;
	}

	bool IsOpen()
	{
		return static_cast<bool>(s_gs);
	}

	void StartThread()
	{
	}

	void ShutdownThread()
	{
		WaitForClose();
	}

	void PresentCurrentFrame()
	{
	}

	void WaitGS(bool syncRegs, bool weakWait, bool isMTVU)
	{
	}

	void ResetGS(bool hardware_reset)
	{
		if (EnsureGsOpen())
			s_gs->Reset(hardware_reset);
	}

	bool WaitForOpen()
	{
		return EnsureGsOpen();
	}

	void WaitForClose()
	{
		s_gs.reset();
	}

	void Freeze(FreezeAction mode, FreezeData& data)
	{
		if (!EnsureGsOpen())
		{
			data.retval = -1;
			return;
		}

		if (mode == FreezeAction::Save)
			data.retval = s_gs->Freeze(data.fdata, false);
		else if (mode == FreezeAction::Size)
			data.retval = s_gs->Freeze(data.fdata, true);
		else
			data.retval = s_gs->Defrost(data.fdata);
	}

	int GetCurrentVsyncQueueSize()
	{
		return 0;
	}

	void PostVsyncStart(bool registers_written)
	{
		if (!EnsureGsOpen())
			return;

		s_gs->TraceGsStateSnapshot(Pcsx2Trace::GsTraceStateTriggerVSyncStart);
		s_gs->PCRTCDisplays.SetVideoMode(s_gs->GetVideoMode());
		s_gs->PCRTCDisplays.EnableDisplays(s_gs->m_regs->PMODE, s_gs->m_regs->SMODE2, s_gs->isReallyInterlaced());
		s_gs->PCRTCDisplays.SetRects(0, s_gs->m_regs->DISP[0].DISPLAY, s_gs->m_regs->DISP[0].DISPFB);
		s_gs->PCRTCDisplays.SetRects(1, s_gs->m_regs->DISP[1].DISPLAY, s_gs->m_regs->DISP[1].DISPFB);
		s_gs->PCRTCDisplays.CheckSameSource();
		s_gs->PCRTCDisplays.CalculateDisplayOffset(s_gs->m_scanmask_used);
		s_gs->PCRTCDisplays.CalculateFramebufferOffset(s_gs->m_scanmask_used, s_gs->m_regs->DISP[0].DISPFB, s_gs->m_regs->DISP[1].DISPFB);
		s_gs->Flush(GSState::VSYNC);
		g_perfmon.EndFrame(false);
		if ((g_perfmon.GetFrame() & 0x1f) == 0)
			g_perfmon.Update();
		(void)registers_written;
	}

	void InitAndReadFIFO(u8* mem, u32 qwc)
	{
		if (EnsureGsOpen())
		{
			s_gs->InitReadFIFO(mem, qwc);
			s_gs->ReadFIFO(mem, qwc);
		}
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

bool GSValidatePortableState()
{
	// PCSX2 owner: GS/GS.cpp::GSValidatePortableState(). The Vita headless
	// mailbox owns the live GSState instance instead of g_gs_renderer.
	return MTGS::EnsureGsOpen() && MTGS::s_gs->ValidatePortableState();
}

void GSTraceStateSnapshot(u8 trigger)
{
	if (MTGS::s_gs)
		MTGS::s_gs->TraceGsStateSnapshot(trigger);
}

const u8* GSTraceLocalMemoryData(size_t* size)
{
	if (size)
		*size = 0;

	return MTGS::s_gs ? MTGS::s_gs->TraceGsLocalMemoryData(size) : nullptr;
}

const u8* VitaGS::GetLocalMemoryForTrace(size_t* size)
{
	return GSTraceLocalMemoryData(size);
}

void Gif_AddGSPacketMTVU(GS_Packet& gsPack, GIF_PATH path)
{
	(void)gsPack;
	(void)path;
	MTGS::TransferCompletedMtvUPath1Packet();
}

void Gif_AddCompletedGSPacket(GS_Packet& gsPack, GIF_PATH path)
{
	MTGS::TransferCompletedPacket(gsPack, path);
}

void Gif_AddBlankGSPacket(u32 size, GIF_PATH path)
{
	gifUnit.gifPath[path].readAmount.fetch_add(size, std::memory_order_acq_rel);
	gifUnit.gifPath[path].readAmount.fetch_sub(size, std::memory_order_acq_rel);
}

void Gif_MTGS_Wait(bool isMTVU)
{
}
