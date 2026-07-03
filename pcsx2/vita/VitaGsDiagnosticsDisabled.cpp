// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSDump.h"
#include "GS/GSPng.h"

void GSDumpBase::ReadFIFO(u32 size)
{
	(void)size;
}

void GSDumpBase::Transfer(int index, const u8* mem, size_t size)
{
	(void)index;
	(void)mem;
	(void)size;
}

bool GSPng::Save(GSPng::Format fmt, const std::string& file, const u8* image, int w, int h, int pitch, int compression, bool rb_swapped)
{
	(void)fmt;
	(void)file;
	(void)image;
	(void)w;
	(void)h;
	(void)pitch;
	(void)compression;
	(void)rb_swapped;
	return false;
}
