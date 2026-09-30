#pragma once

#include <ember/assets/asset.h>
#include <ember/core/common.h>
#include <ember/sync/spin_mutex.h>

#include <efsw/efsw.hpp>

namespace ember::detail
{
	/**
	 * The OS watch behind hot reload: efsw runs its own thread and calls back on it for every file
	 * event under a watched directory, and the callback does one thing, name the file to the manager
	 * by AssetId. Debounce and the reload itself belong to AssetManager::pump(), on the frame clock.
	 * Private to the module: nothing outside sees efsw.
	 */
	class FileWatcher final : efsw::FileWatchListener
	{
	public:
		explicit FileWatcher(AssetManager& sink) noexcept;
		~FileWatcher() noexcept override;

		FileWatcher(const FileWatcher&)			   = delete;
		FileWatcher& operator=(const FileWatcher&) = delete;

		/**
		 * Watches `directory`, subdirectories included, and names a file there `prefix/` and its path
		 * below it, or just that path for an empty prefix: the names the manager gave its assets.
		 * `directory` is absolute, normalised and ends in a separator. False when it cannot be
		 * watched; loads still work, saves just go unnoticed.
		 */
		[[nodiscard]] bool watch(StringView directory, StringView prefix) noexcept;

	private:
		/// Up to this many directories: the root and the mounts. A fixed table, so efsw's thread
		/// reads it without the engine heap, which that thread has never met.
		static constexpr u32 MAX_WATCHES = 8;

		struct Watch
		{
			efsw::WatchID id	= 0;
			char directory[512] = {};
			char prefix[64]		= {};
			u32 directory_size	= 0;
			u32 prefix_size		= 0;
		};

		void handleFileAction(efsw::WatchID watch, const std::string& dir, const std::string& filename,
							  efsw::Action action, const std::string& old_filename) override;

		AssetManager& m_sink;
		efsw::FileWatcher m_watcher;

		SpinMutex m_lock; // added on the owner thread, read on efsw's
		Watch m_watches[MAX_WATCHES];
		u32 m_count	   = 0;
		bool m_started = false;
	};
}
