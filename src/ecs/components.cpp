#include <ember/core/logger.h>
#include <ember/ecs/components.h>

#include <algorithm>
#include <cmath>

/**
 * Component types declared at run time: their bytes live in EnTT storages of plain blobs, one storage
 * per type, named after the type, so a script's component costs what a C++ one does. Every operation
 * on a ComponentInfo takes the info first so these can find their storage and their fields.
 */
namespace ember::ecs
{
	namespace
	{
		/** A dynamic component's bytes: the storage holds blobs of the smallest size that fits. */
		template <u32 N> struct Blob
		{
			alignas(8) u8 bytes[N];
		};

		/** A dynamic component with no fields: present or not, as a C++ tag is. */
		struct DynamicTag
		{
		};

		constexpr u32 BLOB_SIZES[] = {8, 16, 32, 64, 128, 256};

		[[nodiscard]] u32 blob_for(u32 size) noexcept
		{
			for (const u32 blob : BLOB_SIZES)
				if (size <= blob)
					return blob;
			return 0;
		}

		template <u32 N> struct BlobOps
		{
			static void assure(const ComponentInfo& self, entt::registry& registry) noexcept
			{
				(void)registry.storage<Blob<N>>(self.type);
			}

			static void emplace(const ComponentInfo& self, entt::registry& registry, Entity entity,
								const void* value) noexcept
			{
				auto& storage = registry.storage<Blob<N>>(self.type);
				Blob<N>& blob = storage.contains(entity) ? storage.get(entity) : storage.emplace(entity);
				std::memcpy(blob.bytes, value, self.size);
			}

			static void remove(const ComponentInfo& self, entt::registry& registry, Entity entity) noexcept
			{
				registry.storage<Blob<N>>(self.type).remove(entity);
			}

			static const void* find(const ComponentInfo& self, const entt::registry& registry, Entity entity) noexcept
			{
				const auto* storage = registry.storage<Blob<N>>(self.type);
				return storage != nullptr && storage->contains(entity) ? storage->get(entity).bytes : nullptr;
			}

			static void* get(const ComponentInfo& self, entt::registry& registry, Entity entity) noexcept
			{
				auto& storage = registry.storage<Blob<N>>(self.type);
				return storage.contains(entity) ? storage.get(entity).bytes : nullptr;
			}
		};

		struct TagOps
		{
			static void assure(const ComponentInfo& self, entt::registry& registry) noexcept
			{
				(void)registry.storage<DynamicTag>(self.type);
			}

			static void emplace(const ComponentInfo& self, entt::registry& registry, Entity entity, const void*) noexcept
			{
				auto& storage = registry.storage<DynamicTag>(self.type);
				if (!storage.contains(entity))
					storage.emplace(entity);
			}

			static void remove(const ComponentInfo& self, entt::registry& registry, Entity entity) noexcept
			{
				registry.storage<DynamicTag>(self.type).remove(entity);
			}

			static const void* find(const ComponentInfo& self, const entt::registry& registry, Entity entity) noexcept
			{
				const auto* storage = registry.storage<DynamicTag>(self.type);
				return storage != nullptr && storage->contains(entity) ? &detail::TAG_VALUE<DynamicTag> : nullptr;
			}

			static void* get(const ComponentInfo& self, entt::registry& registry, Entity entity) noexcept
			{
				return registry.storage<DynamicTag>(self.type).contains(entity) ? &detail::TAG_VALUE<DynamicTag>
																				: nullptr;
			}
		};

		template <class Ops> void install(ComponentInfo& info) noexcept
		{
			info.assure	 = &Ops::assure;
			info.emplace = &Ops::emplace;
			info.remove	 = &Ops::remove;
			info.find	 = &Ops::find;
			info.get	 = &Ops::get;
		}

		template <class T> [[nodiscard]] T read_as(const void* p) noexcept
		{
			T value;
			std::memcpy(&value, p, sizeof(T));
			return value;
		}

		template <class T> void store_as(void* p, T value) noexcept { std::memcpy(p, &value, sizeof(T)); }

		/** The wire form of a dynamic component: field by field, in layout order, each at its natural width. */
		template <class Stream> bool serialize_dynamic(const ComponentInfo& self, Stream& stream, u8* bytes) noexcept
		{
			for (const FieldInfo& field : self.fields)
			{
				u8* p = bytes + field.offset;
				switch (field.type)
				{
					case FieldType::Bool:
					{
						bool value = Stream::IsWriting ? read_as<bool>(p) : false;
						serialize_bool(stream, value);
						if (Stream::IsReading)
							store_as<bool>(p, value);
						break;
					}
					case FieldType::U8:
					{
						u32 value = Stream::IsWriting ? read_as<u8>(p) : 0;
						serialize_bits(stream, value, 8);
						if (Stream::IsReading)
							store_as<u8>(p, static_cast<u8>(value));
						break;
					}
					case FieldType::U16:
					{
						u32 value = Stream::IsWriting ? read_as<u16>(p) : 0;
						serialize_bits(stream, value, 16);
						if (Stream::IsReading)
							store_as<u16>(p, static_cast<u16>(value));
						break;
					}
					case FieldType::U32:
					{
						u32 value = Stream::IsWriting ? read_as<u32>(p) : 0;
						serialize_bits(stream, value, 32);
						if (Stream::IsReading)
							store_as<u32>(p, value);
						break;
					}
					case FieldType::I32:
					{
						u32 value = Stream::IsWriting ? static_cast<u32>(read_as<i32>(p)) : 0;
						serialize_bits(stream, value, 32);
						if (Stream::IsReading)
							store_as<i32>(p, static_cast<i32>(value));
						break;
					}
					case FieldType::F32:
					{
						f32 value = Stream::IsWriting ? read_as<f32>(p) : 0.0f;
						serialize_float(stream, value);
						if (Stream::IsReading)
							store_as<f32>(p, value);
						break;
					}
					case FieldType::Vec2:
					{
						f32 x = Stream::IsWriting ? read_as<f32>(p) : 0.0f;
						f32 y = Stream::IsWriting ? read_as<f32>(p + 4) : 0.0f;
						serialize_float(stream, x);
						serialize_float(stream, y);
						if (Stream::IsReading)
						{
							store_as<f32>(p, x);
							store_as<f32>(p + 4, y);
						}
						break;
					}
					default:
						break;
				}
			}
			return true;
		}

		bool write_dynamic(const ComponentInfo& self, serialize::WriteStream& stream, const void* value) noexcept
		{
			// The macros read through the pointer when writing: nothing is stored.
			return serialize_dynamic(self, stream, static_cast<u8*>(const_cast<void*>(value)));
		}

		bool read_dynamic(const ComponentInfo& self, serialize::ReadStream& stream, void* value) noexcept
		{
			return serialize_dynamic(self, stream, static_cast<u8*>(value));
		}

		/** Floats and vectors move; everything else holds the earlier sample until t reaches 1. */
		void interpolate_dynamic(const ComponentInfo& self, const void* from, const void* to, f32 t, void* out) noexcept
		{
			std::memcpy(out, t < 1.0f ? from : to, self.size);
			for (const FieldInfo& field : self.fields)
			{
				const u8* a = static_cast<const u8*>(from) + field.offset;
				const u8* b = static_cast<const u8*>(to) + field.offset;
				u8* o		= static_cast<u8*>(out) + field.offset;
				if (field.type == FieldType::F32)
				{
					store_as<f32>(o, read_as<f32>(a) + (read_as<f32>(b) - read_as<f32>(a)) * t);
				}
				else if (field.type == FieldType::Vec2)
				{
					store_as<f32>(o, read_as<f32>(a) + (read_as<f32>(b) - read_as<f32>(a)) * t);
					store_as<f32>(o + 4, read_as<f32>(a + 4) + (read_as<f32>(b + 4) - read_as<f32>(a + 4)) * t);
				}
			}
		}
	}

	const FieldInfo* ComponentInfo::field(StringView name) const noexcept
	{
		for (const FieldInfo& candidate : fields)
			if (candidate.name == name)
				return &candidate;
		return nullptr;
	}

	bool describe_dynamic(const DynamicComponentDef& def, ComponentInfo& out) noexcept
	{
		const Kind kind = normalize(def.kind);
		if (def.name.empty() || !one_home(kind))
		{
			EMBER_WARN("component '{}': a name, and one of Sim, Server and Client", StringView(def.name));
			return false;
		}
		for (size_t i = 0; i < def.fields.size(); ++i)
			for (size_t j = i + 1; j < def.fields.size(); ++j)
				if (def.fields[i].name == def.fields[j].name)
				{
					EMBER_WARN("component '{}': field '{}' twice", StringView(def.name), StringView(def.fields[i].name));
					return false;
				}

		// Each field at its natural alignment, in the order given: the same bytes on every machine.
		u32 size = 0;
		Vector<FieldInfo> fields(&memory::heap(MemoryTag::ECS));
		for (const FieldDef& field : def.fields)
		{
			const u32 width = field_size(field.type);
			if (width == 0)
				return false;
			size = (size + width - 1) / width * width;
			fields.push_back({.name = String(field.name, &memory::heap(MemoryTag::ECS)), .type = field.type, .offset = size});
			size += width;
		}
		size = (size + 7) / 8 * 8;

		if (has_any(kind, Kind::Replicated) && size > MAX_REPLICATED_BYTES)
		{
			EMBER_WARN("component '{}': a Replicated component is {} bytes at most; this one is {}", StringView(def.name),
					   MAX_REPLICATED_BYTES, size);
			return false;
		}

		out.own_name = String(def.name, &memory::heap(MemoryTag::ECS));
		out.name	 = out.own_name;
		out.kind	 = kind;
		out.size	 = size;
		out.dynamic	 = true;
		out.fields	 = std::move(fields);

		// Named for itself, apart from every C++ type's hash, which is of the type's name alone.
		String storage_name("dynamic/", &memory::heap(MemoryTag::ECS));
		storage_name += def.name;
		out.type = entt::hashed_string::value(storage_name.c_str(), storage_name.size());

		out.defaults.assign(size, 0);
		for (size_t i = 0; i < def.fields.size(); ++i)
			write_field(out.fields[i], out.defaults.data(), def.fields[i].value, def.fields[i].y);
		return true;
	}

	ComponentId Components::add_dynamic(const DynamicComponentDef& def) noexcept
	{
		if (find(StringView(def.name)) != nullptr)
		{
			EMBER_WARN("component '{}': the name is taken", StringView(def.name));
			return NO_COMPONENT;
		}

		ComponentInfo described;
		if (!describe_dynamic(def, described))
			return NO_COMPONENT;

		EMBER_ASSERT(m_infos.size() < MAX_COMPONENTS && "more component types than MAX_COMPONENTS");

		ComponentInfo& info = m_infos.emplace_back();
		info.own_name		= std::move(described.own_name);
		info.name			= info.own_name;
		info.kind			= described.kind;
		info.size			= described.size;
		info.id				= static_cast<ComponentId>(m_infos.size() - 1);
		info.type			= described.type;
		info.dynamic		= true;
		info.fields			= std::move(described.fields);
		info.defaults		= std::move(described.defaults);

		switch (blob_for(info.size))
		{
			case 0:
				install<TagOps>(info);
				break;
			case 8:
				install<BlobOps<8>>(info);
				break;
			case 16:
				install<BlobOps<16>>(info);
				break;
			case 32:
				install<BlobOps<32>>(info);
				break;
			case 64:
				install<BlobOps<64>>(info);
				break;
			case 128:
				install<BlobOps<128>>(info);
				break;
			default:
				install<BlobOps<256>>(info);
				break;
		}

		if (has_any(info.kind, Kind::Replicated))
		{
			info.write = &write_dynamic;
			info.read  = &read_dynamic;
		}
		if (has_any(info.kind, Kind::Interpolated))
			info.interpolate = &interpolate_dynamic;

		return info.id;
	}

	const ComponentInfo* Components::find(std::string_view name) const noexcept
	{
		for (const ComponentInfo& info : m_infos)
		{
			if (info.name == name)
				return &info;
		}

		return nullptr;
	}

	const ComponentInfo* Components::find(entt::id_type type) const noexcept
	{
		for (const ComponentInfo& info : m_infos)
		{
			if (info.type == type)
				return &info;
		}

		return nullptr;
	}

	f64 read_field(const FieldInfo& field, const void* component) noexcept
	{
		const u8* p = static_cast<const u8*>(component) + field.offset;
		switch (field.type)
		{
			case FieldType::Bool:
				return read_as<bool>(p) ? 1.0 : 0.0;
			case FieldType::U8:
				return read_as<u8>(p);
			case FieldType::U16:
				return read_as<u16>(p);
			case FieldType::U32:
				return read_as<u32>(p);
			case FieldType::I32:
				return read_as<i32>(p);
			case FieldType::F32:
			case FieldType::Vec2:
				return read_as<f32>(p);
			default:
				return 0.0;
		}
	}

	void write_field(const FieldInfo& field, void* component, f64 value, f32 y) noexcept
	{
		u8* p = static_cast<u8*>(component) + field.offset;
		switch (field.type)
		{
			case FieldType::Bool:
				store_as<bool>(p, value != 0.0);
				return;
			case FieldType::U8:
				store_as<u8>(p, static_cast<u8>(std::clamp(value, 0.0, 255.0)));
				return;
			case FieldType::U16:
				store_as<u16>(p, static_cast<u16>(std::clamp(value, 0.0, 65535.0)));
				return;
			case FieldType::U32:
				store_as<u32>(p, static_cast<u32>(std::clamp(value, 0.0, 4294967295.0)));
				return;
			case FieldType::I32:
				store_as<i32>(p, static_cast<i32>(std::clamp(value, -2147483648.0, 2147483647.0)));
				return;
			case FieldType::F32:
				store_as<f32>(p, static_cast<f32>(value));
				return;
			case FieldType::Vec2:
				store_as<f32>(p, static_cast<f32>(value));
				store_as<f32>(p + 4, y);
				return;
			default:
				return;
		}
	}
}
