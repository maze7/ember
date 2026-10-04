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
	 * FMOD reads the file itself, on its own threads, as it streams music from it, so there is nothing
	 * here to wait for: the reference turns true at the next pump, and the bank's events play a few
	 * frames after, once FMOD has them. What is asked for meanwhile is asked for again.
	 *
	 * Registered by the runtime with its audio engine, which every bank loads into.
	 */
	struct BankAsset
	{
		String file;		 // the bank's, absolute
		bool loaded = false; // the engine holds this file's bank on this payload's account

		/// FMOD opens the file itself.
		static constexpr bool READS_OWN_FILES = true;

		static bool load(AssetLoad& load, BankAsset& out) noexcept;
		static bool publish(AssetServices& services, BankAsset& asset) noexcept;
		static void reload(AssetServices& services, BankAsset& live, BankAsset& fresh) noexcept;
		static void unload(AssetServices& services, BankAsset& asset) noexcept;
	};

	static_assert(AssetType<BankAsset>);
}
