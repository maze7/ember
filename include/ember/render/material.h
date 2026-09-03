#pragma once

#include <ember/containers/pool.h>
#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/buffer.h>
#include <ember/gpu/common.h>
#include <ember/render/common.h>

namespace ember::gpu
{
	class Device;
}

namespace ember::render
{
	struct MaterialPoolDef
	{
		const char* name = "materials";

		/// Bytes per material, the size of the owning shader family's GPU struct.
		/// A multiple of 16 so std430 strides match the C++ layout.
		u32 stride = 0;

		u32 capacity = 256;

		/**
		 * One record of stride bytes that reads as an obvious mistake on screen
		 * (fallback texture under a white tint, full roughness, no discard).
		 * It permanently owns slot 0, so a null handle renders as the error
		 * material, and destroy writes it over freed slots. An all-zero record
		 * is not enough; a zero tint would multiply the fallback to black.
		 */
		Span<const u8> error_record = {};
	};

	[[nodiscard]] constexpr bool is_valid(const MaterialPoolDef& def) noexcept
	{
		return def.name != nullptr && def.stride != 0 && def.stride % 16 == 0 && def.capacity > 1 &&
			   def.capacity <= 65536 && def.error_record.size() == def.stride;
	}

	/**
	 * One shader family's material storage: a bindless table of fixed stride
	 * records addressed by MaterialHandle index, which is what ObjectData's
	 * material field carries. The renderer core never reads the bytes; only the
	 * family's own shaders cast them, so nothing here assumes any material
	 * model.
	 *
	 * This pool serves exactly one shader family, and every object drawn by a
	 * pass belongs to the pass's family. A game with several families owns
	 * several pools drawn by their own passes; when families must mix inside
	 * one visibility stream, the object's material field becomes a packed
	 * {family, slot} pair and streams bucket on the family bits. Nothing in
	 * this class changes for that.
	 *
	 * Mutations write CPU shadow records and mark the slot dirty; sync()
	 * uploads each dirty slot once per frame through the staging ring. That
	 * single upload point is what makes edits last-write-wins and same frame
	 * destroy plus reuse safe, since overlapping staged copies within one
	 * frame are unordered by the device contract.
	 *
	 * destroy() writes the error record over the slot, covering the window
	 * until reuse. It is a diagnostic state and never a lifetime system: a
	 * material must outlive every object that references it, and after the
	 * slot is reused a stale reference silently reads the new record.
	 */
	class MaterialPool
	{
	public:
		MaterialPool() noexcept;
		~MaterialPool() noexcept;

		MaterialPool(const MaterialPool&)			 = delete;
		MaterialPool& operator=(const MaterialPool&) = delete;

		/// Creates the table and claims slot 0 for the error record. Runs once.
		void init(gpu::Device& device, const MaterialPoolDef& def) noexcept;

		/// Destroys the table. Call before the device goes down; live materials
		/// at this point are a leak and assert in debug.
		void shutdown(gpu::Device& device) noexcept;

		/// `data` is one record of exactly stride bytes. It reaches the GPU at
		/// the next sync(). Null handle when the pool is full; the failure is
		/// logged.
		[[nodiscard]] MaterialHandle create(Span<const u8> data) noexcept;

		template <class T> [[nodiscard]] MaterialHandle create(const T& record) noexcept
		{
			static_assert(std::is_trivially_copyable_v<T>, "material records are raw bytes");
			return create(Span<const u8>{reinterpret_cast<const u8*>(&record), sizeof(T)});
		}

		/// Rewrites a live record; last write in a frame wins. Safe on null and
		/// stale handles.
		void update(MaterialHandle handle, Span<const u8> data) noexcept;

		template <class T> void update(MaterialHandle handle, const T& record) noexcept
		{
			static_assert(std::is_trivially_copyable_v<T>, "material records are raw bytes");
			update(handle, Span<const u8>{reinterpret_cast<const u8*>(&record), sizeof(T)});
		}

		/// Safe on null and stale handles.
		void destroy(MaterialHandle handle) noexcept;

		/// Uploads every dirty record. Call once per frame, inside
		/// begin/end_frame, before the graph executes.
		void sync(gpu::Device& device) noexcept;

		[[nodiscard]] bool is_valid(MaterialHandle handle) const noexcept { return m_records.contains(handle); }

		/// Bindless slot of the table; record i belongs to handle index i.
		[[nodiscard]] u32 table_index() const noexcept { return bindless_index(m_table); }

		/// Live materials, the error record included.
		[[nodiscard]] u32 material_count() const noexcept { return m_records.size(); }

		[[nodiscard]] u32 stride() const noexcept { return m_stride; }

	private:
		void mark_dirty(u32 slot) noexcept;

		/// Generations and slot reuse only; the record bytes live in the shadow
		/// block and the table.
		Pool<Material, u8> m_records;

		BufferHandle m_table = {};
		u32 m_stride		 = 0;

		MaterialHandle m_error = {};

		// One block: [dirty bits][dirty list][shadow records]. The shadow is
		// the CPU truth sync uploads from; slot 0 doubles as the scrub source.
		std::pmr::memory_resource* m_resource = nullptr;
		void* m_block						  = nullptr;
		size_t m_block_size					  = 0;

		u64* m_dirty_bits = nullptr;
		u32* m_dirty_list = nullptr;
		u32 m_dirty_count = 0;
		u8* m_shadow	  = nullptr;
	};
}
