#include <ember/core/hash.h>
#include <ember/ecs/system.h>
#include <ember/script/components.h>
#include <ember/script/host.h>
#include <ember/script/source.h>
#include <ember/script/ui.h>

#include <gtest/gtest.h>

#include <imgui.h>

#include <optional>
#include <utility>

/**
 * The scripts' debug UI, headless: a Dear ImGui context with no window, frames begun and ended around the
 * panels' windows, the ui library's widgets from Luau, and an inspection drawn with its push buttons.
 */
namespace ui_test
{
	using namespace ember;
	using namespace ember::script;

	struct Scratch
	{
		u32 value = 0;
		f64 wide  = 0.0;
		bool flag = false;
	};
	EMBER_COMPONENT(Scratch, Sim);

	inline constexpr auto PLAIN = ecs::prefab("plain", Scratch{});

	class UiTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			auto parsed = Aliases::parse("scripts", bytes_of(R"({ "aliases": {} })"));
			ASSERT_TRUE(parsed.has_value());
			m_aliases = std::move(*parsed);

			m_registry.prefabs(PLAIN);
			register_components(m_registry);
			m_registry.present_exclusive<&run_present>();
			m_world.emplace(m_registry, ecs::Role::Client);
			m_host = &m_world->add_resource<Host>(*m_world, HostDef{.budget = 20'000});
			m_host->binding().expose<Scratch>({.derived = true});
			bind_ui(m_host->binding());

			ImGui::CreateContext();
			ImGuiIO& io			  = ImGui::GetIO();
			io.DisplaySize		  = {1280.0f, 720.0f};
			io.DeltaTime		  = 1.0f / 60.0f;
			io.IniFilename		  = nullptr;
			unsigned char* pixels = nullptr;
			int width = 0, height = 0;
			io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		}

		void TearDown() override { ImGui::DestroyContext(); }

		[[nodiscard]] static Span<const u8> bytes_of(StringView text) noexcept
		{
			return {reinterpret_cast<const u8*>(text.data()), text.size()};
		}

		void load(StringView path, StringView text)
		{
			auto compiled = compile(path, bytes_of(text), m_aliases);
			ASSERT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message.c_str());
			Vector<Source> sources(&memory::heap(MemoryTag::Scripting));
			sources.push_back(std::move(*compiled));
			m_host->reload(sources);
		}

		template <class F> void frame(F&& body)
		{
			ImGui::NewFrame();
			body();
			ImGui::Render();
		}

		[[nodiscard]] Scratch& scratch() { return m_world->registry.get<Scratch>(m_entity); }

		ecs::Registry m_registry;
		std::optional<ecs::World> m_world;
		Host* m_host = nullptr;
		Aliases m_aliases;
		ecs::Entity m_entity = ecs::NO_ENTITY;
	};

	TEST_F(UiTest, PanelsDrawWithTheUiLibraryAndKeepTheirStateAcrossAReload)
	{
		load("scripts/client/panels/count.luau", R"(
			panel "Counter" {
				state = { n = 0, on = true },
				frame = function(state)
					state.n += 1
					ui.text("frames")
					ui.value("n", state.n)
					state.on = ui.checkbox("on", state.on)
					if ui.button("add") then state.n += 100 end
					local v = ui.slider("speed", 0.5, 0, 1)
					ui.same_line()
					ui.separator()
					if ui.header("more") then ui.text("inside") end
					ui.tree("tree", function() ui.text("leaf") end)
					for e, s in world:query(Scratch) do s.value = state.n s.wide = v end
				end,
			}
		)");
		ASSERT_TRUE(m_host->problems().empty()) << m_host->problems().front().message.c_str();
		m_entity = m_world->spawn(PLAIN);
		ASSERT_EQ(m_host->panel_count(), 1u);
		EXPECT_EQ(m_host->panel_name(0), "Counter");

		PanelWindows windows;
		bool list						   = true;
		windows.open[hash_text("Counter")] = true;
		for (int i = 0; i < 3; ++i)
			frame([&] { draw_panels(*m_host, windows, &list); });
		EXPECT_EQ(scratch().value, 3u) << "a frame a frame, inside its window";
		EXPECT_DOUBLE_EQ(scratch().wide, 0.5);
		EXPECT_TRUE(m_host->problems().empty())
			<< (m_host->problems().empty() ? "" : m_host->problems().front().message.c_str());

		// Saved: the new frame counts by ten from where the old one's state stood.
		load("scripts/client/panels/count.luau", R"(
			panel "Counter" {
				state = { n = 0 },
				frame = function(state)
					state.n += 10
					for e, s in world:query(Scratch) do s.value = state.n end
				end,
			}
		)");
		frame([&] { draw_panels(*m_host, windows, &list); });
		EXPECT_EQ(scratch().value, 13u);

		// Closed, it runs no more.
		windows.open[hash_text("Counter")] = false;
		frame([&] { draw_panels(*m_host, windows, &list); });
		EXPECT_EQ(scratch().value, 13u);
	}

	TEST_F(UiTest, AFailingFrameStopsItsPanelAndTheUiLibraryIsForPanelsAlone)
	{
		load("scripts/client/panels/broken.luau", R"(
			panel "Broken" {
				frame = function()
					ui.tree("open", function() error("boom") end, true)
				end,
			}
			system("Present", function()
				ui.text("not here")
			end)
		)");
		m_entity = m_world->spawn(PLAIN);
		PanelWindows windows;
		windows.open[hash_text("Broken")] = true;
		frame([&] { draw_panels(*m_host, windows, nullptr); });
		EXPECT_TRUE(m_host->panel_failed(0));
		ASSERT_FALSE(m_host->problems().empty());
		EXPECT_NE(StringView(m_host->problems().front().message).find("boom"), StringView::npos);

		// The tree it opened was closed for it: the next frame's windows stand.
		frame([&] { draw_panels(*m_host, windows, nullptr); });

		m_host->clear_problems();
		m_host->set_present(1.0, 1.0);
		m_world->run(ecs::Phase::Present);
		bool found = false;
		for (const Problem& problem : m_host->problems())
			found = found || StringView(problem.message).find("draws in a panel's frame") != StringView::npos;
		EXPECT_TRUE(found) << "ui outside a panel is refused";
	}

	TEST_F(UiTest, AnInspectionDrawsWithItsPushButtons)
	{
		Inspection inspection;
		inspection.found	   = true;
		inspection.tick		   = 40;
		Inspection::Slot& slot = inspection.slots.emplace_back();
		slot.graph			   = "brown_slime";
		slot.state			   = "hop";
		slot.since			   = 30;
		slot.elapsed		   = 10;
		slot.states.push_back("doze");
		slot.states.push_back("hop");
		slot.marks.push_back({.name = "leap", .at = 6});
		inspection.stories.push_back({.name = "den", .waiting = "wait", .line = 12, .wake = 50});
		inspection.modifiers.push_back({.name = "burning", .stacks = 1, .power = 1.0f, .left = 60});
		inspection.history.push_back({.slot = 0, .from = "doze", .to = "hop", .tick = 30});

		std::optional<StatePush> pushed;
		frame(
			[&]
			{
				ImGui::Begin("inspect");
				pushed = draw_inspection(inspection, true);
				ImGui::End();
			});
		EXPECT_FALSE(pushed.has_value()) << "nothing pressed";
	}
}
