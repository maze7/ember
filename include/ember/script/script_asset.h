#pragma once

#include <ember/assets/asset.h>
#include <ember/script/source.h>

#include <optional>

namespace ember::script
{
	/**
	 * A script as an asset: its text compiled on the loader job into the Source a host takes, with
	 * the lints the parse found. The modules it requires are assets it holds, so a lib stays loaded
	 * while anything needs it. When a lib reloads, the library hands a host everything that requires
	 * it as well, the lib first, from the imports the sources name.
	 */
	struct ScriptAsset
	{
		Source source;
		std::optional<Problem> problem;			// why it did not compile: the source has no bytecode then
		Vector<AssetRef<ScriptAsset>> required; // what it requires, kept loaded
		u32 generation = 0;						// moves on every reload

		static bool load(AssetLoad& load, ScriptAsset& out) noexcept;
		static void unload(AssetServices& services, ScriptAsset& asset) noexcept;
		static void reload(AssetServices& services, ScriptAsset& live, ScriptAsset& fresh) noexcept;
	};

	static_assert(AssetType<ScriptAsset>);

	/** Registers the type: a game's init, once the manager is up. */
	void register_script_assets(AssetManager& assets) noexcept;

	/**
	 * Every script below the scripts root, as the main thread holds them: loaded at start, followed
	 * through the manager's reloads, and handed to hosts as batches of Sources, each after what it
	 * requires. One per process: both of a host session's worlds take its batches, and a joiner's
	 * client takes what the server's files become once they land here.
	 */
	class ScriptLibrary final
	{
	public:
		explicit ScriptLibrary(StringView root = "scripts") noexcept;

		/** Lists the root through the manager and loads every .luau below it, waiting for them: a game's init. */
		void start(AssetManager& assets) noexcept;

		/** Lets every script go: a game's shutdown, before the manager's, which refuses to end with references alive. */
		void stop() noexcept;

		/** Every loaded script's Source, ordered for a host: what a new world starts with. */
		[[nodiscard]] Vector<Source> all() const noexcept;

		/**
		 * Once a frame, after take_changed(): new .luau files among the changes are loaded, and every
		 * script whose generation moved since the last call goes into `out`, with everything that
		 * requires one of them, however far down the chain, ordered for a host. A script that failed
		 * to compile is reported and left out, so the host keeps its last version.
		 */
		void update(AssetManager& assets, Span<const AssetChange> changes, Vector<Source>& out) noexcept;

		/** Scripts whose payload moved since they were last handed out, or never were: what the next update() gives. */
		[[nodiscard]] u32 moved_since_handed_out() const noexcept;

		/** Over every loaded script's text, in path order: what a panel compares between machines. */
		[[nodiscard]] u64 hash() const noexcept;
		[[nodiscard]] u32 count() const noexcept { return static_cast<u32>(m_entries.size()); }

	private:
		struct Entry
		{
			AssetRef<ScriptAsset> ref;
			u32 generation = NEVER; // the last handed out
		};

		static constexpr u32 NEVER = ~u32{0};

		[[nodiscard]] bool is_script(StringView name) const noexcept;
		void add(AssetManager& assets, StringView name) noexcept;

		String m_root;
		Vector<Entry> m_entries;
		HashMap<u64, u32> m_by_path; // hash_text(name) → entry
	};

	/** Sources in an order a host loads them in: each after what it requires, ties by path. */
	void order_for_load(Vector<Source>& sources) noexcept;
}
