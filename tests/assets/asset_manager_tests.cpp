#include <ember/assets/asset.h>
#include <ember/assets/texture_asset.h>
#include <ember/core/filesystem.h>
#include <ember/gpu/device.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>

#include <gtest/gtest.h>

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

			ASSERT_TRUE(fs::remove_tree(m_root).has_value());
			ASSERT_TRUE(fs::create_directories(m_root + "/sub").has_value());

			jobs::initialize({.worker_count = 4});
			m_assets.init(m_device, def());
			m_assets.register_type<BytesAsset>("bytes");

			BytesAsset::unloads = 0;
		}

		void TearDown() override
		{
			m_assets.shutdown();
			jobs::shutdown();
			(void)fs::remove_tree(m_root);
		}

		/// `size` bytes of a pattern keyed by `seed`; the asset path is the name.
		void write(const char* name, size_t size, u8 seed)
		{
			std::vector<u8> bytes(size);
			for (size_t i = 0; i < size; ++i)
				bytes[i] = static_cast<u8>(seed + i);

			write_bytes(name, Span<const u8>(bytes));
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
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
		0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0xc4, 0x0f, 0xbe, 0x8b, 0x00, 0x00, 0x00,
		0x6e, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0x0f, 0x04, 0x0c, 0x0c, 0x0c, 0xf6, 0x8d, 0x0d, 0xff,
		0x19, 0xd8, 0xf7, 0xff, 0x9e, 0x1f, 0xf7, 0xff, 0xef, 0xff, 0xff, 0x0c, 0x0c, 0x4c, 0x0c, 0x2c, 0x0c, 0xac,
		0x0c, 0xec, 0x0c, 0x9c, 0x0c, 0x5c, 0x0c, 0xdc, 0x0c, 0x3c, 0x0c, 0xbc, 0x0c, 0x7c, 0x0c, 0xfc, 0x0c, 0x02,
		0x0c, 0x82, 0x0c, 0x42, 0x0c, 0xc2, 0x0c, 0x22, 0x0c, 0xa2, 0x0c, 0x62, 0x0c, 0xe2, 0x0c, 0x12, 0x0c, 0x92,
		0x0c, 0x52, 0x0c, 0xd2, 0x0c, 0x32, 0x0c, 0xb2, 0x0c, 0x72, 0x0c, 0xf2, 0x0c, 0x0a, 0x0c, 0x8a, 0x0c, 0x4a,
		0x0c, 0xca, 0x0c, 0x2a, 0x0c, 0xaa, 0x0c, 0x6a, 0x0c, 0xea, 0x0c, 0x1a, 0x0c, 0x9a, 0x0c, 0x5a, 0x0c, 0xda,
		0x0c, 0x3a, 0x0c, 0xba, 0x0c, 0x7a, 0x0c, 0xfa, 0x0c, 0x06, 0x00, 0x8c, 0x8b, 0x22, 0x8e, 0x00, 0x00, 0x00,
		0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
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
}
