#include <ember/assets/bank_asset.h>
#include <ember/audio/engine.h>

namespace ember
{
	bool BankAsset::load(AssetLoad& load, BankAsset& out) noexcept
	{
		// Any thread: only the bytes. The engine is the owner thread's, and takes them at the pump.
		out.name  = load.path();
		out.bytes = load.take_bytes();
		return !out.bytes.empty();
	}

	bool BankAsset::publish(AssetServices& services, BankAsset& asset) noexcept
	{
		// The engine copies the bytes as it takes them, so the payload lets its copy go at once.
		asset.loaded = services.context<audio::Engine>().load_bank(asset.name, asset.bytes.bytes());
		asset.bytes.reset();
		return true;
	}

	void BankAsset::reload(AssetServices& services, BankAsset& live, BankAsset& fresh) noexcept
	{
		// The same name, built again: the engine lets the old bank go and loads the new one.
		live.loaded = services.context<audio::Engine>().load_bank(live.name, fresh.bytes.bytes());
		fresh.bytes.reset();
	}

	void BankAsset::unload(AssetServices& services, BankAsset& asset) noexcept
	{
		// A reload's spent payload, and one that never reached the engine, hold nothing there.
		if (asset.loaded)
			services.context<audio::Engine>().unload_bank(asset.name);
	}
}
