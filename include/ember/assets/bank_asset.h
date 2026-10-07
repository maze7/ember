#pragma once

#include <ember/assets/asset.h>
#include <ember/core/common.h>

namespace ember
{
	/**
	 * A bank FMOD Studio built, as an asset: "audio/Master.bank". While a reference holds it the audio
	 * engine has its events, and a build in Studio reloads it under the running game: what plays from
	 * the old bank stops, and what is asked for still starts again from the new one.
	 *
	 * The bytes are read like any asset's, through whatever source holds them, and handed to the
	 * engine from memory at the pump, which copies them; so a bank in a pack, or one that arrived
	 * from another machine, plays like one on disk. The engine loads it on its own threads, so the
	 * reference turns true at the next pump and the bank's events play a few frames after. What is
	 * asked for meanwhile is asked for again.
	 *
	 * Registered by the runtime with its audio engine, which every bank loads into.
	 */
	struct BankAsset
	{
		String name{&memory::heap(MemoryTag::Assets)}; // the asset's: what the engine keys the bank by
		fs::FileData bytes;							   // the file, until the engine has taken its copy
		bool loaded = false;						   // the engine holds this name's bank on this payload's account

		static bool load(AssetLoad& load, BankAsset& out) noexcept;
		static bool publish(AssetServices& services, BankAsset& asset) noexcept;
		static void reload(AssetServices& services, BankAsset& live, BankAsset& fresh) noexcept;
		static void unload(AssetServices& services, BankAsset& asset) noexcept;
	};

	static_assert(AssetType<BankAsset>);
}
