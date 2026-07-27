// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "Input/InputManager.h"

#include "Config.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"

#include <algorithm>
#include <limits>
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
	InputManager::VitaPadAutoFireButton s_auto_fire_button =
		InputManager::VitaPadAutoFireButton::None;
	u32 s_auto_fire_pressed_frames = 1;
	u32 s_auto_fire_released_frames = 1;
	u32 s_auto_fire_phase = 0;
	bool s_auto_fire_active = false;

#if defined(VITASX2_QEMU_VALIDATION)
	VitaPadSnapshot s_qemu_snapshot;
	u32 s_qemu_applied_snapshots = 0;
	u32 s_qemu_skipped_snapshots = 0;
	u32 s_qemu_analog_writes = 0;
	u32 s_qemu_button_writes = 0;
#else
	bool s_ctrl_initialized = false;
#endif

	constexpr u32 VITA_PAD_BUTTON_MASK =
		InputManager::VitaPadButton_Select |
		InputManager::VitaPadButton_Start |
		InputManager::VitaPadButton_Up |
		InputManager::VitaPadButton_Right |
		InputManager::VitaPadButton_Down |
		InputManager::VitaPadButton_Left |
		InputManager::VitaPadButton_L1 |
		InputManager::VitaPadButton_R1 |
		InputManager::VitaPadButton_Triangle |
		InputManager::VitaPadButton_Circle |
		InputManager::VitaPadButton_Cross |
		InputManager::VitaPadButton_Square;

	u8 DigitalPressure(bool pressed)
	{
		return pressed ? 0xff : 0x00;
	}

	u32 GetVitaPadAutoFireMask()
	{
		if (!s_auto_fire_active || s_auto_fire_phase >= s_auto_fire_pressed_frames)
			return 0;

		switch (s_auto_fire_button)
		{
			case InputManager::VitaPadAutoFireButton::Cross:
				return InputManager::VitaPadButton_Cross;
			case InputManager::VitaPadAutoFireButton::Circle:
				return InputManager::VitaPadButton_Circle;
			default:
				return 0;
		}
	}

	void SetPressureButton(PadBase* pad, u32 index, bool pressed, u8 pressure)
	{
		pad->SetRawPressureButton(index, {pressed, pressed ? pressure : 0x00});
	}

	void SetPressureButtonIfChanged(PadBase* pad, u32 changed_buttons, u32 buttons, u32 mask, u32 index)
	{
		if ((changed_buttons & mask) == 0)
			return;

		const bool pressed = (buttons & mask) != 0;
		SetPressureButton(pad, index, pressed, DigitalPressure(pressed));
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemu_button_writes++;
#endif
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
		const bool can_diff_from_previous = s_last_applied_snapshot_valid && s_last_applied_pad == pad;
		const VitaPadSnapshot previous_snapshot = s_last_applied_snapshot;
		if (can_diff_from_previous && previous_snapshot == snapshot)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			s_qemu_skipped_snapshots++;
#endif
			return;
		}

		if (!can_diff_from_previous ||
			previous_snapshot.lx != lx ||
			previous_snapshot.ly != ly ||
			previous_snapshot.rx != rx ||
			previous_snapshot.ry != ry)
		{
			pad->SetRawAnalogs({lx, ly}, {rx, ry});
#if defined(VITASX2_QEMU_VALIDATION)
			s_qemu_analog_writes++;
#endif
		}

		const u32 changed_buttons = can_diff_from_previous ?
			((previous_snapshot.buttons ^ buttons) & VITA_PAD_BUTTON_MASK) :
			VITA_PAD_BUTTON_MASK;

		s_last_applied_snapshot = snapshot;
		s_last_applied_pad = pad;
		s_last_applied_snapshot_valid = true;
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemu_applied_snapshots++;
#endif

		SetPressureButtonIfChanged(pad, changed_buttons, buttons, InputManager::VitaPadButton_Select,
			PadDualshock2::PAD_SELECT);
		SetPressureButtonIfChanged(pad, changed_buttons, buttons, InputManager::VitaPadButton_Start,
			PadDualshock2::PAD_START);
		SetPressureButtonIfChanged(pad, changed_buttons, buttons, InputManager::VitaPadButton_Up,
			PadDualshock2::PAD_UP);
		SetPressureButtonIfChanged(pad, changed_buttons, buttons, InputManager::VitaPadButton_Right,
			PadDualshock2::PAD_RIGHT);
		SetPressureButtonIfChanged(pad, changed_buttons, buttons, InputManager::VitaPadButton_Down,
			PadDualshock2::PAD_DOWN);
		SetPressureButtonIfChanged(pad, changed_buttons, buttons, InputManager::VitaPadButton_Left,
			PadDualshock2::PAD_LEFT);
		SetPressureButtonIfChanged(pad, changed_buttons, buttons, InputManager::VitaPadButton_L1,
			PadDualshock2::PAD_L1);
		SetPressureButtonIfChanged(pad, changed_buttons, buttons, InputManager::VitaPadButton_R1,
			PadDualshock2::PAD_R1);
		SetPressureButtonIfChanged(pad, changed_buttons, buttons, InputManager::VitaPadButton_Triangle,
			PadDualshock2::PAD_TRIANGLE);
		SetPressureButtonIfChanged(pad, changed_buttons, buttons, InputManager::VitaPadButton_Circle,
			PadDualshock2::PAD_CIRCLE);
		SetPressureButtonIfChanged(pad, changed_buttons, buttons, InputManager::VitaPadButton_Cross,
			PadDualshock2::PAD_CROSS);
		SetPressureButtonIfChanged(pad, changed_buttons, buttons, InputManager::VitaPadButton_Square,
			PadDualshock2::PAD_SQUARE);
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
	ResetVitaPadAutoFire();
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
	ApplyVitaPadState(s_qemu_snapshot.buttons | GetVitaPadAutoFireMask(),
		s_qemu_snapshot.lx, s_qemu_snapshot.ly, s_qemu_snapshot.rx, s_qemu_snapshot.ry);
#else
	EnsureCtrlInitialized();

	SceCtrlData pad = {};
	if (sceCtrlPeekBufferPositiveExt(0, &pad, 1) < 1)
	{
		if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1)
			return;
	}

	ApplyVitaPadState(TranslateVitaButtons(pad) | GetVitaPadAutoFireMask(),
		pad.lx, pad.ly, pad.rx, pad.ry);
#endif
}

bool InputManager::ConfigureVitaPadAutoFire(
	VitaPadAutoFireButton button, u32 pressed_frames, u32 released_frames)
{
	if (button != VitaPadAutoFireButton::None &&
		(pressed_frames == 0 || released_frames == 0 ||
			pressed_frames > std::numeric_limits<u32>::max() - released_frames))
	{
		return false;
	}

	s_auto_fire_button = button;
	s_auto_fire_pressed_frames = std::max(pressed_frames, 1u);
	s_auto_fire_released_frames = std::max(released_frames, 1u);
	ResetVitaPadAutoFire();
	return true;
}

void InputManager::NotifyVitaPadElfEntry()
{
	s_auto_fire_active = (s_auto_fire_button != VitaPadAutoFireButton::None);
	s_auto_fire_phase = s_auto_fire_active ?
		(s_auto_fire_pressed_frames + s_auto_fire_released_frames - 1) : 0;
	InvalidateVitaPadStateCache();
}

void InputManager::ResetVitaPadAutoFire()
{
	s_auto_fire_phase = 0;
	s_auto_fire_active = false;
	InvalidateVitaPadStateCache();
}

void InputManager::AdvanceVitaPadAutoFireFrame()
{
	if (!s_auto_fire_active)
		return;

	const u32 period = s_auto_fire_pressed_frames + s_auto_fire_released_frames;
	s_auto_fire_phase++;
	if (s_auto_fire_phase == period)
		s_auto_fire_phase = 0;
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
	s_qemu_analog_writes = 0;
	s_qemu_button_writes = 0;
}

u32 InputManager::GetVitaPadAppliedSnapshotsForTesting()
{
	return s_qemu_applied_snapshots;
}

u32 InputManager::GetVitaPadSkippedSnapshotsForTesting()
{
	return s_qemu_skipped_snapshots;
}

u32 InputManager::GetVitaPadAnalogWritesForTesting()
{
	return s_qemu_analog_writes;
}

u32 InputManager::GetVitaPadButtonWritesForTesting()
{
	return s_qemu_button_writes;
}
#endif
