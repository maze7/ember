#include <ember/core/filesystem.h>
#include <ember/script/source.h>

#include <gtest/gtest.h>

#include <string>

#if defined(EMBER_PLATFORM_WINDOWS)
	#include <process.h>
#else
	#include <unistd.h>
#endif

namespace
{
	using namespace ember;
	using namespace ember::script;

	[[nodiscard]] Span<const u8> bytes_of(StringView text) noexcept
	{
		return {reinterpret_cast<const u8*>(text.data()), text.size()};
	}

	[[nodiscard]] Aliases aliases()
	{
		auto parsed = Aliases::parse("scripts", bytes_of(R"({ "aliases": { "lib": "./lib", "rules": "./sim/rules" } })"));
		EXPECT_TRUE(parsed.has_value());
		return parsed ? std::move(*parsed) : Aliases();
	}

	[[nodiscard]] Result<Source, Problem> make(StringView path, StringView text) { return compile(path, bytes_of(text), aliases()); }

	[[nodiscard]] bool has_lint(const Source& source, Severity severity, StringView fragment)
	{
		for (const Problem& lint : source.lints)
			if (lint.severity == severity && StringView(lint.message).find(fragment) != StringView::npos)
				return true;
		return false;
	}

	TEST(SourceCompile, GivesBytecodeAContextAndAHash)
	{
		const auto source = make("scripts/sim/rules/sword.luau", "local x = 1\nreturn x");
		ASSERT_TRUE(source.has_value());
		EXPECT_FALSE(source->bytecode.empty());
		EXPECT_EQ(source->context, Context::Sim);
		EXPECT_NE(source->text_hash, 0u);
		EXPECT_TRUE(source->loadable());
		EXPECT_TRUE(source->lints.empty());

		const auto same = make("scripts/sim/rules/sword.luau", "local x = 1\nreturn x");
		ASSERT_TRUE(same.has_value());
		EXPECT_EQ(same->bytecode, source->bytecode);
		EXPECT_EQ(same->text_hash, source->text_hash);
	}

	TEST(SourceCompile, ContextsFollowTheDirectory)
	{
		EXPECT_EQ(context_of("scripts/sim/a.luau"), Context::Sim);
		EXPECT_EQ(context_of("scripts/server/a.luau"), Context::Server);
		EXPECT_EQ(context_of("scripts/client/ui/a.luau"), Context::Client);
		EXPECT_EQ(context_of("scripts/lib/a.luau"), Context::Lib);
		EXPECT_EQ(context_of("scripts/a.luau"), Context::Count);
		EXPECT_EQ(context_of("other/sim/a.luau"), Context::Count);
		EXPECT_EQ(context_of("mods/sim/a.luau", "mods"), Context::Sim);

		const auto outside = make("scripts/a.luau", "return 1");
		EXPECT_FALSE(outside.has_value());
	}

	TEST(SourceCompile, AParseErrorIsAProblemWithItsPlace)
	{
		const auto source = make("scripts/sim/a.luau", "local x =\nlocal y = (");
		ASSERT_FALSE(source.has_value());
		EXPECT_EQ(source.error().path, "scripts/sim/a.luau");
		EXPECT_GE(source.error().line, 1u);
		EXPECT_EQ(source.error().severity, Severity::Error);
		EXPECT_FALSE(source.error().message.empty());
	}

	TEST(SourceLint, ForbiddenGlobalsAreErrorsEverywhere)
	{
		for (const char* path : {"scripts/sim/a.luau", "scripts/server/a.luau", "scripts/client/a.luau", "scripts/lib/a.luau"})
		{
			const auto source = make(path, "local t = os.time()\nlocal f = loadstring('x')\nreturn t");
			ASSERT_TRUE(source.has_value()) << path;
			EXPECT_TRUE(has_lint(*source, Severity::Error, "'os'")) << path;
			EXPECT_TRUE(has_lint(*source, Severity::Error, "'loadstring'")) << path;
			EXPECT_FALSE(source->loadable()) << path;
		}
	}

	TEST(SourceLint, CoroutinesAreForClientsAlone)
	{
		const auto sim = make("scripts/sim/a.luau", "local c = coroutine.create(function() end)\nreturn c");
		ASSERT_TRUE(sim.has_value());
		EXPECT_TRUE(has_lint(*sim, Severity::Error, "coroutine"));

		const auto client = make("scripts/client/a.luau", "local c = coroutine.create(function() end)\nreturn c");
		ASSERT_TRUE(client.has_value());
		EXPECT_TRUE(client->lints.empty());
	}

	TEST(SourceLint, PlatformRandomAndPowAreRefusedWhereEveryMachineMustAgree)
	{
		const auto sim = make("scripts/sim/a.luau", "local r = math.random()\nlocal p = 2 ^ 3\nreturn r + p");
		ASSERT_TRUE(sim.has_value());
		EXPECT_TRUE(has_lint(*sim, Severity::Error, "math.random"));
		EXPECT_TRUE(has_lint(*sim, Severity::Error, "'^'"));

		const auto lib = make("scripts/lib/a.luau", "return function() return math.random() end");
		ASSERT_TRUE(lib.has_value());
		EXPECT_TRUE(has_lint(*lib, Severity::Error, "math.random"));

		const auto server = make("scripts/server/a.luau", "local r = math.random()\nlocal p = 2 ^ 3\nreturn r + p");
		ASSERT_TRUE(server.has_value());
		EXPECT_TRUE(server->lints.empty());
	}

	TEST(SourceLint, ModuleLocalsAssignedInFunctionsAreState)
	{
		const char* text = "local count = 0\nlocal function bump() count += 1 end\nreturn bump";

		const auto sim = make("scripts/sim/a.luau", text);
		ASSERT_TRUE(sim.has_value());
		EXPECT_TRUE(has_lint(*sim, Severity::Error, "'count'"));
		EXPECT_FALSE(sim->loadable());

		const auto client = make("scripts/client/a.luau", text);
		ASSERT_TRUE(client.has_value());
		EXPECT_TRUE(has_lint(*client, Severity::Warning, "'count'"));
		EXPECT_TRUE(client->loadable());

		// Reading one is fine, and so is a local inside the function.
		const auto fine = make("scripts/sim/b.luau", "local LIMIT = 3\nlocal function f() local n = 0 n += LIMIT return n end\nreturn f");
		ASSERT_TRUE(fine.has_value());
		EXPECT_TRUE(fine->lints.empty());
	}

	TEST(SourceLint, RequiresResolveThroughAliasesAndRelativePaths)
	{
		const auto source = make("scripts/sim/rules/sword.luau",
								 "local moves = require(\"@lib/moves\")\n"
								 "local steer = require(\"./steer\")\n"
								 "local up = require(\"../common\")\n"
								 "local twice = require(\"@lib/moves\")\n"
								 "return { moves, steer, up, twice }");
		ASSERT_TRUE(source.has_value());
		EXPECT_TRUE(source->lints.empty());
		ASSERT_EQ(source->imports.size(), 3u);
		EXPECT_EQ(source->imports[0].required, "@lib/moves");
		EXPECT_EQ(source->imports[0].path, "scripts/lib/moves.luau");
		EXPECT_EQ(source->imports[1].required, "./steer");
		EXPECT_EQ(source->imports[1].path, "scripts/sim/rules/steer.luau");
		EXPECT_EQ(source->imports[2].required, "../common");
		EXPECT_EQ(source->imports[2].path, "scripts/sim/common.luau");
	}

	TEST(SourceLint, ARequireTheHostCannotResolveIsAnError)
	{
		const auto dynamic = make("scripts/sim/a.luau", "local name = 'x'\nlocal m = require(name)\nreturn m");
		ASSERT_TRUE(dynamic.has_value());
		EXPECT_TRUE(has_lint(*dynamic, Severity::Error, "require"));

		const auto unknown = make("scripts/sim/a.luau", "local m = require(\"@nowhere/x\")\nreturn m");
		ASSERT_TRUE(unknown.has_value());
		EXPECT_TRUE(has_lint(*unknown, Severity::Error, "require"));

		const auto climbing = make("scripts/sim/a.luau", "local m = require(\"../../secrets\")\nreturn m");
		ASSERT_TRUE(climbing.has_value());
		EXPECT_TRUE(has_lint(*climbing, Severity::Error, "require"));
	}

	TEST(SourceAliases, ParseReadsRelaxedJsonAndKeepsToTheRoot)
	{
		const auto fine = Aliases::parse("scripts", bytes_of("{\n  // the libs\n  \"aliases\": { \"lib\": \"./lib\", },\n}"));
		ASSERT_TRUE(fine.has_value());
		EXPECT_EQ(fine->count(), 1u);

		String out(&memory::heap(MemoryTag::Scripting));
		EXPECT_TRUE(fine->resolve("@lib/a/b", "scripts/sim/x.luau", out));
		EXPECT_EQ(out, "scripts/lib/a/b.luau");
		EXPECT_FALSE(fine->resolve("@other/x", "scripts/sim/x.luau", out));
		EXPECT_TRUE(fine->resolve("./y", "scripts/sim/x.luau", out));
		EXPECT_EQ(out, "scripts/sim/y.luau");
		EXPECT_FALSE(fine->resolve("../../y", "scripts/sim/x.luau", out));

		const auto empty = Aliases::parse("scripts", {});
		ASSERT_TRUE(empty.has_value());
		EXPECT_EQ(empty->count(), 0u);

		const auto escaping = Aliases::parse("scripts", bytes_of(R"({ "aliases": { "up": "../elsewhere" } })"));
		EXPECT_FALSE(escaping.has_value());

		const auto broken = Aliases::parse("scripts", bytes_of("{ aliases: "));
		EXPECT_FALSE(broken.has_value());
	}

	TEST(SourceDirectory, CompilesEveryScriptBelowTheRootInPathOrder)
	{
		String scratch(&memory::heap(MemoryTag::Engine));
		ASSERT_TRUE(fs::temporary_directory(scratch).has_value());
		ASSERT_TRUE(fs::join(scratch, scratch, "ember_script_source_tests_" + std::to_string(getpid())).has_value());
		(void)fs::remove_tree(scratch);
		ASSERT_TRUE(fs::create_directories(scratch + "/scripts/sim/rules").has_value());
		ASSERT_TRUE(fs::create_directories(scratch + "/scripts/lib").has_value());

		const auto write = [&](const char* name, StringView text)
		{ ASSERT_TRUE(fs::write_file(scratch + "/" + name, bytes_of(text)).has_value()); };
		write("scripts/.luaurc", R"({ "aliases": { "lib": "./lib" } })");
		write("scripts/lib/moves.luau", "return { dash = 2 }");
		write("scripts/sim/rules/sword.luau", "local moves = require('@lib/moves')\nreturn moves");
		write("scripts/sim/broken.luau", "local x = (");
		write("scripts/sim/notes.txt", "not a script");

		Vector<Problem> problems(&memory::heap(MemoryTag::Scripting));
		Vector<Source> sources = compile_directory(scratch, problems);
		(void)fs::remove_tree(scratch);

		ASSERT_EQ(sources.size(), 2u);
		EXPECT_EQ(sources[0].path, "scripts/lib/moves.luau");
		EXPECT_EQ(sources[0].context, Context::Lib);
		EXPECT_EQ(sources[1].path, "scripts/sim/rules/sword.luau");
		ASSERT_EQ(sources[1].imports.size(), 1u);
		EXPECT_EQ(sources[1].imports[0].path, "scripts/lib/moves.luau");

		ASSERT_EQ(problems.size(), 1u);
		EXPECT_EQ(problems[0].path, "scripts/sim/broken.luau");
	}
}
