#include "asset_manager.h"
#include "graphics/texture.h"
#include "core/time.h"

using namespace Ember;

AssetManager::AssetManager(std::string_view asset_dir)
	: m_root_dir(asset_dir)
	// , m_asset_watcher(*this, asset_dir)
{
	init_default_collections();
}

void AssetManager::on_file_changed(const std::string& absolute_path) {
	// calculate relative path to match asset keys
	std::filesystem::path full_path(absolute_path);
	std::filesystem::path relative = std::filesystem::relative(full_path);

	std::lock_guard<std::mutex> lock(m_reload_mutex);
	m_pending_reloads[relative.string()] = Time::seconds() + 0.2;
}

void AssetManager::init_default_collections() {
	init<Texture>();
}

void AssetManager::update() {
	std::vector<std::string> assets_to_reload;
	double now = Time::seconds();

	// swap queue to minimize lock time
	{
		std::lock_guard<std::mutex> lock(m_reload_mutex);

		// Iterate using an iterator so we can safely erase items
		for (auto it = m_pending_reloads.begin(); it != m_pending_reloads.end();) {
			// Check if the current time has passed the scheduled time
			if (now >= it->second) {
				assets_to_reload.push_back(it->first);
				it = m_pending_reloads.erase(it); // remove it
			} else {
				++it;
			}
		}
	}

	// Process reloads on the main thread
	for (const auto& path : assets_to_reload) {
		reload_asset(path);
	}
}

void AssetManager::reload_asset(const std::string& path) {
	// Since we don't know which collection owns a given path, we ask
	// all active collections to try reloading it. Only the collection
	// that actually has this path in m_assets will do work.
	for (auto& collection : m_collections) {
		if (collection) {
			collection->reload(path);
		}
	}
}
