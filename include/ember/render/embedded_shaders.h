#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>

namespace ember::render::embedded
{
	/// Engine-cooked SPIR-V linked into the library: the defaults features
	/// use when a Def carries no shader override.
	[[nodiscard]] Span<const u8> cull_shader() noexcept;
	[[nodiscard]] Span<const u8> mesh_shader() noexcept;
	[[nodiscard]] Span<const u8> sprite_shader() noexcept;
	[[nodiscard]] Span<const u8> upscale_shader() noexcept;
}
