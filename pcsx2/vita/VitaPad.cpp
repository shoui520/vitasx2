// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "SIO/Pad/PadNotConnected.h"
#include "SIO/Sio.h"

#include "common/SettingsInterface.h"

#include "fmt/format.h"

#include <array>
#include <memory>

namespace Pad
{
	static std::array<std::unique_ptr<PadBase>, NUM_CONTROLLER_PORTS> s_controllers;

	static ControllerType GetDefaultControllerType(u32 unified_slot)
	{
		return (unified_slot == 0) ? ControllerType::DualShock2 : ControllerType::NotConnected;
	}

	static std::unique_ptr<PadBase> CreatePad(ControllerType type, u8 unified_slot, size_t eject_ticks = 0)
	{
		switch (type)
		{
			case ControllerType::DualShock2:
				return std::make_unique<PadDualshock2>(unified_slot, eject_ticks);
			case ControllerType::NotConnected:
			default:
				return std::make_unique<PadNotConnected>(unified_slot, eject_ticks);
		}
	}

	static PadBase* EnsurePad(u8 unified_slot)
	{
		if (unified_slot >= NUM_CONTROLLER_PORTS)
			unified_slot = 0;

		if (!s_controllers[unified_slot])
			s_controllers[unified_slot] = CreatePad(GetDefaultControllerType(unified_slot), unified_slot);

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
	return GetDefaultControllerType(pad);
}

void Pad::LoadConfig(const SettingsInterface& si)
{
	for (u8 i = 0; i < NUM_CONTROLLER_PORTS; i++)
	{
		const std::string section = GetConfigSection(i);
		const ControllerInfo* ci = GetConfigControllerType(si, section.c_str(), i);
		s_controllers[i] = CreatePad(ci ? ci->type : GetDefaultControllerType(i), i);
	}
}

void Pad::SetDefaultControllerConfig(SettingsInterface& si)
{
	for (u32 i = 0; i < NUM_CONTROLLER_PORTS; i++)
	{
		const std::string section = GetConfigSection(i);
		const ControllerInfo* ci = GetControllerInfo(GetDefaultPadType(i));
		si.SetStringValue(section.c_str(), "Type", ci->name);
	}
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
	return {
		{PadNotConnected::ControllerInfo.name, PadNotConnected::ControllerInfo.display_name},
		{PadDualshock2::ControllerInfo.name, PadDualshock2::ControllerInfo.display_name},
	};
}

const Pad::ControllerInfo* Pad::GetControllerInfo(ControllerType type)
{
	switch (type)
	{
		case ControllerType::DualShock2:
			return &PadDualshock2::ControllerInfo;
		case ControllerType::NotConnected:
		default:
			return &PadNotConnected::ControllerInfo;
	}
}

const Pad::ControllerInfo* Pad::GetControllerInfoByName(const std::string_view name)
{
	if (name == PadDualshock2::ControllerInfo.name)
		return &PadDualshock2::ControllerInfo;
	if (name == PadNotConnected::ControllerInfo.name)
		return &PadNotConnected::ControllerInfo;

	return nullptr;
}

const Pad::ControllerInfo* Pad::GetConfigControllerType(const SettingsInterface& si, const char* section, u32 port)
{
	const ControllerInfo* default_info = GetControllerInfo(GetDefaultPadType(port));
	const std::string type = si.GetStringValue(section, "Type", default_info->name);
	const ControllerInfo* info = GetControllerInfoByName(type);
	return info ? info : default_info;
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
	if (unifiedSlot >= NUM_CONTROLLER_PORTS)
		return false;

	return EnsurePad(unifiedSlot)->GetType() != ControllerType::NotConnected;
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
	if (controller >= NUM_CONTROLLER_PORTS)
		return;

	EnsurePad(static_cast<u8>(controller))->Set(bind, value);
}

bool Pad::Freeze(StateWrapper& sw)
{
	if (!sw.DoMarker("PAD"))
		return false;

	for (u8 i = 0; i < NUM_CONTROLLER_PORTS; i++)
	{
		ControllerType type = EnsurePad(i)->GetType();
		sw.Do(&type);
		if (sw.IsReading())
			s_controllers[i] = CreatePad(type, i);

		if (!EnsurePad(i)->Freeze(sw))
			return false;
	}

	return !sw.HasError();
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
	for (u32 i = 0; i < static_cast<u32>(bindings.size()); i++)
	{
		if (name == bindings[i].name)
			return i;
	}

	return std::nullopt;
}
