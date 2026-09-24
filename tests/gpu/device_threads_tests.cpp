#include <ember/gpu/device.h>
#include <ember/memory/memory.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace
{
	using namespace ember;

	/**
	 * A headless device on whatever adapter exists, validation on so the layers referee the
	 * threading rules. Tests skip where there is no Vulkan at all. Validation counts are process
	 * wide, so every test measures its own delta.
	 */
	class DeviceThreads : public testing::Test
	{
	protected:
		void SetUp() override
		{
			m_errors = gpu::Device::validation_error_count();

			if (!m_device)
				GTEST_SKIP() << "no Vulkan adapter";
		}

		[[nodiscard]] u32 new_errors() const { return gpu::Device::validation_error_count() - m_errors; }

		gpu::Device m_device{gpu::DeviceDef{.adapter = gpu::AdapterPreference::Any}};
		u32 m_errors = 0;
	};

	/// Every thread that touches the engine announces itself to the allocator, like a worker does.
	struct EngineThread
	{
		template <class F>
		explicit EngineThread(F&& body)
			: thread(
				  [fn = std::forward<F>(body)]() mutable
				  {
					  memory::initialize_thread();
					  fn();
					  memory::shutdown_thread();
				  })
		{
		}

		std::thread thread;
	};

	[[nodiscard]] std::vector<u8> checker(u32 size)
	{
		std::vector<u8> pixels(size_t{size} * size * 4);

		for (u32 y = 0; y < size; ++y)
			for (u32 x = 0; x < size; ++x)
			{
				const u8 v = ((x / 4 + y / 4) & 1) != 0 ? 255 : 0;
				u8* texel  = &pixels[(size_t{y} * size + x) * 4];
				texel[0]   = v;
				texel[1]   = static_cast<u8>(255 - v);
				texel[2]   = static_cast<u8>(x);
				texel[3]   = 255;
			}

		return pixels;
	}

	[[nodiscard]] gpu::TextureDef sampled_texture(Span<const u8> pixels, u32 size, bool streamed)
	{
		return {
			.name		  = "test.texture",
			.extent		  = {size, size, 1},
			.format		  = gpu::TextureFormat::RGBA8Unorm,
			.usage		  = gpu::TextureUsage::Sampled,
			.initial_data = pixels,
			.streamed	  = streamed,
		};
	}

	TEST_F(DeviceThreads, WorkersCreateAndDestroyWhileTheOwnerRunsFrames)
	{
		constexpr u32 THREADS = 8;
		constexpr u32 ROUNDS  = 64;
		constexpr u32 KEEP	  = 6; // handles a thread holds before it starts destroying the oldest

		const std::vector<u8> pixels = checker(16);

		std::atomic<u32> finished{0};
		std::atomic<u32> failures{0};
		std::vector<EngineThread> workers;
		workers.reserve(THREADS);

		for (u32 t = 0; t < THREADS; ++t)
		{
			workers.emplace_back(
				[&, t]
				{
					std::vector<TextureHandle> live;

					for (u32 round = 0; round < ROUNDS; ++round)
					{
						// The def says critical on even rounds: off the owner thread the upload
						// streams regardless, which is the rule under test.
						const TextureHandle texture =
							m_device.create_texture(sampled_texture(pixels, 16, (round & 1) != 0));
						const SamplerHandle sampler = m_device.create_sampler({.name = "test.sampler"});

						if (texture.is_null() || sampler.is_null())
							failures.fetch_add(1);

						live.push_back(texture);
						m_device.destroy(sampler);

						if (live.size() > KEEP)
						{
							m_device.destroy(live.front());
							live.erase(live.begin());
						}

						// A stale handle answers false from any thread; a live one true.
						if (!m_device.is_valid(live.back()))
							failures.fetch_add(1);
					}

					for (const TextureHandle texture : live)
						m_device.destroy(texture);

					finished.fetch_add(1, std::memory_order_release);
				});
		}

		// The owner keeps frames going underneath: every begin_frame drains, promotes and polls
		// while the workers insert and retire, and every end_frame submits what they staged.
		const BufferHandle buffer = m_device.create_buffer({
			.name  = "test.owner",
			.size  = 4096,
			.usage = gpu::BufferUsage::Storage,
		});
		ASSERT_FALSE(buffer.is_null());

		for (u32 frame = 0; finished.load(std::memory_order_acquire) < THREADS || frame < 4; ++frame)
		{
			(void)m_device.begin_frame();

			const u32 words[4] = {frame, frame * 2, frame * 3, frame * 4};
			m_device.update_buffer(buffer, 0, {reinterpret_cast<const u8*>(words), sizeof(words)});

			(void)m_device.end_frame();
		}

		for (EngineThread& worker : workers)
			worker.thread.join();

		m_device.destroy(buffer);
		m_device.wait_idle();

		EXPECT_EQ(failures.load(), 0u);
		EXPECT_EQ(new_errors(), 0u);
	}

	TEST_F(DeviceThreads, StreamedTextureBecomesResidentOnceItsUploadLands)
	{
		const std::vector<u8> pixels = checker(32);
		TextureHandle texture;

		{
			EngineThread loader([&] { texture = m_device.create_texture(sampled_texture(pixels, 32, false)); });
			loader.thread.join();
		}

		ASSERT_FALSE(texture.is_null());

		// Recorded into an open streamed batch that no frame has submitted: not resident, and
		// asking from this thread is fine.
		EXPECT_FALSE(m_device.is_resident(texture));

		// The first end_frame submits the batch; the wait proves it landed, and the next
		// begin_frame promotes the descriptor. wait_idle alone must do as much.
		(void)m_device.begin_frame();
		(void)m_device.end_frame();
		m_device.wait_idle();

		EXPECT_TRUE(m_device.is_resident(texture));

		(void)m_device.begin_frame();
		(void)m_device.end_frame();

		m_device.destroy(texture);
		m_device.wait_idle();

		EXPECT_EQ(new_errors(), 0u);
	}

	TEST_F(DeviceThreads, DeviceLocalInitialDataOffTheOwnerThreadIsRefused)
	{
		static constexpr u8 BYTES[64] = {};
		BufferHandle buffer;

		{
			EngineThread worker(
				[&]
				{
					buffer = m_device.create_buffer({
						.name		  = "test.refused",
						.size		  = sizeof(BYTES),
						.usage		  = gpu::BufferUsage::Storage,
						.initial_data = Span<const u8>{BYTES, sizeof(BYTES)},
					});
				});
			worker.thread.join();
		}

		EXPECT_TRUE(buffer.is_null());
		EXPECT_EQ(new_errors(), 0u);
	}
}
