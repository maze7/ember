#pragma once

#include <ember/assets/asset.h>
#include <ember/core/common.h>

#include <efsw/efsw.hpp>

namespace ember::detail
{
	/**
	 * The OS watch behind hot reload: efsw runs its own thread and calls back on it for every file
	 * event under the root, and the callback does one thing, name the file to the manager by
	 * AssetId. Debounce and the reload itself belong to AssetManager::pump(), on the frame clock.
	 * Private to the module: nothing outside sees efsw.
	 */
	class FileWatcher final : efsw::FileWatchListener
	{
	public:
		FileWatcher(AssetManager& sink, StringView root) noexcept;
		~FileWatcher() noexcept override;

		FileWatcher(const FileWatcher&)			   = delete;
		FileWatcher& operator=(const FileWatcher&) = delete;

		/// Starts watching the root, subdirectories included. False when the directory cannot be
		/// watched; loads still work, saves just go unnoticed.
		[[nodiscard]] bool start() noexcept;

	private:
		void handleFileAction(efsw::WatchID watch, const std::string& dir, const std::string& filename,
							  efsw::Action action, const std::string& old_filename) override;

		AssetManager& m_sink;
		String m_root; // absolute, normalised, with a trailing separator: what efsw's dir strings begin with
		efsw::FileWatcher m_watcher;
	};
}
