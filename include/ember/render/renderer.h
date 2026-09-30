#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/common.h>
#include <ember/memory/memory.h>
#include <ember/render/frame_constants.h>
#include <ember/render/geometry.h>
#include <ember/render/gpu_scene.h>
#include <ember/render/graph.h>
#include <ember/render/material_registry.h>
#include <ember/render/scene.h>
#include <ember/render/view.h>
#include <ember/render/visibility.h>

#include <glm/vec3.hpp>

#include <type_traits>

namespace ember::gpu
{
	class Device;
}

namespace ember::render
{
	class Renderer;

	/// Views per frame: the main view and the ones features add, shadow cascades and reflections.
	inline constexpr u32 MAX_FRAME_VIEWS = 8;

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

		/// The lights every surface reads: a lighting feature's table and count, and the ambient
		/// term, set in prepare(). Without one, surfaces see a white ambient alone, under which the
		/// Lit model shades like Unlit.
		u32 lights		  = 0;
		u32 light_count	  = 0;
		glm::vec3 ambient = {1.0f, 1.0f, 1.0f};
	};

	/**
	 * One frame as features see it. Built views are culled between build_views
	 * and add_passes, so visibility[i] and view_constants[i] answer for views[i]
	 * by the time passes are declared. View 0 is always the main view.
	 */
	struct RenderFrame
	{
		gpu::Device& device;
		RenderScene& scene;
		GpuScene& gpu_scene;
		GeometryPool& geometry;
		MaterialRegistry& materials;
		RenderGraph& graph;
		Arena& scratch;

		u32 frame_slot = 0;

		FrameResources resources = {};

		/// Bound at CONSTANTS_FRAME by every scene pass: time and the scene's tables.
		FrameConstants constants = {};

		/// Where each bucket's draws go in every view's argument buffer.
		DrawBuckets buckets = {};

		View views[MAX_FRAME_VIEWS]					  = {};
		ViewConstants view_constants[MAX_FRAME_VIEWS] = {}; // bound at CONSTANTS_PASS when drawing a view
		ViewVisibility visibility[MAX_FRAME_VIEWS]	  = {};
		u32 view_count								  = 0;
		bool views_locked							  = false;

		/// build_views phase only. Returns the view's id, its index in views,
		/// view_constants and visibility. A full frame returns id 0 in release so
		/// a runaway feature aliases the main view instead of indexing garbage.
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
	 * One composable unit of rendering policy: the world's surfaces, an upscale,
	 * an overlay, each with its passes. Features declare passes onto the graph
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
		u32 object_capacity			  = 1u << 17;
		u32 command_capacity		  = 1u << 17;
		GeometryPoolDef geometry	  = {};
		MaterialRegistryDef materials = {};

		/// Cooked cull kernel override; empty uses the engine's embedded shaders/cull.slang.
		Span<const u8> cull_shader = {};
	};

	/// Every object can be visible at once, so a view's argument buffer holds a draw for each.
	[[nodiscard]] constexpr bool is_valid(const RendererDef& def) noexcept
	{
		return def.object_capacity != 0 && def.command_capacity >= def.object_capacity && is_valid(def.geometry) &&
			   is_valid(def.materials);
	}

	/// The app's frame, as render() needs it.
	struct FrameTiming
	{
		u32 slot	   = 0;	   // the device's frame in flight, which per frame GPU resources index by
		f32 delta_time = 0.0f; // seconds since the last frame
	};

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
	 * them; the mechanisms are never optional. Materials live in the registry,
	 * which every feature draws from. The game supplies per frame inputs (view,
	 * output, timing) and reaches owned state through accessors; hand sim code
	 * the RenderScene& at wiring time so gameplay includes stay at scene.h.
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

		/**
		 * Renders one frame: syncs the GPU mirrors, lays out the draw buckets,
		 * runs the feature phases and executes the graph. scratch is the frame's
		 * render scratch; everything the renderer allocates for the frame comes
		 * from it.
		 */
		void render(const View& main_view, const RenderOutput& output, const FrameTiming& timing,
					Arena& scratch) noexcept;

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

		/// Every material type and material; games create, edit and destroy through it.
		[[nodiscard]] MaterialRegistry& materials() noexcept { return m_materials; }
		[[nodiscard]] const MaterialRegistry& materials() const noexcept { return m_materials; }

		/// Stats surfaces for debug UI.
		[[nodiscard]] const GpuScene& gpu_scene() const noexcept { return m_gpu_scene; }
		[[nodiscard]] gpu::Device& gpu() noexcept
		{
			EMBER_ASSERT(m_device != nullptr);
			return *m_device;
		}

	private:
		struct FeatureEntry
		{
			RenderFeature* feature = nullptr;

			// delete_object through the base pointer would hand the allocator
			// the base type's size; the thunk downcasts so size and alignment
			// stay honest.
			void (*destroy)(RenderFeature*) = nullptr;
		};

		[[nodiscard]] FrameConstants frame_constants(f32 delta_time) noexcept;
		[[nodiscard]] DrawBuckets layout_buckets(Arena& scratch) noexcept;

		gpu::Device* m_device = nullptr;

		RenderScene m_scene;
		GeometryPool m_geometry;
		GpuScene m_gpu_scene;
		MaterialRegistry m_materials;
		Visibility m_visibility;
		RenderGraph m_graph;

		Vector<FeatureEntry> m_features;
		const View* m_cull_override = nullptr;

		f64 m_time		  = 0.0; // FrameConstants.time before it wraps
		u32 m_frame_index = 0;
	};
}
