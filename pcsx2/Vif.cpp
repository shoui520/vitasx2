// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "GS.h"
#include "Gif.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"

#include <type_traits>

alignas(16) vifStruct vif0, vif1;

static bool PortableBoolIsCanonical(const bool& value)
{
	static_assert(sizeof(bool) == sizeof(u8));
	u8 representation = 0;
	std::memcpy(&representation, &value, sizeof(representation));
	return representation <= 1u;
}

static bool IsActiveVifUnpack(const vifStruct& vif)
{
	const u32 command = static_cast<u32>(vif.cmd) & 0x7fu;
	return command >= 0x60u && command <= 0x7fu && vif.pass != 0;
}

static bool ValidatePortableVifContinuation(const vifStruct& vif, const nVifStruct& nvif, u32 index)
{
	if (!PortableBoolIsCanonical(vif.done) ||
		!PortableBoolIsCanonical(vif.vifstalled.enabled) ||
		!PortableBoolIsCanonical(vif.stallontag) ||
		!PortableBoolIsCanonical(vif.waitforvu) ||
		!PortableBoolIsCanonical(vif.irqoffset.enabled) ||
		!PortableBoolIsCanonical(vif.queued_program) ||
		!PortableBoolIsCanonical(vif.queued_gif_wait) ||
		vif.cmd < 0 || vif.cmd > 0xff || vif.pass < 0 || vif.pass > 1 ||
		vif.irq < 0 || vif.irq > 1 || vif.usn > 1u || vif.start_aligned > 4u ||
		vif.tag.cmd > 0xffu || vif.vifstalled.value > VIF_IRQ_STALL ||
		vif.irqoffset.value >= 4u || (vif.inprogress & ~0x11u) != 0 ||
		vif.dmamode > VIF_CHAIN_MODE || nvif.bSize > sizeof(nvif.buffer) ||
		(nvif.bSize & 3u) != 0)
	{
		return false;
	}

	if (vif.queued_program && vif.queued_pc != ~0u &&
		vif.queued_pc > (index ? 0x7ffu : 0x1ffu))
	{
		return false;
	}

	if (vif.cmd == 0)
		return vif.pass == 0 && nvif.bSize == 0;
	if (vif.pass == 0)
		return nvif.bSize == 0;

	const u32 command = static_cast<u32>(vif.cmd) & 0x7fu;
	switch (command)
	{
		case 0x20: // Vif_Codes.cpp::vifCode_STMask().
			return vif.tag.size == 1u && nvif.bSize == 0;

		case 0x30: // Vif_Codes.cpp::vifCode_STRow().
		case 0x31: // Vif_Codes.cpp::vifCode_STCol().
			return vif.tag.addr < 4u && vif.tag.size <= 4u &&
				vif.tag.addr + vif.tag.size == 4u && nvif.bSize == 0;

		case 0x4a: // Vif_Codes.cpp::vifCode_MPG().
			return vif.tag.addr <= (index ? VU1_PROGSIZE : VU0_PROGSIZE) &&
				(vif.tag.addr & 3u) == 0 && vif.tag.size > 0 &&
				vif.tag.size <= 512u && nvif.bSize == 0;

		case 0x50: // Vif_Codes.cpp::vifCode_Direct().
		case 0x51: // Vif_Codes.cpp::vifCode_DirectHL().
			return index == 1u && vif.tag.size > 0 && vif.tag.size <= 65536u * 4u &&
				nvif.bSize == 0;

		default:
			break;
	}

	if (command >= 0x60u)
	{
		const u32 unpack_type = command & 0x0fu;
		const bool valid_unpack_type = unpack_type != 0x03u && unpack_type != 0x07u &&
			unpack_type != 0x0bu;
		const u32 memory_size = index ? VU1_MEMSIZE : VU0_MEMSIZE;
		return valid_unpack_type && vif.tag.addr < memory_size &&
			(vif.tag.addr & 0xfu) == 0 && vif.tag.size <= 1024u &&
			static_cast<u64>(nvif.bSize) + static_cast<u64>(vif.tag.size) * 4u <=
				sizeof(nvif.buffer) && vif.start_aligned >= 1u;
	}

	return nvif.bSize == 0;
}

static bool ValidatePortableVifRegistersForUnit(const vifStruct& vif,
	const nVifStruct& nvif, const VIFregisters& regs)
{
	if (regs.mode > 3u || regs.num > 256u)
		return false;

	if (vif.cmd == 0 || vif.pass == 0 || (static_cast<u32>(vif.cmd) & 0x7fu) < 0x60u)
		return true;

	const u32 command_byte = static_cast<u32>(vif.cmd);
	const u32 unpack_type = command_byte & 0x0fu;
	const u32 vector_bytes = nVifT[unpack_type];
	const u32 original_num_byte = (regs.code >> 16) & 0xffu;
	const u32 original_num = original_num_byte != 0 ? original_num_byte : 256u;
	const u32 write_length = regs.cycle.wl != 0 ? regs.cycle.wl : 256u;
	u32 source_vectors = original_num;
	if (write_length > regs.cycle.cl)
	{
		source_vectors = static_cast<u32>(regs.cycle.cl) * (original_num / write_length) +
			std::min(original_num % write_length, static_cast<u32>(regs.cycle.cl));
	}
	const u32 expected_words = (source_vectors * vector_bytes + 3u) / 4u;

	return vector_bytes != 0 && expected_words != 0 && vif.tag.size != 0 &&
		((regs.code >> 24) & 0xffu) == command_byte && vif.tag.cmd == command_byte &&
		vif.usn == ((regs.code >> 14) & 1u) && regs.num != 0 && regs.num <= original_num &&
		(nvif.bSize != 0 || regs.num == original_num) &&
		static_cast<u64>(nvif.bSize) + static_cast<u64>(vif.tag.size) * 4u ==
			static_cast<u64>(expected_words) * 4u;
}

bool vifValidatePortableRegisters()
{
	return ValidatePortableVifRegistersForUnit(vif0, nVif[0], vif0Regs) &&
		ValidatePortableVifRegistersForUnit(vif1, nVif[1], vif1Regs);
}

void vif0Reset()
{
	/* Reset the whole VIF, meaning the internal pcsx2 vars and all the registers */
	std::memset(&vif0, 0, sizeof(vif0));
	std::memset(&vif0Regs, 0, sizeof(vif0Regs));

	resetNewVif(0);
}

void vif1Reset()
{
	/* Reset the whole VIF, meaning the internal pcsx2 vars, and all the registers */
	std::memset(&vif1, 0, sizeof(vif1));
	std::memset(&vif1Regs, 0, sizeof(vif1Regs));

	resetNewVif(1);
}

bool SaveStateBase::vif0Freeze()
{
	if (!FreezeTag("VIF0dma"))
		return false;

	Freeze(g_vif0Cycles);

	if (IsPortableReplay())
	{
		// Vif_Dynarec.cpp::dVifUnpack() and VitaVifInterpreter.cpp leave
		// different cycle-position residue after a completed UNPACK. The next
		// vifUnpackSetup() resets it, exactly matching HashVifState's boundary.
		static_assert(std::is_trivially_copyable_v<vifStruct>);
		vifStruct portable_vif;
		std::memcpy(&portable_vif, &vif0, sizeof(portable_vif));
		if (IsSaving() && !IsActiveVifUnpack(portable_vif))
			portable_vif.cl = 0;
		Freeze(portable_vif);
		if (IsLoading())
		{
			if (!IsActiveVifUnpack(portable_vif) && portable_vif.cl != 0)
			{
				Console.Error("Portable replay VIF0 has non-canonical completed-UNPACK residue.");
				m_error = true;
				return false;
			}
			std::memcpy(&vif0, &portable_vif, sizeof(vif0));
		}
	}
	else
	{
		Freeze(vif0);
	}

	Freeze(nVif[0].bSize);
	if (IsPortableReplay() && !ValidatePortableVifContinuation(vif0, nVif[0], 0))
	{
		Console.Error("Portable replay VIF0 continuation is invalid.");
		m_error = true;
		return false;
	}
	if (IsPortableReplay() && IsSaving() &&
		!ValidatePortableVifRegistersForUnit(vif0, nVif[0], vif0Regs))
	{
		Console.Error("Portable replay VIF0 hardware-register continuation is invalid.");
		m_error = true;
		return false;
	}
	FreezeMem(nVif[0].buffer, nVif[0].bSize);

	return IsOkay();
}

bool SaveStateBase::vif1Freeze()
{
	if (!FreezeTag("VIF1dma"))
		return false;

	Freeze(g_vif1Cycles);

	if (IsPortableReplay())
	{
		static_assert(std::is_trivially_copyable_v<vifStruct>);
		vifStruct portable_vif;
		std::memcpy(&portable_vif, &vif1, sizeof(portable_vif));
		if (IsSaving() && !IsActiveVifUnpack(portable_vif))
			portable_vif.cl = 0;
		Freeze(portable_vif);
		if (IsLoading())
		{
			if (!IsActiveVifUnpack(portable_vif) && portable_vif.cl != 0)
			{
				Console.Error("Portable replay VIF1 has non-canonical completed-UNPACK residue.");
				m_error = true;
				return false;
			}
			std::memcpy(&vif1, &portable_vif, sizeof(vif1));
		}
	}
	else
	{
		Freeze(vif1);
	}

	Freeze(nVif[1].bSize);
	if (IsPortableReplay() && !ValidatePortableVifContinuation(vif1, nVif[1], 1))
	{
		Console.Error("Portable replay VIF1 continuation is invalid.");
		m_error = true;
		return false;
	}
	if (IsPortableReplay() && IsSaving() &&
		!ValidatePortableVifRegistersForUnit(vif1, nVif[1], vif1Regs))
	{
		Console.Error("Portable replay VIF1 hardware-register continuation is invalid.");
		m_error = true;
		return false;
	}
	FreezeMem(nVif[1].buffer, nVif[1].bSize);

	return IsOkay();
}

//------------------------------------------------------------------
// Vif0/Vif1 Write32
//------------------------------------------------------------------

__fi void vif0FBRST(u32 value)
{
	VIF_LOG("VIF0_FBRST write32 0x%8.8x", value);
	/* Fixme: Forcebreaks are pretty unknown for operation, presumption is it just stops it what its doing
			  usually accompanied by a reset, but if we find a broken game which falls here, we need to see it! (Refraction) */
	if (value & 0x2) // Forcebreak Vif,
	{
		/* I guess we should stop the VIF dma here, but not 100% sure (linuz) */
		cpuRegs.interrupt &= ~1; //Stop all vif0 DMA's
		vif0Regs.stat.VFS = true;
		vif0Regs.stat.VPS = VPS_IDLE;
		Console.WriteLn("vif0 force break");
	}

	if (value & 0x4) // Stop Vif.
	{
		// Not completely sure about this, can't remember what game, used this, but 'draining' the VIF helped it, instead of
		//  just stoppin the VIF (linuz).
		vif0Regs.stat.VSS = true;
		vif0Regs.stat.VPS = VPS_IDLE;
		vif0.vifstalled.enabled = VifStallEnable(vif0ch);
		vif0.vifstalled.value = VIF_IRQ_STALL;
	}

	if (value & 0x8) // Cancel Vif Stall.
	{
		bool cancel = false;

		/* Cancel stall, first check if there is a stall to cancel, and then clear VIF0_STAT VSS|VFS|VIS|INT|ER0|ER1 bits */
		if (vif0Regs.stat.test(VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS))
			cancel = true;

		vif0Regs.stat.clear_flags(VIF0_STAT_VSS | VIF0_STAT_VFS | VIF0_STAT_VIS |
								  VIF0_STAT_INT | VIF0_STAT_ER0 | VIF0_STAT_ER1);
		if (cancel)
		{
			g_vif0Cycles = 0;
			// loop necessary for spiderman
			if (vif0ch.chcr.STR)
				CPU_INT(DMAC_VIF0, 0); // Gets the timing right - Flatout
		}
	}

	if (value & 0x1) // Reset Vif.
	{
		//Console.WriteLn("Vif0 Reset %x", vif0Regs.stat._u32);
		u128 SaveCol;
		u128 SaveRow;

		//	if(vif0ch.chcr.STR) DevCon.Warning("FBRST While Vif0 active");
		//Must Preserve Row/Col registers! (Downhill Domination for testing)
		SaveCol._u64[0] = vif0.MaskCol._u64[0];
		SaveCol._u64[1] = vif0.MaskCol._u64[1];
		SaveRow._u64[0] = vif0.MaskRow._u64[0];
		SaveRow._u64[1] = vif0.MaskRow._u64[1];
		std::memset(&vif0, 0, sizeof(vif0));
		vif0.MaskCol._u64[0] = SaveCol._u64[0];
		vif0.MaskCol._u64[1] = SaveCol._u64[1];
		vif0.MaskRow._u64[0] = SaveRow._u64[0];
		vif0.MaskRow._u64[1] = SaveRow._u64[1];
		vif0ch.qwc = 0; //?
		cpuRegs.interrupt &= ~1; //Stop all vif0 DMA's
		psHu64(VIF0_FIFO) = 0;
		psHu64(VIF0_FIFO + 8) = 0;
		vif0.vifstalled.enabled = false;
		vif0.irqoffset.enabled = false;
		vif0.inprogress = 0;
		vif0.cmd = 0;
		vif0.done = true;
		vif0ch.chcr.STR = false;
		vif0Regs.err.reset();
		vif0Regs.stat.clear_flags(VIF0_STAT_FQC | VIF0_STAT_INT | VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS | VIF0_STAT_VPS); // FQC=0
	}
}

__fi void vif1FBRST(u32 value)
{
	VIF_LOG("VIF1_FBRST write32 0x%8.8x", value);

	/* Fixme: Forcebreaks are pretty unknown for operation, presumption is it just stops it what its doing
			  usually accompanied by a reset, but if we find a broken game which falls here, we need to see it! (Refraction) */

	if (FBRST(value).FBK) // Forcebreak Vif.
	{
		/* I guess we should stop the VIF dma here, but not 100% sure (linuz) */
		vif1Regs.stat.VFS = true;
		vif1Regs.stat.VPS = VPS_IDLE;
		cpuRegs.interrupt &= ~((1 << 1) | (1 << 10)); //Stop all vif1 DMA's
		vif1.vifstalled.enabled = VifStallEnable(vif1ch);
		vif1.vifstalled.value = VIF_IRQ_STALL;
		Console.WriteLn("vif1 force break");
	}

	if (FBRST(value).STP) // Stop Vif.
	{
		// Not completely sure about this, can't remember what game used this, but 'draining' the VIF helped it, instead of
		// just stoppin the VIF (linuz).
		vif1Regs.stat.VSS = true;
		vif1Regs.stat.VPS = VPS_IDLE;
		vif1.vifstalled.enabled = VifStallEnable(vif1ch);
		vif1.vifstalled.value = VIF_IRQ_STALL;
	}

	if (FBRST(value).STC) // Cancel Vif Stall.
	{
		bool cancel = false;
		//DevCon.Warning("Cancel stall. Stat = %x", vif1Regs.stat._u32);
		// Cancel stall, first check if there is a stall to cancel, and then clear VIF1_STAT VSS|VFS|VIS|INT|ER0|ER1 bits
		if (vif1Regs.stat.test(VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		{
			cancel = true;
		}

		vif1Regs.stat.clear_flags(VIF1_STAT_VSS | VIF1_STAT_VFS | VIF1_STAT_VIS |
								  VIF1_STAT_INT | VIF1_STAT_ER0 | VIF1_STAT_ER1);

		if (cancel)
		{
			g_vif1Cycles = 0;
			// loop necessary for spiderman
			switch (dmacRegs.ctrl.MFD)
			{
			case MFD_VIF1:
				//Console.WriteLn("MFIFO Stall");
				//MFIFO active and not empty
				if (vif1ch.chcr.STR && !vif1Regs.stat.test(VIF1_STAT_FDR))
					CPU_INT(DMAC_MFIFO_VIF, 0);
				break;

			case NO_MFD:
			case MFD_RESERVED:
			case MFD_GIF: // Wonder if this should be with VIF?
				// Gets the timing right - Flatout
				if (vif1ch.chcr.STR && !vif1Regs.stat.test(VIF1_STAT_FDR))
					CPU_INT(DMAC_VIF1, 0);
				break;
			}

			//vif1ch.chcr.STR = true;
		}
	}

	if (FBRST(value).RST) // Reset Vif.
	{
		u128 SaveCol;
		u128 SaveRow;
		//if(vif1ch.chcr.STR) DevCon.Warning("FBRST While Vif1 active");
		//Must Preserve Row/Col registers! (Downhill Domination for testing) - Really shouldnt be part of the vifstruct.
		SaveCol._u64[0] = vif1.MaskCol._u64[0];
		SaveCol._u64[1] = vif1.MaskCol._u64[1];
		SaveRow._u64[0] = vif1.MaskRow._u64[0];
		SaveRow._u64[1] = vif1.MaskRow._u64[1];
		u8 mfifo_empty = vif1.inprogress & 0x10;
		std::memset(&vif1, 0, sizeof(vif1));
		vif1.MaskCol._u64[0] = SaveCol._u64[0];
		vif1.MaskCol._u64[1] = SaveCol._u64[1];
		vif1.MaskRow._u64[0] = SaveRow._u64[0];
		vif1.MaskRow._u64[1] = SaveRow._u64[1];


		GUNIT_WARN(Color_Red, "VIF FBRST Reset MSK = %x", vif1Regs.mskpath3);
		vif1Regs.mskpath3 = false;
		gifRegs.stat.M3P = 0;
		vif1Regs.err.reset();
		vif1.inprogress = mfifo_empty;
		vif1.cmd = 0;
		vif1.vifstalled.enabled = false;
		vif1Regs.stat._u32 = 0;
	}
}

__fi void vif1STAT(u32 value)
{
	VIF_LOG("VIF1_STAT write32 0x%8.8x", value);

	/* Only FDR bit is writable, so mask the rest */
	if ((vif1Regs.stat.FDR) ^ ((tVIF_STAT&)value).FDR)
	{
		bool isStalled = false;
		// different so can't be stalled
		if (vif1Regs.stat.test(VIF1_STAT_INT | VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		{
			DbgCon.WriteLn("changing dir when vif1 fifo stalled done = %x qwc = %x stat = %x", vif1.done, vif1ch.qwc, vif1Regs.stat._u32);
			isStalled = true;
		}

		//Hack!! Hotwheels seems to leave 1QW in the fifo and expect the DMA to be ready for a reverse FIFO
		//There's no important data in there so for it to work, we will just end it.
		//Hotwheels had this in the "direction when stalled" area, however Sled Storm seems to keep an eye on the dma
		//position, as we clear it and set it to the end well before the interrupt, the game assumes it's finished,
		//then proceeds to reverse the dma before we have even done it ourselves. So lets just make sure VIF is ready :)
		if (vif1ch.qwc > 0 || isStalled == false)
		{
			if (vif1ch.chcr.STR)
			{
				vif1ch.qwc = 0;
				hwDmacIrq(DMAC_VIF1);
				vif1ch.chcr.STR = false;
			}
			cpuRegs.interrupt &= ~((1 << DMAC_VIF1) | (1 << DMAC_MFIFO_VIF));
		}
		//This is actually more important for our handling, else the DMA for reverse fifo doesnt start properly.
	}

	vif1Regs.stat.FDR = VIF_STAT(value).FDR;

	if (vif1Regs.stat.FDR) // Vif transferring to memory.
	{
		// Hack but it checks this is true before transfer? (fatal frame)
		// Update Refraction: Use of this function has been investigated and understood.
		// Before this ever happens, a DIRECT/HL command takes place sending the transfer info to the GS
		// One of the registers told about this is TRXREG which tells us how much data is going to transfer (th x tw) in words
		// As far as the GS is concerned, the transfer starts as soon as TRXDIR is accessed, which is why fatal frame
		// was expecting data, the GS should already be sending it over (buffering in the FIFO)

		vif1Regs.stat.FQC = std::min((u32)16, vif1.GSLastDownloadSize);
		//Console.Warning("Reversing VIF Transfer for %x QWC", vif1.GSLastDownloadSize);
	}
	else // Memory transferring to Vif.
	{
		//Sometimes the value from the GS is bigger than vif wanted, so it just sets it back and cancels it.
		//Other times it can read it off ;)
		vif1Regs.stat.FQC = 0;
		if (vif1ch.chcr.STR)
			CPU_INT(DMAC_VIF1, 0);
	}
}

#define caseVif(x) (idx ? VIF1_##x : VIF0_##x)

_vifT __fi u32 vifRead32(u32 mem)
{
	vifStruct& vif = MTVU_VifX;
	bool wait = idx && THREAD_VU1;

	switch (mem)
	{
		case caseVif(ROW0):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskRow._u32[0];
		case caseVif(ROW1):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskRow._u32[1];
		case caseVif(ROW2):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskRow._u32[2];
		case caseVif(ROW3):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskRow._u32[3];

		case caseVif(COL0):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskCol._u32[0];
		case caseVif(COL1):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskCol._u32[1];
		case caseVif(COL2):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskCol._u32[2];
		case caseVif(COL3):
			if (wait)
				vu1Thread.WaitVU();
			return vif.MaskCol._u32[3];
	}

	return psHu32(mem);
}

// returns FALSE if no writeback is needed (or writeback is handled internally)
// returns TRUE if the caller should writeback the value to the eeHw register map.
_vifT __fi bool vifWrite32(u32 mem, u32 value)
{
	vifStruct& vif = GetVifX;

	switch (mem)
	{
		case caseVif(MARK):
			VIF_LOG("VIF%d_MARK write32 0x%8.8x", idx, value);
			vifXRegs.stat.MRK = false;
			//vifXRegs.mark	   = value;
			break;

		case caseVif(FBRST):
			if (!idx)
				vif0FBRST(value);
			else
				vif1FBRST(value);
			return false;

		case caseVif(STAT):
			if (idx)
			{ // Only Vif1 does this stuff?
				vif1STAT(value);
			}
			return false;

		case caseVif(ERR):
		case caseVif(MODE):
			// standard register writes -- handled by caller.
			break;

		case caseVif(ROW0):
			vif.MaskRow._u32[0] = value;
			vu1Thread.WriteRow(vif);
			return false;
		case caseVif(ROW1):
			vif.MaskRow._u32[1] = value;
			vu1Thread.WriteRow(vif);
			return false;
		case caseVif(ROW2):
			vif.MaskRow._u32[2] = value;
			vu1Thread.WriteRow(vif);
			return false;
		case caseVif(ROW3):
			vif.MaskRow._u32[3] = value;
			vu1Thread.WriteRow(vif);
			return false;

		case caseVif(COL0):
			vif.MaskCol._u32[0] = value;
			vu1Thread.WriteCol(vif);
			return false;
		case caseVif(COL1):
			vif.MaskCol._u32[1] = value;
			vu1Thread.WriteCol(vif);
			return false;
		case caseVif(COL2):
			vif.MaskCol._u32[2] = value;
			vu1Thread.WriteCol(vif);
			return false;
		case caseVif(COL3):
			vif.MaskCol._u32[3] = value;
			vu1Thread.WriteCol(vif);
			return false;
	}

	// fall-through case: issue standard writeback behavior.
	return true;
}

template u32 vifRead32<0>(u32 mem);
template u32 vifRead32<1>(u32 mem);

template bool vifWrite32<0>(u32 mem, u32 value);
template bool vifWrite32<1>(u32 mem, u32 value);
