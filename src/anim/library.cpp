#include <ember/anim/library.h>
#include <ember/anim/parse.h>

#include <ember/core/filesystem.h>
#include <ember/core/logger.h>

#include <stb_image.h>

#include <utility>

namespace ember::anim
{
	namespace
	{
		[[nodiscard]] StringView text_of(const AssetLoad& load) noexcept
		{
			const Span<const u8> bytes = load.bytes();
			return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
		}
	}

	bool SheetAsset::load(AssetLoad& load, SheetAsset& out) noexcept
	{
		const StringView text = text_of(load);
		String error(&load.heap());
		String image_name(&load.heap());

		if (!sheet_image(text, image_name, error))
		{
			EMBER_ERROR("{}: {}", load.path(), error);
			return false;
		}

		// The image twice: as a texture for drawing, and its pixels here, to measure each sprite,
		// read by name through whatever source holds it, which makes a save to the image reload the
		// sheet too. A cooked sheet will carry both, decoded once.
		out.texture = load.load<TextureAsset>(image_name);

		const auto file = load.read(image_name);
		if (!file)
		{
			EMBER_ERROR("{}: cannot read '{}' ({})", load.path(), image_name, enum_name(file.error().code));
			return false;
		}

		int width = 0, height = 0, channels = 0;
		stbi_uc* pixels =
			stbi_load_from_memory(file->data(), static_cast<int>(file->size()), &width, &height, &channels, 4);
		if (pixels == nullptr)
		{
			EMBER_ERROR("{}: '{}': {}", load.path(), image_name, stbi_failure_reason());
			return false;
		}

		const Image image{
			.extent = {static_cast<u32>(width), static_cast<u32>(height)},
			.rgba	= {pixels, size_t{static_cast<u32>(width)} * static_cast<u32>(height) * 4},
		};
		const bool parsed = parse_sheet(text, image, out.sheet, error);
		stbi_image_free(pixels);

		if (!parsed)
			EMBER_ERROR("{}: {}", load.path(), error);
		return parsed;
	}

	// Plain data, swapped whole: what a frame copied out of the old one lives until the manager unloads `fresh`.
	void SheetAsset::reload(AssetServices&, SheetAsset& live, SheetAsset& fresh) noexcept
	{
		std::swap(live.sheet, fresh.sheet);
		std::swap(live.texture, fresh.texture);
	}

	void SheetAsset::unload(AssetServices&, SheetAsset&) noexcept {}

	bool RigAsset::load(AssetLoad& load, RigAsset& out) noexcept
	{
		String error(&load.heap());
		if (parse_rig(text_of(load), out.rig, error))
			return true;

		EMBER_ERROR("{}: {}", load.path(), error);
		return false;
	}

	void RigAsset::reload(AssetServices&, RigAsset& live, RigAsset& fresh) noexcept { std::swap(live.rig, fresh.rig); }

	void RigAsset::unload(AssetServices&, RigAsset&) noexcept {}

	void register_types(AssetManager& assets) noexcept
	{
		assets.register_type<SheetAsset>("sheet");
		assets.register_type<RigAsset>("anim");
	}

	u16 Library::sheet_id(const char* path) noexcept
	{
		if (path == nullptr)
			return NO_ID;

		const auto [found, added] = m_sheet_ids.try_emplace(hash_text(path), static_cast<u16>(m_sheets.size()));
		if (added)
			m_sheets.push_back(m_assets->load<SheetAsset>(path));
		return found->second;
	}

	u16 Library::rig_id(const char* path) noexcept
	{
		if (path == nullptr)
			return NO_ID;

		const auto [found, added] = m_rig_ids.try_emplace(hash_text(path), static_cast<u16>(m_rigs.size()));
		if (added)
		{
			m_rigs.push_back(m_assets->load<RigAsset>(path));
			m_defaults.emplace_back();
		}
		return found->second;
	}

	void Library::refresh() noexcept
	{
		// A lookup per slot of each rig, not of each entity: a few dozen, and nothing to keep in step.
		for (size_t id = 0; id < m_rigs.size(); ++id)
		{
			m_defaults[id].clear();
			if (const Rig* loaded = rig(static_cast<u16>(id)))
				for (const SlotDefault& slot : loaded->slots)
					m_defaults[id].push_back({slot.slot, sheet_id(slot.sheet.c_str())});
		}
	}

	const Sheet* Library::sheet(u16 id) const noexcept
	{
		const SheetAsset* asset = id < m_sheets.size() ? m_sheets[id].get() : nullptr;
		return asset != nullptr ? &asset->sheet : nullptr;
	}

	const Rig* Library::rig(u16 id) const noexcept
	{
		const RigAsset* asset = id < m_rigs.size() ? m_rigs[id].get() : nullptr;
		return asset != nullptr ? &asset->rig : nullptr;
	}

	u16 Library::default_sheet(u16 rig, Name slot) const noexcept
	{
		const u16* id = rig < m_defaults.size() ? find(m_defaults[rig], slot) : nullptr;
		return id != nullptr ? *id : NO_ID;
	}

	TextureHandle Library::texture(u16 sheet) const noexcept
	{
		const SheetAsset* asset = sheet < m_sheets.size() ? m_sheets[sheet].get() : nullptr;
		return asset != nullptr && asset->texture ? asset->texture->texture : TextureHandle{};
	}
}
