#include "asset_watcher.h"
#include "asset_manager.h"
#include <filesystem>

using namespace Ember;

AssetWatcher::AssetWatcher(AssetManager& mgr, std::string_view asset_dir) : m_manager(mgr) {
	m_watch_id = m_file_watcher.addWatch(asset_dir.data(), this, true);
	m_file_watcher.watch();
}

void AssetWatcher::handleFileAction(
	efsw::WatchID watch_id,
	const std::string& dir,
	const std::string& filename,
	efsw::Action action,
	std::string old_filename
) {
	// Only care about modifications
	if (action == efsw::Actions::Modified) {
		std::filesystem::path full_path = std::filesystem::path(dir) / filename;
		m_manager.on_file_changed(full_path.string());
	}
}
