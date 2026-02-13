#pragma once

#include "material.h"
#include "target.h"
#include "math/rect.h"
#include "graphics/mesh.h"
#include "graphics/blend_mode.h"
#include "graphics/enums/cull_mode.h"
#include "graphics/enums/depth_compare.h"

namespace Ember
{
	struct VertexBufferBinding
	{
		VertexBuffer buffer;
		bool instance_input_rate = false;
	};

	/// @brief Stores information required to submit a draw command.
	struct DrawCommand
	{
		/// @brief Render Target. If not assigned, will default to the Back Buffer.
		Target* target = nullptr;

		/// @brief Material to use
		Material& material;

		/// @brief VertexBuffers to use and their associated input rate.
		std::array<VertexBufferBinding, 4> vertex_buffers;

		/// @brief Index Buffer to use. Set index_count for the number of indices to draw.
		Handle<Buffer> index_buffer = Handle<Buffer>::null;

		/// @brief The offset into the index buffer
		u32 index_offset = 0;

		/// @brief The number of indices to draw per instance when using an index buffer.
		u32 index_count = 0;

		/// @brief Size (in bytes) of an index
		u32 index_size = 0;

		/// @brief When using an index buffer this offsets the value of each index.
		/// otherwise, this is an offset into the Vertex Buffer.
		u32 vertex_offset = 0;

		/// @brief Number of vertices to draw per instance when not using an Index buffer.
		u32 vertex_count = 0;

		/// @brief The number of instances to draw. Should always be at least 1.
		u32 instance_count = 1;

		/// @brief The Render State BlendMode
		BlendMode blend_mode = BlendMode::Premultiply;

		/// @brief The Render State CullMode
		CullMode cull_mode = CullMode::None;

		/// @brief The Depth comparison function, only used if depth_test_enabled == true
		DepthCompare depth_compare = DepthCompare::Less;

		/// @brief If the Depth Test is enabled
		bool depth_test_enabled = false;

		/// @brief If Writing to the Depth Buffer is enabled
		bool depth_write_enabled = false;

		/// @brief Render Viewport
		Optional<Recti> viewport = NullOpt;

		/// @brief Render state scissor rectangle
		Optional<Recti> scissor = NullOpt;

		/// @brief Creates a DrawCommand on the given Mesh and Material.
		template <typename TVertex, IndexType TIndex, typename TInstance>
		DrawCommand(Target* target_, const Mesh<TVertex, TIndex, TInstance>& mesh, Material& material_)
			: target(target_)
			, material(material_)
		{
			// Configure vertex and instance buffer from mesh
			if (mesh.instance_buffer().has_value()) {
				vertex_buffers = std::array<VertexBufferBinding, 4>{{
					{ mesh.vertex_buffer(), false },
					{ mesh.instance_buffer().value(), true },
				}};

				instance_count = mesh.instance_count();
			} else {
				vertex_buffers = std::array<VertexBufferBinding, 4>{{ mesh.vertex_buffer(), false }};
				instance_count = 1;
			}

			// Configure index buffer from mesh
			if (!mesh.index_buffer().is_null()) {
				index_buffer = mesh.index_buffer();
				index_count = mesh.index_count();
				index_size = sizeof(TIndex);
			} else {
				vertex_count = mesh.vertex_count();
			}
		}
	};
}
