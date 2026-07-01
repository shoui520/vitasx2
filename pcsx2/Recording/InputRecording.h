// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

class InputRecording
{
public:
	bool isActive() const { return false; }
};

inline InputRecording g_InputRecording;

inline bool InputRecordingFreeze()
{
	return true;
}
