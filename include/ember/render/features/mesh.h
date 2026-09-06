#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/command_list.h>
#include <ember/gpu/pipeline.h>
#include <ember/gpu/texture.h>
#include <ember/render/material.h>
#include <ember/render/renderer.h>

#include <glm/vec4.hpp>

namespace ember::render
{
	/// The unlit family's material record, mirrored in shaders/mesh.slang. The
	/// defaults are the error record: texture index zero reads the heap
	/// fallback and the white tint cannot hide it.
	struct MeshMaterial
	{
		u32 albedo		   = 0;
		u32 albedo_sampler = 0;
		u32 mip			   = 0; // explicit level; the family doubles as bring-up tooling
		u32 pad			   = 0;
		glm::vec4 tint	   = {1.0f, 1.0f, 1.0f, 1.0f};
	};

	static_assert(sizeof(MeshMaterial) == 32 && std::is_trivially_copyable_v<MeshMaterial>);

	/**
	 * The first standard shader family: opaque scene geometry, point-friendly
	 * unlit texturing, a fixed directional term keyed off the vertex normal so
	 * broken attributes show as wrong shading instead of hiding. Every game's
	 * bring-up renderer; the lit families (stylized, PBR) register instead of
	 * it, not on top of it.
	 *
	 * Owns its material pool, as every family feature does; games create and
	 * edit records through materials().
	 */
	class MeshFeature final : public RenderFeature
	{
	public:
		struct Def
		{
			/// Cooked SPIR-V override; empty uses the engine's embedded shaders/mesh.slang
			Span<const u8> shader = {};

			/// Match the target family: the swapchain format standalone,
			/// RGBA16Float under the upscale feature.
			gpu::TextureFormat color_format = gpu::TextureFormat::RGBA8Unorm;
			gpu::TextureFormat depth_format = gpu::TextureFormat::D32Float;

			gpu::ClearColor clear = {0.0f, 0.0f, 0.0f, 1.0f};

			u32 material_capacity = 256;
		};

		MeshFeature(Renderer& renderer, const Def& def) noexcept;

		void shutdown(gpu::Device& gpu) noexcept override;
		void add_passes(RenderFrame& frame) noexcept override;

		/// The family's material table; the game creates and edits through it.
		[[nodiscard]] MaterialPool& materials() noexcept { return m_materials; }

	private:
		GraphicsPipelineHandle m_pipeline = {};
		MaterialPool m_materials;

		gpu::TextureFormat m_depth_format = gpu::TextureFormat::D32Float;
		gpu::ClearColor m_clear			  = {};
	};
}
