// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSCapture.h"
#include "GS.h"

#include "common/Threading.h"

void GSEndCapture()
{
}

bool GSCapture::BeginCapture(float fps, GSVector2i recommendedResolution, float aspect, std::string filename)
{
	return false;
}

bool GSCapture::DeliverVideoFrame(GSTexture* stex)
{
	return false;
}

void GSCapture::DeliverAudioPacket(const float* frames)
{
}

void GSCapture::EndCapture()
{
}

bool GSCapture::IsCapturing()
{
	return false;
}

bool GSCapture::IsCapturingVideo()
{
	return false;
}

bool GSCapture::IsCapturingAudio()
{
	return false;
}

TinyString GSCapture::GetElapsedTime()
{
	return {};
}

const Threading::ThreadHandle& GSCapture::GetEncoderThreadHandle()
{
	static Threading::ThreadHandle handle;
	return handle;
}

GSVector2i GSCapture::GetSize()
{
	return {};
}

std::string GSCapture::GetNextCaptureFileName()
{
	return {};
}

void GSCapture::Flush()
{
}

GSCapture::CodecList GSCapture::GetVideoCodecList(const char* container)
{
	return {};
}

GSCapture::CodecList GSCapture::GetAudioCodecList(const char* container)
{
	return {};
}

GSCapture::FormatList GSCapture::GetVideoFormatList(const char* codec)
{
	return {};
}
