#pragma once

#include <imgui.h>
#include <stack>
#include "graphics/mesh.h"
#include "graphics/material.h"
#include "graphics/texture.h"
#include "graphics/batcher.h"
#include "input/input.h"

namespace Ember
{
	struct ImGuiVertex
	{
		glm::vec2	position;
		glm::vec2	tex_coord;
		u32			color;

		static VertexFormat format() {
			return VertexFormat::create<ImGuiVertex>({
				{ .index = 0, .type = VertexType::Float2, .normalized = false },
				{ .index = 1, .type = VertexType::Float2, .normalized = false },
				{ .index = 2, .type = VertexType::UByte4, .normalized = true },
			});
		}
	};

	class ImGuiRenderer
	{
	public:
		/// @brief UI Scaling
		float scale = 2.0f;

		/// @brief Constructs an ImGui Renderer.
		explicit ImGuiRenderer(Window& window, Input& input, RenderDevice* gpu);

		/// @brief Destructor handles resource cleanup
		~ImGuiRenderer();

		/// ImGui Renderer is not copyable or movable
		ImGuiRenderer(const ImGuiRenderer&) = delete;
		ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;
		ImGuiRenderer(ImGuiRenderer&&) = delete;
		ImGuiRenderer& operator=(ImGuiRenderer&&) = delete;

		/// @brief Begins a new ImGui Frame. ImGui methods are available between begin_layout() and end_layout()
		void begin_layout();

		/// @brief Ends an ImGui Frame. Call this at the end of your update method.
		void end_layout();

		/// @brief Begins a new Batch in an ImGui window. Returns true if any batch contents will be visible.
		/// @note Call end_batch() regardless of return value.
		bool begin_batch(Batcher*& batch, Rectf& bounds);

		/// @brief Begin a new Batch in an ImGui window. Returns true if any batch contents will be visible.
		/// @note Call end_batch() regardless of return value.
		bool begin_batch(const glm::vec2& size, Batcher*& batch, Rectf& bounds);

		// @brief Ends a Batch in an ImGui Window
		void end_batch();

		/// @brief Renders the ImGui buffers. Call this in your Render method.
		void render();

		/// @brief Gets a Texture ID to draw in ImGui
		ImTextureID texture_id(Handle<Texture> texture);

		/// @brief Mouse position relative to ImGui elements.
		glm::vec2 mouse_position() const;

		/// @brief If the ImGui Context wants text input
		bool wants_text_input() const;

	private:
		Window& m_window;
		Input& m_input;

		RenderDevice* m_gpu = nullptr;
		ImGuiContext* m_context = nullptr;

		// Rendering resources
		Unique<Mesh<ImGuiVertex, u16>> m_mesh;
		Unique<Material> m_material;
		Handle<Shader> m_shader;
		Handle<Texture> m_font_texture;

		// Texture and Batcher management
		std::vector<Handle<Texture>> m_bound_textures;
		std::vector<Unique<Batcher>> m_batcher_pool;
		std::vector<Batcher*> m_batchers_used;
		std::stack<Batcher*> m_batchers_stack;

		// Draw data
		std::vector<ImGuiVertex> m_vertices;
		std::vector<ImDrawIdx> m_indices;

		// Calculated once per frame
		glm::mat4 m_projection;

		bool m_wants_text_input = false;
	};
}
