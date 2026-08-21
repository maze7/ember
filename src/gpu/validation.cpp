#include "ember/gpu/common.h"
#include <ember/core/logger.h>
#include <gpu/validation.h>

#include <algorithm>

namespace ember::gpu
{
	/// Clamps one def field into [lo, hi], logging when the caller's value was out of contract.
	[[nodiscard]] u32 validate(u32 value, u32 lo, u32 hi, const char* name) noexcept
	{
		u32 result = std::clamp(value, lo, hi);

		if (result != value)
			EMBER_WARN("gpu: DeviceDef {} = {} outside [{}, {}]; using {}", name, value, lo, hi, result);

		return result;
	}

	DeviceDef validated(const DeviceDef& raw) noexcept
	{
		DeviceDef def = raw;

		def.frames_in_flight = validate(def.frames_in_flight, 1, MAX_FRAMES_IN_FLIGHT, "frames_in_flight");

		DeviceLimits& limits  = def.limits;
		limits.max_swapchains = validate(limits.max_swapchains, 1, MAX_SWAPCHAINS, "limits.max_swapchains");
		limits.max_samplers	  = validate(limits.max_samplers, 1, MAX_BINDLESS_SAMPLERS, "limits.max_samplers");

		// A handle's index is u16: a pool past 65535 could mint slots no handle can address.
		constexpr u32 MAX_POOL = 65535;
		limits.max_buffers	   = validate(limits.max_buffers, 1, MAX_POOL, "limits.max_buffers");
		limits.max_textures	   = validate(limits.max_textures, 1, MAX_POOL, "limits.max_textures");
		limits.max_graphics_pipelines =
			validate(limits.max_graphics_pipelines, 1, MAX_POOL, "limits.max_graphics_pipelines");
		limits.max_compute_pipelines =
			validate(limits.max_compute_pipelines, 1, MAX_POOL, "limits.max_compute_pipelines");

		return def;
	}
}
