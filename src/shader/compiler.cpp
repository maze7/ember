#include <ember/core/filesystem.h>
#include <ember/core/logger.h>
#include <ember/memory/memory.h>
#include <ember/shader/compiler.h>

#include <shader/fence.h>
#include <shader/reflection.h>
#include <shader/report.h>

#include <slang-com-ptr.h>

#include <algorithm>
#include <cstring>

#ifndef EMBER_SHADER_DIR
	#error "EMBER_SHADER_DIR names the engine's shader directory; src/shader/CMakeLists.txt defines it"
#endif

namespace ember::shader
{
	using Slang::ComPtr;

	namespace
	{
		/// Each domain's entry module, and the interface a type implements to belong to it.
		constexpr const char* ENTRY_MODULES[]	  = {"surface", "screen"};
		constexpr const char* DOMAIN_INTERFACES[] = {"IMaterial", "IScreenMaterial"};

		static_assert(std::size(ENTRY_MODULES) == static_cast<size_t>(material::Domain::Count));
		static_assert(std::size(DOMAIN_INTERFACES) == static_cast<size_t>(material::Domain::Count));

		/// Where shading models live, and what makes a struct one.
		constexpr const char* SHADING_MODULE	= "shading";
		constexpr const char* SHADING_INTERFACE = "IShadingModel";

		[[nodiscard]] Heap& tools() noexcept { return memory::heap(MemoryTag::Tools); }

		[[nodiscard]] bool equal_ignoring_case(StringView a, StringView b) noexcept
		{
			const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };

			return a.size() == b.size() &&
				   std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
		}

		void add_dependencies(slang::IModule* module, Vector<String>& out) noexcept
		{
			for (SlangInt32 i = 0; i < module->getDependencyFileCount(); ++i)
			{
				const StringView path = module->getDependencyFilePath(i);

				if (std::find(out.begin(), out.end(), path) == out.end())
					out.emplace_back(path);
			}
		}
	}

	struct Compiler::Impl
	{
		/// A file the session has read, as it was then. The session is good while every one matches.
		struct Stamp
		{
			String path;
			u64 size		= 0;
			i64 modified_ns = 0;
		};

		Impl() noexcept
			: engine_dir(&tools()), engine_modules(&tools()), search_paths(&tools()), default_shading(&tools()),
			  shading_models(&tools()), stamps(&tools())
		{
		}

		ComPtr<slang::IGlobalSession> global;
		SlangCapabilityID demote = SLANG_CAPABILITY_UNKNOWN;

		String engine_dir;			   // absolute, ending in '/'
		Vector<String> engine_modules; // every module directly in engine_dir but the authoring API
		Vector<String> search_paths;   // engine_dir, then the include directories
		String default_shading;

		// Everything below belongs to one session and goes with it.
		ComPtr<slang::ISession> session;
		slang::IModule* entry_modules[static_cast<size_t>(material::Domain::Count)] = {};
		Vector<String> shading_models; // the IShadingModel structs in shading.slang
		Vector<Stamp> stamps;		   // every file the session has read
		u32 serial = 0;				   // numbers module names, which a session needs unique
		bool stale = true;			   // the next compile opens a new session

		[[nodiscard]] EngineShaders engine() const noexcept
		{
			return {engine_dir, {engine_modules.data(), engine_modules.size()}};
		}

		[[nodiscard]] bool has_loaded(StringView path) const noexcept
		{
			for (SlangInt i = 0; i < session->getLoadedModuleCount(); ++i)
				if (const char* loaded = session->getLoadedModule(i)->getFilePath();
					loaded != nullptr && path == loaded)
					return true;

			return false;
		}

		[[nodiscard]] bool current() const noexcept
		{
			for (const Stamp& stamp : stamps)
			{
				const auto info = fs::status(stamp.path);

				if (!info || info->size != stamp.size || info->modified_time_ns != stamp.modified_ns)
					return false;
			}

			return true;
		}

		/// Every file the session has read: each loaded module's dependencies, which cover what it
		/// #included as well as what it imported.
		void record_stamps() noexcept
		{
			stamps.clear();

			for (SlangInt i = 0; session && i < session->getLoadedModuleCount(); ++i)
			{
				slang::IModule* module = session->getLoadedModule(i);

				for (SlangInt32 d = 0; d < module->getDependencyFileCount(); ++d)
				{
					const StringView path = module->getDependencyFilePath(d);

					if (std::any_of(stamps.begin(), stamps.end(),
									[&](const Stamp& stamp) { return stamp.path == path; }))
						continue;

					// A source with no file behind it (the link module) cannot go stale.
					if (const auto info = fs::status(path))
						stamps.push_back({String(path, &tools()), info->size, info->modified_time_ns});
				}
			}
		}

		/// The warm session, unless it has loaded `path` before or a file it read has changed since;
		/// then a new one.
		[[nodiscard]] bool open_session(StringView path, Report& report) noexcept
		{
			if (session && !stale && !has_loaded(path) && current())
				return true;

			session = nullptr;
			std::fill(std::begin(entry_modules), std::end(entry_modules), nullptr);
			shading_models.clear();
			stamps.clear();
			serial = 0;
			stale  = false;

			// The one contract for every Slang file the engine compiles, so plain programs and
			// material types agree on matrix layout, entry point names and what discard means.
			slang::CompilerOptionEntry options[4] = {};

			options[0].name				  = slang::CompilerOptionName::MatrixLayoutColumn;
			options[0].value.kind		  = slang::CompilerOptionValueKind::Int;
			options[0].value.intValue0	  = 1;
			options[1].name				  = slang::CompilerOptionName::VulkanUseEntryPointName;
			options[1].value.kind		  = slang::CompilerOptionValueKind::Int;
			options[1].value.intValue0	  = 1;
			options[2].name				  = slang::CompilerOptionName::Capability;
			options[2].value.kind		  = slang::CompilerOptionValueKind::Int;
			options[2].value.intValue0	  = demote;
			options[3].name				  = slang::CompilerOptionName::DisableWarnings;
			options[3].value.kind		  = slang::CompilerOptionValueKind::String;
			options[3].value.stringValue0 = "39001,41012";

			Vector<const char*> paths(&tools());
			for (const String& path : search_paths)
				paths.push_back(path.c_str());

			slang::TargetDesc target{};
			target.format = SLANG_SPIRV;

			slang::SessionDesc desc{};
			desc.targets				  = &target;
			desc.targetCount			  = 1;
			desc.searchPaths			  = paths.data();
			desc.searchPathCount		  = static_cast<SlangInt>(paths.size());
			desc.compilerOptionEntries	  = options;
			desc.compilerOptionEntryCount = static_cast<u32>(std::size(options));

			if (SLANG_FAILED(global->createSession(desc, session.writeRef())))
			{
				stale = true;
				report.error(engine_dir, "libslang could not open a session");
				return false;
			}

			return true;
		}

		/// Loads `source` as a module of this session under a name unique to it. Slang wants the
		/// text null terminated.
		[[nodiscard]] slang::IModule* load(const char* prefix, const String& file, StringView source,
										   Report& report) noexcept
		{
			char name[24] = {};
			fmt::format_to_n(name, sizeof(name) - 1, "{}_{}", prefix, serial++);

			const String text(source, &tools());
			ComPtr<slang::IBlob> diagnostics;

			slang::IModule* module =
				session->loadModuleFromSourceString(name, file.c_str(), text.c_str(), diagnostics.writeRef());
			report.forward(diagnostics);

			// A session keeps a failed attempt by its path and answers the next with the same failure,
			// whatever the text says now: a fixed save must start over in a session of its own.
			if (module == nullptr)
				stale = true;

			return module;
		}

		/**
		 * Composes the parts with every entry point `entries` defines, links them and writes the
		 * SPIR-V of all those entry points as one blob. The linked program comes back for its
		 * reflection; null on failure, reported.
		 */
		[[nodiscard]] ComPtr<slang::IComponentType> link(slang::IModule* entries,
														 Span<slang::IComponentType* const> modules, StringView file,
														 Vector<u32>& spirv, Report& report) noexcept
		{
			Vector<slang::IComponentType*> parts(modules.begin(), modules.end(), &tools());
			Vector<ComPtr<slang::IEntryPoint>> points(&tools());

			for (SlangInt32 i = 0; i < entries->getDefinedEntryPointCount(); ++i)
			{
				ComPtr<slang::IEntryPoint> point;
				entries->getDefinedEntryPoint(i, point.writeRef());
				parts.push_back(point.get());
				points.push_back(std::move(point));
			}

			if (points.empty())
			{
				report.error(file, "declares no entry points; mark them with [shader(\"vertex\")] and the like");
				return nullptr;
			}

			ComPtr<slang::IComponentType> composite;
			ComPtr<slang::IComponentType> linked;
			ComPtr<slang::IBlob> code;
			ComPtr<slang::IBlob> diagnostics;

			const bool built =
				SLANG_SUCCEEDED(session->createCompositeComponentType(parts.data(), static_cast<SlangInt>(parts.size()),
																	  composite.writeRef(), diagnostics.writeRef())) &&
				SLANG_SUCCEEDED(composite->link(linked.writeRef(), diagnostics.writeRef())) &&
				SLANG_SUCCEEDED(linked->getTargetCode(0, code.writeRef(), diagnostics.writeRef()));

			report.forward(diagnostics);

			if (!built)
			{
				report.error(file, "did not link");
				return nullptr;
			}

			spirv.resize(code->getBufferSize() / sizeof(u32));
			std::memcpy(spirv.data(), code->getBufferPointer(), spirv.size() * sizeof(u32));
			return linked;
		}

		/**
		 * What every compile shares: the path made absolute, since Slang resolves the imports beside
		 * a file from it and matches reloads by it; a session that is still good; and afterwards the
		 * stamps of everything that session has read.
		 */
		template <class Compile>
		[[nodiscard]] bool run(StringView path, String& diagnostics, Compile&& compile) noexcept
		{
			Report report(diagnostics);

			String file(&tools());
			if (!fs::absolute(path, file))
				file = path;

			if (!open_session(file, report))
				return false;

			const bool compiled = compile(file, report);
			record_stamps();
			return compiled;
		}

		[[nodiscard]] slang::IModule* entry_module(material::Domain domain, Report& report) noexcept
		{
			slang::IModule*& module = entry_modules[static_cast<size_t>(domain)];

			if (module == nullptr)
			{
				ComPtr<slang::IBlob> diagnostics;
				module = session->loadModule(ENTRY_MODULES[static_cast<size_t>(domain)], diagnostics.writeRef());
				report.forward(diagnostics);

				if (module == nullptr)
				{
					// An engine file failed, most likely mid-edit: the next compile starts over.
					stale = true;
					report.error(engine_dir, "the engine's {} module did not compile",
								 ENTRY_MODULES[static_cast<size_t>(domain)]);
				}
			}

			return module;
		}

		/// The model a surface type named, or the default, matched ignoring case against the
		/// structs in shading.slang that implement IShadingModel.
		void resolve_shading(StringView requested, StringView file, material::State& state, Report& report) noexcept
		{
			if (shading_models.empty())
			{
				for (SlangInt i = 0; i < session->getLoadedModuleCount(); ++i)
				{
					slang::IModule* module = session->getLoadedModule(i);

					if (std::strcmp(module->getName(), SHADING_MODULE) != 0)
						continue;

					slang::ProgramLayout* types	 = module->getLayout();
					slang::TypeReflection* model = types->findTypeByName(SHADING_INTERFACE);
					slang::DeclReflection* decls = module->getModuleReflection();

					for (u32 d = 0; model != nullptr && d < decls->getChildrenCount(); ++d)
					{
						slang::DeclReflection* decl = decls->getChild(d);

						if (decl->getKind() == slang::DeclReflection::Kind::Struct &&
							types->isSubType(decl->getType(), model))
							shading_models.emplace_back(decl->getName());
					}
				}
			}

			const StringView wanted = requested.empty() ? StringView(default_shading) : requested;

			for (const String& model : shading_models)
			{
				if (equal_ignoring_case(model, wanted))
				{
					state.shading = model;
					return;
				}
			}

			String known(&tools());
			for (const String& model : shading_models)
			{
				known += known.empty() ? "" : ", ";
				for (const char c : model)
					known += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
			}

			report.error(file, "shading model '{}' is not one of: {}", wanted, known);
		}

		[[nodiscard]] bool compile_program(const String& file, StringView source, Program& out, Report& report) noexcept
		{
			slang::IModule* module = load("program", file, source, report);

			if (module == nullptr)
				return false;

			slang::IComponentType* parts[] = {module};

			if (!link(module, parts, file, out.spirv, report))
				return false;

			add_dependencies(module, out.dependencies);
			return true;
		}

		[[nodiscard]] bool compile_material(const String& file, StringView source, material::Type& out,
											Report& report) noexcept
		{
			slang::IModule* module = load("type", file, source, report);

			if (module == nullptr)
				return false;

			// What the module read is known from here on, and is recorded before anything else can
			// fail: a compile that fails still names the files a fix can be made in.
			add_dependencies(module, out.dependencies);

			check_imports(module, file, source, engine(), report);
			check_globals(session, module, engine(), report);

			// The one struct implementing a domain's interface is the type, and which interface it
			// implements is its domain.
			slang::ProgramLayout* types	 = module->getLayout();
			slang::DeclReflection* decls = module->getModuleReflection();
			slang::DeclReflection* found = nullptr;
			String names(&tools());
			u32 count = 0;

			for (u32 i = 0; i < decls->getChildrenCount(); ++i)
			{
				slang::DeclReflection* decl = decls->getChild(i);

				for (u32 d = 0; decl->getKind() == slang::DeclReflection::Kind::Struct && d < std::size(ENTRY_MODULES);
					 ++d)
				{
					slang::TypeReflection* domain = types->findTypeByName(DOMAIN_INTERFACES[d]);

					if (domain == nullptr || !types->isSubType(decl->getType(), domain))
						continue;

					if (count++ == 0)
					{
						found	   = decl;
						out.domain = static_cast<material::Domain>(d);
					}

					names += names.empty() ? "" : ", ";
					names += decl->getName();
				}
			}

			// Without exactly one type there is nothing to reflect. Every other problem is reported
			// and the compile carries on, so one pass lists them all before anything links.
			if (count != 1)
			{
				if (count == 0)
					report.error(file, "declares no struct implementing IMaterial or IScreenMaterial; a material "
									   "file imports material and declares one");
				else
					report.error(file, "declares {} material structs ({}); a file is one type", count, names);

				return false;
			}

			StringView shading;
			out.name = found->getName();
			reflect_state(found->getType(), out.domain, file, out.state, shading, report);

			// The record and the per-object data come from the author's module, before anything
			// links: std430 depends on the target, not on linkage, so the offsets are the ones the
			// linked entry points use, and every problem the type has is reported by this compile.
			reflect_record(types->getTypeLayout(found->getType(), slang::LayoutRules::DefaultStructuredBuffer),
						   out.name, file, out.record, report);

			for (u32 i = 0; i < found->getChildrenCount(); ++i)
			{
				slang::DeclReflection* child = found->getChild(i);

				if (child->getKind() == slang::DeclReflection::Kind::Struct &&
					std::strcmp(child->getName(), "Instance") == 0)
					reflect_instance(
						types->getTypeLayout(child->getType(), slang::LayoutRules::DefaultStructuredBuffer), out.name,
						file, out.instance, report);
			}

			slang::IModule* entry = entry_module(out.domain, report);

			if (entry != nullptr)
				add_dependencies(entry, out.dependencies);

			if (out.domain == material::Domain::Surface && entry != nullptr)
				resolve_shading(shading, file, out.state, report);

			if (report.failed())
				return false;

			// The modules the compiler writes: one export each, giving the entry module's link-time
			// types their definitions. Each imports only what its export names, so a type may share
			// its name with a shading model (a type called Unlit is natural) without making either
			// export ambiguous. The material glue imports the author's module by its session name, so
			// no file an author writes ever names it.
			String material_glue(&tools());
			fmt::format_to(std::back_inserter(material_glue),
						   "import material;\nimport {};\nexport struct Material : {} = {};\n", module->getName(),
						   DOMAIN_INTERFACES[static_cast<size_t>(out.domain)], out.name);

			String glue_path(file, &tools());
			glue_path += "#material";

			slang::IModule* material_link = load("link", glue_path, material_glue, report);

			if (material_link == nullptr)
			{
				report.error(file,
							 "'{}' cannot be linked: it must be visible to the engine (leave out any `module` "
							 "line, or declare it `public`) and not share a name with the authoring API",
							 out.name);
				return false;
			}

			slang::IComponentType* parts[4] = {entry, module, material_link};
			u32 part_count					= 3;

			if (out.domain == material::Domain::Surface)
			{
				String shading_glue(&tools());
				fmt::format_to(std::back_inserter(shading_glue),
							   "import shading;\nexport struct Shading : IShadingModel = {};\n", out.state.shading);

				glue_path = file;
				glue_path += "#shading";

				// resolve_shading() found this model among the engine's, so only a broken engine
				// module fails here, and Slang has said why.
				slang::IModule* shading_link = load("link", glue_path, shading_glue, report);

				if (shading_link == nullptr)
					return false;

				parts[part_count++] = shading_link;
			}

			const ComPtr<slang::IComponentType> linked = link(entry, {parts, part_count}, file, out.spirv, report);
			if (!linked)
				return false;

			// The domain fixes the entry points passes ask for. One renamed in an entry module fails
			// here, loudly, instead of at the first pipeline built from this type.
			const char* entry_points[] = {material::VERTEX_ENTRY, material::COLOR_ENTRY, material::DEPTH_ENTRY};
			const u32 expected		   = out.domain == material::Domain::Surface ? 3 : 2;

			for (u32 i = 0; i < expected; ++i)
				if (linked->getLayout()->findEntryPointByName(entry_points[i]) == nullptr)
					report.error(engine_dir, "the engine's {} module has no {} entry point",
								 ENTRY_MODULES[static_cast<size_t>(out.domain)], entry_points[i]);

			out.hash = material::hash_type(out);
			return !report.failed();
		}
	};

	Compiler::~Compiler() noexcept { shutdown(); }

	bool Compiler::initialize(const CompilerDef& def) noexcept
	{
		EMBER_ASSERT(m_impl == nullptr && "initialize runs once");

		Impl* impl = memory::new_object<Impl>(MemoryTag::Tools);

		const auto fail = [&]() noexcept
		{
			memory::delete_object(MemoryTag::Tools, impl);
			return false;
		};

		const char* engine_dir = def.engine_dir != nullptr ? def.engine_dir : EMBER_SHADER_DIR;

		if (!fs::absolute(engine_dir, impl->engine_dir))
		{
			EMBER_ERROR("shader compiler: '{}' is not a usable path", engine_dir);
			return fail();
		}

		if (impl->engine_dir.back() != '/')
			impl->engine_dir += '/';

		// Every module directly in the engine directory is the engine's own; the authoring API is
		// the one a material file may import.
		const auto listed =
			fs::enumerate(impl->engine_dir,
						  [&](const fs::DirectoryEntry& entry) noexcept
						  {
							  if (entry.type == fs::FileType::Regular && fs::extension(entry.name) == ".slang" &&
								  fs::stem(entry.name) != "material")
								  impl->engine_modules.emplace_back(fs::stem(entry.name));

							  return fs::Visit::Continue;
						  });

		if (!listed || impl->engine_modules.empty())
		{
			EMBER_ERROR("shader compiler: no engine shaders in '{}'", impl->engine_dir);
			return fail();
		}

		impl->search_paths.emplace_back(impl->engine_dir);

		for (const char* dir : def.include_dirs)
		{
			String absolute(&tools());
			if (fs::absolute(dir, absolute))
				impl->search_paths.push_back(std::move(absolute));
		}

		impl->default_shading = def.default_shading;

		// The global session loads Slang's core module: the one slow step, paid once per process.
		if (SLANG_FAILED(slang::createGlobalSession(impl->global.writeRef())))
		{
			EMBER_ERROR("shader compiler: libslang did not start");
			return fail();
		}

		impl->demote = impl->global->findCapability("spvDemoteToHelperInvocation");
		m_impl		 = impl;
		return true;
	}

	void Compiler::shutdown() noexcept
	{
		std::lock_guard lock(m_lock);

		memory::delete_object(MemoryTag::Tools, m_impl);
		m_impl = nullptr;
	}

	bool Compiler::compile_program(StringView path, StringView source, Program& out, String& diagnostics) noexcept
	{
		EMBER_ASSERT(m_impl != nullptr && "compile before initialize");

		std::lock_guard lock(m_lock);
		out = {};

		return m_impl->run(path, diagnostics, [&](const String& file, Report& report) noexcept
						   { return m_impl->compile_program(file, source, out, report); });
	}

	bool Compiler::compile_material(StringView path, StringView source, material::Type& out,
									String& diagnostics) noexcept
	{
		EMBER_ASSERT(m_impl != nullptr && "compile before initialize");

		std::lock_guard lock(m_lock);
		out = {};

		return m_impl->run(path, diagnostics, [&](const String& file, Report& report) noexcept
						   { return m_impl->compile_material(file, source, out, report); });
	}

	StringView Compiler::engine_dir() const noexcept
	{
		return m_impl != nullptr ? StringView(m_impl->engine_dir) : StringView();
	}

	const char* Compiler::configured_engine_dir() noexcept { return EMBER_SHADER_DIR; }
}
