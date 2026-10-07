#include <ember/assets/asset.h>
#include <ember/core/filesystem.h>
#include <ember/gpu/device.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>
#include <ember/script/script_asset.h>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#if defined(EMBER_PLATFORM_WINDOWS)
	#include <process.h>
#else
	#include <unistd.h>
#endif

/**
 * Scripts through a real asset manager over a scratch directory, as the asset tests drive one: the
 * library loads what is there, follows reloads, and hands hosts the batches a session passes on.
 */
namespace
{
	using namespace ember;
	using namespace ember::script;

	[[nodiscard]] int process_id() noexcept
	{
#if defined(EMBER_PLATFORM_WINDOWS)
		return _getpid();
#else
		return getpid();
#endif
	}

	class ScriptAssetTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			String scratch(&memory::heap(MemoryTag::Engine));
			ASSERT_TRUE(fs::temporary_directory(scratch).has_value());
			ASSERT_TRUE(fs::join(scratch, scratch, "ember_script_asset_tests_" + std::to_string(process_id())).has_value());
			m_root.assign(scratch.data(), scratch.size());

			if (const auto removed = fs::remove_tree(m_root); !removed)
			{
				ASSERT_EQ(removed.error().code, fs::FileErrorCode::NotFound);
			}
			ASSERT_TRUE(fs::create_directories(m_root + "/scripts/lib").has_value());
			ASSERT_TRUE(fs::create_directories(m_root + "/scripts/sim/rules").has_value());

			write("scripts/.luaurc", R"({ "aliases": { "lib": "./lib" } })");
			write("scripts/lib/z.luau", "return { base = 1 }");
			write("scripts/lib/a.luau", "local z = require('./z')\nreturn { k = z.base + 1 }");
			write("scripts/sim/rules/sword.luau", "local a = require('@lib/a')\nsystem('Act', function() end)");
			write("scripts/sim/plain.luau", "system('Act', function() end)");

			jobs::initialize({.worker_count = 4});
			m_assets.init(m_device, {.root = m_root.c_str(), .max_assets = 64, .hot_reload = false, .reload_delay_ms = 0});
			register_script_assets(m_assets);
		}

		void TearDown() override
		{
			m_library.reset();
			m_assets.shutdown();
			jobs::shutdown();
			(void)fs::remove_tree(m_root);
		}

		void write(const char* name, StringView text)
		{
			ASSERT_TRUE(fs::write_file(m_root + "/" + name, {reinterpret_cast<const u8*>(text.data()), text.size()})
							.has_value());
		}

		/**
		 * A save, as the frame loop sees it: the change, the pump that queues the reload, and the pumps
		 * that land it. The loader is a job, so the pumps go on until the library sees the payload move,
		 * or a broken one settle, as a game's frames would.
		 */
		void reload(const char* name)
		{
			m_assets.notify_changed(asset_id(name));
			pump();
			for (int i = 0; i < 200; ++i)
			{
				m_assets.wait_idle();
				pump();
				if (m_library->moved_since_handed_out() > 0)
					return;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		void pump() { m_assets.pump(++m_frame); }

		[[nodiscard]] Vector<Source> update(std::initializer_list<const char*> changed = {})
		{
			Vector<AssetChange> changes(&memory::heap(MemoryTag::Engine));
			for (const char* name : changed)
				changes.push_back({.name = String(name, &memory::heap(MemoryTag::Engine)), .id = asset_id(name)});

			Vector<Source> out(&memory::heap(MemoryTag::Scripting));
			m_library->update(m_assets, changes, out);
			return out;
		}

		[[nodiscard]] static Vector<String> paths(const Vector<Source>& sources)
		{
			Vector<String> out(&memory::heap(MemoryTag::Engine));
			for (const Source& source : sources)
				out.push_back(String(source.path, &memory::heap(MemoryTag::Engine)));
			return out;
		}

		std::string m_root;
		u64 m_frame = 0;
		gpu::Device m_device{gpu::DeviceDef{.adapter = gpu::AdapterPreference::Any}};
		AssetManager m_assets;
		std::optional<ScriptLibrary> m_library{std::in_place};
	};

	TEST_F(ScriptAssetTest, StartLoadsEveryScriptEachAfterWhatItRequires)
	{
		m_library->start(m_assets);
		EXPECT_EQ(m_library->count(), 4u);

		const Vector<Source> all = m_library->all();
		const Vector<String> order = paths(all);
		ASSERT_EQ(order.size(), 4u);
		EXPECT_EQ(order[0], "scripts/lib/z.luau"); // before a, which requires it, though a sorts first
		EXPECT_EQ(order[1], "scripts/lib/a.luau");
		EXPECT_EQ(order[2], "scripts/sim/plain.luau");
		EXPECT_EQ(order[3], "scripts/sim/rules/sword.luau");

		EXPECT_EQ(all[3].context, Context::Sim);
		ASSERT_EQ(all[3].imports.size(), 1u);
		EXPECT_EQ(all[3].imports[0].path, "scripts/lib/a.luau");
		EXPECT_NE(m_library->hash(), 0u);
		EXPECT_TRUE(update().empty()); // nothing moved since
	}

	TEST_F(ScriptAssetTest, ASaveMovesTheScriptAndEverythingThatRequiresIt)
	{
		m_library->start(m_assets);
		const u64 before = m_library->hash();

		write("scripts/lib/z.luau", "return { base = 5 }");
		reload("scripts/lib/z.luau");

		const Vector<String> moved = paths(update());
		ASSERT_EQ(moved.size(), 3u);
		EXPECT_EQ(moved[0], "scripts/lib/z.luau");
		EXPECT_EQ(moved[1], "scripts/lib/a.luau");			  // requires z
		EXPECT_EQ(moved[2], "scripts/sim/rules/sword.luau"); // requires a
		EXPECT_NE(m_library->hash(), before);
		EXPECT_TRUE(update().empty());
	}

	TEST_F(ScriptAssetTest, ABrokenSaveIsReportedAndLeftOutUntilTheNextOne)
	{
		m_library->start(m_assets);

		write("scripts/sim/plain.luau", "system('Act', function(");
		reload("scripts/sim/plain.luau");
		EXPECT_TRUE(update().empty());
		EXPECT_EQ(m_library->all().size(), 3u); // the broken one is not handed out

		write("scripts/sim/plain.luau", "system('Act', function() end)\nreturn 1");
		reload("scripts/sim/plain.luau");
		const Vector<String> moved = paths(update());
		ASSERT_EQ(moved.size(), 1u);
		EXPECT_EQ(moved[0], "scripts/sim/plain.luau");
		EXPECT_EQ(m_library->all().size(), 4u);
	}

	TEST_F(ScriptAssetTest, ANewFileComesThroughTheChanges)
	{
		m_library->start(m_assets);

		write("scripts/sim/fresh.luau", "system('React', function() end)");
		const Vector<String> moved = paths(update({"scripts/sim/fresh.luau", "textures/not_a_script.png"}));
		ASSERT_EQ(moved.size(), 1u);
		EXPECT_EQ(moved[0], "scripts/sim/fresh.luau");
		EXPECT_EQ(m_library->count(), 5u);
		EXPECT_TRUE(update({"scripts/sim/fresh.luau"}).empty()); // known now; nothing moved
	}

	TEST(OrderForLoad, EachAfterWhatItRequiresTiesByPath)
	{
		Heap& heap = memory::heap(MemoryTag::Scripting);
		const auto source = [&](const char* path, std::initializer_list<const char*> imports)
		{
			Source made;
			made.path = String(path, &heap);
			for (const char* import : imports)
				made.imports.push_back({.required = String(import, &heap), .path = String(import, &heap)});
			return made;
		};

		Vector<Source> sources(&heap);
		sources.push_back(source("scripts/sim/c.luau", {"scripts/lib/b.luau"}));
		sources.push_back(source("scripts/lib/b.luau", {"scripts/lib/d.luau"}));
		sources.push_back(source("scripts/lib/d.luau", {}));
		sources.push_back(source("scripts/sim/a.luau", {"scripts/sim/c.luau"}));
		sources.push_back(source("scripts/lib/loop1.luau", {"scripts/lib/loop2.luau"}));
		sources.push_back(source("scripts/lib/loop2.luau", {"scripts/lib/loop1.luau"}));
		order_for_load(sources);

		ASSERT_EQ(sources.size(), 6u);
		EXPECT_EQ(sources[0].path, "scripts/lib/d.luau");
		EXPECT_EQ(sources[1].path, "scripts/lib/b.luau");
		EXPECT_EQ(sources[2].path, "scripts/lib/loop2.luau"); // a cycle: placed in path order, the host reports it
		EXPECT_EQ(sources[3].path, "scripts/lib/loop1.luau");
		EXPECT_EQ(sources[4].path, "scripts/sim/c.luau");
		EXPECT_EQ(sources[5].path, "scripts/sim/a.luau");
	}
}
