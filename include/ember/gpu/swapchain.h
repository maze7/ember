#pragma once

#include <ember/core/common.h>
#include <ember/core/handle.h>

namespace ember
{
	/// Mirror of platform/window.h's declaration so this header does not drag
	/// the platform layer's includes into GPU user code.
	struct Window;
	using WindowHandle = Handle<Window>;
}

namespace ember::gpu
{
	enum class PresentMode : u8
	{
		VSync,	   // FIFO: always available, never tears.
		Mailbox,   // low-latency triple buffer; falls back to VSync with a log
		Immediate, // may tear; falls back to VSync with a log
	};

	struct SwapchainDef
	{
		WindowHandle window{};
		PresentMode present_mode = PresentMode::VSync;
		u32 image_count			 = 3; // clamped to the surface's [min, max]
	};
}
