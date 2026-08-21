#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <ember/platform/platform.h>

#include <ember/containers/pool.h>
#include <ember/core/logger.h>
#include <ember/input/input.h>
#include <ember/memory/memory.h>
#include <ember/sync/thread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace
{
	constexpr SDL_InitFlags SDL_SUBSYSTEMS = SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD;

	std::atomic_bool s_platform_claimed = false;

	void log_sdl_failure(const char* operation) noexcept
	{
		const char* error = SDL_GetError();

		EMBER_ERROR("{} failed: {}", operation, error != nullptr ? error : "unknown SDL error");
	}

	struct SdlWindow
	{
		SDL_Window* handle = nullptr;
		ember::u32 id	   = 0;

		SdlWindow() noexcept = default;

		SdlWindow(SDL_Window* native, ember::u32 window_id) noexcept : handle(native), id(window_id) {}

		~SdlWindow() noexcept
		{
			if (handle != nullptr)
				SDL_DestroyWindow(handle);
		}

		SdlWindow(const SdlWindow&)			   = delete;
		SdlWindow& operator=(const SdlWindow&) = delete;

		SdlWindow(SdlWindow&& other) noexcept
			: handle(std::exchange(other.handle, nullptr)), id(std::exchange(other.id, 0))
		{
		}

		SdlWindow& operator=(SdlWindow&&) = delete;
	};

	struct SdlCursor
	{
		SDL_Cursor* handle = nullptr;

		SdlCursor() noexcept = default;

		explicit SdlCursor(SDL_Cursor* native) noexcept : handle(native) {}

		~SdlCursor() noexcept
		{
			if (handle != nullptr)
				SDL_DestroyCursor(handle);
		}

		SdlCursor(const SdlCursor&)			   = delete;
		SdlCursor& operator=(const SdlCursor&) = delete;

		SdlCursor(SdlCursor&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}

		SdlCursor& operator=(SdlCursor&&) = delete;
	};

	struct SdlGamepad
	{
		SDL_Gamepad* handle = nullptr;
		ember::GamepadId id{};
		ember::GamepadInfo info{};
	};

	static_assert(std::is_nothrow_move_constructible_v<SdlWindow>);
	static_assert(std::is_nothrow_destructible_v<SdlWindow>);
	static_assert(std::is_nothrow_move_constructible_v<SdlCursor>);
	static_assert(std::is_nothrow_destructible_v<SdlCursor>);

	[[nodiscard]] std::optional<SDL_WindowFlags> to_sdl_window_flags(ember::WindowFlags flags) noexcept
	{
		constexpr ember::u16 KNOWN_FLAGS = static_cast<ember::u16>(ember::WindowFlags::Resizable) |
										   static_cast<ember::u16>(ember::WindowFlags::Fullscreen) |
										   static_cast<ember::u16>(ember::WindowFlags::Hidden) |
										   static_cast<ember::u16>(ember::WindowFlags::Borderless) |
										   static_cast<ember::u16>(ember::WindowFlags::HighPixelDensity);

		const ember::u16 value = static_cast<ember::u16>(flags);

		if ((value & static_cast<ember::u16>(~KNOWN_FLAGS)) != 0)
			return std::nullopt;

		SDL_WindowFlags result = 0;

		if (ember::has_any(flags, ember::WindowFlags::Resizable))
			result |= SDL_WINDOW_RESIZABLE;

		if (ember::has_any(flags, ember::WindowFlags::Fullscreen))
			result |= SDL_WINDOW_FULLSCREEN;

		if (ember::has_any(flags, ember::WindowFlags::Hidden))
			result |= SDL_WINDOW_HIDDEN;

		if (ember::has_any(flags, ember::WindowFlags::Borderless))
			result |= SDL_WINDOW_BORDERLESS;

		if (ember::has_any(flags, ember::WindowFlags::HighPixelDensity))
		{
			result |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
		}

#if EMBER_GPU_VULKAN
		result |= SDL_WINDOW_VULKAN;
#endif

		return result;
	}

	constexpr std::array<SDL_SystemCursor, 20> SDL_SYSTEM_CURSORS = {
		SDL_SYSTEM_CURSOR_DEFAULT,	   SDL_SYSTEM_CURSOR_TEXT,		  SDL_SYSTEM_CURSOR_WAIT,
		SDL_SYSTEM_CURSOR_CROSSHAIR,   SDL_SYSTEM_CURSOR_PROGRESS,	  SDL_SYSTEM_CURSOR_NWSE_RESIZE,
		SDL_SYSTEM_CURSOR_NESW_RESIZE, SDL_SYSTEM_CURSOR_EW_RESIZE,	  SDL_SYSTEM_CURSOR_NS_RESIZE,
		SDL_SYSTEM_CURSOR_MOVE,		   SDL_SYSTEM_CURSOR_NOT_ALLOWED, SDL_SYSTEM_CURSOR_POINTER,
		SDL_SYSTEM_CURSOR_NW_RESIZE,   SDL_SYSTEM_CURSOR_N_RESIZE,	  SDL_SYSTEM_CURSOR_NE_RESIZE,
		SDL_SYSTEM_CURSOR_E_RESIZE,	   SDL_SYSTEM_CURSOR_SE_RESIZE,	  SDL_SYSTEM_CURSOR_S_RESIZE,
		SDL_SYSTEM_CURSOR_SW_RESIZE,   SDL_SYSTEM_CURSOR_W_RESIZE,
	};

	static_assert(SDL_SYSTEM_CURSORS.size() == static_cast<size_t>(ember::SystemCursor::ResizeW) + 1);

	[[nodiscard]] std::optional<ember::MouseButton> mouse_button_from_sdl(ember::u8 button) noexcept
	{
		switch (button)
		{
			case SDL_BUTTON_LEFT:
				return ember::MouseButton::Left;

			case SDL_BUTTON_MIDDLE:
				return ember::MouseButton::Middle;

			case SDL_BUTTON_RIGHT:
				return ember::MouseButton::Right;

			case SDL_BUTTON_X1:
				return ember::MouseButton::X1;

			case SDL_BUTTON_X2:
				return ember::MouseButton::X2;

			default:
				return std::nullopt;
		}
	}

	[[nodiscard]] ember::Key key_from_sdl(SDL_Scancode scancode) noexcept
	{
		const auto value = static_cast<ember::i32>(scancode);

		if (value <= 0 || value >= static_cast<ember::i32>(ember::Key::Count))
		{
			return ember::Key::Unknown;
		}

		return static_cast<ember::Key>(value);
	}

	[[nodiscard]] ember::GamepadType gamepad_type_from_sdl(SDL_GamepadType type) noexcept
	{
		switch (type)
		{
			case SDL_GAMEPAD_TYPE_STANDARD:
				return ember::GamepadType::Standard;

			case SDL_GAMEPAD_TYPE_XBOX360:
				return ember::GamepadType::Xbox360;

			case SDL_GAMEPAD_TYPE_XBOXONE:
				return ember::GamepadType::XboxOne;

			case SDL_GAMEPAD_TYPE_PS3:
				return ember::GamepadType::PlayStation3;

			case SDL_GAMEPAD_TYPE_PS4:
				return ember::GamepadType::PlayStation4;

			case SDL_GAMEPAD_TYPE_PS5:
				return ember::GamepadType::PlayStation5;

			case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
				return ember::GamepadType::SwitchPro;

			case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
				return ember::GamepadType::SwitchJoyConLeft;

			case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
				return ember::GamepadType::SwitchJoyConRight;

			case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
				return ember::GamepadType::SwitchJoyConPair;

			case SDL_GAMEPAD_TYPE_GAMECUBE:
				return ember::GamepadType::GameCube;

			case SDL_GAMEPAD_TYPE_STEAM:
				return ember::GamepadType::Steam;

			case SDL_GAMEPAD_TYPE_UNKNOWN:
			case SDL_GAMEPAD_TYPE_COUNT:
			default:
				return ember::GamepadType::Unknown;
		}
	}

	[[nodiscard]] ember::GamepadInfo gamepad_info_from_sdl(SDL_Gamepad* gamepad) noexcept
	{
		ember::GamepadInfo info{};

		if (const char* name = SDL_GetGamepadName(gamepad); name != nullptr)
		{
			const size_t length = std::min(std::char_traits<char>::length(name), info.name_buffer.size() - 1);

			std::memcpy(info.name_buffer.data(), name, length);

			info.name_buffer[length] = '\0';
		}

		info.type = gamepad_type_from_sdl(SDL_GetGamepadType(gamepad));

		info.vendor = SDL_GetGamepadVendor(gamepad);

		info.product = SDL_GetGamepadProduct(gamepad);

		info.product_version = SDL_GetGamepadProductVersion(gamepad);

		return info;
	}

	[[nodiscard]] Uint16 rumble_intensity(ember::f32 value) noexcept
	{
		if (!std::isfinite(value))
			return 0;

		value = std::clamp(value, 0.0f, 1.0f);

		constexpr ember::f32 MAX_VALUE = static_cast<ember::f32>(std::numeric_limits<Uint16>::max());

		return static_cast<Uint16>(value * MAX_VALUE + 0.5f);
	}

	static_assert(static_cast<int>(ember::GamepadButton::A) == SDL_GAMEPAD_BUTTON_SOUTH);

	static_assert(static_cast<int>(ember::GamepadButton::Right) == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

	static_assert(static_cast<int>(ember::GamepadButton::Count) == SDL_GAMEPAD_BUTTON_MISC1);

	static_assert(static_cast<int>(ember::GamepadAxis::RightTrigger) == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
}

namespace ember
{
	struct Platform::Impl
	{
		using WindowPool   = Pool<Window, SdlWindow>;
		using CursorPool   = Pool<Cursor, SdlCursor>;
		using WindowLookup = HashMap<u32, WindowHandle>;

		Impl() noexcept
			: windows(MemoryTag::Platform), cursors(MemoryTag::Platform),
			  windows_by_id(WindowLookup::allocator_type{&memory::heap(MemoryTag::Platform)})
		{
			bool expected = false;

			if (!s_platform_claimed.compare_exchange_strong(
					expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
			{
				EMBER_ERROR("Only one Ember Platform may exist at a time");
				return;
			}

			claimed = true;

			SDL_SetMainReady();

			if (!SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"))
			{
				EMBER_WARN("Failed to enable background gamepad events: {}", SDL_GetError());
			}

			if (!SDL_Init(SDL_SUBSYSTEMS))
			{
				log_sdl_failure("SDL_Init");
				SDL_Quit();

				claimed = false;
				s_platform_claimed.store(false, std::memory_order_release);
				return;
			}

			windows.reserve(8);
			cursors.reserve(16);
			windows_by_id.reserve(8);

			initialized = true;
		}

		~Impl() noexcept
		{
			for (SdlGamepad& gamepad : gamepads)
			{
				if (gamepad.handle != nullptr)
				{
					(void)SDL_RumbleGamepad(gamepad.handle, 0, 0, 0);

					SDL_CloseGamepad(gamepad.handle);
				}

				gamepad = {};
			}

			active_cursor = {};
			cursors.clear();

			windows_by_id.clear();
			windows.clear();

			if (initialized)
				SDL_Quit();

			if (claimed)
			{
				s_platform_claimed.store(false, std::memory_order_release);
			}
		}

		[[nodiscard]] bool is_owner_thread() const noexcept { return current_thread_id() == owner_thread; }

		[[nodiscard]] WindowHandle window_from_id(SDL_WindowID id) const noexcept
		{
			const auto it = windows_by_id.find(static_cast<u32>(id));

			if (it == windows_by_id.end() || !windows.contains(it->second))
			{
				return {};
			}

			return it->second;
		}

		[[nodiscard]] SdlGamepad* find_gamepad(GamepadId id) noexcept
		{
			for (SdlGamepad& gamepad : gamepads)
			{
				if (gamepad.handle != nullptr && gamepad.id == id)
				{
					return &gamepad;
				}
			}

			return nullptr;
		}

		u32 owner_thread = current_thread_id();

		WindowPool windows;
		CursorPool cursors;
		WindowLookup windows_by_id;

		std::array<SdlGamepad, Input::MAX_GAMEPADS> gamepads{};

		CursorHandle active_cursor{};

		bool initialized		 = false;
		bool claimed			 = false;
		bool gamepads_discovered = false;
	};

	Platform::Platform() noexcept
	{
		Impl* impl = memory::new_object<Impl>(MemoryTag::Platform);

		if (impl->initialized)
			m_impl = impl;
		else
			memory::delete_object(MemoryTag::Platform, impl);
	}

	Platform::~Platform() noexcept { reset(); }

	Platform::Platform(Platform&& other) noexcept : m_impl(std::exchange(other.m_impl, nullptr)) {}

	Platform& Platform::operator=(Platform&& other) noexcept
	{
		if (this == &other)
			return *this;

		reset();
		m_impl = std::exchange(other.m_impl, nullptr);
		return *this;
	}

	void Platform::reset() noexcept
	{
		if (m_impl == nullptr)
			return;

		if (!m_impl->is_owner_thread())
		{
			EMBER_ASSERT(false && "Platform destroyed from non-owner thread");

			std::terminate();
		}

		Impl* impl = std::exchange(m_impl, nullptr);

		memory::delete_object(MemoryTag::Platform, impl);
	}

	PumpResult Platform::pump_events(Input& input) noexcept
	{
		PumpResult result{};

		if (m_impl == nullptr)
			return result;

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return result;

		auto open_gamepad = [this, &input](GamepadId id, u64 timestamp) noexcept
		{
			if (!id.is_valid())
				return;

			if (SdlGamepad* existing = m_impl->find_gamepad(id); existing != nullptr)
			{
				existing->info = gamepad_info_from_sdl(existing->handle);

				(void)input.connect_gamepad(id, existing->info, timestamp);

				return;
			}

			SdlGamepad* free_slot = nullptr;

			for (SdlGamepad& gamepad : m_impl->gamepads)
			{
				if (gamepad.handle == nullptr)
				{
					free_slot = &gamepad;
					break;
				}
			}

			if (free_slot == nullptr)
				return;

			SDL_Gamepad* native = SDL_OpenGamepad(static_cast<SDL_JoystickID>(id.value));

			if (native == nullptr)
			{
				log_sdl_failure("SDL_OpenGamepad");
				return;
			}

			GamepadInfo info = gamepad_info_from_sdl(native);

			if (!input.connect_gamepad(id, info, timestamp).has_value())
			{
				SDL_CloseGamepad(native);

				EMBER_WARN(
					"Ignoring gamepad {} because all "
					"Ember input slots are in use",
					id.value);

				return;
			}

			free_slot->handle = native;
			free_slot->id	  = id;
			free_slot->info	  = info;
		};

		auto close_gamepad = [this, &input](GamepadId id, u64 timestamp) noexcept
		{
			input.disconnect_gamepad(id, timestamp);

			SdlGamepad* gamepad = m_impl->find_gamepad(id);

			if (gamepad == nullptr)
				return;

			(void)SDL_RumbleGamepad(gamepad->handle, 0, 0, 0);

			SDL_CloseGamepad(gamepad->handle);
			*gamepad = {};
		};

		auto remap_gamepad = [this, &input](GamepadId id) noexcept
		{
			SdlGamepad* gamepad = m_impl->find_gamepad(id);

			if (gamepad == nullptr)
				return;

			gamepad->info = gamepad_info_from_sdl(gamepad->handle);

			input.remap_gamepad(id, gamepad->info);
		};

		auto discover_gamepads = [&open_gamepad](u64 timestamp) noexcept -> bool
		{
			int count			= 0;
			SDL_JoystickID* ids = SDL_GetGamepads(&count);

			if (ids == nullptr)
			{
				log_sdl_failure("SDL_GetGamepads");
				return false;
			}

			for (int i = 0; i < count; ++i)
			{
				open_gamepad(GamepadId{static_cast<u32>(ids[i])}, timestamp);
			}

			SDL_free(ids);
			return true;
		};

		const u64 initial_timestamp = SDL_GetTicksNS();

		// Reconcile native handles into a newly supplied/cleared Input.
		for (const SdlGamepad& gamepad : m_impl->gamepads)
		{
			if (gamepad.handle != nullptr)
			{
				(void)input.connect_gamepad(gamepad.id, gamepad.info, initial_timestamp);
			}
		}

		if (!m_impl->gamepads_discovered)
		{
			m_impl->gamepads_discovered = discover_gamepads(initial_timestamp);
		}

		bool refresh_gamepads = false;
		SDL_Event event{};

		while (SDL_PollEvent(&event))
		{
			switch (event.type)
			{
				case SDL_EVENT_QUIT:
					result.quit_requested = true;
					break;

				case SDL_EVENT_KEY_DOWN:
				case SDL_EVENT_KEY_UP:
				{
					const Key key = key_from_sdl(event.key.scancode);

					if (key != Key::Unknown)
					{
						input.on_key(key, event.key.down, event.key.repeat, event.key.timestamp);
					}

					break;
				}

				case SDL_EVENT_TEXT_INPUT:
				{
					const WindowHandle window = m_impl->window_from_id(event.text.windowID);

					if (!window.is_null() && event.text.text != nullptr)
					{
						input.on_text(event.text.text, window);
					}

					break;
				}

				case SDL_EVENT_TEXT_EDITING:
				{
					const WindowHandle window = m_impl->window_from_id(event.edit.windowID);

					if (!window.is_null())
					{
						input.on_composition(
							event.edit.text != nullptr ? std::string_view{event.edit.text} : std::string_view{},
							event.edit.start,
							event.edit.length,
							window);
					}

					break;
				}

				case SDL_EVENT_MOUSE_MOTION:
				{
					if (event.motion.which == SDL_TOUCH_MOUSEID || event.motion.which == SDL_PEN_MOUSEID)
					{
						break;
					}

					const WindowHandle window = m_impl->window_from_id(event.motion.windowID);

					if (!window.is_null())
					{
						input.on_mouse_move(
							{
								event.motion.x,
								event.motion.y,
							},
							{
								event.motion.xrel,
								event.motion.yrel,
							},
							window,
							event.motion.timestamp);
					}

					break;
				}

				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_MOUSE_BUTTON_UP:
				{
					if (event.button.which == SDL_TOUCH_MOUSEID || event.button.which == SDL_PEN_MOUSEID)
					{
						break;
					}

					const std::optional<MouseButton> button = mouse_button_from_sdl(event.button.button);

					if (!button)
						break;

					const WindowHandle window = m_impl->window_from_id(event.button.windowID);

					if (!window.is_null())
					{
						input.on_mouse_button(*button, event.button.down, window, event.button.timestamp);
					}

					break;
				}

				case SDL_EVENT_MOUSE_WHEEL:
				{
					if (event.wheel.which == SDL_TOUCH_MOUSEID || event.wheel.which == SDL_PEN_MOUSEID)
					{
						break;
					}

					const WindowHandle window = m_impl->window_from_id(event.wheel.windowID);

					if (window.is_null())
						break;

					const f32 direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;

					input.on_mouse_wheel(
						{
							event.wheel.x * direction,
							event.wheel.y * direction,
						},
						window,
						event.wheel.timestamp);

					break;
				}

				case SDL_EVENT_GAMEPAD_ADDED:
					open_gamepad(GamepadId{static_cast<u32>(event.gdevice.which)}, event.gdevice.timestamp);
					break;

				case SDL_EVENT_GAMEPAD_REMOVED:
					close_gamepad(GamepadId{static_cast<u32>(event.gdevice.which)}, event.gdevice.timestamp);

					refresh_gamepads = true;
					break;

				case SDL_EVENT_GAMEPAD_REMAPPED:
					remap_gamepad(GamepadId{static_cast<u32>(event.gdevice.which)});
					break;

				case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
				case SDL_EVENT_GAMEPAD_BUTTON_UP:
				{
					if (event.gbutton.button >= static_cast<Uint8>(GamepadButton::Count))
					{
						break;
					}

					input.on_gamepad_button(
						GamepadId{static_cast<u32>(event.gbutton.which)},
						static_cast<GamepadButton>(event.gbutton.button),
						event.gbutton.down,
						event.gbutton.timestamp);

					break;
				}

				case SDL_EVENT_GAMEPAD_AXIS_MOTION:
				{
					if (event.gaxis.axis >= static_cast<Uint8>(GamepadAxis::Count))
					{
						break;
					}

					const f32 value = event.gaxis.value >= 0 ? static_cast<f32>(event.gaxis.value) / 32767.0f
															 : static_cast<f32>(event.gaxis.value) / 32768.0f;

					input.on_gamepad_axis(
						GamepadId{static_cast<u32>(event.gaxis.which)},
						static_cast<GamepadAxis>(event.gaxis.axis),
						value,
						event.gaxis.timestamp);

					break;
				}

				case SDL_EVENT_WINDOW_FOCUS_LOST:
				{
					const WindowHandle window = m_impl->window_from_id(event.window.windowID);

					if (!window.is_null())
					{
						input.on_focus_lost(window, event.window.timestamp);
					}

					break;
				}

				case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
					if (!m_impl->window_from_id(event.window.windowID).is_null())
					{
						result.quit_requested = true;
					}
					break;

				default:
					break;
			}
		}

		if (refresh_gamepads && !discover_gamepads(SDL_GetTicksNS()))
		{
			m_impl->gamepads_discovered = false;
		}

		// Poll first, publish second.
		input.publish(SDL_GetTicksNS());

		return result;
	}

	WindowHandle Platform::create_window(const WindowDef& def) noexcept
	{
		if (m_impl == nullptr)
			return {};

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return {};

		if (def.size.x <= 0 || def.size.y <= 0)
		{
			EMBER_WARN("Invalid window size {}x{}", def.size.x, def.size.y);

			return {};
		}

		const std::optional<SDL_WindowFlags> flags = to_sdl_window_flags(def.flags);

		if (!flags)
		{
			EMBER_WARN("Window description contains unknown flags");

			return {};
		}

		SDL_Window* native = SDL_CreateWindow(def.title, def.size.x, def.size.y, *flags);

		if (native == nullptr)
		{
			log_sdl_failure("SDL_CreateWindow");
			return {};
		}

		const SDL_WindowID id = SDL_GetWindowID(native);

		if (id == 0)
		{
			log_sdl_failure("SDL_GetWindowID");
			SDL_DestroyWindow(native);
			return {};
		}

		const WindowHandle handle = m_impl->windows.emplace(native, static_cast<u32>(id));

		const bool inserted = m_impl->windows_by_id.emplace(static_cast<u32>(id), handle).second;

		if (!inserted)
		{
			EMBER_ERROR("SDL returned duplicate window ID {}", id);

			(void)m_impl->windows.erase(handle);
			return {};
		}

		return handle;
	}

	glm::uvec2 Platform::window_pixel_size(WindowHandle handle) const noexcept
	{
		if (m_impl == nullptr)
			return {};

		EMBER_ASSERT(m_impl->is_owner_thread());

		SdlWindow* window = m_impl->windows.try_get(handle);

		if (window == nullptr)
			return {};

		int width  = 0;
		int height = 0;

		if (!SDL_GetWindowSizeInPixels(window->handle, &width, &height))
		{
			log_sdl_failure("SDL_GetWindowSizeInPixels");
			return {};
		}

		// Minimized windows legitimately report zero; callers treat that as "suspended".
		return {
			static_cast<u32>(width < 0 ? 0 : width),
			static_cast<u32>(height < 0 ? 0 : height),
		};
	}

	void Platform::destroy_window(WindowHandle handle) noexcept
	{
		if (m_impl == nullptr)
			return;

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return;

		auto* window = m_impl->windows.try_get(handle);

		if (window == nullptr)
		{
			EMBER_ASSERT(false && "Invalid WindowHandle");
			return;
		}

		m_impl->windows_by_id.erase(window->id);
		(void)m_impl->windows.erase(handle);
	}

	void Platform::set_window_title(WindowHandle handle, const char* title) noexcept
	{
		if (m_impl == nullptr)
			return;

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return;

		auto* window = m_impl->windows.try_get(handle);

		if (window == nullptr)
		{
			EMBER_ASSERT(false && "Invalid WindowHandle");
			return;
		}

		if (!SDL_SetWindowTitle(window->handle, title))
		{
			log_sdl_failure("SDL_SetWindowTitle");
		}
	}

	void Platform::set_window_size(WindowHandle handle, u32 width, u32 height) noexcept
	{
		if (m_impl == nullptr)
			return;

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return;

		auto* window = m_impl->windows.try_get(handle);

		if (window == nullptr)
		{
			EMBER_ASSERT(false && "Invalid WindowHandle");
			return;
		}

		constexpr u32 MAX_SIZE = static_cast<u32>(std::numeric_limits<int>::max());

		if (width == 0 || height == 0 || width > MAX_SIZE || height > MAX_SIZE)
		{
			EMBER_WARN("Invalid window size {}x{}", width, height);

			return;
		}

		if (!SDL_SetWindowSize(window->handle, static_cast<int>(width), static_cast<int>(height)))
		{
			log_sdl_failure("SDL_SetWindowSize");
		}
	}

	void Platform::set_cursor_mode(WindowHandle handle, CursorMode mode) noexcept
	{
		if (m_impl == nullptr)
			return;

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return;

		auto* window = m_impl->windows.try_get(handle);

		if (window == nullptr)
		{
			EMBER_ASSERT(false && "Invalid WindowHandle");
			return;
		}

		switch (mode)
		{
			case CursorMode::Normal:
				if (!SDL_SetWindowRelativeMouseMode(window->handle, false))
				{
					log_sdl_failure("SDL_SetWindowRelativeMouseMode");
					return;
				}

				if (!SDL_ShowCursor())
					log_sdl_failure("SDL_ShowCursor");
				break;

			case CursorMode::Hidden:
				if (!SDL_SetWindowRelativeMouseMode(window->handle, false))
				{
					log_sdl_failure("SDL_SetWindowRelativeMouseMode");
					return;
				}

				if (!SDL_HideCursor())
					log_sdl_failure("SDL_HideCursor");
				break;

			case CursorMode::Relative:
				if (!SDL_SetWindowRelativeMouseMode(window->handle, true))
				{
					log_sdl_failure("SDL_SetWindowRelativeMouseMode");
				}
				break;

			default:
				EMBER_ASSERT(false && "Invalid CursorMode");
				break;
		}
	}

	CursorHandle Platform::create_system_cursor(SystemCursor cursor) noexcept
	{
		if (m_impl == nullptr)
			return {};

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return {};

		const size_t index = static_cast<size_t>(cursor);

		if (index >= SDL_SYSTEM_CURSORS.size())
		{
			EMBER_ASSERT(false && "Invalid SystemCursor");
			return {};
		}

		SDL_Cursor* native = SDL_CreateSystemCursor(SDL_SYSTEM_CURSORS[index]);

		if (native == nullptr)
		{
			log_sdl_failure("SDL_CreateSystemCursor");
			return {};
		}

		return m_impl->cursors.emplace(native);
	}

	CursorHandle Platform::create_cursor(const CursorImageView& image) noexcept
	{
		if (m_impl == nullptr)
			return {};

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return {};

		if (image.size.x <= 0 || image.size.y <= 0 || image.hotspot.x < 0 || image.hotspot.y < 0 ||
			image.hotspot.x >= image.size.x || image.hotspot.y >= image.size.y)
		{
			EMBER_WARN("Invalid custom cursor description");

			return {};
		}

		const size_t width = static_cast<size_t>(image.size.x);

		const size_t height = static_cast<size_t>(image.size.y);

		if (width > std::numeric_limits<size_t>::max() / 4)
		{
			EMBER_WARN("Custom cursor width overflows");
			return {};
		}

		const size_t row_bytes = width * 4;

		const size_t source_pitch = image.pitch == 0 ? row_bytes : static_cast<size_t>(image.pitch);

		if (source_pitch < row_bytes)
		{
			EMBER_WARN("Custom cursor pitch is smaller than one row");

			return {};
		}

		if (height > 1 && source_pitch > (std::numeric_limits<size_t>::max() - row_bytes) / (height - 1))
		{
			EMBER_WARN("Custom cursor byte size overflows");

			return {};
		}

		const size_t required_size = row_bytes + (height > 1 ? source_pitch * (height - 1) : 0);

		if (image.rgba8.size() < required_size)
		{
			EMBER_WARN("Custom cursor requires {} bytes, got {}", required_size, image.rgba8.size());

			return {};
		}

		SDL_Surface* surface = SDL_CreateSurface(image.size.x, image.size.y, SDL_PIXELFORMAT_RGBA32);

		if (surface == nullptr)
		{
			log_sdl_failure("SDL_CreateSurface");
			return {};
		}

		if (surface->pixels == nullptr || surface->pitch <= 0 || static_cast<size_t>(surface->pitch) < row_bytes)
		{
			EMBER_ERROR("SDL returned an invalid cursor surface");

			SDL_DestroySurface(surface);
			return {};
		}

		auto* destination = static_cast<std::byte*>(surface->pixels);

		for (size_t row = 0; row < height; ++row)
		{
			std::memcpy(
				destination + row * static_cast<size_t>(surface->pitch),
				image.rgba8.data() + row * source_pitch,
				row_bytes);
		}

		SDL_Cursor* native = SDL_CreateColorCursor(surface, image.hotspot.x, image.hotspot.y);

		SDL_DestroySurface(surface);

		if (native == nullptr)
		{
			log_sdl_failure("SDL_CreateColorCursor");
			return {};
		}

		return m_impl->cursors.emplace(native);
	}

	void Platform::destroy_cursor(CursorHandle cursor) noexcept
	{
		if (m_impl == nullptr)
			return;

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return;

		if (!m_impl->cursors.contains(cursor))
		{
			EMBER_ASSERT(false && "Invalid CursorHandle");
			return;
		}

		const bool was_active = m_impl->active_cursor == cursor;

		(void)m_impl->cursors.erase(cursor);

		if (was_active)
			m_impl->active_cursor = {};
	}

	void Platform::set_cursor(CursorHandle cursor) noexcept
	{
		if (m_impl == nullptr)
			return;

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return;

		SdlCursor* native = m_impl->cursors.try_get(cursor);

		if (native == nullptr)
		{
			EMBER_ASSERT(false && "Invalid CursorHandle");
			return;
		}

		if (!SDL_SetCursor(native->handle))
		{
			log_sdl_failure("SDL_SetCursor");
			return;
		}

		m_impl->active_cursor = cursor;
	}

	void Platform::reset_cursor() noexcept
	{
		if (m_impl == nullptr)
			return;

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return;

		SDL_Cursor* default_cursor = SDL_GetDefaultCursor();

		if (default_cursor == nullptr)
		{
			log_sdl_failure("SDL_GetDefaultCursor");
			return;
		}

		if (!SDL_SetCursor(default_cursor))
		{
			log_sdl_failure("SDL_SetCursor");
			return;
		}

		m_impl->active_cursor = {};
	}

	void Platform::set_cursor_visible(bool visible) noexcept
	{
		if (m_impl == nullptr)
			return;

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return;

		const bool succeeded = visible ? SDL_ShowCursor() : SDL_HideCursor();

		if (!succeeded)
		{
			log_sdl_failure(visible ? "SDL_ShowCursor" : "SDL_HideCursor");
		}
	}

	void Platform::start_text_input(WindowHandle handle) noexcept
	{
		if (m_impl == nullptr)
			return;

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return;

		auto* window = m_impl->windows.try_get(handle);

		if (window == nullptr)
		{
			EMBER_ASSERT(false && "Invalid WindowHandle");
			return;
		}

		if (!SDL_StartTextInput(window->handle))
			log_sdl_failure("SDL_StartTextInput");
	}

	void Platform::stop_text_input(WindowHandle handle) noexcept
	{
		if (m_impl == nullptr)
			return;

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return;

		auto* window = m_impl->windows.try_get(handle);

		if (window == nullptr)
		{
			EMBER_ASSERT(false && "Invalid WindowHandle");
			return;
		}

		if (!SDL_StopTextInput(window->handle))
			log_sdl_failure("SDL_StopTextInput");
	}

	void Platform::set_clipboard_text(const char* text) noexcept
	{
		if (m_impl == nullptr)
			return;

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return;

		if (!SDL_SetClipboardText(text))
			log_sdl_failure("SDL_SetClipboardText");
	}

	std::string Platform::clipboard_text() const noexcept
	{
		if (m_impl == nullptr)
			return {};

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return {};

		char* native = SDL_GetClipboardText();

		if (native == nullptr)
		{
			log_sdl_failure("SDL_GetClipboardText");
			return {};
		}

		std::string result{native};
		SDL_free(native);
		return result;
	}

	void Platform::rumble(GamepadId gamepad, f32 low_intensity, f32 high_intensity, u32 duration_ms) noexcept
	{
		if (m_impl == nullptr || !gamepad.is_valid())
		{
			return;
		}

		EMBER_ASSERT(m_impl->is_owner_thread());

		if (!m_impl->is_owner_thread())
			return;

		SdlGamepad* native = m_impl->find_gamepad(gamepad);

		if (native == nullptr)
			return;

		if (!SDL_RumbleGamepad(
				native->handle, rumble_intensity(low_intensity), rumble_intensity(high_intensity), duration_ms))
		{
			log_sdl_failure("SDL_RumbleGamepad");
		}
	}

	NativeWindow Platform::native_window(WindowHandle handle) const noexcept
	{
		if (m_impl == nullptr || !m_impl->is_owner_thread())
			return {};

		auto* window = m_impl->windows.try_get(handle);

		if (window == nullptr)
			return {};

		return {.backend = WindowBackend::Sdl3, .value = window->handle};
	}
}
