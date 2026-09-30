#include <ember/core/hash.h>
#include <ember/core/json.h>
#include <ember/material/type.h>
#include <ember/memory/memory.h>

#include <fmt/format.h>

#include <bit>
#include <charconv>
#include <cstring>
#include <iterator>

namespace ember::material
{
	namespace
	{
		/// Bumped whenever a change would make an older cook misread; the reader refuses other
		/// versions outright, since a cooked type can always be cooked again.
		const u32 TYPE_FORMAT = 1;

		/// The one size a parameter of this shape can have: what makes a stale of damaged file
		/// detectable before any record is written through it.
		constexpr u32 param_bytes(ParamKind kind, u8 components) noexcept
		{
			return is_texture(kind) ? TEXTURE_REF_BYTES : 4u * components;
		}

		u64 hash_layout(const Layout& layout, u64 hash) noexcept
		{
			hash = hash_value(layout.size, hash);

			for (const Param& param : layout.params)
			{
				hash = hash_text(param.name, hash);
				hash = hash_text(param.display, hash);
				hash = hash_text(param.preset, hash);
				hash = hash_value(param.kind, hash);
				hash = hash_value(param.components, hash);
				hash = hash_value(param.offset, hash);
				hash = hash_value(param.size, hash);
				hash = hash_value(param.color, hash);
				hash = hash_value(param.hdr, hash);
				hash = hash_value(param.linear, hash);
				hash = hash_value(param.has_range, hash);
				hash = hash_value(param.range_min, hash);
				hash = hash_value(param.range_max, hash);
				hash = hash_value(param.filter, hash);
				hash = hash_value(param.wrap, hash);
			}

			return hash;
		}

		void write_layout(JsonNode node, const Layout& layout) noexcept
		{
			node.set("size", layout.size);
			JsonNode params = node.array("params");

			// Keys that hold their default are left out: the files stay short and a diff shows
			// only what an author actually declared.
			for (const Param& param : layout.params)
			{
				JsonNode entry = params.push_object();
				entry.set("name", StringView(param.name));
				entry.set("kind", enum_name(param.kind));

				if (param.components != 1)
					entry.set("components", u32{param.components});

				entry.set("offset", param.offset);
				entry.set("size", param.size);

				if (!param.display.empty())
					entry.set("display", StringView(param.display));
				if (!param.preset.empty())
					entry.set("default", StringView(param.preset));
				if (param.color)
					entry.set("color", true);
				if (param.hdr)
					entry.set("hdr", true);

				if (param.has_range)
				{
					JsonNode range = entry.array("range");
					range.push(param.range_min);
					range.push(param.range_max);
				}

				if (is_texture(param.kind))
				{
					entry.set("filter", enum_name(param.filter));
					entry.set("wrap", enum_name(param.wrap));

					if (param.linear)
						entry.set("linear", true);
				}
			}
		}

		/// Records the first problem a read ifnds; every later check reads false through it.
		template <class... Args> bool refuse(String& error, fmt::format_string<Args...> format, Args&&... args)
		{
			error.clear();
			fmt::format_to(std::back_inserter(error), format, std::forward<Args>(args)...);
			return false;
		}

		bool read_layout(JsonValue node, StringView what, Layout& out, String& error) noexcept
		{
			if (!node.is_object())
				return true; // an absent section is an empty layout

			if (!node["size"].read(out.size))
				return refuse(error, "{} has no size", what);

			for (JsonValue entry : node["params"].elements())
			{
				Param& param = out.params.emplace_back();

				StringView name;
				if (!entry["name"].read(name) || name.empty())
					return refuse(error, "{} parameter {} has no name", what, out.params.size() - 1);

				param.name = name;

				u32 components = 1;
				(void)entry["components"].read(components);

				if (!entry["kind"].read_enum(param.kind))
					return refuse(error, "{} '{}' has an unknown kind", what, name);

				if (components < 1 || components > 4 || (is_texture(param.kind) && components != 1))
					return refuse(error, "{} '{}' cannot have {} components", what, name, components);

				param.components = static_cast<u8>(components);

				if (!entry["offset"].read(param.offset) || !entry["size"].read(param.size))
					return refuse(error, "{} '{}' has no offset or size", what, name);

				if (param.size != param_bytes(param.kind, param.components))
					return refuse(error, "{} '{}' is {} bytes, which its kind cannot be", what, name, param.size);

				if (u64{param.offset} + param.size > out.size)
					return refuse(error, "{} '{}' runs past the end ({} + {} > {})", what, name, param.offset,
								  param.size, out.size);

				StringView text;
				if (entry["display"].read(text))
					param.display = text;
				if (entry["default"].read(text))
					param.preset = text;

				param.color	 = entry["color"].read_or(false);
				param.hdr	 = entry["hdr"].read_or(false);
				param.linear = entry["linear"].read_or(false);

				f32 range[2] = {};
				if (entry["range"].numbers(Span<f32>(range, 2)) == 2)
				{
					param.has_range = true;
					param.range_min = range[0];
					param.range_max = range[1];
				}

				if (JsonValue filter = entry["filter"]; filter && !filter.read_enum(param.filter))
					return refuse(error, "{} '{}' has an unknown filter", what, name);

				if (JsonValue wrap = entry["wrap"]; wrap && !wrap.read_enum(param.wrap))
					return refuse(error, "{} '{}' has an unknown wrap", what, name);
			}

			return true;
		}
	}

	bool parse_value(const Param& param, StringView text, u32 (&words)[4]) noexcept
	{
		if (is_texture(param.kind))
			return false;

		const auto separator = [](char c) { return c == ' ' || c == '\t' || c == ','; };

		u32 count = 0;
		size_t at = 0;

		while (at < text.size())
		{
			if (separator(text[at]))
			{
				++at;
				continue;
			}

			size_t end = at;
			while (end < text.size() && !separator(text[end]))
				++end;

			if (count == param.components)
				return false; // more values than the parameter has components

			const char* first = text.data() + at;
			const char* last  = text.data() + end;
			u32& word		  = words[count++];

			// from_chars must consume the whole token, so "0.5x" is an error rather than 0.5.
			const auto parsed = [&](auto& value) noexcept
			{
				const auto [ptr, ec] = std::from_chars(first, last, value);
				return ec == std::errc{} && ptr == last;
			};

			switch (param.kind)
			{
				case ParamKind::Float:
				{
					f32 value = 0.0f;
					if (!parsed(value))
						return false;
					word = std::bit_cast<u32>(value);
					break;
				}
				case ParamKind::Int:
				{
					i32 value = 0;
					if (!parsed(value))
						return false;
					word = std::bit_cast<u32>(value);
					break;
				}
				case ParamKind::Uint:
				{
					if (!parsed(word))
						return false;
					break;
				}
				case ParamKind::Bool:
				{
					const StringView token(first, end - at);
					if (token != "true" && token != "false" && token != "1" && token != "0")
						return false;
					word = token == "true" || token == "1" ? 1u : 0u;
					break;
				}
				default:
					return false;
			}

			at = end;
		}

		return count == param.components;
	}

	const Param* find_param(const Layout& layout, StringView name) noexcept
	{
		for (const Param& param : layout.params)
			if (param.name == name)
				return &param;

		return nullptr;
	}

	u64 hash_type(const Type& type) noexcept
	{
		u64 hash = hash_bytes(type.bytecode());
		hash	 = hash_value(type.domain, hash);
		hash	 = hash_text(type.name, hash);
		hash	 = hash_layout(type.record, hash);
		hash	 = hash_layout(type.instance, hash);

		const State& state = type.state;
		hash			   = hash_value(state.queue, hash);
		hash			   = hash_value(state.cull, hash);
		hash			   = hash_value(state.blend, hash);
		hash			   = hash_value(state.depth_write, hash);
		hash			   = hash_value(state.casts_shadow, hash);
		hash			   = hash_value(state.priority, hash);
		return hash_text(state.shading, hash);
	}

	bool write_type(const Type& type, String& out) noexcept
	{
		JsonWriter writer(memory::heap(MemoryTag::Graphics));
		JsonNode root = writer.root_object();

		// Hex, because a u64 does not survive a JSON reader that parses numbers as doubles.
		char hash[17] = {};
		fmt::format_to_n(hash, 16, "{:016x}", type.hash);

		root.set("format", TYPE_FORMAT);
		root.set("domain", enum_name(type.domain));
		root.set("name", StringView(type.name));
		root.set("hash", StringView(hash, 16));

		const State& state = type.state;
		JsonNode node	   = root.object("state");
		node.set("queue", enum_name(state.queue));
		node.set("cull", enum_name(state.cull));
		node.set("blend", enum_name(state.blend));
		node.set("depth_write", state.depth_write);
		node.set("casts_shadow", state.casts_shadow);
		node.set("priority", state.priority);

		if (!state.shading.empty())
			node.set("shading", StringView(state.shading));

		write_layout(root.object("record"), type.record);

		if (!type.instance.params.empty())
			write_layout(root.object("instance"), type.instance);

		return writer.write(out);
	}

	bool read_type(StringView text, Type& out, String& error) noexcept
	{
		Json json;
		JsonError parse_error;

		if (!json.parse(text, memory::heap(MemoryTag::Graphics), JsonRead::Strict, &parse_error))
			return refuse(error, "{}:{}: {}", parse_error.line, parse_error.column, parse_error.message);

		const JsonValue root = json.root();

		u32 format = 0;
		if (!root["format"].read(format) || format != TYPE_FORMAT)
			return refuse(error, "format {} is not {}; cook the type again", format, TYPE_FORMAT);

		if (!root["domain"].read_enum(out.domain))
			return refuse(error, "unknown domain");

		StringView name;
		if (!root["name"].read(name) || name.empty())
			return refuse(error, "no name");

		out.name = name;

		StringView hash;
		if (!root["hash"].read(hash) ||
			std::from_chars(hash.data(), hash.data() + hash.size(), out.hash, 16).ec != std::errc{})
			return refuse(error, "no hash");

		const JsonValue state = root["state"];
		State& to			  = out.state;

		if (!state["queue"].read_enum(to.queue) || !state["cull"].read_enum(to.cull) ||
			!state["blend"].read_enum(to.blend) || !state["depth_write"].read(to.depth_write) ||
			!state["casts_shadow"].read(to.casts_shadow) || !state["priority"].read(to.priority))
			return refuse(error, "incomplete state");

		StringView shading;
		to.shading = state["shading"].read(shading) ? shading : StringView();

		out.record	 = {};
		out.instance = {};

		return read_layout(root["record"], "record", out.record, error) &&
			   read_layout(root["instance"], "instance", out.instance, error);
	}

	bool read_cooked(StringView type_file, Span<const u8> spirv, Type& out, String& error) noexcept
	{
		if (!read_type(type_file, out, error))
			return false;

		if (spirv.empty() || spirv.size() % sizeof(u32) != 0)
			return refuse(error, "the bytecode is {} bytes, which is not SPIR-V", spirv.size());

		// Copied into words, since a file or an embedded array promises no alignment.
		out.spirv.resize(spirv.size() / sizeof(u32));
		std::memcpy(out.spirv.data(), spirv.data(), spirv.size());
		out.dependencies.clear();

		if (hash_type(out) != out.hash)
			return refuse(error, "'{}' was not cooked with this bytecode; cook it again", out.name);

		return true;
	}
}
