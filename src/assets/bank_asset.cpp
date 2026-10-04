#include <ember/assets/bank_asset.h>
#include <ember/audio/engine.h>
#include <ember/core/filesystem.h>

namespace ember
{
	bool BankAsset::load(AssetLoad& load, BankAsset& out) noexcept
	{
		// Any thread: only that the file is there. The engine is the owner thread's, and takes it at the pump.
		const auto found = fs::exists(load.file());
		if (!found || !found.value())
			return false;

		out.file = String(load.file());
		return true;
	}

	bool BankAsset::publish(AssetServices& services, BankAsset& asset) noexcept
	{
		asset.loaded = services.context<audio::Engine>().load_bank(asset.file);
		return true;
	}

	void BankAsset::reload(AssetServices& services, BankAsset& live, BankAsset&) noexcept
	{
		// The same file, built again: the engine lets the old bank go and loads the new one.
		live.loaded = services.context<audio::Engine>().load_bank(live.file);
	}

	void BankAsset::unload(AssetServices& services, BankAsset& asset) noexcept
	{
		// A reload's spent payload, and one that never reached the engine, hold nothing there.
		if (asset.loaded)
			services.context<audio::Engine>().unload_bank(asset.file);
	}
}
