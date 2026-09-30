#include <ember/material/type.h>

#include <gtest/gtest.h>

#include <bit>

namespace
{
	using namespace ember;
	using namespace ember::material;

	/// A type with every field away from its default, so a round trip that drops one shows.
	Type full_type()
	{
		Type type;
		type.domain = Domain::Surface;
		type.name	= "Sprite";
		type.spirv	= {0x07230203u, 1u, 2u, 3u};

		Param albedo;
		albedo.name	  = "albedo";
		albedo.kind	  = ParamKind::Texture2D;
		albedo.size	  = TEXTURE_REF_BYTES;
		albedo.filter = gpu::Filter::Nearest;
		albedo.wrap	  = gpu::AddressMode::ClampToEdge;
		albedo.linear = true;
		albedo.preset = "flat";

		Param tint;
		tint.name		= "tint";
		tint.display	= "Tint";
		tint.kind		= ParamKind::Float;
		tint.components = 4;
		tint.offset		= 16;
		tint.size		= 16;
		tint.color		= true;
		tint.hdr		= true;
		tint.has_range	= true;
		tint.range_min	= -1.0f;
		tint.range_max	= 2.5f;
		tint.preset		= "1 0.5 0.25 1";

		type.record.params = {albedo, tint};
		type.record.size   = 32;

		Param flash;
		flash.name = "flash";
		flash.kind = ParamKind::Float;
		flash.size = 4;

		type.instance.params = {flash};
		type.instance.size	 = 4;

		type.state.queue		= Queue::Cutout;
		type.state.cull			= gpu::CullMode::None;
		type.state.casts_shadow = false;
		type.state.priority		= -2;
		type.state.shading		= "Lit";

		type.hash = hash_type(type);
		return type;
	}

	void expect_same_layout(const Layout& a, const Layout& b)
	{
		ASSERT_EQ(a.params.size(), b.params.size());
		EXPECT_EQ(a.size, b.size);

		for (size_t i = 0; i < a.params.size(); ++i)
		{
			const Param& x = a.params[i];
			const Param& y = b.params[i];

			EXPECT_EQ(x.name, y.name);
			EXPECT_EQ(x.display, y.display);
			EXPECT_EQ(x.preset, y.preset);
			EXPECT_EQ(x.kind, y.kind);
			EXPECT_EQ(x.components, y.components);
			EXPECT_EQ(x.offset, y.offset);
			EXPECT_EQ(x.size, y.size);
			EXPECT_EQ(x.color, y.color);
			EXPECT_EQ(x.hdr, y.hdr);
			EXPECT_EQ(x.linear, y.linear);
			EXPECT_EQ(x.has_range, y.has_range);
			EXPECT_EQ(x.range_min, y.range_min);
			EXPECT_EQ(x.range_max, y.range_max);
			EXPECT_EQ(x.filter, y.filter);
			EXPECT_EQ(x.wrap, y.wrap);
		}
	}
}

TEST(TypeFile, RoundTripsEveryField)
{
	const Type written = full_type();

	String text;
	ASSERT_TRUE(write_type(written, text));

	Type read;
	String error;
	ASSERT_TRUE(read_type(text, read, error)) << error;

	EXPECT_EQ(read.domain, written.domain);
	EXPECT_EQ(read.name, written.name);
	EXPECT_EQ(read.hash, written.hash);
	EXPECT_EQ(read.state.queue, Queue::Cutout);
	EXPECT_EQ(read.state.cull, gpu::CullMode::None);
	EXPECT_FALSE(read.state.casts_shadow);
	EXPECT_EQ(read.state.priority, -2);
	EXPECT_EQ(read.state.shading, "Lit");
	expect_same_layout(read.record, written.record);
	expect_same_layout(read.instance, written.instance);

	// The file carries no SPIR-V; with the words back, the type hashes like the original.
	read.spirv = written.spirv;
	EXPECT_EQ(hash_type(read), written.hash);
}

TEST(TypeFile, RefusesWhatWouldMisreadARecord)
{
	String text;
	ASSERT_TRUE(write_type(full_type(), text));

	const auto refused = [&](StringView from, StringView to)
	{
		String broken	= text;
		const size_t at = broken.find(from);
		EXPECT_NE(at, String::npos) << from;
		broken.replace(at, from.size(), to);

		Type type;
		String error;
		const bool read = read_type(broken, type, error);
		EXPECT_FALSE(error.empty()) << to;
		return !read;
	};

	EXPECT_TRUE(refused("\"format\": 1", "\"format\": 99"));
	EXPECT_TRUE(refused("\"kind\": \"float\"", "\"kind\": \"double\""));
	EXPECT_TRUE(refused("\"offset\": 16", "\"offset\": 24"));		// runs past the record
	EXPECT_TRUE(refused("\"components\": 4", "\"components\": 3")); // 16 bytes cannot be a float3
	EXPECT_TRUE(refused("\"queue\": \"cutout\"", "\"queue\": \"sideways\""));
	EXPECT_TRUE(refused("\"filter\": \"point\"", "\"filter\": \"cubic\""));
}

TEST(TypeHash, CoversWhatARegistryWouldRebuildFor)
{
	const Type base = full_type();

	const auto differs = [&](auto&& change)
	{
		Type changed = base;
		change(changed);
		return hash_type(changed) != base.hash;
	};

	EXPECT_TRUE(differs([](Type& t) { t.spirv.back() ^= 1u; }));
	EXPECT_TRUE(differs([](Type& t) { t.record.params[1].preset = "0 0 0 1"; }));
	EXPECT_TRUE(differs([](Type& t) { t.record.params[1].offset = 20; }));
	EXPECT_TRUE(differs([](Type& t) { t.state.cull = gpu::CullMode::Back; }));
	EXPECT_TRUE(differs([](Type& t) { t.state.shading = "Unlit"; }));

	// Dependencies are where the source came from, not what was built.
	EXPECT_FALSE(differs([](Type& t) { t.dependencies.emplace_back("elsewhere.slang"); }));
}

TEST(ParamValues, ParseTheWayFilesWriteThem)
{
	Param param;
	u32 words[4] = {};

	param.kind		 = ParamKind::Float;
	param.components = 4;
	ASSERT_TRUE(parse_value(param, "1 0.5, -2 1e3", words));
	EXPECT_EQ(std::bit_cast<f32>(words[1]), 0.5f);
	EXPECT_EQ(std::bit_cast<f32>(words[3]), 1000.0f);

	EXPECT_FALSE(parse_value(param, "1 2 3", words));	   // too few
	EXPECT_FALSE(parse_value(param, "1 2 3 4 5", words));  // too many
	EXPECT_FALSE(parse_value(param, "1 2 3 0.5x", words)); // a token must parse whole

	param.kind		 = ParamKind::Int;
	param.components = 2;
	ASSERT_TRUE(parse_value(param, "-3 7", words));
	EXPECT_EQ(std::bit_cast<i32>(words[0]), -3);
	EXPECT_FALSE(parse_value(param, "1.5 2", words));

	param.kind		 = ParamKind::Bool;
	param.components = 1;
	ASSERT_TRUE(parse_value(param, "true", words));
	EXPECT_EQ(words[0], 1u);
	EXPECT_FALSE(parse_value(param, "yes", words));

	param.kind = ParamKind::Texture2D;
	EXPECT_FALSE(parse_value(param, "0", words)); // textures are named, not numbered
}

TEST(CookedPair, JoinsOnlyTheBytecodeItWasCookedWith)
{
	const Type written = full_type();

	String text;
	ASSERT_TRUE(write_type(written, text));

	Type read;
	String error;
	ASSERT_TRUE(read_cooked(text, written.bytecode(), read, error)) << error;
	EXPECT_EQ(read.spirv, written.spirv);
	EXPECT_EQ(read.hash, written.hash);

	// The .spv of another cook: one word differs, and the stored hash says so.
	Type other = written;
	other.spirv.back() ^= 1u;

	EXPECT_FALSE(read_cooked(text, other.bytecode(), read, error));
	EXPECT_NE(error.find("cook it again"), String::npos) << error;

	// Bytes that are not whole words are not SPIR-V at all.
	EXPECT_FALSE(read_cooked(text, written.bytecode().first(7), read, error));
	EXPECT_FALSE(read_cooked(text, {}, read, error));
}
