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

		[[nodiscard]] constexpr bool is_write(gpu::BufferState state) noexcept
		{
			switch (state)
			{
				case gpu::BufferState::ShaderWrite:
				case gpu::BufferState::CopyDst:
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

	BufferHandle PassContext::buffer(GraphBuffer handle) const noexcept
	{
		EMBER_ASSERT(graph != nullptr && handle.index < graph->m_buffer_count);
		if (graph == nullptr || handle.index >= graph->m_buffer_count)
			return {};

		return graph->m_buffers[handle.index].physical;
	}

	Extent2D PassContext::extent(GraphTexture handle) const noexcept
	{
		EMBER_ASSERT(graph != nullptr && handle.index < graph->m_texture_count);
		if (graph == nullptr || handle.index >= graph->m_texture_count)
			return {};

		return graph->m_textures[handle.index].def.extent;
	}

	u64 PassContext::size(GraphBuffer handle) const noexcept
	{
		EMBER_ASSERT(graph != nullptr && handle.index < graph->m_buffer_count);
		if (graph == nullptr || handle.index >= graph->m_buffer_count)
			return 0;

		return graph->m_buffers[handle.index].def.size;
	}

	void RenderGraph::Pass::add_use(GraphTexture texture, gpu::TextureState state) noexcept
	{
		EMBER_ASSERT(!texture.is_null());
		EMBER_ASSERT(state != gpu::TextureState::Undefined && state != gpu::TextureState::Present);

		for (u32 i = 0; i < m_texture_use_count; ++i)
		{
			if (m_texture_uses[i].texture.index == texture.index)
			{
				// One state per texture per pass; a second, different one is a design error.
				EMBER_ASSERT(m_texture_uses[i].state == state);
				return;
			}
		}

		EMBER_ASSERT(m_texture_use_count < MAX_PASS_USES);
		if (m_texture_use_count >= MAX_PASS_USES)
			return;

		m_texture_uses[m_texture_use_count++] = {texture, state};
	}

	void RenderGraph::Pass::add_use(GraphBuffer buffer, gpu::BufferState state) noexcept
	{
		EMBER_ASSERT(!buffer.is_null());

		for (u32 i = 0; i < m_buffer_use_count; ++i)
		{
			if (m_buffer_uses[i].buffer.index == buffer.index)
			{
				EMBER_ASSERT(m_buffer_uses[i].state == state);
				return;
			}
		}

		EMBER_ASSERT(m_buffer_use_count < MAX_PASS_USES);
		if (m_buffer_use_count >= MAX_PASS_USES)
			return;

		m_buffer_uses[m_buffer_use_count++] = {buffer, state};
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

	RenderGraph::Pass& RenderGraph::Pass::read(GraphBuffer buffer, gpu::BufferState state) noexcept
	{
		add_use(buffer, state);
		return *this;
	}

	RenderGraph::Pass& RenderGraph::Pass::write(GraphBuffer buffer, gpu::BufferState state) noexcept
	{
		add_use(buffer, state);
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
		m_buffer_count	= 0;
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

	GraphBuffer RenderGraph::create(const GraphBufferDef& def) noexcept
	{
		EMBER_ASSERT(m_buffer_count < MAX_GRAPH_BUFFERS);
		EMBER_ASSERT(def.size > 0);
		if (m_buffer_count >= MAX_GRAPH_BUFFERS)
			return {};

		m_buffers[m_buffer_count] = {
			.def	  = def,
			.state	  = resting_state(def.usage),
			.resting  = resting_state(def.usage),
			.imported = false,
		};

		return {static_cast<u16>(m_buffer_count++)};
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

	GraphBuffer
	RenderGraph::import(BufferHandle buffer, gpu::BufferState current, gpu::BufferState final_state, u64 size) noexcept
	{
		EMBER_ASSERT(m_buffer_count < MAX_GRAPH_BUFFERS);
		if (m_buffer_count >= MAX_GRAPH_BUFFERS)
			return {};

		m_buffers[m_buffer_count] = {
			.def	  = {.name = "imported", .size = size},
			.physical = buffer,
			.state	  = current,
			.resting  = final_state,
			.imported = true,
		};

		return {static_cast<u16>(m_buffer_count++)};
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
			TexturePoolEntry& entry = m_texture_pool[i];
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
			TexturePoolEntry& entry = m_texture_pool[i];
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

	BufferHandle RenderGraph::acquire(gpu::Device& device, const GraphBufferDef& def, u32& pool_slot) noexcept
	{
		for (u32 i = 0; i < MAX_POOL_ENTRIES; ++i)
		{
			BufferPoolEntry& entry = m_buffer_pool[i];
			if (!entry.in_use && !entry.buffer.is_null() && entry.size == def.size && entry.usage == def.usage)
			{
				entry.in_use = true;
				pool_slot	 = i;
				return entry.buffer;
			}
		}

		const BufferHandle buffer = device.create_buffer({
			.name  = def.name,
			.size  = def.size,
			.usage = def.usage,
		});

		pool_slot = UNPOOLED;

		if (buffer.is_null())
		{
			EMBER_ERROR("graph transient '{}' failed to create", def.name);
			return buffer;
		}

		for (u32 i = 0; i < MAX_POOL_ENTRIES; ++i)
		{
			BufferPoolEntry& entry = m_buffer_pool[i];
			if (entry.buffer.is_null() && !entry.in_use)
			{
				entry = {
					.buffer			 = buffer,
					.size			 = def.size,
					.usage			 = def.usage,
					.last_used_frame = m_frame,
					.in_use			 = true,
				};
				pool_slot = i;
				return buffer;
			}
		}

		// Pool full: the buffer still serves this frame and dies at release.
		EMBER_ASSERT(false);
		return buffer;
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
				TexturePoolEntry& entry = m_texture_pool[texture.pool_slot];
				entry.in_use			= false;
				entry.last_used_frame	= m_frame;
			}
			else if (!texture.physical.is_null())
			{
				device.destroy(texture.physical);
			}
		}

		for (u32 i = 0; i < MAX_POOL_ENTRIES; ++i)
		{
			TexturePoolEntry& entry = m_texture_pool[i];
			if (!entry.in_use && !entry.texture.is_null() && m_frame - entry.last_used_frame > POOL_IDLE_FRAMES)
			{
				device.destroy(entry.texture);
				entry = {};
			}
		}

		for (u32 i = 0; i < m_buffer_count; ++i)
		{
			VirtualBuffer& buffer = m_buffers[i];
			if (buffer.imported)
				continue;

			if (buffer.pool_slot != UNPOOLED)
			{
				BufferPoolEntry& entry = m_buffer_pool[buffer.pool_slot];
				entry.in_use		   = false;
				entry.last_used_frame  = m_frame;
			}
			else if (!buffer.physical.is_null())
			{
				device.destroy(buffer.physical);
			}
		}

		for (u32 i = 0; i < MAX_POOL_ENTRIES; ++i)
		{
			BufferPoolEntry& entry = m_buffer_pool[i];
			if (!entry.in_use && !entry.buffer.is_null() && m_frame - entry.last_used_frame > POOL_IDLE_FRAMES)
			{
				device.destroy(entry.buffer);
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

		for (u32 i = 0; i < m_buffer_count; ++i)
		{
			VirtualBuffer& buffer = m_buffers[i];
			if (!buffer.imported)
				buffer.physical = acquire(device, buffer.def, buffer.pool_slot);
		}

		gpu::CommandList cmd = device.begin_command_list();

		for (u32 pass_index = 0; pass_index < m_pass_count; ++pass_index)
		{
			Pass& pass = m_passes[pass_index];

			cmd.begin_zone(pass.m_name);

			gpu::TextureBarrier texture_barriers[MAX_PASS_USES];
			gpu::BufferBarrier buffer_barriers[MAX_PASS_USES];
			u32 texture_barrier_count = 0;
			u32 buffer_barrier_count  = 0;

			for (u32 i = 0; i < pass.m_texture_use_count; ++i)
			{
				const Pass::TextureUse& use = pass.m_texture_uses[i];
				VirtualTexture& texture		= m_textures[use.texture.index];

				if (texture.state != use.state || is_write(use.state))
				{
					texture_barriers[texture_barrier_count++] = {
						.texture = texture.physical,
						.before	 = texture.state,
						.after	 = use.state,
					};
					texture.state = use.state;
				}
			}

			for (u32 i = 0; i < pass.m_buffer_use_count; ++i)
			{
				const Pass::BufferUse& use = pass.m_buffer_uses[i];
				VirtualBuffer& buffer	   = m_buffers[use.buffer.index];

				// A read to read scope change still barriers; the last write was only made
				// visible to the scopes named then.
				if (buffer.state != use.state || is_write(use.state))
				{
					buffer_barriers[buffer_barrier_count++] = {
						.buffer = buffer.physical,
						.before = buffer.state,
						.after	= use.state,
					};
					buffer.state = use.state;
				}
			}

			if (texture_barrier_count + buffer_barrier_count > 0)
				cmd.barrier({texture_barriers, texture_barrier_count}, {buffer_barriers, buffer_barrier_count});

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

		gpu::TextureBarrier texture_finals[MAX_GRAPH_TEXTURES];
		gpu::BufferBarrier buffer_finals[MAX_GRAPH_BUFFERS];
		u32 texture_final_count = 0;
		u32 buffer_final_count	= 0;

		for (u32 i = 0; i < m_texture_count; ++i)
		{
			VirtualTexture& texture = m_textures[i];
			if (texture.state != texture.resting)
			{
				texture_finals[texture_final_count++] = {
					.texture = texture.physical,
					.before	 = texture.state,
					.after	 = texture.resting,
				};
				texture.state = texture.resting;
			}
		}

		for (u32 i = 0; i < m_buffer_count; ++i)
		{
			VirtualBuffer& buffer = m_buffers[i];
			if (buffer.state != buffer.resting)
			{
				buffer_finals[buffer_final_count++] = {
					.buffer = buffer.physical,
					.before = buffer.state,
					.after	= buffer.resting,
				};
				buffer.state = buffer.resting;
			}
		}

		if (texture_final_count + buffer_final_count > 0)
			cmd.barrier({texture_finals, texture_final_count}, {buffer_finals, buffer_final_count});

		device.submit(cmd);
		release(device);
		++m_frame;
	}

	void RenderGraph::shutdown(gpu::Device& device) noexcept
	{
		// Teardown texture pool
		for (u32 i = 0; i < MAX_POOL_ENTRIES; ++i)
		{
			TexturePoolEntry& entry = m_texture_pool[i];
			EMBER_ASSERT(!entry.in_use);

			if (!entry.texture.is_null())
				device.destroy(entry.texture);
			entry = {};
		}

		// Teardown buffer pool
		for (u32 i = 0; i < MAX_POOL_ENTRIES; ++i)
		{
			BufferPoolEntry& entry = m_buffer_pool[i];
			EMBER_ASSERT(!entry.in_use);

			if (!entry.buffer.is_null())
				device.destroy(entry.buffer);
			entry = {};
		}
	}
}
