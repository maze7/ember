#pragma once

#include <ember/containers/dirty_set.h>
#include <ember/containers/pool.h>
#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/gpu/sampler.h>
#include <ember/material/type.h>
#include <ember/render/common.h>

#include <bit>
#include <glm/fwd.hpp>

// Forward declarations
namespace ember
{
	class Arena;

	namespace gpu
	{
		class Device;
	}
}

namespace ember::render
{
	struct MaterialRegistryDef
	{
		/// Types held at once, the error type included. A type's index is its draw bucket, so this
		/// also bounds the buckets a cull sorts into.
		u32 max_types = 256;

		/// Materials held at once, the error material included: the key table's rows. Every
		/// object's material field indexes that table, so no handle index reaches past it.
		u32 max_materials = 4096;

		/// Records a type's table holds at its first sync. The table doubles whenever it fills, up
		/// to the 65536 a key's slot half can address.
		u32 initial_records = 16;
	};

	[[nodiscard]] constexpr bool is_valid(const MaterialRegistryDef& def) noexcept
	{
		return def.max_types > 1 && def.max_types <= 65536 && def.max_materials > 1 && def.max_materials <= 65536 &&
			   def.initial_records != 0 && def.initial_records <= 65536;
	}

	/**
	 * A material key: the draw bucket in the high half, which is the type's index, and the record's
	 * slot in the type's table in the low half. The key table holds one per material index, and
	 * shaders split it the same way.
	 */
	[[nodiscard]] constexpr u32 material_key(u16 bucket, u16 slot) noexcept { return u32{bucket} << 16 | slot; }
	[[nodiscard]] constexpr u16 key_bucket(u32 key) noexcept { return static_cast<u16>(key >> 16); }
	[[nodiscard]] constexpr u16 key_slot(u32 key) noexcept { return static_cast<u16>(key & 0xFFFFu); }

	/// The error type is the registry's first type and the error material the first record of its
	/// table, so the error key is zero, and a zeroed row is the error.
	inline constexpr u32 ERROR_MATERIAL_KEY = 0;

	/// The material types that ship with the engine, cooked into Ember::Render and registered at
	/// init, so a game draws without writing a type and ship builds need no files for them.
	enum class StockType : u8
	{
		Unlit,	// shaders/materials/unlit.slang: texture, tint and vertex colour, no lighting
		Sprite, // shaders/materials/sprite.slang: alpha-tested cards with a per-object atlas frame
		Count,
	};

	/**
	 * Where one bucket's draws go in a view's argument buffer: its first entry, and how many it can
	 * hold, which is how many objects use a material of its type. Mirrored in shaders/cull.slang.
	 */
	struct BucketRange
	{
		u32 first	 = 0;
		u32 capacity = 0;
	};

	static_assert(sizeof(BucketRange) == 8);

	/**
	 * Walks maximal runs of consecutive slots in one bucket through an ascending list of material
	 * keys, calling fn(bucket, first_slot, count) per run: each is one copy into one type's table.
	 * Consecutive keys can straddle two buckets at a slot boundary; a run never does.
	 */
	template <class Fn> void for_each_key_run(Span<const u32> sorted_keys, Fn&& fn) noexcept
	{
		const u32 count = static_cast<u32>(sorted_keys.size());

		for (u32 i = 0; i < count;)
		{
			const u32 first = sorted_keys[i];
			u32 run			= 1;

			while (i + run < count && sorted_keys[i + run] == first + run &&
				   key_bucket(first + run) == key_bucket(first))
				++run;

			fn(key_bucket(first), key_slot(first), run);
			i += run;
		}
	}

	namespace detail
	{
		/// A number parameter's value as C++ spells it: its kind, its width and its bit patterns.
		struct NumberValue
		{
			material::ParamKind kind = material::ParamKind::Float;
			u8 components			 = 1;
			u32 words[4]			 = {};
		};

		[[nodiscard]] constexpr NumberValue number_value(f32 value) noexcept
		{
			return {material::ParamKind::Float, 1, {std::bit_cast<u32>(value)}};
		}

		[[nodiscard]] constexpr NumberValue number_value(i32 value) noexcept
		{
			return {material::ParamKind::Int, 1, {std::bit_cast<u32>(value)}};
		}

		[[nodiscard]] constexpr NumberValue number_value(u32 value) noexcept
		{
			return {material::ParamKind::Uint, 1, {value}};
		}

		[[nodiscard]] constexpr NumberValue number_value(bool value) noexcept
		{
			return {material::ParamKind::Bool, 1, {value ? 1u : 0u}};
		}

		template <glm::length_t N, class T, glm::qualifier Q>
		[[nodiscard]] constexpr NumberValue number_value(const glm::vec<N, T, Q>& value) noexcept
		{
			static_assert(N >= 2 && N <= 4, "parameters are scalars or vectors of two to four");

			NumberValue out = number_value(value[0]);
			out.components	= static_cast<u8>(N);

			for (glm::length_t i = 1; i < N; ++i)
				out.words[i] = number_value(value[i]).words[0];

			return out;
		}
	}

	/**
	 * Every material type and every material the renderer draws, and the GPU tables shaders read
	 * them through.
	 *
	 * A type is a compiled material::Type: bytecode, record layout and state. The registry gives it a
	 * record table, one GPU record per material of the type, which doubles when it fills. A type's
	 * handle index is its draw bucket: one bucket is one type, and so one pipeline per pass.
	 *
	 * A material is a type plus values. Values are kept as they were set, by parameter name, and the
	 * record is derived from them: it starts as the type's [Default] record and each value is
	 * written through the layout. Replacing a type, a hot reload that moved, added or retyped
	 * fields, therefore re-encodes every material of it in place, and a value the new layout lacks
	 * waits for a layout that has it again.
	 *
	 * The key table has one u32 per material index: the bucket in the high half, the record's slot
	 * in its type's table in the low half. Objects store the material's index and shaders read the
	 * key through it, so a material that changes type or moves slot rewrites one row and no object
	 * ever learns of it.
	 *
	 * Row 0 is the error material, the engine's error type at slot 0, which makes the error key
	 * zero. Any row reset to zero draws the error type: a null handle, a destroyed material, a
	 * material whose type was removed, and a screen material put on an object all draw the magenta
	 * checker instead of whatever a stale index would find.
	 *
	 * Everything but sync() is CPU work on shadow copies. sync() uploads what changed, creates and
	 * grows tables and retires the tables of removed types, once per frame. Uploads ride the staging
	 * ring in the frame's command stream, which orders them after every earlier frame's reads, so
	 * slots and rows are reused at once.
	 *
	 * Threading: set() runs on any frame thread at once, provided no two calls name the same
	 * material. Everything else that changes state belongs to the owner phase, and sync() to the
	 * render phase, after the writers are joined.
	 */
	class MaterialRegistry
	{
	public:
		MaterialRegistry() noexcept;
		~MaterialRegistry() noexcept;

		MaterialRegistry(const MaterialRegistry&)			 = delete;
		MaterialRegistry& operator=(const MaterialRegistry&) = delete;

		/// Creates the key table and the builtin textures and samplers, and registers the types cooked
		/// into Ember::Render: the error type and material first, then the stock types. Runs once.
		void init(gpu::Device& device, const MaterialRegistryDef& def) noexcept;

		/// Destroys every GPU object the registry made and drops the remaining types. Destroy
		/// materials first; live ones here are a leak and assert in debug.
		void shutdown(gpu::Device& device) noexcept;

		/// Takes a compiled or cooked type. Null when the registry is full or the layout cannot be
		/// keyed by name; the reason is logged.
		[[nodiscard]] MaterialTypeHandle add_type(material::Type type) noexcept;

		/**
		 * A new build of a live type, as hot reload produces one: every material of it re-encodes
		 * from its values, and generation() moves so passes rebuild their pipelines. A build whose
		 * hash matches the live one changed nothing a draw can see and is ignored. False for a stale
		 * handle or a layout that cannot be keyed.
		 */
		bool replace_type(MaterialTypeHandle handle, material::Type type) noexcept;

		/// Materials still of the type draw as the error type until destroyed, and refuse set().
		/// The error type cannot be removed.
		void remove_type(MaterialTypeHandle handle) noexcept;

		/// A new material with the type's defaults. Null when the registry or the type's table is
		/// full; logged.
		[[nodiscard]] MaterialHandle create(MaterialTypeHandle type) noexcept;

		/// Safe on null and stale handles. The error material cannot be destroyed.
		void destroy(MaterialHandle material) noexcept;

		/**
		 * Sets a number parameter from f32, i32, u32 or bool, or a glm vector of one of them, which
		 * must match the parameter's kind and width exactly. Colours are set as an artist picks
		 * them, sRGB unless the parameter is [Hdr]; the record holds them linear. False for a dead
		 * material or one whose type was removed; false and logged for an unknown name or a
		 * mismatched kind.
		 */
		template <class T> bool set(MaterialHandle material, StringView param, const T& value) noexcept
		{
			return set_number(material, param, detail::number_value(value));
		}

		/// Sets a texture parameter, sampled as its [Filter] and [Wrap] say. A null texture reads
		/// white. The texture must outlive the material's use of it.
		bool set(MaterialHandle material, StringView param, TextureHandle texture) noexcept;

		/// Sets a texture parameter with the material's own filter and wrap.
		bool set(MaterialHandle material, StringView param, TextureHandle texture, gpu::Filter filter,
				 gpu::AddressMode wrap) noexcept;

		bool set(MaterialHandle material, StringView param, material::BuiltinTexture texture) noexcept;

		/// Uploads what changed since the last sync. Once per frame, before the graph executes;
		/// scratch holds the sorted slot lists only until the copies are recorded.
		void sync(gpu::Device& device, Arena& scratch) noexcept;

		/// The type as the registry holds it: bytecode, layout and state. Null for a stale handle.
		[[nodiscard]] const material::Type* type(MaterialTypeHandle handle) const noexcept;

		/// Null for a stale handle, and for a material whose type was removed.
		[[nodiscard]] MaterialTypeHandle type_of(MaterialHandle material) const noexcept;

		/// Moves on every replace_type() that changed something: a pass holding pipelines built at
		/// another generation builds them again.
		[[nodiscard]] u32 generation(MaterialTypeHandle handle) const noexcept;

		/// The material's record as the next sync uploads it: for inspectors and tests. Empty for a
		/// dead material; valid until the next create() or replace_type() moves the table.
		[[nodiscard]] Span<const u8> record(MaterialHandle material) const noexcept;

		/// Per-object data a new object of the type starts with: its Instance layout's [Default]s.
		[[nodiscard]] Span<const u8> instance_defaults(MaterialTypeHandle handle) const noexcept;

		/// The same, for an object of this material. Empty for a dead material, whose objects draw the
		/// error type, which has no per-object data.
		[[nodiscard]] Span<const u8> instance_defaults(MaterialHandle material) const noexcept
		{
			return instance_defaults(type_of(material));
		}

		/**
		 * Lays this frame's buckets out end to end in one argument buffer: each bucket's capacity is
		 * the number of objects using a material of its type, `users` counting objects per material
		 * index. Writes a range for every bucket into `out`, bucket_count() of them, and returns the
		 * draws they hold together. An object whose material is gone counts where its row now points,
		 * toward the error type.
		 */
		u32 layout_buckets(Span<const u32> users, Span<BucketRange> out) const noexcept;

		/// Calls fn(handle, type, generation) for every live type, in bucket order.
		template <class Fn> void for_each_type(Fn&& fn) const noexcept
		{
			for (auto it = m_types.begin(); it != m_types.end(); ++it)
				fn(it.handle(), it->type, it->generation);
		}

		/// The key table's row for the handle's index, as the next sync uploads it.
		[[nodiscard]] u32 key(MaterialHandle material) const noexcept;

		[[nodiscard]] TextureHandle builtin(material::BuiltinTexture texture) const noexcept;
		[[nodiscard]] SamplerHandle sampler(gpu::Filter filter, gpu::AddressMode wrap) const noexcept;

		/// Bindless slots for shaders. A type's table moves when it grows; read it every frame.
		[[nodiscard]] u32 key_table_index() const noexcept { return bindless_index(m_key_table); }
		[[nodiscard]] u32 table_index(MaterialTypeHandle handle) const noexcept;

		[[nodiscard]] MaterialTypeHandle error_type() const noexcept { return m_error_type; }
		[[nodiscard]] MaterialHandle error_material() const noexcept { return m_error; }

		[[nodiscard]] MaterialTypeHandle stock_type(StockType type) const noexcept
		{
			return m_stock[static_cast<size_t>(type)];
		}

		/// One per type slot, live or not: a key's bucket always has a range.
		[[nodiscard]] u32 bucket_count() const noexcept { return m_types.capacity(); }

		/// Material indices a handle can carry: the key table's rows.
		[[nodiscard]] u32 material_capacity() const noexcept { return m_materials.capacity(); }

		[[nodiscard]] u32 type_count() const noexcept { return m_types.size(); }
		[[nodiscard]] u32 material_count() const noexcept { return m_materials.size(); }

	private:
		/// One value a material was given, keyed by its parameter's name so it outlives any layout.
		struct Value
		{
			u64 name				 = 0; // hash_text of the parameter's name
			material::ParamKind kind = material::ParamKind::Float;
			u8 components			 = 1;

			// Textures: the texture, or the builtin while it is null, and the sampler, which follows
			// the parameter's [Filter] and [Wrap] unless the material chose its own.
			material::BuiltinTexture builtin = material::BuiltinTexture::White;
			bool own_sampler				 = false;
			gpu::Filter filter				 = gpu::Filter::Linear;
			gpu::AddressMode wrap			 = gpu::AddressMode::Repeat;
			TextureHandle texture			 = {};

			u32 words[4] = {}; // numbers as they were set: colours in sRGB
		};

		struct TypeEntry
		{
			material::Type type;
			Vector<u64> names;			  // hash_text of each record parameter's name, in layout order
			Vector<u8> defaults;		  // the record every material of the type starts from
			Vector<u8> instance_defaults; // per-object data before the game writes any
			Vector<u8> shadow;			  // capacity records: the CPU truth the table mirrors
			Vector<u16> free_slots;

			BufferHandle table = {};
			u32 capacity	   = 0;
			u32 high_water	   = 0; // slots handed out at least once: the part of the table in use
			u32 generation	   = 1;
			bool resize		   = true; // sync creates the table at capacity records
			bool upload		   = true; // sync uploads every record in use
		};

		/// Trivially destructible on purpose: sync reads a destroyed material's scrubbed entry, and
		/// a null type there is what says it has no record to upload.
		struct MaterialEntry
		{
			MaterialTypeHandle type = {};
			u16 slot				= 0;
		};

		bool set_number(MaterialHandle material, StringView param, const detail::NumberValue& value) noexcept;
		bool assign(MaterialHandle material, StringView param, Value value) noexcept;

		[[nodiscard]] bool build(TypeEntry& entry) const noexcept;
		void encode_defaults(const material::Layout& layout, Vector<u8>& out) const noexcept;
		void encode(TypeEntry& type, u16 slot, Span<const Value> values) const noexcept;
		void write(u8* record, const material::Param& param, const Value& value) const noexcept;

		[[nodiscard]] static bool allocate_slot(TypeEntry& type, u16& slot) noexcept;
		[[nodiscard]] u32 key_of(const MaterialEntry& material) const noexcept;
		void rekey(MaterialHandle material) noexcept;

		void create_builtins(gpu::Device& device) noexcept;

		Pool<MaterialType, TypeEntry> m_types;
		Pool<Material, MaterialEntry, Vector<Value>> m_materials;

		Vector<u32> m_keys; // the key table's CPU truth, one row per material index
		BufferHandle m_key_table = {};
		DirtySet m_dirty; // material indices whose record or row changed

		Vector<BufferHandle> m_retired; // tables of removed types, destroyed at the next sync

		// Every sampler a parameter can ask for, made once: that fixed table is the deduplication.
		static constexpr size_t FILTERS = enum_names<gpu::Filter>().size();
		static constexpr size_t WRAPS	= enum_names<gpu::AddressMode>().size();

		TextureHandle m_builtins[static_cast<size_t>(material::BuiltinTexture::Count)] = {};
		SamplerHandle m_samplers[FILTERS][WRAPS]									   = {};

		MaterialTypeHandle m_error_type									  = {};
		MaterialHandle m_error											  = {};
		MaterialTypeHandle m_stock[static_cast<size_t>(StockType::Count)] = {};
		u32 m_initial_records											  = 16;
	};
}
