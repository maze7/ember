#pragma once

#include <ember/core/common.h>
#include <ember/memory/memory.h>
#include <ember/script/binding.h>
#include <ember/script/host.h>

#include <optional>

/**
 * Ember::ScriptUi: a client's debug UI for its scripts, in Dear ImGui. The `ui` library that `panel` frames
 * draw with, the windows the panels open in, and an inspector's view of what a host knows of an entity. A
 * client's tools link it; a dedicated server never does.
 */
namespace ember::script
{
	/**
	 * The `ui` library: Dear ImGui's widgets for client scripts' `panel` frames, immediate mode, as a game's
	 * own panels are written. ui.text, ui.value, ui.button, ui.checkbox, ui.slider, ui.header, ui.tree,
	 * ui.separator and ui.same_line; each raises outside a panel's frame. Bind it with a client's packs.
	 */
	void bind_ui(Binding& binding) noexcept;

	/** Which script panels are open, by name: a debug UI keeps it from frame to frame. */
	struct PanelWindows
	{
		HashMap<u64, bool> open{&memory::heap(MemoryTag::Engine)};
	};

	/**
	 * The script panels: while `list` is open, a window listing them with a checkbox each; then every open one in
	 * a window of its own, its frame run inside. One whose frame failed says so instead, until its module
	 * reloads. Main thread, inside an ImGui frame, between the world's runs.
	 */
	void draw_panels(Host& host, PanelWindows& windows, bool* list) noexcept;

	/** What an inspection's buttons asked for: one of the entity's graph slots pushed into a state. */
	struct StatePush
	{
		u32 slot = 0;
		String state{&memory::heap(MemoryTag::Engine)};
	};

	/**
	 * An inspection, as an inspector shows it, into the current ImGui window: each graph slot's state, how long
	 * it has been in it and its marks; the stories and where each waits; the modifiers; the watched entity's
	 * last transitions. With `pushes`, a button for every state of each slot: the one pressed comes back.
	 */
	[[nodiscard]] std::optional<StatePush> draw_inspection(const Inspection& inspection, bool pushes) noexcept;
}
