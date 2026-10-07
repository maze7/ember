#include <ember/script/script_asset.h>

#include <ember/core/hash.h>
#include <ember/core/logger.h>

#include <algorithm>

namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Scripting); }

		/** The scripts root a script's name is under: its first segment. */
		[[nodiscard]] StringView root_of(StringView path) noexcept
		{
			const size_t slash = path.find('/');
			return slash == StringView::npos ? path : path.substr(0, slash);
		}

		void log_problem(const Problem& problem) noexcept
		{
			if (problem.severity == Severity::Error)
				EMBER_ERROR("script {}:{}: {}", StringView(problem.path), problem.line, StringView(problem.message));
			else
				EMBER_WARN("script {}:{}: {}", StringView(problem.path), problem.line, StringView(problem.message));
		}
	}

	bool ScriptAsset::load(AssetLoad& load, ScriptAsset& out) noexcept
	{
		// The aliases beside the scripts, read as a dependency: a change to them compiles everything again.
		const StringView root = root_of(load.path());
		String luaurc(root, &heap());
		luaurc += "/.luaurc";

		Aliases aliases;
		if (const auto read = load.read(luaurc))
		{
			auto parsed = Aliases::parse(root, read->bytes());
			if (parsed)
				aliases = std::move(*parsed);
			else
				log_problem(parsed.error());
		}
		else if (read.error().code != fs::FileErrorCode::NotFound)
		{
			EMBER_WARN("script {}: cannot read {} ({})", load.path(), StringView(luaurc), enum_name(read.error().code));
		}

		// A text that does not compile is still the asset, with its problem: the host refuses it and a
		// panel shows why, and the next save tries again through the same slot.
		auto compiled = compile(load.path(), load.bytes(), aliases);
		if (!compiled)
		{
			out.problem			= std::move(compiled.error());
			out.source.path		= String(load.path(), &heap());
			out.source.context	= context_of(load.path(), root);
			return true;
		}

		out.source = std::move(*compiled);
		for (const Import& import : out.source.imports)
			out.required.push_back(load.load<ScriptAsset>(import.path));
		return true;
	}

	void ScriptAsset::unload(AssetServices&, ScriptAsset&) noexcept
	{
		// The required references release with the payload; nothing else is held.
	}

	void ScriptAsset::reload(AssetServices&, ScriptAsset& live, ScriptAsset& fresh) noexcept
	{
		live.source	 = std::move(fresh.source);
		live.problem = std::move(fresh.problem);
		std::swap(live.required, fresh.required); // the old requirements go with the spent payload
		++live.generation;
	}

	void register_script_assets(AssetManager& assets) noexcept { assets.register_type<ScriptAsset>("script"); }

	// --- ScriptLibrary ----------------------------------------------------------------------------

	ScriptLibrary::ScriptLibrary(StringView root) noexcept : m_root(root, &heap()), m_entries(&heap()), m_by_path(&heap())
	{
	}

	bool ScriptLibrary::is_script(StringView name) const noexcept
	{
		// Below the root, a script and not the definitions the host writes beside them for the editor.
		return name.size() > m_root.size() + 1 && name.starts_with(m_root) && name[m_root.size()] == '/' &&
			   name.ends_with(".luau") && !name.ends_with(".d.luau");
	}

	void ScriptLibrary::add(AssetManager& assets, StringView name) noexcept
	{
		Entry entry;
		entry.ref = assets.load<ScriptAsset>(name);
		m_by_path.insert_or_assign(hash_text(name), static_cast<u32>(m_entries.size()));
		m_entries.push_back(std::move(entry));
	}

	void ScriptLibrary::start(AssetManager& assets) noexcept
	{
		Vector<String> names(&heap());
		if (const auto listed = assets.enumerate(m_root, names); !listed)
		{
			EMBER_WARN("scripts: nothing at '{}' below the asset root ({})", StringView(m_root),
					   enum_name(listed.error().code));
			return;
		}

		std::sort(names.begin(), names.end());
		for (const String& name : names)
			if (is_script(name))
				add(assets, name);

		// Every first load, waited for here on the owner thread: a script type publishes nothing, so
		// nothing waits on a pump this thread would be holding up.
		for (Entry& entry : m_entries)
			entry.ref.wait();
		for (Entry& entry : m_entries)
		{
			if (const ScriptAsset* asset = entry.ref.get())
			{
				entry.generation = asset->generation;
				if (asset->problem)
					log_problem(*asset->problem);
			}
		}

		EMBER_INFO("scripts: {} loaded from '{}'", m_entries.size(), StringView(m_root));
	}

	void ScriptLibrary::stop() noexcept
	{
		m_entries.clear();
		m_by_path.clear();
	}

	Vector<Source> ScriptLibrary::all() const noexcept
	{
		Vector<Source> sources(&heap());
		for (const Entry& entry : m_entries)
			if (const ScriptAsset* asset = entry.ref.get(); asset != nullptr && !asset->problem)
				sources.push_back(asset->source);
		order_for_load(sources);
		return sources;
	}

	void ScriptLibrary::update(AssetManager& assets, Span<const AssetChange> changes, Vector<Source>& out) noexcept
	{
		// A file that appeared is loaded now and waited for, as the start waited for the rest.
		for (const AssetChange& change : changes)
		{
			if (!is_script(change.name) || m_by_path.contains(hash_text(change.name)))
				continue;
			add(assets, change.name);
			m_entries.back().ref.wait();
		}

		// Whatever moved since last time: a reload landed, or one was never handed out.
		Vector<u8> moved(m_entries.size(), u8{0}, &heap());
		bool any = false;
		for (size_t i = 0; i < m_entries.size(); ++i)
		{
			Entry& entry			 = m_entries[i];
			const ScriptAsset* asset = entry.ref.get();
			if (asset == nullptr || asset->generation == entry.generation)
				continue;

			entry.generation = asset->generation;
			if (asset->problem)
				log_problem(*asset->problem);
			else
				moved[i] = 1, any = true;
		}
		if (!any)
			return;

		// And whatever requires one of them, however far down the chain: a host loads a module again
		// after what it requires, so its require() hands out the new value.
		for (bool grew = true; grew;)
		{
			grew = false;
			for (size_t i = 0; i < m_entries.size(); ++i)
			{
				const ScriptAsset* asset = m_entries[i].ref.get();
				if (moved[i] || asset == nullptr || asset->problem)
					continue;
				for (const Import& import : asset->source.imports)
				{
					const auto it = m_by_path.find(hash_text(import.path));
					if (it != m_by_path.end() && moved[it->second])
					{
						moved[i] = 1;
						grew	 = true;
						break;
					}
				}
			}
		}

		for (size_t i = 0; i < m_entries.size(); ++i)
			if (moved[i])
				out.push_back(m_entries[i].ref.get()->source);
		order_for_load(out);
	}

	u32 ScriptLibrary::moved_since_handed_out() const noexcept
	{
		u32 moved = 0;
		for (const Entry& entry : m_entries)
			if (const ScriptAsset* asset = entry.ref.get(); asset != nullptr && asset->generation != entry.generation)
				++moved;
		return moved;
	}

	u64 ScriptLibrary::hash() const noexcept
	{
		Vector<const ScriptAsset*> loaded(&heap());
		for (const Entry& entry : m_entries)
			if (const ScriptAsset* asset = entry.ref.get(); asset != nullptr && !asset->problem)
				loaded.push_back(asset);
		std::sort(loaded.begin(), loaded.end(),
				  [](const ScriptAsset* a, const ScriptAsset* b) { return a->source.path < b->source.path; });

		u64 hash = HASH_SEED;
		for (const ScriptAsset* asset : loaded)
			hash = hash_value(asset->source.text_hash, hash);
		return hash;
	}

	void order_for_load(Vector<Source>& sources) noexcept
	{
		// By path first, so ties break the same everywhere; then each after what it requires, as far
		// as what it requires is in this batch.
		std::sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) { return a.path < b.path; });

		const size_t count = sources.size();
		Vector<u8> state(count, 0, &heap()); // 0 unseen, 1 visiting, 2 placed
		Vector<Source> ordered(&heap());
		ordered.reserve(count);

		const auto index_of = [&](StringView path) noexcept -> size_t
		{
			for (size_t i = 0; i < count; ++i)
				if (sources[i].path == path)
					return i;
			return count;
		};

		const auto visit = [&](auto& self, size_t i) -> void
		{
			if (state[i] != 0)
				return; // placed, or on the path here: a cycle, which the host reports when it loads
			state[i] = 1;
			for (const Import& import : sources[i].imports)
				if (const size_t j = index_of(import.path); j < count)
					self(self, j);
			state[i] = 2;
			ordered.push_back(std::move(sources[i]));
		};

		for (size_t i = 0; i < count; ++i)
			visit(visit, i);

		sources = std::move(ordered);
	}
}
