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

TEST_F(Cooked, TheEmbeddedTypesAreWhatTheCompilerBuilds)
{
	const struct
	{
		const char* file;
		const char* name;
		Span<const u8> (*type_file)() noexcept;
		Span<const u8> (*spirv)() noexcept;
	} types[] = {
		{"materials/error.slang", "Error", render::embedded::error_material_type,
		 render::embedded::error_material_spirv},
		{"materials/unlit.slang", "Unlit", render::embedded::unlit_material_type,
		 render::embedded::unlit_material_spirv},
		{"materials/sprite.slang", "Sprite", render::embedded::sprite_material_type,
		 render::embedded::sprite_material_spirv},
	};

	for (const auto& type : types)
	{
		const Span<const u8> type_file = type.type_file();

		material::Type cooked;
		String error;
		ASSERT_TRUE(material::read_cooked({reinterpret_cast<const char*>(type_file.data()), type_file.size()},
										  type.spirv(), cooked, error))
			<< type.file << ": " << error;

		const String path = engine_file(type.file);

		material::Type compiled;
		String diagnostics;
		ASSERT_TRUE(m_compiler.compile_material(path, read(path), compiled, diagnostics)) << diagnostics;

		// The hash covers the bytecode, the record, the per-object data and the state.
		EXPECT_EQ(cooked.hash, compiled.hash) << type.file;
		EXPECT_TRUE(same_bytes(cooked.bytecode(), compiled.bytecode())) << type.file;
		EXPECT_EQ(cooked.name, type.name);
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
