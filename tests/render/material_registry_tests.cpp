#include <ember/gpu/device.h>
#include <ember/memory/memory.h>
#include <ember/memory/pmr/arena.h>
#include <ember/render/material_registry.h>
#include <ember/render/scene.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace
{
	using namespace ember;
	using namespace ember::render;
	using material::BuiltinTexture;
	using material::Domain;
	using material::ParamKind;

	struct KeyRun
	{
		u16 bucket = 0;
		u16 first  = 0;
		u32 count  = 0;

		bool operator==(const KeyRun&) const = default;
	};

	[[nodiscard]] std::vector<KeyRun> runs(std::initializer_list<u32> keys)
	{
		const std::vector<u32> sorted(keys);
		std::vector<KeyRun> out;

		for_each_key_run({sorted.data(), sorted.size()},
						 [&](u16 bucket, u16 first, u32 count) { out.push_back({bucket, first, count}); });

		return out;
	}

	TEST(MaterialKeys, SplitIntoBucketAndSlot)
	{
		const u32 key = material_key(3, 70);
		EXPECT_EQ(key_bucket(key), 3u);
		EXPECT_EQ(key_slot(key), 70u);
		EXPECT_EQ(material_key(0, 0), ERROR_MATERIAL_KEY);
	}

	TEST(MaterialKeys, RunsMergeConsecutiveSlotsOfOneBucket)
	{
		EXPECT_EQ(runs({material_key(1, 4), material_key(1, 5), material_key(1, 6), material_key(1, 9)}),
				  (std::vector<KeyRun>{{1, 4, 3}, {1, 9, 1}}));
	}

	TEST(MaterialKeys, RunsNeverCrossIntoTheNextBucket)
	{
		// The last slot of bucket 1 and the first of bucket 2 are consecutive integers.
		EXPECT_EQ(runs({material_key(1, 0xFFFF), material_key(2, 0), material_key(2, 1)}),
				  (std::vector<KeyRun>{{1, 0xFFFF, 1}, {2, 0, 2}}));
	}

	material::Param number(const char* name, ParamKind kind, u8 components, u32 offset, const char* preset = "")
	{
		material::Param param;
		param.name		 = name;
		param.kind		 = kind;
		param.components = components;
		param.offset	 = offset;
		param.size		 = 4u * components;
		param.preset	 = preset;
		return param;
	}

	material::Param color(const char* name, u8 components, u32 offset, const char* preset = "", bool hdr = false)
	{
		material::Param param = number(name, ParamKind::Float, components, offset, preset);
		param.color			  = true;
		param.hdr			  = hdr;
		return param;
	}

	material::Param texture(const char* name, u32 offset, const char* preset = "",
							gpu::Filter filter = gpu::Filter::Linear, gpu::AddressMode wrap = gpu::AddressMode::Repeat)
	{
		material::Param param;
		param.name	 = name;
		param.kind	 = ParamKind::Texture2D;
		param.offset = offset;
		param.size	 = material::TEXTURE_REF_BYTES;
		param.preset = preset;
		param.filter = filter;
		param.wrap	 = wrap;
		return param;
	}

	/// A type as the compiler would reflect one, built by hand so every offset is chosen. The
	/// registry never reads the bytecode, so a header's worth of words stands in for it.
	material::Type make_type(const char* name, std::vector<material::Param> params, u32 size,
							 Domain domain = Domain::Surface)
	{
		material::Type type;
		type.domain		   = domain;
		type.name		   = name;
		type.spirv		   = {0x07230203u, 0x00010600u, 0u, 1u, 0u};
		type.record.params = {params.begin(), params.end()};
		type.record.size   = size;
		type.hash		   = material::hash_type(type);
		return type;
	}

	template <class T> T read(Span<const u8> record, u32 offset)
	{
		T value{};
		EXPECT_LE(offset + sizeof(T), record.size());
		std::memcpy(&value, record.data() + offset, sizeof(T));
		return value;
	}

	/// The same type with per-object data of its own: an Instance laid out as `params`.
	material::Type with_instance(material::Type type, std::vector<material::Param> params, u32 size)
	{
		type.instance.params = {params.begin(), params.end()};
		type.instance.size	 = size;
		type.hash			 = material::hash_type(type);
		return type;
	}

	/// sRGB 0.5, decoded: the value a mid-grey picked in an editor becomes in a record.
	constexpr f32 LINEAR_HALF = 0.2140411f;

	/**
	 * A registry on a real device with validation on, small enough that tables grow within a test.
	 * Every material a test makes is destroyed at teardown, as the registry asks of its users, and a
	 * test passes only if the layers saw nothing wrong.
	 */
	class Materials : public testing::Test
	{
	protected:
		void SetUp() override
		{
			if (!m_device)
				GTEST_SKIP() << "no Vulkan adapter";

			m_errors = gpu::Device::validation_error_count();
			m_registry.init(m_device, {.max_types = 8, .max_materials = 64, .initial_records = 2});

			m_scratch.init(memory::tagged_heap(), "materials.test");
			m_scratch.begin(heap_tag(MemoryLifetime::RenderScratch, 1));
		}

		void TearDown() override
		{
			if (!m_device)
				return;

			for (MaterialHandle material : m_made)
				m_registry.destroy(material);

			sync();
			m_registry.shutdown(m_device);
			m_device.wait_idle();

			memory::tagged_heap().free(m_scratch.end());
			m_scratch.shutdown();

			EXPECT_EQ(gpu::Device::validation_error_count() - m_errors, 0u);
		}

		MaterialHandle make(MaterialTypeHandle type)
		{
			const MaterialHandle material = m_registry.create(type);
			EXPECT_FALSE(material.is_null());
			m_made.push_back(material);
			return material;
		}

		/// One frame with nothing in it but the registry's uploads.
		void sync()
		{
			(void)m_device.begin_frame();
			m_registry.sync(m_device, m_scratch);
			(void)m_device.end_frame();
		}

		[[nodiscard]] u32 index_of(BuiltinTexture texture) const { return bindless_index(m_registry.builtin(texture)); }

		[[nodiscard]] u32 index_of(gpu::Filter filter, gpu::AddressMode wrap) const
		{
			return bindless_index(m_registry.sampler(filter, wrap));
		}

		gpu::Device m_device{gpu::DeviceDef{.enable_validation = true, .adapter = gpu::AdapterPreference::Any}};
		MaterialRegistry m_registry;
		Arena m_scratch;
		std::vector<MaterialHandle> m_made;
		u32 m_errors = 0;
	};
}

TEST_F(Materials, TheErrorMaterialIsRowZero)
{
	const material::Type* error = m_registry.type(m_registry.error_type());
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(error->name, "Error");

	EXPECT_EQ(m_registry.error_material().index, 0u);
	EXPECT_EQ(m_registry.key(m_registry.error_material()), ERROR_MATERIAL_KEY);
	EXPECT_EQ(m_registry.key(MaterialHandle{}), ERROR_MATERIAL_KEY); // a null handle reads the same row

	// The cooked error type's one parameter, magenta, is the same in sRGB and linear.
	const Span<const u8> record = m_registry.record(m_registry.error_material());
	ASSERT_EQ(record.size(), 16u);
	EXPECT_EQ(read<glm::vec4>(record, 0), glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));
}

TEST_F(Materials, NewMaterialsStartFromTheTypeDefaults)
{
	const MaterialTypeHandle type = m_registry.add_type(
		make_type("Defaults",
				  {
					  color("tint", 4, 0, "0.5 0.5 0.5 1"),
					  number("roughness", ParamKind::Float, 1, 16, "0.25"),
					  number("count", ParamKind::Int, 1, 20, "-3"),
					  texture("albedo", 24),
					  texture("normals", 32, "flat", gpu::Filter::Nearest, gpu::AddressMode::ClampToEdge),
				  },
				  48));
	ASSERT_FALSE(type.is_null());

	const MaterialHandle material = make(type);
	const Span<const u8> record	  = m_registry.record(material);
	ASSERT_EQ(record.size(), 48u);

	const glm::vec4 tint = read<glm::vec4>(record, 0);
	EXPECT_NEAR(tint.r, LINEAR_HALF, 1e-6f);
	EXPECT_NEAR(tint.b, LINEAR_HALF, 1e-6f);
	EXPECT_EQ(tint.a, 1.0f); // alpha is coverage, never gamma encoded

	EXPECT_EQ(read<f32>(record, 16), 0.25f);
	EXPECT_EQ(read<i32>(record, 20), -3);

	// A texture without a [Default] reads white, through its parameter's sampler.
	EXPECT_EQ(read<glm::uvec2>(record, 24),
			  glm::uvec2(index_of(BuiltinTexture::White), index_of(gpu::Filter::Linear, gpu::AddressMode::Repeat)));
	EXPECT_EQ(read<glm::uvec2>(record, 32), glm::uvec2(index_of(BuiltinTexture::Flat),
													   index_of(gpu::Filter::Nearest, gpu::AddressMode::ClampToEdge)));

	EXPECT_EQ(m_registry.key(material), material_key(type.index, 0));
	EXPECT_EQ(m_registry.type_of(material), type);
}

TEST_F(Materials, ValuesWriteThroughTheLayout)
{
	const MaterialTypeHandle type = m_registry.add_type(make_type("Values",
																  {
																	  color("tint", 4, 0),
																	  color("glow", 3, 16, "", true),
																	  number("roughness", ParamKind::Float, 1, 28),
																	  number("cell", ParamKind::Uint, 2, 32),
																	  number("lit", ParamKind::Bool, 1, 40),
																  },
																  48));

	const MaterialHandle material = make(type);

	EXPECT_TRUE(m_registry.set(material, "tint", glm::vec4(0.5f, 1.0f, 0.0f, 0.5f)));
	EXPECT_TRUE(m_registry.set(material, "glow", glm::vec3(4.0f, 0.5f, 0.0f)));
	EXPECT_TRUE(m_registry.set(material, "roughness", 0.75f));
	EXPECT_TRUE(m_registry.set(material, "cell", glm::uvec2(7u, 9u)));
	EXPECT_TRUE(m_registry.set(material, "lit", true));

	const Span<const u8> record = m_registry.record(material);

	const glm::vec4 tint = read<glm::vec4>(record, 0);
	EXPECT_NEAR(tint.r, LINEAR_HALF, 1e-6f);
	EXPECT_EQ(tint.g, 1.0f);
	EXPECT_EQ(tint.a, 0.5f);

	EXPECT_EQ(read<glm::vec3>(record, 16), glm::vec3(4.0f, 0.5f, 0.0f)); // [Hdr] is linear as written
	EXPECT_EQ(read<f32>(record, 28), 0.75f);
	EXPECT_EQ(read<glm::uvec2>(record, 32), glm::uvec2(7u, 9u));
	EXPECT_EQ(read<u32>(record, 40), 1u);
}

TEST_F(Materials, SetRefusesWhatDoesNotFit)
{
	const MaterialTypeHandle type = m_registry.add_type(make_type(
		"Strict", {color("tint", 4, 0, "1 1 1 1"), number("roughness", ParamKind::Float, 1, 16), texture("albedo", 20)},
		32));

	const MaterialHandle material = make(type);
	const std::vector<u8> before(m_registry.record(material).begin(), m_registry.record(material).end());

	EXPECT_FALSE(m_registry.set(material, "tnit", glm::vec4(0.0f)));	  // no such parameter
	EXPECT_FALSE(m_registry.set(material, "tint", glm::vec3(0.0f)));	  // too narrow
	EXPECT_FALSE(m_registry.set(material, "roughness", 1));				  // an int, not a float
	EXPECT_FALSE(m_registry.set(material, "albedo", 1.0f));				  // a number on a texture
	EXPECT_FALSE(m_registry.set(material, "roughness", TextureHandle{})); // a texture on a number
	EXPECT_FALSE(m_registry.set(MaterialHandle{}, "tint", glm::vec4(0.0f)));

	const Span<const u8> after = m_registry.record(material);
	EXPECT_TRUE(std::equal(before.begin(), before.end(), after.begin(), after.end()));
}

TEST_F(Materials, TexturesUseTheirParametersSamplerUnlessTheMaterialChoosesOne)
{
	const MaterialTypeHandle type =
		m_registry.add_type(make_type("Textured", {texture("albedo", 0, "", gpu::Filter::Nearest)}, 8));

	const MaterialHandle material = make(type);
	const TextureHandle art		  = m_registry.builtin(BuiltinTexture::Error); // any live texture will do

	ASSERT_TRUE(m_registry.set(material, "albedo", art));
	EXPECT_EQ(read<glm::uvec2>(m_registry.record(material), 0),
			  glm::uvec2(bindless_index(art), index_of(gpu::Filter::Nearest, gpu::AddressMode::Repeat)));

	ASSERT_TRUE(m_registry.set(material, "albedo", art, gpu::Filter::Linear, gpu::AddressMode::ClampToBorder));
	EXPECT_EQ(read<glm::uvec2>(m_registry.record(material), 0),
			  glm::uvec2(bindless_index(art), index_of(gpu::Filter::Linear, gpu::AddressMode::ClampToBorder)));

	ASSERT_TRUE(m_registry.set(material, "albedo", BuiltinTexture::Black));
	EXPECT_EQ(read<u32>(m_registry.record(material), 0), index_of(BuiltinTexture::Black));
}

TEST_F(Materials, TablesDoubleAndKeepEveryRecord)
{
	const MaterialTypeHandle type = m_registry.add_type(make_type("Growing", {number("id", ParamKind::Uint, 1, 0)}, 4));

	// The registry starts every table at two records, so this crosses two doublings.
	std::vector<MaterialHandle> materials;
	for (u32 i = 0; i < 5; ++i)
	{
		materials.push_back(make(type));
		ASSERT_TRUE(m_registry.set(materials.back(), "id", 100u + i));
	}

	sync();
	const u32 table = m_registry.table_index(type);
	EXPECT_NE(table, 0u);

	for (u32 i = 0; i < 5; ++i)
	{
		EXPECT_EQ(read<u32>(m_registry.record(materials[i]), 0), 100u + i);
		EXPECT_EQ(m_registry.key(materials[i]), material_key(type.index, static_cast<u16>(i)));
	}

	// Three more fit in the eight slots; a fourth doubles the table, which moves.
	for (u32 i = 0; i < 3; ++i)
		(void)make(type);

	sync();
	EXPECT_EQ(m_registry.table_index(type), table);

	(void)make(type);
	sync();
	EXPECT_NE(m_registry.table_index(type), table);
	EXPECT_EQ(read<u32>(m_registry.record(materials[4]), 0), 104u);
}

TEST_F(Materials, DestroyedMaterialsDrawTheErrorAndGiveBackTheirSlot)
{
	const MaterialTypeHandle type = m_registry.add_type(make_type("Reused", {number("id", ParamKind::Uint, 1, 0)}, 4));

	const MaterialHandle first	= make(type);
	const MaterialHandle second = make(type);
	EXPECT_EQ(key_slot(m_registry.key(second)), 1u);

	m_registry.destroy(first);
	EXPECT_EQ(m_registry.key(first), ERROR_MATERIAL_KEY);
	EXPECT_TRUE(m_registry.record(first).empty());

	const MaterialHandle third = make(type);
	EXPECT_EQ(m_registry.key(third), material_key(type.index, 0)); // the freed slot, straight away
}

TEST_F(Materials, ReplacingATypeReencodesItsMaterialsByName)
{
	const MaterialTypeHandle type = m_registry.add_type(
		make_type("Evolving", {number("a", ParamKind::Float, 1, 0), number("b", ParamKind::Float, 1, 4)}, 8));

	const MaterialHandle material = make(type);
	ASSERT_TRUE(m_registry.set(material, "a", 1.0f));
	ASSERT_TRUE(m_registry.set(material, "b", 2.0f));
	sync();

	// Reordered, grown and with a new field: every value lands at its new offset.
	const material::Type reordered = make_type("Evolving",
											   {
												   number("b", ParamKind::Float, 1, 0),
												   number("c", ParamKind::Float, 1, 4, "5"),
												   number("a", ParamKind::Float, 1, 8),
											   },
											   12);

	const u32 generation = m_registry.generation(type);
	ASSERT_TRUE(m_registry.replace_type(type, reordered));
	EXPECT_EQ(m_registry.generation(type), generation + 1);

	Span<const u8> record = m_registry.record(material);
	ASSERT_EQ(record.size(), 12u);
	EXPECT_EQ(read<f32>(record, 0), 2.0f);
	EXPECT_EQ(read<f32>(record, 4), 5.0f);
	EXPECT_EQ(read<f32>(record, 8), 1.0f);
	sync();

	// The same build again changed nothing a draw can see.
	ASSERT_TRUE(m_registry.replace_type(type, reordered));
	EXPECT_EQ(m_registry.generation(type), generation + 1);

	// a becomes a float2: the value no longer fits and the default stands in, until a build where
	// it fits again brings it back.
	ASSERT_TRUE(m_registry.replace_type(
		type, make_type("Evolving", {number("a", ParamKind::Float, 2, 0), number("b", ParamKind::Float, 1, 8)}, 12)));
	EXPECT_EQ(read<glm::vec2>(m_registry.record(material), 0), glm::vec2(0.0f));

	ASSERT_TRUE(m_registry.replace_type(type, reordered));
	EXPECT_EQ(read<f32>(m_registry.record(material), 8), 1.0f);
}

TEST_F(Materials, OnlyLiveSurfaceTypesKeyToTheirOwnBucket)
{
	const MaterialTypeHandle screen =
		m_registry.add_type(make_type("Grade", {color("tint", 4, 0, "1 1 1 1")}, 16, Domain::Screen));
	const MaterialTypeHandle surface =
		m_registry.add_type(make_type("Doomed", {number("id", ParamKind::Uint, 1, 0)}, 4));

	// A screen material has a record for the screen pass, but on an object it is a mistake.
	const MaterialHandle graded = make(screen);
	EXPECT_EQ(m_registry.key(graded), ERROR_MATERIAL_KEY);
	EXPECT_FALSE(m_registry.record(graded).empty());

	const MaterialHandle orphan = make(surface);
	EXPECT_NE(m_registry.key(orphan), ERROR_MATERIAL_KEY);
	sync();

	m_registry.remove_type(surface);
	EXPECT_EQ(m_registry.key(orphan), ERROR_MATERIAL_KEY);
	EXPECT_TRUE(m_registry.type_of(orphan).is_null());
	EXPECT_FALSE(m_registry.set(orphan, "id", 1u));
	sync(); // retires the removed type's table
}

TEST_F(Materials, TheStockTypesShipWithTheEngine)
{
	const material::Type* unlit	 = m_registry.type(m_registry.stock_type(StockType::Unlit));
	const material::Type* sprite = m_registry.type(m_registry.stock_type(StockType::Sprite));

	ASSERT_NE(unlit, nullptr);
	ASSERT_NE(sprite, nullptr);
	EXPECT_EQ(unlit->name, "Unlit");
	EXPECT_EQ(sprite->name, "Sprite");
	EXPECT_EQ(sprite->state.queue, material::Queue::Cutout);

	// A sprite shows its whole texture until the game picks a frame.
	const MaterialHandle card = make(m_registry.stock_type(StockType::Sprite));
	EXPECT_EQ(read<glm::vec4>(m_registry.instance_defaults(card), 0), glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
}

TEST_F(Materials, BucketsLieEndToEndSizedByTheirObjects)
{
	const MaterialTypeHandle type = m_registry.add_type(make_type("Counted", {number("id", ParamKind::Uint, 1, 0)}, 4));
	const MaterialTypeHandle sprite = m_registry.stock_type(StockType::Sprite);

	const MaterialHandle a	  = make(type);
	const MaterialHandle b	  = make(type);
	const MaterialHandle card = make(sprite);

	std::vector<u32> users(m_registry.material_capacity(), 0);
	users[a.index]	  = 2;
	users[b.index]	  = 3;
	users[card.index] = 4;
	users[40]		  = 1; // an index no material holds: its row names the error type

	std::vector<BucketRange> ranges(m_registry.bucket_count());
	EXPECT_EQ(m_registry.layout_buckets({users.data(), users.size()}, {ranges.data(), ranges.size()}), 10u);

	EXPECT_EQ(ranges[m_registry.error_type().index].capacity, 1u);
	EXPECT_EQ(ranges[type.index].capacity, 5u);
	EXPECT_EQ(ranges[sprite.index].capacity, 4u);

	for (size_t i = 1; i < ranges.size(); ++i)
		EXPECT_EQ(ranges[i].first, ranges[i - 1].first + ranges[i - 1].capacity) << i;
}

TEST_F(Materials, AMaterialMovedToATypeWithOtherInstanceDefaultsIsListedForReseeding)
{
	const std::vector<material::Param> frame = {number("frame", ParamKind::Float, 4, 0, "0 0 1 1")};

	const MaterialTypeHandle plain = m_registry.add_type(make_type("Plain", {number("a", ParamKind::Float, 1, 0)}, 4));
	const MaterialTypeHandle framed =
		m_registry.add_type(with_instance(make_type("Framed", {number("a", ParamKind::Float, 1, 0)}, 4), frame, 16));
	const MaterialTypeHandle framed_too =
		m_registry.add_type(with_instance(make_type("FramedToo", {number("b", ParamKind::Float, 1, 0)}, 4), frame, 16));

	const MaterialHandle material = make(plain);
	EXPECT_TRUE(m_registry.reseeds().empty());

	ASSERT_TRUE(m_registry.set_type(material, framed));
	ASSERT_EQ(m_registry.reseeds().size(), 1u);
	EXPECT_EQ(m_registry.reseeds()[0], material);

	// Another type with the same Instance defaults leaves the objects' data as it is.
	m_registry.clear_reseeds();
	ASSERT_TRUE(m_registry.set_type(material, framed_too));
	EXPECT_TRUE(m_registry.reseeds().empty());
}

TEST_F(Materials, ARebuildWithOtherInstanceDefaultsListsEveryMaterialOfTheType)
{
	const auto build = [](const char* rate)
	{
		return with_instance(make_type("Blinking", {number("a", ParamKind::Float, 1, 0)}, 4),
							 {number("rate", ParamKind::Float, 1, 0, rate)}, 4);
	};

	const MaterialTypeHandle type = m_registry.add_type(build("1"));
	const MaterialHandle a		  = make(type);
	const MaterialHandle b		  = make(type);

	// New code, same defaults: nothing to reseed.
	material::Type recompiled = build("1");
	recompiled.spirv.push_back(0u);
	recompiled.hash = material::hash_type(recompiled);

	ASSERT_TRUE(m_registry.replace_type(type, std::move(recompiled)));
	EXPECT_TRUE(m_registry.reseeds().empty());

	ASSERT_TRUE(m_registry.replace_type(type, build("0.5")));
	ASSERT_EQ(m_registry.reseeds().size(), 2u);
	EXPECT_NE(std::find(m_registry.reseeds().begin(), m_registry.reseeds().end(), a), m_registry.reseeds().end());
	EXPECT_NE(std::find(m_registry.reseeds().begin(), m_registry.reseeds().end(), b), m_registry.reseeds().end());
}

TEST_F(Materials, ObjectsFollowTheirTypesDefaultsUntilTheGameWritesTheirData)
{
	const MaterialTypeHandle plain = m_registry.add_type(make_type("Plain", {number("a", ParamKind::Float, 1, 0)}, 4));
	const MaterialTypeHandle framed =
		m_registry.add_type(with_instance(make_type("Framed", {number("a", ParamKind::Float, 1, 0)}, 4),
										  {number("frame", ParamKind::Float, 4, 0, "0 0 1 1")}, 16));

	const MaterialHandle material = make(plain);

	RenderScene scene;
	scene.init(8, &m_registry);

	const glm::vec4 given(0.5f);
	const auto untouched = scene.create_object({.material = material});
	const auto seeded	 = scene.create_object({
		.material = material,
		.instance = {reinterpret_cast<const u8*>(&given), sizeof(given)},
	});
	const auto written	 = scene.create_object({.material = material});
	scene.set_instance(written, glm::vec4(0.25f));

	// The material moves to a type that frames its objects: what the renderer does each frame.
	ASSERT_TRUE(m_registry.set_type(material, framed));
	scene.reseed(m_registry.reseeds());
	m_registry.clear_reseeds();

	const auto data = [&](RenderObjectHandle object)
	{ return read<glm::vec4>({scene.instance(object.index).bytes, material::INSTANCE_BYTES}, 0); };

	EXPECT_EQ(data(untouched), glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)); // the engine's: the new defaults
	EXPECT_EQ(data(seeded), given);								   // the game's, as it gave them
	EXPECT_EQ(data(written), glm::vec4(0.25f));					   // and as it wrote them
}

TEST_F(Materials, AnObjectGivenAMaterialOfAnotherTypeStartsItsDataOver)
{
	const std::vector<material::Param> frame = {number("frame", ParamKind::Float, 4, 0, "0 0 1 1")};

	const MaterialTypeHandle plain = m_registry.add_type(make_type("Plain", {number("a", ParamKind::Float, 1, 0)}, 4));
	const MaterialTypeHandle framed =
		m_registry.add_type(with_instance(make_type("Framed", {number("a", ParamKind::Float, 1, 0)}, 4), frame, 16));

	const MaterialHandle plain_material = make(plain);
	const MaterialHandle framed_a		= make(framed);
	const MaterialHandle framed_b		= make(framed);

	RenderScene scene;
	scene.init(4, &m_registry);

	const auto object = scene.create_object({.material = framed_a});
	scene.set_instance(object, glm::vec4(0.5f));

	const auto data = [&]
	{ return read<glm::vec4>({scene.instance(object.index).bytes, material::INSTANCE_BYTES}, 0); };

	// The same type: the data is still the object's.
	scene.set_material(object, framed_b);
	EXPECT_EQ(data(), glm::vec4(0.5f));

	// Another type: its defaults, which a type without an Instance leaves at zero.
	scene.set_material(object, plain_material);
	EXPECT_EQ(data(), glm::vec4(0.0f));

	scene.set_material(object, framed_a);
	EXPECT_EQ(data(), glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));

	// And being the engine's again, it follows the defaults when the type is rebuilt with others.
	ASSERT_TRUE(
		m_registry.replace_type(framed, with_instance(make_type("Framed", {number("a", ParamKind::Float, 1, 0)}, 4),
													  {number("frame", ParamKind::Float, 4, 0, "0 0 0.5 0.5")}, 16)));
	scene.reseed(m_registry.reseeds());
	m_registry.clear_reseeds();
	EXPECT_EQ(data(), glm::vec4(0.0f, 0.0f, 0.5f, 0.5f));
}
