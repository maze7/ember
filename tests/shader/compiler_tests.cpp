#include <ember/core/filesystem.h>
#include <ember/memory/memory.h>
#include <ember/shader/compiler.h>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>

namespace
{
	using namespace ember;
	using namespace ember::material;
	using ember::shader::Compiler;

	/**
	 * One compiler for the suite. Its global session takes tens of milliseconds to start, and
	 * sharing it exercises what hot reload relies on: one warm session serving many compiles and
	 * replaced exactly when it must be. Material files are written into a scratch directory with a
	 * lib/ the compiler searches, so every compile resolves real files the way the game's will.
	 */
	class Compiling : public testing::Test
	{
	protected:
		static void SetUpTestSuite()
		{
			String temp;
			ASSERT_TRUE(fs::temporary_directory(temp));

			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			s_root			 = new String();
			ASSERT_TRUE(fs::join(*s_root, temp, fmt::format("ember_shader_tests_{}", stamp)));

			String lib;
			ASSERT_TRUE(fs::join(lib, *s_root, "lib"));
			ASSERT_TRUE(fs::create_directories(lib));

			const char* include_dirs[] = {lib.c_str()};

			s_compiler = new Compiler();
			ASSERT_TRUE(s_compiler->initialize({.include_dirs = include_dirs}));
		}

		static void TearDownTestSuite()
		{
			delete s_compiler;
			s_compiler = nullptr;

			if (s_root != nullptr)
				(void)fs::remove_tree(*s_root);

			delete s_root;
			s_root = nullptr;
		}

		static String write(StringView name, StringView text)
		{
			String path;
			EXPECT_TRUE(fs::join(path, *s_root, name));
			EXPECT_TRUE(fs::write_file(path, {reinterpret_cast<const u8*>(text.data()), text.size()}));
			return path;
		}

		/// Writes `text` as `name` and compiles it as a material type: the type lands in m_type,
		/// whatever the compiler said in m_diagnostics.
		bool compile(StringView name, StringView text)
		{
			m_path = write(name, text);
			m_diagnostics.clear();
			return s_compiler->compile_material(m_path, text, m_type, m_diagnostics);
		}

		[[nodiscard]] const Param& param(StringView name) const
		{
			const Param* found = find_param(m_type.record, name);
			EXPECT_NE(found, nullptr) << name;

			static const Param missing;
			return found != nullptr ? *found : missing;
		}

		[[nodiscard]] bool said(StringView text) const
		{
			return StringView(m_diagnostics).find(text) != StringView::npos;
		}

		[[nodiscard]] u32 errors() const
		{
			u32 count = 0;
			for (size_t at = m_diagnostics.find(": error: "); at != String::npos;
				 at		   = m_diagnostics.find(": error: ", at + 1))
				++count;
			return count;
		}

		static inline Compiler* s_compiler = nullptr;
		static inline String* s_root	   = nullptr;

		Type m_type;
		String m_diagnostics;
		String m_path;
	};

	[[nodiscard]] bool contains_word(const Vector<u32>& spirv, f32 value)
	{
		return std::find(spirv.begin(), spirv.end(), std::bit_cast<u32>(value)) != spirv.end();
	}
}

TEST_F(Compiling, CompilesTheEngineErrorType)
{
	String path;
	ASSERT_TRUE(fs::join(path, s_compiler->engine_dir(), "materials/error.slang"));

	auto file = fs::read_file(path, memory::heap(MemoryTag::Tools));
	ASSERT_TRUE(file);
	ASSERT_TRUE(s_compiler->compile_material(path, file->text(), m_type, m_diagnostics)) << m_diagnostics;

	EXPECT_EQ(m_type.domain, Domain::Surface);
	EXPECT_EQ(m_type.name, "Error");
	ASSERT_GE(m_type.spirv.size(), 5u);
	EXPECT_EQ(m_type.spirv[0], 0x07230203u); // the SPIR-V magic number

	EXPECT_EQ(m_type.state.queue, Queue::Opaque);
	EXPECT_EQ(m_type.state.cull, gpu::CullMode::None);
	EXPECT_EQ(m_type.state.shading, "Unlit");

	EXPECT_EQ(m_type.record.size, 16u);
	const Param& color = param("color");
	EXPECT_EQ(color.kind, ParamKind::Float);
	EXPECT_EQ(color.components, 4);
	EXPECT_EQ(color.offset, 0u);
	EXPECT_TRUE(color.color);
	EXPECT_EQ(color.preset, "1 0 1 1");
	EXPECT_EQ(m_type.hash, hash_type(m_type));
}

TEST_F(Compiling, ReflectsEveryParameterKindAtItsStd430Offset)
{
	ASSERT_TRUE(compile("kinds.slang", R"(
import material;

struct Kinds : IMaterial
{
	[Range(-1, 2.5)] float f1;
	float2 f2;
	[Color] float3 f3;
	[Color] [Hdr] [Default("1 2 3 1")] float4 f4;
	int i1;
	uint u1;
	bool b1;
	[Filter("point")] [Wrap("clamp")] [Linear] Texture2DRef t2d;
	Texture2DArrayRef t2da;
	TextureCubeRef tcube;
	int2 i2;
	uint3 u3;

	void surface(SurfaceInput s, inout Surface out) { out.albedo = f3; }
};
)")) << m_diagnostics;

	struct Expected
	{
		const char* name;
		ParamKind kind;
		u8 components;
		u32 offset;
	};

	// std430: a float3 aligns to 16, a texture ref packs at 4, and the stride rounds up to 16.
	const Expected expected[] = {
		{"f1", ParamKind::Float, 1, 0},
		{"f2", ParamKind::Float, 2, 8},
		{"f3", ParamKind::Float, 3, 16},
		{"f4", ParamKind::Float, 4, 32},
		{"i1", ParamKind::Int, 1, 48},
		{"u1", ParamKind::Uint, 1, 52},
		{"b1", ParamKind::Bool, 1, 56},
		{"t2d", ParamKind::Texture2D, 1, 60},
		{"t2da", ParamKind::Texture2DArray, 1, 68},
		{"tcube", ParamKind::TextureCube, 1, 76},
		{"i2", ParamKind::Int, 2, 88},
		{"u3", ParamKind::Uint, 3, 96},
	};

	ASSERT_EQ(m_type.record.params.size(), std::size(expected));
	EXPECT_EQ(m_type.record.size, 112u);

	for (const Expected& e : expected)
	{
		const Param& p = param(e.name);
		EXPECT_EQ(p.kind, e.kind) << e.name;
		EXPECT_EQ(p.components, e.components) << e.name;
		EXPECT_EQ(p.offset, e.offset) << e.name;
	}

	EXPECT_TRUE(param("f1").has_range);
	EXPECT_EQ(param("f1").range_min, -1.0f);
	EXPECT_EQ(param("f1").range_max, 2.5f);
	EXPECT_TRUE(param("f3").color);
	EXPECT_TRUE(param("f4").hdr);
	EXPECT_EQ(param("f4").preset, "1 2 3 1");
	EXPECT_EQ(param("t2d").filter, gpu::Filter::Nearest);
	EXPECT_EQ(param("t2d").wrap, gpu::AddressMode::ClampToEdge);
	EXPECT_TRUE(param("t2d").linear);
}

TEST_F(Compiling, ReadsTheTypeAttributes)
{
	ASSERT_TRUE(compile("attributes.slang", R"(
import material;

[Queue("transparent")] [Cull("front")] [Blend("additive")] [DepthWrite(true)] [Shadow(true)] [Priority(-3)] [Shading("unlit")]
struct Attributes : IMaterial
{
	void surface(SurfaceInput s, inout Surface out) {}
};
)")) << m_diagnostics;

	const State& state = m_type.state;
	EXPECT_EQ(state.queue, Queue::Transparent);
	EXPECT_EQ(state.cull, gpu::CullMode::Front);
	EXPECT_EQ(state.blend, gpu::BlendPreset::Additive);
	EXPECT_TRUE(state.depth_write);
	EXPECT_TRUE(state.casts_shadow);
	EXPECT_EQ(state.priority, -3);
	EXPECT_EQ(state.shading, "Unlit");
}

TEST_F(Compiling, TransparentTypesBlendOverTheWorldByDefault)
{
	ASSERT_TRUE(compile("glass.slang", R"(
import material;

[Queue("transparent")]
struct Glass : IMaterial
{
	void surface(SurfaceInput s, inout Surface out) { out.opacity = 0.5; }
};
)")) << m_diagnostics;

	EXPECT_EQ(m_type.state.blend, gpu::BlendPreset::AlphaBlend);
	EXPECT_FALSE(m_type.state.depth_write);
	EXPECT_FALSE(m_type.state.casts_shadow);
	EXPECT_EQ(m_type.state.shading, "Lit"); // the default model
}

TEST_F(Compiling, ListsWhatAnAttributeAccepts)
{
	EXPECT_FALSE(compile("typo.slang", R"(
import material;

[Queue("cutot")]
struct Typo : IMaterial
{
	void surface(SurfaceInput s, inout Surface out) {}
};
)"));

	EXPECT_TRUE(said("queue 'cutot' is not one of: opaque, cutout, transparent")) << m_diagnostics;
}

TEST_F(Compiling, ReportsEveryAttributeThatDoesNotFitInOneCompile)
{
	EXPECT_FALSE(compile("misfit.slang", R"(
import material;

[Blend("additive")]
struct Misfit : IMaterial
{
	[Range(0, 1)] Texture2DRef a;
	[Color] float2 b;
	[Filter("point")] float c;
	[Hdr] float3 d;
	[Default("1 2")] float4 e;
	[Default("grey")] Texture2DRef f;

	void surface(SurfaceInput s, inout Surface out) {}
};
)"));

	EXPECT_TRUE(said("[Blend] needs [Queue(\"transparent\")]")) << m_diagnostics;
	EXPECT_TRUE(said("Misfit.a: [Range] needs a number"));
	EXPECT_TRUE(said("Misfit.b: [Color] needs a float3 or float4"));
	EXPECT_TRUE(said("Misfit.c: [Filter], [Wrap] and [Linear] are for textures"));
	EXPECT_TRUE(said("Misfit.d: [Hdr] qualifies [Color]"));
	EXPECT_TRUE(said("Misfit.e: [Default(\"1 2\")] is not 4 float values"));
	EXPECT_TRUE(said("Misfit.f: [Default(\"grey\")] is not white, black, flat or error"));
	EXPECT_EQ(errors(), 7u);
}

TEST_F(Compiling, PacksInstanceDataInDeclarationOrder)
{
	ASSERT_TRUE(compile("packed.slang", R"(
import material;

struct Packed : IMaterial
{
	struct Instance
	{
		float a;
		[Color] float3 b;
		uint c;
	};

	void surface(SurfaceInput s, inout Surface out)
	{
		let data = instance<Instance>(s);
		out.albedo = data.b * data.a;
	}
};
)")) << m_diagnostics;

	// Packed, the way ByteAddressBuffer.Load<T> reads it; std430 would have put b at 16.
	const Layout& instance = m_type.instance;
	ASSERT_EQ(instance.params.size(), 3u);
	EXPECT_EQ(instance.params[1].name, "b");
	EXPECT_EQ(instance.params[1].offset, 4u);
	EXPECT_EQ(instance.params[1].size, 12u);
	EXPECT_TRUE(instance.params[1].color);
	EXPECT_EQ(instance.params[2].offset, 16u);
	EXPECT_EQ(instance.size, 20u);
}

TEST_F(Compiling, RefusesInstanceDataThatDoesNotFit)
{
	EXPECT_FALSE(compile("bloated.slang", R"(
import material;

struct Bloated : IMaterial
{
	struct Instance
	{
		float4 a;
		float4 b;
		float c;
		Texture2DRef d;
	};

	void surface(SurfaceInput s, inout Surface out) {}
};
)"));

	EXPECT_TRUE(said("Bloated.Instance.d: per-object data holds numbers")) << m_diagnostics;
	EXPECT_TRUE(said("Bloated.Instance is 36 bytes; per-object data holds 32 at most"));
}

TEST_F(Compiling, ATypeIsExactlyOneStruct)
{
	EXPECT_FALSE(compile("none.slang", "import material;\nstruct Helper { float x; };\n"));
	EXPECT_TRUE(said("declares no struct implementing IMaterial or IScreenMaterial")) << m_diagnostics;

	EXPECT_FALSE(compile("two.slang", R"(
import material;
struct One : IMaterial { void surface(SurfaceInput s, inout Surface out) {} };
struct Two : IMaterial { void surface(SurfaceInput s, inout Surface out) {} };
)"));
	EXPECT_TRUE(said("declares 2 material structs (One, Two)")) << m_diagnostics;
}

TEST_F(Compiling, DetectsTheScreenDomainFromTheInterface)
{
	ASSERT_TRUE(compile("grade.slang", R"(
import material;

struct Grade : IScreenMaterial
{
	[Color] float4 tint;

	float4 pixel(ScreenInput s) { return scene_color(s.uv) * tint; }
};
)")) << m_diagnostics;

	EXPECT_EQ(m_type.domain, Domain::Screen);
	EXPECT_TRUE(m_type.state.shading.empty());
	EXPECT_EQ(m_type.state.cull, gpu::CullMode::None);

	EXPECT_FALSE(compile("queued.slang", R"(
import material;

[Queue("opaque")]
struct Queued : IScreenMaterial
{
	float4 pixel(ScreenInput s) { return 1.0; }
};
)"));
	EXPECT_TRUE(said("[Queue] does not apply to a screen material")) << m_diagnostics;
}

TEST_F(Compiling, MatchesShadingModelsIgnoringCase)
{
	ASSERT_TRUE(compile("loud.slang", R"(
import material;
[Shading("LIT")]
struct Loud : IMaterial { void surface(SurfaceInput s, inout Surface out) {} };
)")) << m_diagnostics;
	EXPECT_EQ(m_type.state.shading, "Lit");

	EXPECT_FALSE(compile("toon.slang", R"(
import material;
[Shading("toon")]
struct Toon : IMaterial { void surface(SurfaceInput s, inout Surface out) {} };
)"));
	EXPECT_TRUE(said("shading model 'toon' is not one of: unlit, lit")) << m_diagnostics;
}

TEST_F(Compiling, RefusesEngineImportsFromMaterialsAndTheirLibraries)
{
	EXPECT_FALSE(compile("sneaky.slang", R"(import material;
// import render; is only a comment
import ember;
import "frame.slang";
struct Sneaky : IMaterial { void surface(SurfaceInput s, inout Surface out) {} };
)"));

	EXPECT_TRUE(said("sneaky.slang:3: error: imports 'ember', an engine module")) << m_diagnostics;
	EXPECT_TRUE(said("sneaky.slang:4: error: imports 'frame'"));
	EXPECT_FALSE(said("'render'"));

	(void)write("lib/leaky.slang", "import shading;\npublic float leak() { return 1.0; }\n");

	EXPECT_FALSE(compile("innocent.slang", R"(
import material;
import leaky;
struct Innocent : IMaterial { void surface(SurfaceInput s, inout Surface out) { out.albedo = leak(); } };
)"));
	EXPECT_TRUE(said("leaky.slang:1: error: imports 'shading'")) << m_diagnostics;
}

TEST_F(Compiling, RefusesGlobalsThatWouldBeShaderParameters)
{
	EXPECT_FALSE(compile("globals.slang", R"(
import material;

static const float GAIN = 2.0;
[[vk::binding(0, 0)]] Texture2D stolen[];
float naked;

struct Globals : IMaterial
{
	void surface(SurfaceInput s, inout Surface out) { out.albedo = stolen[0].Load(int3(0, 0, 0)).rgb * GAIN * naked; }
};
)"));

	EXPECT_TRUE(said("declares the global 'stolen'")) << m_diagnostics;
	EXPECT_TRUE(said("declares the global 'naked'"));
	EXPECT_FALSE(said("'GAIN'"));
}

TEST_F(Compiling, SlangErrorsNameTheAuthorsFileAndLine)
{
	EXPECT_FALSE(compile("mismatch.slang", R"(import material;

struct Mismatch : IMaterial
{
	Texture2DRef albedo;

	void surface(SurfaceInput s, inout Surface out) { out.albedo = albedo.sample(s.uv); }
};
)"));

	EXPECT_TRUE(said("mismatch.slang:7:")) << m_diagnostics;
}

TEST_F(Compiling, RecompilingAFileSeesItsNewSource)
{
	const char* first  = "import material;\nstruct Evolving : IMaterial { float a; void surface(SurfaceInput s, inout "
						 "Surface out) {} };\n";
	const char* second = "import material;\nstruct Evolving : IMaterial { float a; float b; void surface(SurfaceInput "
						 "s, inout Surface out) {} };\n";

	ASSERT_TRUE(compile("evolving.slang", first)) << m_diagnostics;
	EXPECT_EQ(m_type.record.params.size(), 1u);

	ASSERT_TRUE(compile("evolving.slang", second)) << m_diagnostics;
	EXPECT_EQ(m_type.record.params.size(), 2u);
}

TEST_F(Compiling, AnEditedLibraryReachesTheNextCompile)
{
	const auto type = [](StringView name)
	{
		return fmt::format("import material;\nimport tone;\nstruct {} : IMaterial {{ void surface(SurfaceInput s, "
						   "inout Surface out) {{ out.albedo = TONE; }} }};\n",
						   name);
	};

	(void)write("lib/tone.slang", "public static const float TONE = 0.123;\n");
	ASSERT_TRUE(compile("before.slang", type("Before"))) << m_diagnostics;
	EXPECT_TRUE(contains_word(m_type.spirv, 0.123f));

	// A different type, so only the stamps can tell the warm session its copy of tone is old.
	(void)write("lib/tone.slang", "public static const float TONE = 0.4567;\n");
	ASSERT_TRUE(compile("after.slang", type("After"))) << m_diagnostics;
	EXPECT_TRUE(contains_word(m_type.spirv, 0.4567f));
	EXPECT_FALSE(contains_word(m_type.spirv, 0.123f));
}

TEST_F(Compiling, AnEditedCommentChangesNothing)
{
	const char* code = "import material;\nstruct Stable : IMaterial { float a; void surface(SurfaceInput s, inout "
					   "Surface out) { out.albedo = a; } };\n";

	ASSERT_TRUE(compile("stable.slang", code)) << m_diagnostics;
	const u64 hash = m_type.hash;

	ASSERT_TRUE(compile("stable.slang", fmt::format("// A comment the artist added.\n{}", code))) << m_diagnostics;
	EXPECT_EQ(m_type.hash, hash);
}

TEST_F(Compiling, ListsEveryFileTheCompileRead)
{
	(void)write("lib/tone.slang", "public static const float TONE = 0.5;\n");

	ASSERT_TRUE(compile("reader.slang", "import material;\nimport tone;\nstruct Reader : IMaterial { void "
										"surface(SurfaceInput s, inout Surface out) { out.albedo = TONE; } };\n"))
		<< m_diagnostics;

	const Vector<String>& files = m_type.dependencies;
	ASSERT_FALSE(files.empty());
	EXPECT_EQ(files.front(), m_path);

	const auto read = [&](StringView name)
	{
		return std::any_of(files.begin(), files.end(),
						   [&](const String& file) { return StringView(file).ends_with(name); });
	};

	EXPECT_TRUE(read("/material.slang"));
	EXPECT_TRUE(read("/surface.slang"));
	EXPECT_TRUE(read("/shading.slang"));
	EXPECT_TRUE(read("/tone.slang"));
}

TEST_F(Compiling, ATypeMayShareItsNameWithAShadingModel)
{
	// Each link-time export is written in a module of its own, so neither sees the other's Lit.
	ASSERT_TRUE(compile("lit.slang", "import material;\nstruct Lit : IMaterial { void surface(SurfaceInput s, "
									 "inout Surface out) {} };\n"))
		<< m_diagnostics;

	EXPECT_EQ(m_type.name, "Lit");
	EXPECT_EQ(m_type.state.shading, "Lit");
}

TEST_F(Compiling, CompilesTheEnginesPlainPrograms)
{
	for (const char* name : {"cull.slang", "upscale.slang", "imgui.slang"})
	{
		String path;
		ASSERT_TRUE(fs::join(path, s_compiler->engine_dir(), name));

		auto file = fs::read_file(path, memory::heap(MemoryTag::Tools));
		ASSERT_TRUE(file) << name;

		shader::Program program;
		String diagnostics;
		ASSERT_TRUE(s_compiler->compile_program(path, file->text(), program, diagnostics)) << name << diagnostics;

		ASSERT_FALSE(program.spirv.empty()) << name;
		EXPECT_EQ(program.spirv[0], 0x07230203u) << name;
		EXPECT_EQ(program.dependencies.front(), path) << name;
	}
}

TEST_F(Compiling, AProgramWithoutEntryPointsIsRefused)
{
	const String path = write("helpers_only.slang", "import ember;\nfloat helper() { return 1.0; }\n");

	shader::Program program;
	String diagnostics;
	EXPECT_FALSE(
		s_compiler->compile_program(path, "import ember;\nfloat helper() { return 1.0; }\n", program, diagnostics));
	EXPECT_NE(StringView(diagnostics).find("declares no entry points"), StringView::npos) << diagnostics;
}

TEST_F(Compiling, AFixedSaveCompilesAfterABrokenOne)
{
	// What hot reload does with a typo: the broken save of a file, then the fix.
	ASSERT_FALSE(compile("typo.slang", "import material;\nstruct Typo : IMaterial { oops };\n"));

	EXPECT_TRUE(compile("typo.slang", "import material;\nstruct Typo : IMaterial { void surface(SurfaceInput s, "
									  "inout Surface out) {} };\n"))
		<< m_diagnostics;
}

TEST_F(Compiling, AFailedCompileStillListsWhatItRead)
{
	(void)write("lib/tone.slang", "public static const float TONE = 0.5;\n");

	// It loads, imports and all, and then fails a check: the fix may be made in any file it read.
	ASSERT_FALSE(compile("unfit.slang", "import material;\nimport tone;\n[Queue(\"sideways\")] struct Unfit : "
										"IMaterial { void surface(SurfaceInput s, inout Surface out) { out.albedo = "
										"TONE; } };\n"));

	const Vector<String>& files = m_type.dependencies;
	EXPECT_TRUE(std::any_of(files.begin(), files.end(),
							[](const String& file) { return StringView(file).ends_with("/tone.slang"); }));
}
