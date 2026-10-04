#include <ember/assets/asset.h>
#include <ember/assets/texture_asset.h>
#include <ember/core/filesystem.h>
#include <ember/gpu/device.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if defined(EMBER_PLATFORM_WINDOWS)
	#include <process.h>
#else
	#include <unistd.h>
#endif

namespace
{
	using namespace ember;

	[[nodiscard]] int process_id() noexcept
	{
#if defined(EMBER_PLATFORM_WINDOWS)
		return _getpid();
#else
		return getpid();
#endif
	}

	/// The simplest payload: the file's own bytes, kept. An empty file is a failed load, which is
	/// how the tests reach the failure paths without a broken decoder. No reload() of its own, so
	/// a reload swaps it: the old bytes end up in the fresh payload and are unloaded from there.
	struct BytesAsset
	{
		fs::FileData bytes;

		inline static std::atomic<u32> unloads{0};

		static bool load(AssetLoad& load, BytesAsset& out) noexcept
		{
			if (load.bytes().empty())
				return false;

			out.bytes = load.take_bytes();
			return true;
		}

		/// The bytes free themselves with the payload; only the count is left to do.
		static void unload(AssetServices&, BytesAsset&) noexcept { unloads.fetch_add(1); }
	};

	static_assert(AssetType<BytesAsset>);

	[[nodiscard]] StringView text_of(Span<const u8> bytes) noexcept
	{
		return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
	}

	/// What an owner-thread system is to these tests: a registry only the owner thread may change,
	/// which is why the assets that fill it publish there. It keeps a log of its hooks.
	struct Registry
	{
		std::vector<std::string> log;
		bool off_thread = false; // a hook ran somewhere other than the owner thread
	};

	/// A payload a registry finishes. Its file is a line of text, then optionally "part: <path>",
	/// another of its kind it is made of: it publishes once that part has settled, loaded or
	/// failed, and binds the part's text whenever it hears the part has changed.
	struct PartAsset
	{
		std::string text;
		AssetRef<PartAsset> part;
		std::string bound; // the part's text when last bound

		static bool load(AssetLoad& load, PartAsset& out) noexcept
		{
			const StringView file = text_of(load.bytes());
			const size_t line	  = file.find('\n');

			out.text = std::string(file.substr(0, line));

			if (line != StringView::npos && file.substr(line + 1).starts_with("part: "))
				out.part = load.load<PartAsset>(file.substr(line + 7));

			return !out.text.empty();
		}

		static bool publish(AssetServices& services, PartAsset& asset) noexcept
		{
			if (asset.part.state() == AssetState::Loading && !asset.part.is_null())
				return false;

			log(services, "publish " + asset.text);
			bind(asset);
			return true;
		}

		static void refresh(AssetServices& services, PartAsset& asset) noexcept
		{
			log(services, "refresh " + asset.text);
			bind(asset);
		}

		static void reload(AssetServices& services, PartAsset& live, PartAsset& fresh) noexcept
		{
			log(services, "reload " + fresh.text);
			std::swap(live.text, fresh.text);
			std::swap(live.part, fresh.part);
			bind(live);
		}

		static void unload(AssetServices& services, PartAsset& asset) noexcept
		{
			log(services, "unload " + asset.text);
		}

		static void bind(PartAsset& asset) noexcept { asset.bound = asset.part ? asset.part->text : std::string(); }

		static void log(AssetServices& services, std::string entry) noexcept
		{
			Registry& registry = services.context<Registry>();
			registry.off_thread |= !jobs::is_main();
			registry.log.push_back(std::move(entry));
		}
	};

	static_assert(AssetType<PartAsset> && HasAssetPublish<PartAsset> && HasAssetRefresh<PartAsset>);

	/// A payload made from files that are no assets: its file lists them, a line each, as names or
	/// absolute paths. It counts its loads, so a test sees a change to one of them reload it, and a
	/// line of "!" fails the load after the files before it were named, as a compile error would.
	struct ListAsset
	{
		inline static std::atomic<u32> loads{0};

		static bool load(AssetLoad& load, ListAsset&) noexcept
		{
			loads.fetch_add(1);
			StringView file = text_of(load.bytes());

			while (!file.empty())
			{
				const size_t line	   = std::min(file.find('\n'), file.size());
				const StringView entry = file.substr(0, line);

				if (entry == "!")
					return false;

				load.depends_on(entry);
				file.remove_prefix(std::min(line + 1, file.size()));
			}

			return true;
		}

		static void unload(AssetServices&, ListAsset&) noexcept {}
	};

	/// A payload whose loader reads for itself: the manager names the file and reads nothing.
	struct SelfReadAsset
	{
		static constexpr bool READS_OWN_FILES = true;

		std::string file;
		size_t bytes = 0;

		static bool load(AssetLoad& load, SelfReadAsset& out) noexcept
		{
			out.file  = std::string(load.file());
			out.bytes = load.bytes().size();
			return true;
		}

		static void unload(AssetServices&, SelfReadAsset&) noexcept {}
	};

	static_assert(ReadsOwnFiles<SelfReadAsset> && !ReadsOwnFiles<BytesAsset>);

	/// A game's own type, which it registers in its init(): after the engine's loads have begun.
	struct NoteAsset
	{
		std::string text;

		static bool load(AssetLoad& load, NoteAsset& out) noexcept
		{
			out.text = std::string(text_of(load.bytes()));
			return !out.text.empty();
		}

		static void unload(AssetServices&, NoteAsset&) noexcept {}
	};

	/// A manager over a scratch directory of this process's own, with the job system's IO thread
	/// and a headless device under it. The watcher is off here so the tests drive notify_changed()
	/// by hand and stay deterministic.
	class AssetManagerTest : public testing::Test
	{
	protected:
		virtual AssetManagerDef def() const
		{
			return {.root = m_root.c_str(), .max_assets = 64, .hot_reload = false, .reload_delay_ms = 0};
		}

		void SetUp() override
		{
			String scratch(&memory::heap(MemoryTag::Engine));
			ASSERT_TRUE(fs::temporary_directory(scratch).has_value());
			ASSERT_TRUE(fs::join(scratch, scratch, "ember_asset_tests_" + std::to_string(process_id())).has_value());
			m_root.assign(scratch.data(), scratch.size());

			// A run that crashed leaves its directory behind; a clean one leaves none to remove.
			if (const auto removed = fs::remove_tree(m_root); !removed)
			{
				ASSERT_EQ(removed.error().code, fs::FileErrorCode::NotFound);
			}

			ASSERT_TRUE(fs::create_directories(m_root + "/sub").has_value());

			jobs::initialize({.worker_count = 4});
			m_assets.init(m_device, def());
			m_assets.register_type<BytesAsset>("bytes");
			m_assets.register_type<PartAsset>("part", &m_registry);
			m_assets.register_type<ListAsset>("list");
			m_assets.register_type<SelfReadAsset>("self read");

			BytesAsset::unloads = 0;
			ListAsset::loads	= 0;
		}

		void TearDown() override
		{
			m_assets.shutdown();
			jobs::shutdown();
			(void)fs::remove_tree(m_root);
		}

		/// `size` bytes of a pattern keyed by `seed`; the asset path is the name.
		void write(const char* name, size_t size, u8 seed) { write_to(m_root + "/" + name, size, seed); }

		/// The same pattern at a path of its own, outside the root.
		static void write_to(const std::string& path, size_t size, u8 seed)
		{
			std::vector<u8> bytes(size);
			for (size_t i = 0; i < size; ++i)
				bytes[i] = static_cast<u8>(seed + i);

			ASSERT_TRUE(fs::write_file(path, Span<const u8>(bytes)).has_value());
		}

		void write_bytes(const char* name, Span<const u8> bytes)
		{
			ASSERT_TRUE(fs::write_file(m_root + "/" + name, bytes).has_value());
		}

		static bool matches(const AssetRef<BytesAsset>& ref, size_t size, u8 seed)
		{
			if (!ref || ref->bytes.size() != size)
				return false;

			const Span<const u8> bytes = ref->bytes.bytes();

			for (size_t i = 0; i < size; ++i)
				if (bytes[i] != static_cast<u8>(seed + i))
					return false;

			return true;
		}

		/// The frame loop's view of a reload: a change, the pump that queues it, the loader, and
		/// the pump that folds the result in.
		void reload_now(const char* name)
		{
			m_assets.notify_changed(asset_id(name));
			m_assets.pump(++m_frame);
			m_assets.wait_idle();
			m_assets.pump(++m_frame);
		}

		void pump() { m_assets.pump(++m_frame); }

		void write_text(const char* name, std::string_view text)
		{
			write_bytes(name, {reinterpret_cast<const u8*>(text.data()), text.size()});
		}

		/// Waits for the loaders from a job, which, unlike the owner thread, publishes nothing.
		void wait_for_loaders()
		{
			auto idle = [this]() noexcept { m_assets.wait_idle(); };
			jobs::Batch batch(jobs::make_job(idle, "wait for loaders"));
		}

		Registry m_registry;
		std::string m_root;
		u64 m_frame = 0;

		gpu::Device m_device{gpu::DeviceDef{.adapter = gpu::AdapterPreference::Any}};
		AssetManager m_assets;
	};

	TEST_F(AssetManagerTest, LoadThenWaitGivesThePayload)
	{
		write("a.bin", 500, 1);

		const AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("a.bin");
		ASSERT_FALSE(ref.is_null());

		ref.wait();

		EXPECT_TRUE(ref);
		EXPECT_TRUE(matches(ref, 500, 1));
		EXPECT_EQ(ref.state(), AssetState::Loaded);
	}

	TEST_F(AssetManagerTest, TheSamePathIsTheSameAsset)
	{
		write("same.bin", 16, 2);

		const AssetRef<BytesAsset> first  = m_assets.load<BytesAsset>("same.bin");
		const AssetRef<BytesAsset> second = m_assets.load<BytesAsset>("same.bin");

		first.wait();

		EXPECT_EQ(first.get(), second.get());
		EXPECT_EQ(m_assets.stats().assets, 1u);
	}

	TEST_F(AssetManagerTest, AMissingFileFailsAndWaitStillReturns)
	{
		const AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("nope.bin");
		ref.wait();

		EXPECT_FALSE(ref);
		EXPECT_EQ(ref.state(), AssetState::Failed);
	}

	TEST_F(AssetManagerTest, ALoaderThatReturnsFalseFails)
	{
		write("empty.bin", 0, 0);

		const AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("empty.bin");
		ref.wait();

		EXPECT_FALSE(ref);
		EXPECT_EQ(m_assets.stats().failed, 1u);
	}

	TEST_F(AssetManagerTest, AReloadChangesThePayloadInPlaceAndUnloadsTheOldContentsLater)
	{
		write("hot.bin", 300, 1);

		const AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("hot.bin");
		ref.wait();
		ASSERT_TRUE(matches(ref, 300, 1));

		const BytesAsset* payload = ref.get();

		write("hot.bin", 200, 9);
		reload_now("hot.bin");

		// Same payload object, new contents, nothing for the holder to do.
		EXPECT_EQ(ref.get(), payload);
		EXPECT_TRUE(matches(ref, 200, 9));
		EXPECT_EQ(ref.state(), AssetState::Loaded);

		// The old bytes wait out the grace, for a pointer a frame may have copied.
		EXPECT_EQ(BytesAsset::unloads.load(), 0u);
		pump();
		EXPECT_EQ(BytesAsset::unloads.load(), 1u);
		EXPECT_TRUE(matches(ref, 200, 9));
	}

	TEST_F(AssetManagerTest, AFailedReloadKeepsTheOldPayload)
	{
		write("keep.bin", 64, 3);

		const AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("keep.bin");
		ref.wait();

		write("keep.bin", 0, 0); // a broken save
		reload_now("keep.bin");

		EXPECT_TRUE(matches(ref, 64, 3));
		EXPECT_EQ(ref.state(), AssetState::Loaded);
	}

	TEST_F(AssetManagerTest, AFailedFirstLoadRecoversOnReload)
	{
		write("late.bin", 0, 0);

		const AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("late.bin");
		ref.wait();
		ASSERT_FALSE(ref);

		write("late.bin", 24, 4);
		reload_now("late.bin");

		EXPECT_TRUE(matches(ref, 24, 4));
		EXPECT_EQ(ref.state(), AssetState::Loaded);
	}

	TEST_F(AssetManagerTest, DroppingTheLastReferenceUnloadsTheAssetAfterTheGrace)
	{
		write("gone.bin", 32, 4);

		AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("gone.bin");
		ref.wait();

		AssetRef<BytesAsset> copy = ref;
		ref.reset();
		pump();
		EXPECT_EQ(m_assets.stats().assets, 1u); // the copy holds it

		copy.reset();
		pump(); // the slot goes; the payload waits out the grace
		EXPECT_EQ(m_assets.stats().assets, 0u);
		EXPECT_EQ(BytesAsset::unloads.load(), 0u);
		pump();
		EXPECT_EQ(BytesAsset::unloads.load(), 1u);

		// Loading it again is a fresh asset.
		const AssetRef<BytesAsset> again = m_assets.load<BytesAsset>("gone.bin");
		again.wait();
		EXPECT_TRUE(matches(again, 32, 4));
	}

	TEST_F(AssetManagerTest, LoadingAgainBeforeThePumpRevivesTheSameAsset)
	{
		write("revive.bin", 12, 6);

		AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("revive.bin");
		ref.wait();
		const BytesAsset* payload = ref.get();

		ref.reset();
		ref = m_assets.load<BytesAsset>("revive.bin"); // before any pump: the slot is still there

		pump();
		pump();
		pump();

		EXPECT_EQ(ref.get(), payload);
		EXPECT_EQ(m_assets.stats().assets, 1u);
		EXPECT_EQ(BytesAsset::unloads.load(), 0u);
	}

	TEST_F(AssetManagerTest, DroppingWhileTheLoadIsInFlightLeavesTheSlotToTheLoader)
	{
		write("race.bin", 4096, 5);

		// Dropped before the loader can possibly have finished: whichever side gets there first,
		// the slot ends up freed and nothing leaks (the suite's leak check covers the rest).
		AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("race.bin");
		ref.reset();

		pump();
		m_assets.wait_idle();
		pump();
		pump();
		pump();

		EXPECT_EQ(m_assets.stats().assets, 0u);
		EXPECT_LE(BytesAsset::unloads.load(), 1u);
	}

	TEST_F(AssetManagerTest, AChangeToAnUnknownFileIsIgnored)
	{
		m_assets.notify_changed(asset_id("nothing/here.bin"));
		pump();
		m_assets.wait_idle();

		EXPECT_EQ(m_assets.stats().assets, 0u);
	}

	TEST_F(AssetManagerTest, APathThatLeavesTheRootIsRefused)
	{
		const AssetRef<BytesAsset> outside = m_assets.load<BytesAsset>("../escape.bin");
		EXPECT_TRUE(outside.is_null());

		const AssetRef<BytesAsset> absolute = m_assets.load<BytesAsset>("/etc/hostname");
		EXPECT_TRUE(absolute.is_null());

		EXPECT_EQ(m_assets.stats().assets, 0u);
	}

	TEST_F(AssetManagerTest, ManyAssetsLoadAtOnceAndWaitIdleCoversThemAll)
	{
		constexpr u32 COUNT = 48;

		std::vector<AssetRef<BytesAsset>> refs;

		for (u32 i = 0; i < COUNT; ++i)
		{
			const std::string name = "many" + std::to_string(i) + ".bin";
			write(name.c_str(), 100 + i, static_cast<u8>(i));
			refs.push_back(m_assets.load<BytesAsset>(name));
		}

		m_assets.wait_idle();

		for (u32 i = 0; i < COUNT; ++i)
			EXPECT_TRUE(matches(refs[i], 100 + i, static_cast<u8>(i))) << "asset " << i;

		const AssetManager::Stats stats = m_assets.stats();
		EXPECT_EQ(stats.assets, COUNT);
		EXPECT_EQ(stats.loading, 0u);
		EXPECT_EQ(stats.failed, 0u);
	}

	TEST_F(AssetManagerTest, ATypeRegisteredWhileLoadsRunLoadsAndLeavesThemBe)
	{
		// Loads in flight, as the material library's are by the time an app's init() runs.
		constexpr u32 COUNT = 32;

		std::vector<AssetRef<BytesAsset>> early;

		for (u32 i = 0; i < COUNT; ++i)
		{
			const std::string name = "early" + std::to_string(i) + ".bin";
			write(name.c_str(), 1000 + i, static_cast<u8>(i));
			early.push_back(m_assets.load<BytesAsset>(name));
		}

		m_assets.register_type<NoteAsset>("note");
		write_text("a.note", "registered late");
		const AssetRef<NoteAsset> note = m_assets.load<NoteAsset>("a.note");

		m_assets.wait_idle();

		ASSERT_TRUE(note);
		EXPECT_EQ(note->text, "registered late");

		for (u32 i = 0; i < COUNT; ++i)
			EXPECT_TRUE(matches(early[i], 1000 + i, static_cast<u8>(i))) << "asset " << i;
	}

	TEST_F(AssetManagerTest, ATypeThatPublishesOnTheOwnerThreadIsSeenOnlyOnceThePumpHasPublishedIt)
	{
		write_text("a.part", "alpha");

		const AssetRef<PartAsset> ref = m_assets.load<PartAsset>("a.part");
		wait_for_loaders();

		// Loaded, but not yet published: nobody sees a payload the registry has not taken.
		EXPECT_FALSE(ref);
		EXPECT_EQ(ref.state(), AssetState::Loading);
		EXPECT_TRUE(m_registry.log.empty());

		pump();

		ASSERT_TRUE(ref);
		EXPECT_EQ(ref->text, "alpha");
		EXPECT_EQ(m_registry.log, (std::vector<std::string>{"publish alpha"}));
		EXPECT_FALSE(m_registry.off_thread);
	}

	TEST_F(AssetManagerTest, TheOwnerThreadWaitingForOnePublishesItItself)
	{
		write_text("b.part", "bravo");

		const AssetRef<PartAsset> ref = m_assets.load<PartAsset>("b.part");
		ref.wait(); // no pump runs while this thread waits: the wait publishes

		ASSERT_TRUE(ref);
		EXPECT_EQ(ref.state(), AssetState::Loaded);
		EXPECT_EQ(ref->text, "bravo");
	}

	TEST_F(AssetManagerTest, AnAssetPublishesOnceWhatItLoadedHasSettled)
	{
		write_text("whole.part", "whole\npart: piece.part");
		write_text("piece.part", "piece");
		write_text("broken.part", "broken\npart: missing.part");

		const AssetRef<PartAsset> whole	 = m_assets.load<PartAsset>("whole.part");
		const AssetRef<PartAsset> broken = m_assets.load<PartAsset>("broken.part");
		m_assets.wait_idle();

		// Whichever loader finished first, the part published before the whole that waits on it,
		// and a part that failed let its whole publish without it.
		ASSERT_TRUE(whole);
		ASSERT_TRUE(broken);
		EXPECT_EQ(whole->bound, "piece");
		EXPECT_EQ(broken->bound, "");
		EXPECT_EQ(broken->part.state(), AssetState::Failed);

		const auto order = [&](const char* entry)
		{ return std::find(m_registry.log.begin(), m_registry.log.end(), entry) - m_registry.log.begin(); };

		EXPECT_LT(order("publish piece"), order("publish whole"));
	}

	TEST_F(AssetManagerTest, APayloadHearsWhenWhatItLoadedArrivesOrReloads)
	{
		write_text("late.part", "late\npart: slow.part");

		// The part is broken at first, so the whole publishes without it.
		const AssetRef<PartAsset> whole = m_assets.load<PartAsset>("late.part");
		whole.wait();
		ASSERT_TRUE(whole);
		EXPECT_EQ(whole->bound, "");

		// Then the part is written and arrives: the whole binds it at the pump that publishes it.
		write_text("slow.part", "slow");
		reload_now("slow.part");
		EXPECT_EQ(whole->bound, "slow");

		// And when the part reloads, the whole binds what it says now.
		write_text("slow.part", "slower");
		reload_now("slow.part");
		EXPECT_EQ(whole->bound, "slower");
		EXPECT_FALSE(m_registry.off_thread);
	}

	TEST_F(AssetManagerTest, AChangeToAFileAnAssetDependsOnReloadsTheAsset)
	{
		write_text("sub/common.inc", "shared");
		write_text("sub/other.inc", "other");
		write_text("one.list", "sub/common.inc");
		write_text("two.list", m_root + "/sub/common.inc\nsub/other.inc"); // an absolute path names it too

		const AssetRef<ListAsset> one = m_assets.load<ListAsset>("one.list");
		const AssetRef<ListAsset> two = m_assets.load<ListAsset>("two.list");
		m_assets.wait_idle();
		ASSERT_EQ(ListAsset::loads.load(), 2u);

		reload_now("sub/common.inc");
		EXPECT_EQ(ListAsset::loads.load(), 4u); // both were made from it

		reload_now("sub/other.inc");
		EXPECT_EQ(ListAsset::loads.load(), 5u); // only the second

		// A reload names its dependencies anew: one that is gone no longer reloads anything.
		write_text("two.list", "sub/other.inc");
		reload_now("two.list");
		ASSERT_EQ(ListAsset::loads.load(), 6u);

		reload_now("sub/common.inc");
		EXPECT_EQ(ListAsset::loads.load(), 7u);
	}

	TEST_F(AssetManagerTest, AFailedLoadAddsToWhatTheLastGoodOneDependedOn)
	{
		write_text("sub/a.inc", "a");
		write_text("sub/b.inc", "b");
		write_text("one.list", "sub/a.inc");

		const AssetRef<ListAsset> one = m_assets.load<ListAsset>("one.list");
		one.wait();
		ASSERT_EQ(ListAsset::loads.load(), 1u);

		// A broken save that got as far as naming another file: both are watched now, since the
		// fix may be made in either.
		write_text("one.list", "sub/b.inc\n!");
		reload_now("one.list");
		ASSERT_EQ(ListAsset::loads.load(), 2u);

		reload_now("sub/a.inc");
		EXPECT_EQ(ListAsset::loads.load(), 3u);

		reload_now("sub/b.inc");
		EXPECT_EQ(ListAsset::loads.load(), 4u);
	}

	TEST_F(AssetManagerTest, ATypeThatReadsItsOwnFilesIsGivenTheFileAndNoBytes)
	{
		// Nothing on disk: the manager never reads it, so it cannot fail to.
		const AssetRef<SelfReadAsset> ref = m_assets.load<SelfReadAsset>("sub/../own/thing.src");
		ref.wait();

		ASSERT_TRUE(ref);
		EXPECT_EQ(ref->file, m_root + "/own/thing.src");
		EXPECT_EQ(ref->bytes, 0u);
	}

	TEST_F(AssetManagerTest, AMountServesADirectoryOutsideTheRootUnderItsPrefix)
	{
		const std::string engine = m_root + "_engine";
		ASSERT_TRUE(fs::create_directories(engine + "/shaders").has_value());
		ASSERT_TRUE(fs::write_file(engine + "/shaders/core.bin", Span<const u8>(std::vector<u8>{7, 8, 9})).has_value());

		m_assets.mount("engine", engine);

		const AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("engine/shaders/core.bin");
		ref.wait();
		EXPECT_TRUE(matches(ref, 3, 7));

		// A file under a mount is named with its prefix, as a dependency and as a change.
		write_text("uses.list", engine + "/shaders/core.bin");
		const AssetRef<ListAsset> list = m_assets.load<ListAsset>("uses.list");
		m_assets.wait_idle();
		ASSERT_EQ(ListAsset::loads.load(), 1u);

		reload_now("engine/shaders/core.bin");
		EXPECT_EQ(ListAsset::loads.load(), 2u);

		(void)fs::remove_tree(engine);
	}

	TEST_F(AssetManagerTest, ShutdownUnloadsAnAssetBeforeTheAssetsItHolds)
	{
		write_text("outer.part", "outer\npart: inner.part");
		write_text("inner.part", "inner");

		AssetRef<PartAsset> outer = m_assets.load<PartAsset>("outer.part");
		outer.wait();
		ASSERT_TRUE(outer);
		outer.reset();

		// Only the outer payload holds the inner asset now; it has to go first for the inner to.
		m_assets.shutdown();

		const auto order = [&](const char* entry)
		{ return std::find(m_registry.log.begin(), m_registry.log.end(), entry) - m_registry.log.begin(); };

		ASSERT_LT(order("unload inner"), static_cast<ptrdiff_t>(m_registry.log.size()));
		EXPECT_LT(order("unload outer"), order("unload inner"));
	}

	/// A 4 by 4 RGBA PNG, so the engine's own texture type is exercised end to end.
	constexpr u8 SMALL_PNG[] = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00,
		0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x06, 0x00, 0x00, 0x00, 0xa9, 0xf1, 0x9e, 0x7e, 0x00,
		0x00, 0x00, 0x2b, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x15, 0xc8, 0x31, 0x01, 0x00, 0x30, 0x0c, 0xc3,
		0xb0, 0x00, 0x2b, 0x30, 0x9f, 0x05, 0x15, 0x7e, 0x9b, 0x7b, 0xe8, 0x51, 0x92, 0x7d, 0x23, 0x54, 0x25,
		0x63, 0x08, 0x75, 0x2e, 0x30, 0x84, 0xca, 0x45, 0x0d, 0xa1, 0xea, 0x03, 0x39, 0xc8, 0x23, 0x31, 0x35,
		0xad, 0xbf, 0x59, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
	};

	/// An 8 by 8 one, so a reload changes the extent as well as the pixels.
	constexpr u8 BIG_PNG[] = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00,
		0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0xc4, 0x0f, 0xbe, 0x8b, 0x00,
		0x00, 0x00, 0x5c, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x15, 0xca, 0x31, 0x01, 0x03, 0x41, 0x08, 0x00,
		0xb0, 0x93, 0xf2, 0x52, 0x90, 0x82, 0x14, 0xa4, 0x20, 0x05, 0x29, 0x38, 0x69, 0xc3, 0x90, 0x2d, 0xef,
		0xbd, 0xfa, 0x7d, 0x04, 0x49, 0xd1, 0x0c, 0xcb, 0x7b, 0x9f, 0x40, 0x90, 0x14, 0xcd, 0xb0, 0xdf, 0x85,
		0x10, 0x08, 0x92, 0xa2, 0x19, 0x36, 0x2e, 0xa4, 0x40, 0x90, 0x14, 0xcd, 0xb0, 0x79, 0xa1, 0x04, 0x82,
		0xa4, 0x68, 0x86, 0xad, 0x0b, 0x2d, 0x10, 0x24, 0x45, 0x33, 0x6c, 0x5f, 0x18, 0x81, 0x20, 0x29, 0x9a,
		0x61, 0xe7, 0xc2, 0x0a, 0x04, 0x49, 0xd1, 0x0c, 0xcb, 0x1f, 0xfa, 0x91, 0x97, 0xc1, 0x4d, 0x43, 0xd8,
		0x85, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
	};

	TEST_F(AssetManagerTest, ATextureReloadKeepsItsHandleAndTakesTheNewPixels)
	{
		if (!m_device)
			GTEST_SKIP() << "no Vulkan adapter";

		write_bytes("art.png", SMALL_PNG);

		const AssetRef<TextureAsset> texture = m_assets.load<TextureAsset>("art.png");
		texture.wait();
		ASSERT_TRUE(texture);

		const TextureHandle handle = texture->texture;
		EXPECT_EQ(texture->extent.width, 4u);
		EXPECT_TRUE(m_device.is_valid(handle));

		// Streamed: the pixels land with the next upload submit, and wait_idle proves it.
		(void)m_device.begin_frame();
		(void)m_device.end_frame();
		m_device.wait_idle();
		EXPECT_TRUE(m_device.is_resident(handle));

		// The save: a bigger image behind the same handle. The fold happens at the second pump,
		// the swap on the device once the new upload has landed.
		write_bytes("art.png", BIG_PNG);
		reload_now("art.png");

		EXPECT_EQ(texture->texture, handle);
		EXPECT_EQ(texture->extent.width, 8u);
		EXPECT_EQ(texture->extent.height, 8u);
		EXPECT_FALSE(m_device.is_resident(handle)); // the new pixels are still on their way

		(void)m_device.begin_frame();
		(void)m_device.end_frame();
		m_device.wait_idle();
		(void)m_device.begin_frame(); // promotion swaps the image behind the handle
		(void)m_device.end_frame();

		EXPECT_TRUE(m_device.is_valid(handle));
		EXPECT_TRUE(m_device.is_resident(handle));

		// The fresh payload carried no handle, so its unload after the grace destroys nothing.
		pump();
		pump();
		pump();
		m_device.wait_idle();
		EXPECT_TRUE(m_device.is_valid(handle));
	}

	/// The same manager with the watcher on and a short quiet time, so a save on disk is the
	/// only thing that tells it to reload.
	class WatchedAssetManagerTest : public AssetManagerTest
	{
	protected:
		AssetManagerDef def() const override
		{
			return {.root = m_root.c_str(), .max_assets = 64, .hot_reload = true, .reload_delay_ms = 50};
		}

		/// Pumps the way the frame loop would, once a frame, until the payload reads as expected
		/// or a few seconds pass.
		bool pump_until(const AssetRef<BytesAsset>& ref, size_t size, u8 seed)
		{
			for (u32 frame = 0; frame < 300; ++frame)
			{
				pump();

				if (matches(ref, size, seed))
					return true;

				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}

			return false;
		}
	};

	TEST_F(WatchedAssetManagerTest, ASaveOnDiskReloadsWithoutAnyoneBeingTold)
	{
		write("live.bin", 100, 1);

		const AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("live.bin");
		ref.wait();
		ASSERT_TRUE(matches(ref, 100, 1));

		write("live.bin", 120, 2); // the save

		EXPECT_TRUE(pump_until(ref, 120, 2));
		m_assets.wait_idle();
	}

	TEST_F(WatchedAssetManagerTest, AFileInASubdirectoryIsNamedTheWayItWasLoaded)
	{
		write("sub/deep.bin", 40, 3);

		const AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("sub/deep.bin");
		ref.wait();
		ASSERT_TRUE(matches(ref, 40, 3));

		write("sub/deep.bin", 44, 7);

		EXPECT_TRUE(pump_until(ref, 44, 7));
		m_assets.wait_idle();
	}

	TEST_F(WatchedAssetManagerTest, ABurstOfWritesReloadsOnceTheFileGoesQuiet)
	{
		write("burst.bin", 10, 1);

		const AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("burst.bin");
		ref.wait();

		// An editor saving in steps: several writes inside the quiet time. The last one is the
		// one that counts, and nothing reloads while the burst is still going.
		for (u8 step = 2; step <= 6; ++step)
		{
			write("burst.bin", 10 + step, step);
			pump();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}

		EXPECT_TRUE(matches(ref, 10, 1));

		EXPECT_TRUE(pump_until(ref, 16, 6));
		m_assets.wait_idle();
	}

	TEST_F(WatchedAssetManagerTest, ASaveUnderAMountReloadsTheAssetNamedWithItsPrefix)
	{
		const std::string engine = m_root + "_engine";
		ASSERT_TRUE(fs::create_directories(engine + "/deep").has_value());
		write_to(engine + "/deep/live.bin", 20, 1);

		m_assets.mount("engine", engine);

		const AssetRef<BytesAsset> ref = m_assets.load<BytesAsset>("engine/deep/live.bin");
		ref.wait();
		ASSERT_TRUE(matches(ref, 20, 1));

		write_to(engine + "/deep/live.bin", 30, 2);

		EXPECT_TRUE(pump_until(ref, 30, 2));
		m_assets.wait_idle();
		(void)fs::remove_tree(engine);
	}
}
