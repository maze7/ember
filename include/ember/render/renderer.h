#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/render/geometry.h>
#include <ember/render/gpu_scene.h>
#include <ember/render/graph.h>
#include <ember/render/scene.h>
#include <ember/render/view.h>
#include <ember/render/visibility.h>

#include <type_traits>

namespace ember::gpu
{
	class Device;
}

namespace ember::render
{
	class Renderer;

	/// Views per frame; matched to the readback query slots so every view's
	/// count is capturable.
	inline constexpr u32 MAX_FRAME_VIEWS = VISIBILITY_QUERY_SLOTS;

	/**
	 * Semantic frame resources. Producers assign, consumers read; a null handle
	 * means no registered feature produces it this frame, and optional
	 * consumers fall back while required consumers assert at the use site.
	 * Members are Ember-standard semantics only; game features share their own
	 * data through their constructors, never through this struct.
	 */
	struct FrameResources
	{
		GraphTexture output		 = {}; // imported render target; the last feature writes it
		GraphTexture scene_color = {}; // shading output once an intermediate target exists
		GraphTexture scene_depth = {}; // published by whichever feature owns the depth target

		Extent2D output_extent = {}; // sizes feature-created targets that match the output

		/// Internal render resolution: what world features size their targets
		/// by. Defaults to the output extent; an upscale feature overrides it
		/// when the scene renders through an internal target.
		Extent2D scene_extent = {};
	};

	/**
	 * One frame as features see it. Built views are culled between build_views
	 * and add_passes, so visibility[i] answers for views[i] by the time passes
	 * are declared. View 0 is always the main view.
	 */
	struct RenderFrame
	{
		gpu::Device& device;
		RenderScene& scene;
		GpuScene& gpu_scene;
		GeometryPool& geometry;
		RenderGraph& graph;

		u32 frame_slot = 0;

		FrameResources resources = {};

		View views[MAX_FRAME_VIEWS]				   = {};
		ViewVisibility visibility[MAX_FRAME_VIEWS] = {};
		u32 view_count							   = 0;
		bool views_locked						   = false;

		/// build_views phase only. Returns the view's id, which is also its
		/// visibility and readback query slot. A full frame returns id 0 in
		/// release so a runaway feature aliases the main view instead of
		/// indexing garbage.
		u32 add_view(const View& view) noexcept
		{
			EMBER_ASSERT(!views_locked && "views are fixed once culling starts");
			EMBER_ASSERT(view_count < MAX_FRAME_VIEWS);

			if (views_locked || view_count >= MAX_FRAME_VIEWS) [[unlikely]]
				return 0;

			views[view_count] = view;
			return view_count++;
		}
	};

	/**
	 * One composable unit of rendering policy, usually one shader family with
	 * its passes and material storage. Features declare passes onto the graph
	 * and communicate through FrameResources or their own construction time
	 * wiring; registration order is pass order, and that ordering is the
	 * contract games control in one place.
	 *
	 * Dynamic dispatch is deliberate here: features compose at frame graph
	 * granularity, an open set of game-provided types, which is the cold
	 * boundary virtual calls exist for.
	 */
	class RenderFeature
	{
	public:
		virtual ~RenderFeature() = default;

		/// Publish frame targets (scene color, bloom chains) before any
		/// feature declares passes. Runs for every feature ahead of build_views.
		virtual void prepare(RenderFrame&) noexcept {}

		/// Releases GPU resources the feature owns; runs before destruction in
		/// reverse registration order.
		virtual void shutdown(gpu::Device&) noexcept {}

		/// Contribute secondary views (shadow cascades, reflections).
		virtual void build_views(RenderFrame&) noexcept {}

		/// Declare graph passes. Every view is culled by now.
		virtual void add_passes(RenderFrame&) noexcept = 0;
	};

	struct RendererDef
	{
		u32 object_capacity		 = 1u << 17;
		u32 command_capacity	 = 1u << 17;
		GeometryPoolDef geometry = {};

		/// Cooked cull kernel override; empty uses the engine's embedded shaders/cull.slang.
		Span<const u8> cull_shader = {};
	};

	[[nodiscard]] constexpr bool is_valid(const RendererDef& def) noexcept
	{
		return def.object_capacity != 0 && def.command_capacity != 0 && is_valid(def.geometry);
	}

	struct RenderOutput
	{
		TextureHandle texture		  = {};
		Extent2D extent				  = {};
		gpu::TextureState initial	  = gpu::TextureState::Undefined;
		gpu::TextureState final_state = gpu::TextureState::Present;
	};

	/**
	 * The render domain's owner and orchestrator: the proxy scene, the GPU
	 * mirrors, geometry, visibility, the graph and the registered features all
	 * live here, and render() runs the fixed phase contract over them every
	 * frame: prepare, build views, cull every view, add passes in registration
	 * order, capture counts, execute.
	 *
	 * Renderer policy is which features a game registers and how it configures
	 * them; the mechanisms are never optional. Features own their shader
	 * family state, material pools included. The game supplies per frame
	 * inputs (view, output) and reaches owned state through accessors; hand
	 * sim code the RenderScene& at wiring time so gameplay includes stay at
	 * scene.h.
	 */
	class Renderer
	{
	public:
		Renderer() noexcept;

		Renderer(const Renderer&)			 = delete;
		Renderer& operator=(const Renderer&) = delete;

		/// Creates the scene and the owned mechanisms. Runs once, before any
		/// add_feature or geometry create.
		void init(gpu::Device& device, const RendererDef& def) noexcept;

		/// Shuts features down in reverse registration order, then the
		/// mechanisms. Geometries must be destroyed first; the pool asserts.
		void shutdown(gpu::Device& device) noexcept;

		/**
		 * Constructs F(device, def), registers it last in pass order, and
		 * returns it typed so the game keeps a handle for runtime knobs.
		 * F declares a nested Def.
		 */
		template <class F> F& add_feature(const typename F::Def& def = {}) noexcept
		{
			static_assert(std::is_base_of_v<RenderFeature, F>, "features implement RenderFeature");
			EMBER_ASSERT(m_device != nullptr && "add features after init");

			F* feature = memory::new_object<F>(MemoryTag::Graphics, *this, def);

			m_features.push_back({
				.feature = feature,
				.destroy = [](RenderFeature* base)
				{ memory::delete_object(MemoryTag::Graphics, static_cast<F*>(base)); },
			});

			return *feature;
		}

		void render(const View& main_view, const RenderOutput& output, u32 frame_slot) noexcept;

		/// Debug: view 0 culls with this view while it is set, and features keep
		/// rasterizing the main view. The freeze harness. The pointee outlives
		/// the setting.
		void set_cull_override(const View* view) noexcept { m_cull_override = view; }

		/// The proxy scene games mutate.
		[[nodiscard]] RenderScene& scene() noexcept { return m_scene; }
		[[nodiscard]] const RenderScene& scene() const noexcept { return m_scene; }

		/// Geometry lives here because culling reads its table; asset code
		/// creates and destroys through this accessor.
		[[nodiscard]] GeometryPool& geometry() noexcept { return m_geometry; }
		[[nodiscard]] const GeometryPool& geometry() const noexcept { return m_geometry; }

		/// Stats surfaces for debug UI.
		[[nodiscard]] const GpuScene& gpu_scene() const noexcept { return m_gpu_scene; }
		[[nodiscard]] u32 visible_count(u32 frame_slot, u32 view = 0) const noexcept
		{
			return m_readback.value(frame_slot, view);
		}

		[[nodiscard]] gpu::Device& gpu() noexcept
		{
			EMBER_ASSERT(m_device != nullptr);
			return *m_device;
		}

		/// Builtin conveniences beside the heap's slot 0 error fallback: the
		/// art every game asks for by name instead of loading.
		[[nodiscard]] TextureHandle white_texture() const noexcept { return m_white; }
		[[nodiscard]] SamplerHandle point_sampler() const noexcept { return m_point_sampler; }
		[[nodiscard]] SamplerHandle linear_sampler() const noexcept { return m_linear_sampler; }

	private:
		struct FeatureEntry
		{
			RenderFeature* feature = nullptr;

			// delete_object through the base pointer would hand the allocator
			// the base type's size; the thunk downcasts so size and alignment
			// stay honest.
			void (*destroy)(RenderFeature*) = nullptr;
		};

		gpu::Device* m_device = nullptr;

		RenderScene m_scene;
		GeometryPool m_geometry;
		GpuScene m_gpu_scene;
		Visibility m_visibility;
		VisibilityReadback m_readback;
		RenderGraph m_graph;

		TextureHandle m_white		   = {};
		SamplerHandle m_point_sampler  = {};
		SamplerHandle m_linear_sampler = {};

		Vector<FeatureEntry> m_features;
		const View* m_cull_override = nullptr;
	};
}
