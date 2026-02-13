#pragma once

#include "asset.h"
#include "assets/asset_collection.h"
#include "assets/asset_watcher.h"
#include "core/common.h"
#include <filesystem>

namespace Ember
{
	class AssetManager
	{
	public:
		AssetManager(std::string_view asset_dir);

		/**
		 * @brief Initializes an AssetCollection for the requested type.
		 * @tparam T Asset type to be initialized
		 */
		template <IsAsset T>
		void init() {
			constexpr AssetType asset_type = T::asset_type();
			constexpr int id = static_cast<int>(asset_type);
			m_collections.resize(std::max(m_collections.size(), static_cast<size_t>(id + 1)));
			EMBER_ASSERT(m_collections[id] == nullptr && "AssetCollection<T> already initialized!");
			m_collections[id] = make_unique<AssetCollection<T>>(asset_type);
		}

		/**
		 * @brief Retrieves the AssetCollection for the requested type.
		 * @tparam T Asset type
		 * @returns AssetCollection<T> if one has been initialized.
		 */
		template <IsAsset T>
		[[nodiscard]] AssetCollection<T>& of() const {
			return static_cast<AssetCollection<T>&>(of_type(T::asset_type()));
		}

		/**
		 * @brief Retrieves the AssetCollection for a given AssetType
		 * @param asset_type AssetType for collection
		 * @returns IAssetCollection reference for collection of requested AssetType.
		 */
		[[nodiscard]] IAssetCollection& of_type(AssetType asset_type) const {
			return *m_collections[static_cast<int>(asset_type)];
		}

		/**
		 * @brief Retrieves an AssetHandle<T> for the asset at the provided path.
		 * @tparam T the Asset type
		 * @param name Name of the asset to be retrieved
		 */
		template <IsAsset T>
		[[nodiscard]] Ref<T> get(std::string_view name) const {
			return of<T>().get(name);
		}

		/**
		 * @brief Preloads an asset by name.
		 * @tparam T Asset type to be preloaded
		 * @param name Name of the asset to be preloaded.
		 */
		template <IsAsset T>
		void preload(std::string_view name) const {
			// cast to void to ignore compiler warning.
			static_cast<void>(of<T>().get(name));
		}

		/** @returns Filesystem path to the root asset directory. */
		const std::filesystem::path& root_directory() const { return m_root_dir; }

		/** @brief Called once at the start at every frame. Processes any pending hot-reloads. */
		void update();

	protected:
		friend class AssetWatcher;

		/**
		 * @brief Handles a file change event triggered by the AssetWatcher.
		 * @param absolute_path Absolute path of the asset that changed.
		 */
		void on_file_changed(const std::string& absolute_path);

	private:
		/**
		 * @brief Initializes the default AssetCollections for use.
		 */
		void init_default_collections();

		/**
		 * @brief Reloads a given asset by path (if it belongs to one of the AssetCollections).
		 * @prarm path String path of the asset to be reloaded.
		 */
		void reload_asset(const std::string& path);

		// Root directory that contains all assets.
		std::filesystem::path m_root_dir;

		// AssetWatcher responsible for queuing hot-reload events
		// AssetWatcher m_asset_watcher;

		// AssetCollections for each type of assset.
		std::vector<Unique<IAssetCollection>> m_collections;

		// Hot-reload queue
		std::mutex m_reload_mutex;
		std::unordered_map<std::string, double> m_pending_reloads;
	};
}
