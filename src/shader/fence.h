#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>

#include <slang.h>

namespace ember::shader
{
	class Report;

	/**
	 * The engine's side of the authoring boundary: its shader directory and the modules directly in
	 * it. Its subdirectories hold material types, the error type and the stock library, which are
	 * authored and fenced like any other.
	 */
	struct EngineShaders
	{
		StringView dir;				// absolute, ending in '/'
		Span<const String> modules; // every module directly in `dir` except the authoring API

		[[nodiscard]] bool owns(StringView path) const noexcept;
		[[nodiscard]] bool has_module(StringView name) const noexcept;
	};

	/**
	 * Refuses any import of an engine module, other than the authoring API, in the files `module`
	 * read outside the engine: the author's file, its libraries and whatever they #include. Such an
	 * import would bring the heap and the scene tables back into scope. Slang reports which files a
	 * module read but not what they import, so the text is read here; `source` is the author's
	 * file, already in hand.
	 */
	void check_imports(slang::IModule* module, StringView file, StringView source, const EngineShaders& engine,
					   Report& report) noexcept;

	/**
	 * Refuses any global declared by an author module that `module` depends on, unless it is static:
	 * a material's inputs are its fields, and any other global would be a shader parameter the
	 * engine never binds. Reflection sees every declaration, #included ones too.
	 */
	void check_globals(slang::ISession* session, slang::IModule* module, const EngineShaders& engine,
					   Report& report) noexcept;
}
