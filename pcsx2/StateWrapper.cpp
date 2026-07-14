// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "StateWrapper.h"
#include "common/Console.h"
#include <cinttypes>
#include <cstring>

StateWrapper::IStream::~IStream() = default;

StateWrapper::StateWrapper(IStream* stream, Mode mode, u32 version, DataFormat data_format)
	: m_stream(stream)
	, m_mode(mode)
	, m_version(version)
	, m_data_format(data_format)
{
}

StateWrapper::~StateWrapper() = default;

void StateWrapper::DoBytes(void* data, size_t length)
{
	if (m_mode == Mode::Read)
	{
		if (m_error || (m_error |= (m_stream->Read(data, static_cast<u32>(length)) != static_cast<u32>(length))) == true)
			std::memset(data, 0, length);
	}
	else
	{
		if (!m_error)
			m_error |= (m_stream->Write(data, static_cast<u32>(length)) != static_cast<u32>(length));
	}
}

void StateWrapper::Do(bool* value_ptr)
{
	if (m_mode == Mode::Read)
	{
		u8 value = 0;
		if (!m_error)
			m_error |= (m_stream->Read(&value, sizeof(value)) != sizeof(value));
		*value_ptr = (value != 0);
	}
	else
	{
		u8 value = static_cast<u8>(*value_ptr);
		if (!m_error)
			m_error |= (m_stream->Write(&value, sizeof(value)) != sizeof(value));
	}
}

void StateWrapper::Do(std::string* value_ptr)
{
	static constexpr u32 PORTABLE_REPLAY_MAX_STRING_LENGTH = 4096;

	if (m_mode == Mode::Write && IsPortableReplay() &&
		value_ptr->length() > PORTABLE_REPLAY_MAX_STRING_LENGTH)
	{
		m_error = true;
		return;
	}

	u32 length = static_cast<u32>(value_ptr->length());
	Do(&length);
	if (m_mode == Mode::Read)
	{
		if (m_error)
		{
			value_ptr->clear();
			return;
		}

		if (IsPortableReplay())
		{
			// Portable data is an untrusted, fixed-schema replay envelope. Prove
			// both the schema bound and the bytes remaining before allocating.
			const u32 data_position = m_stream->GetPosition();
			if (length > PORTABLE_REPLAY_MAX_STRING_LENGTH ||
				!m_stream->SeekRelative(static_cast<s32>(length)) ||
				!m_stream->SeekAbsolute(data_position))
			{
				m_error = true;
				value_ptr->clear();
				return;
			}
		}

		value_ptr->resize(length);
	}
	if (length > 0)
		DoBytes(value_ptr->data(), length);
	if (m_mode == Mode::Read && !m_error)
		value_ptr->resize(std::strlen(value_ptr->c_str()));
}

bool StateWrapper::DoMarker(const char* marker)
{
	std::string file_value(marker);
	Do(&file_value);
	if (m_error)
		return false;

	if (m_mode == Mode::Write || file_value == marker)
		return true;

	Console.WriteLn("Marker mismatch at offset %u: found '%s' expected '%s'", m_stream->GetPosition(),
		file_value.c_str(), marker);

	return false;
}

StateWrapper::ReadOnlyMemoryStream::ReadOnlyMemoryStream(const void* buf, u32 buf_length)
	: m_buf(static_cast<const u8*>(buf))
	, m_buf_length(buf_length)
{
}

u32 StateWrapper::ReadOnlyMemoryStream::Read(void* buf, u32 count)
{
	count = std::min(m_buf_length - m_buf_position, count);
	if (count > 0)
	{
		std::memcpy(buf, &m_buf[m_buf_position], count);
		m_buf_position += count;
	}
	return count;
}

u32 StateWrapper::ReadOnlyMemoryStream::Write(const void* buf, u32 count)
{
	return 0;
}

u32 StateWrapper::ReadOnlyMemoryStream::GetPosition()
{
	return m_buf_position;
}

bool StateWrapper::ReadOnlyMemoryStream::SeekAbsolute(u32 pos)
{
	if (pos > m_buf_length)
		return false;

	m_buf_position = pos;
	return true;
}

bool StateWrapper::ReadOnlyMemoryStream::SeekRelative(s32 count)
{
	if (count < 0)
	{
		if (static_cast<u32>(-count) > m_buf_position)
			return false;

		m_buf_position -= static_cast<u32>(-count);
		return true;
	}
	else
	{
		if ((m_buf_position + static_cast<u32>(count)) > m_buf_length)
			return false;

		m_buf_position += static_cast<u32>(count);
		return true;
	}
}

StateWrapper::MemoryStream::MemoryStream(void* buf, u32 buf_length)
	: m_buf(static_cast<u8*>(buf))
	, m_buf_length(buf_length)
{
}

u32 StateWrapper::MemoryStream::Read(void* buf, u32 count)
{
	count = std::min(m_buf_length - m_buf_position, count);
	if (count > 0)
	{
		std::memcpy(buf, &m_buf[m_buf_position], count);
		m_buf_position += count;
	}
	return count;
}

u32 StateWrapper::MemoryStream::Write(const void* buf, u32 count)
{
	count = std::min(m_buf_length - m_buf_position, count);
	if (count > 0)
	{
		std::memcpy(&m_buf[m_buf_position], buf, count);
		m_buf_position += count;
	}
	return count;
}

u32 StateWrapper::MemoryStream::GetPosition()
{
	return m_buf_position;
}

bool StateWrapper::MemoryStream::SeekAbsolute(u32 pos)
{
	if (pos > m_buf_length)
		return false;

	m_buf_position = pos;
	return true;
}

bool StateWrapper::MemoryStream::SeekRelative(s32 count)
{
	if (count < 0)
	{
		if (static_cast<u32>(-count) > m_buf_position)
			return false;

		m_buf_position -= static_cast<u32>(-count);
		return true;
	}
	else
	{
		if ((m_buf_position + static_cast<u32>(count)) > m_buf_length)
			return false;

		m_buf_position += static_cast<u32>(count);
		return true;
	}
}

StateWrapper::VectorMemoryStream::VectorMemoryStream() = default;

StateWrapper::VectorMemoryStream::VectorMemoryStream(u32 reserve)
{
	m_buf.reserve(reserve);
}

u32 StateWrapper::VectorMemoryStream::Read(void* buf, u32 count)
{
	count = std::min(static_cast<u32>(m_buf.size() - m_buf_position), count);
	if (count > 0)
	{
		std::memcpy(buf, &m_buf[m_buf_position], count);
		m_buf_position += count;
	}
	return count;
}

u32 StateWrapper::VectorMemoryStream::Write(const void* buf, u32 count)
{
	if (count > 0)
	{
		Expand(m_buf_position + count);
		std::memcpy(&m_buf[m_buf_position], buf, count);
		m_buf_position += count;
	}

	return count;
}

u32 StateWrapper::VectorMemoryStream::GetPosition()
{
	return m_buf_position;
}

bool StateWrapper::VectorMemoryStream::SeekAbsolute(u32 pos)
{
	if (pos > m_buf.size())
		return false;

	m_buf_position = pos;
	return true;
}

bool StateWrapper::VectorMemoryStream::SeekRelative(s32 count)
{
	if (count < 0)
	{
		if (static_cast<u32>(-count) > m_buf_position)
			return false;

		m_buf_position -= static_cast<u32>(-count);
		return true;
	}
	else
	{
		if ((m_buf_position + static_cast<u32>(count)) > m_buf.size())
			return false;

		m_buf_position += static_cast<u32>(count);
		return true;
	}
}

void StateWrapper::VectorMemoryStream::Expand(u32 new_size)
{
	if (m_buf.size() >= new_size)
		return;

	// don't grow more than 4K at a time
	const u32 grow_size = std::min((m_buf.size() > 4096u) ? 4096u : static_cast<u32>(m_buf.size()), static_cast<u32>(m_buf.size() - new_size));
	m_buf.reserve(m_buf.size() + grow_size);

	// should take care of growth, right?
	m_buf.resize(new_size);
}
