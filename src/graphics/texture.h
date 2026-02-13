#pragma once

#include "assets/asset.h"
#include "core/handle.h"
#include "core/hash.h"
#include "core/common.h"
#include "ext/vector_uint2.hpp"
#include "graphics/enums/texture_filter.h"
#include "graphics/enums/texture_format.h"
#include "graphics/enums/texture_wrap.h"
#include <span>

namespace Ember
{
	// Forward Declaration of Target
	struct Target;
	class RenderDevice;

	// @brief Struct describing a TextureSampler
	struct TextureSampler
	{
	    TextureFilter filter = TextureFilter::Nearest;
		TextureWrap wrap_x = TextureWrap::Repeat;
		TextureWrap wrap_y = TextureWrap::Repeat;

		auto operator<=>(const TextureSampler&) const = default;
	};

	/// @brief High-level wrapper of a GPU Texture. Provides a nicer API than the RenderDevice
	class Texture : public Asset
	{
	public:
		Texture() = default;
		Texture(Texture&& other) noexcept;
		Texture(u32 width, u32 height, TextureFormat format = TextureFormat::Color, Target* target = nullptr);
		Texture(RenderDevice* gpu, u32 width, u32 height, TextureFormat format = TextureFormat::Color, Target* target = nullptr);
		Texture(u32 width, u32 height, std::span<u8> pixels);
		Texture(RenderDevice* gpu, u32 width, u32 height, std::span<u8> pixels);
		~Texture();

		// Move assignment
		Texture& operator=(Texture&& other) noexcept;

		[[nodiscard]] Handle<Texture> handle() const { return m_handle; }
		[[nodiscard]] TextureFormat format() const { return m_format; }

		[[nodiscard]] auto size() const { return m_size; }
		[[nodiscard]] auto width() const { return m_size.x; }
		[[nodiscard]] auto height() const { return m_size.y; }

		[[nodiscard]] u32 memory_size() const {
			return m_size.x * m_size.y * texture_format_size(m_format);
		}

		static Ref<Texture> load(std::string_view path);
		static constexpr AssetType asset_type() { return AssetType::Texture; }

		void reload(Asset&& other) override;

	private:
		RenderDevice* m_gpu = nullptr;
		Handle<Texture> m_handle;
		TextureFormat m_format = TextureFormat::Color;
		glm::uvec2 m_size{ 0, 0 };
	};
}

template<>
struct std::hash<Ember::TextureSampler>
{
	size_t operator()(const Ember::TextureSampler& sampler) const noexcept {
		return combined_hash(
			sampler.filter,
			sampler.wrap_x,
			sampler.wrap_y
		);
	}
};
