#include "render_device.h"

#if EMBER_USE_SDL
	#include "sdl/render_device_sdl.h"
#endif // EMBER_USE_SDL

using namespace Ember;

Unique<RenderDevice> RenderDevice::s_instance = nullptr;

void RenderDevice::init(Window& window) {
	EMBER_ASSERT(s_instance == nullptr);

#if EMBER_USE_SDL
	s_instance = make_unique<RenderDeviceSDL>(window);
#endif // EMBER_USE_SDL
}

void RenderDevice::dispose() {
	s_instance.reset();
}

RenderDevice* RenderDevice::instance() {
	return s_instance.get();
}
