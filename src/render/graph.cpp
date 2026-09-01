#include <ember/core/bits.h>
#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/render/graph.h>

namespace ember::render
{
	namespace
	{
		/// Reads never hazard each other; everything else does, layout change or none.
		[[nodiscard]] constexpr bool is_write(gpu::TextureState state) noexcept
		{
			switch (state)
			{
				case gpu::TextureState::RenderTarget:
				case gpu::TextureState::DepthTarget:
				case gpu::TextureState::ShaderWrite:
				case gpu::TextureState::CopyDst:
					return true;
				default:
					return false;
			}
		}

		/// Exact identity, no hashing: every field that changes compatibility is packed.
		[[nodiscard]] constexpr u64 pool_key(const GraphTextureDef& def) noexcept
		{
			return static_cast<u64>(def.extent.width) | (static_cast<u64>(def.extent.height) << 16) |
				   (static_cast<u64>(def.format) << 32) | (static_cast<u64>(def.usage) << 40) |
				   (static_cast<u64>(def.mip_count) << 48);
		}
	}

	TextureHandle PassContext::texture(GraphTexture handle) const noexcept
	{
		EMBER_ASSERT(graph != nullptr && handle.index < graph->m_texture_count);
		if (graph == nullptr || handle.index >= graph->m_texture_count)
			return {};

		return graph->m_textures[handle.index].physical;
	}

	Extent2D PassContext::extent(GraphTexture handle) const noexcept
	{
		EMBER_ASSERT(graph != nullptr && handle.index < graph->m_texture_count);
		if (graph == nullptr || handle.index >= graph->m_texture_count)
			return {};

		return graph->m_textures[handle.index].def.extent;
	}

	void RenderGraph::Pass::add_use(GraphTexture texture, gpu::TextureState state) noexcept
	{
		EMBER_ASSERT(!texture.is_null());
		EMBER_ASSERT(state != gpu::TextureState::Undefined && state != gpu::TextureState::Present);

		for (u32 i = 0; i < m_use_count; ++i)
		{
			if (m_uses[i].texture.index == texture.index)
			{
				// One state per texture per pass; a second, different one is a design error.
				EMBER_ASSERT(m_uses[i].state == state);
				return;
			}
		}

		EMBER_ASSERT(m_use_count < MAX_PASS_USES);
		if (m_use_count >= MAX_PASS_USES)
			return;

		m_uses[m_use_count++] = {texture, state};
	}

	RenderGraph::Pass& RenderGraph::Pass::read(GraphTexture texture, gpu::TextureState state) noexcept
	{
		add_use(texture, state);
		return *this;
	}

	RenderGraph::Pass& RenderGraph::Pass::write(GraphTexture texture, gpu::TextureState state) noexcept
	{
		add_use(texture, state);
		return *this;
	}

	RenderGraph::Pass& RenderGraph::Pass::color(const ColorDef& def) noexcept
	{
		EMBER_ASSERT(m_color_count < MAX_COLOR_ATTACHMENTS);
		if (m_color_count >= MAX_COLOR_ATTACHMENTS)
			return *this;

		m_colors[m_color_count++] = def;
		add_use(def.texture, gpu::TextureState::RenderTarget);
		return *this;
	}

	RenderGraph::Pass& RenderGraph::Pass::depth(const DepthDef& def) noexcept
	{
		EMBER_ASSERT(!m_has_depth);

		m_depth		= def;
		m_has_depth = true;
		add_use(def.texture, gpu::TextureState::DepthTarget);
		return *this;
	}

	void RenderGraph::begin() noexcept
	{
		m_pass_count	= 0;
		m_texture_count = 0;
	}

	GraphTexture RenderGraph::create(const GraphTextureDef& def) noexcept
	{
		EMBER_ASSERT(m_texture_count < MAX_GRAPH_TEXTURES);
		EMBER_ASSERT(def.extent.width > 0 && def.extent.width <= 0xFFFF);
		EMBER_ASSERT(def.extent.height > 0 && def.extent.height <= 0xFFFF);
		if (m_texture_count >= MAX_GRAPH_TEXTURES)
			return {};

		m_textures[m_texture_count] = {
			.def	  = def,
			.state	  = resting_state(def.usage),
			.resting  = resting_state(def.usage),
			.imported = false,
		};

		return {static_cast<u16>(m_texture_count++)};
	}

	GraphTexture RenderGraph::import(
		TextureHandle texture, gpu::TextureState current, gpu::TextureState final_state, Extent2D extent) noexcept
	{
		EMBER_ASSERT(m_texture_count < MAX_GRAPH_TEXTURES);
		if (m_texture_count >= MAX_GRAPH_TEXTURES)
			return {};

		m_textures[m_texture_count] = {
			.def	  = {.name = "imported", .extent = extent},
			.physical = texture,
			.state	  = current,
			.resting  = final_state,
			.imported = true,
		};

		return {static_cast<u16>(m_texture_count++)};
	}

	RenderGraph::Pass& RenderGraph::pass(const char* name) noexcept
	{
		EMBER_ASSERT(name != nullptr);
		EMBER_ASSERT(m_pass_count < MAX_GRAPH_PASSES);

		const u32 index = m_pass_count < MAX_GRAPH_PASSES ? m_pass_count++ : MAX_GRAPH_PASSES;

		Pass& pass	 = m_passes[index];
		pass		 = Pass{};
		pass.m_name	 = name;
		pass.m_graph = this;
		return pass;
	}

	TextureHandle RenderGraph::acquire(gpu::Device& device, const GraphTextureDef& def, u32& pool_slot) noexcept
	{
		const u64 key = pool_key(def);

		for (u32 i = 0; i < MAX_POOL_ENTRIES; ++i)
		{
			PoolEntry& entry = m_pool[i];
			if (!entry.in_use && !entry.texture.is_null() && entry.key == key)
			{
				entry.in_use = true;
				pool_slot	 = i;
				return entry.texture;
			}
		}

		const TextureHandle texture = device.create_texture({
			.name	   = def.name,
			.extent	   = {def.extent.width, def.extent.height, 1},
			.format	   = def.format,
			.mip_count = def.mip_count,
			.usage	   = def.usage,
		});

		pool_slot = UNPOOLED;

		// Creation failure flows through as a null handle: attachments assert in
		// debug, sampled reads land on the heap fallback.
		if (texture.is_null())
		{
			EMBER_ERROR("graph transient '{}' failed to create", def.name);
			return texture;
		}

		for (u32 i = 0; i < MAX_POOL_ENTRIES; ++i)
		{
			PoolEntry& entry = m_pool[i];
			if (entry.texture.is_null() && !entry.in_use)
			{
				entry	  = {.texture = texture, .key = key, .last_used_frame = m_frame, .in_use = true};
				pool_slot = i;
				return texture;
			}
		}

		// Pool full: the texture still serves this frame and dies at release.
		EMBER_ASSERT(false);
		return texture;
	}

	void RenderGraph::release(gpu::Device& device) noexcept
	{
		for (u32 i = 0; i < m_texture_count; ++i)
		{
			VirtualTexture& texture = m_textures[i];
			if (texture.imported)
				continue;

			if (texture.pool_slot != UNPOOLED)
			{
				PoolEntry& entry	  = m_pool[texture.pool_slot];
				entry.in_use		  = false;
				entry.last_used_frame = m_frame;
			}
			else if (!texture.physical.is_null())
			{
				device.destroy(texture.physical);
			}
		}

		for (u32 i = 0; i < MAX_POOL_ENTRIES; ++i)
		{
			PoolEntry& entry = m_pool[i];
			if (!entry.in_use && !entry.texture.is_null() && m_frame - entry.last_used_frame > POOL_IDLE_FRAMES)
			{
				device.destroy(entry.texture);
				entry = {};
			}
		}
	}

	void RenderGraph::execute(gpu::Device& device) noexcept
	{
		for (u32 i = 0; i < m_texture_count; ++i)
		{
			VirtualTexture& texture = m_textures[i];
			if (!texture.imported)
				texture.physical = acquire(device, texture.def, texture.pool_slot);
		}

		gpu::CommandList cmd = device.begin_command_list();

		for (u32 pass_index = 0; pass_index < m_pass_count; ++pass_index)
		{
			Pass& pass = m_passes[pass_index];

			cmd.begin_zone(pass.m_name);

			gpu::TextureBarrier barriers[MAX_PASS_USES];
			u32 barrier_count = 0;

			for (u32 i = 0; i < pass.m_use_count; ++i)
			{
				const Pass::Use& use	= pass.m_uses[i];
				VirtualTexture& texture = m_textures[use.texture.index];

				if (texture.state != use.state || is_write(use.state))
				{
					barriers[barrier_count++] = {
						.texture = texture.physical,
						.before	 = texture.state,
						.after	 = use.state,
					};
					texture.state = use.state;
				}
			}

			if (barrier_count > 0)
				cmd.barrier({barriers, barrier_count});

			const bool raster = pass.m_color_count > 0 || pass.m_has_depth;
			if (raster)
			{
				gpu::ColorAttachment colors[MAX_COLOR_ATTACHMENTS];
				for (u32 i = 0; i < pass.m_color_count; ++i)
				{
					const Pass::ColorDef& def = pass.m_colors[i];

					colors[i] = {
						.texture = m_textures[def.texture.index].physical,
						.mip	 = def.mip,
						.layer	 = def.layer,
						.load	 = def.load,
						.store	 = def.store,
						.clear	 = def.clear,
					};
				}

				gpu::DepthAttachment depth{};
				if (pass.m_has_depth)
				{
					depth = {
						.texture	 = m_textures[pass.m_depth.texture.index].physical,
						.mip		 = pass.m_depth.mip,
						.layer		 = pass.m_depth.layer,
						.load		 = pass.m_depth.load,
						.store		 = pass.m_depth.store,
						.clear_depth = pass.m_depth.clear_depth,
					};
				}

				cmd.begin_rendering({.colors = {colors, pass.m_color_count}, .depth = depth});
			}

			if (pass.m_record != nullptr)
			{
				const PassContext context{this};
				pass.m_record(cmd, context, pass.m_record_data);
			}

			if (raster)
				cmd.end_rendering();

			cmd.end_zone();
		}

		gpu::TextureBarrier finals[MAX_GRAPH_TEXTURES];
		u32 final_count = 0;

		for (u32 i = 0; i < m_texture_count; ++i)
		{
			VirtualTexture& texture = m_textures[i];
			if (texture.state != texture.resting)
			{
				finals[final_count++] = {
					.texture = texture.physical,
					.before	 = texture.state,
					.after	 = texture.resting,
				};
				texture.state = texture.resting;
			}
		}

		if (final_count > 0)
			cmd.barrier({finals, final_count});

		device.submit(cmd);
		release(device);
		++m_frame;
	}

	void RenderGraph::shutdown(gpu::Device& device) noexcept
	{
		for (u32 i = 0; i < MAX_POOL_ENTRIES; ++i)
		{
			PoolEntry& entry = m_pool[i];
			EMBER_ASSERT(!entry.in_use);

			if (!entry.texture.is_null())
				device.destroy(entry.texture);
			entry = {};
		}
	}
}
