// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#if !defined(VITASX2_QEMU_VALIDATION)

#include "VitaGxmArena.h"

#include "GS/Renderers/Common/GSTexture.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace VitaGXM
{
	struct TextureCompletionFence
	{
		std::uint64_t scene = 0;
		std::uint64_t transfer = 0;
	};

	struct TextureLevelLayout
	{
		std::uint32_t offset = 0;
		std::uint32_t pitch = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		bool tiled = false;
	};

	struct TextureFormatInfo
	{
		GSTexture::Format format = GSTexture::Format::Invalid;
		SceGxmTextureFormat texture_format = {};
		SceGxmTransferFormat transfer_format = {};
		SceGxmColorFormat color_format = {};
		SceGxmOutputRegisterSize output_register_size =
			SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT;
		std::uint8_t bytes_per_pixel = 0;
		bool color_renderable = false;
		bool depth_stencil = false;
	};

	const TextureFormatInfo* GetTextureFormatInfo(GSTexture::Format format);

	class GSTextureGXM;

	// GSDeviceGXM owns scene boundaries and transfer completion. Textures expose
	// storage and descriptors but delegate every operation which can race a GXM
	// scene to this interface. Upload staging is transferred by value: the owner
	// must retain it until the returned transfer serial completes.
	class TextureOwner
	{
	public:
		virtual ~TextureOwner() = default;

		virtual Arena& TextureArena() = 0;
		virtual Arena& TransferArena() = 0;

		virtual bool RetireTextureScene(GSTextureGXM& texture, bool preserve_contents,
			std::uint64_t* scene_serial) = 0;
		virtual bool QueueTextureUpload(GSTextureGXM& texture, std::uint32_t level,
			const GSVector4i& destination,
			std::uint32_t source_pitch,
			ArenaAllocation source,
			std::uint64_t* transfer_serial) = 0;
		virtual bool QueueTextureReadback(GSTextureGXM& texture, std::uint32_t level,
			const GSVector4i& source, void* destination,
			std::uint32_t destination_pitch,
			const GSVector4i& destination_rect,
			std::uint64_t* transfer_serial) = 0;
		virtual bool QueueTextureMipmaps(GSTextureGXM& texture,
			std::uint64_t* transfer_serial) = 0;
		virtual bool WaitForTextureTransfer(std::uint64_t transfer_serial) = 0;

		virtual void RetireTextureAllocation(ArenaAllocation allocation,
			const TextureCompletionFence& fence) = 0;
	};

	class GSTextureGXM final : public GSTexture
	{
	public:
		static std::unique_ptr<GSTextureGXM> Create(TextureOwner* owner, Usage usage,
			int width, int height, int levels,
			Format format,
			int* error = nullptr);
		~GSTextureGXM() override;

		void* GetNativeHandle() const override;
		bool Update(const GSVector4i& r, const void* data, int pitch,
			int layer = 0) override;
		bool Map(GSMap& map, const GSVector4i* rect = nullptr,
			int layer = 0) override;
		void Unmap() override;
		void GenerateMipmap() override;

#ifdef PCSX2_DEVBUILD
		void SetDebugName(std::string_view name) override;
#endif

		const TextureFormatInfo& NativeFormat() const { return m_native_format; }
		const SceGxmTexture& Texture() const { return m_texture; }
		SceGxmTexture& Texture() { return m_texture; }
		const SceGxmColorSurface* ColorSurface() const;
		SceGxmColorSurface* ColorSurface();
		const SceGxmDepthStencilSurface* DepthStencilSurface() const;
		SceGxmDepthStencilSurface* DepthStencilSurface();

		const TextureLevelLayout* Level(std::uint32_t level) const;
		void* LevelData(std::uint32_t level) const;
		bool CopyFromLinear(std::uint32_t level, const GSVector4i& destination,
			const void* source, std::uint32_t source_pitch);
		bool CopyToLinear(std::uint32_t level, const GSVector4i& source,
			void* destination, std::uint32_t destination_pitch) const;
		void* StencilData() const { return m_stencil_storage.Data(); }
		std::uint32_t DepthPitch() const { return m_depth_pitch; }
		std::uint32_t StencilPitch() const { return m_stencil_pitch; }
		std::size_t StorageSize() const { return m_storage.Size(); }
		std::size_t StencilStorageSize() const { return m_stencil_storage.Size(); }

		void MarkSceneUse(std::uint64_t serial);
		void MarkTransferUse(std::uint64_t serial);
		const TextureCompletionFence& CompletionFence() const { return m_fence; }
		bool HadOperationFailure() const { return m_operation_failed; }

	private:
		struct PendingMap
		{
			ArenaAllocation staging;
			GSVector4i rect{};
			std::uint32_t pitch = 0;
			std::uint32_t level = 0;
		};

		explicit GSTextureGXM(TextureOwner* owner);
		int Initialize(Usage usage, int width, int height, int levels, Format format);
		int InitializeColorStorage();
		int InitializeDepthStorage();
		bool QueueUpload(ArenaAllocation staging, std::uint32_t pitch,
			const GSVector4i& rect, std::uint32_t level);
		bool ValidateRect(const GSVector4i& rect, std::uint32_t level) const;
		void RetireStorage();

		TextureOwner* m_owner = nullptr;
		TextureFormatInfo m_native_format{};
		SceGxmTexture m_texture{};
		SceGxmColorSurface m_color_surface{};
		SceGxmDepthStencilSurface m_depth_surface{};
		ArenaAllocation m_storage;
		ArenaAllocation m_stencil_storage;
		std::vector<TextureLevelLayout> m_levels;
		PendingMap m_pending_map;
		TextureCompletionFence m_fence{};
		std::uint32_t m_depth_pitch = 0;
		std::uint32_t m_stencil_pitch = 0;
		bool m_has_color_surface = false;
		bool m_has_depth_surface = false;
		bool m_operation_failed = false;
	};

	class GSDownloadTextureGXM final : public GSDownloadTexture
	{
	public:
		static std::unique_ptr<GSDownloadTextureGXM>
		Create(TextureOwner* owner, std::uint32_t width, std::uint32_t height,
			GSTexture::Format format, int* error = nullptr);
		~GSDownloadTextureGXM() override;

		void CopyFromTexture(const GSVector4i& destination, GSTexture* source_texture,
			const GSVector4i& source, std::uint32_t source_level,
			bool use_transfer_pitch = true) override;
		bool Map(const GSVector4i& read_rect) override;
		void Unmap() override;
		void Flush() override;

#ifdef PCSX2_DEVBUILD
		void SetDebugName(std::string_view name) override;
#endif

		bool HadOperationFailure() const { return m_operation_failed; }
		std::uint64_t PendingTransferSerial() const { return m_pending_transfer; }

	private:
		GSDownloadTextureGXM(TextureOwner* owner, std::uint32_t width,
			std::uint32_t height, GSTexture::Format format);
		int Initialize();
		bool ValidateRect(const GSVector4i& rect) const;

		TextureOwner* m_owner = nullptr;
		ArenaAllocation m_storage;
		TextureCompletionFence m_fence{};
		std::uint64_t m_pending_transfer = 0;
		std::uint32_t m_storage_pitch = 0;
		bool m_operation_failed = false;
#ifdef PCSX2_DEVBUILD
		std::string m_debug_name;
#endif
	};
} // namespace VitaGXM

#endif
