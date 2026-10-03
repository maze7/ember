#pragma once

#include <ember/anim/parse.h>
#include <ember/anim/sample.h>

#include <gtest/gtest.h>

#include <vector>

// What the animation tests share: images made in memory, files parsed from text, and a Source over
// what they parsed, so a test samples exactly what a game would without an asset manager or a GPU.
namespace ember::anim::test
{
	/** An RGBA8 image, clear, with opaque rectangles painted on. */
	struct Canvas
	{
		Extent2D extent;
		std::vector<u8> rgba;

		Canvas(u32 width, u32 height) : extent{width, height}, rgba(size_t{width} * height * 4, 0) {}

		Canvas& fill(u32 x0, u32 y0, u32 x1, u32 y1)
		{
			for (u32 y = y0; y < y1; ++y)
				for (u32 x = x0; x < x1; ++x)
					rgba[(size_t{y} * extent.width + x) * 4 + 3] = 255;
			return *this;
		}

		[[nodiscard]] Image image() const { return {.extent = extent, .rgba = {rgba.data(), rgba.size()}}; }
	};

	[[nodiscard]] inline Sheet sheet(StringView text, const Canvas& canvas)
	{
		Sheet sheet;
		String error;
		EXPECT_TRUE(parse_sheet(text, canvas.image(), sheet, error)) << error;
		return sheet;
	}

	[[nodiscard]] inline Rig rig(StringView text)
	{
		Rig rig;
		String error;
		EXPECT_TRUE(parse_rig(text, rig, error)) << error;
		return rig;
	}

	/** Sheets and rigs by id, each rig's default sheets by slot, as a library resolves them. */
	class Assets final : public Source
	{
	public:
		u16 add(Sheet sheet)
		{
			m_sheets.push_back(std::move(sheet));
			return static_cast<u16>(m_sheets.size() - 1);
		}

		/** A rig, its slots' default sheets given in the order it names them. */
		u16 add(Rig rig, std::vector<u16> defaults = {})
		{
			Vector<Named<u16>> slots;
			for (size_t i = 0; i < defaults.size(); ++i)
				slots.push_back({rig.slots[i].slot, defaults[i]});

			m_rigs.push_back(std::move(rig));
			m_defaults.push_back(std::move(slots));
			return static_cast<u16>(m_rigs.size() - 1);
		}

		[[nodiscard]] const Sheet* sheet(u16 id) const noexcept override
		{
			return id < m_sheets.size() ? &m_sheets[id] : nullptr;
		}
		[[nodiscard]] const Rig* rig(u16 id) const noexcept override
		{
			return id < m_rigs.size() ? &m_rigs[id] : nullptr;
		}

		[[nodiscard]] u16 default_sheet(u16 rig, Name slot) const noexcept override
		{
			const u16* id = rig < m_defaults.size() ? find(m_defaults[rig], slot) : nullptr;
			return id != nullptr ? *id : NO_ID;
		}

	private:
		std::vector<Sheet> m_sheets;
		std::vector<Rig> m_rigs;
		std::vector<Vector<Named<u16>>> m_defaults;
	};

	/** An entity's pose at `now`, the frame before it `dt` earlier. */
	[[nodiscard]] inline Pose pose_at(const Source& source, const Animator& animator, const Look& look, f64 now,
									  f64 dt = 1.0 / 60.0)
	{
		Pose pose;
		sample(source, animator, look, now, now - dt, pose);
		return pose;
	}
}
