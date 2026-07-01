// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct HotkeyInfo;
enum class GenericInputBinding : u8;

enum class InputSourceType : u32
{
	Unknown,
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

	static inline const char* InputSourceToString(InputSourceType type)
	{
		return "Vita";
	}

	static inline bool GetInputSourceDefaultEnabled(InputSourceType type)
	{
		return false;
	}

	static inline GenericInputBindingMapping GetGenericBindingMapping(const std::string_view name)
	{
		return {};
	}

	static inline std::vector<std::string_view> SplitChord(const std::string_view chord)
	{
		return {};
	}

	static inline const std::vector<const HotkeyInfo*>& GetHotkeyList()
	{
		static const std::vector<const HotkeyInfo*> empty;
		return empty;
	}

	static inline void SetPadVibrationIntensity(u32 pad, float large_or_single_motor, float small_motor)
	{
	}
} // namespace InputManager
