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

	/// The error material type, drawn for a null or stale material handle and for a type until its
	/// first compile succeeds: its SPIR-V and its .type file, which material::read_cooked joins.
	[[nodiscard]] Span<const u8> error_material_spirv() noexcept;
	[[nodiscard]] Span<const u8> error_material_type() noexcept;
}
