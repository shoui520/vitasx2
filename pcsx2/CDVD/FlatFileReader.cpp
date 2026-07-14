// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "FlatFileReader.h"

#include "CDVD/CdvdCopy.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Error.h"
#include "common/ProgressCallback.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

#if defined(__vita__)
#include <psp2/io/fcntl.h>
#endif

static constexpr size_t CHUNK_SIZE = 128 * 1024;

FlatFileReader::FlatFileReader() = default;

FlatFileReader::~FlatFileReader()
{
#if defined(__vita__)
	pxAssert(m_file < 0);
#else
	pxAssert(!m_file);
#endif
}

#if defined(__vita__)
static bool VitaReadFileAt(SceUID fd, void* destination, u64 offset, size_t size)
{
	u8* const bytes = static_cast<u8*>(destination);
	size_t completed = 0;
	while (completed < size)
	{
		const size_t remaining = size - completed;
		const SceSize request = static_cast<SceSize>(
			std::min<size_t>(remaining, std::numeric_limits<SceSize>::max()));
		const SceSSize result = sceIoPread(fd, bytes + completed, request,
			static_cast<SceOff>(offset + completed));
		if (result <= 0 || static_cast<SceSize>(result) > request)
			return false;
		completed += static_cast<size_t>(result);
	}
	return true;
}
#endif

bool FlatFileReader::Open2(std::string filename, Error* error)
{
	m_filename = std::move(filename);
#if defined(__vita__)
	// Sony PSP2 owner: kernel/iofilemgr.h::{sceIoOpen,sceIoLseek}. Vita
	// newlib's off_t is 32-bit and its libc does not export the nominal
	// fseeko64/ftello64 entry points, so flat DVD images must retain the native
	// 64-bit SceOff contract all the way through the reader.
	m_file = sceIoOpen(m_filename.c_str(), SCE_O_RDONLY, 0);
	if (m_file < 0)
	{
		Error::SetStringFmt(error, "sceIoOpen('{}') failed with 0x{:08x}.",
			m_filename, static_cast<u32>(m_file));
		return false;
	}

	const SceOff filesize = sceIoLseek(m_file, 0, SCE_SEEK_END);
	if (filesize <= 0 || sceIoLseek(m_file, 0, SCE_SEEK_SET) < 0)
	{
		Error::SetStringView(error, "Failed to determine flat-disc file size.");
		Close2();
		return false;
	}
#else
	if (!(m_file = FileSystem::OpenCFile(m_filename.c_str(), "rb", error)))
		return false;

	const s64 filesize = FileSystem::FSize64(m_file);
	if (filesize <= 0)
	{
		Error::SetStringView(error, "Failed to determine file size.");
		Close2();
		return false;
	}
#endif

	m_file_size = static_cast<u64>(filesize);
	return true;
}

bool FlatFileReader::Precache2(ProgressCallback* progress, Error* error)
{
#if defined(__vita__)
	if (m_file < 0 || !CheckAvailableMemoryForPrecaching(m_file_size, error))
		return false;
#else
	if (!m_file || !CheckAvailableMemoryForPrecaching(m_file_size, error))
		return false;
#endif

	m_file_cache = std::make_unique_for_overwrite<u8[]>(m_file_size);
#if defined(__vita__)
	progress->SetProgressRange(100);
	u64 completed = 0;
	while (completed < m_file_size)
	{
		const size_t amount = static_cast<size_t>(
			std::min<u64>(m_file_size - completed, CHUNK_SIZE));
		if (!VitaReadFileAt(m_file, m_file_cache.get() + completed, completed, amount))
		{
			Error::SetStringFmt(error,
				"sceIoPread('{}') failed at offset {}.", m_filename, completed);
			m_file_cache.reset();
			return false;
		}
		completed += amount;
		progress->SetProgressValue(static_cast<u32>((completed * 100) / m_file_size));
	}

	sceIoClose(m_file);
	m_file = -1;
#else
	if (FileSystem::FSeek64(m_file, 0, SEEK_SET) != 0 ||
		FileSystem::ReadFileWithProgress(
			m_file, m_file_cache.get(), m_file_size, progress, error) != m_file_size)
	{
		m_file_cache.reset();
		return false;
	}

	std::fclose(m_file);
	m_file = nullptr;
#endif
	return true;
}

ThreadedFileReader::Chunk FlatFileReader::ChunkForOffset(u64 offset)
{
	ThreadedFileReader::Chunk chunk = {};
	if (offset >= m_file_size)
	{
		chunk.chunkID = -1;
	}
	else
	{
		chunk.chunkID = offset / CHUNK_SIZE;
		chunk.length = static_cast<u32>(std::min<u64>(m_file_size - offset, CHUNK_SIZE));
		chunk.offset = static_cast<u64>(chunk.chunkID) * CHUNK_SIZE;
	}

	return chunk;
}

int FlatFileReader::ReadChunk(void* dst, s64 blockID)
{
	if (blockID < 0)
		return -1;

	const u64 file_offset = static_cast<u64>(blockID) * CHUNK_SIZE;
	if (file_offset >= m_file_size)
		return -1;
	if (m_file_cache)
	{
		const u64 read_size = std::min<u64>(m_file_size - file_offset, CHUNK_SIZE);
		CdvdCopyBytes(dst, &m_file_cache[file_offset], read_size);
		return static_cast<int>(read_size);
	}

	const u32 read_size = static_cast<u32>(std::min<u64>(m_file_size - file_offset, CHUNK_SIZE));

#if defined(__vita__)
	// Sony PSP2 owner: kernel/iofilemgr.h::sceIoPread(). Positional reads keep
	// the complete SceOff and remain correct if the threaded reader later gains
	// more than one in-flight request.
	return VitaReadFileAt(m_file, dst, file_offset, read_size) ?
		static_cast<int>(read_size) : -1;
#else
	if (FileSystem::FSeek64(m_file, file_offset, SEEK_SET) != 0)
		return -1;
	return (std::fread(dst, read_size, 1, m_file) == 1) ? static_cast<int>(read_size) : 0;
#endif
}

void FlatFileReader::Close2()
{
#if defined(__vita__)
	if (m_file < 0)
		return;

	sceIoClose(m_file);
	m_file = -1;
#else
	if (!m_file)
		return;

	std::fclose(m_file);
	m_file = nullptr;
#endif
	m_file_size = 0;
}

u32 FlatFileReader::GetBlockCount() const
{
	return static_cast<u32>(m_file_size / m_blocksize);
}
