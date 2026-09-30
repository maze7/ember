#pragma once

#include <ember/assets/asset.h>
#include <ember/assets/texture_asset.h>
#include <ember/core/common.h>
#include <ember/gpu/sampler.h>
#include <ember/material/type.h>
#include <ember/render/common.h>
#include <ember/render/material_registry.h>

#include <mutex>

namespace ember::shader
{
	class Compiler;
}

namespace ember
{
	/**
	 * The prefix the engine's shader directory is served under in builds that compile, and the way
	 * a .material names a stock type in every build: "ember/materials/sprite.slang".
	 */
	inline constexpr const char* ENGINE_SHADER_MOUNT = "ember";

	/**
	 * A material type from its Slang source, in the registry for as long as the asset lives.
	 *
	 * The handle never changes. A save rebuilds the type on the IO thread and the pump hands the new
	 * build to replace_type(), which re-encodes every material of the type and moves its generation,
	 * so passes rebuild their pipelines at the next frame. A build that fails keeps the last good one
	 * drawing; a type that never built is a failed asset, and materials that name it draw the error
	 * type until a save fixes it.
	 *
	 * Builds with the compiler compile the source, and hot reload follows every file the compile
	 * read: the type's own imports and the engine modules it links against. Ship builds, and dev
	 * builds that ask to, read the pair ember_cook wrote beside the source. A stock type's path names
	 * the engine's own type: its source is compiled only to hot reload it, into the stock handle.
	 */
	struct MaterialTypeAsset
	{
		render::MaterialTypeHandle type = {};

		/// A build on its way to the registry: made on the IO thread, handed over by the pump.
		material::Type build;

		bool stock = false; // the engine's own type: replaced in place, never removed

		/// The compiler reads the source, and everything it imports, itself; a cooked pair is two files.
		static constexpr bool READS_OWN_FILES = true;

		static bool load(AssetLoad& load, MaterialTypeAsset& out) noexcept;
		static bool publish(AssetServices& services, MaterialTypeAsset& asset) noexcept;
		static void reload(AssetServices& services, MaterialTypeAsset& live, MaterialTypeAsset& fresh) noexcept;
		static void unload(AssetServices& services, MaterialTypeAsset& asset) noexcept;
	};

	static_assert(AssetType<MaterialTypeAsset>);

	/**
	 * One value a .material file sets, read before the file's type is known: numbers as JSON gave
	 * them, or the texture it names. Written through the type's layout once the type is here.
	 */
	struct MaterialValue
	{
		enum class Form : u8
		{
			Numbers,
			Texture,
			Builtin,
		};

		String name{&memory::heap(MemoryTag::Assets)};
		Form form = Form::Numbers;

		// Numbers: up to four, and whether they were written as true and false.
		f64 numbers[4] = {};
		u8 count	   = 0;
		bool booleans  = false;

		// Textures: the file's, or a builtin, and whatever of its sampler the value picks itself; the
		// parameter's [Filter] and [Wrap] stand for the rest.
		AssetRef<TextureAsset> texture;
		material::BuiltinTexture builtin = material::BuiltinTexture::White;
		bool has_filter					 = false;
		bool has_wrap					 = false;
		gpu::Filter filter				 = gpu::Filter::Linear;
		gpu::AddressMode wrap			 = gpu::AddressMode::Repeat;

		/// The texture's state when it was last written, so a refresh writes only what has arrived since.
		AssetState written = AssetState::Loading;
	};

	/**
	 * A material from a .material file: a type, and the values it sets.
	 *
	 *   {
	 *       "type": "ember/materials/sprite.slang",
	 *       "values": {
	 *           "albedo": "characters/hero.png",
	 *           "tint":   [1.0, 0.96, 0.9, 1.0],
	 *           "cutoff": 0.4,
	 *           "mask":   {"texture": "characters/hero_mask.png", "filter": "linear", "wrap": "clamp"}
	 *       }
	 *   }
	 *
	 * Numbers are JSON numbers, arrays of up to four, or booleans. Textures are paths, a builtin's
	 * name (white, black, flat, error), or an object that picks the sampler as well. Colours are
	 * written as a picker gives them, in sRGB. People write these files, so comments and trailing
	 * commas are allowed.
	 *
	 * The handle never changes. The loader reads the file and requests the type and the textures,
	 * and waits for none of them. The pump publishes the material once its type has settled: of
	 * that type if it built, of the error type if it failed. Textures bind as they arrive, each
	 * parameter showing its default until then and the error texture if its file will not load. A
	 * save re-reads the file into the same material, and a type or texture that reloads reaches it
	 * the same way.
	 */
	struct MaterialAsset
	{
		render::MaterialHandle material = {};

		String path{&memory::heap(MemoryTag::Assets)}; // for messages: the file the values came from
		AssetRef<MaterialTypeAsset> type;			   // the type the file names
		Vector<MaterialValue> values{&memory::heap(MemoryTag::Assets)};

		// Owner thread: what the material is bound to. The type drawing now stays loaded until the one
		// the file names takes over, so a save that names another type never flashes the error type.
		AssetRef<MaterialTypeAsset> drawing;
		u32 generation = 0; // of the type drawing, when the values were written through it

		static bool load(AssetLoad& load, MaterialAsset& out) noexcept;
		static bool publish(AssetServices& services, MaterialAsset& asset) noexcept;
		static void refresh(AssetServices& services, MaterialAsset& asset) noexcept;
		static void reload(AssetServices& services, MaterialAsset& live, MaterialAsset& fresh) noexcept;
		static void unload(AssetServices& services, MaterialAsset& asset) noexcept;
	};

	static_assert(AssetType<MaterialAsset>);

	struct MaterialAssetsDef
	{
		/**
		 * Searched for what a type imports, after the type's own directory and the asset root: the
		 * game's shader libraries. Relative to the working directory.
		 */
		Span<const char* const> include_dirs = {};

		/// The shading model of surface types that name none.
		const char* default_shading = "lit";

		/// The engine's shaders. Null is the directory this build was configured with.
		const char* engine_dir = nullptr;

		/// Read cooked pairs even though the compiler is here: a dev build running as a ship build does.
		bool cooked = false;
	};

	/**
	 * What the two material asset types share, and the runtime owns one of: the registry they fill,
	 * and in builds with the compiler, the compiler and the engine's shaders served under
	 * ENGINE_SHADER_MOUNT. The compiler starts on the first compile, on the IO thread, so a boot pays
	 * for it only when a type is built from source, and never on the owner thread.
	 */
	class MaterialAssets final
	{
	public:
		MaterialAssets() noexcept;
		~MaterialAssets() noexcept;

		MaterialAssets(const MaterialAssets&)			 = delete;
		MaterialAssets& operator=(const MaterialAssets&) = delete;

		/**
		 * Registers both types with `assets`, and in a build that compiles, mounts the engine's shaders:
		 * before the manager's first load. While the manager watches, it also loads the stock types'
		 * sources, so a save to one, or to an engine module they link, reaches the stock type in place.
		 * The registry and the manager must outlive every material asset.
		 */
		void init(AssetManager& assets, render::MaterialRegistry& registry, const MaterialAssetsDef& def = {}) noexcept;

		/// Lets go of the stock sources. Before the asset manager shuts down, which unloads the rest.
		void shutdown() noexcept;

		[[nodiscard]] render::MaterialRegistry& registry() const noexcept { return *m_registry; }

		/// The stock type a path names, the same in every build; null for any other path.
		[[nodiscard]] render::MaterialTypeHandle stock_type(StringView path) const noexcept;

		/// True when types build from their sources; false in ship builds, and in any asked to read cooked pairs.
		[[nodiscard]] bool compiles() const noexcept { return !m_cooked; }

		/// True when stock types are built from their sources, which only hot reload needs.
		[[nodiscard]] bool builds_stock() const noexcept { return m_build_stock; }

		/**
		 * Builds a type from the file behind its asset: compiles it, or reads the cooked pair beside
		 * it. On the IO thread, one build at a time. False with the reasons in `problems`; the files a
		 * build read are in `out.dependencies` either way.
		 */
		[[nodiscard]] bool build(StringView file, material::Type& out, String& problems) noexcept;

	private:
		render::MaterialRegistry* m_registry = nullptr;
		AssetRef<MaterialTypeAsset> m_stock_sources[static_cast<size_t>(render::StockType::Count)];
		bool m_cooked	   = true;
		bool m_build_stock = false;

		// Builds with the compiler only. The compiler is made at init and started by the first build.
		shader::Compiler* m_compiler = nullptr;
		std::once_flag m_start;
		bool m_started = false;
		String m_engine_dir{&memory::heap(MemoryTag::Assets)};
		String m_default_shading{&memory::heap(MemoryTag::Assets)};
		Vector<String> m_include_dirs{&memory::heap(MemoryTag::Assets)};
	};
}
