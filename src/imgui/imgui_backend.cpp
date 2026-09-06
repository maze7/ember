#include <ember/imgui/imgui_backend.h>

#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/input/input.h>
#include <ember/platform/platform.h>
#include <ember/imgui/embedded_shader.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <string>

namespace ember::imgui
{
	namespace
	{
		struct PushBlock
		{
			f32 scale[2];
			f32 translate[2];
			u32 vertex_buffer;
			u32 texture;
			u32 sampler;
			u32 first_vertex;
		};
		static_assert(sizeof(PushBlock) == PUSH_CONSTANT_BYTES);
		static_assert(sizeof(ImDrawVert) == 20);
		static_assert(sizeof(ImDrawIdx) == 2, "pipeline binds U16 indices");

		/// ImGui is a single global context; this is its ember half. Owner thread only.
		struct State
		{
			gpu::Device* device = nullptr;
			Platform* platform	= nullptr;
			GraphicsPipelineHandle pipeline{};
			SamplerHandle sampler{};
			CursorHandle cursors[ImGuiMouseCursor_COUNT]{};
			ImGuiMouseCursor last_cursor = ImGuiMouseCursor_Arrow;
			WindowHandle window{};
			std::string clipboard;
			bool text_input_active = false;
		};

		State s_state;

		SystemCursor to_system_cursor(ImGuiMouseCursor cursor) noexcept
		{
			switch (cursor)
			{
				case ImGuiMouseCursor_TextInput:
					return SystemCursor::Text;
				case ImGuiMouseCursor_ResizeAll:
					return SystemCursor::Move;
				case ImGuiMouseCursor_ResizeNS:
					return SystemCursor::ResizeVertical;
				case ImGuiMouseCursor_ResizeEW:
					return SystemCursor::ResizeHorizontal;
				case ImGuiMouseCursor_ResizeNESW:
					return SystemCursor::ResizeNESW;
				case ImGuiMouseCursor_ResizeNWSE:
					return SystemCursor::ResizeNWSE;
				case ImGuiMouseCursor_Hand:
					return SystemCursor::Pointer;
				case ImGuiMouseCursor_Wait:
					return SystemCursor::Wait;
				case ImGuiMouseCursor_Progress:
					return SystemCursor::Progress;
				case ImGuiMouseCursor_NotAllowed:
					return SystemCursor::NotAllowed;
				default:
					return SystemCursor::Default;
			}
		}

		ImGuiKey to_imgui_key(Key key) noexcept
		{
			const u32 value = static_cast<u32>(key);

			if (key >= Key::A && key <= Key::Z)
				return static_cast<ImGuiKey>(ImGuiKey_A + (value - static_cast<u32>(Key::A)));
			if (key >= Key::D1 && key <= Key::D9)
				return static_cast<ImGuiKey>(ImGuiKey_1 + (value - static_cast<u32>(Key::D1)));
			if (key >= Key::F1 && key <= Key::F12)
				return static_cast<ImGuiKey>(ImGuiKey_F1 + (value - static_cast<u32>(Key::F1)));
			if (key >= Key::F13 && key <= Key::F24)
				return static_cast<ImGuiKey>(ImGuiKey_F13 + (value - static_cast<u32>(Key::F13)));
			if (key >= Key::Keypad1 && key <= Key::Keypad9)
				return static_cast<ImGuiKey>(ImGuiKey_Keypad1 + (value - static_cast<u32>(Key::Keypad1)));

			switch (key)
			{
				case Key::D0:
					return ImGuiKey_0;
				case Key::Keypad0:
					return ImGuiKey_Keypad0;
				case Key::Enter:
					return ImGuiKey_Enter;
				case Key::Escape:
					return ImGuiKey_Escape;
				case Key::Backspace:
					return ImGuiKey_Backspace;
				case Key::Tab:
					return ImGuiKey_Tab;
				case Key::Space:
					return ImGuiKey_Space;
				case Key::Minus:
					return ImGuiKey_Minus;
				case Key::Equals:
					return ImGuiKey_Equal;
				case Key::LeftBracket:
					return ImGuiKey_LeftBracket;
				case Key::RightBracket:
					return ImGuiKey_RightBracket;
				case Key::Backslash:
					return ImGuiKey_Backslash;
				case Key::Semicolon:
					return ImGuiKey_Semicolon;
				case Key::Apostrophe:
					return ImGuiKey_Apostrophe;
				case Key::Tilde:
					return ImGuiKey_GraveAccent;
				case Key::Comma:
					return ImGuiKey_Comma;
				case Key::Period:
					return ImGuiKey_Period;
				case Key::Slash:
					return ImGuiKey_Slash;
				case Key::Capslock:
					return ImGuiKey_CapsLock;
				case Key::PrintScreen:
					return ImGuiKey_PrintScreen;
				case Key::ScrollLock:
					return ImGuiKey_ScrollLock;
				case Key::Pause:
					return ImGuiKey_Pause;
				case Key::Insert:
					return ImGuiKey_Insert;
				case Key::Home:
					return ImGuiKey_Home;
				case Key::PageUp:
					return ImGuiKey_PageUp;
				case Key::Delete:
					return ImGuiKey_Delete;
				case Key::End:
					return ImGuiKey_End;
				case Key::PageDown:
					return ImGuiKey_PageDown;
				case Key::Right:
					return ImGuiKey_RightArrow;
				case Key::Left:
					return ImGuiKey_LeftArrow;
				case Key::Down:
					return ImGuiKey_DownArrow;
				case Key::Up:
					return ImGuiKey_UpArrow;
				case Key::Numlock:
					return ImGuiKey_NumLock;
				case Key::Application:
					return ImGuiKey_Menu;
				case Key::KeypadDivide:
					return ImGuiKey_KeypadDivide;
				case Key::KeypadMultiply:
					return ImGuiKey_KeypadMultiply;
				case Key::KeypadMinus:
					return ImGuiKey_KeypadSubtract;
				case Key::KeypadPlus:
					return ImGuiKey_KeypadAdd;
				case Key::KeypadEnter:
					return ImGuiKey_KeypadEnter;
				case Key::KeypadPeriod:
					return ImGuiKey_KeypadDecimal;
				case Key::KeypadEquals:
					return ImGuiKey_KeypadEqual;
				case Key::LeftControl:
					return ImGuiKey_LeftCtrl;
				case Key::LeftShift:
					return ImGuiKey_LeftShift;
				case Key::LeftAlt:
					return ImGuiKey_LeftAlt;
				case Key::LeftOS:
					return ImGuiKey_LeftSuper;
				case Key::RightControl:
					return ImGuiKey_RightCtrl;
				case Key::RightShift:
					return ImGuiKey_RightShift;
				case Key::RightAlt:
					return ImGuiKey_RightAlt;
				case Key::RightOS:
					return ImGuiKey_RightSuper;
				default:
					return ImGuiKey_None;
			}
		}

		const char* get_clipboard(ImGuiContext*) noexcept
		{
			s_state.clipboard = s_state.platform->clipboard_text();
			return s_state.clipboard.c_str();
		}

		void set_clipboard(ImGuiContext*, const char* text) noexcept { s_state.platform->set_clipboard_text(text); }

		void apply_cursor() noexcept
		{
			if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) != 0)
				return;

			const ImGuiMouseCursor cursor = ImGui::GetMouseCursor();
			if (cursor == s_state.last_cursor)
				return;
			s_state.last_cursor = cursor;

			if (cursor == ImGuiMouseCursor_None)
			{
				s_state.platform->set_cursor_visible(false);
				return;
			}

			s_state.platform->set_cursor_visible(true);

			const CursorHandle handle = s_state.cursors[cursor];
			if (!handle.is_null())
				s_state.platform->set_cursor(handle);
			else
				s_state.platform->reset_cursor();
		}

		static_assert(sizeof(TextureHandle) == sizeof(u32));

		void* pack_handle(TextureHandle handle) noexcept
		{
			return reinterpret_cast<void*>(static_cast<uintptr_t>(std::bit_cast<u32>(handle)));
		}

		TextureHandle unpack_handle(void* user) noexcept
		{
			return std::bit_cast<TextureHandle>(static_cast<u32>(reinterpret_cast<uintptr_t>(user)));
		}

		/// Serves one texture request from the core. TexID carries the bindless
		/// index for shaders; the TextureHandle rides BackendUserData so update
		/// and destroy can round trip.
		void process_texture(gpu::Device& device, ImTextureData* texture) noexcept
		{
			if (texture->Status == ImTextureStatus_WantCreate)
			{
				if (texture->Format != ImTextureFormat_RGBA32)
				{
					EMBER_ERROR("imgui: unsupported atlas format");
					return;
				}

				const TextureHandle handle = device.create_texture({
					.name	= "imgui.atlas",
					.extent = {static_cast<u32>(texture->Width), static_cast<u32>(texture->Height), 1},
					.format = gpu::TextureFormat::RGBA8Unorm,
					.initial_data =
						{
							static_cast<const u8*>(texture->GetPixels()),
							static_cast<u64>(texture->GetSizeInBytes()),
						},
				});

				if (handle.is_null())
					return;

				texture->BackendUserData = pack_handle(handle);
				texture->SetTexID(static_cast<ImTextureID>(bindless_index(handle)));
				texture->SetStatus(ImTextureStatus_OK);
				return;
			}

			if (texture->Status == ImTextureStatus_WantUpdates)
			{
				// update_texture replaces the whole subresource. The atlas is small
				// and updates are bursty while new glyphs rasterize, so the full
				// image goes up rather than the dirty rects.
				device.update_texture(
					unpack_handle(texture->BackendUserData),
					0,
					0,
					{static_cast<const u8*>(texture->GetPixels()), static_cast<u64>(texture->GetSizeInBytes())});

				texture->SetStatus(ImTextureStatus_OK);
				return;
			}

			if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0)
			{
				device.destroy(unpack_handle(texture->BackendUserData));
				texture->BackendUserData = nullptr;
				texture->SetTexID(ImTextureID_Invalid);
				texture->SetStatus(ImTextureStatus_Destroyed);
			}
		}
	}

	bool init(gpu::Device& device, Platform& platform, const BackendDef& def) noexcept
	{
		EMBER_ASSERT(s_state.device == nullptr);

		auto& shader = def.shader.empty() ? render::embedded::imgui_shader() : def.shader;

		if (def.color_format == gpu::TextureFormat::Undefined)
		{
			EMBER_ERROR("imgui: invalid BackendDef");
			return false;
		}

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		ImGuiIO& io				= ImGui::GetIO();
		io.BackendPlatformName	= "ember";
		io.BackendRendererName	= "ember::gpu";
		io.BackendFlags		   |= ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_RendererHasVtxOffset |
								  ImGuiBackendFlags_RendererHasTextures;
		io.ConfigFlags		   |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;

		ImGuiPlatformIO& platform_io			= ImGui::GetPlatformIO();
		platform_io.Platform_GetClipboardTextFn = get_clipboard;
		platform_io.Platform_SetClipboardTextFn = set_clipboard;

		const SamplerHandle sampler = device.create_sampler({
			.name	   = "imgui.sampler",
			.address_u = gpu::AddressMode::ClampToEdge,
			.address_v = gpu::AddressMode::ClampToEdge,
		});

		const GraphicsPipelineHandle pipeline = device.create_graphics_pipeline({
			.name		   = "imgui",
			.vertex		   = {.code = shader, .entry = "vs_main"},
			.fragment	   = {.code = shader, .entry = "fs_main"},
			.color_formats = {def.color_format},
			.blend		   = gpu::BlendPreset::AlphaBlend,
		});

		if (sampler.is_null() || pipeline.is_null())
		{
			EMBER_ERROR("imgui: resource creation failed");

			if (!sampler.is_null())
				device.destroy(sampler);
			if (!pipeline.is_null())
				device.destroy(pipeline);

			ImGui::DestroyContext();
			return false;
		}

		for (int i = 0; i < ImGuiMouseCursor_COUNT; ++i)
			s_state.cursors[i] = platform.create_system_cursor(to_system_cursor(i));

		s_state.device	 = &device;
		s_state.platform = &platform;
		s_state.pipeline = pipeline;
		s_state.sampler	 = sampler;
		return true;
	}

	void shutdown(gpu::Device& device) noexcept
	{
		if (s_state.device == nullptr)
			return;

		if (s_state.text_input_active)
			s_state.platform->stop_text_input(s_state.window);

		for (CursorHandle& cursor : s_state.cursors)
		{
			if (!cursor.is_null())
				s_state.platform->destroy_cursor(cursor);
		}

		device.destroy(s_state.pipeline);
		device.destroy(s_state.sampler);

		for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
		{
			if (texture->BackendUserData != nullptr)
			{
				device.destroy(unpack_handle(texture->BackendUserData));
				texture->BackendUserData = nullptr;
				texture->SetTexID(ImTextureID_Invalid);
				texture->SetStatus(ImTextureStatus_Destroyed);
			}
		}

		ImGui::DestroyContext();
		s_state = {};
	}

	void new_frame(const Input& input, WindowHandle window, Extent2D display, f32 dt) noexcept
	{
		EMBER_ASSERT(s_state.device != nullptr);

		s_state.window = window;

		ImGuiIO& io	   = ImGui::GetIO();
		io.DisplaySize = ImVec2(
			static_cast<f32>(display.width > 0 ? display.width : 1),
			static_cast<f32>(display.height > 0 ? display.height : 1));
		io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;

		const Mouse& mouse = input.mouse();
		if (mouse.window() == window)
			io.AddMousePosEvent(mouse.x(), mouse.y());
		else
			io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);

		struct ButtonMap
		{
			MouseButton button;
			int index;
		};
		constexpr ButtonMap BUTTONS[] = {
			{MouseButton::Left, 0},
			{MouseButton::Right, 1},
			{MouseButton::Middle, 2},
			{MouseButton::X1, 3},
			{MouseButton::X2, 4},
		};

		for (const auto& [button, index] : BUTTONS)
		{
			if (mouse.pressed(button))
				io.AddMouseButtonEvent(index, true);
			if (mouse.released(button))
				io.AddMouseButtonEvent(index, false);
		}

		const glm::vec2 wheel = mouse.wheel();
		if (wheel.x != 0.0f || wheel.y != 0.0f)
			io.AddMouseWheelEvent(wheel.x, wheel.y);

		const Keyboard& keyboard = input.keyboard();

		// Modifiers land before keys so shortcut state is coherent within the frame.
		io.AddKeyEvent(ImGuiMod_Ctrl, keyboard.control());
		io.AddKeyEvent(ImGuiMod_Shift, keyboard.shift());
		io.AddKeyEvent(ImGuiMod_Alt, keyboard.alt());
		io.AddKeyEvent(ImGuiMod_Super, keyboard.down(Key::LeftOS) || keyboard.down(Key::RightOS));

		for (u32 i = 1; i < Keyboard::KEY_COUNT; ++i)
		{
			const Key key		= static_cast<Key>(i);
			const bool pressed	= keyboard.pressed(key);
			const bool released = keyboard.released(key);
			if (!pressed && !released)
				continue;

			const ImGuiKey mapped = to_imgui_key(key);
			if (mapped == ImGuiKey_None)
				continue;

			if (pressed)
				io.AddKeyEvent(mapped, true);
			if (released)
				io.AddKeyEvent(mapped, false);
		}

		const std::string_view text = keyboard.text();
		if (!text.empty())
		{
			const std::string terminated(text);
			io.AddInputCharactersUTF8(terminated.c_str());
		}

		if (io.WantTextInput != s_state.text_input_active)
		{
			s_state.text_input_active = io.WantTextInput;
			if (io.WantTextInput)
				s_state.platform->start_text_input(window);
			else
				s_state.platform->stop_text_input(window);
		}

		apply_cursor();

		ImGui::NewFrame();
	}

	void render(gpu::CommandList& cmd) noexcept
	{
		EMBER_ASSERT(s_state.device != nullptr);

		ImGui::Render();

		const ImDrawData* draw_data = ImGui::GetDrawData();
		if (draw_data == nullptr)
			return;

		if (draw_data->Textures != nullptr)
		{
			for (ImTextureData* texture : *draw_data->Textures)
			{
				if (texture->Status != ImTextureStatus_OK)
					process_texture(*s_state.device, texture);
			}
		}

		if (draw_data->TotalVtxCount <= 0 || draw_data->CmdListsCount <= 0)
			return;

		gpu::TransientAllocator& transient = s_state.device->transient();

		const auto vertices = transient.allocate_array<ImDrawVert>(static_cast<u32>(draw_data->TotalVtxCount));
		const auto indices	= transient.allocate_array<ImDrawIdx>(static_cast<u32>(draw_data->TotalIdxCount));
		if (!vertices.valid() || !indices.valid())
		{
			EMBER_WARN("imgui: transient ring exhausted, dropping the UI frame");
			return;
		}

		u32 vertex_cursor = 0;
		u32 index_cursor  = 0;
		for (const ImDrawList* list : draw_data->CmdLists)
		{
			std::memcpy(
				vertices.data + vertex_cursor,
				list->VtxBuffer.Data,
				static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
			std::memcpy(
				indices.data + index_cursor,
				list->IdxBuffer.Data,
				static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));

			vertex_cursor += static_cast<u32>(list->VtxBuffer.Size);
			index_cursor  += static_cast<u32>(list->IdxBuffer.Size);
		}

		EMBER_GPU_ZONE(cmd, "imgui");

		cmd.set_pipeline(s_state.pipeline);
		cmd.set_index_buffer(indices.buffer, gpu::IndexFormat::U16, indices.offset);

		const f32 width	 = draw_data->DisplaySize.x;
		const f32 height = draw_data->DisplaySize.y;

		PushBlock push{
			.scale		   = {2.0f / width, -2.0f / height},
			.translate	   = {-1.0f, 1.0f},
			.vertex_buffer = bindless_index(vertices.buffer),
			.texture	   = 0,
			.sampler	   = bindless_index(s_state.sampler),
		};

		// draw_indexed's base vertex is added to the fetched index before it
		// reaches SV_VertexID, so it carries the ring element base for free.
		const u32 base_vertex = vertices.first_element();

		u32 list_vertex = 0;
		u32 list_index	= 0;

		for (const ImDrawList* list : draw_data->CmdLists)
		{
			for (const ImDrawCmd& draw : list->CmdBuffer)
			{
				if (draw.UserCallback != nullptr)
				{
					draw.UserCallback(list, &draw);
					continue;
				}

				const f32 clip_x0 = std::max(draw.ClipRect.x - draw_data->DisplayPos.x, 0.0f);
				const f32 clip_y0 = std::max(draw.ClipRect.y - draw_data->DisplayPos.y, 0.0f);
				const f32 clip_x1 = std::min(draw.ClipRect.z - draw_data->DisplayPos.x, width);
				const f32 clip_y1 = std::min(draw.ClipRect.w - draw_data->DisplayPos.y, height);
				if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0)
					continue;

				cmd.set_scissor({
					.x		= static_cast<i32>(clip_x0),
					.y		= static_cast<i32>(clip_y0),
					.width	= static_cast<u32>(clip_x1 - clip_x0),
					.height = static_cast<u32>(clip_y1 - clip_y0),
				});

				push.texture	  = static_cast<u32>(draw.GetTexID());
				push.first_vertex = base_vertex + list_vertex + draw.VtxOffset;
				cmd.set_push_constants(push);

				cmd.draw_indexed(draw.ElemCount, 1, list_index + draw.IdxOffset, 0, 0);
			}

			list_vertex += static_cast<u32>(list->VtxBuffer.Size);
			list_index	+= static_cast<u32>(list->IdxBuffer.Size);
		}
	}

	void discard() noexcept
	{
		if (s_state.device != nullptr)
			ImGui::EndFrame();
	}

	bool wants_mouse() noexcept { return s_state.device != nullptr && ImGui::GetIO().WantCaptureMouse; }

	bool wants_keyboard() noexcept { return s_state.device != nullptr && ImGui::GetIO().WantCaptureKeyboard; }
}
