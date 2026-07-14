// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "Gif_Unit.h"
#include "Vif_Dma.h"
#include "MTVU.h"

Gif_Unit gifUnit;

namespace
{
	bool IsCanonicalPortableGifBool(const bool& value)
	{
		static_assert(sizeof(bool) == sizeof(u8));
		u8 representation = 0;
		std::memcpy(&representation, &value, sizeof(representation));
		return representation <= 1u;
	}

	bool IsValidPortableGifUnitState()
	{
		u32 transfer_type = 0;
		static_assert(sizeof(transfer_type) == sizeof(gifUnit.lastTranType));
		std::memcpy(&transfer_type, &gifUnit.lastTranType, sizeof(transfer_type));
		const bool valid_transfer_type = transfer_type == GIF_TRANS_INVALID ||
			transfer_type == GIF_TRANS_XGKICK || transfer_type == GIF_TRANS_MTVU ||
			transfer_type == GIF_TRANS_DIRECT || transfer_type == GIF_TRANS_DIRECTHL ||
			transfer_type == GIF_TRANS_DMA || transfer_type == GIF_TRANS_FIFO;

		return IsCanonicalPortableGifBool(gifUnit.gsSIGNAL.queued) &&
			IsCanonicalPortableGifBool(gifUnit.gsFINISH.gsFINISHFired) &&
			IsCanonicalPortableGifBool(gifUnit.gsFINISH.gsFINISHPending) &&
			valid_transfer_type;
	}

	bool IsValidPortableGifTag(const Gif_Path& gif_path, u32 saved_state,
		u32 tag_nloop, u32 tag_flg, u32 tag_nreg, u8 tag_has_ad, u8 tag_is_valid)
	{
		if (tag_is_valid == 0)
			return true;

		// Gif_Tag::setTag() can remain live across an execution boundary only for
		// a PACKED tag containing A+D. All other formats are consumed atomically
		// or marked invalid before returning to the scheduler.
		if (saved_state != GIF_PATH_PACKED || tag_flg != GIF_FLG_PACKED ||
			tag_nloop == 0 || tag_has_ad != 1)
		{
			return false;
		}

		const u32 expected_nregs = ((tag_nreg - 1) & 0xf) + 1;
		if (gif_path.gifTag.nRegs != expected_nregs ||
			gif_path.gifTag.nRegIdx >= expected_nregs ||
			gif_path.gifTag.nLoop > tag_nloop ||
			(gif_path.gifTag.nLoop == 0 && gif_path.gifTag.nRegIdx != 0))
		{
			return false;
		}

		const u32 expected_len = expected_nregs * tag_nloop * 16;
		if (gif_path.gifTag.len != expected_len ||
			gif_path.gifTag.cycles != expected_len * 2)
		{
			return false;
		}

		bool expected_has_ad = false;
		for (u32 reg_index = 0; reg_index < expected_nregs; reg_index++)
		{
			const u8 reg = static_cast<u8>((gif_path.gifTag.tag.REGS[reg_index / 8] >>
				((reg_index % 8) * 4)) & 0xf);
			if (gif_path.gifTag.regs[reg_index] != reg)
				return false;
			expected_has_ad |= (reg == GIF_REG_A_D);
		}
		if (!expected_has_ad)
			return false;

		// setTag() accounts for the 16-byte tag before packedStep() can make any
		// progress. A packet containing less than that reachable prefix could
		// otherwise underflow RealignPacket() after load.
		const u64 processed_qwords =
			static_cast<u64>(tag_nloop - gif_path.gifTag.nLoop) * expected_nregs +
			gif_path.gifTag.nRegIdx;
		const u64 minimum_packet_size = (processed_qwords + 1) * 16;
		return minimum_packet_size <= gif_path.gsPack.size;
	}

	bool IsValidPortableGifPath(const Gif_Path& gif_path, u32 path,
		u32 fake_packets, u32 saved_buffer_size, u32 saved_buffer_limit,
		u32 saved_path, u32 saved_state, u32 tag_nloop, u32 tag_eop,
		u32 tag_pre, u32 tag_prim, u32 tag_flg, u32 tag_nreg,
		u8 tag_has_ad, u8 tag_is_valid)
	{
		if (fake_packets != 0 || saved_buffer_size != gif_path.buffSize ||
			saved_buffer_limit != gif_path.buffLimit || saved_path != path ||
			saved_state > GIF_PATH_WAIT || gif_path.curSize > gif_path.buffSize ||
			gif_path.curOffset > gif_path.curSize ||
			gif_path.gsPack.size > gif_path.curOffset ||
			gif_path.gsPack.offset != gif_path.curOffset - gif_path.gsPack.size ||
			gif_path.gsPack.readAmount != 0 ||
			gif_path.dmaRewind > gif_path.buffSize - gif_path.curSize ||
			(path != GIF_PATH_3 && gif_path.dmaRewind != 0) ||
			tag_nloop > 0x7fffu || tag_eop > 1u || tag_pre > 1u ||
			tag_prim > 0x7ffu || tag_flg > 3u || tag_nreg > 0xfu ||
			gif_path.gifTag.nRegs > 16u || gif_path.gifTag.nRegIdx > 15u ||
			tag_has_ad > 1u || tag_is_valid > 1u)
		{
			return false;
		}

		return IsValidPortableGifTag(gif_path, saved_state, tag_nloop,
			tag_flg, tag_nreg, tag_has_ad, tag_is_valid);
	}
}

// Returns true on stalling SIGNAL
bool Gif_HandlerAD(u8* pMem)
{
	u32 reg = pMem[8];
	u32* data = (u32*)pMem;
	if (reg >= GIF_A_D_REG_BITBLTBUF && reg <= GIF_A_D_REG_TRXREG)
	{
		vif1.transfer_registers[reg - GIF_A_D_REG_BITBLTBUF] = *(u64*)pMem;
	}
	else if (reg == GIF_A_D_REG_TRXDIR)
	{ // TRXDIR
		if ((pMem[0] & 3) == 1)
		{                // local -> host
			u8 bpp = 32; // Onimusha does TRXDIR without BLTDIVIDE first, assume 32bit
			switch (vif1.BITBLTBUF.SPSM & 7)
			{
				case 0:
					bpp = 32;
					break;
				case 1:
					bpp = 24;
					break;
				case 2:
					bpp = 16;
					break;
				case 3:
					bpp = 8;
					break;
				default: // 4 is 4 bit but this is forbidden
					Console.Error("Illegal format for GS upload: SPSM=0%02o", vif1.BITBLTBUF.SPSM);
					break;
			}
			// qwords, rounded down; any extra bits are lost
			// games must take care to ensure transfer rectangles are exact multiples of a qword
			vif1.GSLastDownloadSize = vif1.TRXREG.RRW * vif1.TRXREG.RRH * bpp >> 7;
		}
	}
	else if (reg == GIF_A_D_REG_SIGNAL)
	{ // SIGNAL
		if (CSRreg.SIGNAL)
		{ // Time to ignore all subsequent drawing operations.
			GUNIT_WARN(Color_Orange, "GIF Handler - Stalling SIGNAL");
			if (!gifUnit.gsSIGNAL.queued)
			{
				gifUnit.gsSIGNAL.queued = true;
				gifUnit.gsSIGNAL.data[0] = data[0];
				gifUnit.gsSIGNAL.data[1] = data[1];
				return true; // Stalling SIGNAL
			}
		}
		else
		{
			GUNIT_WARN("GIF Handler - SIGNAL");
			GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~data[1]) | (data[0] & data[1]);
			if (!GSIMR.SIGMSK)
				gsIrq();
			CSRreg.SIGNAL = true;
		}
	}
	else if (reg == GIF_A_D_REG_FINISH)
	{ // FINISH
		GUNIT_WARN("GIF Handler - FINISH");
		gifUnit.gsFINISH.gsFINISHFired = false;
		gifUnit.gsFINISH.gsFINISHPending = true;
	}
	else if (reg == GIF_A_D_REG_LABEL)
	{ // LABEL
		GUNIT_WARN("GIF Handler - LABEL");
		GSSIGLBLID.LBLID = (GSSIGLBLID.LBLID & ~data[1]) | (data[0] & data[1]);
	}
	else if (reg >= 0x63 && reg != 0x7f)
	{
		//DevCon.Warning("GIF Handler - Write to unknown register! [reg=%x]", reg);
	}
	return false;
}

void Gif_HandlerAD_MTVU(u8* pMem)
{
	// Note: Atomic communication is with MTVU.cpp Get_GSChanges
	const u8 reg = pMem[8] & 0x7f;
	const u32* data = (u32*)pMem;

	if (reg == GIF_A_D_REG_SIGNAL)
	{ // SIGNAL
		GUNIT_WARN("GIF Handler - SIGNAL");
		if (vu1Thread.mtvuInterrupts.load(std::memory_order_acquire) & VU_Thread::InterruptFlagSignal)
			Console.Error("GIF Handler MTVU - Double SIGNAL Not Handled");
		vu1Thread.gsSignal.store(((u64)data[1] << 32) | data[0], std::memory_order_relaxed);
		vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagSignal, std::memory_order_release);
	}
	else if (reg == GIF_A_D_REG_FINISH)
	{ // FINISH
		GUNIT_WARN("GIF Handler - FINISH");
		u32 old = vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagFinish, std::memory_order_relaxed);
		if (old & VU_Thread::InterruptFlagFinish)
			Console.Error("GIF Handler MTVU - Double FINISH Not Handled");
	}
	else if (reg == GIF_A_D_REG_LABEL)
	{ // LABEL
		GUNIT_WARN("GIF Handler - LABEL");
		// It's okay to coalesce label updates
		u32 labelData = data[0];
		u32 labelMsk = data[1];
		u64 existing = 0;
		u64 wanted = ((u64)labelMsk << 32) | labelData;
		while (!vu1Thread.gsLabel.compare_exchange_weak(existing, wanted, std::memory_order_relaxed))
		{
			u32 existingData = (u32)existing;
			u32 existingMsk = (u32)(existing >> 32);
			u32 wantedData = (existingData & ~labelMsk) | (labelData & labelMsk);
			u32 wantedMsk = existingMsk | labelMsk;
			wanted = ((u64)wantedMsk << 32) | wantedData;
		}
		vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagLabel, std::memory_order_release);
	}
	else if (reg >= 0x63 && reg != 0x7f)
	{
		DevCon.Warning("GIF Handler Debug - Write to unknown register! [reg=%x]", reg);
	}
}

// Returns true if pcsx2 needed to process the packet...
bool Gif_HandlerAD_Debug(u8* pMem)
{
	const u8 reg = pMem[8] & 0x7f;
	if (reg == 0x50)
	{
		Console.Error("GIF Handler Debug - BITBLTBUF");
		return 1;
	}
	else if (reg == 0x52)
	{
		Console.Error("GIF Handler Debug - TRXREG");
		return 1;
	}
	else if (reg == 0x53)
	{
		Console.Error("GIF Handler Debug - TRXDIR");
		return 1;
	}
	else if (reg == 0x60)
	{
		Console.Error("GIF Handler Debug - SIGNAL");
		return 1;
	}
	else if (reg == 0x61)
	{
		Console.Error("GIF Handler Debug - FINISH");
		return 1;
	}
	else if (reg == 0x62)
	{
		Console.Error("GIF Handler Debug - LABEL");
		return 1;
	}
	else if (reg >= 0x63 && reg != 0x7f)
	{
		DevCon.Warning("GIF Handler Debug - Write to unknown register! [reg=%x]", reg);
	}
	return 0;
}

void Gif_FinishIRQ()
{
	if (gifUnit.gsFINISH.gsFINISHPending)
	{
		CSRreg.FINISH = true;
		gifUnit.gsFINISH.gsFINISHPending = false;
	}
	if (CSRreg.FINISH && !GSIMR.FINISHMSK && !gifUnit.gsFINISH.gsFINISHFired)
	{
		gsIrq();
		gifUnit.gsFINISH.gsFINISHFired = true;
	}
}

bool SaveStateBase::gifPathFreeze(u32 path)
{

	Gif_Path& gifPath = gifUnit.gifPath[path];
	if (IsPortableReplay() && IsSaving() &&
		(gifPath.readAmount.load(std::memory_order_acquire) != 0 ||
			gifPath.GetPendingGSPackets() != 0 ||
			!IsValidPortableGifPath(gifPath, path, gifPath.mtvu.fakePackets,
				gifPath.buffSize, gifPath.buffLimit, static_cast<u32>(gifPath.idx),
				static_cast<u32>(gifPath.state), gifPath.gifTag.tag.NLOOP,
				gifPath.gifTag.tag.EOP, gifPath.gifTag.tag.PRE,
				gifPath.gifTag.tag.PRIM, gifPath.gifTag.tag.FLG,
				gifPath.gifTag.tag.NREG, gifPath.gifTag.hasAD ? 1u : 0u,
				gifPath.gifTag.isValid ? 1u : 0u)))
	{
		Console.Error("Portable GIF replay capture found an invalid path continuation.");
		m_error = true;
		return false;
	}
	pxAssertMsg(!gifPath.readAmount, "Gif Path readAmount should be 0!");
	pxAssertMsg(!gifPath.gsPack.readAmount, "GS Pack readAmount should be 0!");
	pxAssertMsg(!gifPath.GetPendingGSPackets(), "MTVU GS Pack Queue should be 0!");

	if (!gifPath.isMTVU())
	{ // FixMe: savestate freeze bug (Gust games) with MTVU enabled
		if (IsSaving())
		{                            // Move all the buffered data to the start of buffer
			gifPath.RealignPacket(); // May add readAmount which we need to clear on load
		}
	}
	u8* bufferPtr = gifPath.buffer; // Backup current buffer ptr
	if (IsPortableReplay())
	{
		// PCSX2's native .p2s path copies Gif_Path up to its MTVU tail. That
		// prefix contains an atomic and a host pointer and is 120 bytes on
		// x86-64 versus 108 on AArch32. Keep the same owner and packet realignment,
		// but serialize only logical continuation state for validation replay.
		if (!FreezeTag("Gif_Path-portable-v1"))
			return false;

		u32 fake_packets = gifPath.mtvu.fakePackets;
		u32 saved_buffer_size = gifPath.buffSize;
		u32 saved_buffer_limit = gifPath.buffLimit;
		u32 saved_path = static_cast<u32>(gifPath.idx);
		u32 saved_state = static_cast<u32>(gifPath.state);
		u32 tag_nloop = gifPath.gifTag.tag.NLOOP;
		u32 tag_eop = gifPath.gifTag.tag.EOP;
		u32 tag_pre = gifPath.gifTag.tag.PRE;
		u32 tag_prim = gifPath.gifTag.tag.PRIM;
		u32 tag_flg = gifPath.gifTag.tag.FLG;
		u32 tag_nreg = gifPath.gifTag.tag.NREG;
		u8 tag_has_ad = gifPath.gifTag.hasAD ? 1u : 0u;
		u8 tag_is_valid = gifPath.gifTag.isValid ? 1u : 0u;

		Freeze(fake_packets);
		Freeze(saved_buffer_size);
		Freeze(saved_buffer_limit);
		Freeze(gifPath.curSize);
		Freeze(gifPath.curOffset);
		Freeze(gifPath.dmaRewind);
		Freeze(tag_nloop);
		Freeze(tag_eop);
		Freeze(tag_pre);
		Freeze(tag_prim);
		Freeze(tag_flg);
		Freeze(tag_nreg);
		Freeze(gifPath.gifTag.tag.REGS);
		Freeze(gifPath.gifTag.nLoop);
		Freeze(gifPath.gifTag.nRegs);
		Freeze(gifPath.gifTag.nRegIdx);
		Freeze(gifPath.gifTag.len);
		Freeze(gifPath.gifTag.cycles);
		Freeze(gifPath.gifTag.regs);
		Freeze(tag_has_ad);
		Freeze(tag_is_valid);
		Freeze(gifPath.gsPack.offset);
		Freeze(gifPath.gsPack.size);
		Freeze(gifPath.gsPack.cycles);
		Freeze(gifPath.gsPack.readAmount);
		Freeze(saved_path);
		Freeze(saved_state);

		if (!IsOkay())
			return false;
		if (!IsValidPortableGifPath(gifPath, path, fake_packets,
				saved_buffer_size, saved_buffer_limit, saved_path, saved_state,
				tag_nloop, tag_eop, tag_pre, tag_prim, tag_flg, tag_nreg,
				tag_has_ad, tag_is_valid))
		{
			Console.Error("Portable GIF replay state contains invalid path continuation.");
			m_error = true;
			return false;
		}

		FreezeMem(bufferPtr, gifPath.curSize);
		gifPath.buffer = bufferPtr;
		if (!IsSaving())
		{
			gifPath.readAmount = 0;
			gifPath.gsPack.readAmount = 0;
			gifPath.idx = static_cast<GIF_PATH>(saved_path);
			gifPath.state = static_cast<GIF_PATH_STATE>(saved_state);
			gifPath.gifTag.tag.NLOOP = tag_nloop;
			gifPath.gifTag.tag.EOP = tag_eop;
			gifPath.gifTag.tag._dummy0 = 0;
			gifPath.gifTag.tag._dummy1 = 0;
			gifPath.gifTag.tag.PRE = tag_pre;
			gifPath.gifTag.tag.PRIM = tag_prim;
			gifPath.gifTag.tag.FLG = tag_flg;
			gifPath.gifTag.tag.NREG = tag_nreg;
			gifPath.gifTag.hasAD = tag_has_ad != 0;
			gifPath.gifTag.isValid = tag_is_valid != 0;
			gifPath.mtvu.Reset();
		}
		return IsOkay();
	}

	Freeze(gifPath.mtvu.fakePackets);
	FreezeMem(&gifPath, sizeof(gifPath) - sizeof(gifPath.mtvu));
	FreezeMem(bufferPtr, gifPath.curSize);
	gifPath.buffer = bufferPtr;
	if (!IsSaving())
	{
		gifPath.readAmount = 0;
		gifPath.gsPack.readAmount = 0;
	}

	return IsOkay();
}

bool SaveStateBase::gifFreeze()
{
	bool mtvuMode = THREAD_VU1;
	pxAssert(vu1Thread.IsDone());
	MTGS::WaitGS();
	if (IsPortableReplay() && IsSaving() && !IsValidPortableGifUnitState())
	{
		Console.Error("Portable GIF replay capture found invalid signal or transfer state.");
		m_error = true;
		return false;
	}
	if (!FreezeTag("Gif Unit"))
		return false;

	Freeze(mtvuMode);
	if (IsPortableReplay() && (!IsCanonicalPortableGifBool(mtvuMode) || mtvuMode))
	{
		Console.Error("Portable GIF replay requires canonical disabled MTVU state.");
		m_error = true;
		return false;
	}
	Freeze(gifUnit.stat);
	Freeze(gifUnit.gsSIGNAL);
	Freeze(gifUnit.gsFINISH);
	Freeze(gifUnit.lastTranType);
	if (IsPortableReplay() && (!IsOkay() || !IsValidPortableGifUnitState()))
	{
		Console.Error("Portable GIF replay state contains invalid signal or transfer state.");
		m_error = true;
		return false;
	}
	if (!gifPathFreeze(GIF_PATH_1) || !gifPathFreeze(GIF_PATH_2) ||
		!gifPathFreeze(GIF_PATH_3))
	{
		return false;
	}
	if (!IsSaving())
	{
		if (mtvuMode != THREAD_VU1)
		{
			DevCon.Warning("gifUnit: MTVU Mode has switched between save/load state");
			// ToDo: gifUnit.SwitchMTVU(mtvuMode);
		}
	}

	return IsOkay();
}
