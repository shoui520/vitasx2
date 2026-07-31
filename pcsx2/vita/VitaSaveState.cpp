// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Vita-only implementation of the core savestate orchestrator. The owning
// implementation is pcsx2/SaveState.cpp; the desktop ZIP, screenshot, UI, and
// achievements layers are intentionally not linked into the Vita target.

#include "CDVD/CDVD.h"
#include "COP0.h"
#include "Cache.h"
#include "Config.h"
#include "Counters.h"
#include "DebugTools/Breakpoints.h"
#include "DebugTools/SymbolImporter.h"
#include "GS.h"
#include "GS/GS.h"
#include "IPU/IPU.h"
#include "IPU/IPUdma.h"
#include "MTGS.h"
#include "MTVU.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "SIO/Multitap/MultitapProtocol.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Sio.h"
#include "SIO/Sio0.h"
#include "SIO/Sio2.h"
#include "SPU2/spu2.h"
#include "SaveState.h"
#include "SaveStateRaw.h"
#include "StateWrapper.h"
#include "USB/USB.h"
#include "USB/deviceproxy.h"
#include "VMManager.h"
#include "Vif.h"
#include "VUmicro.h"
#include "ps2/BiosTools.h"
#include "vtlb.h"

#include "common/Error.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

using namespace R5900;

namespace
{
	enum class PortableLoadContext : u8
	{
		StrictValidation,
		ProductWorkloadReplay,
	};

	static tlbs s_tlb_backup[std::size(tlb)];

	bool GetPortableTlbCacheMask(const tlbs& entry, u32* cache_mask)
	{
		u32 mask_bits = entry.PageMask.UL >> 13;
		u32 bit_count = 0;
		while (mask_bits != 0)
		{
			bit_count += mask_bits & 1u;
			mask_bits >>= 1;
		}

		// PCSX2 owner: COP0.cpp::ConvertPageMask(). Reject malformed page-mask
		// encodings before its derived shift can become undefined.
		if ((bit_count & 1u) != 0 || bit_count > 12u)
			return false;

		*cache_mask = (1u << (12u + bit_count)) - 1u;
		return true;
	}

	bool ValidatePortableCachedTlbs()
	{
		// PCSX2 owner: SaveState.cpp::ValidatePortableCachedTlbs(). cachedTlbs
		// is derived from the architectural 48-entry TLB, so portable state must
		// carry exactly one matching cache record for every eligible entry.
		if (cachedTlbs.count > cachedTlbs.PageMasks.size())
			return false;

		bool matched[std::size(tlb)] = {};
		size_t expected_count = 0;
		for (const tlbs& entry : tlb)
		{
			if (entry.isSPR() ||
				!((entry.EntryLo0.V && entry.EntryLo0.isCached()) ||
					(entry.EntryLo1.V && entry.EntryLo1.isCached())))
			{
				continue;
			}

			u32 cache_mask = 0;
			if (!GetPortableTlbCacheMask(entry, &cache_mask))
				return false;

			++expected_count;
			bool found = false;
			for (size_t i = 0; i < cachedTlbs.count; i++)
			{
				if (matched[i] || cachedTlbs.PageMasks[i] != cache_mask ||
					cachedTlbs.PFN0s[i] != entry.PFN0() ||
					cachedTlbs.PFN1s[i] != entry.PFN1() ||
					cachedTlbs.CacheEnabled0[i] != (entry.EntryLo0.isCached() ? ~0u : 0u) ||
					cachedTlbs.CacheEnabled1[i] != (entry.EntryLo1.isCached() ? ~0u : 0u))
				{
					continue;
				}

				matched[i] = true;
				found = true;
				break;
			}

			if (!found)
				return false;
		}

		return expected_count == cachedTlbs.count;
	}

	// PCSX2 owner: SaveState.cpp::PreLoadPrep(). Portable replay is loaded only
	// into a fully initialized, paused validation VM.
	void PreLoadPrep()
	{
		MTGS::WaitGS(false);
		std::memcpy(s_tlb_backup, tlb, sizeof(s_tlb_backup));
		mmap_ResetBlockTracking();
		VMManager::Internal::ClearCPUExecutionCaches();
	}

	// PCSX2 owner: SaveState.cpp::PostLoadPrep(). Rebuild every host-derived
	// cache from the restored architectural state before execution can resume.
	void PostLoadPrep(bool refresh_vsync)
	{
		resetCache();
		// FreezeInternals() restores cachedTlbs before this host-side remap pass.
		// UnmapTLB() also removes a cache descriptor by PFN/page-mask identity,
		// not by TLB index.  An old mapping can therefore accidentally remove a
		// different, already-restored descriptor when a cached physical mapping
		// moved to another index or VPN.  Keep the validated serialized ordering
		// intact while only the vTLB map is rebuilt.
		const cachedTlbs_t restored_cached_tlbs = cachedTlbs;
		for (int i = 0; i < 48; i++)
		{
			if (std::memcmp(&s_tlb_backup[i], &tlb[i], sizeof(tlbs)) != 0)
			{
				UnmapTLB(s_tlb_backup[i], i);
				MapTLB(tlb[i], i);
			}
		}
		cachedTlbs = restored_cached_tlbs;

		if (EmuConfig.Gamefixes.GoemonTlbHack)
			GoemonPreloadTlb();
		CBreakPoints::SetSkipFirst(BREAKPOINT_EE, 0);
		CBreakPoints::SetSkipFirst(BREAKPOINT_IOP, 0);
		// Portable replay binds the full scheduler/counter state. Recomputing the
		// VSync cadence here would overwrite the exact next-event deltas restored
		// from PCSX2 before AArch32 executes its first guest instruction.
		if (refresh_vsync)
			UpdateVSyncRate(true);

		if (VMManager::Internal::HasBootedELF())
			R5900SymbolImporter.OnElfLoadedInMemory();
	}

	bool PortableReplayMachineIsQuiescent(Error* error, std::string_view operation,
		PortableLoadContext context = PortableLoadContext::StrictValidation)
	{
		// Cache.cpp owns the EE data cache. PostLoadPrep() deliberately clears it,
		// so portable replay must not accept a state whose PS2-visible cache
		// contents would be discarded before the first replayed instruction.
		if (EmuConfig.Cpu.Recompiler.EnableEECache || !isCacheEmpty())
		{
			Error::SetStringFmt(error,
				"Portable replay {} requires EE data-cache emulation disabled and an empty EE data cache.",
				operation);
			return false;
		}
		const bool product_workload =
			context == PortableLoadContext::ProductWorkloadReplay;
		if (product_workload && VMManager::GetState() != VMState::Paused)
		{
			Error::SetStringFmt(error,
				"Portable replay {} requires the initialized Vita VM to remain paused.",
				operation);
			return false;
		}
		if ((VU0.VI[REG_VPU_STAT].UL & 0x101u) != 0)
		{
			Error::SetStringFmt(error,
				"Portable replay {} requires both VUs idle.", operation);
			return false;
		}
		if (!product_workload && THREAD_VU1)
		{
			Error::SetStringFmt(error,
				"Portable replay {} requires MTVU disabled.", operation);
			return false;
		}
		if (product_workload && THREAD_VU1)
		{
			if (!vu1Thread.IsOpen())
			{
				Error::SetString(error,
					"Portable replay workload load requires the configured MTVU worker.");
				return false;
			}
			vu1Thread.WaitVU();
			if (!vu1Thread.IsDone() ||
				(VU0.VI[REG_VPU_STAT].UL & 0x100u) != 0)
			{
				Error::SetString(error,
					"Portable replay workload load could not drain MTVU to an idle VU1 boundary.");
				return false;
			}
		}
		if (!product_workload && !EmuConfig.GS.SynchronousMTGS)
		{
			Error::SetStringFmt(error,
				"Portable replay {} requires synchronous MTGS.", operation);
			return false;
		}
		if (product_workload)
		{
			if (!MTGS::IsOpen())
			{
				Error::SetString(error,
					"Portable replay workload load requires the configured MTGS worker.");
				return false;
			}
			MTGS::WaitGS(false);
		}
		for (u32 port = 0; port < Pad::NUM_CONTROLLER_PORTS; port++)
		{
			const bool connected = Pad::HasConnectedPad(static_cast<u8>(port));
			if (!product_workload && connected)
			{
				Error::SetStringFmt(error,
					"Portable replay {} currently requires every pad port disconnected.",
					operation);
				return false;
			}
			if (product_workload &&
				((port == 0 && !connected) || (port != 0 && connected) ||
					(port == 0 && EmuConfig.Pad.Ports[0].Type !=
						Pad::ControllerType::DualShock2)))
			{
				Error::SetString(error,
					"Portable replay workload load requires one configured DualShock 2 in port 1 only.");
				return false;
			}
		}
		for (u32 port = 0; port < Pcsx2Config::USBOptions::NUM_PORTS; port++)
		{
			if (EmuConfig.USB.Ports[port].DeviceType != DEVTYPE_NONE)
			{
				Error::SetStringFmt(error,
					"Portable replay {} currently requires every USB port disconnected.",
					operation);
				return false;
			}
		}
		return true;
	}

	struct SysStateComponent
	{
		const char* name;
		int (*freeze)(FreezeAction, freezeData*);
	};

	int SysStateMTGSFreeze(FreezeAction mode, freezeData* data)
	{
		MTGS::FreezeData state = {data, 0};
		MTGS::Freeze(mode, state);
		return state.retval;
	}

	static constexpr SysStateComponent GS_COMPONENT = {"GS", SysStateMTGSFreeze};

	class PortableVectorWriteStream final : public StateWrapper::IStream
	{
	public:
		PortableVectorWriteStream(std::vector<u8>& buffer, u32 reserve)
			: m_buffer(buffer)
		{
			m_buffer.clear();
			m_buffer.reserve(reserve);
		}

		u32 Read(void* buf, u32 count) override
		{
			const u32 available = (m_position <= m_buffer.size()) ?
				static_cast<u32>(m_buffer.size() - m_position) : 0;
			count = std::min(count, available);
			if (count != 0)
			{
				std::memcpy(buf, m_buffer.data() + m_position, count);
				m_position += count;
			}
			return count;
		}

		u32 Write(const void* buf, u32 count) override
		{
			if (count > std::numeric_limits<u32>::max() - m_position)
				return 0;
			const u32 end = m_position + count;
			if (end > m_buffer.size())
				m_buffer.resize(end);
			if (count != 0)
			{
				std::memcpy(m_buffer.data() + m_position, buf, count);
				m_position = end;
			}
			return count;
		}

		u32 GetPosition() override { return m_position; }

		bool SeekAbsolute(u32 pos) override
		{
			if (pos > m_buffer.size())
				return false;
			m_position = pos;
			return true;
		}

		bool SeekRelative(s32 count) override
		{
			const s64 position = static_cast<s64>(m_position) + count;
			if (position < 0 || static_cast<u64>(position) > m_buffer.size())
				return false;
			m_position = static_cast<u32>(position);
			return true;
		}

	private:
		std::vector<u8>& m_buffer;
		u32 m_position = 0;
	};

	bool SaveStateWrapperEntry(SaveStateBase& writer, u32 reserve,
		bool (*do_state)(StateWrapper&))
	{
		StateWrapper::VectorMemoryStream stream(reserve);
		StateWrapper state(&stream, StateWrapper::Mode::Write, g_SaveVersion,
			StateWrapper::DataFormat::PortableReplayV1);
		if (!do_state(state) || !state.IsGood())
			return false;

		const std::vector<u8>& buffer = stream.GetBuffer();
		if (!buffer.empty())
		{
			writer.PrepBlock(static_cast<int>(buffer.size()));
			if (!writer.IsOkay())
				return false;
			std::memcpy(writer.GetBlockPtr(), buffer.data(), buffer.size());
			writer.CommitBlock(static_cast<int>(buffer.size()));
		}
		return writer.IsOkay();
	}

	bool SaveStateWrapperBuffer(std::vector<u8>* buffer, u32 reserve,
		bool (*do_state)(StateWrapper&))
	{
		PortableVectorWriteStream stream(*buffer, reserve);
		StateWrapper state(&stream, StateWrapper::Mode::Write, g_SaveVersion,
			StateWrapper::DataFormat::PortableReplayV1);
		return do_state(state) && state.IsGood() && stream.GetPosition() == buffer->size();
	}

	bool SaveLegacyComponent(SaveStateBase& writer, SysStateComponent component)
	{
		freezeData data = {};
		if (component.freeze(FreezeAction::Size, &data) != 0 || data.size < 0)
			return false;
		if (data.size == 0)
			return true;

		writer.PrepBlock(data.size);
		if (!writer.IsOkay())
			return false;
		data.data = writer.GetBlockPtr();
		if (component.freeze(FreezeAction::Save, &data) != 0)
			return false;
		writer.CommitBlock(data.size);
		return writer.IsOkay();
	}

	bool SaveLegacyComponentBuffer(std::vector<u8>* buffer, SysStateComponent component)
	{
		freezeData data = {};
		if (component.freeze(FreezeAction::Size, &data) != 0 || data.size < 0)
			return false;

		buffer->resize(static_cast<size_t>(data.size));
		if (data.size == 0)
			return true;
		data.data = buffer->data();
		return component.freeze(FreezeAction::Save, &data) == 0;
	}

	bool LoadStateWrapperEntry(std::span<const u8> data,
		bool (*do_state)(StateWrapper&))
	{
		StateWrapper::ReadOnlyMemoryStream stream(data.data(), static_cast<u32>(data.size()));
		StateWrapper state(&stream, StateWrapper::Mode::Read, g_SaveVersion,
			StateWrapper::DataFormat::PortableReplayV1);
		return do_state(state) && state.IsGood() && stream.GetPosition() == data.size();
	}

	bool LoadLegacyComponent(std::span<const u8> data, SysStateComponent component)
	{
		freezeData state = {};
		if (component.freeze(FreezeAction::Size, &state) != 0 || state.size < 0 ||
			static_cast<size_t>(state.size) != data.size())
		{
			return false;
		}

		state.data = const_cast<u8*>(data.data());
		return component.freeze(FreezeAction::Load, &state) == 0;
	}

	static constexpr std::array<std::string_view, 16> PORTABLE_ENTRY_NAMES = {{
		SaveStateRaw::PORTABLE_VERSION_ENTRY_NAME,
		"PCSX2 Internal Structures.dat", "eeMemory.bin", "iopMemory.bin",
		"eeHwRegs.bin", "iopHwRegs.bin", "Scratchpad.bin", "vu0Memory.bin",
		"vu1Memory.bin", "vu0MicroMem.bin", "vu1MicroMem.bin", "SPU2.bin",
		"USB.bin", "PAD.bin", "GS.bin", "Achievements.bin",
	}};

	class PortableEntrySource
	{
	public:
		virtual ~PortableEntrySource() = default;
		virtual size_t GetEntryCount() const = 0;
		virtual std::string_view GetEntryName(u32 index) const = 0;
		virtual u64 GetEntrySize(u32 index) const = 0;
		virtual bool Preflight(Error* error) = 0;
		virtual bool ReadEntry(u32 index, std::span<u8> destination, Error* error) = 0;
	};

	class ArchivePortableEntrySource final : public PortableEntrySource
	{
	public:
		explicit ArchivePortableEntrySource(const ArchiveEntryList& entries)
			: m_entries(entries)
		{
		}

		size_t GetEntryCount() const override { return m_entries.GetLength(); }

		std::string_view GetEntryName(u32 index) const override
		{
			return m_entries[index].GetFilename();
		}

		u64 GetEntrySize(u32 index) const override
		{
			return m_entries[index].GetDataSize();
		}

		bool Preflight(Error* error) override
		{
			const std::vector<u8>& source = m_entries.GetBuffer();
			for (u32 i = 0; i < m_entries.GetLength(); i++)
			{
				const ArchiveEntry& entry = m_entries[i];
				const u64 begin = entry.GetDataIndex();
				const u64 size = entry.GetDataSize();
				if (begin > source.size() || size > source.size() - begin)
				{
					Error::SetStringFmt(error, "Portable replay entry '{}' is out of bounds.",
						entry.GetFilename());
					return false;
				}

				if (size == 0)
					continue;
				for (u32 previous = 0; previous < i; previous++)
				{
					const ArchiveEntry& other = m_entries[previous];
					const u64 other_begin = other.GetDataIndex();
					const u64 other_size = other.GetDataSize();
					if (other_size != 0 && begin < other_begin + other_size &&
						other_begin < begin + size)
					{
						Error::SetString(error, "Portable replay entries overlap.");
						return false;
					}
				}
			}
			return true;
		}

		bool ReadEntry(u32 index, std::span<u8> destination, Error* error) override
		{
			const ArchiveEntry& entry = m_entries[index];
			if (destination.size() != entry.GetDataSize())
			{
				Error::SetStringFmt(error,
					"Portable replay entry '{}' requires {} destination bytes, but {} were supplied.",
					entry.GetFilename(), entry.GetDataSize(), destination.size());
				return false;
			}
			if (!destination.empty())
			{
				std::memcpy(destination.data(),
					m_entries.GetBuffer().data() + entry.GetDataIndex(), destination.size());
			}
			return true;
		}

	private:
		const ArchiveEntryList& m_entries;
	};

	class RawFilePortableEntrySource final : public PortableEntrySource
	{
	public:
		explicit RawFilePortableEntrySource(std::unique_ptr<SaveStateRaw::FileReader> reader)
			: m_reader(std::move(reader))
		{
		}

		size_t GetEntryCount() const override { return m_reader->GetEntryCount(); }
		std::string_view GetEntryName(u32 index) const override
		{
			return m_reader->GetEntryName(index);
		}
		u64 GetEntrySize(u32 index) const override { return m_reader->GetEntrySize(index); }
		bool Preflight(Error*) override { return true; }
		bool ReadEntry(u32 index, std::span<u8> destination, Error* error) override
		{
			return m_reader->ReadEntry(index, destination, error);
		}

	private:
		std::unique_ptr<SaveStateRaw::FileReader> m_reader;
	};

	bool ValidatePortableBiosHeader(std::span<const u8> internal, Error* error)
	{
		static constexpr size_t TAG_SIZE = 32;
		static constexpr size_t CHECKSUM_SIZE = sizeof(u32);
		static constexpr size_t DESCRIPTION_SIZE = 256;
		static constexpr std::array<u8, TAG_SIZE> EXPECTED_TAG = {{
			'B', 'I', 'O', 'S',
		}};

		if (internal.size() < TAG_SIZE + CHECKSUM_SIZE + DESCRIPTION_SIZE ||
			!std::equal(EXPECTED_TAG.begin(), EXPECTED_TAG.end(), internal.begin()))
		{
			Error::SetString(error,
				"Portable replay internal state has an invalid BIOS marker.");
			return false;
		}

		const u32 checksum = static_cast<u32>(internal[TAG_SIZE + 0]) |
			(static_cast<u32>(internal[TAG_SIZE + 1]) << 8) |
			(static_cast<u32>(internal[TAG_SIZE + 2]) << 16) |
			(static_cast<u32>(internal[TAG_SIZE + 3]) << 24);
		if (checksum != BiosChecksum)
		{
			Error::SetStringFmt(error,
				"Portable replay BIOS checksum 0x{:08x} does not match current BIOS 0x{:08x}.",
				checksum, BiosChecksum);
			return false;
		}

		return true;
	}

	void SetDefaultError(Error* error, std::string_view message)
	{
		if (!error || !error->IsValid())
			Error::SetStringView(error, message);
	}

	PortableStateLoadResult LoadPortableStateFromSource(PortableEntrySource& source,
		Error* error, bool quiescence_already_validated,
		PortableLoadContext context = PortableLoadContext::StrictValidation)
	{
		if (!quiescence_already_validated &&
			!PortableReplayMachineIsQuiescent(error, "load", context))
			return PortableStateLoadResult::RejectedBeforeMutation;
		if (source.GetEntryCount() != PORTABLE_ENTRY_NAMES.size())
		{
			Error::SetStringFmt(error, "Portable replay state has {} entries, expected {}.",
				source.GetEntryCount(), PORTABLE_ENTRY_NAMES.size());
			return PortableStateLoadResult::RejectedBeforeMutation;
		}
		if (!source.Preflight(error))
			return PortableStateLoadResult::RejectedBeforeMutation;

		for (u32 i = 0; i < PORTABLE_ENTRY_NAMES.size(); i++)
		{
			if (source.GetEntryName(i) != PORTABLE_ENTRY_NAMES[i])
			{
				Error::SetStringFmt(error, "Portable replay entry {} is '{}', expected '{}'.",
					i, source.GetEntryName(i), PORTABLE_ENTRY_NAMES[i]);
				return PortableStateLoadResult::RejectedBeforeMutation;
			}
		}

		const auto size_is = [&](u32 index, size_t expected) {
			return source.GetEntrySize(index) == expected;
		};
		if (!size_is(0, SaveStateRaw::PORTABLE_VERSION_ENTRY_PAYLOAD.size()) ||
			source.GetEntrySize(1) == 0 ||
			!size_is(2, Ps2MemSize::ExposedRam) || !size_is(3, Ps2MemSize::ExposedIopRam) ||
			!size_is(4, sizeof(eeHw)) || !size_is(5, sizeof(iopHw)) ||
			!size_is(6, sizeof(eeMem->Scratch)) || !size_is(7, VU0_MEMSIZE) ||
			!size_is(8, VU1_MEMSIZE) || !size_is(9, VU0_PROGSIZE) ||
			!size_is(10, VU1_PROGSIZE) || source.GetEntrySize(11) == 0 ||
			source.GetEntrySize(12) == 0 || source.GetEntrySize(13) == 0 ||
			source.GetEntrySize(14) == 0 || source.GetEntrySize(15) != 0)
		{
			Error::SetString(error, "Portable replay entry sizes do not match the PCSX2 schema.");
			return PortableStateLoadResult::RejectedBeforeMutation;
		}

		std::array<u8, SaveStateRaw::PORTABLE_VERSION_ENTRY_PAYLOAD.size()> marker = {};
		if (!source.ReadEntry(0, marker, error) ||
			!std::equal(marker.begin(), marker.end(),
				SaveStateRaw::PORTABLE_VERSION_ENTRY_PAYLOAD.begin()))
		{
			SetDefaultError(error, "Portable replay version marker is invalid.");
			return PortableStateLoadResult::RejectedBeforeMutation;
		}

		size_t scratch_capacity = static_cast<size_t>(source.GetEntrySize(1));
		for (u32 index = 11; index <= 14; index++)
		{
			scratch_capacity = std::max(scratch_capacity,
				static_cast<size_t>(source.GetEntrySize(index)));
		}
		std::vector<u8> scratch;
		scratch.reserve(scratch_capacity);
		scratch.resize(static_cast<size_t>(source.GetEntrySize(1)));
		if (!source.ReadEntry(1, scratch, error) || !ValidatePortableBiosHeader(scratch, error))
			return PortableStateLoadResult::RejectedBeforeMutation;

		freezeData gs_size = {};
		if (GS_COMPONENT.freeze(FreezeAction::Size, &gs_size) != 0 || gs_size.size < 0 ||
			static_cast<u64>(gs_size.size) != source.GetEntrySize(14))
		{
			Error::SetString(error, "Portable replay GS entry size is incompatible.");
			return PortableStateLoadResult::RejectedBeforeMutation;
		}

		PreLoadPrep();
		memLoadingState state(scratch, SaveStateBase::DataFormat::PortableReplayV1);
		const bool bios_loaded = state.FreezeBios();
		const bool internals_loaded = bios_loaded && state.FreezeInternals(error);
		const bool stream_okay = state.IsOkay();
		if (!bios_loaded || !internals_loaded || !stream_okay ||
			state.GetCurrentPos() != scratch.size())
		{
			if (!error || !error->IsValid())
			{
				Error::SetStringFmt(error,
					"Portable replay internal state failed (BIOS={}, internals={}, stream={}, consumed={}/{}).",
					bios_loaded ? 1 : 0, internals_loaded ? 1 : 0, stream_okay ? 1 : 0,
					state.GetCurrentPos(), scratch.size());
			}
			return PortableStateLoadResult::FailedAfterMutation;
		}

		// Reuse the one serialization buffer for each subsequent device owner. The
		// fixed RAM/register entries stream directly into their final VM storage.
		scratch.clear();
		const auto read_fixed = [&](u32 index, void* destination, size_t size) {
			return source.ReadEntry(index,
				std::span<u8>(static_cast<u8*>(destination), size), error);
		};
		if (!read_fixed(2, eeMem->Main, Ps2MemSize::ExposedRam) ||
			!read_fixed(3, iopMem->Main, Ps2MemSize::ExposedIopRam) ||
			!read_fixed(4, eeHw, sizeof(eeHw)) ||
			!read_fixed(5, iopHw, sizeof(iopHw)) ||
			!read_fixed(6, eeMem->Scratch, sizeof(eeMem->Scratch)) ||
			!read_fixed(7, VU0.Mem, VU0_MEMSIZE) ||
			!read_fixed(8, VU1.Mem, VU1_MEMSIZE) ||
			!read_fixed(9, VU0.Micro, VU0_PROGSIZE) ||
			!read_fixed(10, VU1.Micro, VU1_PROGSIZE))
		{
			SetDefaultError(error, "Portable replay memory state could not be read completely.");
			return PortableStateLoadResult::FailedAfterMutation;
		}

		if (!vifValidatePortableRegisters())
		{
			Error::SetString(error,
				"Portable replay VIF hardware registers are unsafe or inconsistent with active UNPACK state.");
			return PortableStateLoadResult::FailedAfterMutation;
		}
		if (!ipuValidatePortableState() || !ipuValidatePortableDmaState())
		{
			Error::SetString(error,
				"Portable replay IPU state is unsafe or inconsistent with its FIFO, command, or DMA registers.");
			return PortableStateLoadResult::FailedAfterMutation;
		}

		const auto read_device = [&](u32 index) {
			scratch.resize(static_cast<size_t>(source.GetEntrySize(index)));
			return source.ReadEntry(index, scratch, error);
		};
		bool (*const pad_loader)(StateWrapper&) =
			context == PortableLoadContext::ProductWorkloadReplay ?
				&Pad::FreezePortableReplayWithConfiguredPads : &Pad::Freeze;
		if (!read_device(11) || !LoadStateWrapperEntry(scratch, &SPU2::DoPortableState) ||
			!read_device(12) || !LoadStateWrapperEntry(scratch, &USB::DoState) ||
			!read_device(13) || !LoadStateWrapperEntry(scratch, pad_loader) ||
			!read_device(14) || !LoadLegacyComponent(scratch, GS_COMPONENT))
		{
			SetDefaultError(error, "Portable replay device state is corrupt or under-consumed.");
			return PortableStateLoadResult::FailedAfterMutation;
		}
		if (!GSValidatePortableState())
		{
			Error::SetString(error,
				"Portable replay GS transfer continuation is out of bounds.");
			return PortableStateLoadResult::FailedAfterMutation;
		}

		PostLoadPrep(false);
		if (context == PortableLoadContext::ProductWorkloadReplay && THREAD_VU1)
			vu1Thread.RebuildFromCanonicalStateAfterPortableLoad();
		if (!ValidatePortableCachedTlbs())
		{
			Error::SetString(error,
				"Portable replay post-load TLB remapping corrupted the restored cached-TLB descriptors.");
			return PortableStateLoadResult::FailedAfterMutation;
		}
		return PortableStateLoadResult::Loaded;
	}
} // namespace

// --------------------------------------------------------------------------------------
// SaveStateBase
// --------------------------------------------------------------------------------------

SaveStateBase::SaveStateBase(VmStateBuffer& memblock, DataFormat data_format)
	: m_memory(memblock)
	, m_data_format(data_format)
	, m_version(g_SaveVersion)
{
}

void SaveStateBase::PrepBlock(int size)
{
	if (m_error)
		return;

	if (size < 0 || m_idx < 0 || m_idx > std::numeric_limits<int>::max() - size)
	{
		m_error = true;
		return;
	}

	const int end = m_idx + size;
	if (IsSaving())
	{
		if (static_cast<u32>(end) >= m_memory.size())
			m_memory.resize(static_cast<u32>(end));
	}
	else if (m_memory.size() < static_cast<u32>(end))
	{
		Console.Error("(SaveStateBase) Buffer overflow in PrepBlock(), expected %d got %u",
			end, static_cast<u32>(m_memory.size()));
		m_error = true;
	}
}

bool SaveStateBase::FreezeTag(const char* src)
{
	if (m_error)
		return false;

	char tagspace[32];
	pxAssertMsg(std::strlen(src) < (sizeof(tagspace) - 1),
		"Tag name exceeds the allowed length");
	std::memset(tagspace, 0, sizeof(tagspace));
	StringUtil::Strlcpy(tagspace, src, sizeof(tagspace));
	Freeze(tagspace);

	if (std::strcmp(tagspace, src) != 0)
	{
		Console.Error(fmt::format(
			"Savestate data corruption detected while reading tag: {}", src));
		m_error = true;
		return false;
	}
	return true;
}

bool SaveStateBase::FreezeBios()
{
	if (!FreezeTag("BIOS"))
		return false;

	u32 bioscheck = BiosChecksum;
	char biosdesc[256] = {};
	StringUtil::Strlcpy(biosdesc, BiosDescription, sizeof(biosdesc));
	Freeze(bioscheck);
	Freeze(biosdesc);

	if (bioscheck != BiosChecksum)
	{
		if (IsPortableReplay())
		{
			Console.Error("Portable replay BIOS checksum mismatch: current=0x%08x state=0x%08x",
				BiosChecksum, bioscheck);
			m_error = true;
			return false;
		}
		Console.Error("\n  Warning: BIOS Version Mismatch, savestate may be unstable!");
		Console.Error(
			"    Current BIOS:   %s (crc=0x%08x)\n"
			"    Savestate BIOS: %s (crc=0x%08x)\n",
			BiosDescription.c_str(), BiosChecksum, biosdesc, bioscheck);
	}
	return IsOkay();
}

bool SaveStateBase::FreezeInternals(Error* error)
{
	// PCSX2 owner: SaveState.cpp::SaveStateBase::FreezeInternals(). The order is
	// part of the portable replay format and must remain byte-for-byte aligned
	// with the x86 oracle producer.
	if (THREAD_VU1)
		Console.Warning("MTVU speedhack is enabled, saved states may not be stable");

	if (!vmFreeze() || !FreezeTag("cpuRegs"))
		return false;

	if (IsSaving())
	{
		// Count stays affine while Status.IM7 is clear. A savestate is a real
		// architectural observer, so materialize it at the current EE cycle
		// before serializing cpuRegs.
		COP0_UpdateCount();
	}

	if (IsPortableReplay() && IsSaving())
	{
		// MachineCheckpointTrace.cpp::CaptureRecord() owns this cross-provider
		// distinction: code is decoder scratch, not the instruction at the
		// architectural PC. Canonicalize it without mutating the running VM so an
		// x86 and A32 continuation can compare every remaining serialized byte.
		cpuRegisters portable_cpu_regs = cpuRegs;
		psxRegisters portable_psx_regs = psxRegs;
		portable_cpu_regs.code = 0;
		portable_psx_regs.code = 0;
		Freeze(portable_cpu_regs);
		Freeze(portable_psx_regs);
	}
	else
	{
		Freeze(cpuRegs);
		Freeze(psxRegs);
	}
	Freeze(fpuRegs);
	Freeze(tlb);
	Freeze(cachedTlbs);
	if (IsPortableReplay() && (HasError() || !ValidatePortableCachedTlbs()))
	{
		Error::SetString(error,
			"Portable replay cached-TLB state is inconsistent with the 48 PCSX2 TLB entries.");
		m_error = true;
		return false;
	}
	Freeze(AllowParams1);
	Freeze(AllowParams2);

	if (!FreezeTag("Cycles"))
		return false;
	Freeze(EEsCycle);
	Freeze(EEoCycle);
	Freeze(nextDeltaCounter);
	Freeze(nextStartCounter);
	Freeze(psxNextStartCounter);
	Freeze(psxNextDeltaCounter);

	if (!FreezeTag("EE-Subsystems"))
		return false;

	bool okay = rcntFreeze();
	okay = okay && memFreeze(error);
	okay = okay && gsFreeze();
	okay = okay && vuMicroFreeze();
	okay = okay && vuJITFreeze();
	okay = okay && vif0Freeze();
	okay = okay && vif1Freeze();
	okay = okay && sifFreeze();
	okay = okay && ipuFreeze();
	okay = okay && ipuDmaFreeze();
	okay = okay && gifFreeze();
	okay = okay && gifDmaFreeze();
	okay = okay && sprFreeze();
	okay = okay && mtvuFreeze();
	if (!okay)
		return false;

	if (!FreezeTag("IOP-Subsystems"))
		return false;
	FreezeMem(iopMem->Sif, sizeof(iopMem->Sif));
	okay = psxRcntFreeze();
	if (!okay)
		return false;

	{
		std::optional<StateWrapper::VectorMemoryStream> save_stream;
		std::optional<StateWrapper::ReadOnlyMemoryStream> load_stream;
		if (IsSaving())
			save_stream.emplace();
		else
			load_stream.emplace(&m_memory[m_idx], static_cast<int>(m_memory.size()) - m_idx);

		StateWrapper state(IsSaving() ? static_cast<StateWrapper::IStream*>(&save_stream.value()) :
									  static_cast<StateWrapper::IStream*>(&load_stream.value()),
			IsSaving() ? StateWrapper::Mode::Write : StateWrapper::Mode::Read, g_SaveVersion,
			IsPortableReplay() ? StateWrapper::DataFormat::PortableReplayV1 :
				StateWrapper::DataFormat::Native);

		okay = g_Sio0.DoState(state);
		okay = okay && g_Sio2.DoState(state);
		okay = okay && g_MultitapArr.at(0).DoState(state);
		okay = okay && g_MultitapArr.at(1).DoState(state);
		if (!okay || !state.IsGood())
			return false;

		if (IsSaving())
		{
			FreezeMem(const_cast<u8*>(save_stream->GetBuffer().data()),
				static_cast<int>(save_stream->GetPosition()));
		}
		else
		{
			const int new_idx = m_idx + static_cast<int>(load_stream->GetPosition());
			if (new_idx < m_idx || static_cast<size_t>(new_idx) >= m_memory.size())
				return false;
			m_idx = new_idx;
		}
	}

	okay = cdrFreeze();
	okay = okay && cdvdFreeze();
	okay = okay && deci2Freeze();
	okay = okay && InputRecordingFreeze();
	okay = okay && handleFreeze();

#if defined(VITASX2_VITA) && !defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	if (okay && !IsSaving())
		VitaRestoreEeDeadlineCacheAfterStateLoad();
#endif
	return okay;
}

bool SaveStateBase::InputRecordingFreeze()
{
	// PCSX2 owner: Recording/InputRecording.cpp::InputRecordingFreeze().
	if (!FreezeTag("InputRecording"))
		return false;
	Freeze(g_FrameCount);
	return IsOkay();
}

// --------------------------------------------------------------------------------------
// Memory-backed readers/writers
// --------------------------------------------------------------------------------------

memSavingState::memSavingState(VmStateBuffer& save_to, DataFormat data_format)
	: SaveStateBase(save_to, data_format)
{
}

void memSavingState::FreezeMem(void* data, int size)
{
	if (size == 0 || m_error)
		return;
	if (size < 0 || m_idx < 0 || m_idx > std::numeric_limits<int>::max() - size)
	{
		m_error = true;
		return;
	}

	const int new_size = m_idx + size;
	if (static_cast<u32>(new_size) > m_memory.size())
		m_memory.resize(static_cast<u32>(new_size));
	std::memcpy(&m_memory[m_idx], data, static_cast<size_t>(size));
	m_idx = new_size;
}

memLoadingState::memLoadingState(const VmStateBuffer& load_from, DataFormat data_format)
	: SaveStateBase(const_cast<VmStateBuffer&>(load_from), data_format)
{
}

void memLoadingState::FreezeMem(void* data, int size)
{
	if (size < 0 || m_idx < 0 || m_idx > std::numeric_limits<int>::max() - size ||
		static_cast<u32>(m_idx + size) > m_memory.size())
	{
		m_error = true;
	}

	if (m_error)
	{
		if (size > 0)
			std::memset(data, 0, static_cast<size_t>(size));
		return;
	}

	if (size > 0)
	{
		std::memcpy(data, &m_memory[m_idx], static_cast<size_t>(size));
		m_idx += size;
	}
}

// --------------------------------------------------------------------------------------
// Validation-only portable replay archive
// --------------------------------------------------------------------------------------

std::unique_ptr<ArchiveEntryList> SaveState_DownloadPortableState(Error* error)
{
	if (!PortableReplayMachineIsQuiescent(error, "capture"))
		return nullptr;
	if (!GSValidatePortableState())
	{
		Error::SetString(error,
			"Portable replay capture found an unsafe GS transfer continuation.");
		return nullptr;
	}

	auto entries = std::make_unique<ArchiveEntryList>();
	entries->GetBuffer().resize(64 * 1024 * 1024);
	memSavingState state(entries->GetBuffer(), SaveStateBase::DataFormat::PortableReplayV1);

	const auto add_entry = [&](u32 index, uint start) {
		entries->Add(ArchiveEntry(std::string(PORTABLE_ENTRY_NAMES[index]))
			.SetDataIndex(start).SetDataSize(state.GetCurrentPos() - start));
	};
	const auto save_memory_entry = [&](u32 index, void* data, int size) {
		const uint start = state.GetCurrentPos();
		state.FreezeMem(data, size);
		add_entry(index, start);
	};
	const auto save_empty_entry = [&](u32 index) {
		const uint start = state.GetCurrentPos();
		add_entry(index, start);
	};

	uint start = state.GetCurrentPos();
	state.FreezeMem(const_cast<u8*>(SaveStateRaw::PORTABLE_VERSION_ENTRY_PAYLOAD.data()),
		static_cast<int>(SaveStateRaw::PORTABLE_VERSION_ENTRY_PAYLOAD.size()));
	add_entry(0, start);

	start = state.GetCurrentPos();
	if (!state.FreezeBios() || !state.FreezeInternals(error))
	{
		SetDefaultError(error, "Portable internal-state serialization failed.");
		return nullptr;
	}
	add_entry(1, start);

	save_memory_entry(2, eeMem->Main, Ps2MemSize::ExposedRam);
	save_memory_entry(3, iopMem->Main, Ps2MemSize::ExposedIopRam);
	save_memory_entry(4, eeHw, sizeof(eeHw));
	save_memory_entry(5, iopHw, sizeof(iopHw));
	save_memory_entry(6, eeMem->Scratch, sizeof(eeMem->Scratch));
	save_memory_entry(7, VU0.Mem, VU0_MEMSIZE);
	save_memory_entry(8, VU1.Mem, VU1_MEMSIZE);
	save_memory_entry(9, VU0.Micro, VU0_PROGSIZE);
	save_memory_entry(10, VU1.Micro, VU1_PROGSIZE);

	start = state.GetCurrentPos();
	if (!SaveStateWrapperEntry(state, 3 * 1024 * 1024, &SPU2::DoPortableState))
	{
		Error::SetString(error, "Portable serialization failed for SPU2.bin.");
		return nullptr;
	}
	add_entry(11, start);

	// Disconnected USB ports still leave the OHCI controller, clocks, registers,
	// packet state, and IRQ-visible timing live. PCSX2's USB::DoState() owns that
	// state, so an empty USB entry is never a valid portable replay shortcut.
	start = state.GetCurrentPos();
	if (!SaveStateWrapperEntry(state, 16 * 1024, &USB::DoState) ||
		state.GetCurrentPos() == start)
	{
		Error::SetString(error, "Portable serialization failed for USB.bin.");
		return nullptr;
	}
	add_entry(12, start);

	start = state.GetCurrentPos();
	if (!SaveStateWrapperEntry(state, 16 * 1024, &Pad::Freeze))
	{
		Error::SetString(error, "Portable serialization failed for PAD.bin.");
		return nullptr;
	}
	add_entry(13, start);

	start = state.GetCurrentPos();
	if (!SaveLegacyComponent(state, GS_COMPONENT))
	{
		Error::SetString(error, "Portable serialization failed for GS.bin.");
		return nullptr;
	}
	add_entry(14, start);

	// Achievements are absent from the Vita target and must be disabled in the
	// producer. Keep the canonical optional entry present and empty.
	save_empty_entry(15);

	if (!state.IsOkay())
	{
		Error::SetString(error, "Portable replay output exceeded its state buffer.");
		return nullptr;
	}
	return entries;
}

bool SaveState_SavePortableStateFile(const char* filename, Error* error)
{
	if (!PortableReplayMachineIsQuiescent(error, "capture"))
		return false;
	if (!GSValidatePortableState())
	{
		Error::SetString(error,
			"Portable replay capture found an unsafe GS transfer continuation.");
		return false;
	}
	freezeData gs_size = {};
	if (GS_COMPONENT.freeze(FreezeAction::Size, &gs_size) != 0 || gs_size.size < 0)
	{
		Error::SetString(error, "Portable serialization could not size GS.bin.");
		return false;
	}
	const size_t scratch_capacity = std::max<size_t>(
		static_cast<size_t>(gs_size.size), 3 * 1024 * 1024);

	return SaveStateRaw::EncodeFile(filename,
		[&](u32 index, std::string_view name, std::vector<u8>* scratch,
			std::span<const u8>* data, Error* entry_error) {
			if (index >= PORTABLE_ENTRY_NAMES.size() || name != PORTABLE_ENTRY_NAMES[index])
			{
				Error::SetString(entry_error,
					"Portable replay raw writer requested a noncanonical entry.");
				return false;
			}

			const auto direct = [&](const void* pointer, size_t size) {
				*data = std::span<const u8>(static_cast<const u8*>(pointer), size);
				return true;
			};
			const auto buffered = [&]() {
				*data = std::span<const u8>(*scratch);
				return true;
			};

			switch (index)
			{
				case 0:
					return direct(SaveStateRaw::PORTABLE_VERSION_ENTRY_PAYLOAD.data(),
						SaveStateRaw::PORTABLE_VERSION_ENTRY_PAYLOAD.size());

				case 1:
				{
					scratch->reserve(scratch_capacity);
					memSavingState state(*scratch,
						SaveStateBase::DataFormat::PortableReplayV1);
					if (!state.FreezeBios() || !state.FreezeInternals(entry_error) ||
						!state.IsOkay())
					{
						SetDefaultError(entry_error,
							"Portable internal-state serialization failed.");
						return false;
					}
					scratch->resize(state.GetCurrentPos());
					return buffered();
				}

				case 2:
					return direct(eeMem->Main, Ps2MemSize::ExposedRam);
				case 3:
					return direct(iopMem->Main, Ps2MemSize::ExposedIopRam);
				case 4:
					return direct(eeHw, sizeof(eeHw));
				case 5:
					return direct(iopHw, sizeof(iopHw));
				case 6:
					return direct(eeMem->Scratch, sizeof(eeMem->Scratch));
				case 7:
					return direct(VU0.Mem, VU0_MEMSIZE);
				case 8:
					return direct(VU1.Mem, VU1_MEMSIZE);
				case 9:
					return direct(VU0.Micro, VU0_PROGSIZE);
				case 10:
					return direct(VU1.Micro, VU1_PROGSIZE);

				case 11:
					if (!SaveStateWrapperBuffer(scratch, 3 * 1024 * 1024,
							&SPU2::DoPortableState))
					{
						Error::SetString(entry_error,
							"Portable serialization failed for SPU2.bin.");
						return false;
					}
					return buffered();

				case 12:
					if (!SaveStateWrapperBuffer(scratch, 16 * 1024, &USB::DoState) ||
						scratch->empty())
					{
						Error::SetString(entry_error,
							"Portable serialization failed for USB.bin.");
						return false;
					}
					return buffered();

				case 13:
					if (!SaveStateWrapperBuffer(scratch, 16 * 1024, &Pad::Freeze))
					{
						Error::SetString(entry_error,
							"Portable serialization failed for PAD.bin.");
						return false;
					}
					return buffered();

				case 14:
					if (!SaveLegacyComponentBuffer(scratch, GS_COMPONENT))
					{
						Error::SetString(entry_error,
							"Portable serialization failed for GS.bin.");
						return false;
					}
					return buffered();

				case 15:
					// Achievements are absent from the Vita target. Preserve the
					// canonical optional entry as an empty payload.
					*data = {};
					return true;

				default:
					return false;
			}
		}, error);
}

PortableStateLoadResult SaveState_LoadPortableState(
	const ArchiveEntryList& entries, Error* error)
{
	ArchivePortableEntrySource source(entries);
	return LoadPortableStateFromSource(source, error, false);
}

PortableStateLoadResult SaveState_LoadPortableStateFile(
	const char* filename, Error* error)
{
	if (!PortableReplayMachineIsQuiescent(error, "load"))
		return PortableStateLoadResult::RejectedBeforeMutation;

	std::unique_ptr<SaveStateRaw::FileReader> reader =
		SaveStateRaw::FileReader::Open(filename, error);
	if (!reader)
		return PortableStateLoadResult::RejectedBeforeMutation;

	RawFilePortableEntrySource source(std::move(reader));
	return LoadPortableStateFromSource(source, error, true);
}

PortableStateLoadResult SaveState_LoadPortableStateFileForVitaWorkloadReplay(
	const char* filename, Error* error)
{
	constexpr PortableLoadContext context =
		PortableLoadContext::ProductWorkloadReplay;
	if (!PortableReplayMachineIsQuiescent(error, "workload load", context))
		return PortableStateLoadResult::RejectedBeforeMutation;

	std::unique_ptr<SaveStateRaw::FileReader> reader =
		SaveStateRaw::FileReader::Open(filename, error);
	if (!reader)
		return PortableStateLoadResult::RejectedBeforeMutation;

	RawFilePortableEntrySource source(std::move(reader));
	return LoadPortableStateFromSource(source, error, true, context);
}
