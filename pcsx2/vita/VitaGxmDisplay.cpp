// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "VitaGxmDisplay.h"

#if !defined(VITASX2_QEMU_VALIDATION)

#include <cstring>

namespace VitaGXM
{
	Display::Display() = default;

	// Destruction is deliberately non-owning. Impl::Shutdown() must first end
	// and drain all GXM work before explicitly destroying display resources. If
	// that ordered shutdown fails, an implicit retry here could free memory still
	// referenced by an indeterminate GPU job or display callback.
	Display::~Display() = default;

	int Display::Initialize()
	{
		const int destroy_result = Destroy();
		if (destroy_result < 0)
			return destroy_result;

		m_front_index = 0;
		m_back_index = 1;

		for (std::uint32_t i = 0; i < BufferCount; i++)
		{
			const int allocation_result = AllocateMappedBlock(
				"VitaSX2 GXM display", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
				ColorBufferSize, SCE_GXM_MEMORY_ATTRIB_RW, &m_color_buffers[i]);
			if (allocation_result < 0)
			{
				Destroy();
				return allocation_result;
			}

			// Sony's basic sample initializes displayable CDRAM before making it
			// visible. Black is a safe initial value, not a diagnostic fill.
			std::memset(m_color_buffers[i].base, 0, ColorBufferSize);

			const int surface_result = sceGxmColorSurfaceInit(
				&m_color_surfaces[i], SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
				SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
				SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, Width, Height, StrideInPixels,
				m_color_buffers[i].base);
			if (surface_result < 0)
			{
				Destroy();
				return surface_result;
			}

			const int sync_result = sceGxmSyncObjectCreate(&m_sync_objects[i]);
			if (sync_result < 0 || !m_sync_objects[i])
			{
				Destroy();
				return sync_result < 0 ? sync_result :
				                         static_cast<int>(SCE_GXM_ERROR_INVALID_POINTER);
			}
		}

		const int display_result = SetInitialFrontBuffer();
		if (display_result < 0)
		{
			Destroy();
			return display_result;
		}

		m_ready = true;
		return 0;
	}

	int Display::Finish()
	{
		if (!m_queue_has_entries)
			return 0;

		const int result = sceGxmDisplayQueueFinish();
		if (result >= 0)
			m_queue_has_entries = false;
		return result;
	}

	int Display::Destroy()
	{
		// Sony's basic sample drains the display queue before destroying any
		// sync object or display buffer. A failed drain leaves all resources
		// intact so an in-flight callback can never observe freed CDRAM.
		const int finish_result = Finish();
		if (finish_result < 0)
			return finish_result;

		m_ready = false;

		// Sony's api_libgxm/basic sample owns this teardown order: after the
		// display queue is empty, release each display buffer and then destroy its
		// sync object. The public display contract does not document a NULL
		// sceDisplaySetFrameBuf request, so do not invent one here.
		int first_error = 0;
		for (std::uint32_t i = 0; i < BufferCount; i++)
		{
			if (m_color_buffers[i].IsMapped())
				std::memset(m_color_buffers[i].base, 0, ColorBufferSize);
			if (m_color_buffers[i].IsAllocated())
			{
				const int release_result = ReleaseMappedBlock(&m_color_buffers[i]);
				if (release_result < 0 && first_error == 0)
					first_error = release_result;
			}
			if (m_sync_objects[i])
			{
				const int sync_result =
					sceGxmSyncObjectDestroy(m_sync_objects[i]);
				if (sync_result >= 0)
					m_sync_objects[i] = nullptr;
				else if (first_error == 0)
					first_error = sync_result;
			}
		}

		if (first_error < 0)
			return first_error;

		std::memset(m_color_surfaces, 0, sizeof(m_color_surfaces));
		m_front_index = 0;
		m_back_index = 1;
		return 0;
	}

	bool Display::IsReady() const
	{
		if (!m_ready)
			return false;

		for (std::uint32_t i = 0; i < BufferCount; i++)
		{
			if (!m_color_buffers[i].IsMapped() || !m_sync_objects[i])
				return false;
		}
		return true;
	}

	SceGxmColorSurface* Display::BackColorSurface()
	{
		return IsReady() ? &m_color_surfaces[m_back_index] : nullptr;
	}

	SceGxmSyncObject* Display::FrontSyncObject() const
	{
		return IsReady() ? m_sync_objects[m_front_index] : nullptr;
	}

	SceGxmSyncObject* Display::BackSyncObject() const
	{
		return IsReady() ? m_sync_objects[m_back_index] : nullptr;
	}

	void* Display::BackBufferAddress() const
	{
		return IsReady() ? m_color_buffers[m_back_index].base : nullptr;
	}

	int Display::QueuePresent()
	{
		if (!IsReady())
			return SCE_GXM_ERROR_UNINITIALIZED;

		const std::uint32_t queued_back = m_back_index;
		const CallbackData callback_data = {m_color_buffers[queued_back].base};
		const int result =
			sceGxmDisplayQueueAddEntry(m_sync_objects[m_front_index],
				m_sync_objects[queued_back], &callback_data);
		if (result < 0)
			return result;

		m_queue_has_entries = true;
		m_front_index = queued_back;
		m_back_index = (queued_back + 1u) % BufferCount;
		return 0;
	}

	std::uint32_t Display::DisplayQueueCallbackDataSize()
	{
		return sizeof(CallbackData);
	}

	void Display::DisplayQueueCallback(const void* callback_data)
	{
		const CallbackData* data = static_cast<const CallbackData*>(callback_data);
		if (!data || !data->address)
			return;

		SceDisplayFrameBuf frame = {};
		frame.size = sizeof(frame);
		frame.base = data->address;
		frame.pitch = StrideInPixels;
		frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
		frame.width = Width;
		frame.height = Height;

		// Sony PSP2 SDK gxm/display_queue.h and api_libgxm/common own this
		// sequence: request the next-VSync flip, then wait for that request to
		// complete so libgxm cannot reuse the old front buffer too early.
		if (sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME) >= 0)
			sceDisplayWaitSetFrameBuf();
	}

	SceDisplayFrameBuf Display::MakeFrameBuffer(std::uint32_t index) const
	{
		SceDisplayFrameBuf frame = {};
		frame.size = sizeof(frame);
		frame.base = m_color_buffers[index].base;
		frame.pitch = StrideInPixels;
		frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
		frame.width = Width;
		frame.height = Height;
		return frame;
	}

	int Display::SetInitialFrontBuffer()
	{
		const SceDisplayFrameBuf frame = MakeFrameBuffer(m_front_index);
		const int set_result =
			sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
		if (set_result < 0)
			return set_result;

		return sceDisplayWaitSetFrameBuf();
	}
} // namespace VitaGXM

#endif
