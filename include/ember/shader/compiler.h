#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/shader/program.h>

#include <mutex>

#ifndef EMBER_SHADER_COMPILER
	#define EMBER_SHADER_COMPILER 0
#endif

namespace ember::shader
{
	struct ShaderCompilerDef
	{
		// Searched for #include after the engine's own shaders directory: the asset root,
		// so a material can include "shaders/common.slang" from there. Copied at init.
		Span<const char* const> include_dirs = {};

		// Where compiled programs are kept betweens runs, keyed by the hash of everything
		// that went into them. Null compiles every time. Created when missing.
		const char* cache_dir = nullptr;
	};

	/**
	 * Compiles shader files in process through libslang and reflects what it compiled: the
	 * SPIR-V, the entry points, and for a material the record layout its values encode into
	 * and the pipeline state its struct declared. The same code cooks ahead of time as ember_cook,
	 * so a cooked pair on disk is exactly what a dev build would have compiled. Built without
	 * EMBER_SHADER_COMPILER, init() returns false and cooked pairs are the only source.
	 *
	 * The disk cache makes a second run cost file reads: a compile is looked up by the hash
	 * of its source, every file it included and the compiler's version, so a prelude edit
	 * misses exactly the programs it touches.
	 */
	class ShaderCompiler
	{
	public:
		struct Stats
		{
			u32 compiled = 0; // programs Slang build
			u32 cached	 = 0; // programs the cache served
		};

		ShaderCompiler() noexcept;
		~ShaderCompiler() noexcept;

		ShaderCompiler(const ShaderCompiler&)			 = delete;
		ShaderCompiler& operator=(const ShaderCompiler&) = delete;

		// True when the module was built with libslang.
		[[nodiscard]] static constexpr bool available() noexcept { return EMBER_SHADER_COMPILER != 0; }

		[[nodiscard]] bool initialize(const ShaderCompilerDef& def = {}) noexcept;
		void shutdown() noexcept;

		/**
		 * Compiles one file. `name` is its path as the author knows it, printed in diagnostics
		 * and listed first in the includes; `source` is its text. The domain comes from the
		 * prelude the text includes: none makes a plain program. True with `out` filled, or
		 * false with the reasons in `diagnostics`. Warnings land there either way.
		 */
		[[nodiscard]] bool compile(StringView name, StringView source, Program& out, String& diagnostics) noexcept;

		[[nodiscard]] Stats stats() const noexcept { return m_stats; }

	private:
		struct Impl;

		[[nodiscard]] bool compile_slang(StringView name, StringView wrapped, Program& out,
										 String& diagnostics) noexcept;
		[[nodiscard]] u64 content_hash(u64 source_hash, Span<const String> includes) noexcept;
		[[nodiscard]] bool cached(u64 source_hash, Program& out) noexcept;
		void store(u64 source_hash, const Program& program) noexcept;

		Impl* m_impl = nullptr;
		Vector<String> m_include_dirs;
		String m_cache_dir;
		Stats m_stats;
		std::mutex m_lock;
	};

	// Which domain a file belongs to: the prelude it includes, Plain when it includes none.
	Domain detect_domain(StringView source) noexcept;

	// The engine file appended behind a material of the domain; null for Plain.
	[[nodiscard]] const char* entry_file(Domain domain) noexcept;

	/**
	 * A material's text reaches the engine through its prelude only: nothing named ember_* or
	 * EMBER_*, no vk:: attributes, push constants or registers. Comments and string literals
	 * are skipped. False with the offending line in `diagnostics`.
	 */
	[[nodiscard]] bool check_source(StringView name, StringView source, String& diagnostics) noexcept;

	/// What the compiler sees for a material: a #line naming the author's file, the file, then
	/// the domain's entry file. A plain program passes through unchanged.
	void wrap_source(Domain domain, StringView name, StringView source, String& out) noexcept;

	/// Blocking whole file reads and writes, for tools and the cache. Never on a frame thread;
	/// the io module is the engine's way to a file.
	[[nodiscard]] bool read_file(const char* path, Vector<u8>& out) noexcept;
	[[nodiscard]] bool write_file(const char* path, Span<const u8> bytes) noexcept;
}
