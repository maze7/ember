#include "graphics/texture.h"
#include "graphics/render_device.h"
#include "stb_image.h"

using namespace Ember;

Texture::Texture(Texture&& other) noexcept
	: m_gpu(other.m_gpu)
	, m_handle(other.m_handle)
	, m_size(other.m_size)
	, m_format(other.m_format) {

	other.m_handle = Handle<Texture>::null; // Nullify source so it doesn't destroy the GPU resource.
}

Texture::Texture(u32 width, u32 height, TextureFormat format, Target* target)
	: Texture(RenderDevice::instance(), width, height, format, target) {}

Texture::Texture(RenderDevice* gpu, u32 width, u32 height, TextureFormat format, Target* target) : m_gpu(gpu), m_size(width, height), m_format(format) {
	m_handle = gpu->create_texture({
		.size = { width, height },
		.format = format,
	});
}

Texture::Texture(u32 width, u32 height, std::span<u8> pixels)
	: Texture(RenderDevice::instance(), width, height, pixels) {}

Texture::Texture(RenderDevice* gpu, u32 width, u32 height, std::span<u8> pixels) : m_gpu(gpu), m_size(width, height), m_format(TextureFormat::R8G8B8A8) {
	m_handle = gpu->create_texture({
		.size = { width, height },
		.format = m_format,
		.data = pixels,
	});
}

Texture& Texture::operator=(Texture&& other) noexcept {
	if (this != &other) {
		// Clean up current GPU resource (only if the creating device is still alive)
		if (m_gpu && !m_handle.is_null() && RenderDevice::instance() == m_gpu) {
			m_gpu->dispose_texture(m_handle);
		}

		// Steal data
		m_gpu = other.m_gpu;
		m_handle = other.m_handle;
		m_format = other.m_format;
		m_size = other.m_size;

		// Nullify source
		other.m_handle = Handle<Texture>::null;
	}

	return *this;
}

Texture::~Texture() {
	if (!m_handle.is_null() && m_gpu && RenderDevice::instance() == m_gpu) {
		m_gpu->dispose_texture(m_handle);
	}
}

Ref<Texture> Texture::load(std::string_view path) {
	int width, height, channels;

	// load the image data using stb_image
	stbi_uc* data = stbi_load(path.data(), &width, &height, &channels, 4);
	if (!data) {
		throw Exception(fmt::format("Failed to load texture: {}", path));
	}

	// Create the Texture
	auto tex = make_ref<Texture>(width, height, std::span(data, width * height * 4));

	stbi_image_free(data);

	return tex;
}

void Texture::reload(Asset&& other) {
	// Dynamic cast to ensure we are reloading a texture with a texture
	if (auto* other_tex = dynamic_cast<Texture*>(&other)) {
		if (!m_gpu || RenderDevice::instance() != m_gpu) {
			// Device already disposed or mismatched; can't safely hot-reload GPU resources.
			return;
		}

		// Wait for GPU to finish using the old texture before we kill the handle slot
		m_gpu->wait_idle();

		// Use the move assignment operator to overwrite
		*this = std::move(*other_tex);
	}
}
