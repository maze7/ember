#include <ember/core/filesystem.h>
#include <ember/memory/memory.h>

#include <shader/fence.h>
#include <shader/report.h>

#include <algorithm>

namespace ember::shader
{
	namespace
	{
		/// The one engine module a material file may import.
		constexpr StringView AUTHORING_MODULE = "material";

		struct Import
		{
			StringView module; // the imported path's last component, which names the module
			u32 line = 0;
		};

		[[nodiscard]] constexpr bool is_identifier(char c) noexcept
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		}

		/**
		 * The imports in a Slang source and the line each is on. `import` is a keyword, so every
		 * occurrence outside a comment or a string literal starts an import; the name after it is a
		 * dotted path or a quoted file, and only its last component names the module.
		 */
		void scan_imports(StringView text, Vector<Import>& out) noexcept
		{
			const size_t size = text.size();
			size_t at		  = 0;
			u32 line		  = 1;

			while (at < size)
			{
				const char c = text[at];

				if (c == '\n')
				{
					++line;
					++at;
				}
				else if (c == '/' && at + 1 < size && text[at + 1] == '/')
				{
					while (at < size && text[at] != '\n')
						++at;
				}
				else if (c == '/' && at + 1 < size && text[at + 1] == '*')
				{
					for (at += 2; at < size && !(text[at] == '*' && at + 1 < size && text[at + 1] == '/'); ++at)
						line += text[at] == '\n';
					at += 2;
				}
				else if (c == '"')
				{
					for (++at; at < size && text[at] != '"'; ++at)
					{
						if (text[at] == '\\')
							++at;
						else
							line += text[at] == '\n';
					}
					++at;
				}
				else if (is_identifier(c))
				{
					const size_t start = at;
					while (at < size && is_identifier(text[at]))
						++at;

					if (text.substr(start, at - start) != "import")
						continue;

					const u32 import_line = line;
					while (at < size && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
						line += text[at++] == '\n';

					StringView name;
					if (at < size && text[at] == '"')
					{
						const size_t begin = ++at;
						while (at < size && text[at] != '"' && text[at] != '\n')
							++at;
						name = fs::stem(text.substr(begin, at - begin));
						++at;
					}
					else
					{
						const size_t begin = at;
						while (at < size && (is_identifier(text[at]) || text[at] == '.'))
							++at;
						name = text.substr(begin, at - begin);

						if (const size_t dot = name.rfind('.'); dot != StringView::npos)
							name.remove_prefix(dot + 1);
					}

					if (!name.empty())
						out.push_back({name, import_line});
				}
				else
				{
					++at;
				}
			}
		}

		[[nodiscard]] bool depends_on(slang::IModule* module, StringView path) noexcept
		{
			for (SlangInt32 i = 0; i < module->getDependencyFileCount(); ++i)
				if (path == module->getDependencyFilePath(i))
					return true;

			return false;
		}

		void check_scope(slang::DeclReflection* scope, StringView path, Report& report) noexcept
		{
			for (u32 i = 0; i < scope->getChildrenCount(); ++i)
			{
				slang::DeclReflection* decl = scope->getChild(i);

				if (decl->getKind() == slang::DeclReflection::Kind::Namespace)
					check_scope(decl, path, report);
				else if (decl->getKind() == slang::DeclReflection::Kind::Variable &&
						 decl->asVariable()->findModifier(slang::Modifier::Static) == nullptr)
					report.error(path,
								 "declares the global '{}'; a material's inputs are its fields, so a constant is "
								 "`static const` and anything else is not allowed",
								 decl->getName());
			}
		}
	}

	bool EngineShaders::owns(StringView path) const noexcept
	{
		if (path.size() <= dir.size())
			return false;

		// Slang reports Windows paths with either separator, so they fold before comparing.
		for (size_t i = 0; i < dir.size(); ++i)
			if ((path[i] == '\\' ? '/' : path[i]) != (dir[i] == '\\' ? '/' : dir[i]))
				return false;

		return path.find_first_of("/\\", dir.size()) == StringView::npos;
	}

	bool EngineShaders::has_module(StringView name) const noexcept
	{
		return std::find(modules.begin(), modules.end(), name) != modules.end();
	}

	void check_imports(slang::IModule* module, StringView file, StringView source, const EngineShaders& engine,
					   Report& report) noexcept
	{
		Vector<Import> imports(&memory::heap(MemoryTag::Tools));

		for (SlangInt32 i = 0; i < module->getDependencyFileCount(); ++i)
		{
			const StringView path = module->getDependencyFilePath(i);

			if (engine.owns(path))
				continue;

			fs::FileData data;
			StringView text = source;

			if (path != file)
			{
				auto read = fs::read_file(path, memory::heap(MemoryTag::Tools));

				// Gone since Slang read it: the compiler's stamps replace the session next time.
				if (!read)
					continue;

				data = std::move(read.value());
				text = data.text();
			}

			imports.clear();
			scan_imports(text, imports);

			for (const Import& import : imports)
				if (engine.has_module(import.module))
					report.error_at(path, import.line,
									"imports '{}', an engine module; material files and their libraries may import "
									"'{}' and each other",
									import.module, AUTHORING_MODULE);
		}
	}

	void check_globals(slang::ISession* session, slang::IModule* module, const EngineShaders& engine,
					   Report& report) noexcept
	{
		// Only the modules this compile read: a warm session also holds earlier types, which were
		// judged when they compiled.
		for (SlangInt i = 0; i < session->getLoadedModuleCount(); ++i)
		{
			slang::IModule* loaded = session->getLoadedModule(i);
			const char* path	   = loaded->getFilePath();

			if (path != nullptr && !engine.owns(path) && depends_on(module, path))
				check_scope(loaded->getModuleReflection(), path, report);
		}
	}
}
