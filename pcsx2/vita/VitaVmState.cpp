// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/CDVD.h"
#include "Elfheader.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
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
	static const std::string s_empty_string;
	static const std::vector<u32> s_empty_processors;
	static u32 s_current_crc = 0;
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
		return {};
	}

	std::string GetTitle(bool prefer_en)
	{
		return s_elf_path;
	}

	u32 GetDiscCRC()
	{
		return s_current_crc;
	}

	std::string GetDiscVersion()
	{
		return {};
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
		return {};
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

			s_elf_executed = true;
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
		}
	} // namespace Internal

} // namespace VMManager

void VitaClearVmBootState()
{
	VMManager::SetState(VMState::Shutdown);
	VMManager::ClearELFInfo();
	VMManager::s_elf_override = {};
	VMManager::s_fast_boot_requested = false;
}

void VitaSetFastBootElfOverride(const char* elf_path)
{
	VMManager::ClearELFInfo();
	VMManager::s_elf_override = elf_path ? std::string(elf_path) : std::string();
	VMManager::s_fast_boot_requested = !VMManager::s_elf_override.empty();
}
