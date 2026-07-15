// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#if !defined(VITASX2_QEMU_VALIDATION)

#include "VitaGxmMemory.h"

#include <psp2/display.h>
#include <psp2/gxm.h>

#include <cstdint>

namespace VitaGXM
{
	class Display
	{
	public:
		static constexpr std::uint32_t Width = 960;
		static constexpr std::uint32_t Height = 544;
		static constexpr std::uint32_t StrideInPixels = 1024;
		static constexpr std::uint32_t BufferCount = 2;
		static constexpr std::uint32_t ColorBufferSize =
			StrideInPixels * Height * sizeof(std::uint32_t);

		Display();
		~Display();

		Display(const Display&) = delete;
		Display& operator=(const Display&) = delete;

		// libgxm must already be initialized with DisplayQueueCallback and
		// DisplayQueueCallbackDataSize. Initialize installs a black, complete
		// front buffer before returning so the display never scans an unfinished
		// render target.
		int Initialize();
		int Finish();
		int Destroy();

		bool IsReady() const;
		SceGxmColorSurface* BackColorSurface();
		SceGxmSyncObject* FrontSyncObject() const;
		SceGxmSyncObject* BackSyncObject() const;
		void* BackBufferAddress() const;

		// QueuePresent follows the old/new per-buffer synchronization contract
		// in Sony's graphics/api_libgxm/basic sample and rotates only after the
		// queue accepted the entry. It deliberately does not finish the queue.
		int QueuePresent();

		static std::uint32_t DisplayQueueCallbackDataSize();
		static void DisplayQueueCallback(const void* callback_data);

	private:
		struct CallbackData
		{
			void* address;
		};

		SceDisplayFrameBuf MakeFrameBuffer(std::uint32_t index) const;
		int SetInitialFrontBuffer();

		MappedBlock m_color_buffers[BufferCount]{};
		SceGxmColorSurface m_color_surfaces[BufferCount]{};
		SceGxmSyncObject* m_sync_objects[BufferCount]{};
		std::uint32_t m_front_index = 0;
		std::uint32_t m_back_index = 1;
		bool m_ready = false;
		bool m_queue_has_entries = false;
	};
} // namespace VitaGXM

#endif
