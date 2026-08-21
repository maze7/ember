#pragma once

#include <ember/gpu/device.h>

namespace ember::gpu
{
	/**
	 * Forces every DeviceDef field that sizes a fixed array or the bindless heap
	 * into the bounds published by gpu/common.h. Pure and backend-independent so
	 * one contract governs every backend.
	 *
	 * Clamps and warns invalid fields.
	 */
	[[nodiscard]] DeviceDef validated(const DeviceDef& def) noexcept;
}
