// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/CDVD.h"
#include "Config.h"
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
#include "DebugTools/CoreEventTrace.h"
#include "DebugTools/MachineCheckpointTrace.h"
#endif
#include "DebugTools/EeTrace.h"
#include "DebugTools/GsTrace.h"
#include "DebugTools/IopTrace.h"
#include "DebugTools/IpuTrace.h"
#include "DebugTools/MemTrace.h"
#include "DebugTools/SifTrace.h"
#include "DebugTools/Spu2Trace.h"
#include "DebugTools/VifTrace.h"
#include "DebugTools/VuTrace.h"
#include "Elfheader.h"
#include "Input/InputManager.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "SaveState.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "vita/VitaCore.h"
#include "vtlb.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Console.h"

#include <utility>

namespace VMManager
{
	static VMState s_state = VMState::Shutdown;
	static std::string s_elf_override;
	static std::string s_elf_path;
	static std::string s_disc_serial;
	static std::string s_disc_elf;
	static std::string s_disc_version;
	static const std::string s_empty_string;
	static const std::vector<u32> s_empty_processors;
	static u32 s_current_crc = 0;
	static u32 s_disc_crc = 0;
	static u32 s_elf_entry_point = 0xFFFFFFFFu;
	static bool s_elf_executed = false;
	static bool s_fast_boot_requested = false;

	static void UpdateELFInfo(std::string elf_path)
	{
		Error error;
		ElfObject elfo;
		if (elf_path.empty() || !cdvdLoadElf(&elfo, elf_path, false, &error))
		{
			Console.Error("Vita ELF load info failed for '%s': %s",
				elf_path.c_str(), error.GetDescription().c_str());
			s_elf_path = {};
			s_elf_entry_point = 0xFFFFFFFFu;
			s_current_crc = 0;
			return;
		}

		elfo.LoadHeaders();
		s_current_crc = elfo.GetCRC();
		s_elf_entry_point = elfo.GetEntryPoint();
		s_elf_path = std::move(elf_path);
	}

	static void ClearELFInfo()
	{
		s_current_crc = 0;
		s_elf_executed = false;
		s_elf_path = {};
		s_elf_entry_point = 0xFFFFFFFFu;
	}

	static void ClearDiscInfo()
	{
		s_disc_serial = {};
		s_disc_elf = {};
		s_disc_version = {};
		s_disc_crc = 0;
	}

	static void UpdateDiscInfo()
	{
		// PCSX2 owner: VMManager.cpp::UpdateDiscDetails(). Disc metadata is
		// machine state, not UI decoration: CDVD.cpp::cdvdReadKey() consumes the
		// serial when emulating the MechaCon disc-key command.
		cdvdGetDiscInfo(&s_disc_serial, &s_disc_elf, &s_disc_version, &s_disc_crc, nullptr);
		Console.WriteLn("Vita disc metadata: serial=%s elf=%s version=%s crc=%08x",
			s_disc_serial.c_str(), s_disc_elf.c_str(), s_disc_version.c_str(), s_disc_crc);
	}

	static bool HasBootedELFState()
	{
		return s_current_crc != 0 && s_elf_executed;
	}

	bool PerformEarlyHardwareChecks(const char** error)
	{
		return true;
	}

	VMState GetState()
	{
		return s_state;
	}

	void SetState(VMState state)
	{
		s_state = state;
	}

	bool HasValidVM()
	{
		return s_state != VMState::Shutdown;
	}

	std::string GetDiscPath()
	{
		return {};
	}

	std::string GetDiscELF()
	{
		return s_disc_elf;
	}

	std::string GetTitle(bool prefer_en)
	{
		return s_elf_path;
	}

	u32 GetDiscCRC()
	{
		return s_disc_crc;
	}

	std::string GetDiscVersion()
	{
		return s_disc_version;
	}

	u32 GetCurrentCRC()
	{
		return s_current_crc;
	}

	const std::string& GetCurrentELF()
	{
		return s_elf_path;
	}

	void SetPaused(bool paused)
	{
		s_state = paused ? VMState::Paused : VMState::Running;
	}

	float GetTargetSpeed()
	{
		return 1.0f;
	}

	bool IsTargetSpeedAdjustedToHost()
	{
		return false;
	}

	float GetFrameRate()
	{
		return 0.0f;
	}

	std::string GetDiscSerial()
	{
		return s_disc_serial;
	}

	void UpdateTargetSpeed()
	{
	}

	void UpdateDiscordPresence(bool update_session_time)
	{
	}

	namespace Internal
	{
		bool IsFastBootInProgress()
		{
			return s_fast_boot_requested && !HasBootedELFState();
		}

		bool WasFastBooted()
		{
			return s_fast_boot_requested;
		}

		void DisableFastBoot()
		{
			s_fast_boot_requested = false;
		}

		bool HasBootedELF()
		{
			return HasBootedELFState();
		}

		u32 GetCurrentELFEntryPoint()
		{
			return s_elf_entry_point;
		}

		void FrameRateChanged()
		{
		}

		void Throttle()
		{
		}

		void ClearCPUExecutionCaches()
		{
			Cpu->Reset();
			psxCpu->Reset();
			CpuVU0->Reset();
			CpuVU1->Reset();
		}

		const std::vector<u32>& GetSoftwareRendererProcessorList()
		{
			return s_empty_processors;
		}

		const std::string& GetELFOverride()
		{
			return s_elf_override;
		}

		bool IsExecutionInterrupted()
		{
			return false;
		}

		void ELFLoadingOnCPUThread(std::string elf_path)
		{
			UpdateELFInfo(std::move(elf_path));
			s_elf_executed = false;
		}

		void EntryPointCompilingOnCPUThread()
		{
			if (s_elf_executed)
				return;

			// PCSX2 owner: Interpreter.cpp::intExecute() notifies every
			// trace domain immediately before EntryPointCompilingOnCPUThread().
			// Vita's VM shim owns that edge for both interpreter and A32 EE/IOP.
			Pcsx2Trace::NotifyEeElfEntry(s_elf_entry_point);
#if !defined(VITASX2_VITA) || defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
			Pcsx2Trace::NotifyCoreEventElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifyMachineCheckpointElfEntry(s_elf_entry_point);
#endif
			Pcsx2Trace::NotifyMemElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifyGsElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifyIopElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifyIpuElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifySifElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifySpu2ElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifyVifElfEntry(s_elf_entry_point);
			Pcsx2Trace::NotifyVuElfEntry(s_elf_entry_point);
			s_elf_executed = true;
			// Mirrors VMManager.cpp::EntryPointCompilingOnCPUThread() -> HandleELFChange(true):
			// the BIOS/EELOAD-only InstantDMAHack from ApplyGameFixes() is cleared once the game ELF owns execution.
			EmuConfig.Gamefixes.InstantDMAHack = false;
			mmap_ResetBlockTracking();
			ClearCPUExecutionCaches();
			memBindConditionalHandlers();
			ClearCPUExecutionCaches();
		}

		void VSyncOnCPUThread()
		{
		}

		void PollInputOnCPUThread()
		{
			// PCSX2 owner: VMManager.cpp::Internal::PollInputOnCPUThread(),
			// called by Counters.cpp::VSyncStart() after the frame push. Keep
			// Vita controller sampling on that emulated-frame boundary so SIO2
			// observes one coherent snapshot for the following frame.
			InputManager::PollSources();
		}
	} // namespace Internal

} // namespace VMManager

void VitaClearVmBootState()
{
	VMManager::SetState(VMState::Shutdown);
	VMManager::ClearELFInfo();
	VMManager::ClearDiscInfo();
	VMManager::s_elf_override = {};
	VMManager::s_fast_boot_requested = false;
}

void VitaSetFastBootElfOverride(const char* elf_path)
{
	VMManager::ClearELFInfo();
	VMManager::ClearDiscInfo();
	VMManager::s_elf_override = elf_path ? std::string(elf_path) : std::string();
	VMManager::s_fast_boot_requested = !VMManager::s_elf_override.empty();
}

void VitaSetFastBootDisc()
{
	VMManager::ClearELFInfo();
	VMManager::UpdateDiscInfo();
	VMManager::s_elf_override = {};
	VMManager::s_fast_boot_requested = true;
}

bool SaveStateBase::vmFreeze()
{
	// PCSX2 owner: VMManager.cpp::SaveStateBase::vmFreeze(). Keep the ELF
	// identity in the internal-structures entry; it determines whether the
	// restored PC is BIOS/EELOAD or game code and therefore whether the
	// pre-entry InstantDMA contract is still active.
	const u32 previous_crc = VMManager::s_current_crc;
	const std::string previous_elf = VMManager::s_elf_path;
	const bool previous_elf_executed = VMManager::s_elf_executed;
	Freeze(VMManager::s_current_crc);
	FreezeString(VMManager::s_elf_path);
	Freeze(VMManager::s_elf_executed);

	if (IsLoading())
	{
		if (IsPortableReplay())
		{
			const u32 replay_crc = VMManager::s_current_crc;
			const std::string replay_elf = VMManager::s_elf_path;
			const bool replay_elf_executed = VMManager::s_elf_executed;
			if (replay_elf.empty())
			{
				if (replay_crc != 0 || replay_elf_executed)
				{
					Console.Error("Portable replay contains an invalid empty ELF identity.");
					m_error = true;
					return false;
				}
				VMManager::ClearELFInfo();
			}
			else
			{
				// Re-read the ELF from the currently mounted disc. Normal PCSX2
				// savestates tolerate a changed image; an oracle replay must not.
				VMManager::UpdateELFInfo(replay_elf);
				if (VMManager::s_elf_path != replay_elf ||
					VMManager::s_current_crc != replay_crc)
				{
					Console.Error("Portable replay ELF identity does not match the mounted disc.");
					m_error = true;
					return false;
				}
				VMManager::s_elf_executed = replay_elf_executed;
			}

			EmuConfig.Gamefixes.InstantDMAHack = !VMManager::s_elf_executed;
			return IsOkay();
		}

		if (VMManager::s_elf_path != previous_elf)
		{
			if (VMManager::s_elf_path.empty())
			{
				if (VMManager::s_elf_executed)
					Console.Error("Loaded VM state marks an empty ELF path as executed.");
				VMManager::ClearELFInfo();
			}
			else
			{
				VMManager::UpdateELFInfo(std::move(VMManager::s_elf_path));
			}
		}

		if (VMManager::s_current_crc != previous_crc ||
			VMManager::s_elf_path != previous_elf ||
			VMManager::s_elf_executed != previous_elf_executed)
		{
			// Vita's pruned VM has no desktop settings/patch layer. This is the
			// machine-visible part of VMManager.cpp::HandleELFChange() retained by
			// EntryPointCompilingOnCPUThread() for the deterministic Vita core.
			EmuConfig.Gamefixes.InstantDMAHack = !VMManager::s_elf_executed;
		}
	}

	return IsOkay();
}
