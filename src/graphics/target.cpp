#include "target.h"
#include "graphics/render_device.h"

using namespace Ember;

Target::Target(glm::uvec2 size, std::initializer_list<TextureFormat> attachments)
	: Target(RenderDevice::instance(), size, attachments) {}

Target::Target(RenderDevice* gpu, glm::uvec2 size, std::initializer_list<TextureFormat> attachments) : m_gpu(gpu), m_size(size) {
    EMBER_ASSERT(attachments.size() > 0);
    EMBER_ASSERT(size.x > 0 && size.y > 0);

    m_attachments.reserve(attachments.size());

    // Create backing Texture attachments
    for (auto format : attachments) {
        auto handle = gpu->create_texture({
            .size = size,
            .format = format,
            .is_target_attachment = true
        });

        m_attachments.push_back(handle);
    }
}


Target::~Target() {
	// If the render device is already disposed, it will free GPU resources in its own teardown.
	// Avoid calling into a dead device (and avoid double-free if the device is tearing down).
	if (!m_gpu || RenderDevice::instance() != m_gpu) {
		return;
	}

    // Dispose backing Texture attachments
    for (auto attachment : m_attachments) {
        m_gpu->dispose_texture(attachment);
    }
}
