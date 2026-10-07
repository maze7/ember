#include <ember/script/source.h>

#include <ember/core/filesystem.h>
#include <ember/core/hash.h>
#include <ember/core/json.h>
#include <ember/core/logger.h>

#include <Luau/Ast.h>
#include <Luau/BytecodeBuilder.h>
#include <Luau/Compiler.h>
#include <Luau/Parser.h>
#include <fmt/format.h>

#include <algorithm>
#include <cstring>

namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		[[nodiscard]] Problem make_problem(StringView path, const Luau::Location& location, Severity severity,
										   String message) noexcept
		{
			Problem problem;
			problem.path	 = String(path, &heap());
			problem.line	 = location.begin.line + 1;
			problem.column	 = location.begin.column + 1;
			problem.severity = severity;
			problem.message	 = std::move(message);
			return problem;
		}

		[[nodiscard]] StringView directory_of(StringView path) noexcept
		{
			const size_t slash = path.rfind('/');
			return slash == StringView::npos ? StringView() : path.substr(0, slash);
		}

		[[nodiscard]] StringView text_of(const Luau::AstArray<char>& value) noexcept { return {value.data, value.size}; }

		/** A segment path lexically normalised: "a/./b/../c" is "a/c". False when it climbs above the start. */
		[[nodiscard]] bool normalise(StringView base, StringView relative, String& out) noexcept
		{
			Vector<StringView> segments(&heap());
			const auto push = [&](StringView path)
			{
				size_t start = 0;
				while (start <= path.size())
				{
					const size_t slash	  = path.find('/', start);
					const StringView part = path.substr(start, slash == StringView::npos ? StringView::npos : slash - start);
					start				  = slash == StringView::npos ? path.size() + 1 : slash + 1;

					if (part.empty() || part == ".")
						continue;
					if (part == "..")
					{
						if (segments.empty())
							return false;
						segments.pop_back();
						continue;
					}
					segments.push_back(part);
				}
				return true;
			};

			if (!push(base) || !push(relative))
				return false;

			out.clear();
			for (const StringView segment : segments)
			{
				if (!out.empty())
					out += '/';
				out += segment;
			}
			return !out.empty();
		}

		/** The local an assignment lands on, through any chain of fields and indices: `a.b[c] = x` is `a`. */
		[[nodiscard]] Luau::AstLocal* root_local(Luau::AstExpr* expr) noexcept
		{
			for (;;)
			{
				if (auto* local = expr->as<Luau::AstExprLocal>())
					return local->local;
				if (auto* name = expr->as<Luau::AstExprIndexName>())
					expr = name->expr;
				else if (auto* index = expr->as<Luau::AstExprIndexExpr>())
					expr = index->expr;
				else
					return nullptr;
			}
		}

		constexpr StringView FORBIDDEN_EVERYWHERE[] = {"os", "io", "debug", "loadstring", "getfenv", "setfenv", "newproxy"};

		/**
		 * The lints, in one walk of the tree, and the imports. What they catch is what the netcode cannot
		 * forgive: state kept where a replay cannot restore it, and arithmetic that differs between
		 * machines. A sim script and a lib are held to all of them; a server script to the state rule; a
		 * client script hears about state as a warning, since nothing on a client is replayed.
		 */
		class Linter final : public Luau::AstVisitor
		{
		public:
			Linter(StringView path, const Aliases& aliases, Source& out) noexcept
				: m_path(path), m_aliases(aliases), m_out(out), m_strict(out.context == Context::Sim || out.context == Context::Lib),
				  m_module_locals(&heap()), m_functions(&heap())
			{
			}

			void run(Luau::AstStatBlock* root)
			{
				// What the chunk declares at its top level: the locals a function may not assign to later.
				for (Luau::AstStat* statement : root->body)
				{
					if (auto* local = statement->as<Luau::AstStatLocal>())
						for (Luau::AstLocal* var : local->vars)
							m_module_locals.push_back(var);
					else if (auto* function = statement->as<Luau::AstStatLocalFunction>())
						m_module_locals.push_back(function->name);
				}

				root->visit(this);
			}

			bool visit(Luau::AstExprFunction* function) override
			{
				m_functions.push_back(function->location);
				return true;
			}

			bool visit(Luau::AstStatAssign* assign) override
			{
				for (Luau::AstExpr* var : assign->vars)
					check_assignment(var, assign->location);
				return true;
			}

			bool visit(Luau::AstStatCompoundAssign* assign) override
			{
				check_assignment(assign->var, assign->location);
				return true;
			}

			bool visit(Luau::AstExprGlobal* global) override
			{
				const StringView name = global->name.value;
				for (const StringView forbidden : FORBIDDEN_EVERYWHERE)
					if (name == forbidden)
						lint(global->location, Severity::Error, fmt::format("'{}' is not available to scripts", name));

				if (name == "coroutine" && m_out.context != Context::Client)
					lint(global->location, Severity::Error,
						 "'coroutine' is not available here: a coroutine cannot be put back by a replay or a reload");

				return true;
			}

			bool visit(Luau::AstExprIndexName* index) override
			{
				if (auto* global = index->expr->as<Luau::AstExprGlobal>(); global != nullptr && m_strict)
				{
					const StringView library = global->name.value;
					const StringView member	 = index->index.value;
					if (library == "math" && (member == "random" || member == "randomseed"))
						lint(index->location, Severity::Error,
							 "math.random is the platform's, and no two machines roll alike: a sim script has no dice; put the chance in scripts/server");
				}
				return true;
			}

			bool visit(Luau::AstExprBinary* binary) override
			{
				if (binary->op == Luau::AstExprBinary::Pow && m_strict)
					lint(binary->location, Severity::Error,
						 "'^' calls the platform's pow, which differs between machines: use x * x, math.sqrt or math.pow");
				return true;
			}

			bool visit(Luau::AstExprCall* call) override
			{
				auto* global = call->func->as<Luau::AstExprGlobal>();
				if (global == nullptr || StringView(global->name.value) != "require")
					return true;

				auto* literal = call->args.size == 1 ? call->args.data[0]->as<Luau::AstExprConstantString>() : nullptr;
				if (literal == nullptr)
				{
					lint(call->location, Severity::Error, "require takes one string literal, so the host knows the module before it runs");
					return true;
				}

				const StringView required = text_of(literal->value);
				String resolved(&heap());
				if (!m_aliases.resolve(required, m_path, resolved))
				{
					lint(call->location, Severity::Error,
						 fmt::format("require('{}') names no module: it is '@alias/name' for an alias of scripts/.luaurc, or './name'", required));
					return true;
				}

				for (const Import& import : m_out.imports)
					if (import.path == resolved)
						return true;

				Import import;
				import.required = String(required, &heap());
				import.path		= std::move(resolved);
				m_out.imports.push_back(std::move(import));
				return true;
			}

		private:
			[[nodiscard]] bool inside_function(const Luau::Location& location) const noexcept
			{
				for (const Luau::Location& function : m_functions)
					if (function.encloses(location))
						return true;
				return false;
			}

			void check_assignment(Luau::AstExpr* var, const Luau::Location& location)
			{
				Luau::AstLocal* local = root_local(var);
				if (local == nullptr || !inside_function(location))
					return;
				if (std::find(m_module_locals.begin(), m_module_locals.end(), local) == m_module_locals.end())
					return;

				const Severity severity = m_out.context == Context::Client ? Severity::Warning : Severity::Error;
				lint(location, severity,
					 fmt::format("'{}' is a module local assigned inside a function: state a replay or a reload cannot put back. "
								 "Keep it in a component, or make it a constant",
								 local->name.value));
			}

			void lint(const Luau::Location& location, Severity severity, StringView message)
			{
				m_out.lints.push_back(make_problem(m_path, location, severity, String(message, &heap())));
			}

			StringView m_path;
			const Aliases& m_aliases;
			Source& m_out;
			bool m_strict;
			Vector<Luau::AstLocal*> m_module_locals;
			Vector<Luau::Location> m_functions;
		};
	}

	Context context_of(StringView path, StringView root) noexcept
	{
		if (!path.starts_with(root) || path.size() <= root.size() || path[root.size()] != '/')
			return Context::Count;

		const StringView rest	 = path.substr(root.size() + 1);
		const StringView segment = rest.substr(0, rest.find('/'));
		if (segment == "sim")
			return Context::Sim;
		if (segment == "server")
			return Context::Server;
		if (segment == "client")
			return Context::Client;
		if (segment == "lib")
			return Context::Lib;
		return Context::Count;
	}

	bool Source::loadable() const noexcept
	{
		if (bytecode.empty())
			return false;
		for (const Problem& lint : lints)
			if (lint.severity == Severity::Error)
				return false;
		return true;
	}

	Aliases::Aliases() noexcept : m_root(&heap()), m_aliases(&heap()) {}

	Result<Aliases, Problem> Aliases::parse(StringView root, Span<const u8> luaurc) noexcept
	{
		Aliases aliases;
		aliases.m_root = String(root, &heap());
		if (luaurc.empty())
			return aliases;

		const StringView text(reinterpret_cast<const char*>(luaurc.data()), luaurc.size());

		Json json;
		JsonError error;
		if (!json.parse(text, heap(), JsonRead::Relaxed, &error))
		{
			Problem problem;
			problem.path	= String(root, &heap()) + "/.luaurc";
			problem.line	= error.line;
			problem.column	= error.column;
			problem.message = String(error.message != nullptr ? error.message : "cannot parse", &heap());
			return fail(std::move(problem));
		}

		for (const auto [name, value] : json.root()["aliases"].members())
		{
			StringView directory;
			if (!value.read(directory))
				continue;

			// "./lib" and "lib" both mean the directory below the root; nothing may climb out of it.
			String resolved(&heap());
			if (!normalise(root, directory, resolved) || !resolved.starts_with(root))
			{
				Problem problem;
				problem.path	= String(root, &heap()) + "/.luaurc";
				problem.message = String(fmt::format("alias '{}' points outside the scripts: '{}'", name, directory), &heap());
				return fail(std::move(problem));
			}

			aliases.m_aliases.push_back({.name = String(name, &heap()), .directory = std::move(resolved)});
		}

		return aliases;
	}

	bool Aliases::resolve(StringView required, StringView from, String& out) const noexcept
	{
		String path(&heap());

		if (required.starts_with('@'))
		{
			const size_t slash	   = required.find('/');
			const StringView alias = required.substr(1, slash == StringView::npos ? StringView::npos : slash - 1);
			const StringView rest  = slash == StringView::npos ? StringView() : required.substr(slash + 1);

			const auto found = std::find_if(m_aliases.begin(), m_aliases.end(), [&](const Alias& a) { return a.name == alias; });
			if (found == m_aliases.end() || rest.empty() || !normalise(found->directory, rest, path))
				return false;
		}
		else if (required.starts_with("./") || required.starts_with("../"))
		{
			if (!normalise(directory_of(from), required, path) || !path.starts_with(m_root))
				return false;
		}
		else
		{
			return false;
		}

		if (!path.ends_with(".luau"))
			path += ".luau";
		out = std::move(path);
		return true;
	}

	Result<Source, Problem> compile(StringView path, Span<const u8> text, const Aliases& aliases) noexcept
	{
		Luau::Allocator allocator;
		Luau::AstNameTable names(allocator);
		const Luau::ParseResult parsed =
			Luau::Parser::parse(reinterpret_cast<const char*>(text.data()), text.size(), names, allocator, Luau::ParseOptions{});

		if (!parsed.errors.empty())
		{
			const Luau::ParseError& error = parsed.errors.front();
			return fail(make_problem(path, error.getLocation(), Severity::Error,
									 String(error.getMessage().data(), error.getMessage().size(), &heap())));
		}

		Source source;
		source.path		 = String(path, &heap());
		source.context	 = context_of(path);
		source.text_hash = hash_bytes(text);

		if (source.context == Context::Count)
		{
			Problem problem;
			problem.path	= source.path;
			problem.message = String("a script lives under scripts/sim, scripts/server, scripts/client or scripts/lib: the directory is where it runs", &heap());
			return fail(std::move(problem));
		}

		Linter(path, aliases, source).run(parsed.root);

		Luau::CompileOptions options;
		options.optimizationLevel = 2;
		options.debugLevel		  = 1;
		options.vectorLib		  = "vector";
		options.vectorCtor		  = "create";
		options.vectorType		  = "vector";

		Luau::BytecodeBuilder bytecode;
		try
		{
			Luau::compileOrThrow(bytecode, parsed, names, options);
		}
		catch (const Luau::CompileError& error)
		{
			return fail(make_problem(path, error.getLocation(), Severity::Error, String(error.what(), &heap())));
		}

		const std::string& bytes = bytecode.getBytecode();
		source.bytecode.assign(reinterpret_cast<const u8*>(bytes.data()), reinterpret_cast<const u8*>(bytes.data()) + bytes.size());
		return source;
	}

	namespace
	{
		/** Every file below `directory`, named `prefix/<path below>`, in no particular order. */
		Result<void, fs::FileError> walk(StringView directory, StringView prefix, Vector<String>& out) noexcept
		{
			Vector<String> subdirectories(&heap());

			const auto visited = fs::enumerate(
				directory,
				[&](const fs::DirectoryEntry& entry) noexcept
				{
					String name(prefix, &heap());
					name += '/';
					name += entry.name;

					if (entry.type == fs::FileType::Directory)
						subdirectories.push_back(std::move(name));
					else if (entry.type == fs::FileType::Regular)
						out.push_back(std::move(name));

					return fs::Visit::Continue;
				});
			if (!visited)
				return visited;

			for (const String& subdirectory : subdirectories)
			{
				String path(&heap());
				if (const auto joined = fs::join(path, directory, StringView(subdirectory).substr(prefix.size() + 1)); !joined)
					return joined;
				if (const auto walked = walk(path, subdirectory, out); !walked)
					return walked;
			}

			return {};
		}
	}

	Vector<Source> compile_directory(StringView asset_root, Vector<Problem>& problems, StringView root) noexcept
	{
		Vector<Source> sources(&heap());

		String directory(&heap());
		if (const auto joined = fs::join(directory, asset_root, root); !joined)
			return sources;

		// The aliases beside the scripts, when there is a .luaurc; none is fine.
		Aliases aliases;
		{
			String luaurc(&heap());
			(void)fs::join(luaurc, directory, ".luaurc");
			if (const auto read = fs::read_file(luaurc, heap()))
			{
				auto parsed = Aliases::parse(root, read->bytes());
				if (!parsed)
					problems.push_back(std::move(parsed.error()));
				else
					aliases = std::move(parsed.value());
			}
		}

		Vector<String> names(&heap());
		if (const auto walked = walk(directory, root, names); !walked)
		{
			Problem problem;
			problem.path	= String(root, &heap());
			problem.message = String(fmt::format("cannot list '{}': {}", StringView(directory), enum_name(walked.error().code)), &heap());
			problems.push_back(std::move(problem));
			return sources;
		}

		std::sort(names.begin(), names.end());

		for (const String& name : names)
		{
			// Scripts, not the definitions the host writes beside them for the editor.
			if (!name.ends_with(".luau") || name.ends_with(".d.luau"))
				continue;

			String file(&heap());
			(void)fs::join(file, asset_root, name);

			const auto read = fs::read_file(file, heap());
			if (!read)
			{
				Problem problem;
				problem.path	= name;
				problem.message = String(fmt::format("cannot read: {}", enum_name(read.error().code)), &heap());
				problems.push_back(std::move(problem));
				continue;
			}

			auto compiled = compile(name, read->bytes(), aliases);
			if (!compiled)
			{
				problems.push_back(std::move(compiled.error()));
				continue;
			}

			for (const Problem& lint : compiled->lints)
				problems.push_back(lint);
			sources.push_back(std::move(compiled.value()));
		}

		return sources;
	}
}
