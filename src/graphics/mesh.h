#pragma once

#include "core/common.h"
#include "graphics/render_device.h"

namespace Ember
{
	// Forward-declare the device
	class RenderDevice;

	// A concept to ensure TIndex is a valid index type
	template <class T>
	concept IndexType = std::same_as<T, u16> || std::same_as<T, u32>;

	/**
     * @brief A high-level object that groups vertex, index and optional instance buffers for drawing.
     * @tparam TVertex The vertex data structure. Must have a static `format()` method.
     * @tparam TIndex The index data type (u16 or u32)
     * @tparam TInstance The optional instance data structure. Must have a static `format()` method.
     */
	template <class TVertex, IndexType TIndex, class TInstance = void>
	class Mesh
	{
	public:
		explicit Mesh() : m_gpu(RenderDevice::instance()), m_vertex_buffer({ .format = TVertex::format() }) {
			// Create the vertex buffer
			m_vertex_buffer.handle = m_gpu->create_buffer({
				.usage = BufferUsage::Vertex,
				.size = 1, // Initially empty, some APIs don't like 0 sized buffers.
			});

			// Create the index buffer
			m_index_buffer = m_gpu->create_buffer({
				.usage = BufferUsage::Index,
				.size = 1, // Initially empty, some APIs don't like 0 sized buffers.
			});

			// Conditionally create the instance buffer only if TInstance is not void
			if constexpr (!std::is_void_v<TInstance>) {
				m_instance_buffer->format = TInstance::format();
				m_instance_buffer->handle = m_gpu->create_buffer({
					.usage = BufferUsage::Vertex, // Instance buffers are a type of vertex buffer
					.size = 1, // Initially empty
				});
			}
		}

		~Mesh() {
			// If the render device is already disposed, it will free GPU resources in its own teardown.
			// Avoid asserting/crashing in destructors during shutdown.
			if (!m_gpu || RenderDevice::instance() != m_gpu) {
				return;
			}

			m_gpu->dispose_buffer(m_vertex_buffer.handle);
			m_gpu->dispose_buffer(m_index_buffer);
			if constexpr (!std::is_void_v<TInstance>) {
				if (m_instance_buffer.has_value()) {
					m_gpu->dispose_buffer(m_instance_buffer->handle);
				}
			}
		}

		/**
		 * @brief Uploads vertex data to the GPU.
		 * @param data A span of the vertex data.
		 * @param offset The element offset to start writing at.
		 */
		void set_vertices(std::span<const TVertex> data, u32 offset = 0) {
			m_vertex_count = std::max(m_vertex_count, static_cast<u32>(offset + data.size()));
			RenderDevice::instance()->set_buffer_data(m_vertex_buffer.handle, std::as_bytes(data), offset * sizeof(TVertex));
		}

		/**
		 * @brief Uploads index data to the GPU.
		 * @param data A span of the index data.
		 * @param offset The element offset to start writing at.
		 */
		void set_indices(std::span<const TIndex> data, u32 offset = 0) {
			m_index_count = std::max(m_index_count, static_cast<u32>(offset + data.size()));
			RenderDevice::instance()->set_buffer_data(m_index_buffer, std::as_bytes(data), offset * sizeof(TIndex));
		}

		/**
		 * @brief Uploads instance data to the GPU.
		 * This method only exists if TInstance is not void.
		 */
		void set_instances(std::span<const TInstance> data, u32 offset = 0) requires(!std::is_void_v<TInstance>) {
			m_instance_count = std::max(m_instance_count, static_cast<u32>(offset + data.size()));
			RenderDevice::instance()->set_buffer_data(m_instance_buffer, std::as_bytes(data), offset * sizeof(TInstance));
		}

		/// @returns The number of vertices in the Vertex Buffer
		[[nodiscard]] u32 vertex_count() const { return m_vertex_count; }

		/// @returns The number of indices in the Index buffer.
        [[nodiscard]] u32 index_count() const { return m_index_count; }

        /// @returns The number of instances in the Instance buffer
        [[nodiscard]] u32 instance_count() const { return m_instance_count; }

        /// @returns Handle for the underlying Vertex buffer.
		const VertexBuffer& vertex_buffer() const { return m_vertex_buffer; }

		/// @brief Handle for the underlying index buffer.
		[[nodiscard]] Handle<Buffer> index_buffer() const { return m_index_buffer; }

		/// @brief Optional Handle for the underlying instance buffer (if one exists)
		[[nodiscard]] Optional<VertexBuffer> instance_buffer() const {
			if constexpr (!std::is_void_v<TInstance>) {
				return m_instance_buffer;
			}

			return NullOpt;
		}

		/// @brief Clears the contents of the mesh. (Does not modify underlying memory, only resets counts)
		void clear() {
			m_vertex_count = 0;
			m_index_count = 0;
			m_instance_count = 1;
		}

	private:
		/// @brief RenderDevice used to create this Mesh's GPU resources
		RenderDevice* m_gpu = nullptr;

		/// @brief Underlying VertexBuffer for this Mesh
		VertexBuffer m_vertex_buffer;

		/// @brief Handle to Index Buffer
	   	Handle<Buffer> m_index_buffer = Handle<Buffer>::null;

		/// @brief Instance Bufffer (if one exists)
		Optional<VertexBuffer> m_instance_buffer;

		/// @brief Number of vertices in the Vertex Buffer
		u32 m_vertex_count = 0;

		/// @brief Number of indices in the Index Buffer
		u32 m_index_count = 0;

		/// @brief Number of instances in the Instance Buffer (if one exists)
		u32 m_instance_count = 1;
	};
}
