// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadNotConnected.h"
#include "SIO/Sio.h"

#include "common/SettingsInterface.h"

#include "fmt/format.h"

#include <array>
#include <memory>

namespace Pad
{
	static std::array<std::unique_ptr<PadBase>, NUM_CONTROLLER_PORTS> s_controllers;

	static PadBase* EnsurePad(u8 unified_slot)
	{
		if (unified_slot >= NUM_CONTROLLER_PORTS)
			unified_slot = 0;

		if (!s_controllers[unified_slot])
			s_controllers[unified_slot] = std::make_unique<PadNotConnected>(unified_slot);

		return s_controllers[unified_slot].get();
	}
} // namespace Pad

bool Pad::Initialize()
{
	for (u8 i = 0; i < NUM_CONTROLLER_PORTS; i++)
		EnsurePad(i);
	return true;
}

void Pad::Shutdown()
{
	for (auto& controller : s_controllers)
		controller.reset();
}

Pad::ControllerType Pad::GetDefaultPadType(u32 pad)
{
	return ControllerType::NotConnected;
}

void Pad::LoadConfig(const SettingsInterface& si)
{
	Initialize();
}

void Pad::SetDefaultControllerConfig(SettingsInterface& si)
{
}

void Pad::SetDefaultHotkeyConfig(SettingsInterface& si)
{
}

void Pad::ClearPortBindings(SettingsInterface& si, u32 port)
{
}

void Pad::CopyConfiguration(SettingsInterface* dest_si, const SettingsInterface& src_si, bool copy_pad_config,
	bool copy_pad_bindings, bool copy_hotkey_bindings)
{
}

const std::vector<std::pair<const char*, const char*>> Pad::GetControllerTypeNames()
{
	return {{"None", "Not Connected"}};
}

const Pad::ControllerInfo* Pad::GetControllerInfo(ControllerType type)
{
	return &PadNotConnected::ControllerInfo;
}

const Pad::ControllerInfo* Pad::GetControllerInfoByName(const std::string_view name)
{
	return &PadNotConnected::ControllerInfo;
}

const Pad::ControllerInfo* Pad::GetConfigControllerType(const SettingsInterface& si, const char* section, u32 port)
{
	return &PadNotConnected::ControllerInfo;
}

bool Pad::MapController(SettingsInterface& si, u32 controller, const std::vector<std::pair<GenericInputBinding, std::string>>& mapping)
{
	return false;
}

std::vector<std::string> Pad::GetInputProfileNames()
{
	return {};
}

std::string Pad::GetConfigSection(u32 pad_index)
{
	return fmt::format("Pad{}", pad_index + 1);
}

bool Pad::HasConnectedPad(u8 unifiedSlot)
{
	return false;
}

PadBase* Pad::GetPad(u8 port, u8 slot)
{
	return EnsurePad(sioConvertPortAndSlotToPad(port, slot));
}

PadBase* Pad::GetPad(const u8 unifiedSlot)
{
	return EnsurePad(unifiedSlot);
}

void Pad::SetControllerState(u32 controller, u32 bind, float value)
{
}

bool Pad::Freeze(StateWrapper& sw)
{
	return true;
}

void Pad::SetMacroButtonState(InputBindingKey& key, u32 pad, u32 index, bool state)
{
}

void Pad::UpdateMacroButtons()
{
}

const char* Pad::ControllerInfo::GetLocalizedName() const
{
	return display_name;
}

std::optional<u32> Pad::ControllerInfo::GetBindIndex(const std::string_view name) const
{
	return std::nullopt;
}
