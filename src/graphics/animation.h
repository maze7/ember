#pragma once

#include "graphics/sub_texture.h"

namespace Ember
{
	struct Animation
	{
		bool is_playing = true;
		bool is_looping = true;
		u32 frame_index = 0;
		float frame_counter = 0;
		std::vector<SubTexture> frames;
		std::vector<float> frame_durations;

		void update(float dt) {
			if (frames.empty() || frame_durations.empty()) return;

			auto& frame = frames[frame_index];
			auto duration = frame_durations[frame_index];

			// Increment frame counter
			frame_counter += dt;

			// move to next frame after duration
			while (frame_counter >= duration) {
				frame_counter -= duration;

				// increment frame, move back if we're at the end
				frame_index++;
				if (frame_index >= frames.size()) {
					frame_index = 0;
				}
			}
		}
	};
}
