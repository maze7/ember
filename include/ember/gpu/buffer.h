#pragma once

#include <ember/containers/span.h>
#include <ember/core/bitmask.h>
#include <ember/core/common.h>

namespace ember::gpu
{
	enum class BufferUsage : u8
	{
		None	 = 0,
		Vertex	 = 1 << 0,
		Index	 = 1 << 1,
		Constant = 1 << 2, // bindable through the set-1 dynamic constant slots
		Storage	 = 1 << 3, // lives in the bindless SSBO heap
		Indirect = 1 << 4,
		CopySrc	 = 1 << 5,
		CopyDst	 = 1 << 6,
	};

	EMBER_ENUM_BITWISE_OPS(BufferUsage, u8);

	/// Where the memory lives and how the CPU touches it.
	enum class MemoryLocation : u8
	{
		DeviceLocal, // VRAM, CPU writes go through update_buffer (staged GPU copy).
		Upload,		 // Host-visible, persistenly mapped, write-combined: write-only for the CPU.
		Readback,	 // Host-visible, cached: the GPU copies in, the CPU reads via mapped().
	};

	struct BufferDef
	{
		const char* name	  = "buffer";
		u64 size			  = 0;
		BufferUsage usage	  = BufferUsage::None;
		MemoryLocation memory = MemoryLocation::DeviceLocal;

		// Uploaded before this frame's GPU work. DeviceLocal stages through the ring;
		// Upload memcpys straight into the mapping.
		Span<const u8> initial_data = {};
	};
}
