#include <ember/core/common.h>
#include <ember/sync/thread.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/transient_ring.h>

#include <algorithm>

namespace ember::gpu::vk
{
	namespace
	{
		/// Transient memory serves every bindable class at once: one ring packs better than per-class
		/// rings, and buffers pay nothing for extra usage bits on our targets.
		constexpr VkBufferUsageFlags TRANSIENT_USAGE =
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		/// Host-visible, persitently mapped, pool-registered. On ReBAR/SAM adapters the memory prefers VRAM
		/// (the CPU writes across the bus once; the GPU reads it hot every draw). Pre-ReBAR we deliberately
		/// prefer host memory: the legacy 256 MB BAR window is too scarce to spend on a ring.
		[[nodiscard]] bool
		create_transient_buffer(Backend& backend, u64 size, const char* name, TransientPage& out) noexcept
		{
			VkBufferCreateInfo buffer_info{
				.sType		 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size		 = size,
				.usage		 = TRANSIENT_USAGE,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			};

			VmaAllocationCreateInfo alloc_info{};
			alloc_info.flags =
				VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			alloc_info.usage = backend.context.caps.host_visible_device_local ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
																			  : VMA_MEMORY_USAGE_AUTO_PREFER_HOST;

			VkBuffer buffer			 = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VmaAllocationInfo result{};

			if (VkResult vr = vmaCreateBuffer(
					backend.context.allocator, &buffer_info, &alloc_info, &buffer, &allocation, &result);
				vr != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: transient buffer '{}' ({} bytes) failed: {}", name, size, result_name(vr));
				return false;
			}

			VkBufferDeviceAddressInfo address_info{
				.sType	= VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
				.buffer = buffer,
			};

			BufferHandle handle = backend.resources.buffers.insert(
				BufferHot{.handle = buffer, .address = vkGetBufferDeviceAddress(backend.context.device, &address_info)},
				BufferCold{.allocation = allocation, .size = size, .mapped = result.pMappedData});

			if (handle.is_null())
			{
				EMBER_ERROR("gpu: buffer pool exhausted creating '{}'", name);
				vmaDestroyBuffer(backend.context.allocator, buffer, allocation);
				return false;
			}

			VkMemoryPropertyFlags properties = 0;
			vmaGetAllocationMemoryProperties(backend.context.allocator, allocation, &properties);

			set_name(backend.context, VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(buffer), name);

			out = {
				.handle		= handle,
				.allocation = allocation,
				.cpu		= static_cast<u8*>(result.pMappedData),
				.size		= size,
				.used		= 0,
				.coherent	= (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0,
			};
			return true;
		}

		/// Bump within a page. `alignment` may be a stride (non-power-of-two): the generic divide only
		/// runs on this cold path, never on the wait-free one.
		[[nodiscard]] TransientAllocation page_alloc(TransientPage& page, u32 size, u32 alignment) noexcept
		{
			const u64 aligned = ((page.used + alignment - 1) / alignment) * alignment;

			if (aligned + size > page.size)
				return {};

			page.used = aligned + size;
			return {
				.cpu	= page.cpu + aligned,
				.buffer = page.handle,
				.offset = static_cast<u32>(aligned),
				.size	= size,
			};
		}
	}

	bool transient_boot(Backend& backend, u64 per_slot_bytes) noexcept
	{
		TransientRing& ring = backend.transient_ring;

		// Slices start at k * slice_bytes, so aligning the slice size to the strictest bindable
		// alignment makes every slice start bindable by construction.
		const u64 alignment = std::max<u64>(
			256,
			std::max(
				backend.context.caps.constant_buffer_offset_alignment,
				backend.context.caps.storage_buffer_offset_alignment));
		u64 slice = ((per_slot_bytes + alignment - 1) / alignment) * alignment;

		// TransientAllocation::offset is 32-bit; a ring the u32 can't address is a config error
		// worth surviving. Clamp loudly rather than fail.
		const u64 max_slice = (u64{UINT32_MAX} / backend.context.frames_in_flight) / alignment * alignment;
		if (slice > max_slice)
		{
			EMBER_WARN("gpu: transient_ring_bytes clamped from {} to {} (u32 offset limit)", slice, max_slice);
			slice = max_slice;
		}

		TransientPage as_page{};
		if (!create_transient_buffer(
				backend, slice * backend.context.frames_in_flight, "ember.transient_ring", as_page))
			return false;

		ring.handle		 = as_page.handle;
		ring.allocation	 = as_page.allocation;
		ring.cpu		 = as_page.cpu;
		ring.coherent	 = as_page.coherent;
		ring.slice_bytes = slice;
		ring.page_bytes	 = std::clamp<u64>(slice / 4, 1_mb, 16_mb);

		if (!ring.coherent)
			EMBER_WARN("gpu: transient memory is not host-coherent; end_frame flushes explicitly");

		return true;
	}

	void transient_destroy(Backend& backend) noexcept
	{
		TransientRing& ring = backend.transient_ring;

		// A stray late allocation reports invalid instead of scribbling on freed memory.
		backend.transient.bind(nullptr, {}, 0, 0, nullptr, nullptr);

		// No lock: the caller guarantees the GPU is idle and every worker has stopped.
		const auto release = [&backend](TransientPage& page)
		{
			if (const BufferHot* hot = backend.resources.buffers.try_get(page.handle))
			{
				vmaDestroyBuffer(backend.context.allocator, hot->handle, page.allocation);
				(void)backend.resources.buffers.erase(page.handle);
			}

			page = {};
		};

		release(ring.active);

		for (TransientPage& page : ring.frame_pages)
			release(page);

		for (RetiredPage& retired : ring.retired)
			release(retired.page);

		for (TransientPage& page : ring.free_pages)
			release(page);

		ring.frame_pages.clear();
		ring.retired.clear();
		ring.free_pages.clear();
		ring.page_count = 0;

		// The ring buffer itself is just another pool-registered buffer.
		TransientPage as_page{.handle = ring.handle, .allocation = ring.allocation};
		release(as_page);

		ring.handle			= {};
		ring.allocation		= VK_NULL_HANDLE;
		ring.cpu			= nullptr;
		ring.slice_bytes	= 0;
		ring.frame_begin	= 0;
		ring.overflow_bytes = 0;
	}

	void transient_begin_frame(Backend& backend, u32 slot) noexcept
	{
		TransientRing& ring = backend.transient_ring;

		{
			std::lock_guard lock(ring.mutex);

			// Recycle pages whose frame provably completed. Stamps are monotonic (frames retire in order),
			// so this is the destroy queue's exact-prefix walk again.
			u32 recycled = 0;
			while (recycled < ring.retired.size() && ring.retired[recycled].value <= backend.frame.completed)
			{
				TransientPage page = ring.retired[recycled].page;
				page.used		   = 0;
				ring.free_pages.push_back(page);
				++recycled;
			}

			if (recycled != 0)
				ring.retired.erase(ring.retired.begin(), ring.retired.begin() + recycled);

			// Starvation: a worker overflowed with no page available (workers cannot touch the pool).
			// Create one now, on the owner thread, so next frame self-heals.
			if (u32 starved = ring.starved.exchange(0, std::memory_order_relaxed); starved != 0)
			{
				EMBER_ERROR("gpu: {} transient allocation dropped last frame (no overflow page)", starved);

				if (ring.free_pages.empty() && ring.page_count < MAX_TRANSIENT_PAGES)
				{
					TransientPage page{};
					if (create_transient_buffer(backend, ring.page_bytes, "ember.transient_page", page))
					{
						ring.free_pages.push_back(page);
						++ring.page_count;
					}
				}
			}
		}

		// Bind the frame's slice. The caller's timeline wait is what proved this slice's previous user
		// retired; the allocator itself needs no further synchronization.
		ring.frame_begin = u64{slot} * ring.slice_bytes;
		backend.transient.bind(
			ring.cpu,
			ring.handle,
			ring.frame_begin,
			ring.frame_begin + ring.slice_bytes,
			&transient_overflow,
			&backend);
	}

	void transient_end_frame(Backend& backend, u64 value) noexcept
	{
		TransientRing& ring		  = backend.transient_ring;
		TransientAllocator& alloc = backend.transient;

		const u64 used = alloc.used(ring.frame_begin);

		// Poison: between frames every allocation reports invalid instead of writing into a slice
		// the GPU may still read. One store buys a structurally enforced lifetime.
		alloc.bind(nullptr, {}, 0, 0, nullptr, nullptr);

		// The submit publishes these bytes; non-coherent memory must be flushed first.
		// Coherent (all desktop in practice) skips the call entirely.
		if (!ring.coherent && used != 0)
			(void)vmaFlushAllocation(backend.context.allocator, ring.allocation, ring.frame_begin, used);

		std::lock_guard lock(ring.mutex);

		if (ring.active.cpu != nullptr)
		{
			ring.frame_pages.push_back(ring.active);
			ring.active = {};
		}

		for (const TransientPage& page : ring.frame_pages)
		{
			if (!page.coherent && page.used != 0)
				(void)vmaFlushAllocation(backend.context.allocator, page.allocation, 0, page.used);

			ring.retired.push_back({.page = page, .value = value});
		}
		ring.frame_pages.clear();

		if (ring.overflow_bytes != 0)
		{
			EMBER_WARN(
				"gpu: transient ring overflowed by {} KiB (ring {}/{} KiB); raise DeviceDef::transient_ring_bytes",
				ring.overflow_bytes / 1024,
				used / 1024,
				ring.slice_bytes / 1024);
			ring.overflow_bytes = 0;
		}
	}

	TransientAllocation transient_overflow(void* context, u32 size, u32 alignment) noexcept
	{
		Backend& backend	= *static_cast<Backend*>(context);
		TransientRing& ring = backend.transient_ring;

		std::lock_guard lock(ring.mutex);
		ring.overflow_bytes += size;

		if (ring.active.cpu != nullptr)
		{
			if (const TransientAllocation out = page_alloc(ring.active, size, alignment); out.valid())
				return out;

			ring.frame_pages.push_back(ring.active);
			ring.active = {};
		}

		// Prefer a recycled page; jumbo requests scan for one big enough.
		for (u32 i = 0; i < ring.free_pages.size(); ++i)
		{
			if (ring.free_pages[i].size >= size)
			{
				ring.active		   = ring.free_pages[i];
				ring.free_pages[i] = ring.free_pages[ring.free_pages.size() - 1];
				ring.free_pages.pop_back();
				break;
			}
		}

		if (ring.active.cpu == nullptr)
		{
			// Page creation inserts into the buffer pool: owner-thread-only by the pool's contract.
			// A worker records starvation and fails this one allocation; the next begin_frame creates
			// the page. We tolerate one degraded frame rather than put a lock on the pool that every
			// create_buffer call site would then pay for.
			if (backend.owner_thread != current_thread_id() || ring.page_count >= MAX_TRANSIENT_PAGES)
			{
				ring.starved.fetch_add(1, std::memory_order_relaxed);
				return {};
			}

			const u64 wanted = std::max<u64>(size, ring.page_bytes); // jumbo request, jumbo page

			if (!create_transient_buffer(backend, wanted, "ember.transient_page", ring.active))
				return {};

			++ring.page_count;
		}

		const TransientAllocation out = page_alloc(ring.active, size, alignment);
		if (!out.valid())
			ring.starved.fetch_add(1, std::memory_order_relaxed);

		return out;
	}
}
