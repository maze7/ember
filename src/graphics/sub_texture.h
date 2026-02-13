#pragma once

#include "math/rect.h"
#include "graphics/texture.h"

namespace Ember
{
	struct SubTexture
	{
		/// @brief Non-owning pointer to underlying Texture.
		Texture* texture{};

		/// @brief The source rectangle to sample from the Texture.
		Rectf source{};

		/// @brief The frame of the SubTexture.
		Rectf frame{};

		/// @brief The Texture UV coordinates.
		glm::vec2 tex_coords[4];

		/// @brief The draw coordinates.
		glm::vec2 draw_coords[4];

		constexpr float width() const {
			return frame.w;
		}

		constexpr float height() const {
			return frame.h;
		}

		constexpr glm::vec2 size() const {
			return frame.size();
		}

		constexpr SubTexture() = default;

		explicit SubTexture(Texture* tex) : SubTexture(
			tex,
			{ 0, 0, static_cast<float>(tex ? tex->width() : 0), static_cast<float>(tex ? tex->height() : 0)},
			{ 0, 0, static_cast<float>(tex ? tex->width() : 0), static_cast<float>(tex ? tex->height() : 0)}
		) {}

		SubTexture(Texture* tex, const Rectf& source_)
			: SubTexture(tex, source_, Rectf(0, 0, source_.w, source_.h)) {}

		SubTexture(Texture* tex, Rectf source_, const Rectf& frame_)
			 : texture(tex), source(source_), frame(frame_) {
			draw_coords[0] = { -frame.x, -frame.y };
			draw_coords[1] = { -frame.x + source.w, -frame.y };
			draw_coords[2] = { -frame.x + source.w, -frame.y + source.h };
			draw_coords[3] = { -frame.x, -frame.y + source.h };

			if (texture != nullptr && texture->width() > 0 && texture->height() > 0) {
				const float px = 1.0f / static_cast<float>(texture->width());
				const float py = 1.0f / static_cast<float>(texture->height());

				const float tx0 = source.x * px;
				const float ty0 = source.y * py;
				const float tx1 = source.right() * px;
				const float ty1 = source.bottom() * py;

				tex_coords[0] = { tx0, ty0 };
				tex_coords[1] = { tx1, ty0 };
				tex_coords[2] = { tx1, ty1 };
				tex_coords[3] = { tx0, ty1 };
			}
		}

		std::pair<Rectf, Rectf> get_clip(const Rectf& clip) const {
			Rectf result_src = (clip + source.position()).get_intersection(source);
			Rectf result_frame(
				min(0.0f, frame.x + clip.x),
				min(0.0f, frame.y + clip.y),
				clip.w,
				clip.h
			);

			return { result_src, result_frame };
		}

		std::pair<Rectf, Rectf> get_clip(float x, float y, float w, float h) const {
			return get_clip({ x, y, w, h });
		}

		SubTexture get_clip_sub_texture(const Rectf& clip) const {
			auto [s, f] = get_clip(clip);
			return SubTexture(texture, s, f);
		}
	};
}
