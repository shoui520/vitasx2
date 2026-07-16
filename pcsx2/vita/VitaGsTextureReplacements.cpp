// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/HW/GSTextureReplacements.h"

// PCSX2 owner: GSTextureReplacements.cpp and
// GSTextureReplacementLoaders.cpp. Vita's product configuration disables
// dumping and replacement loading; keep the texture-cache ABI explicit while
// excluding desktop image codecs, filesystem scanning and a worker thread.
namespace GSTextureReplacements
{
	void Initialize() {}
	void GameChanged() {}
	void ReloadReplacementMap() {}
	void UpdateConfig(Pcsx2Config::GSOptions& old_config) { (void)old_config; }
	void Shutdown() {}

	u32 CalcMipmapLevelsForReplacement(u32 width, u32 height)
	{
		(void)width;
		(void)height;
		return 1;
	}

	bool HasAnyReplacementTextures() { return false; }
	bool HasReplacementTextureWithOtherPalette(const GSTextureCache::HashCacheKey& hash)
	{
		(void)hash;
		return false;
	}

	GSTexture* LookupReplacementTexture(const GSTextureCache::HashCacheKey& hash, bool mipmap,
		bool* pending, std::pair<u8, u8>* alpha_minmax)
	{
		(void)hash;
		(void)mipmap;
		(void)alpha_minmax;
		if (pending)
			*pending = false;
		return nullptr;
	}

	GSTexture* CreateReplacementTexture(const ReplacementTexture& rtex, bool mipmap)
	{
		(void)rtex;
		(void)mipmap;
		return nullptr;
	}

	void ProcessAsyncLoadedTextures() {}
	void DumpTexture(const GSTextureCache::HashCacheKey& hash, const GIFRegTEX0& TEX0,
		const GIFRegTEXA& TEXA, GSTextureCache::SourceRegion region, GSLocalMemory& mem, u32 level)
	{
		(void)hash;
		(void)TEX0;
		(void)TEXA;
		(void)region;
		(void)mem;
		(void)level;
	}
	void ClearDumpedTextureList() {}
	u32 GetDumpedTextureCount() { return 0; }
	u32 GetLoadedTextureCount() { return 0; }
	ReplacementTextureLoader GetLoader(const std::string_view filename)
	{
		(void)filename;
		return nullptr;
	}
	bool SavePNGImage(const std::string& filename, u32 width, u32 height,
		const u8* buffer, u32 pitch)
	{
		(void)filename;
		(void)width;
		(void)height;
		(void)buffer;
		(void)pitch;
		return false;
	}
} // namespace GSTextureReplacements
