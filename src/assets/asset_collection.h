#pragma once

#include "asset.h"
#include "core/common.h"
#include <functional>
#include <string_view>
#include <unordered_map>
#include <shared_mutex>
#include <condition_variable>
#include <unordered_set>

namespace Ember
{
	template <IsAsset T>
	using AssetLoader = std::function<Ref<T>(std::string_view)>;

	class IAssetCollection
	{
	public:
		explicit IAssetCollection(AssetType type) : m_asset_type(type) {}
		virtual ~IAssetCollection() = default;

		[[nodiscard]] AssetType asset_type() { return m_asset_type; }

		// The public API for hot-reloading or manual unloading
		void reload(const std::string& path);
		void unload(const std::string& path);

	protected:
		// The core thread-safe retrieval logic
		Ref<Asset> get_internal(const std::string& path);

		// Virtual function that the Template class will implement
		// This allows the Base to ask the Template to load a file
		virtual Ref<Asset> load_asset(std::string_view path) = 0;

	private:
		AssetType m_asset_type;

		// Type-erased storage: Stores the Base Asset pointer
		std::unordered_map<std::string, Ref<Asset>> m_assets;

		// Threading primitives
		mutable std::shared_mutex m_mutex;
		std::condition_variable_any m_cv;
		std::unordered_set<std::string> m_loading_in_progress;
	};

	template <IsAsset T>
	class AssetCollection : public IAssetCollection
	{
	public:
		explicit AssetCollection(AssetType type) : IAssetCollection(type) {}

		void set_loader(AssetLoader<T> loader) {
			m_loader = loader;
		}

		Ref<T> get(std::string_view path) {
			// Call the base class logic
			Ref<Asset> asset = get_internal(std::string(path));

			// Cast the generic base pointer back to the required type
			return std::static_pointer_cast<T>(asset);
		}

	protected:
		// Implementation of the virtual function
		Ref<Asset> load_asset(std::string_view path) override {
			if (m_loader) {
				return m_loader(path);
			}

			return T::load(path);
		}

	private:
		AssetLoader<T> m_loader;
	};
}
