// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class SettingsInterface;
struct HotkeyInfo;
enum class GenericInputBinding : u8;

enum class InputSourceType : u32
{
	Vita,
	Count,
};

struct InputBindingKey
{
	u32 source_type = 0;
	u32 source_index = 0;
	u32 data = 0;
};

namespace InputManager
{
	using GenericInputBindingMapping = std::vector<std::pair<GenericInputBinding, std::string>>;

	enum VitaPadButton : u32
	{
		VitaPadButton_Select = 1u << 0,
		VitaPadButton_Start = 1u << 1,
		VitaPadButton_Up = 1u << 2,
		VitaPadButton_Right = 1u << 3,
		VitaPadButton_Down = 1u << 4,
		VitaPadButton_Left = 1u << 5,
		VitaPadButton_L1 = 1u << 6,
		VitaPadButton_R1 = 1u << 7,
		VitaPadButton_Triangle = 1u << 8,
		VitaPadButton_Circle = 1u << 9,
		VitaPadButton_Cross = 1u << 10,
		VitaPadButton_Square = 1u << 11,
	};

	enum class VitaPadAutoFireButton : u8
	{
		None,
		Cross,
		Circle,
	};

	const char* InputSourceToString(InputSourceType type);
	bool GetInputSourceDefaultEnabled(InputSourceType type);
	GenericInputBindingMapping GetGenericBindingMapping(const std::string_view name);
	std::vector<std::string_view> SplitChord(const std::string_view chord);
	const std::vector<const HotkeyInfo*>& GetHotkeyList();

	void ReloadSources(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock);
	void ReloadBindings(const SettingsInterface& si, const SettingsInterface& binding_si,
		const SettingsInterface& hotkey_binding_si, bool force, bool clear_existing);
	void CloseSources();
	void PollSources();
	void InvalidateVitaPadStateCache();
	bool ConfigureVitaPadAutoFire(VitaPadAutoFireButton button, u32 pressed_frames,
		u32 released_frames, bool accept_physical_input = true);
	// Finite pulses: "frame:Button[+Button]:duration;...". Frame zero is the
	// first guest VSync after ELF entry. Every pulse has an automatic release.
	bool ConfigureVitaPadFrameScript(std::string_view script);
	void NotifyVitaPadElfEntry();
	void BeginVitaPadDeterministicReplay();
	void ResetVitaPadAutomation();
	void AdvanceVitaPadAutomationFrame();
	void PauseVibration();
	void SetPadVibrationIntensity(u32 pad, float large_or_single_motor, float small_motor);

#if defined(VITASX2_QEMU_VALIDATION)
	void SetVitaPadSnapshotForTesting(u32 buttons, u8 lx, u8 ly, u8 rx, u8 ry);
	void ResetVitaPadFastPathCountersForTesting();
	u32 GetVitaPadAppliedSnapshotsForTesting();
	u32 GetVitaPadSkippedSnapshotsForTesting();
	u32 GetVitaPadAnalogWritesForTesting();
	u32 GetVitaPadButtonWritesForTesting();
#endif
} // namespace InputManager
