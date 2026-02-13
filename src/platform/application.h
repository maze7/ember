#pragma once

#include "assets/asset_manager.h"
#include "graphics/batcher.h"
#include "graphics/imgui_renderer.h"
#include "platform/window.h"

namespace Ember
{
	class RenderDevice;

	class Application
	{
	public:
		virtual ~Application();

		/// @brief Entrypoint of the Application, handles lifecycle.
		void run();

		/// @brief Called when the user would like to exit the program.
		void exit();

		/// @returns True if the program should continue running
		[[nodiscard]] bool is_running() const;

		/**
		 * @brief Callback that clients can use to initialize game state prior to the main loop starting.
		 *
		 * This function is called once, after all subsystems have been initialized. It is intended to be used by the
		 * consuming application to initialize any required game state prior to the main loop starting.
		 *
		 * @returns True if initialization succeeded.
		 */
		virtual bool init() { return true; }

		/**
		 * @brief Callback that is called once, before subsystems are disposed.
		 */
		virtual void cleanup() {}

		/**
		 * @brief Callback that is called to render any ImGui UI
		 */
		virtual void imgui() {}

		/**
		 * @brief Called any time an exit is requested (Window close button)
		 * @returns True if the program should exit.
		 */
		virtual bool exit_requested() { return true; }

		/**
		 * @brief Called once per frame. Intended for updating game state that doesn't require a fixed step.
		 * @param dt Delta time
		 */
		virtual void update_variable(double dt, double accumulator) {}

		/**
		 * @brief Called at a fixed frequency (e.g., 60hz). Intended for physics, network, AI updates.
		 * @param dt Detla time
		 */
		virtual void update_fixed(double dt) {}

		/**
		 * @brief Called once per frame. This functiomn can be used to render game state to the screen.
		 */
		virtual void render() {}

		/**
		 * @brief Returns the fixed timestep to use for update_fixed().
		 * Override to implement dynamic timestep adjustment (e.g., for network clock sync).
		 * Default returns 1/60 seconds (60hz).
		 */
		[[nodiscard]] virtual double get_fixed_timestep() const { return 1.0 / 60.0; }

	protected:
		/// Application should not be  constructed without being subclassed.
		Application();

		/// Window Instance
		Window m_window;

		/// Asset Manager
		AssetManager m_assets;

		/// Input Manager
		Input m_input;

		/// Default batcher
		Unique<Batcher> m_batcher;
		Unique<ImGuiRenderer> m_imgui;

	private:
		bool m_running = false;
	};
}
