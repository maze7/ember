#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/core/result.h>
#include <ember/memory/memory.h>
#include <ember/script/problem.h>

namespace ember::script
{
	/**
	 * Where a script runs, from the directory it lives in below the scripts root. The directory is
	 * the rights: a file cannot claim more by what it says inside.
	 *
	 *   sim/      the server, and the owner's client for what it predicts; writes Predicted components
	 *   server/   the server alone; writes anything the server simulates
	 *   client/   clients alone, every frame; writes Client components
	 *   lib/      shared code the others require. It runs with its caller's rights, and is linted to
	 *             the sim's rules when it loads, since a sim script may be that caller.
	 */
	enum class Context : u8
	{
		Sim,
		Server,
		Client,
		Lib,
		Count
	};

	/** The context a path names; Count for a path outside the four directories. */
	[[nodiscard]] Context context_of(StringView path, StringView root = "scripts") noexcept;

	/** One require() a script makes: what it wrote, and the module that names. */
	struct Import
	{
		String required{&memory::heap(MemoryTag::Scripting)}; // "@lib/moves", "./steer"
		String path{&memory::heap(MemoryTag::Scripting)};	  // "scripts/lib/moves.luau"
	};

	/**
	 * A script as hosts take it: compiled once by whoever read the text, so the server, the host's
	 * own client and a joiner that received the text over the wire hold the same bytes. The text hash
	 * is what the courier compares and the join checks; the bytecode is what runs.
	 */
	struct Source
	{
		String path{&memory::heap(MemoryTag::Scripting)}; // "scripts/sim/rules/sword.luau", as assets name it
		Context context = Context::Lib;
		Vector<u8> bytecode{&memory::heap(MemoryTag::Scripting)}; // Luau bytecode: optimization 2, debug 1
		u64 text_hash = 0;										   // hash_bytes() of the text
		Vector<Import> imports{&memory::heap(MemoryTag::Scripting)};
		Vector<Problem> lints{&memory::heap(MemoryTag::Scripting)}; // an Error among them refuses the module

		/** Whether a host may load it: it compiled, and no lint is an error. */
		[[nodiscard]] bool loadable() const noexcept;
	};

	/**
	 * The `aliases` of scripts/.luaurc, read as luau-lsp reads them, so a require Zed resolves is
	 * one the host resolves: "@lib/moves" is "scripts/lib/moves.luau" when the file says
	 * { "aliases": { "lib": "./lib" } }. Relative requires ("./steer", "../lib/x") resolve from the
	 * requiring file's directory.
	 */
	class Aliases final
	{
	public:
		Aliases() noexcept;

		/** From the .luaurc text, for scripts below `root` ("scripts"). An empty text is no aliases, and fine. */
		[[nodiscard]] static Result<Aliases, Problem> parse(StringView root, Span<const u8> luaurc) noexcept;

		/** A require string to a module path; false for one no alias or directory names. */
		[[nodiscard]] bool resolve(StringView required, StringView from, String& out) const noexcept;

		[[nodiscard]] u32 count() const noexcept { return static_cast<u32>(m_aliases.size()); }

	private:
		struct Alias
		{
			String name;	  // "lib"
			String directory; // "scripts/lib"
		};

		String m_root;
		Vector<Alias> m_aliases;
	};

	/**
	 * Parses, lints and compiles one file: pure, allocation-local, any thread, no VM. A text that
	 * does not parse or compile is the Problem; one that does is a Source, lints and all.
	 */
	[[nodiscard]] Result<Source, Problem> compile(StringView path, Span<const u8> text, const Aliases& aliases) noexcept;

	/**
	 * Every .luau below `<asset_root>/<root>` but the .d.luau definitions, sorted by path, compiled with
	 * the aliases of the .luaurc beside them: how a dedicated server and the tests load. Files that fail
	 * are reported in `problems` and left out.
	 */
	[[nodiscard]] Vector<Source> compile_directory(StringView asset_root, Vector<Problem>& problems,
												   StringView root = "scripts") noexcept;
}

namespace ember
{
	EMBER_ENUM_NAMES(script::Context, "Sim", "Server", "Client", "Lib");
}
