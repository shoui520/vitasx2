// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "Input/InputManager.h"

#include "Config.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"

#include <algorithm>
#include <mutex>
#include <string_view>

#if !defined(VITASX2_QEMU_VALIDATION)
#include <psp2/ctrl.h>
#endif

namespace
{
	struct VitaPadSnapshot
	{
		u32 buttons = 0;
		u8 lx = Pad::ANALOG_NEUTRAL_POSITION;
		u8 ly = Pad::ANALOG_NEUTRAL_POSITION;
		u8 rx = Pad::ANALOG_NEUTRAL_POSITION;
		u8 ry = Pad::ANALOG_NEUTRAL_POSITION;

		bool operator==(const VitaPadSnapshot&) const = default;
	};

	VitaPadSnapshot s_last_applied_snapshot;
	PadBase* s_last_applied_pad = nullptr;
	bool s_last_applied_snapshot_valid = false;

#if defined(VITASX2_QEMU_VALIDATION)
	VitaPadSnapshot s_qemu_snapshot;
	u32 s_qemu_applied_snapshots = 0;
	u32 s_qemu_skipped_snapshots = 0;
#else
	bool s_ctrl_initialized = false;
#endif

	u8 DigitalPressure(bool pressed)
	{
		return pressed ? 0xff : 0x00;
	}

	void SetPressureButton(PadBase* pad, u32 index, bool pressed, u8 pressure)
	{
		pad->SetRawPressureButton(index, {pressed, pressed ? pressure : 0x00});
	}

	void ApplyVitaPadState(u32 buttons, u8 lx, u8 ly, u8 rx, u8 ry)
	{
		PadBase* pad = Pad::GetPad(0);
		if (!pad || pad->GetType() != Pad::ControllerType::DualShock2)
		{
			InputManager::InvalidateVitaPadStateCache();
			return;
		}

		const VitaPadSnapshot snapshot = {buttons, lx, ly, rx, ry};
		if (s_last_applied_snapshot_valid && s_last_applied_pad == pad && s_last_applied_snapshot == snapshot)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			s_qemu_skipped_snapshots++;
#endif
			return;
		}

		s_last_applied_snapshot = snapshot;
		s_last_applied_pad = pad;
		s_last_applied_snapshot_valid = true;
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemu_applied_snapshots++;
#endif

		pad->SetRawAnalogs({lx, ly}, {rx, ry});

		SetPressureButton(pad, PadDualshock2::PAD_SELECT, (buttons & InputManager::VitaPadButton_Select) != 0,
			DigitalPressure((buttons & InputManager::VitaPadButton_Select) != 0));
		SetPressureButton(pad, PadDualshock2::PAD_START, (buttons & InputManager::VitaPadButton_Start) != 0,
			DigitalPressure((buttons & InputManager::VitaPadButton_Start) != 0));
		SetPressureButton(pad, PadDualshock2::PAD_UP, (buttons & InputManager::VitaPadButton_Up) != 0,
			DigitalPressure((buttons & InputManager::VitaPadButton_Up) != 0));
		SetPressureButton(pad, PadDualshock2::PAD_RIGHT, (buttons & InputManager::VitaPadButton_Right) != 0,
			DigitalPressure((buttons & InputManager::VitaPadButton_Right) != 0));
		SetPressureButton(pad, PadDualshock2::PAD_DOWN, (buttons & InputManager::VitaPadButton_Down) != 0,
			DigitalPressure((buttons & InputManager::VitaPadButton_Down) != 0));
		SetPressureButton(pad, PadDualshock2::PAD_LEFT, (buttons & InputManager::VitaPadButton_Left) != 0,
			DigitalPressure((buttons & InputManager::VitaPadButton_Left) != 0));
		SetPressureButton(pad, PadDualshock2::PAD_L1, (buttons & InputManager::VitaPadButton_L1) != 0,
			DigitalPressure((buttons & InputManager::VitaPadButton_L1) != 0));
		SetPressureButton(pad, PadDualshock2::PAD_R1, (buttons & InputManager::VitaPadButton_R1) != 0,
			DigitalPressure((buttons & InputManager::VitaPadButton_R1) != 0));
		SetPressureButton(pad, PadDualshock2::PAD_TRIANGLE, (buttons & InputManager::VitaPadButton_Triangle) != 0,
			DigitalPressure((buttons & InputManager::VitaPadButton_Triangle) != 0));
		SetPressureButton(pad, PadDualshock2::PAD_CIRCLE, (buttons & InputManager::VitaPadButton_Circle) != 0,
			DigitalPressure((buttons & InputManager::VitaPadButton_Circle) != 0));
		SetPressureButton(pad, PadDualshock2::PAD_CROSS, (buttons & InputManager::VitaPadButton_Cross) != 0,
			DigitalPressure((buttons & InputManager::VitaPadButton_Cross) != 0));
		SetPressureButton(pad, PadDualshock2::PAD_SQUARE, (buttons & InputManager::VitaPadButton_Square) != 0,
			DigitalPressure((buttons & InputManager::VitaPadButton_Square) != 0));
	}

#if !defined(VITASX2_QEMU_VALIDATION)
	void EnsureCtrlInitialized()
	{
		if (s_ctrl_initialized)
			return;

		if (sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE) < 0)
			sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
		s_ctrl_initialized = true;
	}

	u32 TranslateVitaButtons(const SceCtrlData& pad)
	{
		u32 buttons = 0;
		buttons |= (pad.buttons & SCE_CTRL_SELECT) ? InputManager::VitaPadButton_Select : 0;
		buttons |= (pad.buttons & SCE_CTRL_START) ? InputManager::VitaPadButton_Start : 0;
		buttons |= (pad.buttons & SCE_CTRL_UP) ? InputManager::VitaPadButton_Up : 0;
		buttons |= (pad.buttons & SCE_CTRL_RIGHT) ? InputManager::VitaPadButton_Right : 0;
		buttons |= (pad.buttons & SCE_CTRL_DOWN) ? InputManager::VitaPadButton_Down : 0;
		buttons |= (pad.buttons & SCE_CTRL_LEFT) ? InputManager::VitaPadButton_Left : 0;
		buttons |= (pad.buttons & (SCE_CTRL_L1 | SCE_CTRL_LTRIGGER)) ? InputManager::VitaPadButton_L1 : 0;
		buttons |= (pad.buttons & (SCE_CTRL_R1 | SCE_CTRL_RTRIGGER)) ? InputManager::VitaPadButton_R1 : 0;
		buttons |= (pad.buttons & SCE_CTRL_TRIANGLE) ? InputManager::VitaPadButton_Triangle : 0;
		buttons |= (pad.buttons & SCE_CTRL_CIRCLE) ? InputManager::VitaPadButton_Circle : 0;
		buttons |= (pad.buttons & SCE_CTRL_CROSS) ? InputManager::VitaPadButton_Cross : 0;
		buttons |= (pad.buttons & SCE_CTRL_SQUARE) ? InputManager::VitaPadButton_Square : 0;
		return buttons;
	}
#endif
} // namespace

const char* InputManager::InputSourceToString(InputSourceType type)
{
	switch (type)
	{
		case InputSourceType::Vita:
			return "Vita";
		default:
			return "Unknown";
	}
}

bool InputManager::GetInputSourceDefaultEnabled(InputSourceType type)
{
	return (type == InputSourceType::Vita);
}

InputManager::GenericInputBindingMapping InputManager::GetGenericBindingMapping(const std::string_view name)
{
	if (name != "Vita")
		return {};

	return {
		{GenericInputBinding::DPadUp, "Vita/ButtonUp"},
		{GenericInputBinding::DPadRight, "Vita/ButtonRight"},
		{GenericInputBinding::DPadDown, "Vita/ButtonDown"},
		{GenericInputBinding::DPadLeft, "Vita/ButtonLeft"},
		{GenericInputBinding::Triangle, "Vita/ButtonTriangle"},
		{GenericInputBinding::Circle, "Vita/ButtonCircle"},
		{GenericInputBinding::Cross, "Vita/ButtonCross"},
		{GenericInputBinding::Square, "Vita/ButtonSquare"},
		{GenericInputBinding::Select, "Vita/ButtonSelect"},
		{GenericInputBinding::Start, "Vita/ButtonStart"},
		{GenericInputBinding::L1, "Vita/ButtonL"},
		{GenericInputBinding::R1, "Vita/ButtonR"},
		{GenericInputBinding::LeftStickLeft, "Vita/LeftStickLeft"},
		{GenericInputBinding::LeftStickRight, "Vita/LeftStickRight"},
		{GenericInputBinding::LeftStickUp, "Vita/LeftStickUp"},
		{GenericInputBinding::LeftStickDown, "Vita/LeftStickDown"},
		{GenericInputBinding::RightStickLeft, "Vita/RightStickLeft"},
		{GenericInputBinding::RightStickRight, "Vita/RightStickRight"},
		{GenericInputBinding::RightStickUp, "Vita/RightStickUp"},
		{GenericInputBinding::RightStickDown, "Vita/RightStickDown"},
	};
}

std::vector<std::string_view> InputManager::SplitChord(const std::string_view chord)
{
	std::vector<std::string_view> parts;
	size_t start = 0;
	while (start < chord.size())
	{
		const size_t end = chord.find('&', start);
		const std::string_view part = (end == std::string_view::npos) ? chord.substr(start) : chord.substr(start, end - start);
		if (!part.empty())
			parts.push_back(part);
		if (end == std::string_view::npos)
			break;
		start = end + 1;
	}
	return parts;
}

const std::vector<const HotkeyInfo*>& InputManager::GetHotkeyList()
{
	static const std::vector<const HotkeyInfo*> empty;
	return empty;
}

void InputManager::ReloadSources(const SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	(void)si;
	(void)settings_lock;
	InvalidateVitaPadStateCache();
#if !defined(VITASX2_QEMU_VALIDATION)
	s_ctrl_initialized = false;
	EnsureCtrlInitialized();
#endif
}

void InputManager::ReloadBindings(const SettingsInterface& si, const SettingsInterface& binding_si,
	const SettingsInterface& hotkey_binding_si, bool force, bool clear_existing)
{
	(void)si;
	(void)binding_si;
	(void)hotkey_binding_si;
	(void)force;
	(void)clear_existing;
}

void InputManager::CloseSources()
{
	InvalidateVitaPadStateCache();
	PauseVibration();
}

void InputManager::InvalidateVitaPadStateCache()
{
	s_last_applied_snapshot = {};
	s_last_applied_pad = nullptr;
	s_last_applied_snapshot_valid = false;
}

void InputManager::PollSources()
{
#if defined(VITASX2_QEMU_VALIDATION)
	ApplyVitaPadState(s_qemu_snapshot.buttons, s_qemu_snapshot.lx, s_qemu_snapshot.ly, s_qemu_snapshot.rx, s_qemu_snapshot.ry);
#else
	EnsureCtrlInitialized();

	SceCtrlData pad = {};
	if (sceCtrlPeekBufferPositiveExt(0, &pad, 1) < 1)
	{
		if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1)
			return;
	}

	ApplyVitaPadState(TranslateVitaButtons(pad), pad.lx, pad.ly, pad.rx, pad.ry);
#endif
}

void InputManager::PauseVibration()
{
	SetPadVibrationIntensity(0, 0.0f, 0.0f);
}

void InputManager::SetPadVibrationIntensity(u32 pad, float large_or_single_motor, float small_motor)
{
	(void)pad;
	(void)large_or_single_motor;
	(void)small_motor;

#if !defined(VITASX2_QEMU_VALIDATION)
	if (pad != 0)
		return;

	SceCtrlActuator state = {};
	state.large = static_cast<u8>(std::clamp(large_or_single_motor, 0.0f, 1.0f) * 255.0f);
	state.small = static_cast<u8>(std::clamp(small_motor, 0.0f, 1.0f) * 255.0f);
	sceCtrlSetActuator(1, &state);
#endif
}

#if defined(VITASX2_QEMU_VALIDATION)
void InputManager::SetVitaPadSnapshotForTesting(u32 buttons, u8 lx, u8 ly, u8 rx, u8 ry)
{
	s_qemu_snapshot = {buttons, lx, ly, rx, ry};
}

void InputManager::ResetVitaPadFastPathCountersForTesting()
{
	s_qemu_applied_snapshots = 0;
	s_qemu_skipped_snapshots = 0;
}

u32 InputManager::GetVitaPadAppliedSnapshotsForTesting()
{
	return s_qemu_applied_snapshots;
}

u32 InputManager::GetVitaPadSkippedSnapshotsForTesting()
{
	return s_qemu_skipped_snapshots;
}
#endif
