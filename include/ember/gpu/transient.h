#pragma once

#include <ember/core/bits.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>

#include <atomic>

namespace ember::gpu
{
	/// A per-frame allocation from the transient ring. Valid until the frame that made it retires.
	struct TransientAllocation
	{
		void* cpu = nullptr; // write-combined: write sequentially, never read back
		BufferHandle buffer{};
		u32 offset = 0;
		u32 size   = 0;

		[[nodiscard]] bool valid() const noexcept { return cpu != nullptr; }
	};

	template <class T> struct TransientArray
	{
		T* data = nullptr;
		BufferHandle buffer{};
		u32 offset = 0;
		u32 count  = 0;

		/// Element index of data[0] when the buffer is read as a T array (offset is a multiple of sizeof(T)).
		[[nodiscard]] u32 first_element() const noexcept { return offset / static_cast<u32>(sizeof(T)); }

		[[nodiscard]] bool valid() const noexcept { return data != nullptr; }
	};

	/**
	 * The per-frame bump allocator.
	 *
	 * The fast path is this header: one wait-free fetch_add, a bounds check, and pointer math. Inlined into
	 * callers; at transient call rates a function call would cost more than the allocation. The cold path
	 * (slice exhausted) tail-calls the backend through one function pointer, which keeps backend types
	 * out of this header and out of the ABI.
	 *
	 * THREADING
	 *	allocate() from any thread. The atomic only partitions space between concurrent callers;
	 *	*publication* of the written bytes is the frame submit, so relaxed ordering is correct and costs no fences.
	 *
	 * LIFETIME
	 *	Everything allocated here dies when the current frame's slot retires. There is no free().
	 */
	class TransientAllocator
	{
	public:
		[[nodiscard]] EMBER_FINLINE TransientAllocation allocate(u32 size, u32 alignment) noexcept
		{
			EMBER_ASSERT(size != 0 && is_power_of_two(alignment));

			// Pad-then-align: one wait-free fetch_add instead of a CAS retry loop. Wastes at most
			// alignment-1 bytes per call which is noise against a per-frame reset.
			const u64 begin	  = m_cursor.fetch_add(static_cast<u64>(size) + alignment - 1, std::memory_order_relaxed);
			const u64 aligned = align_up(begin, static_cast<u64>(alignment));

			if (aligned + size > m_limit) [[unlikely]]
				return m_overflow(m_context, size, alignment);

			return {
				.cpu	= m_cpu + aligned,
				.buffer = m_buffer,
				.offset = static_cast<u32>(aligned),
				.size	= size,
			};
		}

		/**
		 * count elements of T, placed so data[0] is element offset/sizeof(T) of a T[] view over the whole buffer.
		 * The offset is rounded to an exact element multiple
		 */
		template <class T> [[nodiscard]] TransientArray<T> allocate_array(u32 count) noexcept
		{
			static_assert(std::is_trivially_copyable_v<T>, "transient memory is memcpy territory");

			constexpr u64 stride = sizeof(T);
			const u64 bytes		 = stride * count;
			EMBER_ASSERT(count != 0 && bytes <= UINT32_MAX);

			const u64 begin	  = m_cursor.fetch_add(bytes + stride - 1, std::memory_order_relaxed);
			const u64 aligned = ((begin + stride - 1) / stride) * stride;

			if (aligned + bytes > m_limit) [[unlikely]]
			{
				// One-off overflow buffers start at offset 0: trivially element-aligned.
				const TransientAllocation raw = m_overflow(m_context, static_cast<u32>(bytes), alignof(T));
				return {
					static_cast<T*>(raw.cpu),
					raw.buffer,
					raw.offset,
					raw.valid() ? count : 0,
				};
			}

			return {reinterpret_cast<T*>(m_cpu + aligned), m_buffer, static_cast<u32>(aligned), count};
		}

		/// Bytes consumed this frame (clamped: failed bumps overshoot the limit).
		[[nodiscard]] u64 used(u64 slice_begin) const noexcept
		{
			return std::min(m_cursor.load(std::memory_order_relaxed), m_limit) - slice_begin;
		}

		/// Backend wiring, called at boot and each begin_frame. Not user API.
		void bind(
			u8* cpu,
			BufferHandle buffer,
			u64 begin,
			u64 limit,
			TransientAllocation (*overflow)(void*, u32, u32),
			void* context) noexcept
		{
			m_cpu	   = cpu;
			m_buffer   = buffer;
			m_limit	   = limit;
			m_overflow = overflow;
			m_context  = context;
			m_cursor.store(begin, std::memory_order_relaxed);
		}

	private:
		static TransientAllocation null_overflow(void*, u32, u32) noexcept { return {}; }

		std::atomic<u64> m_cursor{0};
		u64 m_limit = 0; // 0 (default state): every allocation takes the overflow path
		u8* m_cpu	= nullptr;
		BufferHandle m_buffer{};
		TransientAllocation (*m_overflow)(void*, u32, u32) = &null_overflow;
		void* m_context									   = nullptr;
	};
}
