// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SIO/Sio.h"

#include "SIO/SioTypes.h"
#include "SIO/Memcard/MemoryCardProtocol.h"
#include "Counters.h"

#include "StateWrapper.h"

#include "common/Console.h"
#include "Host.h"
#include "IconsPromptFont.h"

#include <algorithm>
#include <atomic>
#include <utility>

_mcd mcds[2][4];
_mcd *mcd;

namespace
{
	// MemoryCardFile.cpp::MC2_ERASE_SIZE is 528 * 16 = 8448 bytes. The protocol's
	// dynamic legacy buffer is currently unused, but keep a generous bound above
	// the concrete owner maximum while preventing arbitrary replay allocations.
	static constexpr u32 PORTABLE_MCD_BUFFER_LIMIT = 64 * 1024;
	static constexpr u32 PORTABLE_MCD_MAX_AUTO_EJECT_TICKS = 60;
	static constexpr u32 PS1_MEMORY_CARD_SECTORS = 0x400;
	static constexpr u32 PS1_MEMORY_CARD_SECTOR_SIZE = 128;

	bool PortableMemoryCardAddressIsValid(_mcd& card, u32 sector_address,
		u32 transfer_address)
	{
		if (!card.IsPresent())
			return true;

		if (card.IsPSX())
		{
			if (sector_address >= PS1_MEMORY_CARD_SECTORS)
				return false;
			const u32 sector_begin = sector_address * PS1_MEMORY_CARD_SECTOR_SIZE;
			return transfer_address >= sector_begin &&
				transfer_address <= sector_begin + PS1_MEMORY_CARD_SECTOR_SIZE;
		}

		McdSizeInfo info = {};
		card.GetSizeInfo(info);
		if ((info.SectorSize != 512 && info.SectorSize != 1024) ||
			info.EraseBlockSizeInSectors == 0 || info.EraseBlockSizeInSectors > 16 ||
			info.McdSizeInSectors == 0 || sector_address >= info.McdSizeInSectors)
		{
			return false;
		}

		const u64 backing_size =
			static_cast<u64>(info.SectorSize + 16) * info.McdSizeInSectors;
		const u64 sector_begin =
			static_cast<u64>(info.SectorSize + 16) * sector_address;
		const u64 sector_end = std::min<u64>(
			sector_begin + info.SectorSize + 16, backing_size);
		return transfer_address >= sector_begin && transfer_address <= sector_end;
	}

	bool DoPortableMemoryCardSlot(StateWrapper& sw, _mcd& card,
		u8 expected_port, u8 expected_slot)
	{
		u8 current_command = card.currentCommand;
		u8 term = card.term;
		u8 good_sector = card.goodSector ? 1 : 0;
		u8 msb = card.msb;
		u8 lsb = card.lsb;
		u32 sector_address = card.sectorAddr;
		u32 transfer_address = card.transferAddr;
		u32 buffer_size = static_cast<u32>(card.buf.size());
		u8 flag = card.FLAG;
		u8 port = card.port;
		u8 slot = card.slot;
		u32 auto_eject_ticks = static_cast<u32>(card.autoEjectTicks);

		if (sw.IsWriting() &&
			(card.buf.size() > PORTABLE_MCD_BUFFER_LIMIT ||
			 card.autoEjectTicks > PORTABLE_MCD_MAX_AUTO_EJECT_TICKS ||
			 port != expected_port || slot != expected_slot ||
			 !PortableMemoryCardAddressIsValid(card, sector_address, transfer_address)))
		{
			Console.Error("Portable replay memory-card slot state is invalid.");
			return false;
		}

		sw.Do(&current_command);
		sw.Do(&term);
		sw.Do(&good_sector);
		sw.Do(&msb);
		sw.Do(&lsb);
		sw.Do(&sector_address);
		sw.Do(&transfer_address);
		sw.Do(&buffer_size);
		if (sw.HasError() || buffer_size > PORTABLE_MCD_BUFFER_LIMIT)
		{
			Console.Error("Portable replay memory-card transfer buffer is invalid.");
			return false;
		}

		std::vector<u8> buffer;
		if (sw.IsWriting())
			buffer = card.buf;
		else
			buffer.resize(buffer_size);
		if (buffer_size != 0)
			sw.DoBytes(buffer.data(), buffer_size);

		sw.Do(&flag);
		sw.Do(&port);
		sw.Do(&slot);
		sw.Do(&auto_eject_ticks);
		if (sw.HasError() || good_sector > 1 || port != expected_port ||
			slot != expected_slot ||
			auto_eject_ticks > PORTABLE_MCD_MAX_AUTO_EJECT_TICKS ||
			!PortableMemoryCardAddressIsValid(card, sector_address, transfer_address))
		{
			Console.Error("Portable replay memory-card slot provenance is invalid.");
			return false;
		}

		if (sw.IsReading())
		{
			card.currentCommand = current_command;
			card.term = term;
			card.goodSector = (good_sector != 0);
			card.msb = msb;
			card.lsb = lsb;
			card.sectorAddr = sector_address;
			card.transferAddr = transfer_address;
			card.buf = std::move(buffer);
			card.FLAG = flag;
			card.port = port;
			card.slot = slot;
			card.autoEjectTicks = auto_eject_ticks;
		}
		return true;
	}
} // namespace

bool sioDoPortableMemoryCardState(StateWrapper& sw)
{
	if (!sw.IsPortableReplay() || !sw.DoMarker("SioMemoryCards-v1"))
		return false;

	u8 selected_port = 0xff;
	u8 selected_slot = 0xff;
	if (sw.IsWriting())
	{
		for (u8 port = 0; port < SIO::PORTS; port++)
		{
			for (u8 slot = 0; slot < SIO::SLOTS; slot++)
			{
				if (mcd == &mcds[port][slot])
				{
					selected_port = port;
					selected_slot = slot;
				}
			}
		}
		if (selected_port == 0xff)
		{
			Console.Error("Portable replay selected memory-card pointer has no slot provenance.");
			return false;
		}
	}

	for (u8 port = 0; port < SIO::PORTS; port++)
	{
		for (u8 slot = 0; slot < SIO::SLOTS; slot++)
		{
			if (!DoPortableMemoryCardSlot(sw, mcds[port][slot], port, slot))
				return false;
		}
	}

	sw.Do(&selected_port);
	sw.Do(&selected_slot);
	if (sw.HasError() || selected_port >= SIO::PORTS || selected_slot >= SIO::SLOTS ||
		!g_MemoryCardProtocol.DoPortableState(sw))
	{
		Console.Error("Portable replay selected memory-card provenance is invalid.");
		return false;
	}

	if (sw.IsReading())
		mcd = &mcds[selected_port][selected_slot];
	return true;
}

void sioNextFrame() {
	for ( uint port = 0; port < 2; ++port ) {
		for ( uint slot = 0; slot < 4; ++slot ) {
			mcds[port][slot].NextFrame();
		}
	}
}

void sioSetGameSerial( const std::string& serial ) {
	for ( uint port = 0; port < 2; ++port ) {
		for ( uint slot = 0; slot < 4; ++slot ) {
			if ( mcds[port][slot].ReIndex( serial ) ) {
				AutoEject::Set( port, slot );
			}
		}
	}
}

std::tuple<u32, u32> sioConvertPadToPortAndSlot(u32 index)
{
	if (index > 4) // [5,6,7]
		return std::make_tuple(1, index - 4); // 2B,2C,2D
	else if (index > 1) // [2,3,4]
		return std::make_tuple(0, index - 1); // 1B,1C,1D
	else // [0,1]
		return std::make_tuple(index, 0); // 1A,2A
}

u32 sioConvertPortAndSlotToPad(u32 port, u32 slot)
{
	if (slot == 0)
		return port;
	else if (port == 0) // slot=[0,1]
		return slot + 1; // 2,3,4
	else
		return slot + 4; // 5,6,7
}

bool sioPadIsMultitapSlot(u32 index)
{
	return (index >= 2);
}

bool sioPortAndSlotIsMultitap(u32 port, u32 slot)
{
	return (slot != 0);
}

void AutoEject::CountDownTicks()
{
	bool reinserted = false;
	for (size_t port = 0; port < SIO::PORTS; port++)
	{
		for (size_t slot = 0; slot < SIO::SLOTS; slot++)
		{
			if (mcds[port][slot].autoEjectTicks > 0)
			{
				if (--mcds[port][slot].autoEjectTicks == 0)
					reinserted |= EmuConfig.Mcd[sioConvertPortAndSlotToPad(port, slot)].Enabled;
			}
		}
	}

	if (reinserted)
	{
		Host::AddIconOSDMessage("AutoEjectAllSet", ICON_PF_MEMORY_CARD,
			TRANSLATE_SV("MemoryCard", "Memory Cards reinserted."), Host::OSD_INFO_DURATION);
	}
}

void AutoEject::Set(size_t port, size_t slot)
{
	if (mcds[port][slot].autoEjectTicks == 0)
	{
		mcds[port][slot].autoEjectTicks = 60; // 60 frames is enough.
		mcds[port][slot].term = Terminator::NOT_READY; // Reset terminator to NOT_READY (0x66), forces the PS2 to recheck the memcard.
	}
}

void AutoEject::Clear(size_t port, size_t slot)
{
	mcds[port][slot].autoEjectTicks = 0;
}

void AutoEject::SetAll()
{
	Host::AddIconOSDMessage("AutoEjectAllSet", ICON_PF_MEMORY_CARD,
		TRANSLATE_SV("MemoryCard", "Force ejecting all Memory Cards. Reinserting in 1 second."), Host::OSD_INFO_DURATION);

	for (size_t port = 0; port < SIO::PORTS; port++)
	{
		for (size_t slot = 0; slot < SIO::SLOTS; slot++)
		{
			AutoEject::Set(port, slot);
		}
	}
}

void AutoEject::ClearAll()
{
	for (size_t port = 0; port < SIO::PORTS; port++)
	{
		for (size_t slot = 0; slot < SIO::SLOTS; slot++)
		{
			AutoEject::Clear(port, slot);
		}
	}
}

// Decremented once per frame if nonzero, indicates how many more frames must pass before
// memcards are considered "no longer being written to". Used as a way to detect if it is
// unsafe to shutdown the VM due to memcard access.
static std::atomic_uint32_t currentBusyTicks = 0;

uint32_t sioLastFrameMcdBusy = 0;

void MemcardBusy::Decrement()
{
	if (currentBusyTicks.load(std::memory_order_relaxed) == 0)
		return;

	currentBusyTicks.fetch_sub(1, std::memory_order_release);
}

void MemcardBusy::SetBusy()
{
	currentBusyTicks.store(300, std::memory_order_release);
	sioLastFrameMcdBusy = g_FrameCount;
}

bool MemcardBusy::IsBusy()
{
	return (currentBusyTicks.load(std::memory_order_acquire) > 0);
}

void MemcardBusy::ClearBusy()
{
	currentBusyTicks.store(0, std::memory_order_release);
	sioLastFrameMcdBusy = 0;
}

void MemcardBusy::CheckSaveStateDependency()
{
	if (g_FrameCount - sioLastFrameMcdBusy > NUM_FRAMES_BEFORE_SAVESTATE_DEPENDENCY_WARNING)
	{
		Host::AddIconOSDMessage("MemcardBusy", ICON_PF_MEMORY_CARD,
			TRANSLATE_SV("MemoryCard", "The virtual console hasn't saved to your memory card in a long time.\nSavestates should not be used in place of in-game saves."), Host::OSD_INFO_DURATION);
	}
}
