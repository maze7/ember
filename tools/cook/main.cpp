/*
 * ember_cook: the build's way from a Slang file to the files the engine loads. It runs the same
 * shader::Compiler that hot reload runs, so a shader cooked at build time and one compiled at run
 * time come from one set of options and are the same bytes.
 *
 *     ember_cook program  <source.slang> -o <output.spv> [--depfile <file>] [-I <dir>]...
 *     ember_cook material <source.slang> -o <output.spv> [--depfile <file>] [-I <dir>]...
 *
 * A material also writes its .type beside the .spv. Exit status: 0 cooked, 1 the file did not
 * cook, 2 the command line is wrong.
 */

#include <ember/core/filesystem.h>
#include <ember/memory/memory.h>
#include <ember/shader/compiler.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstdio>

namespace
{
	using namespace ember;

	constexpr const char* USAGE = "usage: ember_cook <program|material> <source.slang> -o <output.spv> "
								  "[--depfile <file>] [-I <dir>]...\n";

	enum class Kind : u8
	{
		Program,
		Material,
	};

	struct Options
	{
		Kind kind = Kind::Program;
		StringView source;
		StringView output;
		StringView depfile;
		Vector<const char*> include_dirs;
	};

	[[nodiscard]] Heap& tools() noexcept { return memory::heap(MemoryTag::Tools); }

	[[nodiscard]] Span<const u8> as_bytes(StringView text) noexcept
	{
		return {reinterpret_cast<const u8*>(text.data()), text.size()};
	}

	/// The tool's own errors, worded like the compiler's so a build log reads as one voice. False, so
	/// a failing step can return it.
	template <class... Args> bool complain(fmt::format_string<Args...> format, Args&&... args) noexcept
	{
		std::fputs("ember_cook: error: ", stderr);
		fmt::print(stderr, format, std::forward<Args>(args)...);
		std::fputc('\n', stderr);
		return false;
	}

	[[nodiscard]] bool parse(int argc, char** argv, Options& out) noexcept
	{
		if (argc < 3)
			return false;

		const StringView kind = argv[1];

		if (kind == "program")
			out.kind = Kind::Program;
		else if (kind == "material")
			out.kind = Kind::Material;
		else
			return false;

		out.source = argv[2];

		for (int i = 3; i < argc; ++i)
		{
			const StringView option = argv[i];

			if (i + 1 == argc)
				return false; // every option takes a value

			if (option == "-o")
				out.output = argv[++i];
			else if (option == "--depfile")
				out.depfile = argv[++i];
			else if (option == "-I")
				out.include_dirs.push_back(argv[++i]);
			else
				return false;
		}

		return !out.output.empty();
	}

	/**
	 * Replaces `path` with `bytes` unless it already holds exactly them. An unchanged file keeps its
	 * timestamp, and since the build restats what a custom command wrote, everything downstream (the
	 * embed, its compile, every relink) is skipped: an edit that changes no output, a comment or a
	 * rebuilt tool, costs one cook. The replace is atomic, so an interrupted cook never leaves a
	 * truncated file with a fresh timestamp that the next build would trust. Durability, an fsync per
	 * file, buys nothing here: a build output lost to a power cut is cooked again.
	 */
	[[nodiscard]] bool write_if_changed(StringView path, Span<const u8> bytes) noexcept
	{
		if (const auto existing = fs::read_file(path, tools()))
		{
			const Span<const u8> old = existing->bytes();

			if (std::equal(old.begin(), old.end(), bytes.begin(), bytes.end()))
				return true;
		}

		if (const auto written = fs::write_file_atomic(path, bytes, fs::WriteDurability::None); !written)
			return complain("cannot write '{}' ({})", path, enum_name(written.error().code));

		return true;
	}

	/// A path in a Makefile rule, escaped the way GCC escapes its own: otherwise a space would end
	/// the path, a '#' start a comment and a '$' a variable.
	void append_path(String& rule, StringView path) noexcept
	{
		for (const char c : path)
		{
			if (c == ' ' || c == '#')
				rule += '\\';
			else if (c == '$')
				rule += '$';

			rule += c;
		}
	}

	/**
	 * Everything a successful compile produced: the bytecode, then a material's .type beside it, and
	 * last the depfile. The depfile is a Makefile rule naming the output and every file the compile
	 * read, which Ninja and Make load after the command, so an edit to any of them, an engine module
	 * or a game library, recooks exactly the files that read it.
	 */
	[[nodiscard]] bool write_cooked(const Options& options, Span<const u8> bytecode, StringView type_file,
									Span<const String> dependencies) noexcept
	{
		if (const StringView dir = fs::parent(options.output); !dir.empty())
			if (const auto created = fs::create_directories(dir); !created)
				return complain("cannot create '{}' ({})", dir, enum_name(created.error().code));

		if (!write_if_changed(options.output, bytecode))
			return false;

		if (!type_file.empty())
		{
			const StringView base =
				options.output.substr(0, options.output.size() - fs::extension(options.output).size());

			String path(base, &tools());
			path += material::TYPE_EXTENSION;

			if (!write_if_changed(path, as_bytes(type_file)))
				return false;
		}

		if (options.depfile.empty())
			return true;

		String rule(&tools());
		append_path(rule, options.output);
		rule += ':';

		for (const String& file : dependencies)
		{
			rule += " \\\n  ";
			append_path(rule, file);
		}

		rule += '\n';
		return write_if_changed(options.depfile, as_bytes(rule));
	}

	[[nodiscard]] bool cook(const Options& options) noexcept
	{
		// A material's pair shares a stem, so whoever loads the .type finds the bytecode beside it.
		if (options.kind == Kind::Material && fs::extension(options.output) != material::SPIRV_EXTENSION)
			return complain("a material's bytecode is its {} file, and its .type goes beside it; '{}' is not",
							material::SPIRV_EXTENSION, options.output);

		shader::Compiler compiler;

		if (!compiler.initialize({.include_dirs = {options.include_dirs.data(), options.include_dirs.size()}}))
			return false; // it logged why

		const auto source = fs::read_file(options.source, tools());

		if (!source)
			return complain("cannot read '{}' ({})", options.source, enum_name(source.error().code));

		String diagnostics(&tools());
		shader::Program program;
		material::Type type;

		const bool compiled = options.kind == Kind::Program
								  ? compiler.compile_program(options.source, source->text(), program, diagnostics)
								  : compiler.compile_material(options.source, source->text(), type, diagnostics);

		// Warnings as well as errors, as the compiler wrote them: each names the author's file and
		// line, never the tool, and that is what the build log and the IDE show.
		std::fputs(diagnostics.c_str(), stderr);

		if (!compiled)
			return false;

		if (options.kind == Kind::Program)
			return write_cooked(options, program.bytecode(), {}, program.dependencies);

		String type_file(&tools());

		if (!material::write_type(type, type_file))
			return complain("cannot describe '{}' as a .type file", type.name);

		return write_cooked(options, type.bytecode(), type_file, type.dependencies);
	}

	/// Everything the tool allocates lives in here, so it is released before memory reports leaks.
	[[nodiscard]] int run(int argc, char** argv) noexcept
	{
		Options options;

		if (!parse(argc, argv, options))
		{
			std::fputs(USAGE, stderr);
			return 2;
		}

		return cook(options) ? 0 : 1;
	}
}

int main(int argc, char** argv)
{
	if (!ember::memory::initialize())
	{
		std::fputs("ember_cook: error: memory did not initialize\n", stderr);
		return 1;
	}

	const int result = run(argc, argv);
	ember::memory::shutdown();
	return result;
}
