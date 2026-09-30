#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/material/type.h>

#include <mutex>

namespace ember::shader
{
	struct CompilerDef
	{
		/**
		 * The engine's shader directory: its modules, and the entry modules material types link against.
		 * Null uses the directory this build was configured with.
		 */
		const char* engine_dir = nullptr;

		/**
		 * Searched for the modules a material file imports, after the file's own directory: the game's
		 * shader libraries. Copied at init.
		 */
		Span<const char* const> include_dirs = {};

		/** The shading model of surface types that name none, matched like [Shading] */
		const char* default_shading = "lit";
	};

	/**
	 * A plain program: a Slang file with entry points of its own, such as the cull kernel, the
	 * upscale or imgui. No material is involved; the engine embeds these.
	 */
	struct Program
	{
		Vector<u32> spirv;			 // every entry point the file declares, in one blob
		Vector<String> dependencies; // every file the compile read, the program's own first

		Span<const u8> bytecode() const noexcept
		{
			return {reinterpret_cast<const u8*>(spirv.data()), spirv.size() * sizeof(u32)};
		}
	};

	/**
	 * The engine's way from Slang to SPIR-V, in process through libslang, with one set of options
	 * for everything it compiles: plain programs, and material types. A material type's file becomes
	 * a module, the compiler writes the one module that links it (exporting the author's struct as
	 * the entry points' Material and the shading model it asked for as Shading), links the domain's
	 * entry module against both, and reflects the result into a MaterialType.
	 *
	 * The authoring boundary is enforced here: no file outside the engine may import an engine module
	 * other than the authoring API, and no author module may decdlare a global that would be a shader
	 * parameter.
	 *
	 * A Slang session caches every module it loads and cannot load a file twice, so the session is
	 * kept warm between compiles and replaced when a compile names a file it has loaded before (a
	 * reload) or any file it has read has changed on disk (an edited library). A warm compile takes
	 * around 20ms; a replaced session adds the engine modules' load again, tens of milliseconds.
	 *
	 * Compiles may come from any thread; they run one at a time behind a lock, because Slang sessions
	 * are not reentrant.
	 */
	class Compiler
	{
	public:
		Compiler() noexcept = default;
		~Compiler() noexcept;

		Compiler(const Compiler&)			 = delete;
		Compiler& operator=(const Compiler&) = delete;

		/// False, logged, when libslang cannot start or the engine directory holds no shaders.
		[[nodiscard]] bool initialize(const CompilerDef& def = {}) noexcept;
		void shutdown() noexcept;

		/**
		 * Compiles one plain program: every entry point the file marks with [shader(...)]. `path` is
		 * the file on disk, which diagnostics name and imports resolve against; `source` is its text,
		 * already read. True with `out` filled; false with the reasons in `diagnostics`.
		 */
		[[nodiscard]] bool compile_program(StringView path, StringView source, Program& out,
										   String& diagnostics) noexcept;

		/**
		 * Compiles one material type, the same way. False with every problem the type has in
		 * `diagnostics`: the whole file is checked before anything links.
		 */
		[[nodiscard]] bool compile_material(StringView path, StringView source, material::Type& out,
											String& diagnostics) noexcept;

		/** The engine shader directory in use: absolute, ending in a separator */
		StringView engine_dir() const noexcept;

		/** The engine shader directory this build was configured with: what a null engine_dir means. */
		[[nodiscard]] static const char* configured_engine_dir() noexcept;

	private:
		struct Impl;

		Impl* m_impl = nullptr;
		std::mutex m_lock;
	};
}
