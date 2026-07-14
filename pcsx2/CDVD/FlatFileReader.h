// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "CDVD/ThreadedFileReader.h"

#include <cstdio>

#if defined(__vita__)
#include <psp2common/types.h>
#endif

class FlatFileReader final : public ThreadedFileReader
{
	DeclareNoncopyableObject(FlatFileReader);

#if defined(__vita__)
	SceUID m_file = -1;
#else
	std::FILE* m_file = nullptr;
#endif
	std::unique_ptr<u8[]> m_file_cache;
	u64 m_file_size = 0;

public:
	FlatFileReader();
	~FlatFileReader() override;

	bool Open2(std::string filename, Error* error) override;

	bool Precache2(ProgressCallback* progress, Error* error) override;

	Chunk ChunkForOffset(u64 offset) override;
	int ReadChunk(void* dst, s64 blockID) override;

	void Close2() override;

	u32 GetBlockCount() const override;
};
