#include <assets/file_watcher.h>

#include <ember/core/logger.h>
#include <ember/memory/memory.h>

#include <cstring>
#include <mutex>

namespace ember::detail
{
	FileWatcher::FileWatcher(AssetManager& sink) noexcept : m_sink(sink) {}

	FileWatcher::~FileWatcher() noexcept = default; // efsw joins its thread here

	bool FileWatcher::watch(StringView directory, StringView prefix) noexcept
	{
		// The manager resolved the directory: absolute, normalised, ending in a separator. Every
		// directory efsw reports below it begins with it, spelt the platform's way.
		EMBER_ASSERT(!directory.empty() && directory.back() == '/');

		if (m_count == MAX_WATCHES || directory.size() >= sizeof(Watch::directory) ||
			prefix.size() >= sizeof(Watch::prefix))
		{
			EMBER_WARN("cannot watch '{}' for asset changes: too many watches or too long a path", directory);
			return false;
		}

		const efsw::WatchID id = m_watcher.addWatch(std::string(directory), this, true);

		if (id < 0)
		{
			EMBER_WARN("asset watch on '{}' failed: {}", directory, efsw::Errors::Log::getLastErrorLog());
			return false;
		}

		{
			std::lock_guard lock(m_lock);

			Watch& watch = m_watches[m_count++];
			watch.id	 = id;
			std::memcpy(watch.directory, directory.data(), directory.size());
			std::memcpy(watch.prefix, prefix.data(), prefix.size());
			watch.directory_size = static_cast<u32>(directory.size());
			watch.prefix_size	 = static_cast<u32>(prefix.size());
		}

		// One thread serves every watch; it starts with the first.
		if (!m_started)
		{
			m_watcher.watch();
			m_started = true;
		}

		EMBER_INFO("watching '{}' for asset changes", directory);
		return true;
	}

	void FileWatcher::handleFileAction(efsw::WatchID id, const std::string& dir, const std::string& filename,
									   efsw::Action action, const std::string&)
	{
		// A deletion is not a reload: the file that replaces it announces itself. A move reports
		// under the new name, which is the one an asset would be loaded by.
		if (action == efsw::Actions::Delete)
			return;

		// The name is the prefix, the directory's tail and the file: what the asset was loaded by,
		// separators aside, which asset_id reads alike. Built in place because this is efsw's
		// thread, which the engine heap has never met.
		char name[640];
		size_t size = 0;

		{
			std::lock_guard lock(m_lock);

			const Watch* watch = nullptr;
			for (u32 i = 0; i < m_count && watch == nullptr; ++i)
				if (m_watches[i].id == id)
					watch = &m_watches[i];

			// The directory is spelt with '/', the reported one the platform's way: the separators
			// fold as the prefix is compared.
			if (watch == nullptr || dir.size() < watch->directory_size)
				return;

			for (size_t i = 0; i < watch->directory_size; ++i)
				if ((dir[i] == '\\' ? '/' : dir[i]) != watch->directory[i])
					return;

			const size_t dir_tail = dir.size() - watch->directory_size;

			if (watch->prefix_size + 1 + dir_tail + filename.size() >= sizeof(name))
			{
				EMBER_WARN("asset path too long to watch: {}{}", dir, filename);
				return;
			}

			if (watch->prefix_size != 0)
			{
				std::memcpy(name, watch->prefix, watch->prefix_size);
				size		 = watch->prefix_size;
				name[size++] = '/';
			}

			std::memcpy(name + size, dir.data() + watch->directory_size, dir_tail);
			size += dir_tail;
		}

		std::memcpy(name + size, filename.data(), filename.size());
		size += filename.size();

		m_sink.notify_changed(asset_id(StringView(name, size)));
	}
}
