#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>

namespace ember::render::embedded
{
	/// Engine-cooked SPIR-V linked into the library: the defaults features
	/// use when a Def carries no shader override.
	[[nodiscard]] Span<const u8> cull_shader() noexcept;
	[[nodiscard]] Span<const u8> upscale_shader() noexcept;

	/// The engine's material types, each as its SPIR-V and its .type file, which material::read_cooked
	/// joins. The registry loads them at init: the error type, drawn for a null or stale material
	/// handle and for a type until its first compile succeeds, and the stock types.
	[[nodiscard]] Span<const u8> error_material_spirv() noexcept;
	[[nodiscard]] Span<const u8> error_material_type() noexcept;
	[[nodiscard]] Span<const u8> unlit_material_spirv() noexcept;
	[[nodiscard]] Span<const u8> unlit_material_type() noexcept;
	[[nodiscard]] Span<const u8> sprite_material_spirv() noexcept;
	[[nodiscard]] Span<const u8> sprite_material_type() noexcept;
}
