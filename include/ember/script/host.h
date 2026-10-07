#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/ecs/world.h>
#include <ember/memory/memory.h>
#include <ember/script/binding.h>
#include <ember/script/problem.h>
#include <ember/script/source.h>

struct lua_State;

namespace ember::script
{
	struct HostDef
	{
		u32 budget	   = 100'000; // VM safepoints one call may pass before it is stopped and named: an endless loop
		u32 gc_step_kb = 64;	  // what the collector is asked to reclaim at each script point
		bool checks	   = true;	  // a number that does not fit its field is refused, not wrapped: dev builds
	};

	/** What the host has done, for a panel: totals since it started, and the latest tick's cost. */
	struct Stats
	{
		u64 calls	= 0; // system functions run
		u64 queries = 0;
		u64 reads	= 0; // component fields read
		u64 writes	= 0; // fields written, and commands made
		u64 errors	= 0;
		f64 simulate_us	 = 0.0; // the latest run of every simulate point
		f64 present_us	 = 0.0; // the latest present point
		u32 modules		 = 0;
		u32 systems		 = 0;
		u32 disabled	 = 0; // systems off until their module reloads
		u32 problems	 = 0;
		size_t lua_bytes = 0; // the VM's heap
		u64 hash		 = 0; // over the loaded modules' text: equal on two machines running the same scripts
	};

	/**
	 * One world's Luau: the VM, the modules loaded into it and the systems they declared, run through
	 * the script points a game registers in its schedule. A host belongs to its world's thread, a
	 * server world's to the server thread and a client's to main, and nothing here is shared.
	 *
	 * Every module runs in a sandboxed environment of its own; what it may touch is the binding, and
	 * what it may write follows from the component kinds and the directory it lives in: a sim script
	 * writes Predicted components, a server script what the server simulates, a client script Client
	 * components. A write outside that is refused with the file and line. A module that fails to load
	 * keeps its last good version; a system that raises an error is off until its module reloads; and
	 * every problem is one record with a count, never a flood.
	 *
	 * A host runs nothing until it is given sources: a world that never gets any, an editor's edit
	 * world, pays nothing at its script points.
	 */
	class Host final
	{
	public:
		explicit Host(ecs::World& world, const HostDef& def = {}) noexcept;
		~Host() noexcept;

		Host(const Host&)			 = delete;
		Host& operator=(const Host&) = delete;

		/** What scripts may touch. Fill it before the first reload(): the VM is made from it then, once. */
		[[nodiscard]] Binding& binding() noexcept { return m_binding; }
		[[nodiscard]] const Binding& binding() const noexcept { return m_binding; }
		[[nodiscard]] ecs::World& world() noexcept { return m_world; }

		/**
		 * Loads these sources, fresh versions of modules it has or new ones, each after what it requires
		 * when the batch is in that order; require() loads what a module needs on demand otherwise. The
		 * first call makes the VM. A module that fails keeps its last good version and is reported.
		 * Between ticks, on the world's thread.
		 */
		void reload(Span<const Source> sources) noexcept;

		/** The tick the next script point simulates: what now() answers. */
		void set_tick(u32 tick) noexcept { m_now = tick; }

		/** A script point: the systems declared for this stage, in path order, then the collector's step. */
		void simulate(u8 stage, ecs::Commands& commands) noexcept;

		/** The Present point: a client's per-frame systems. */
		void present(ecs::Commands& commands) noexcept;

		[[nodiscard]] bool started() const noexcept { return m_L != nullptr; }
		[[nodiscard]] u64 hash() const noexcept { return m_hash; }
		[[nodiscard]] Span<const Problem> problems() const noexcept { return {m_problems.data(), m_problems.size()}; }
		void clear_problems() noexcept { m_problems.clear(); }

		/** The totals, and the latest tick's cost. */
		[[nodiscard]] Stats stats() const noexcept;

		/**
		 * Writes ember.d.luau for luau-lsp from the binding: exactly what this host exposes, so the
		 * editor completes and checks against the running game. Dev builds, at start, before or after
		 * the first reload alike.
		 */
		[[nodiscard]] bool write_definitions(StringView path) const noexcept;

		/** The VM, for tests and packs that need it directly; null until the first reload(). */
		[[nodiscard]] lua_State* state() noexcept { return m_L; }

	private:
		friend struct HostAccess;

		/** A loaded module: its thread, which holds its environment, and what its top level returned. */
		struct Module
		{
			String path;
			Context context	  = Context::Lib;
			u64 text_hash	  = 0;
			lua_State* thread = nullptr;
			int thread_ref	  = -1;
			int value_ref	  = -1; // what require() hands out; -1 for nothing
			Vector<Import> imports{&memory::heap(MemoryTag::Scripting)};
			bool loaded	 = false;
			bool loading = false; // its top level is running: a require() of it now is a cycle
		};

		/** A system a module declared: its function, and where in the schedule it runs. */
		struct System
		{
			u32 module	  = 0;
			u32 ordinal	  = 0; // its place among the module's systems
			u8 stage	  = 0; // the binding's stage index, or PRESENT
			int ref		  = -1;
			bool disabled = false;
			f64 us		  = 0.0; // the latest run's cost
		};

		/** A source given to reload(), kept for require() to load on demand; dirty until loaded. */
		struct Pending
		{
			Source source;
			bool dirty = false;
		};

		static constexpr u8 PRESENT = 0xff;

		static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize) noexcept;
		static void interrupt(lua_State* L, int gc);
		static i16 useratom(lua_State* L, const char* s, size_t l);
		static int lua_system(lua_State* L);
		static int lua_now(lua_State* L);
		static int lua_require(lua_State* L);
		static int lua_trampoline(lua_State* L);

		[[nodiscard]] i16 atom(StringView name) noexcept;
		void make_state() noexcept;
		void install_binding() noexcept;
		void push_function(const Function& function) noexcept;
		[[nodiscard]] Module* find_module(StringView path) noexcept;
		[[nodiscard]] Module* ensure_loaded(StringView path) noexcept;
		[[nodiscard]] bool load_module(Pending& pending) noexcept;
		void unload_module(u32 index) noexcept;
		void run_bucket(u32 bucket) noexcept;
		void rebuild_order() noexcept;
		void report(StringView path, u32 line, Severity severity, StringView message) noexcept;
		void report_lua(StringView path, const char* message) noexcept;

		ecs::World& m_world;
		HostDef m_def;
		Binding m_binding;
		lua_State* m_L = nullptr;
		u32 m_now	   = 0;

		// Names the metamethods dispatch on, as atoms: what useratom() answers for a string.
		HashMap<u64, i16> m_atoms; // by hash_text(name)
		Vector<String> m_atom_names;
		Vector<ecs::ComponentId> m_component_by_atom;
		Vector<i16> m_verb_by_atom;
		int m_ref_coroutine	  = -1; // the coroutine library, for client scripts alone
		int m_ref_math_random = -1; // math with random() kept, for server and client scripts

		HashMap<u64, Pending> m_sources; // by hash_text(path)
		Vector<Module> m_modules;
		HashMap<u64, u32> m_module_by_path;
		Vector<System> m_systems;
		Vector<Vector<u32>> m_buckets; // m_systems indices by stage, in run order; the last is Present
		Vector<f64> m_bucket_us;
		Vector<System> m_staging; // what the loading module has declared so far

		// While a module loads or a system runs: whose, and with what rights.
		i32 m_loading			  = -1; // m_modules index
		Context m_context		  = Context::Count;
		ecs::Commands* m_commands = nullptr;
		u64 m_steps				  = 0; // safepoints this call

		Stats m_stats;
		Vector<Problem> m_problems;
		size_t m_lua_bytes = 0;
		u64 m_hash		   = 0;
	};

	/** The system a game registers for a stage: registry.simulate_exclusive<&run_stage<Stage::Act>>(Stage::Act). */
	template <auto Stage> void run_stage(Host& host, ecs::Commands& commands)
	{
		host.simulate(static_cast<u8>(Stage), commands);
	}

	/** The Present point: registry.present_exclusive<&run_present>(). */
	void run_present(Host& host, ecs::Commands& commands);
}
