#pragma once

#include "efsw/efsw.hpp"

namespace Ember
{
	class AssetManager;
	class AssetWatcher : public efsw::FileWatchListener
	{
	public:
		AssetWatcher(AssetManager& mgr, std::string_view assets_dir);

		void handleFileAction(
			efsw::WatchID watch_id,
			const std::string& dir,
			const std::string& filename,
			efsw::Action action,
			std::string old_filename
		);

	private:
		AssetManager& m_manager;
		efsw::FileWatcher m_file_watcher;
		efsw::WatchID m_watch_id;
	};
}
