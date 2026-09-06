#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>

namespace ember::render::embedded
{
	/// Engine-cooked SPIR-V linked into the library.
	[[nodiscard]] Span<const u8> imgui_shader() noexcept;
}
