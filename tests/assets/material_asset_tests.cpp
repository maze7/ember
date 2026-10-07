#include <ember/assets/material_asset.h>
#include <ember/assets/pack_source.h>
#include <ember/core/filesystem.h>
#include <ember/gpu/device.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>
#include <ember/render/material_registry.h>
#include <ember/render/scene.h>
#include <ember/shader/compiler.h>

#include <glm/vec4.hpp>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>

#if defined(EMBER_PLATFORM_WINDOWS)
	#include <process.h>
#else
	#include <unistd.h>
#endif

namespace
{
	using namespace ember;
	using render::MaterialRegistry;
	using render::MaterialTypeHandle;
	using render::StockType;

	[[nodiscard]] int process_id() noexcept
	{
#if defined(EMBER_PLATFORM_WINDOWS)
		return _getpid();
#else
		return getpid();
#endif
	}

	/// A type to load: a tinted texture scaled by a strength, which gives the tests a colour, a
	/// number and a texture to find in the record, and an engine helper to be rebuilt through.
	constexpr std::string_view GLOW = R"(import material;

struct Glow : IMaterial
{
	[Color] [Default("1 1 1 1")] float4 tint;
	[Default("1")] float strength;
	Texture2DRef albedo;

	void surface(SurfaceInput s, inout Surface out)
	{
		let texel  = albedo.sample(s.uv).rgb * tint.rgb * strength;
		out.albedo = texel * luminance(texel);
	}
};
)";

	/// A 4 by 4 RGBA PNG.
	constexpr u8 SMALL_PNG[] = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00,
		0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x06, 0x00, 0x00, 0x00, 0xa9, 0xf1, 0x9e, 0x7e, 0x00,
		0x00, 0x00, 0x2b, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x15, 0xc8, 0x31, 0x01, 0x00, 0x30, 0x0c, 0xc3,
		0xb0, 0x00, 0x2b, 0x30, 0x9f, 0x05, 0x15, 0x7e, 0x9b, 0x7b, 0xe8, 0x51, 0x92, 0x7d, 0x23, 0x54, 0x25,
		0x63, 0x08, 0x75, 0x2e, 0x30, 0x84, 0xca, 0x45, 0x0d, 0xa1, 0xea, 0x03, 0x39, 0xc8, 0x23, 0x31, 0x35,
		0xad, 0xbf, 0x59, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
	};

	/**
	 * What the runtime wires up, without a window: a registry on a real device, an asset manager over
	 * a scratch root, and the material library between them. The watch is off, so the tests say when
	 * a file changed, and the engine's shaders are a scratch copy, so a test can edit one.
	 */
	class MaterialAssetTest : public testing::Test
	{
	protected:
		virtual bool watching() const { return false; }
		virtual bool cooked() const { return false; }

		void SetUp() override
		{
			if (!m_device)
				GTEST_SKIP() << "no Vulkan adapter";

			String scratch(&memory::heap(MemoryTag::Engine));
			ASSERT_TRUE(fs::temporary_directory(scratch).has_value());
			ASSERT_TRUE(
				fs::join(scratch, scratch, "ember_material_asset_tests_" + std::to_string(process_id())).has_value());
			m_scratch.assign(scratch.data(), scratch.size());
			m_root	 = m_scratch + "/assets";
			m_engine = m_scratch + "/shaders";

			if (const auto removed = fs::remove_tree(m_scratch); !removed)
			{
				ASSERT_EQ(removed.error().code, fs::FileErrorCode::NotFound);
			}

			ASSERT_TRUE(fs::create_directories(m_root + "/materials").has_value());
			ASSERT_TRUE(fs::create_directories(m_root + "/textures").has_value());
			copy_engine_shaders();

			m_errors = gpu::Device::validation_error_count();
			jobs::initialize({.worker_count = 4});

			m_registry.init(m_device, {.max_types = 16, .max_materials = 64, .initial_records = 4});
			m_assets.init(m_device, {.root			  = m_root.c_str(),
									 .max_assets	  = 64,
									 .hot_reload	  = watching(),
									 .reload_delay_ms = 0,
									 .record_changes  = true});
			m_materials.init(m_assets, m_registry, {.engine_dir = m_engine.c_str(), .cooked = cooked()});

			// A ship build reads cooked pairs only: the tests that compile have nothing to test there.
			if (!cooked() && !m_materials.compiles())
				GTEST_SKIP() << "built without the shader compiler";
		}

		void TearDown() override
		{
			if (!m_device)
				return;

			m_materials.shutdown();
			m_assets.shutdown();
			m_registry.shutdown(m_device);
			m_device.wait_idle();
			jobs::shutdown();

			(void)fs::remove_tree(m_scratch);
			EXPECT_EQ(gpu::Device::validation_error_count() - m_errors, 0u);
		}

		/// The engine's shaders, copied so a test can change one the way an engine programmer would.
		void copy_engine_shaders()
		{
			const std::string from = shader::Compiler::configured_engine_dir();

			for (const char* dir : {"", "/materials"})
			{
				ASSERT_TRUE(fs::create_directories(m_engine + dir).has_value());

				const auto listed =
					fs::enumerate(from + dir,
								  [&](const fs::DirectoryEntry& entry) noexcept
								  {
									  if (entry.type == fs::FileType::Regular)
										  (void)fs::copy_file(from + dir + "/" + std::string(entry.name),
															  m_engine + dir + "/" + std::string(entry.name));
									  return fs::Visit::Continue;
								  });

				ASSERT_TRUE(listed.has_value());
			}
		}

		void write(const std::string& path, std::string_view text)
		{
			ASSERT_TRUE(fs::write_file(path, {reinterpret_cast<const u8*>(text.data()), text.size()}).has_value());
		}

		void write_asset(const char* name, std::string_view text) { write(m_root + "/" + name, text); }

		/// An edit to a copied engine shader: `from` replaced by `to`, which must be there.
		void edit_engine(const char* name, std::string_view from, std::string_view to)
		{
			const auto file = fs::read_file(m_engine + "/" + name, memory::heap(MemoryTag::Engine));
			ASSERT_TRUE(file.has_value());

			std::string text(file->text());
			const size_t at = text.find(from);
			ASSERT_NE(at, std::string::npos) << name << " has no " << from;

			text.replace(at, from.size(), to);
			write(m_engine + "/" + name, text);
		}

		/// The frame loop's view of a save: the change, the pump that queues its reloads, the loaders,
		/// and the pump that folds what they made into the live payloads and refreshes what used them.
		void reload_now(const char* name)
		{
			m_assets.notify_changed(asset_id(name));
			m_assets.pump(++m_frame);
			m_assets.wait_idle();
			m_assets.pump(++m_frame);
		}

		template <class T> [[nodiscard]] T read(render::MaterialHandle material, const char* param)
		{
			const material::Type* type = m_registry.type(m_registry.type_of(material));
			const material::Param* at  = type != nullptr ? material::find_param(type->record, param) : nullptr;

			T value{};
			if (at != nullptr)
				std::memcpy(&value, m_registry.record(material).data() + at->offset, sizeof(T));

			return value;
		}

		/// The texture a record's texture parameter names, by its bindless index.
		[[nodiscard]] u32 texture_index(render::MaterialHandle material, const char* param)
		{
			return read<u32>(material, param);
		}

		std::string m_scratch;
		std::string m_root;
		std::string m_engine;
		u64 m_frame	 = 0;
		u32 m_errors = 0;

		gpu::Device m_device{gpu::DeviceDef{.enable_validation = true, .adapter = gpu::AdapterPreference::Any}};
		MaterialRegistry m_registry;
		AssetManager m_assets;
		MaterialAssets m_materials;
	};

	using MaterialTypeAssets = MaterialAssetTest;

	TEST_F(MaterialTypeAssets, ATypeBuildsFromItsSourceAndKeepsItsHandleAcrossSaves)
	{
		write_asset("materials/glow.slang", GLOW);

		const AssetRef<MaterialTypeAsset> glow = m_assets.load<MaterialTypeAsset>("materials/glow.slang");
		glow.wait();

		ASSERT_TRUE(glow);
		const MaterialTypeHandle handle = glow->type;
		ASSERT_NE(m_registry.type(handle), nullptr);
		EXPECT_EQ(m_registry.type(handle)->name, "Glow");

		const u32 generation = m_registry.generation(handle);

		// The save adds a parameter. The handle stays; the build behind it moves on.
		std::string saved(GLOW);
		saved.insert(saved.find("\tTexture2DRef"), "\t[Default(\"0.5\")] float pulse;\n");
		write_asset("materials/glow.slang", saved);
		reload_now("materials/glow.slang");

		EXPECT_EQ(glow->type, handle);
		EXPECT_EQ(m_registry.generation(handle), generation + 1);
		EXPECT_NE(material::find_param(m_registry.type(handle)->record, "pulse"), nullptr);
	}

	TEST_F(MaterialTypeAssets, ACompileWritesTheTypesPairBesideItsSource)
	{
		write_asset("materials/glow.slang", GLOW);

		const AssetRef<MaterialTypeAsset> glow = m_assets.load<MaterialTypeAsset>("materials/glow.slang");
		glow.wait();
		ASSERT_TRUE(glow);

		// What a cooked build, or another machine, reads: the pair, and it is the type that was built.
		const auto type_file = fs::read_file(m_root + "/materials/glow.type", memory::heap(MemoryTag::Engine));
		const auto spirv	 = fs::read_file(m_root + "/materials/glow.spv", memory::heap(MemoryTag::Engine));
		ASSERT_TRUE(type_file.has_value());
		ASSERT_TRUE(spirv.has_value());

		material::Type cooked;
		String error;
		ASSERT_TRUE(material::read_cooked(type_file->text(), spirv->bytes(), cooked, error)) << error;
		EXPECT_EQ(cooked.name, "Glow");
		EXPECT_EQ(cooked.hash, m_registry.type(glow->type)->hash);

		// The cook announced what it wrote, so a host sends the pair on without waiting on the watcher.
		Vector<AssetChange> changes(&memory::heap(MemoryTag::Engine));
		m_assets.pump(++m_frame);
		m_assets.take_changed(changes);
		std::vector<std::string> announced;
		for (const AssetChange& change : changes)
			announced.emplace_back(change.name);
		std::sort(announced.begin(), announced.end());
		ASSERT_EQ(announced.size(), 2u);
		EXPECT_EQ(announced[0], "materials/glow.spv");
		EXPECT_EQ(announced[1], "materials/glow.type");

		// A save that changes nothing observable leaves the pair as it was: nothing to send on.
		std::string commented(GLOW);
		commented.insert(0, "// a comment\n");
		write_asset("materials/glow.slang", commented);
		reload_now("materials/glow.slang");

		const auto type_again  = fs::read_file(m_root + "/materials/glow.type", memory::heap(MemoryTag::Engine));
		const auto spirv_again = fs::read_file(m_root + "/materials/glow.spv", memory::heap(MemoryTag::Engine));
		ASSERT_TRUE(type_again.has_value() && spirv_again.has_value());
		EXPECT_EQ(type_again->text(), type_file->text());
		EXPECT_TRUE(std::equal(spirv->bytes().begin(), spirv->bytes().end(), spirv_again->bytes().begin(),
							   spirv_again->bytes().end()));

		// And nothing was announced for it: an unchanged pair is nothing to send.
		m_assets.take_changed(changes);
		EXPECT_TRUE(changes.empty());
	}

	TEST_F(MaterialTypeAssets, ABrokenSaveKeepsTheLastGoodBuildDrawing)
	{
		write_asset("materials/glow.slang", GLOW);

		const AssetRef<MaterialTypeAsset> glow = m_assets.load<MaterialTypeAsset>("materials/glow.slang");
		glow.wait();
		ASSERT_TRUE(glow);

		const u32 generation = m_registry.generation(glow->type);

		write_asset("materials/glow.slang", "import material;\nstruct Glow : IMaterial { oops };\n");
		reload_now("materials/glow.slang");

		EXPECT_EQ(glow.state(), AssetState::Loaded);
		EXPECT_EQ(m_registry.generation(glow->type), generation);
		EXPECT_EQ(m_registry.type(glow->type)->name, "Glow");
	}

	TEST_F(MaterialTypeAssets, ALibraryATypeImportsRebuildsItWhenSaved)
	{
		write_asset("materials/tone.slang", "public float3 tone(float3 c) { return c * 0.5; }\n");

		std::string toned(GLOW);
		toned.insert(toned.find("\nstruct"), "\nimport tone;\n");
		toned.replace(toned.find("texel * luminance(texel)"), 24, "tone(texel)");
		write_asset("materials/glow.slang", toned);

		const AssetRef<MaterialTypeAsset> glow = m_assets.load<MaterialTypeAsset>("materials/glow.slang");
		glow.wait();
		ASSERT_TRUE(glow);

		const u32 generation = m_registry.generation(glow->type);

		// The library is no asset; the type said it was made from it.
		write_asset("materials/tone.slang", "public float3 tone(float3 c) { return c * 0.25; }\n");
		reload_now("materials/tone.slang");

		EXPECT_EQ(m_registry.generation(glow->type), generation + 1);
	}

	TEST_F(MaterialTypeAssets, AnEngineModuleEditRebuildsTheTypesLinkedAgainstIt)
	{
		write_asset("materials/glow.slang", GLOW);

		const AssetRef<MaterialTypeAsset> glow = m_assets.load<MaterialTypeAsset>("materials/glow.slang");
		glow.wait();
		ASSERT_TRUE(glow);

		const u32 generation = m_registry.generation(glow->type);

		// The engine's shaders are served as ember/, so the edit is named the way the watch names it.
		edit_engine("material.slang", "float3(0.2126, 0.7152, 0.0722)", "float3(0.3, 0.6, 0.1)");
		reload_now("ember/material.slang");

		EXPECT_EQ(m_registry.generation(glow->type), generation + 1);
	}

	using MaterialFiles = MaterialAssetTest;

	TEST_F(MaterialFiles, AMaterialBindsItsTypeTexturesAndValuesWhateverOrderTheyLoadIn)
	{
		write_asset("materials/glow.slang", GLOW);
		write(m_root + "/textures/a.png",
			  std::string_view(reinterpret_cast<const char*>(SMALL_PNG), sizeof(SMALL_PNG)));
		write_asset("materials/hero.material", R"({
			// People write these: comments and trailing commas are fine.
			"type": "materials/glow.slang",
			"values": {
				"tint": [1.0, 0.5, 0.25, 1.0],
				"strength": 2,
				"albedo": "textures/a.png",
			},
		})");

		const AssetRef<MaterialAsset> hero = m_assets.load<MaterialAsset>("materials/hero.material");
		m_assets.wait_idle();

		ASSERT_TRUE(hero);
		ASSERT_TRUE(hero->type);

		const AssetRef<TextureAsset> texture = m_assets.load<TextureAsset>("textures/a.png");
		ASSERT_TRUE(texture);

		EXPECT_EQ(m_registry.type_of(hero->material), hero->type->type);
		EXPECT_EQ(read<f32>(hero->material, "strength"), 2.0f);
		EXPECT_EQ(read<glm::vec4>(hero->material, "tint").x, 1.0f);
		EXPECT_NEAR(read<glm::vec4>(hero->material, "tint").y, 0.2140f, 1e-4f); // sRGB 0.5, linear
		EXPECT_EQ(texture_index(hero->material, "albedo"), bindless_index(texture->texture));
	}

	TEST_F(MaterialFiles, SavingTheFileRebindsTheSameMaterialToWhatItNowSays)
	{
		write_asset("materials/glow.slang", GLOW);
		write_asset("materials/hero.material",
					R"({"type": "materials/glow.slang", "values": {"strength": 2, "tint": [0, 0, 0, 1]}})");

		const AssetRef<MaterialAsset> hero = m_assets.load<MaterialAsset>("materials/hero.material");
		hero.wait();
		ASSERT_TRUE(hero);

		const render::MaterialHandle handle = hero->material;

		// The tint is gone from the file, so it is back to its default; the strength is new.
		write_asset("materials/hero.material", R"({"type": "materials/glow.slang", "values": {"strength": 3}})");
		reload_now("materials/hero.material");

		EXPECT_EQ(hero->material, handle);
		EXPECT_EQ(read<f32>(hero->material, "strength"), 3.0f);
		EXPECT_EQ(read<glm::vec4>(hero->material, "tint"), glm::vec4(1.0f));
	}

	TEST_F(MaterialFiles, ATextureThatWillNotLoadShowsTheErrorTextureUntilItDoes)
	{
		write_asset("materials/glow.slang", GLOW);
		write_asset("materials/hero.material",
					R"({"type": "materials/glow.slang", "values": {"albedo": "textures/late.png"}})");

		const AssetRef<MaterialAsset> hero = m_assets.load<MaterialAsset>("materials/hero.material");
		hero.wait();
		ASSERT_TRUE(hero);

		EXPECT_EQ(texture_index(hero->material, "albedo"),
				  bindless_index(m_registry.builtin(material::BuiltinTexture::Error)));

		// The file appears: the texture loads, and the material hears it at the pump that follows.
		write(m_root + "/textures/late.png",
			  std::string_view(reinterpret_cast<const char*>(SMALL_PNG), sizeof(SMALL_PNG)));
		reload_now("textures/late.png");

		const AssetRef<TextureAsset> texture = m_assets.load<TextureAsset>("textures/late.png");
		ASSERT_TRUE(texture);
		EXPECT_EQ(texture_index(hero->material, "albedo"), bindless_index(texture->texture));
	}

	TEST_F(MaterialFiles, AMaterialOfATypeThatFailsDrawsTheErrorTypeUntilASaveFixesIt)
	{
		write_asset("materials/glow.slang", "import material;\nstruct Glow : IMaterial { oops };\n");
		write_asset("materials/hero.material", R"({"type": "materials/glow.slang", "values": {"strength": 2}})");

		const AssetRef<MaterialAsset> hero = m_assets.load<MaterialAsset>("materials/hero.material");
		hero.wait();

		ASSERT_TRUE(hero);
		EXPECT_EQ(hero->type.state(), AssetState::Failed);
		EXPECT_EQ(m_registry.type_of(hero->material), m_registry.error_type());
		EXPECT_EQ(render::key_bucket(m_registry.key(hero->material)), m_registry.error_type().index);

		// Fixed: the type publishes for the first time and the material moves onto it, values and all.
		write_asset("materials/glow.slang", GLOW);
		reload_now("materials/glow.slang");

		ASSERT_TRUE(hero->type);
		EXPECT_EQ(m_registry.type_of(hero->material), hero->type->type);
		EXPECT_EQ(read<f32>(hero->material, "strength"), 2.0f);
	}

	TEST_F(MaterialFiles, AnObjectMadeWhileItsTypeWasBrokenTakesTheTypesDefaultsOnceItIsFixed)
	{
		write_asset("materials/card.slang", "import material;\nstruct Card : IMaterial { oops };\n");
		write_asset("materials/card.material", R"({"type": "materials/card.slang"})");

		const AssetRef<MaterialAsset> card = m_assets.load<MaterialAsset>("materials/card.material");
		card.wait();
		ASSERT_TRUE(card);

		// The game makes its object while the type is broken: the error type has no per-object data.
		render::RenderScene scene;
		scene.init(4, &m_registry);
		const render::RenderObjectHandle object = scene.create_object({.material = card->material});

		write_asset("materials/card.slang", R"(import material;

struct Card : IMaterial
{
	Texture2DRef albedo;

	struct Instance
	{
		[Default("0 0 1 1")] float4 frame;
	};

	void surface(SurfaceInput s, inout Surface out)
	{
		let data   = instance<Instance>(s);
		out.albedo = albedo.sample(lerp(data.frame.xy, data.frame.zw, s.uv)).rgb;
	}
};
)");
		reload_now("materials/card.slang");

		// What the renderer does each frame: the material moved type, so its object takes the defaults.
		scene.reseed(m_registry.reseeds());
		m_registry.clear_reseeds();

		glm::vec4 frame(-1.0f);
		std::memcpy(&frame, scene.instance(object.index).bytes, sizeof(frame));
		EXPECT_EQ(frame, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));

		scene.destroy_object(object);
	}

	TEST_F(MaterialFiles, NamingAnotherTypeMovesTheMaterialOnceThatTypeIsHere)
	{
		write_asset("materials/glow.slang", GLOW);
		write_asset("materials/hero.material", R"({"type": "ember/materials/unlit.slang"})");

		const AssetRef<MaterialAsset> hero = m_assets.load<MaterialAsset>("materials/hero.material");
		hero.wait();
		ASSERT_TRUE(hero);

		const render::MaterialHandle handle = hero->material;
		EXPECT_EQ(m_registry.type_of(handle), m_registry.stock_type(StockType::Unlit));

		write_asset("materials/hero.material", R"({"type": "materials/glow.slang", "values": {"strength": 4}})");
		reload_now("materials/hero.material");

		ASSERT_TRUE(hero->type);
		EXPECT_EQ(hero->material, handle);
		EXPECT_EQ(m_registry.type_of(handle), hero->type->type);
		EXPECT_EQ(read<f32>(handle, "strength"), 4.0f);
	}

	TEST_F(MaterialFiles, AValueNoParameterCouldHoldFailsTheSaveAndKeepsTheMaterial)
	{
		write_asset("materials/glow.slang", GLOW);
		write_asset("materials/hero.material", R"({"type": "materials/glow.slang", "values": {"strength": 2}})");

		const AssetRef<MaterialAsset> hero = m_assets.load<MaterialAsset>("materials/hero.material");
		hero.wait();
		ASSERT_TRUE(hero);

		write_asset("materials/hero.material",
					R"({"type": "materials/glow.slang", "values": {"strength": [1, 2, 3, 4, 5]}})");
		reload_now("materials/hero.material");

		EXPECT_EQ(read<f32>(hero->material, "strength"), 2.0f);
	}

	/// The same, reading the pairs ember_cook writes instead of compiling: how a ship build loads.
	class CookedMaterialFiles : public MaterialAssetTest
	{
	protected:
		bool cooked() const override { return true; }

		/// Cooks `source` beside `name`, as ember_cook would: the .type file and the SPIR-V.
		void cook(const char* name, std::string_view source)
		{
			shader::Compiler compiler;
			ASSERT_TRUE(compiler.initialize({.engine_dir = m_engine.c_str()}));

			const std::string path = m_root + "/" + name;
			material::Type type;
			String diagnostics;
			ASSERT_TRUE(compiler.compile_material(path, source, type, diagnostics)) << diagnostics;

			String type_file;
			ASSERT_TRUE(material::write_type(type, type_file));

			const std::string stem = path.substr(0, path.rfind('.'));
			write(stem + material::TYPE_EXTENSION, type_file);
			write(stem + material::SPIRV_EXTENSION,
				  std::string_view(reinterpret_cast<const char*>(type.spirv.data()), type.spirv.size() * sizeof(u32)));
		}
	};

	TEST_F(CookedMaterialFiles, ACookedPairStandsInForItsSource)
	{
		cook("materials/glow.slang", GLOW); // and no source beside it
		write_asset("materials/hero.material", R"({"type": "materials/glow.slang", "values": {"strength": 2}})");

		const AssetRef<MaterialAsset> hero = m_assets.load<MaterialAsset>("materials/hero.material");
		hero.wait();

		ASSERT_TRUE(hero);
		ASSERT_TRUE(hero->type);
		EXPECT_EQ(m_registry.type(hero->type->type)->name, "Glow");
		EXPECT_EQ(read<f32>(hero->material, "strength"), 2.0f);

		// A cook that rewrites the pair reloads the type, when something is watching.
		const u32 generation = m_registry.generation(hero->type->type);

		std::string recooked(GLOW);
		recooked.replace(recooked.find("[Default(\"1\")]"), 14, "[Default(\"3\")]");
		cook("materials/glow.slang", recooked);
		reload_now("materials/glow.type");

		EXPECT_EQ(m_registry.generation(hero->type->type), generation + 1);
	}

	TEST_F(CookedMaterialFiles, AMaterialNamesAStockTypeByItsEnginePathInEveryBuild)
	{
		write_asset("materials/leaf.material",
					R"({"type": "ember/materials/sprite.slang", "values": {"cutoff": 0.25, "albedo": "flat"}})");

		const AssetRef<MaterialAsset> leaf = m_assets.load<MaterialAsset>("materials/leaf.material");
		leaf.wait();

		ASSERT_TRUE(leaf);
		EXPECT_EQ(m_registry.type_of(leaf->material), m_registry.stock_type(StockType::Sprite));
		EXPECT_EQ(read<f32>(leaf->material, "cutoff"), 0.25f);
		EXPECT_EQ(texture_index(leaf->material, "albedo"),
				  bindless_index(m_registry.builtin(material::BuiltinTexture::Flat)));
	}

	/// A cooked build whose game content is one pack, as a shipped build's might be.
	class PackedMaterialFiles : public CookedMaterialFiles
	{
	protected:
		PackSource m_pack; // outlives the manager, which TearDown shuts down first
	};

	TEST_F(PackedMaterialFiles, ACookedTypeAndItsMaterialLoadFromAPack)
	{
		// Cooked as loose files first, then packed, and the loose files taken away: only the pack has them.
		cook("materials/glow.slang", GLOW);

		const auto type_file = fs::read_file(m_root + "/materials/glow.type", memory::heap(MemoryTag::Engine));
		const auto spirv	 = fs::read_file(m_root + "/materials/glow.spv", memory::heap(MemoryTag::Engine));
		ASSERT_TRUE(type_file.has_value() && spirv.has_value());

		const std::string_view hero =
			R"({"type": "pack/materials/glow.slang", "values": {"strength": 4, "albedo": "white"}})";
		const PackEntry entries[] = {
			{"materials/glow.type", type_file->bytes()},
			{"materials/glow.spv", spirv->bytes()},
			{"materials/hero.material", {reinterpret_cast<const u8*>(hero.data()), hero.size()}},
		};

		const std::string pack_path = m_scratch + "/game.pack";
		ASSERT_TRUE(PackSource::write(pack_path, entries).has_value());
		ASSERT_TRUE(fs::remove_file(m_root + "/materials/glow.type").has_value());
		ASSERT_TRUE(fs::remove_file(m_root + "/materials/glow.spv").has_value());

		ASSERT_TRUE(m_pack.open(pack_path).has_value());
		m_assets.mount("pack", m_pack);

		const AssetRef<MaterialAsset> loaded = m_assets.load<MaterialAsset>("pack/materials/hero.material");
		loaded.wait();

		ASSERT_TRUE(loaded);
		ASSERT_TRUE(loaded->type);
		EXPECT_EQ(m_registry.type(loaded->type->type)->name, "Glow");
		EXPECT_EQ(read<f32>(loaded->material, "strength"), 4.0f);
	}

	/// With the watch on, the stock types' sources are loaded too, so an edit reaches the stock type.
	class WatchedMaterialAssets : public MaterialAssetTest
	{
	protected:
		bool watching() const override { return true; }
	};

	TEST_F(WatchedMaterialAssets, AnEditToAStockTypesSourceReachesTheStockTypeInPlace)
	{
		ASSERT_TRUE(m_materials.builds_stock());
		m_assets.wait_idle();

		const MaterialTypeHandle unlit = m_registry.stock_type(StockType::Unlit);
		const u32 generation		   = m_registry.generation(unlit);

		// The first build of the source is the embedded one, so it changed nothing.
		EXPECT_EQ(generation, 1u);

		edit_engine("materials/unlit.slang", "[Default(\"1 1 1 1\")]", "[Default(\"1 0 0 1\")]");
		reload_now("ember/materials/unlit.slang");

		EXPECT_EQ(m_registry.stock_type(StockType::Unlit), unlit);
		EXPECT_EQ(m_registry.generation(unlit), generation + 1);
	}
}
