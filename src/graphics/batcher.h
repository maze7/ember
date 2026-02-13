#pragma once

#include "graphics/blend_mode.h"
#include "graphics/material.h"
#include "graphics/mesh.h"
#include "graphics/sub_texture.h"
#include "graphics/render_device.h"
#include "math/rect.h"
#include "math/quad.h"

namespace Ember
{
	/// @brief The default Vertex for the Batcher
	struct BatcherVertex
	{
		glm::vec2 pos;
		glm::vec2 tex;
		Color col;

		/// R = Multiply, G = Wash, B = Fill, A = Padding
		Color mode;

		static VertexFormat format() {
			return VertexFormat::create<BatcherVertex>({
				{ .index = 0, .type = VertexType::Float2 },
				{ .index = 1, .type = VertexType::Float2 },
				{ .index = 2, .type = VertexType::UByte4, .normalized = true },
				{ .index = 3, .type = VertexType::UByte4, .normalized = true }
			});
		}
	};

	class Batcher
	{
	public:
		/// @brief Sprite Batcher Texture drawing modes
		enum class Modes
		{
			/// @brief Renders Textures normally, Multiplied by the Vertex Color
			Normal,
			/// @brief Renders Textures washed using Vertex colors, only using Texture alpha channel.
			Wash,
			/// @brief Renders only using Vertex Colors, essentially ignoring the Texture data entirely.
			Fill
		};

		Batcher();
		explicit Batcher(RenderDevice* gpu);

		~Batcher();

		// Batchers cannot be copied.
		Batcher(const Batcher&) = delete;
		Batcher& operator=(const Batcher&) = delete;

		// /// @brief Clears the Batcher
		void clear();

		// /// @brief Uploads the current state of the internal Mesh to the GPU
		void upload();

		/**
		 * @brief Renders the batched data to a target.
		 * @param target The render target to draw to.
		 * @param matrix The camera/view projection matrix.
		 * @param scissor Optional scissor rect
		 */
		void render(const Target& target, const glm::mat4& matrix, Optional<Recti> scissor = NullOpt);

		/**
	     * @brief Renders the batched data using a default orthographic projection.
	     * @param target The render target to draw to.
	     * @param viewport Optional viewport to define the projection size. Defaults to the target's size.
	     * @param scissor Optional scissor rect.
	     */
	    void render(const Target& target, Optional<Recti> viewport = NullOpt, Optional<Recti> scissor = NullOpt);

		void push_matrix(const glm::mat3& mat, bool relative = true);
		glm::mat3 pop_matrix();

		void push_scissor(const Recti& scissor);
		void pop_scissor();

		void push_blend(BlendMode blend);
		void pop_blend();

		void push_sampler(const TextureSampler& sampler);
		void pop_sampler();

		void push_material(const Material& material);
		void pop_material();

		void push_mode(Modes mode);
		void pop_mode();

		void quad(const glm::vec2& v0, const glm::vec2& v1, const glm::vec2& v2, const glm::vec2& v3, const Color& color);
		void quad(const glm::vec2& v0, const glm::vec2& v1, const glm::vec2& v2, const glm::vec2& v3, const Color& c0, const Color& c1, const Color& c2, const Color& c3);
		void quad(Handle<Texture> texture, const glm::vec2& v0, const glm::vec2& v1, const glm::vec2& v2, const glm::vec2& v3, const glm::vec2& t0, const glm::vec2& t1, const glm::vec2& t2, const glm::vec2& t3, const Color& color);
		void quad(Handle<Texture> texture, const glm::vec2& v0, const glm::vec2& v1, const glm::vec2& v2, const glm::vec2& v3, const glm::vec2& t0, const glm::vec2& t1, const glm::vec2& t2, const glm::vec2& t3, const Color& col0, const Color& col1, const Color& col2, const Color& col3);

		void quad_line(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c, const glm::vec2& d, float line_width, const Color& color);
		void quad_line(Quad& quad, float line_width, const Color& color);

		void rect(const Rectf& rect, const Color& color);
		void rect(const glm::vec2& pos, const glm::vec2& size, Color color);
		void rect(float x, float y, float width, float height, Color color);
		void rect(const Rectf& rect, Color c0, Color c1, Color c2, Color c3);
		void rect(const glm::vec2& pos, const glm::vec2& size, Color c0, Color c1, Color c2, Color c3);
		void rect(float x, float y, float width, float height, Color c0, Color c1, Color c2, Color c3);

		void rect_line(const Rectf& rect, float line_width, Color color);
		void rect_dashed(Rectf r, float line_width, Color color, float dash_length, float dash_offset);

		void line_dashed(const glm::vec2& from ,const glm::vec2& to, float line_width, Color color, float dash_length, float offset_percent);

		void image(Handle<Texture> texture, const glm::vec2& pos0, const glm::vec2& pos1, const glm::vec2& pos2, const glm::vec2& pos3, const glm::vec2& t0, const glm::vec2& t1, const glm::vec2& t2, const glm::vec2& t3, const Color& col0, const Color& col1, const Color& col2, const Color& col3);
		void image(const SubTexture& subtex, Color color);
		void image(const SubTexture& subtex, const glm::vec2& pos, Color color);
		void image(const SubTexture& subtex, const glm::vec2& pos, const glm::vec2& origin, const glm::vec2& scale, float rotation, const Color& color);

        const glm::mat3& matrix() const { return m_matrix; }
        std::optional<Recti> scissor() const { return m_batch.scissor; }
        u32 triangle_count() const { return m_index_count / 3; }
        u32 vertex_count() const { return m_vertex_count; }
        u32 index_count() const { return m_index_count; }
        u32 batch_count() const { return m_batches.size() + (m_batch.has_elements() ? 1 : 0); }

	private:
		static constexpr Color NORMAL_MODE = Color(255, 0, 0, 0);
		static constexpr Color WASH_MODE = Color(0, 255, 0, 0);
		static constexpr Color FILL_MODE = Color(0, 0, 255, 0);

		struct Batch
		{
			Material material;
			BlendMode blend = BlendMode::Premultiply;
			Handle<Texture> texture = Handle<Texture>::null;
			Optional<Recti> scissor;
			TextureSampler sampler;
			u32 offset;
			u32 elements;

			bool has_elements() const {
				return elements > 0;
			}
		};

		RenderDevice* m_gpu = nullptr;
		Handle<Shader> m_default_shader;

		void set_texture(Handle<Texture> texture);
		void set_sampler(const TextureSampler& sampler);
		void set_material(const Material& material);
		void set_scissor(const Optional<Recti>& scissor);
		void set_blend(BlendMode blend);

		void request(u32 vertex_append_count, u32 index_append_count, std::span<BatcherVertex>& out_vertices, std::span<u32>& out_indices, u32& out_vertex_offset);
		glm::vec2 intersection(const glm::vec2& p0, const glm::vec2& p1, const glm::vec2& q0, const glm::vec2& q1);

		Ref<Material> m_default_material;
		Ref<Mesh<BatcherVertex, u32>> m_mesh;
		TextureSampler m_default_sampler;
		Color m_mode = Color(255, 0, 0, 0);

		glm::mat3 m_matrix;
		bool m_mesh_dirty = false;
		u32 m_vertex_count = 0;
		u32 m_index_count = 0;

		Batch m_batch;
		std::vector<Batch> m_batches;
		std::vector<glm::mat3> m_matrix_stack;
		std::vector<Material> m_material_stack;
		std::vector<Optional<Recti>> m_scissor_stack;
		std::vector<BatcherVertex> m_vertex_buffer;
		std::vector<BlendMode> m_blend_stack;
		std::vector<TextureSampler> m_sampler_stack;
		std::vector<Color> m_mode_stack;
		std::vector<u32> m_index_buffer;
	};
}
