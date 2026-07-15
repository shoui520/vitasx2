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
#include "common/Assertions.h"
#include "common/Error.h"
#include "vita/VitaGxmGsState.h"
#include "vita/VitaGsMailbox.h"

#include <array>
#include <cstring>
#include <memory>

Pcsx2Config::GSOptions GSConfig;

GSRendererType GSGetCurrentRenderer()
{
	return GSRendererType::SW;
}

bool GSIsHardwareRenderer()
{
	// This predicate selects PCSX2 hardware texture-cache, transfer, reset and
	// PCRTC semantics; it is not an accelerator-presence bit. VitaGxmGsState
	// still owns canonical GSLocalMemory like the software path, so reporting
	// hardware here would enter unported GSRendererHW mechanisms.
	return false;
}

namespace MTGS
{
	static Threading::ThreadHandle s_thread_handle;

	static bool s_native_presenter_enabled = false;
	static std::unique_ptr<VitaGxmGsState> s_gs;

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

		s_gs = std::make_unique<VitaGxmGsState>(s_native_presenter_enabled);
		if (s_native_presenter_enabled && !s_gs->IsNativePresenterReady())
		{
			s_gs.reset();
			return false;
		}
		s_gs->SetRegsMem(g_RealGSMem);
		// PCSX2 owner: GS/GS.cpp::OpenGSRenderer(). Construction establishes
		// GSState; the MTGS::ResetGS() caller applies the requested hardware or
		// soft reset. Repeating a hardware reset here was both redundant and
		// observably different from the renderer lifecycle owner.
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
		if (s_gs)
			s_gs->Present();
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

		s_gs->PCRTCDisplays.SetVideoMode(s_gs->GetVideoMode());
		s_gs->PCRTCDisplays.EnableDisplays(s_gs->m_regs->PMODE, s_gs->m_regs->SMODE2, s_gs->isReallyInterlaced());
		s_gs->PCRTCDisplays.SetRects(0, s_gs->m_regs->DISP[0].DISPLAY, s_gs->m_regs->DISP[0].DISPFB);
		s_gs->PCRTCDisplays.SetRects(1, s_gs->m_regs->DISP[1].DISPLAY, s_gs->m_regs->DISP[1].DISPFB);
		s_gs->PCRTCDisplays.CheckSameSource();
		s_gs->PCRTCDisplays.CalculateDisplayOffset(s_gs->m_scanmask_used);
		s_gs->PCRTCDisplays.CalculateFramebufferOffset(s_gs->m_scanmask_used, s_gs->m_regs->DISP[0].DISPFB, s_gs->m_regs->DISP[1].DISPFB);
		s_gs->Flush(GSState::VSYNC);
		s_gs->VSync();
		g_perfmon.EndFrame(false);
		if ((g_perfmon.GetFrame() & 0x1f) == 0)
			g_perfmon.Update();
		// PCSX2 owner: GS.cpp::GSvsync() snapshots only after Flush() and
		// GSRenderer::VSync(), so pending draws and frame-aged state are visible at
		// the same architectural boundary as the x86 trace oracle.
		s_gs->TraceGsStateSnapshot(Pcsx2Trace::GsTraceStateTriggerVSyncStart);
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

void VitaGS::SetNativePresenterEnabled(bool enabled)
{
	// The GS owner must be selected before VMManager opens it. Changing GXM
	// process ownership while a GSState is live would violate libgxm teardown.
	pxAssertRel(!MTGS::s_gs, "native GS presenter selection changed while open");
	MTGS::s_native_presenter_enabled = enabled;
}

bool VitaGS::IsNativePresenterEnabled()
{
	return MTGS::s_native_presenter_enabled;
}

bool GSValidatePortableState()
{
	// PCSX2 owner: GS/GS.cpp::GSValidatePortableState(). The Vita mailbox owns
	// the live canonical GSState instance instead of g_gs_renderer.
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

#if defined(VITASX2_PRODUCT_BOOT_VALIDATION) && VITASX2_PRODUCT_BOOT_VALIDATION
bool VitaGS::ValidateCanonicalLocalMemoryRing(
	CanonicalRingValidationResult* result, Error* error)
{
	if (!result)
	{
		Error::SetString(error, "GS canonical-ring validation has no result storage.");
		return false;
	}
	*result = {};

	if (!MTGS::EnsureGsOpen())
	{
		Error::SetString(error, "GS canonical-ring validation could not open GSState.");
		return false;
	}

	// This is byte-for-byte the boundary packet emitted by
	// tests/ps2/gs_transfer_trace. PCSX2's x86 owner maps one 4 MiB backing
	// store four times, so DBP 0x3fff plus DSAX 64 lands back at canonical
	// byte offset 0x1f00.
	alignas(16) constexpr std::array<u64, 14> PACKET = {
		0x1000000000000004ull, 0x000000000000000eull,
		0x00013fff00010000ull, 0x0000000000000050ull,
		0x0000004000000000ull, 0x0000000000000051ull,
		0x0000000200000002ull, 0x0000000000000052ull,
		0x0000000000000000ull, 0x0000000000000053ull,
		0x0800000000008001ull, 0x0000000000000000ull,
		0xff405060ff102030ull, 0xffa0b0c0ff708090ull,
	};
	constexpr size_t CANONICAL_BYTES = 4 * 1024 * 1024;
	constexpr u64 ZERO_HASH = 0xf8e3e56ce9222325ull;
	constexpr u64 PACKET_HASH = 0x13403ace12c224dcull;
	constexpr u64 LOCAL_HASH = 0xe77d7ce0beced965ull;
	constexpr std::array<u32, 4> PIXELS = {
		0xff102030u, 0xff405060u, 0xff708090u, 0xffa0b0c0u,
	};

	const char* failure = nullptr;
	auto fail = [&failure](const char* reason) {
		if (!failure)
			failure = reason;
		return false;
	};

	const bool content_ok = [&]() {
		size_t local_bytes = 0;
		u8* const local = const_cast<u8*>(
			MTGS::s_gs->TraceGsLocalMemoryData(&local_bytes));
		result->canonical_bytes = static_cast<u32>(local_bytes);
		result->packet_qwc = static_cast<u32>(PACKET.size() / 2);
		if (!local || local_bytes != CANONICAL_BYTES)
			return fail("GSState did not expose one canonical 4 MiB local-memory window");
		if (Pcsx2Trace::HashGsTraceBytes(local, local_bytes) != ZERO_HASH)
			return fail("new GS local memory was not zero before boundary validation");

		result->packet_hash = Pcsx2Trace::HashGsTraceBytes(
			PACKET.data(), PACKET.size() * sizeof(PACKET[0]));
		if (result->packet_hash != PACKET_HASH)
			return fail("embedded GS boundary packet does not match the x86 oracle fixture");

		MTGS::s_gs->Transfer<3>(reinterpret_cast<const u8*>(PACKET.data()),
			result->packet_qwc);
		result->local_hash = Pcsx2Trace::HashGsTraceBytes(local, local_bytes);
		if (result->local_hash != LOCAL_HASH)
			return fail("boundary GIF upload did not match PCSX2 canonical GS memory");

		for (u32 i = 0; i < PIXELS.size(); i++)
		{
			const int x = 64 + static_cast<int>(i & 1u);
			const int y = static_cast<int>(i >> 1);
			const u32 address = GSLocalMemory::PixelAddress32(x, y, 0x3fff, 1);
			if (address >= (CANONICAL_BYTES / sizeof(u32)) ||
				MTGS::s_gs->m_mem.ReadPixel32(address) != PIXELS[i])
			{
				return fail("boundary GIF pixel address or value was not canonical");
			}
			result->pixel_checks++;
		}

		// Cover every distinct scalar swizzle representation which consumes the
		// final PAHelper address. One page step from the last GS block must fold
		// into the canonical window in the PSM's own address units.
		const u32 address32 = GSLocalMemory::PixelAddress32(64, 0, 0x3fff, 1);
		const u32 address16 = GSLocalMemory::PixelAddress16(64, 0, 0x3fff, 1);
		const u32 address16s = GSLocalMemory::PixelAddress16S(64, 0, 0x3fff, 1);
		const u32 address8 = GSLocalMemory::PixelAddress8(128, 0, 0x3fff, 1);
		const u32 address4 = GSLocalMemory::PixelAddress4(128, 0, 0x3fff, 1);
		const u32 address32z = GSLocalMemory::PixelAddress32Z(64, 0, 0x3fff, 1);
		const u32 address16z = GSLocalMemory::PixelAddress16Z(64, 0, 0x3fff, 1);
		const u32 address16sz = GSLocalMemory::PixelAddress16SZ(64, 0, 0x3fff, 1);
		if (address32 != 0x07c0u || address16 != 0x0f80u ||
			address16s != 0x0f80u || address8 != 0x1f00u ||
			address4 != 0x3e00u || address32z != 0x01c0u ||
			address16z != 0x0380u || address16sz != 0x0380u)
		{
			return fail("a scalar GS swizzle did not match PCSX2's modulo address");
		}

		auto check32 = [&](u32 address, u32 value) {
			if (address >= CANONICAL_BYTES / sizeof(u32))
				return false;
			MTGS::s_gs->m_mem.WritePixel32(address, value);
			return MTGS::s_gs->m_mem.ReadPixel32(address) == value;
		};
		auto check16 = [&](u32 address, u16 value) {
			if (address >= CANONICAL_BYTES / sizeof(u16))
				return false;
			MTGS::s_gs->m_mem.WritePixel16(address, value);
			return MTGS::s_gs->m_mem.ReadPixel16(address) == value;
		};
		if (!check32(address32, 0x12345678u) ||
			!check16(address16, 0x1357u) ||
			!check16(address16s, 0x2468u) ||
			address8 >= CANONICAL_BYTES ||
			address4 >= CANONICAL_BYTES * 2u ||
			!check32(address32z, 0x89abcdefu) ||
			!check16(address16z, 0x55aau) ||
			!check16(address16sz, 0xaa55u))
		{
			return fail("a scalar GS swizzle escaped its canonical address units");
		}
		MTGS::s_gs->m_mem.WritePixel8(address8, 0x5au);
		if (MTGS::s_gs->m_mem.ReadPixel8(address8) != 0x5au)
			return fail("8-bit GS scalar wrapping changed the stored value");
		// address4 is a nibble address for the same physical byte as address8.
		// Validate it only after the complete-byte round trip above.
		MTGS::s_gs->m_mem.WritePixel4(address4, 0x0du);
		if (MTGS::s_gs->m_mem.ReadPixel4(address4) != 0x0du)
		{
			return fail("4-bit GS scalar wrapping changed the stored value");
		}
		result->address_checks = 8;

		// PCSX2's CSM1 loaders consume two or four contiguous blocks. Compare a
		// normal source against the same bytes split across the canonical seam;
		// this enters all three cold staging call sites without duplicating the
		// owner's CLUT swizzle algorithm in the test.
		std::array<u8, 1024> source_bytes = {};
		std::array<u32, 256> reference_palette = {};
		std::array<u32, 256> wrapped_palette = {};
		const GIFRegTEXCLUT texclut = {};
		GIFRegTEXA texa = {};
		texa.TA0 = 0x80;
		texa.TA1 = 0xff;

		auto block_pointer = [&](u32 cpsm, u32 bp) -> u8* {
			switch (cpsm)
			{
				case PSMCT32: return MTGS::s_gs->m_mem.BlockPtr32(0, 0, bp, 1);
				case PSMCT16: return MTGS::s_gs->m_mem.BlockPtr16(0, 0, bp, 1);
				case PSMCT16S: return MTGS::s_gs->m_mem.BlockPtr16S(0, 0, bp, 1);
				default: return nullptr;
			}
		};
		auto load_palette = [&](u32 cpsm, u32 cbp,
			std::array<u32, 256>* output) {
			GIFRegTEX0 tex0 = {};
			tex0.PSM = PSMT8;
			tex0.CBP = cbp;
			tex0.CPSM = cpsm;
			tex0.CSM = 0;
			tex0.CSA = 0;
			MTGS::s_gs->m_mem.m_clut.Reset();
			MTGS::s_gs->m_mem.m_clut.Write(tex0, texclut);
			MTGS::s_gs->m_mem.m_clut.Read32(tex0, texa);
			for (u32 i = 0; i < output->size(); i++)
				(*output)[i] = MTGS::s_gs->m_mem.m_clut[i];
		};

		constexpr std::array<u32, 3> CLUT_FORMATS = {
			PSMCT32, PSMCT16, PSMCT16S,
		};
		for (u32 cpsm : CLUT_FORMATS)
		{
			const size_t span = (cpsm == PSMCT32) ? 1024 : 512;
			u32 pattern = 0x6d2b79f5u ^ (cpsm * 0x9e3779b9u);
			for (size_t i = 0; i < span; i++)
			{
				pattern = pattern * 1664525u + 1013904223u;
				source_bytes[i] = static_cast<u8>(pattern >> 24);
			}

			std::memset(local, 0, local_bytes);
			u8* const normal_source = block_pointer(cpsm, 0x0100);
			if (!normal_source)
				return fail("CLUT validation selected an unsupported source format");
			std::memcpy(normal_source, source_bytes.data(), span);
			load_palette(cpsm, 0x0100, &reference_palette);

			std::memset(local, 0, local_bytes);
			u8* const wrapped_source = block_pointer(cpsm, 0x3fff);
			if (!wrapped_source)
				return fail("CLUT boundary source pointer was unavailable");
			const size_t tail = static_cast<size_t>((local + local_bytes) - wrapped_source);
			if (tail >= span)
				return fail("CLUT boundary source did not cross the canonical seam");
			std::memcpy(wrapped_source, source_bytes.data(), tail);
			std::memcpy(local, source_bytes.data() + tail, span - tail);
			load_palette(cpsm, 0x3fff, &wrapped_palette);
			if (wrapped_palette != reference_palette)
				return fail("seam-staged CSM1 palette differs from PCSX2's contiguous loader");
			result->clut_cases++;
		}

		// The optimized PSMCT32 download reads two aligned eight-pixel groups.
		// Starting at the final block makes the second group advance from block
		// 0x3fff to block 0, while each vector load remains within one block.
		std::array<u32, 16> expected_readback = {};
		alignas(16) std::array<u32, 16> actual_readback = {};
		for (u32 i = 0; i < expected_readback.size(); i++)
		{
			expected_readback[i] = 0xa5000000u | (i * 0x010101u);
			MTGS::s_gs->m_mem.WritePixel32(static_cast<int>(i), 0,
				expected_readback[i], 0x3fff, 1);
		}
		GIFRegBITBLTBUF blit = {};
		blit.SBP = 0x3fff;
		blit.SBW = 1;
		blit.SPSM = PSMCT32;
		GIFRegTRXPOS position = {};
		position.SSAX = 0;
		GIFRegTRXREG extent = {};
		extent.RRW = static_cast<u32>(expected_readback.size());
		extent.RRH = 1;
		int tx = position.SSAX;
		int ty = position.SSAY;
		MTGS::s_gs->m_mem.ReadImageX(tx, ty,
			reinterpret_cast<u8*>(actual_readback.data()),
			static_cast<int>(actual_readback.size() * sizeof(actual_readback[0])),
			blit, position, extent);
		if (actual_readback != expected_readback || tx != position.SSAX || ty != 1)
			return fail("optimized GS readback differed at the canonical seam");
		result->readback_checks = static_cast<u32>(actual_readback.size());
		return true;
	}();

	// Transfer() retains upload bookkeeping which Reset() intentionally does
	// not discard. Destroy and recreate the mailbox-owned GSState so the guest
	// begins from the same clean state it had before this validation.
	MTGS::WaitForClose();
	const bool reopened = MTGS::EnsureGsOpen();
	if (reopened)
		MTGS::ResetGS(true);

	size_t reopened_bytes = 0;
	const u8* const reopened_local = reopened ?
		MTGS::s_gs->TraceGsLocalMemoryData(&reopened_bytes) : nullptr;
	result->reopened_hash = reopened_local ?
		Pcsx2Trace::HashGsTraceBytes(reopened_local, reopened_bytes) : 0;
	result->reopened_clean = reopened && MTGS::IsOpen() &&
		reopened_bytes == CANONICAL_BYTES && result->reopened_hash == ZERO_HASH;
	if (!result->reopened_clean && !failure)
		failure = "GSState did not reopen with a clean canonical local-memory window";

	if (!content_ok || !result->reopened_clean)
	{
		Error::SetString(error, failure ? failure : "GS canonical-ring validation failed.");
		return false;
	}
	return true;
}
#endif

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
