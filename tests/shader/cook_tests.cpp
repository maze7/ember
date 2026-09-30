#include <ember/core/filesystem.h>
#include <ember/memory/memory.h>
#include <ember/render/embedded_shaders.h>
#include <ember/shader/compiler.h>

#include <gtest/gtest.h>

#include <algorithm>

namespace
{
	using namespace ember;

	/**
	 * What the build cooked, through ember_cook in a process of its own, against what this process
	 * compiles through the same compiler. Equal bytes are the promise the one contract makes: a
	 * shader hot reload compiles is the shader the build would have cooked.
	 */
	class Cooked : public testing::Test
	{
	protected:
		void SetUp() override { ASSERT_TRUE(m_compiler.initialize()); }

		[[nodiscard]] String engine_file(StringView name) const
		{
			String path;
			EXPECT_TRUE(fs::join(path, m_compiler.engine_dir(), name));
			return path;
		}

		[[nodiscard]] static String read(StringView path)
		{
			auto file = fs::read_file(path, memory::heap(MemoryTag::Tools));
			EXPECT_TRUE(file) << path;
			return file ? String(file->text()) : String();
		}

		[[nodiscard]] static bool same_bytes(Span<const u8> a, Span<const u8> b)
		{
			return std::equal(a.begin(), a.end(), b.begin(), b.end());
		}

		shader::Compiler m_compiler;
	};
}

TEST_F(Cooked, TheEmbeddedProgramsAreWhatTheCompilerBuilds)
{
	const struct
	{
		const char* file;
		Span<const u8> (*embedded)() noexcept;
	} programs[] = {
		{"cull.slang", render::embedded::cull_shader},
		{"mesh.slang", render::embedded::mesh_shader},
		{"sprite.slang", render::embedded::sprite_shader},
		{"upscale.slang", render::embedded::upscale_shader},
	};

	for (const auto& program : programs)
	{
		const String path = engine_file(program.file);

		shader::Program compiled;
		String diagnostics;
		ASSERT_TRUE(m_compiler.compile_program(path, read(path), compiled, diagnostics)) << diagnostics;

		EXPECT_TRUE(same_bytes(program.embedded(), compiled.bytecode())) << program.file;
	}
}

TEST_F(Cooked, TheEmbeddedErrorTypeIsWhatTheCompilerBuilds)
{
	const Span<const u8> type_file = render::embedded::error_material_type();

	material::Type cooked;
	String error;
	ASSERT_TRUE(material::read_cooked({reinterpret_cast<const char*>(type_file.data()), type_file.size()},
									  render::embedded::error_material_spirv(), cooked, error))
		<< error;

	const String path = engine_file("materials/error.slang");

	material::Type compiled;
	String diagnostics;
	ASSERT_TRUE(m_compiler.compile_material(path, read(path), compiled, diagnostics)) << diagnostics;

	// The hash covers the bytecode, the record, the per-object data and the state.
	EXPECT_EQ(cooked.hash, compiled.hash);
	EXPECT_TRUE(same_bytes(cooked.bytecode(), compiled.bytecode()));
	EXPECT_EQ(cooked.name, "Error");
}
