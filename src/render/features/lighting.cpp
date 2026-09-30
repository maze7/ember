#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/math/color.h>
#include <ember/render/features/lighting.h>

#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace ember::render
{
	bool make_shadow_view(const View& camera, glm::vec3 direction, const ShadowFit& fit, View& out) noexcept
	{
		// The camera's frustum, near corners then far, x before y so the edges below index them.
		// Reverse Z puts the near plane at depth one and the far plane at zero.
		const glm::mat4 inverse = glm::inverse(camera.view_projection);
		const glm::vec3 forward = view_forward(camera.view);
		const bool orthographic = camera.projection[2][3] == 0.0f;

		glm::vec3 corners[8];

		for (u32 i = 0; i < 4; ++i)
		{
			const glm::vec2 ndc{(i & 1) != 0 ? 1.0f : -1.0f, (i & 2) != 0 ? 1.0f : -1.0f};
			const glm::vec4 near = inverse * glm::vec4(ndc, 1.0f, 1.0f);
			const glm::vec4 far	 = inverse * glm::vec4(ndc, 0.0f, 1.0f);

			corners[i] = glm::vec3(near) / near.w;

			// An orthographic view is a finite box, and the heights bound what matters of it.
			if (orthographic)
			{
				corners[i + 4] = glm::vec3(far) / far.w;
				continue;
			}

			// A perspective view reaches the horizon, where an infinite far plane unprojects to a
			// direction (w = 0), so its shadows stop at the fit's distance.
			const bool infinite = std::abs(far.w) < 1e-6f;
			glm::vec3 ray		= infinite ? glm::vec3(far) : glm::vec3(far) / far.w - corners[i];
			const f32 length	= infinite ? fit.distance : std::min(glm::length(ray), fit.distance);

			if (glm::dot(ray, forward) < 0.0f)
				ray = -ray;

			corners[i + 4] = corners[i] + glm::normalize(ray) * length;
		}

		// The part of the frustum between the heights: its corners inside them, and where its edges
		// cross them. A convex solid cut by two planes has no other vertices.
		constexpr u8 EDGES[12][2] = {{0, 4}, {1, 5}, {2, 6}, {3, 7}, {0, 1}, {1, 3},
									 {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}};

		glm::vec3 points[8 + 12 * 2];
		u32 count = 0;

		for (const glm::vec3& corner : corners)
			if (corner.y >= fit.min_height && corner.y <= fit.max_height)
				points[count++] = corner;

		for (const auto& [from, to] : EDGES)
		{
			const glm::vec3 a = corners[from];
			const glm::vec3 b = corners[to];

			for (const f32 height : {fit.min_height, fit.max_height})
				if ((a.y - height) * (b.y - height) < 0.0f)
					points[count++] = a + (b - a) * ((height - a.y) / (b.y - a.y));
		}

		if (count == 0)
			return false;

		// The light's frame depends on its direction alone, so the texel grid never turns with the
		// camera. Any up that is not the direction itself will do.
		const glm::vec3 up = std::abs(direction.z) < 0.99f ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
		const glm::mat4 rotation = glm::lookAt(glm::vec3(0.0f), direction, up);

		glm::vec3 low(std::numeric_limits<f32>::max());
		glm::vec3 high(std::numeric_limits<f32>::lowest());

		for (u32 i = 0; i < count; ++i)
		{
			const glm::vec3 point = glm::vec3(rotation * glm::vec4(points[i], 1.0f));
			low					  = glm::min(low, point);
			high				  = glm::max(high, point);
		}

		// Square, a texel of margin a side, then rounded up to a step near an eighth of itself.
		// Panning only moves what the camera sees, so the size holds through the float noise that
		// moving leaves in the bounds, and with it the texel's size.
		const f32 needed = std::max(high.x - low.x, high.y - low.y) * (1.0f + 2.0f / static_cast<f32>(fit.resolution));
		const f32 step	 = std::exp2(std::floor(std::log2(std::max(needed, 1e-3f))) - 3.0f);
		const f32 extent = std::ceil(needed / step) * step;
		const f32 texel	 = extent / static_cast<f32>(fit.resolution);

		// The centre moves in whole texels, so the grid stays put in the world.
		const glm::vec2 centre = glm::floor(glm::vec2(low + high) * 0.5f / texel + 0.5f) * texel;

		// Depth spans the receivers, with a texel of margin. Casters nearer the light clamp to the near
		// plane rather than clip, so it need not reach them.
		const f32 near = -high.z - texel;
		const f32 far  = -low.z + texel;
		const f32 half = extent * 0.5f;

		const glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(-centre, 0.0f)) * rotation;

		out = make_view(view, ortho_reverse_z(half, half, near, far), {fit.resolution, fit.resolution}, LAYER_ALL,
						"shadow");
		out.required = ObjectFlags::CastsShadow;

		// No near plane for the cull either. Under reverse Z it is plane five; a plane with no normal
		// and positive w passes every sphere.
		out.frustum.planes[5] = {0.0f, 0.0f, 0.0f, 1.0f};
		return true;
	}

	glm::mat4 shadow_map_matrix(const View& shadow, u32 resolution) noexcept
	{
		// The projection's x scale is 2 / extent and its depth scale 1 / (far - near), which make half
		// a texel of world into depth.
		const f32 texel = 2.0f / (shadow.projection[0][0] * static_cast<f32>(resolution));
		const f32 bias	= 0.5f * texel * shadow.projection[2][2];

		// Clip space to the map: x from [-1, 1] to u in [0, 1]; y flips, since the viewport is flipped
		// and a texture's row zero is its top; depth moves toward the light, which reverse Z calls up.
		glm::mat4 to_map(1.0f);
		to_map[0][0] = 0.5f;
		to_map[1][1] = -0.5f;
		to_map[3]	 = {0.5f, 0.5f, bias, 1.0f};

		return to_map * shadow.view_projection;
	}

	LightingFeature::LightingFeature(Renderer&, const Def& def) noexcept : m_fit(def.shadow)
	{
		set_ambient(def.sky, def.ground);
		set_bands(def.bands, def.dither);

		m_constants.dither_cell = std::max(def.dither_cell, 1e-6f);
	}

	void LightingFeature::shutdown(gpu::Device& device) noexcept { device.destroy(std::exchange(m_shadow_map, {})); }

	void LightingFeature::prepare(RenderFrame& frame) noexcept
	{
		LightingConstants& lighting = frame.constants.lighting;
		lighting					= m_constants;

		const u32 count = frame.scene.light_count();

		if (count == 0)
			return;

		// The lights gather in the frame's scratch, where naming one's shadow map is a write to cached
		// memory, then reach the ring in one sequential copy.
		auto* lights =
			static_cast<LightData*>(frame.scratch.allocate_fast(count * sizeof(LightData), alignof(LightData)));
		const u32 copied = frame.scene.copy_lights({lights, count});

		// One shadow map for now, for the first directional light that asks: the sun.
		for (u32 i = 0; i < copied; ++i)
		{
			if ((lights[i].flags & LIGHT_CASTS_SHADOW) != 0 && lights[i].range == 0.0f)
			{
				lights[i].shadow = shadow(frame, lights[i].direction) ? 1 : 0;
				break;
			}
		}

		// Written once, front to back, which is all write-combined memory tolerates. The frame's
		// submit publishes it and the frame's retirement frees it.
		const gpu::TransientArray<LightData> table = frame.device.transient().allocate_array<LightData>(copied);

		if (!table.valid()) [[unlikely]]
		{
			EMBER_ERROR("transient ring exhausted; no lights this frame");
			return;
		}

		std::memcpy(table.data, lights, copied * sizeof(LightData));

		lighting.light_count = copied;
		lighting.lights		 = bindless_index(table.buffer);
		lighting.first_light = table.first_element();
	}

	bool LightingFeature::shadow(RenderFrame& frame, glm::vec3 direction) noexcept
	{
		View view;

		if (frame.resources.shadow_map_count == MAX_SHADOW_MAPS ||
			!make_shadow_view(frame.views[0], direction, m_fit, view))
			return false;

		if (m_shadow_map.is_null())
		{
			m_shadow_map = frame.device.create_texture({
				.name	= "lighting.shadow_map",
				.extent = {m_fit.resolution, m_fit.resolution, 1},
				.format = SHADOW_MAP_FORMAT,
				.usage	= gpu::TextureUsage::DepthStencilTarget | gpu::TextureUsage::Sampled,
			});

			if (m_shadow_map.is_null()) // the device logged why
				return false;
		}

		const gpu::TransientArray<ShadowData> table = frame.device.transient().allocate_array<ShadowData>(1);

		if (!table.valid()) [[unlikely]]
			return false;

		// Id zero is the main view's: what a full frame hands back after its assert.
		const u32 id = frame.add_view(view);

		if (id == 0) [[unlikely]]
			return false;

		table.data[0] = {
			.world_to_map = shadow_map_matrix(view, m_fit.resolution),
			.map		  = bindless_index(m_shadow_map),
			.size		  = static_cast<f32>(m_fit.resolution),
		};

		frame.constants.lighting.shadows	  = bindless_index(table.buffer);
		frame.constants.lighting.first_shadow = table.first_element();

		// Its contents are this frame's alone, drawn before anything reads them, so it enters undefined.
		frame.resources.shadow_maps[frame.resources.shadow_map_count++] = {
			.view	 = id,
			.texture = frame.graph.import(m_shadow_map, gpu::TextureState::Undefined, gpu::TextureState::ShaderRead,
										  {m_fit.resolution, m_fit.resolution}),
		};

		return true;
	}

	void LightingFeature::set_ambient(glm::vec3 sky, glm::vec3 ground) noexcept
	{
		m_constants.sky	   = linear_from_srgb(sky);
		m_constants.ground = linear_from_srgb(ground);
	}

	void LightingFeature::set_bands(u32 bands, f32 dither) noexcept
	{
		m_constants.bands  = bands;
		m_constants.dither = std::clamp(dither, 0.0f, 1.0f);
	}
}
