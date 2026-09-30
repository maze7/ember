#include <ember/core/hash.h>
#include <ember/core/json.h>
#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/memory/pmr/arena.h>
#include <ember/render/embedded_shaders.h>
#include <ember/render/gpu_scene.h>
#include <ember/render/material_registry.h>

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace ember::render
{
	using material::BuiltinTexture;
	using material::Domain;
	using material::is_texture;
	using material::Layout;
	using material::Param;
	using material::ParamKind;

	namespace
	{
		/// Records one table can hold: what a key's low half addresses.
		constexpr u32 MAX_RECORDS = 65536;

		[[nodiscard]] Heap& graphics() noexcept { return memory::heap(MemoryTag::Graphics); }

		/// The sRGB transfer function inverted (IEC 61966-2-1): a colour picker's value in the linear
		/// space shading works in.
		[[nodiscard]] f32 linear_from_srgb(f32 c) noexcept
		{
			return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
		}

		/// "", "2", "3" or "4": what follows a kind's name to spell a vector.
		[[nodiscard]] StringView width(u8 components) noexcept
		{
			constexpr StringView widths[] = {"", "", "2", "3", "4"};
			return components < std::size(widths) ? widths[components] : StringView();
		}

		/// A value fits a parameter of its kind and width. A texture fits any texture parameter,
		/// since a handle does not say whether it names an array or a cube.
		[[nodiscard]] bool fits(const Param& param, ParamKind kind, u8 components) noexcept
		{
			if (is_texture(kind) || is_texture(param.kind))
				return is_texture(kind) && is_texture(param.kind);

			return kind == param.kind && components == param.components;
		}

		void write_number(u8* record, const Param& param, const u32 (&words)[4]) noexcept
		{
			u32 bits[4] = {words[0], words[1], words[2], words[3]};

			// Values hold colours as they were picked, in sRGB, and shading reads them linear. Alpha
			// is coverage and never gamma encoded, and an [Hdr] colour is linear already.
			if (param.color && !param.hdr)
				for (u32 i = 0; i < 3; ++i)
					bits[i] = std::bit_cast<u32>(linear_from_srgb(std::bit_cast<f32>(bits[i])));

			std::memcpy(record + param.offset, bits, u32{param.components} * sizeof(u32));
		}

		void write_texture(u8* record, const Param& param, TextureHandle texture, SamplerHandle sampler) noexcept
		{
			// The authoring API's *Ref: the texture's bindless index, then the sampler's.
			const u32 ref[2] = {bindless_index(texture), bindless_index(sampler)};
			static_assert(sizeof(ref) == material::TEXTURE_REF_BYTES);

			std::memcpy(record + param.offset, ref, sizeof(ref));
		}

		/// A type cooked into this library: its .type file and its SPIR-V.
		struct CookedType
		{
			Span<const u8> (*type_file)() noexcept;
			Span<const u8> (*spirv)() noexcept;
		};

		/// The error type first, which gives it index 0, then the stock types in StockType's order.
		constexpr CookedType COOKED_TYPES[] = {
			{embedded::error_material_type, embedded::error_material_spirv},
			{embedded::unlit_material_type, embedded::unlit_material_spirv},
			{embedded::sprite_material_type, embedded::sprite_material_spirv},
		};

		static_assert(std::size(COOKED_TYPES) == 1 + static_cast<size_t>(StockType::Count));
	}

	MaterialRegistry::MaterialRegistry() noexcept
		: m_types(MemoryTag::Graphics), m_materials(MemoryTag::Graphics), m_keys(&graphics()),
		  m_dirty(MemoryTag::Graphics), m_retired(&graphics())
	{
	}

	MaterialRegistry::~MaterialRegistry() noexcept = default;

	void MaterialRegistry::init(gpu::Device& device, const MaterialRegistryDef& def) noexcept
	{
		EMBER_ASSERT(m_key_table.is_null() && "init runs once");
		EMBER_ASSERT(ember::render::is_valid(def));

		m_types.init(def.max_types);
		m_materials.init(def.max_materials);
		m_dirty.init(def.max_materials);
		m_initial_records = def.initial_records;

		// Every row starts as the error key, so an index no material has claimed draws the error
		// type instead of whatever the buffer held.
		m_keys.assign(def.max_materials, ERROR_MATERIAL_KEY);

		m_key_table = device.create_buffer({
			.name		  = "materials.keys",
			.size		  = u64{def.max_materials} * sizeof(u32),
			.usage		  = gpu::BufferUsage::Storage,
			.initial_data = {reinterpret_cast<const u8*>(m_keys.data()), m_keys.size() * sizeof(u32)},
		});

		if (m_key_table.is_null())
			EMBER_ERROR("material key table creation failed");

		create_builtins(device);

		// The cooked types load the way a ship build loads every type, so that path runs on every
		// boot. This build cooked them, so a failure here is a broken build.
		MaterialTypeHandle cooked[std::size(COOKED_TYPES)] = {};

		for (u32 i = 0; i < std::size(COOKED_TYPES); ++i)
		{
			const Span<const u8> type_file = COOKED_TYPES[i].type_file();

			material::Type type;
			String problem(&graphics());

			if (!material::read_cooked({reinterpret_cast<const char*>(type_file.data()), type_file.size()},
									   COOKED_TYPES[i].spirv(), type, problem))
			{
				EMBER_ASSERT(false && "this build cooked the engine's types");
				EMBER_ERROR("an embedded material type does not load: {}", problem);
				continue;
			}

			cooked[i] = add_type(std::move(type));
		}

		m_error_type = cooked[0];
		m_error		 = create(m_error_type);
		std::copy(std::begin(cooked) + 1, std::end(cooked), std::begin(m_stock));

		// A fresh pool hands out index 0 first: that, and slot 0, is what makes the error key zero.
		EMBER_ASSERT(m_error_type.index == 0 && m_error.index == 0 && key(m_error) == ERROR_MATERIAL_KEY);
	}

	void MaterialRegistry::shutdown(gpu::Device& device) noexcept
	{
		// The error material is the registry's own; everything else was the game's to destroy.
		if (!m_error.is_null())
			(void)m_materials.erase(m_error);

		EMBER_ASSERT(m_materials.empty() && "destroy materials before the registry shuts down");
		m_materials.clear();

		for (BufferHandle table : m_retired)
			device.destroy(table);

		for (TypeEntry& type : m_types)
			if (!type.table.is_null())
				device.destroy(type.table);

		m_retired.clear();
		m_types.clear();

		for (TextureHandle& texture : m_builtins)
			device.destroy(std::exchange(texture, {}));

		for (auto& wraps : m_samplers)
			for (SamplerHandle& sampler : wraps)
				device.destroy(std::exchange(sampler, {}));

		if (!m_key_table.is_null())
			device.destroy(m_key_table);

		m_key_table	 = {};
		m_error_type = {};
		m_error		 = {};

		std::fill(std::begin(m_stock), std::end(m_stock), MaterialTypeHandle{});
	}

	MaterialTypeHandle MaterialRegistry::add_type(material::Type type) noexcept
	{
		TypeEntry entry;
		entry.type	   = std::move(type);
		entry.capacity = m_initial_records;

		if (!build(entry))
			return {};

		entry.shadow.resize(u64{entry.capacity} * entry.type.record.size);

		const MaterialTypeHandle handle = m_types.insert(std::move(entry));

		if (handle.is_null()) [[unlikely]]
			EMBER_ERROR("the material registry is full ({} types)", m_types.capacity());

		return handle;
	}

	bool MaterialRegistry::replace_type(MaterialTypeHandle handle, material::Type type) noexcept
	{
		TypeEntry* entry = m_types.get(handle);

		if (entry == nullptr)
			return false;

		// A hash covers everything a draw can see; equal means an edited comment.
		if (type.hash == entry->type.hash)
			return true;

		// Built aside, so a layout that cannot be keyed leaves the live type drawing.
		TypeEntry next;
		next.type = std::move(type);

		if (!build(next))
			return false;

		const bool resized = next.type.record.size != entry->type.record.size;

		entry->type				 = std::move(next.type);
		entry->names			 = std::move(next.names);
		entry->defaults			 = std::move(next.defaults);
		entry->instance_defaults = std::move(next.instance_defaults);

		if (resized)
		{
			entry->shadow.assign(u64{entry->capacity} * entry->type.record.size, 0);
			entry->resize = true;
		}

		entry->upload = true;
		++entry->generation;

		// Every material of the type encodes again from its values, and its row is rewritten, since
		// the new build may be of another domain. A value that no longer fits its parameter waits,
		// the default standing in, for a build where it fits again; nothing on screen says why the
		// material changed, so each such parameter is named once.
		const Span<const Param> params = {entry->type.record.params.data(), entry->type.record.params.size()};
		Vector<bool> named(params.size(), false, &graphics());

		for (auto it = m_materials.begin(); it != m_materials.end(); ++it)
		{
			if (it->type != handle)
				continue;

			const Vector<Value>& values = *m_materials.get_cold(it.handle());

			for (const Value& value : values)
			{
				const size_t i = std::find(entry->names.begin(), entry->names.end(), value.name) - entry->names.begin();

				if (i == params.size() || named[i] || fits(params[i], value.kind, value.components))
					continue;

				named[i] = true;
				EMBER_WARN(
					"{}.{} now holds {}{}; materials that set it as {}{} take its default until it holds that again",
					entry->type.name, params[i].name, enum_name(params[i].kind), width(params[i].components),
					enum_name(value.kind), width(value.components));
			}

			encode(*entry, it->slot, {values.data(), values.size()});
			rekey(it.handle());
		}

		return true;
	}

	void MaterialRegistry::remove_type(MaterialTypeHandle handle) noexcept
	{
		EMBER_ASSERT(handle != m_error_type && "the error type lives as long as the registry");

		TypeEntry* entry = m_types.get(handle);

		if (entry == nullptr || handle == m_error_type)
			return;

		if (!entry->table.is_null())
			m_retired.push_back(entry->table);

		(void)m_types.erase(handle);

		// With the type gone, key_of() answers the error key for every material still naming it.
		for (auto it = m_materials.begin(); it != m_materials.end(); ++it)
			if (it->type == handle)
				rekey(it.handle());
	}

	MaterialHandle MaterialRegistry::create(MaterialTypeHandle type_handle) noexcept
	{
		TypeEntry* type = m_types.get(type_handle);

		EMBER_ASSERT(type != nullptr && "create with a live type");
		if (type == nullptr) [[unlikely]]
			return {};

		u16 slot = 0;

		if (!allocate_slot(*type, slot)) [[unlikely]]
		{
			EMBER_ERROR("{} has {} materials, as many as one type can hold", type->type.name, MAX_RECORDS);
			return {};
		}

		const MaterialHandle handle = m_materials.insert(MaterialEntry{type_handle, slot}, Vector<Value>(&graphics()));

		if (handle.is_null()) [[unlikely]]
		{
			type->free_slots.push_back(slot);
			EMBER_ERROR("the material registry is full ({} materials)", m_materials.capacity());
			return {};
		}

		encode(*type, slot, {});
		rekey(handle);
		return handle;
	}

	void MaterialRegistry::destroy(MaterialHandle handle) noexcept
	{
		EMBER_ASSERT(handle != m_error && "the error material lives as long as the registry");

		MaterialEntry* material = m_materials.get(handle);

		if (material == nullptr || handle == m_error)
			return;

		if (TypeEntry* type = m_types.get(material->type))
			type->free_slots.push_back(material->slot);

		// Scrubbed while the handle is live, so key_of() answers the error key and the next sync
		// uploads that row with no record behind it. The slot can be handed out again at once:
		// the row upload is ordered before any read of the new material.
		*material = {};
		rekey(handle);

		(void)m_materials.erase(handle);
	}

	bool MaterialRegistry::set(MaterialHandle material, StringView param, TextureHandle texture) noexcept
	{
		Value value;
		value.kind	  = ParamKind::Texture2D;
		value.texture = texture;
		return assign(material, param, value);
	}

	bool MaterialRegistry::set(MaterialHandle material, StringView param, TextureHandle texture, gpu::Filter filter,
							   gpu::AddressMode wrap) noexcept
	{
		Value value;
		value.kind		  = ParamKind::Texture2D;
		value.texture	  = texture;
		value.own_sampler = true;
		value.filter	  = filter;
		value.wrap		  = wrap;
		return assign(material, param, value);
	}

	bool MaterialRegistry::set(MaterialHandle material, StringView param, BuiltinTexture texture) noexcept
	{
		Value value;
		value.kind	  = ParamKind::Texture2D;
		value.builtin = texture;
		return assign(material, param, value);
	}

	bool MaterialRegistry::set_number(MaterialHandle material, StringView param,
									  const detail::NumberValue& number) noexcept
	{
		Value value;
		value.kind		 = number.kind;
		value.components = number.components;
		std::memcpy(value.words, number.words, sizeof(value.words));
		return assign(material, param, value);
	}

	bool MaterialRegistry::assign(MaterialHandle handle, StringView name, Value value) noexcept
	{
		MaterialEntry* material = m_materials.get(handle);
		TypeEntry* type			= material != nullptr ? m_types.get(material->type) : nullptr;

		if (type == nullptr) [[unlikely]]
			return false;

		const Param* param = material::find_param(type->type.record, name);

		if (param == nullptr) [[unlikely]]
		{
			EMBER_ERROR("{} has no parameter '{}'", type->type.name, name);
			return false;
		}

		if (!fits(*param, value.kind, value.components)) [[unlikely]]
		{
			EMBER_ERROR("{}.{} holds {}{}, not {}{}", type->type.name, name, enum_name(param->kind),
						width(param->components), enum_name(value.kind), width(value.components));
			return false;
		}

		value.name		 = hash_text(name);
		value.kind		 = param->kind;
		value.components = param->components;

		Vector<Value>& values = *m_materials.get_cold(handle);

		const auto existing =
			std::find_if(values.begin(), values.end(), [&](const Value& v) { return v.name == value.name; });

		if (existing != values.end())
			*existing = value;
		else
			values.push_back(value);

		// One parameter changed, so only its bytes are written; the rest of the record is current.
		write(type->shadow.data() + u64{material->slot} * type->type.record.size, *param, value);
		m_dirty.mark(handle.index);
		return true;
	}

	void MaterialRegistry::sync(gpu::Device& device, Arena& scratch) noexcept
	{
		for (BufferHandle table : m_retired)
			device.destroy(table);

		m_retired.clear();

		const Span<const u32> dirty = m_dirty.slots();
		const u32 count				= static_cast<u32>(dirty.size());

		// Sorted copies: material indices for the key rows, and the keys of the records whose type
		// does not go up whole this frame. A key sorts by type, then slot, so each run of
		// consecutive slots within one type is one copy.
		auto* rows		 = static_cast<u32*>(scratch.allocate_fast(count * sizeof(u32), alignof(u32)));
		auto* records	 = static_cast<u32*>(scratch.allocate_fast(count * sizeof(u32), alignof(u32)));
		u32 record_count = 0;

		std::memcpy(rows, dirty.data(), count * sizeof(u32));
		std::sort(rows, rows + count);

		for (u32 i = 0; i < count; ++i)
		{
			const MaterialEntry& material = m_materials.hot_data()[rows[i]];
			const TypeEntry* type		  = m_types.get(material.type);

			if (type != nullptr && !type->upload)
				records[record_count++] = material_key(material.type.index, material.slot);
		}

		std::sort(records, records + record_count);

		for_each_key_run({records, record_count},
						 [&](u16 bucket, u16 first, u32 run) noexcept
						 {
							 // The type was live when its key was collected, so its slot holds it.
							 const TypeEntry& type = m_types.hot_data()[bucket];
							 const u64 stride	   = type.type.record.size;

							 if (!type.table.is_null() && stride != 0)
								 device.update_buffer(type.table, first * stride,
													  {type.shadow.data() + first * stride, run * stride});
						 });

		// New, grown and replaced types go up whole. The old table of a grown type is destroyed
		// with the frames still reading it; the draws recorded after this sync read the new one.
		for (TypeEntry& type : m_types)
		{
			const u64 stride = type.type.record.size;

			if (type.resize)
			{
				if (!type.table.is_null())
					device.destroy(type.table);

				char name[96] = {};
				fmt::format_to_n(name, sizeof(name) - 1, "materials.{}", type.type.name);

				type.table = device.create_buffer({
					.name  = name,
					.size  = std::max<u64>(u64{type.capacity} * stride, 16),
					.usage = gpu::BufferUsage::Storage,
				});

				if (type.table.is_null()) [[unlikely]]
					EMBER_ERROR("record table for {} ({} records) creation failed", type.type.name, type.capacity);

				type.resize = false;
			}

			if (type.upload && !type.table.is_null() && type.high_water != 0 && stride != 0)
				device.update_buffer(type.table, 0, {type.shadow.data(), type.high_water * stride});

			type.upload = false;
		}

		for_each_slot_run({rows, count},
						  [&](u32 first, u32 run) noexcept
						  {
							  device.update_buffer(
								  m_key_table, u64{first} * sizeof(u32),
								  {reinterpret_cast<const u8*>(m_keys.data() + first), u64{run} * sizeof(u32)});
						  });

		m_dirty.clear();
	}

	const material::Type* MaterialRegistry::type(MaterialTypeHandle handle) const noexcept
	{
		const TypeEntry* entry = m_types.get(handle);
		return entry != nullptr ? &entry->type : nullptr;
	}

	MaterialTypeHandle MaterialRegistry::type_of(MaterialHandle handle) const noexcept
	{
		const MaterialEntry* material = m_materials.get(handle);
		return material != nullptr && m_types.contains(material->type) ? material->type : MaterialTypeHandle{};
	}

	u32 MaterialRegistry::generation(MaterialTypeHandle handle) const noexcept
	{
		const TypeEntry* entry = m_types.get(handle);
		return entry != nullptr ? entry->generation : 0;
	}

	Span<const u8> MaterialRegistry::record(MaterialHandle handle) const noexcept
	{
		const MaterialEntry* material = m_materials.get(handle);
		const TypeEntry* type		  = material != nullptr ? m_types.get(material->type) : nullptr;

		if (type == nullptr)
			return {};

		const u64 stride = type->type.record.size;
		return {type->shadow.data() + material->slot * stride, stride};
	}

	Span<const u8> MaterialRegistry::instance_defaults(MaterialTypeHandle handle) const noexcept
	{
		const TypeEntry* entry = m_types.get(handle);
		return entry != nullptr ? Span<const u8>{entry->instance_defaults.data(), entry->instance_defaults.size()}
								: Span<const u8>{};
	}

	u32 MaterialRegistry::layout_buckets(Span<const u32> users, Span<BucketRange> out) const noexcept
	{
		EMBER_ASSERT(out.size() == bucket_count());

		for (BucketRange& range : out)
			range = {};

		// Every object counts toward the bucket its material's row names, dead rows included: they
		// name the error bucket, which is where those objects draw.
		const size_t rows = std::min(users.size(), m_keys.size());

		for (size_t i = 0; i < rows; ++i)
			if (users[i] != 0)
				out[key_bucket(m_keys[i])].capacity += users[i];

		u32 first = 0;

		for (BucketRange& range : out)
		{
			range.first = first;
			first += range.capacity;
		}

		return first;
	}

	u32 MaterialRegistry::key(MaterialHandle handle) const noexcept
	{
		return handle.index < m_keys.size() ? m_keys[handle.index] : ERROR_MATERIAL_KEY;
	}

	TextureHandle MaterialRegistry::builtin(BuiltinTexture texture) const noexcept
	{
		return m_builtins[static_cast<size_t>(texture)];
	}

	SamplerHandle MaterialRegistry::sampler(gpu::Filter filter, gpu::AddressMode wrap) const noexcept
	{
		return m_samplers[static_cast<size_t>(filter)][static_cast<size_t>(wrap)];
	}

	u32 MaterialRegistry::table_index(MaterialTypeHandle handle) const noexcept
	{
		const TypeEntry* entry = m_types.get(handle);
		return entry != nullptr ? bindless_index(entry->table) : 0;
	}

	bool MaterialRegistry::build(TypeEntry& entry) const noexcept
	{
		const Layout& record = entry.type.record;

		entry.names.clear();

		// Values find their parameter by the hash of its name, so two names that hash alike would
		// share values. It never happens with 64 bits and a type's few names, but it is checked
		// here, once per build, rather than trusted forever.
		for (const Param& param : record.params)
		{
			const u64 name = hash_text(param.name);

			if (std::find(entry.names.begin(), entry.names.end(), name) != entry.names.end()) [[unlikely]]
			{
				EMBER_ERROR("{}.{}: its name hashes like another parameter's; rename one", entry.type.name, param.name);
				return false;
			}

			entry.names.push_back(name);
		}

		encode_defaults(record, entry.defaults);
		encode_defaults(entry.type.instance, entry.instance_defaults);
		return true;
	}

	void MaterialRegistry::encode_defaults(const Layout& layout, Vector<u8>& out) const noexcept
	{
		out.assign(layout.size, 0);

		for (const Param& param : layout.params)
		{
			// The compiler parsed every [Default] with these same readers, so none fails here; an
			// absent one leaves numbers zero and textures white.
			if (is_texture(param.kind))
			{
				BuiltinTexture preset = BuiltinTexture::White;
				(void)parse_enum(StringView(param.preset), preset);

				write_texture(out.data(), param, builtin(preset), sampler(param.filter, param.wrap));
				continue;
			}

			u32 words[4] = {};
			if (!param.preset.empty())
				(void)material::parse_value(param, param.preset, words);

			write_number(out.data(), param, words);
		}
	}

	void MaterialRegistry::encode(TypeEntry& type, u16 slot, Span<const Value> values) const noexcept
	{
		const Layout& layout = type.type.record;
		u8* record			 = type.shadow.data() + u64{slot} * layout.size;

		std::memcpy(record, type.defaults.data(), layout.size);

		for (u32 i = 0; i < layout.params.size(); ++i)
		{
			const Param& param = layout.params[i];

			for (const Value& value : values)
			{
				if (value.name == type.names[i] && fits(param, value.kind, value.components))
				{
					write(record, param, value);
					break;
				}
			}
		}
	}

	void MaterialRegistry::write(u8* record, const Param& param, const Value& value) const noexcept
	{
		if (!is_texture(param.kind))
		{
			write_number(record, param, value.words);
			return;
		}

		const TextureHandle texture = value.texture.is_null() ? builtin(value.builtin) : value.texture;
		const SamplerHandle sampler =
			value.own_sampler ? this->sampler(value.filter, value.wrap) : this->sampler(param.filter, param.wrap);

		write_texture(record, param, texture, sampler);
	}

	bool MaterialRegistry::allocate_slot(TypeEntry& type, u16& slot) noexcept
	{
		if (!type.free_slots.empty())
		{
			slot = type.free_slots.back();
			type.free_slots.pop_back();
			return true;
		}

		if (type.high_water == type.capacity)
		{
			if (type.capacity == MAX_RECORDS)
				return false;

			// Doubling keeps growth amortised to a copy per record, and the table's new bindless
			// index reaches shaders through the per-draw constants, so no key moves.
			type.capacity = std::min(type.capacity * 2, MAX_RECORDS);
			type.shadow.resize(u64{type.capacity} * type.type.record.size);
			type.resize = true;
			type.upload = true;
		}

		slot = static_cast<u16>(type.high_water++);
		return true;
	}

	u32 MaterialRegistry::key_of(const MaterialEntry& material) const noexcept
	{
		const TypeEntry* type = m_types.get(material.type);

		// Only surface types draw on geometry. A screen material on an object is a mistake, and
		// mistakes draw as the error type.
		if (type == nullptr || type->type.domain != Domain::Surface)
			return ERROR_MATERIAL_KEY;

		return material_key(material.type.index, material.slot);
	}

	void MaterialRegistry::rekey(MaterialHandle handle) noexcept
	{
		m_keys[handle.index] = key_of(m_materials.hot_data()[handle.index]);
		m_dirty.mark(handle.index);
	}

	void MaterialRegistry::create_builtins(gpu::Device& device) noexcept
	{
		// One texel each, but the error texture: a 2x2 checker reads as a mistake at any size.
		// Unorm throughout: 0 and 255 are the same in sRGB, and the flat normal is data.
		const u8 white[] = {255, 255, 255, 255};
		const u8 black[] = {0, 0, 0, 255};
		const u8 flat[]	 = {128, 128, 255, 255}; // tangent-space (0, 0, 1)
		const u8 error[] = {255, 0, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 0, 255, 255};

		const struct
		{
			const char* name;
			Span<const u8> texels;
			u32 size;
		} builtins[] = {
			{"materials.white", white, 1},
			{"materials.black", black, 1},
			{"materials.flat", flat, 1},
			{"materials.error", error, 2},
		};

		static_assert(std::size(builtins) == static_cast<size_t>(BuiltinTexture::Count));

		for (u32 i = 0; i < std::size(builtins); ++i)
		{
			m_builtins[i] = device.create_texture({
				.name		  = builtins[i].name,
				.extent		  = {builtins[i].size, builtins[i].size, 1},
				.format		  = gpu::TextureFormat::RGBA8Unorm,
				.usage		  = gpu::TextureUsage::Sampled,
				.initial_data = builtins[i].texels,
			});
		}

		for (u32 f = 0; f < FILTERS; ++f)
		{
			for (u32 w = 0; w < WRAPS; ++w)
			{
				const auto filter = static_cast<gpu::Filter>(f);
				const auto wrap	  = static_cast<gpu::AddressMode>(w);
				char name[48]	  = {};
				fmt::format_to_n(name, sizeof(name) - 1, "materials.{}.{}", enum_name(filter), enum_name(wrap));

				m_samplers[f][w] = device.create_sampler({
					.name		= name,
					.min_filter = filter,
					.mag_filter = filter,
					.mip_filter = filter,
					.address_u	= wrap,
					.address_v	= wrap,
					.address_w	= wrap,

					// Anisotropy is what keeps a filtered texture sharp at grazing angles; point
					// sampling is pixel art, which must never blend.
					.max_anisotropy = static_cast<u8>(filter == gpu::Filter::Linear ? 8 : 0),

					// Outside a clamped-to-border sprite or decal there should be nothing.
					.border = gpu::BorderColor::TransparentBlack,
				});
			}
		}
	}
}
