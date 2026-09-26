#include <assets/file_watcher.h>

#include <ember/core/logger.h>
#include <ember/memory/memory.h>

#include <cstring>

namespace ember::detail
{
	FileWatcher::FileWatcher(AssetManager& sink, StringView root) noexcept
		: m_sink(sink), m_root(root, &memory::heap(MemoryTag::Assets))
	{
		// The manager resolved the root: absolute, normalised, ending in a separator. Every
		// directory efsw reports begins with it, spelt the platform's way.
		EMBER_ASSERT(!m_root.empty() && m_root.back() == '/');
	}

	FileWatcher::~FileWatcher() noexcept = default; // efsw joins its thread here

	bool FileWatcher::start() noexcept
	{
		const efsw::WatchID id = m_watcher.addWatch(std::string(m_root.c_str()), this, true);

		if (id < 0)
		{
			EMBER_WARN("asset watch on '{}' failed: {}", m_root, efsw::Errors::Log::getLastErrorLog());
			return false;
		}

		m_watcher.watch();
		EMBER_INFO("watching '{}' for asset changes", m_root);
		return true;
	}

	void FileWatcher::handleFileAction(efsw::WatchID, const std::string& dir, const std::string& filename,
									   efsw::Action action, const std::string&)
	{
		// A deletion is not a reload: the file that replaces it announces itself. A move reports
		// under the new name, which is the one an asset would be loaded by.
		if (action == efsw::Actions::Delete)
			return;

		// The root is spelt with '/', the directory the platform's way: the separators fold as
		// the prefix is compared.
		if (dir.size() < m_root.size())
			return;

		for (size_t i = 0; i < m_root.size(); ++i)
			if ((dir[i] == '\\' ? '/' : dir[i]) != m_root[i])
				return;

		// The relative path is the directory's tail, then the name, spelt with '/' so it hashes as
		// the game loaded it; joined in place because this is efsw's thread, which the engine heap
		// has never met.
		char relative[512];
		const size_t dir_tail = dir.size() - m_root.size();

		if (dir_tail + filename.size() >= sizeof(relative))
		{
			EMBER_WARN("asset path too long to watch: {}{}", dir, filename);
			return;
		}

		for (size_t i = 0; i < dir_tail; ++i)
		{
			const char c = dir[m_root.size() + i];
			relative[i]	 = c == '\\' ? '/' : c;
		}

		std::memcpy(relative + dir_tail, filename.data(), filename.size());

		m_sink.notify_changed(asset_id(StringView(relative, dir_tail + filename.size())));
	}
}
