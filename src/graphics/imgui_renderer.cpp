#include "graphics/imgui_renderer.h"
#include <ext/matrix_clip_space.hpp>
#include <gtc/matrix_transform.hpp>
#include <gtx/matrix_transform_2d.hpp>
#include "core/time.h"
#include "graphics/draw_cmd.h"
#include "implot.h"
#include "platform/window.h"
#include <utility>
#include <algorithm>

using namespace Ember;

static const std::vector<std::pair<ImGuiKey, Key> > keys = {
	{ImGuiKey_Tab, Key::Tab},
	{ImGuiKey_LeftArrow, Key::Left},
	{ImGuiKey_RightArrow, Key::Right},
	{ImGuiKey_UpArrow, Key::Up},
	{ImGuiKey_DownArrow, Key::Down},
	{ImGuiKey_PageUp, Key::PageUp},
	{ImGuiKey_PageDown, Key::PageDown},
	{ImGuiKey_Home, Key::Home},
	{ImGuiKey_End, Key::End},
	{ImGuiKey_Insert, Key::Insert},
	{ImGuiKey_Delete, Key::Delete},
	{ImGuiKey_Backspace, Key::Backspace},
	{ImGuiKey_Space, Key::Space},
	{ImGuiKey_Enter, Key::Enter},
	{ImGuiKey_Escape, Key::Escape},
	{ImGuiKey_LeftCtrl, Key::LeftControl},
	{ImGuiKey_LeftShift, Key::LeftShift},
	{ImGuiKey_LeftAlt, Key::LeftAlt},
	{ImGuiKey_LeftSuper, Key::LeftOS},
	{ImGuiKey_RightCtrl, Key::RightControl},
	{ImGuiKey_RightShift, Key::RightShift},
	{ImGuiKey_RightAlt, Key::RightAlt},
	{ImGuiKey_RightSuper, Key::RightOS},
	{ImGuiKey_Menu, Key::Menu},

	{ImGuiKey_0, Key::D0},
	{ImGuiKey_1, Key::D1},
	{ImGuiKey_2, Key::D2},
	{ImGuiKey_3, Key::D3},
	{ImGuiKey_4, Key::D4},
	{ImGuiKey_5, Key::D5},
	{ImGuiKey_6, Key::D6},
	{ImGuiKey_7, Key::D7},
	{ImGuiKey_8, Key::D8},
	{ImGuiKey_9, Key::D9},

	{ImGuiKey_A, Key::A},
	{ImGuiKey_B, Key::B},
	{ImGuiKey_C, Key::C},
	{ImGuiKey_D, Key::D},
	{ImGuiKey_E, Key::E},
	{ImGuiKey_F, Key::F},
	{ImGuiKey_G, Key::G},
	{ImGuiKey_H, Key::H},
	{ImGuiKey_I, Key::I},
	{ImGuiKey_J, Key::J},
	{ImGuiKey_K, Key::K},
	{ImGuiKey_L, Key::L},
	{ImGuiKey_M, Key::M},
	{ImGuiKey_N, Key::N},
	{ImGuiKey_O, Key::O},
	{ImGuiKey_P, Key::P},
	{ImGuiKey_Q, Key::Q},
	{ImGuiKey_R, Key::R},
	{ImGuiKey_S, Key::S},
	{ImGuiKey_T, Key::T},
	{ImGuiKey_U, Key::U},
	{ImGuiKey_V, Key::V},
	{ImGuiKey_W, Key::W},
	{ImGuiKey_X, Key::X},
	{ImGuiKey_Y, Key::Y},
	{ImGuiKey_Z, Key::Z},

	{ImGuiKey_F1, Key::F1},
	{ImGuiKey_F2, Key::F2},
	{ImGuiKey_F3, Key::F3},
	{ImGuiKey_F4, Key::F4},
	{ImGuiKey_F5, Key::F5},
	{ImGuiKey_F6, Key::F6},
	{ImGuiKey_F7, Key::F7},
	{ImGuiKey_F8, Key::F8},
	{ImGuiKey_F9, Key::F9},
	{ImGuiKey_F10, Key::F10},
	{ImGuiKey_F11, Key::F11},
	{ImGuiKey_F12, Key::F12},

	{ImGuiKey_Apostrophe, Key::Apostrophe},
	{ImGuiKey_Comma, Key::Comma},
	{ImGuiKey_Minus, Key::Minus},
	{ImGuiKey_Period, Key::Period},
	{ImGuiKey_Slash, Key::Slash},
	{ImGuiKey_Semicolon, Key::Semicolon},
	{ImGuiKey_Equal, Key::Equals},
	{ImGuiKey_LeftBracket, Key::LeftBracket},
	{ImGuiKey_Backslash, Key::Backslash},
	{ImGuiKey_RightBracket, Key::RightBracket},
	{ImGuiKey_GraveAccent, Key::Tilde},

	{ImGuiKey_CapsLock, Key::Capslock},
	{ImGuiKey_ScrollLock, Key::ScrollLock},
	{ImGuiKey_NumLock, Key::Numlock},
	{ImGuiKey_PrintScreen, Key::PrintScreen},
	{ImGuiKey_Pause, Key::Pause},

	{ImGuiKey_Keypad0, Key::Keypad0},
	{ImGuiKey_Keypad1, Key::Keypad1},
	{ImGuiKey_Keypad2, Key::Keypad2},
	{ImGuiKey_Keypad3, Key::Keypad3},
	{ImGuiKey_Keypad4, Key::Keypad4},
	{ImGuiKey_Keypad5, Key::Keypad5},
	{ImGuiKey_Keypad6, Key::Keypad6},
	{ImGuiKey_Keypad7, Key::Keypad7},
	{ImGuiKey_Keypad8, Key::Keypad8},
	{ImGuiKey_Keypad9, Key::Keypad9},
	{ImGuiKey_KeypadDecimal, Key::KeypadPeroid}, // Check spelling
	{ImGuiKey_KeypadDivide, Key::KeypadDivide},
	{ImGuiKey_KeypadMultiply, Key::KeypadMultiply},
	{ImGuiKey_KeypadSubtract, Key::KeypadMinus},
	{ImGuiKey_KeypadAdd, Key::KeypadPlus},
	{ImGuiKey_KeypadEnter, Key::KeypadEnter},
	{ImGuiKey_KeypadEqual, Key::KeypadEquals},
};

ImGuiRenderer::ImGuiRenderer(Window& window, Input& input, RenderDevice* gpu) : m_window(window), m_input(input), m_gpu(gpu), m_font_texture() {
	// create imgui context
	m_context = ImGui::CreateContext();
	ImGui::SetCurrentContext(m_context);

	auto& io = ImGui::GetIO();
	io.BackendFlags = ImGuiBackendFlags_None;
	io.ConfigFlags = ImGuiConfigFlags_DockingEnable;

	// load ImGui font
	io.Fonts->AddFontDefault();
	unsigned char* pixels = nullptr;
	int width, height, bytes_per_pixel;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bytes_per_pixel);
	m_font_texture = m_gpu->create_texture({
		.size = { width, height },
		.data = std::span(pixels, width * height * 4)
	});

	EMBER_ASSERT(!m_font_texture.is_null());

	// apply theme
	auto& colors = ImGui::GetStyle().Colors;
	colors[ImGuiCol_WindowBg] = ImVec4{0.1f, 0.1f, 0.13f, 1.0f};
	colors[ImGuiCol_MenuBarBg] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};

	// Border
	colors[ImGuiCol_Border] = ImVec4{0.44f, 0.37f, 0.61f, 0.29f};
	colors[ImGuiCol_BorderShadow] = ImVec4{0.0f, 0.0f, 0.0f, 0.24f};

	// Text
	colors[ImGuiCol_Text] = ImVec4{1.0f, 1.0f, 1.0f, 1.0f};
	colors[ImGuiCol_TextDisabled] = ImVec4{0.5f, 0.5f, 0.5f, 1.0f};

	// Headers
	colors[ImGuiCol_Header] = ImVec4{0.13f, 0.13f, 0.17, 1.0f};
	colors[ImGuiCol_HeaderHovered] = ImVec4{0.19f, 0.2f, 0.25f, 1.0f};
	colors[ImGuiCol_HeaderActive] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};

	// Buttons
	colors[ImGuiCol_Button] = ImVec4{0.13f, 0.13f, 0.17, 1.0f};
	colors[ImGuiCol_ButtonHovered] = ImVec4{0.19f, 0.2f, 0.25f, 1.0f};
	colors[ImGuiCol_ButtonActive] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
	colors[ImGuiCol_CheckMark] = ImVec4{0.74f, 0.58f, 0.98f, 1.0f};

	// Popups
	colors[ImGuiCol_PopupBg] = ImVec4{0.1f, 0.1f, 0.13f, 0.92f};

	// Slider
	colors[ImGuiCol_SliderGrab] = ImVec4{0.44f, 0.37f, 0.61f, 0.54f};
	colors[ImGuiCol_SliderGrabActive] = ImVec4{0.74f, 0.58f, 0.98f, 0.54f};

	// Frame BG
	colors[ImGuiCol_FrameBg] = ImVec4{0.13f, 0.13, 0.17, 1.0f};
	colors[ImGuiCol_FrameBgHovered] = ImVec4{0.19f, 0.2f, 0.25f, 1.0f};
	colors[ImGuiCol_FrameBgActive] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};

	// Tabs
	colors[ImGuiCol_Tab] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
	colors[ImGuiCol_TabHovered] = ImVec4{0.24, 0.24f, 0.32f, 1.0f};
	colors[ImGuiCol_TabActive] = ImVec4{0.2f, 0.22f, 0.27f, 1.0f};
	colors[ImGuiCol_TabUnfocused] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
	colors[ImGuiCol_TabUnfocusedActive] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};

	// Title
	colors[ImGuiCol_TitleBg] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
	colors[ImGuiCol_TitleBgActive] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
	colors[ImGuiCol_TitleBgCollapsed] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};

	// Scrollbar
	colors[ImGuiCol_ScrollbarBg] = ImVec4{0.1f, 0.1f, 0.13f, 1.0f};
	colors[ImGuiCol_ScrollbarGrab] = ImVec4{0.16f, 0.16f, 0.21f, 1.0f};
	colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4{0.19f, 0.2f, 0.25f, 1.0f};
	colors[ImGuiCol_ScrollbarGrabActive] = ImVec4{0.24f, 0.24f, 0.32f, 1.0f};

	// Seperator
	colors[ImGuiCol_Separator] = ImVec4{0.44f, 0.37f, 0.61f, 1.0f};
	colors[ImGuiCol_SeparatorHovered] = ImVec4{0.74f, 0.58f, 0.98f, 1.0f};
	colors[ImGuiCol_SeparatorActive] = ImVec4{0.84f, 0.58f, 1.0f, 1.0f};

	// Resize Grip
	colors[ImGuiCol_ResizeGrip] = ImVec4{0.44f, 0.37f, 0.61f, 0.29f};
	colors[ImGuiCol_ResizeGripHovered] = ImVec4{0.74f, 0.58f, 0.98f, 0.29f};
	colors[ImGuiCol_ResizeGripActive] = ImVec4{0.84f, 0.58f, 1.0f, 0.29f};

	// Docking
	colors[ImGuiCol_DockingPreview] = ImVec4{0.44f, 0.37f, 0.61f, 1.0f};

	auto& style = ImGui::GetStyle();
	style.TabRounding = 4;
	style.ScrollbarRounding = 9;
	style.WindowRounding = 7;
	style.GrabRounding = 3;
	style.FrameRounding = 3;
	style.PopupRounding = 4;
	style.ChildRounding = 4;

	// create drawing resources
	m_mesh = make_unique<Mesh<ImGuiVertex, ImDrawIdx>>();
	auto v_code = load_file("assets/shaders/imgui.vert.spv");
	auto f_code = load_file("assets/shaders/imgui.frag.spv");
	m_shader = m_gpu->create_shader({
		.vertex = {
			.code = std::span(v_code),
			.num_uniform_buffers = 1,
			.entrypoint = "main"
		},
		.fragment = {
			.code = std::span(f_code),
			.num_samplers = 1,
			.entrypoint = "main"
		}
	});
	m_material = make_unique<Material>(m_shader);

	ImGui::SetCurrentContext(nullptr);
	ImPlot::CreateContext();
}

ImGuiRenderer::~ImGuiRenderer() {
	if (m_context) {
		ImPlot::DestroyContext();
		ImGui::DestroyContext(m_context);
		m_context = nullptr;
	}

	if (m_gpu && RenderDevice::instance() == m_gpu) {
		m_gpu->dispose_texture(m_font_texture);
		m_gpu->dispose_shader(m_shader);
	}
}

void ImGuiRenderer::begin_layout() {
	EMBER_ASSERT(ImGui::GetCurrentContext() == nullptr && "ImGui Context already created. Did you forget to call end_layout()?");
	ImGui::SetCurrentContext(m_context);

	scale = m_window.pixel_density();

	// Clear per-frame data
    m_bound_textures.clear();
    for (auto* batcher : m_batchers_used) {
    	// move ownership back to the pool
        m_batcher_pool.push_back(Unique<Batcher>(batcher));
    }
    m_batchers_used.clear();
    while (!m_batchers_stack.empty()) m_batchers_stack.pop();

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->SetTexID(texture_id(m_font_texture));

    auto fb_size = m_gpu->framebuffer().size();

    // Setup IO
    io.DeltaTime = Time::delta();
    io.DisplaySize = { fb_size.x / scale, fb_size.y / scale };
    io.DisplayFramebufferScale = { scale, scale };

    // Forward mouse events to ImGui
    auto& mouse = m_input.mouse();
    glm::vec2 mouse_pos = { mouse.position().x / scale, mouse.position().y / scale };
    io.AddMousePosEvent(mouse_pos.x, mouse_pos.y);
    io.AddMouseButtonEvent(0, mouse.down(MouseButton::Left) || mouse.pressed(MouseButton::Left));
    io.AddMouseButtonEvent(1, mouse.down(MouseButton::Right) || mouse.pressed(MouseButton::Right));
    io.AddMouseButtonEvent(2, mouse.down(MouseButton::Middle) || mouse.pressed(MouseButton::Middle));
    io.AddMouseWheelEvent(mouse.wheel().x, mouse.wheel().y);

    // Forward Key events to ImGui
    auto& keyboard = m_input.keyboard();
    for (const auto& [im_key, ember_key] : keys) {
    	io.AddKeyEvent(im_key, keyboard.down(ember_key));
    }

    // Forward modifier keys to ImGui
    io.AddKeyEvent(ImGuiKey_ModShift, keyboard.shift());
    io.AddKeyEvent(ImGuiKey_ModAlt, keyboard.alt());
    io.AddKeyEvent(ImGuiKey_ModCtrl, keyboard.ctrl());
    io.AddKeyEvent(ImGuiKey_ModSuper, keyboard.down(Key::LeftOS) || keyboard.down(Key::RightOS));

    // Forward text input to ImGui
    for (char32_t c : keyboard.text) {
    	io.AddInputCharacter(c);
    }

    m_wants_text_input = io.WantTextInput;
    ImGui::NewFrame();
}

void ImGuiRenderer::end_layout()
{
	EMBER_ASSERT(ImGui::GetCurrentContext() == m_context && "Mismatched begin_layout()/end_layout() calls!");
	ImGui::Render();
	ImGui::SetCurrentContext(nullptr);
}

// Overlod that calls the main implementation
bool ImGuiRenderer::begin_batch(Batcher*& batch, Rectf& bounds) {
	return begin_batch({ ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y }, batch, bounds);
}

bool ImGuiRenderer::begin_batch(const glm::vec2& size, Batcher*& batch, Rectf& bounds) {
	ImVec2 min_im = ImGui::GetCursorScreenPos();
	glm::vec2 min = { min_im.x, min_im.y };
	Rectf screenspace = { min.x, min.y, size.x, size.y };

	auto clip_min_im = ImGui::GetWindowDrawList()->GetClipRectMin();
	auto clip_max_im = ImGui::GetWindowDrawList()->GetClipRectMax();
	Rectf clip = { clip_min_im.x, clip_min_im.y, clip_max_im.x - clip_min_im.x, clip_max_im.y - clip_min_im.y };
	Recti scissor = screenspace.get_intersection(clip).scale(scale).to_int();

	ImGui::Dummy({ size.x, size.y });

	if (!m_batcher_pool.empty()) {
		batch = m_batcher_pool.back().release();
		m_batcher_pool.pop_back();
	} else {
		batch = new Batcher(m_gpu);
	}

	batch->clear();
	m_batchers_used.push_back(batch);
	m_batchers_stack.push(batch);

	// Add a custom callback command to render the batch
	ImGui::GetWindowDrawList()->AddCallback(nullptr, (void*)(intptr_t)m_batchers_used.size());

	glm::mat3 translation_mat = glm::translate(glm::mat3(1.0f), min);
	batch->push_matrix(translation_mat);
	batch->push_scissor(scissor);
	bounds = { 0, 0, screenspace.w, screenspace.h };

	return scissor.w > 0 && scissor.h > 0;
}

void ImGuiRenderer::end_batch() {
	EMBER_ASSERT(!m_batchers_stack.empty());
	auto* batch = m_batchers_stack.top();
	batch->pop_matrix();
	batch->pop_scissor();
	m_batchers_stack.pop();
}

void ImGuiRenderer::render() {
	EMBER_ASSERT(ImGui::GetCurrentContext() == nullptr);
	ImGui::SetCurrentContext(m_context);

	ImDrawData* data = ImGui::GetDrawData();
	if (!data || data->TotalVtxCount <= 0) {
		ImGui::SetCurrentContext(nullptr);
		return;
	}

	// Resize vertex buffer if necessary
	if (static_cast<size_t>(data->TotalVtxCount) > m_vertices.size()) {
		m_vertices.resize(data->TotalVtxCount);
	}

	// Resize index buffer if necessary
	if (static_cast<size_t>(data->TotalIdxCount) > m_indices.size()) {
		m_indices.resize(data->TotalIdxCount);
	}

	// Copy data from ImGui buffers to our own
	size_t vertex_offset = 0;
	size_t index_offset = 0;
	for (int i = 0; i < data->CmdListsCount; ++i) {
		const ImDrawList* cmd_list = data->CmdLists[i];
		std::memcpy(m_vertices.data() + vertex_offset, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImGuiVertex));
		std::memcpy(m_indices.data() + index_offset, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));
		vertex_offset += cmd_list->VtxBuffer.Size;
		index_offset += cmd_list->IdxBuffer.Size;
	}

	// upload buffer data to the GPU mesh
	m_mesh->set_vertices(std::span(m_vertices.data(), vertex_offset));
	m_mesh->set_indices(std::span(m_indices.data(), index_offset));

	auto framebuffer_size = m_gpu->framebuffer().size();
    glm::mat4 ortho_matrix = glm::ortho(
        0.0f, (float)framebuffer_size.x,
        (float)framebuffer_size.y, 0.0f
    );

    glm::mat4 scale_matrix = glm::scale(
        glm::mat4(1.0f),
        glm::vec3(data->FramebufferScale.x, data->FramebufferScale.y, 1.0f)
    );

	m_projection = ortho_matrix * scale_matrix;
	m_material->vertex.set_uniform_buffer(m_projection);

	// Create a single DrawCommand to be reused for standard ImGui geometry
	DrawCommand pass(&m_gpu->framebuffer(), *m_mesh, *m_material);
	pass.blend_mode = BlendMode(BlendOp::Add, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha);

	// Render command lists
	size_t global_vtx_offset = 0;
	size_t global_idx_offset = 0;
	for (int i = 0; i < data->CmdListsCount; i++) {
		const ImDrawList* cmd_list = data->CmdLists[i];
		for (const auto& cmd : cmd_list->CmdBuffer) {
			Recti scissor = {
				(int)(cmd.ClipRect.x * data->FramebufferScale.x),
				(int)(cmd.ClipRect.y * data->FramebufferScale.y),
				(int)((cmd.ClipRect.z - cmd.ClipRect.x) * data->FramebufferScale.x),
				(int)((cmd.ClipRect.w - cmd.ClipRect.y) * data->FramebufferScale.y)
			};

			if (scissor.w <= 0 || scissor.h <= 0)
				continue;

			if (cmd.UserCallback == nullptr) {
				// This is a standard ImGui draw call
				auto texture_index = cmd.TextureId;
				m_material->fragment.samplers = { m_bound_textures[texture_index], TextureSampler() };

				pass.scissor = scissor;
				pass.index_count = cmd.ElemCount;
				pass.vertex_offset = cmd.VtxOffset + global_vtx_offset;
				pass.index_offset = cmd.IdxOffset + global_idx_offset;
				m_gpu->submit(pass);
			} else {
				// This is a Batcher draw call
				size_t batch_index = (size_t)(intptr_t)cmd.UserCallbackData;

				if (batch_index > 0 && batch_index <= m_batchers_used.size()) {
					// Get the correct batcher and render it using the main projection matrix
					auto* batcher = m_batchers_used[batch_index - 1];
					batcher->render(m_gpu->framebuffer(), m_projection, scissor);
				}
			}
		}

		global_vtx_offset += cmd_list->VtxBuffer.Size;
		global_idx_offset += cmd_list->IdxBuffer.Size;
	}

	ImGui::SetCurrentContext(nullptr);
}

ImTextureID ImGuiRenderer::texture_id(Handle<Texture> handle) {
	auto id = m_bound_textures.size();
	m_bound_textures.push_back(handle);
	return (ImTextureID)(uintptr_t)id;
}

bool ImGuiRenderer::wants_text_input() const {
	return m_wants_text_input;
}
