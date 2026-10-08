#include <ember/script/ui.h>

#include <ember/core/hash.h>
#include <ember/script/lua.h>

#include <imgui.h>

namespace ember::script
{
	namespace
	{
		[[nodiscard]] Heap& heap() noexcept { return memory::heap(MemoryTag::Engine); }

		/** A panel's frame is open: its window is the current one. */
		void check_panel(lua_State* L, const char* what)
		{
			if (!host_of(L).in_panel())
				luaL_error(L, "%s draws in a panel's frame: panel \"name\" { frame = function() ... end }", what);
		}

		int ui_text(lua_State* L)
		{
			check_panel(L, "ui.text");
			size_t length	 = 0;
			const char* text = luaL_tolstring(L, 1, &length);
			ImGui::TextUnformatted(text, text + length);
			return 0;
		}

		int ui_value(lua_State* L)
		{
			check_panel(L, "ui.value");
			const char* label = luaL_checkstring(L, 1);
			size_t length	  = 0;
			const char* value = luaL_tolstring(L, 2, &length);
			ImGui::Text("%s: %.*s", label, static_cast<int>(length), value);
			return 0;
		}

		int ui_button(lua_State* L)
		{
			check_panel(L, "ui.button");
			lua_pushboolean(L, ImGui::Button(luaL_checkstring(L, 1)));
			return 1;
		}

		int ui_checkbox(lua_State* L)
		{
			check_panel(L, "ui.checkbox");
			bool on = lua_toboolean(L, 2) != 0;
			(void)ImGui::Checkbox(luaL_checkstring(L, 1), &on);
			lua_pushboolean(L, on);
			return 1;
		}

		int ui_slider(lua_State* L)
		{
			check_panel(L, "ui.slider");
			const char* label = luaL_checkstring(L, 1);
			f32 value		  = static_cast<f32>(luaL_checknumber(L, 2));
			const f32 low	  = static_cast<f32>(luaL_checknumber(L, 3));
			const f32 high	  = static_cast<f32>(luaL_checknumber(L, 4));
			(void)ImGui::SliderFloat(label, &value, low, high);
			lua_pushnumber(L, static_cast<f64>(value));
			return 1;
		}

		int ui_header(lua_State* L)
		{
			check_panel(L, "ui.header");
			lua_pushboolean(L, ImGui::CollapsingHeader(luaL_checkstring(L, 1)));
			return 1;
		}

		/**
		 * ui.tree(label, body, opened?): body runs while the node is open, open at first when opened says so, and
		 * the node closes whatever body does.
		 */
		int ui_tree(lua_State* L)
		{
			check_panel(L, "ui.tree");
			const char* label = luaL_checkstring(L, 1);
			luaL_checktype(L, 2, LUA_TFUNCTION);
			const ImGuiTreeNodeFlags flags =
				lua_toboolean(L, 3) ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
			if (!ImGui::TreeNodeEx(label, flags))
				return 0;
			lua_pushvalue(L, 2);
			const int status = lua_pcall(L, 0, 0, 0);
			ImGui::TreePop();
			if (status != LUA_OK)
				lua_error(L); // its message, on top, onwards to the frame's caller
			return 0;
		}

		int ui_separator(lua_State* L)
		{
			check_panel(L, "ui.separator");
			ImGui::Separator();
			return 0;
		}

		int ui_same_line(lua_State* L)
		{
			check_panel(L, "ui.same_line");
			ImGui::SameLine();
			return 0;
		}

		constexpr ContextMask CLIENT = ContextMask::Client;

		constexpr Function UI[] = {
			{"text", "(text: any) -> ()", ui_text, CLIENT},
			{"value", "(label: string, value: any) -> ()", ui_value, CLIENT},
			{"button", "(label: string) -> boolean", ui_button, CLIENT},
			{"checkbox", "(label: string, on: boolean) -> boolean", ui_checkbox, CLIENT},
			{"slider", "(label: string, value: number, low: number, high: number) -> number", ui_slider, CLIENT},
			{"header", "(label: string) -> boolean", ui_header, CLIENT},
			{"tree", "(label: string, body: () -> (), opened: boolean?) -> ()", ui_tree, CLIENT},
			{"separator", "() -> ()", ui_separator, CLIENT},
			{"same_line", "() -> ()", ui_same_line, CLIENT},
		};

		constexpr ImVec4 FAILED_COLOR = {1.0f, 0.45f, 0.4f, 1.0f};
		constexpr ImVec4 PASSED_COLOR = {0.55f, 0.55f, 0.55f, 1.0f};
	}

	void bind_ui(Binding& binding) noexcept { binding.library("ui", UI); }

	void draw_panels(Host& host, PanelWindows& windows, bool* list) noexcept
	{
		const u32 count = host.panel_count();
		if (list != nullptr && *list)
		{
			ImGui::SetNextWindowSize({260.0f, 0.0f}, ImGuiCond_FirstUseEver);
			if (ImGui::Begin("Script panels", list))
			{
				if (count == 0)
					ImGui::TextDisabled("No client script declares a panel.");
				for (u32 i = 0; i < count; ++i)
				{
					String label(host.panel_name(i), &heap());
					if (host.panel_failed(i))
						label += " (failed)";
					bool& open = windows.open[hash_text(host.panel_name(i))];
					(void)ImGui::Checkbox(label.c_str(), &open);
				}
			}
			ImGui::End();
		}

		for (u32 i = 0; i < count; ++i)
		{
			const auto it = windows.open.find(hash_text(host.panel_name(i)));
			if (it == windows.open.end() || !it->second)
				continue;
			const String title(host.panel_name(i), &heap());
			ImGui::SetNextWindowSize({320.0f, 240.0f}, ImGuiCond_FirstUseEver);
			if (ImGui::Begin(title.c_str(), &it->second))
			{
				if (host.panel_failed(i))
					ImGui::TextColored(FAILED_COLOR,
									   "Its frame failed: the script's problem says where. Save it to try again.");
				else
					(void)host.run_panel(i);
			}
			ImGui::End();
		}
	}

	std::optional<StatePush> draw_inspection(const Inspection& inspection, bool pushes) noexcept
	{
		std::optional<StatePush> pushed;
		if (!inspection.found)
		{
			ImGui::TextDisabled("Not here: gone, or not yet arrived.");
			return pushed;
		}
		ImGui::TextDisabled("as of tick %u", inspection.tick);

		for (u32 s = 0; s < inspection.slots.size(); ++s)
		{
			const Inspection::Slot& slot = inspection.slots[s];
			ImGui::PushID(static_cast<int>(s));
			ImGui::Separator();
			ImGui::Text("%s", slot.graph.c_str());
			ImGui::SameLine();
			if (slot.disabled)
				ImGui::TextColored(FAILED_COLOR, "stopped: a handler failed");
			else
				ImGui::Text("in %s for %u ticks (since %u)", slot.state.empty() ? "-" : slot.state.c_str(),
							slot.elapsed, slot.since);
			for (const Inspection::Mark& mark : slot.marks)
			{
				if (slot.elapsed >= mark.at)
					ImGui::TextColored(PASSED_COLOR, "  %s at %u", mark.name.c_str(), mark.at);
				else
					ImGui::Text("  %s at %u", mark.name.c_str(), mark.at);
			}
			if (pushes && !slot.states.empty())
			{
				ImGui::TextDisabled("push into:");
				const f32 right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
				for (u32 k = 0; k < slot.states.size(); ++k)
				{
					const char* name = slot.states[k].c_str();
					const f32 width	 = ImGui::CalcTextSize(name).x + ImGui::GetStyle().FramePadding.x * 2.0f;
					if (k > 0 && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width < right)
						ImGui::SameLine();
					if (ImGui::SmallButton(name))
						pushed = StatePush{.slot = s, .state = String(slot.states[k], &heap())};
				}
			}
			ImGui::PopID();
		}

		if (!inspection.stories.empty())
		{
			ImGui::Separator();
			for (const Inspection::Story& story : inspection.stories)
			{
				if (story.waiting == "failed")
					ImGui::TextColored(FAILED_COLOR, "story %s: failed", story.name.c_str());
				else if (story.waiting.empty())
					ImGui::Text("story %s: running", story.name.c_str());
				else if (!story.event.empty())
					ImGui::Text("story %s: %s \"%s\", line %u", story.name.c_str(), story.waiting.c_str(),
								story.event.c_str(), story.line);
				else if (story.wake != 0)
					ImGui::Text("story %s: %s until tick %u, line %u", story.name.c_str(), story.waiting.c_str(),
								story.wake, story.line);
				else
					ImGui::Text("story %s: %s", story.name.c_str(), story.waiting.c_str());
			}
		}

		if (!inspection.modifiers.empty())
		{
			ImGui::Separator();
			for (const Inspection::Modifier& modifier : inspection.modifiers)
			{
				if (modifier.left != 0)
					ImGui::Text("%s x%u, power %.0f, %u ticks left", modifier.name.c_str(), modifier.stacks,
								modifier.power, modifier.left);
				else
					ImGui::Text("%s x%u, power %.0f, until cured", modifier.name.c_str(), modifier.stacks,
								modifier.power);
			}
		}

		if (!inspection.history.empty())
		{
			ImGui::Separator();
			for (u32 i = static_cast<u32>(inspection.history.size()); i-- > 0;)
			{
				const Inspection::Step& step = inspection.history[i];
				ImGui::TextColored(PASSED_COLOR, "tick %u: slot %u %s -> %s", step.tick, step.slot, step.from.c_str(),
								   step.to.c_str());
			}
		}
		return pushed;
	}
}
