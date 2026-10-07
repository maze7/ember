#include <ember/assets/material_asset.h>
#include <ember/core/filesystem.h>
#include <ember/core/json.h>
#include <ember/core/logger.h>
#include <ember/jobs/job_system.h>

#if EMBER_SHADER_COMPILER
	#include <ember/shader/compiler.h>
#endif

#include <fmt/format.h>

#include <bit>
#include <cmath>
#include <iterator>
#include <limits>

namespace ember
{
	using material::BuiltinTexture;
	using material::is_texture;
	using material::Param;
	using material::ParamKind;
	using render::MaterialRegistry;
	using render::MaterialTypeHandle;
	using render::StockType;

	namespace
	{
		[[nodiscard]] Heap& assets_heap() noexcept { return memory::heap(MemoryTag::Assets); }

		/// Where each stock type's source is served, as a .material names it.
		constexpr struct
		{
			StringView path;
			StockType type;
		} STOCK_SOURCES[] = {
			{"ember/materials/unlit.slang", StockType::Unlit},
			{"ember/materials/sprite.slang", StockType::Sprite},
		};

		static_assert(std::size(STOCK_SOURCES) == static_cast<size_t>(StockType::Count));
		static_assert(STOCK_SOURCES[0].path.starts_with(ENGINE_SHADER_MOUNT));

		[[nodiscard]] StringView text_of(Span<const u8> bytes) noexcept
		{
			return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
		}

		/// "", "2", "3" or "4": what follows a kind's name to spell a vector.
		[[nodiscard]] StringView width(u8 components) noexcept
		{
			constexpr StringView widths[] = {"", "", "2", "3", "4"};
			return components < std::size(widths) ? widths[components] : StringView();
		}

		[[nodiscard]] Span<const u8> as_bytes(StringView text) noexcept
		{
			return {reinterpret_cast<const u8*>(text.data()), text.size()};
		}

		/** The name of one of a type's pair: its source's name with the extension swapped. */
		[[nodiscard]] String pair_name(StringView source, const char* extension) noexcept
		{
			String name(source.substr(0, source.size() - fs::extension(source).size()), &assets_heap());
			name += extension;
			return name;
		}

		/**
		 * A type from the pair ember_cook, or a cooking build, wrote beside its source, read by name
		 * through whatever source holds the type: a directory, or a pack. Both files become
		 * dependencies by being read, so a cook that rewrites them reloads the type.
		 */
		[[nodiscard]] bool read_pair(AssetLoad& load, material::Type& out, String& problems) noexcept
		{
			const String type_name	= pair_name(load.path(), material::TYPE_EXTENSION);
			const String spirv_name = pair_name(load.path(), material::SPIRV_EXTENSION);

			const auto type_file = load.read(type_name);
			const auto spirv	 = load.read(spirv_name);

			if (!type_file || !spirv)
			{
				fmt::format_to(std::back_inserter(problems), "no cooked pair beside it: {} and {} are read together",
							   type_name, spirv_name);
				return false;
			}

			return material::read_cooked(type_file->text(), spirv->bytes(), out, problems);
		}

		/** Writes a file only when its bytes differ, as ember_cook does: an unchanged pair wakes no watcher. */
		[[nodiscard]] bool write_if_changed(StringView path, Span<const u8> bytes, String& problems,
											bool& wrote) noexcept
		{
			wrote = false;

			if (const auto existing = fs::read_file(path, assets_heap());
				existing && existing->size() == bytes.size() &&
				std::equal(bytes.begin(), bytes.end(), existing->data()))
				return true;

			if (const auto written = fs::write_file_atomic(path, bytes, fs::WriteDurability::None); !written)
			{
				fmt::format_to(std::back_inserter(problems), "cannot write {} ({})", path,
							   enum_name(written.error().code));
				return false;
			}

			wrote = true;
			return true;
		}

		/** The pair ember_cook would write beside the source, from a build: the SPIR-V and the .type file. */
		[[nodiscard]] bool write_pair(StringView file, const material::Type& type, String& problems,
									  MaterialAssets::PairWritten& written) noexcept
		{
			String type_text(&assets_heap());
			if (!material::write_type(type, type_text))
			{
				problems = "its .type file could not be written: a value in it has no spelling";
				return false;
			}

			return write_if_changed(pair_name(file, material::SPIRV_EXTENSION), type.bytecode(), problems,
									written.spirv) &&
				   write_if_changed(pair_name(file, material::TYPE_EXTENSION), as_bytes(type_text), problems,
									written.type);
		}

		/// A value as the file spells it. False for a spelling no parameter could hold.
		[[nodiscard]] bool read_value(AssetLoad& load, JsonValue json, MaterialValue& out) noexcept
		{
			// Numbers: one, or up to four in an array, all numbers or all true and false.
			if (json.is_number() || json.is_bool() || json.is_array())
			{
				const u32 count = json.is_array() ? json.size() : 1;

				if (count == 0 || count > std::size(out.numbers))
					return false;

				out.form	 = MaterialValue::Form::Numbers;
				out.count	 = static_cast<u8>(count);
				out.booleans = (json.is_array() ? json[0u] : json).is_bool();

				for (u32 i = 0; i < count; ++i)
				{
					const JsonValue element = json.is_array() ? json[i] : json;
					bool flag				= false;

					if (out.booleans ? !element.read(flag) : !element.read(out.numbers[i]))
						return false;

					if (out.booleans)
						out.numbers[i] = flag ? 1.0 : 0.0;
				}

				return true;
			}

			// A texture: a path or a builtin's name, and in an object, with a sampler of its own.
			StringView name;
			const JsonValue texture = json.is_object() ? json["texture"] : json;

			if (!texture.read(name) || name.empty())
				return false;

			if (json.is_object())
			{
				const JsonValue filter = json["filter"];
				const JsonValue wrap   = json["wrap"];

				out.has_filter = !filter.is_null();
				out.has_wrap   = !wrap.is_null();

				if ((out.has_filter && !filter.read_enum(out.filter)) || (out.has_wrap && !wrap.read_enum(out.wrap)))
					return false;
			}

			if (parse_enum(name, out.builtin))
			{
				out.form = MaterialValue::Form::Builtin;
				return true;
			}

			// Requested, not waited for: the texture binds when it arrives.
			out.form	= MaterialValue::Form::Texture;
			out.texture = load.load<TextureAsset>(name);
			return !out.texture.is_null();
		}

		/// The file's numbers as the parameter holds them: its kind, and exactly its width.
		[[nodiscard]] bool to_number(const Param& param, const MaterialValue& value,
									 render::detail::NumberValue& out) noexcept
		{
			if (is_texture(param.kind) || value.count != param.components)
				return false;

			out.kind	   = param.kind;
			out.components = param.components;

			for (u32 i = 0; i < value.count; ++i)
			{
				const f64 number	= value.numbers[i];
				const bool integral = !value.booleans && number == std::trunc(number);

				switch (param.kind)
				{
					case ParamKind::Float:
						if (value.booleans)
							return false;
						out.words[i] = std::bit_cast<u32>(static_cast<f32>(number));
						break;

					case ParamKind::Int:
						if (!integral || number < std::numeric_limits<i32>::min() ||
							number > std::numeric_limits<i32>::max())
							return false;
						out.words[i] = std::bit_cast<u32>(static_cast<i32>(number));
						break;

					case ParamKind::Uint:
						if (!integral || number < 0.0 || number > std::numeric_limits<u32>::max())
							return false;
						out.words[i] = static_cast<u32>(number);
						break;

					case ParamKind::Bool:
						if (number != 0.0 && number != 1.0)
							return false;
						out.words[i] = number != 0.0 ? 1u : 0u;
						break;

					default:
						return false;
				}
			}

			return true;
		}

		/**
		 * Writes one value through the type's layout. Numbers are written only with `everything`;
		 * a texture also whenever its state has moved since it was last written, which is what a
		 * refresh brings: the file's texture once it loads, the error texture if it will not, and
		 * until then the parameter's default, so a texture that is merely slow never looks wrong.
		 */
		void write(MaterialRegistry& registry, const MaterialAsset& asset, const material::Type& type,
				   MaterialValue& value, bool everything) noexcept
		{
			const Param* param = material::find_param(type.record, value.name);

			if (value.form == MaterialValue::Form::Numbers)
			{
				if (!everything)
					return;

				render::detail::NumberValue number;

				if (param == nullptr)
					EMBER_WARN("{}: {} has no parameter '{}'", asset.path, type.name, value.name);
				else if (!to_number(*param, value, number))
					EMBER_WARN("{}: '{}' does not fit {}.{}, which holds {}{}", asset.path, value.name, type.name,
							   value.name, enum_name(param->kind), width(param->components));
				else
					(void)registry.set(asset.material, value.name, number);

				return;
			}

			const AssetState state =
				value.form == MaterialValue::Form::Builtin ? AssetState::Loaded : value.texture.state();

			if (!everything && state == value.written)
				return;

			value.written = state;

			if (param == nullptr || !is_texture(param->kind))
			{
				EMBER_WARN("{}: {} has no texture parameter '{}'", asset.path, type.name, value.name);
				return;
			}

			BuiltinTexture builtin = value.builtin;
			TextureHandle texture  = {};

			if (value.form == MaterialValue::Form::Texture)
			{
				if (state == AssetState::Loaded)
				{
					texture = value.texture->texture;
				}
				else
				{
					// The parameter's own default while it loads, the checker once it has failed.
					builtin = BuiltinTexture::White;
					if (state == AssetState::Loading)
						(void)parse_enum(StringView(param->preset), builtin);
					else
						builtin = BuiltinTexture::Error;
				}
			}

			if (texture.is_null() && !value.has_filter && !value.has_wrap)
			{
				(void)registry.set(asset.material, value.name, builtin);
				return;
			}

			if (texture.is_null())
				texture = registry.builtin(builtin);

			if (!value.has_filter && !value.has_wrap)
				(void)registry.set(asset.material, value.name, texture);
			else
				(void)registry.set(asset.material, value.name, texture, value.has_filter ? value.filter : param->filter,
								   value.has_wrap ? value.wrap : param->wrap);
		}

		/**
		 * Makes the registry's material what the file says, as far as what the file names has arrived:
		 * its type, then each value through that type's layout. With `everything` the record is
		 * rebuilt from the type's defaults and the file's values alone, so a value taken out of the
		 * file goes back to its default; otherwise only what has changed since is written.
		 */
		void bind(MaterialRegistry& registry, MaterialAsset& asset, bool everything) noexcept
		{
			// While the type the file names is on its way (a save named another one), the material stays
			// as it was: the type drawing now, and the values written through it.
			if (!asset.type.is_null() && asset.type.state() == AssetState::Loading)
				return;

			// A type that failed, or one the registry refused, draws the error type until a save fixes it.
			MaterialTypeHandle target = asset.type ? asset.type->type : MaterialTypeHandle{};

			if (registry.type(target) == nullptr)
				target = registry.error_type();

			if (registry.type_of(asset.material) != target)
			{
				(void)registry.set_type(asset.material, target);
				everything = true;
			}

			asset.drawing = asset.type;

			// A rebuilt type may have gained a parameter the file sets, or changed one's kind.
			const u32 generation = registry.generation(target);
			everything |= generation != asset.generation;
			asset.generation = generation;

			if (target == registry.error_type())
				return;

			if (everything)
				registry.clear(asset.material);

			const material::Type& type = *registry.type(target);

			for (MaterialValue& value : asset.values)
				write(registry, asset, type, value, everything);
		}
	}

	bool MaterialTypeAsset::load(AssetLoad& load, MaterialTypeAsset& out) noexcept
	{
		MaterialAssets& library = load.context<MaterialAssets>();

		// A stock type's path names the engine's own type. Its source is built only for hot reload;
		// otherwise the build cooked into the engine stands, and there is nothing to read.
		out.type  = library.stock_type(load.path());
		out.stock = !out.type.is_null();

		if (out.stock && !library.builds_stock())
			return true;

		String problems(&load.heap());

		// A build without the compiler, or one asked to run cooked: the pair, by name, from whatever
		// source holds the type.
		if (!library.compiles())
		{
			if (read_pair(load, out.build, problems))
				return true;

			EMBER_ERROR("material type '{}' did not load:\n{}", load.path(), problems);
			return false;
		}

		// A compile needs the source as a file: a type in a pack is read cooked, never compiled.
		if (load.file().empty())
		{
			EMBER_ERROR("material type '{}': no source file to compile; it is served by a pack", load.path());
			return false;
		}

		// On the IO thread: a compile reads the source and every file it imports, holds its thread for
		// tens of milliseconds, and wants more stack than a fiber has.
		struct Build
		{
			MaterialAssets* library;
			StringView file;
			bool stock;
			material::Type* type;
			String* problems;
			bool built;
			MaterialAssets::PairWritten written;
		};

		Build build{&library, load.file(), out.stock, &out.build, &problems, false, {}};
		jobs::Counter done;

		const auto submitted = jobs::submit_io(
			{
				.fn =
					[](void* data) noexcept
				{
					Build& build = *static_cast<Build*>(data);
					build.built =
						build.library->build(build.file, build.stock, *build.type, *build.problems, build.written);
				},
				.data	  = &build,
				.name	  = "build material type",
				.priority = jobs::JobPriority::Low,
			},
			done);

		if (!submitted)
		{
			EMBER_ERROR("material type '{}': build refused ({})", load.path(), enum_name(submitted.error()));
			return false;
		}

		jobs::wait(done);

		// What the cook wrote is news now, not when the watcher gets round to it: a host sends the pair
		// on at once, and the watcher's own report of the write is not counted again.
		if (build.written.type)
			load.notify_written(pair_name(load.path(), material::TYPE_EXTENSION));
		if (build.written.spirv)
			load.notify_written(pair_name(load.path(), material::SPIRV_EXTENSION));

		// Everything the build read reloads the type when it changes: its imports, the engine modules it
		// links, or the pair it was cooked into. A failed build names what it got to, where the fix is.
		for (const String& file : out.build.dependencies)
			load.depends_on(file);

		if (!build.built)
		{
			EMBER_ERROR("material type '{}' did not build:\n{}", load.path(), problems);
			return false;
		}

		return true;
	}

	bool MaterialTypeAsset::publish(AssetServices& services, MaterialTypeAsset& asset) noexcept
	{
		MaterialRegistry& registry = services.context<MaterialAssets>().registry();

		// A stock source hands its build to the stock type, if it made one.
		if (asset.stock)
		{
			if (!asset.build.spirv.empty())
				(void)registry.replace_type(asset.type, std::move(asset.build));

			return true;
		}

		// A full registry, or a layout it cannot key, leaves the handle null and logged: materials of
		// the type draw the error type, as they would for a failed compile.
		asset.type = registry.add_type(std::move(asset.build));
		return true;
	}

	void MaterialTypeAsset::reload(AssetServices& services, MaterialTypeAsset& live, MaterialTypeAsset& fresh) noexcept
	{
		MaterialRegistry& registry = services.context<MaterialAssets>().registry();

		// The handle stays and the build behind it changes. One the registry refuses leaves the last good
		// build drawing, as a failed compile does.
		if (live.type.is_null())
			live.type = registry.add_type(std::move(fresh.build));
		else if (!fresh.build.spirv.empty())
			(void)registry.replace_type(live.type, std::move(fresh.build));
	}

	void MaterialTypeAsset::unload(AssetServices& services, MaterialTypeAsset& asset) noexcept
	{
		// Materials still of the type draw the error type from here; a stock type outlives every asset.
		if (!asset.stock && !asset.type.is_null())
			services.context<MaterialAssets>().registry().remove_type(asset.type);
	}

	bool MaterialAsset::load(AssetLoad& load, MaterialAsset& out) noexcept
	{
		out.path = load.path();

		Json json;
		JsonError error;

		if (!json.parse(text_of(load.bytes()), load.heap(), JsonRead::Relaxed, &error))
		{
			EMBER_ERROR("{}:{}:{}: {}", load.path(), error.line, error.column, error.message);
			return false;
		}

		const JsonValue root = json.root();
		StringView type;

		if (!root["type"].read(type))
		{
			EMBER_ERROR("{}: names no \"type\", the path of a material type's .slang file", load.path());
			return false;
		}

		// Requested, not waited for: the pump publishes this material once the type has settled.
		out.type = load.load<MaterialTypeAsset>(type);

		// A value no parameter could hold fails the whole file, so a save with a typo in it keeps the
		// material as it was rather than lose that value to its default.
		for (const auto [name, json_value] : root["values"].members())
		{
			MaterialValue& value = out.values.emplace_back();
			value.name			 = name;

			if (!read_value(load, json_value, value))
			{
				EMBER_ERROR("{}: \"{}\" is neither numbers nor a texture: up to four numbers, true or false, a "
							"texture's path or a builtin's name, or an object with a \"texture\"",
							load.path(), name);
				return false;
			}
		}

		return true;
	}

	bool MaterialAsset::publish(AssetServices& services, MaterialAsset& asset) noexcept
	{
		// Not before the type has settled: until then there is no layout to write the values through, and a
		// material made now would draw the error type for the frames in between.
		if (!asset.type.is_null() && asset.type.state() == AssetState::Loading)
			return false;

		MaterialRegistry& registry	  = services.context<MaterialAssets>().registry();
		const MaterialTypeHandle type = asset.type ? asset.type->type : MaterialTypeHandle{};

		// Made of the type it will draw with, so binding moves nothing. A full registry leaves the handle
		// null, logged, and objects given it draw the error type.
		asset.material = registry.create(registry.type(type) != nullptr ? type : registry.error_type());

		if (!asset.material.is_null())
			bind(registry, asset, true);

		return true;
	}

	void MaterialAsset::refresh(AssetServices& services, MaterialAsset& asset) noexcept
	{
		if (!asset.material.is_null())
			bind(services.context<MaterialAssets>().registry(), asset, false);
	}

	void MaterialAsset::reload(AssetServices& services, MaterialAsset& live, MaterialAsset& fresh) noexcept
	{
		// The file's new say, into the material everyone holds. The old say leaves with `fresh`, and its
		// references go when that is unloaded, after the new ones have taken hold.
		std::swap(live.type, fresh.type);
		std::swap(live.values, fresh.values);

		if (!live.material.is_null())
			bind(services.context<MaterialAssets>().registry(), live, true);
	}

	void MaterialAsset::unload(AssetServices& services, MaterialAsset& asset) noexcept
	{
		// Objects still using it draw the error type from the next sync: the game let go of it too soon.
		services.context<MaterialAssets>().registry().destroy(asset.material);
	}

	MaterialAssets::MaterialAssets() noexcept = default;

	MaterialAssets::~MaterialAssets() noexcept
	{
#if EMBER_SHADER_COMPILER
		// Every build ran on a loader's behalf, and the asset manager waited for its loaders.
		memory::delete_object(MemoryTag::Tools, m_compiler);
#endif
	}

	void MaterialAssets::init(AssetManager& assets, MaterialRegistry& registry, const MaterialAssetsDef& def) noexcept
	{
		EMBER_ASSERT(m_registry == nullptr && "init runs once");

		m_registry = &registry;

		assets.register_type<MaterialTypeAsset>("material type", this);
		assets.register_type<MaterialAsset>("material", this);

#if EMBER_SHADER_COMPILER
		m_cooked	  = def.cooked;
		m_write_pairs = def.write_pairs;

		if (!m_cooked)
		{
			m_compiler	 = memory::new_object<shader::Compiler>(MemoryTag::Tools);
			m_engine_dir = def.engine_dir != nullptr ? def.engine_dir : shader::Compiler::configured_engine_dir();

			// The asset root first: a type imports a library by its path below the root, as it loads one.
			m_include_dirs.emplace_back(assets.root());

			for (const char* dir : def.include_dirs)
				m_include_dirs.emplace_back(dir);

			// The engine's shaders under a prefix of their own. Every file a compile reads then has a name
			// hot reload knows, so an edit to an engine module reaches every type that links it.
			assets.mount(ENGINE_SHADER_MOUNT, m_engine_dir);

			// Without a watch, a stock source would only build the type the engine already embeds.
			m_build_stock = assets.watching();
		}
#else
		(void)def;
#endif

		for (u32 i = 0; m_build_stock && i < std::size(STOCK_SOURCES); ++i)
			m_stock_sources[i] = assets.load<MaterialTypeAsset>(STOCK_SOURCES[i].path);
	}

	void MaterialAssets::shutdown() noexcept
	{
		for (AssetRef<MaterialTypeAsset>& source : m_stock_sources)
			source.reset();
	}

	MaterialTypeHandle MaterialAssets::stock_type(StringView path) const noexcept
	{
		for (const auto& source : STOCK_SOURCES)
			if (path == source.path)
				return m_registry->stock_type(source.type);

		return {};
	}

	bool MaterialAssets::build(StringView file, bool stock, material::Type& out, String& problems,
							   PairWritten& written) noexcept
	{
#if EMBER_SHADER_COMPILER
		if (!m_cooked)
		{
			// The compiler's global session takes tens of milliseconds to start. The first build pays,
			// here on the IO thread, rather than every boot on the owner thread.
			std::call_once(m_start,
						   [this]() noexcept
						   {
							   Vector<const char*> dirs(&assets_heap());
							   for (const String& dir : m_include_dirs)
								   dirs.push_back(dir.c_str());

							   m_started = m_compiler->initialize({
								   .engine_dir	 = m_engine_dir.c_str(),
								   .include_dirs = {dirs.data(), dirs.size()},
							   });
						   });

			if (!m_started)
			{
				problems = "the shader compiler did not start; its error is logged above";
				return false;
			}

			const auto source = fs::read_file(file, assets_heap());

			if (!source)
			{
				fmt::format_to(std::back_inserter(problems), "cannot be read ({})", enum_name(source.error().code));
				return false;
			}

			if (!m_compiler->compile_material(file, source->text(), out, problems))
				return false;

			// The cook: a game type's build goes beside its source as the pair a cooked build reads, so
			// the tree holds what this build runs. A stock source lives in the engine's directory, which
			// cooked builds embed, so nothing is written there. The pair is this build's own output, not
			// one of its dependencies: a save compiles once.
			return stock || !m_write_pairs || write_pair(file, out, problems, written);
		}
#endif

		(void)stock;
		(void)out;
		(void)written;
		problems = "this build does not compile; a type loads from its cooked pair";
		return false;
	}
}
