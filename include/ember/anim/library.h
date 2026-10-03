#pragma once

#include <ember/anim/sample.h>
#include <ember/assets/asset.h>
#include <ember/assets/texture_asset.h>

namespace ember::anim
{
	/** A .sheet: its sprites, measured on its image, and the image as a texture. A save to either reloads it. */
	struct SheetAsset
	{
		Sheet sheet;
		AssetRef<TextureAsset> texture;

		static bool load(AssetLoad& load, SheetAsset& out) noexcept;
		static void reload(AssetServices& services, SheetAsset& live, SheetAsset& fresh) noexcept;
		static void unload(AssetServices& services, SheetAsset& asset) noexcept;
	};

	/** A .anim. */
	struct RigAsset
	{
		Rig rig;

		static bool load(AssetLoad& load, RigAsset& out) noexcept;
		static void reload(AssetServices& services, RigAsset& live, RigAsset& fresh) noexcept;
		static void unload(AssetServices& services, RigAsset& asset) noexcept;
	};

	/** Registers both with the asset manager: once, before its first load. */
	void register_types(AssetManager& assets) noexcept;

	/**
	 * A world's sheets and rigs, by the ids its components keep: each loads the first time a path
	 * asks for it and stays, so an id never changes, and hot reload reaches it through the manager.
	 * Ids are handed out on one thread at a time; lookups are any thread's.
	 */
	class Library final : public Source
	{
	public:
		explicit Library(AssetManager& assets) noexcept : m_assets(&assets) {}

		[[nodiscard]] u16 sheet_id(const char* path) noexcept;
		[[nodiscard]] u16 rig_id(const char* path) noexcept;

		/**
		 * Gives the sheets each loaded rig names for its slots their ids. Once a frame, so a rig that
		 * has just loaded, or reloaded naming other sheets, shows them.
		 */
		void refresh() noexcept;

		[[nodiscard]] const Sheet* sheet(u16 id) const noexcept override;
		[[nodiscard]] const Rig* rig(u16 id) const noexcept override;
		[[nodiscard]] u16 default_sheet(u16 rig, Name slot) const noexcept override;

		/** The sheet's image; null while it loads. */
		[[nodiscard]] TextureHandle texture(u16 sheet) const noexcept;

	private:
		AssetManager* m_assets;
		Vector<AssetRef<SheetAsset>> m_sheets;
		Vector<AssetRef<RigAsset>> m_rigs;
		Vector<Vector<Named<u16>>> m_defaults; // each rig's slots' default sheets, by rig id
		HashMap<u64, u16> m_sheet_ids;		   // by the path's hash
		HashMap<u64, u16> m_rig_ids;
	};
}
