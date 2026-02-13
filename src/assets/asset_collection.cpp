#include "asset_collection.h"

using namespace Ember;

Ref<Asset> IAssetCollection::get_internal(const std::string& path) {
	// Fast path: check cache (read lock)
	{
		std::shared_lock lock(m_mutex);
		if (auto it = m_assets.find(path); it != m_assets.end()) {
			return it->second;
		}
	}

	// Slow path: load (write lock)
	std::unique_lock lock(m_mutex);

	// If another thread is loading this exact assset, wait for it.
	m_cv.wait(lock, [&]{
		return m_loading_in_progress.find(path) == m_loading_in_progress.end();
	});

	// Did the other thread finish loading it successfully?
	if (auto it = m_assets.find(path); it != m_assets.end()) {
		return it->second;
	}

	// Mark as loading so other threads wait
	m_loading_in_progress.insert(path);

	// Release lock while reading from disk
	lock.unlock();

	Ref<Asset> new_asset = nullptr;
	try {
		// Call the virtual function (redirects to the Template class)
		new_asset = load_asset(path);
	} catch (...) {
		// Ensure we clean up if loading fails
		std::unique_lock re_lock(m_mutex);
		m_loading_in_progress.erase(path);
		m_cv.notify_all();
		throw;
	}

	// Re-acquire lock to update map
	lock.lock();
	m_loading_in_progress.erase(path);

	if (new_asset) {
		m_assets[path] = new_asset;
	}

	// Wake up any threads waiting for this asset
	m_cv.notify_all();

	return new_asset;
}

void IAssetCollection::reload(const std::string& path) {
	Ref<Asset> asset = nullptr;

	// Thread-safe lookup of asset
	{
		std::shared_lock lock(m_mutex);
		if (auto it = m_assets.find(path); it != m_assets.end()) {
			asset = it->second;
		}
	}

	// If the asset is currently loaded, reload it.
	// If it's not in memory, we don't care (lazy loading will handle it).
	if (asset) {
		try {
			// Load the new data from disk
			Ref<Asset> new_asset = load_asset(path);

			// Update the existing asset instance in-place
			asset->reload(std::move(*new_asset));
			EMBER_INFO("Hot-reloaded asset: {}", path);
		} catch (std::exception& e) {
			EMBER_ERROR("Failed to hot-reload {}: {}", path, e.what());
		}
	}
}
