#include <assets/file_watcher.h>

#include <ember/core/logger.h>
#include <ember/memory/memory.h>

#include <cstring>
#include <filesystem>

namespace ember::detail
{
	FileWatcher::FileWatcher(AssetManager& sink, StringView root) noexcept
		: m_sink(sink), m_root(&memory::heap(MemoryTag::Assets))
	{
		// efsw reports directories the way it was given the root, so the root goes in absolute
		// and normalised, and every reported path is then the root plus a relative tail.
		std::error_code error;
		const std::filesystem::path absolute = std::filesystem::absolute(root, error).lexically_normal();

		m_root = absolute.generic_string().c_str();

		if (!m_root.empty() && m_root.back() != '/')
			m_root += '/';
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

		// The relative path is dir minus the root, then the name; joined in place because this is
		// efsw's thread, which the engine heap has never met.
		if (dir.size() < m_root.size() || std::memcmp(dir.data(), m_root.data(), m_root.size()) != 0)
			return;

		char relative[512];
		const size_t dir_tail = dir.size() - m_root.size();

		if (dir_tail + filename.size() >= sizeof(relative))
		{
			EMBER_WARN("asset path too long to watch: {}{}", dir, filename);
			return;
		}

		std::memcpy(relative, dir.data() + m_root.size(), dir_tail);
		std::memcpy(relative + dir_tail, filename.data(), filename.size());

		// asset_id folds the separators, so a path efsw spells with backslashes names the same
		// asset the game loaded with forward slashes.
		m_sink.notify_changed(asset_id(StringView(relative, dir_tail + filename.size())));
	}
}
