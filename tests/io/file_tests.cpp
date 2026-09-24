#include <ember/io/file.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
	using namespace ember;
	using namespace ember::io;

	/// A scratch directory of files with known contents, gone when the test is.
	class FileIoTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			m_dir = std::filesystem::temp_directory_path() / "ember_io_tests";
			std::filesystem::create_directories(m_dir);
			jobs::initialize({.worker_count = 4});
		}

		void TearDown() override
		{
			jobs::shutdown();
			std::filesystem::remove_all(m_dir);
		}

		/// Writes `size` bytes of a pattern keyed by `seed` and returns the path.
		std::string write(const char* name, size_t size, u8 seed)
		{
			const std::string path = (m_dir / name).string();

			std::FILE* file = std::fopen(path.c_str(), "wb");
			EXPECT_NE(file, nullptr);

			for (size_t i = 0; i < size; ++i)
			{
				const u8 byte = static_cast<u8>(seed + i);
				std::fwrite(&byte, 1, 1, file);
			}

			std::fclose(file);
			return path;
		}

		static bool matches(Span<const u8> bytes, size_t size, u8 seed)
		{
			if (bytes.size() != size)
				return false;

			for (size_t i = 0; i < size; ++i)
				if (bytes[i] != static_cast<u8>(seed + i))
					return false;

			return true;
		}

		/// The whole protocol from a job's point of view: submit, park, use.
		static FileRead read_now(FileIo& io, const char* path)
		{
			FileRead read{.path = path, .memory = &memory::heap(MemoryTag::Assets)};
			jobs::Counter done{1};

			io.read(read, done);
			jobs::wait(done);

			return read;
		}

		std::filesystem::path m_dir;
	};

	TEST_F(FileIoTest, AJobReadsAFileAndResumesWithItsBytes)
	{
		const std::string path = write("one.bin", 1000, 7);

		FileIo io;
		io.init();

		bool intact = false;

		auto job = [&]
		{
			FileRead read = read_now(io, path.c_str());

			intact = read.error == FileError::None && matches(read.bytes, 1000, 7);
			read.release();
		};

		jobs::Counter done;
		jobs::kick(jobs::make_job(job, "read"), &done);
		jobs::wait(done);

		EXPECT_TRUE(intact);
		EXPECT_EQ(io.pending(), 0u);
	}

	TEST_F(FileIoTest, ManyJobsParkAtOnceAndEveryReadComesBackIntact)
	{
		constexpr u32 FILES = 8;
		constexpr u32 JOBS	= 64;

		std::vector<std::string> paths;
		for (u32 i = 0; i < FILES; ++i)
			paths.push_back(write(("many" + std::to_string(i) + ".bin").c_str(), 4096 + i * 100, static_cast<u8>(i)));

		FileIo io;
		io.init({.queue_capacity = 16}); // smaller than the burst: the full path must not lose a signal

		std::atomic<u32> intact{0};
		std::atomic<u32> refused{0};

		auto body = [&](jobs::JobRange range)
		{
			for (u32 i = range.begin; i < range.end; ++i)
			{
				const u32 file = i % FILES;

				// A refused read still signals; try again the way a loader with a retry would.
				for (;;)
				{
					FileRead read = read_now(io, paths[file].c_str());

					if (read.error == FileError::QueueFull)
					{
						refused.fetch_add(1);
						continue;
					}

					if (read.error == FileError::None && matches(read.bytes, 4096 + file * 100, static_cast<u8>(file)))
						intact.fetch_add(1);

					read.release();
					break;
				}
			}
		};

		jobs::parallel_for({.count = JOBS, .grain = 1, .name = "reads"}, body);

		EXPECT_EQ(intact.load(), JOBS);
		EXPECT_EQ(io.pending(), 0u);
	}

	TEST_F(FileIoTest, AMissingFileFailsAndStillSignals)
	{
		FileIo io;
		io.init();

		const std::string path = (m_dir / "nope.bin").string();
		const FileRead read	   = read_now(io, path.c_str());

		EXPECT_EQ(read.error, FileError::NotFound);
		EXPECT_TRUE(read.bytes.empty());
	}

	TEST_F(FileIoTest, AnEmptyFileReadsAsNoBytesAndNoError)
	{
		const std::string path = write("empty.bin", 0, 0);

		FileIo io;
		io.init();

		FileRead read = read_now(io, path.c_str());

		EXPECT_EQ(read.error, FileError::None);
		EXPECT_TRUE(read.bytes.empty());
		read.release(); // a no-op, and legal
	}

	TEST_F(FileIoTest, AFileOverTheLimitIsRefused)
	{
		const std::string path = write("big.bin", 17, 1);

		FileIo io;
		io.init({.max_bytes = 16});

		const FileRead read = read_now(io, path.c_str());

		EXPECT_EQ(read.error, FileError::TooLarge);
		EXPECT_TRUE(read.bytes.empty());
	}

	TEST_F(FileIoTest, AReadBeforeInitFailsFast)
	{
		const std::string path = write("early.bin", 8, 3);

		FileIo io; // never started: no thread, no cells

		const FileRead read = read_now(io, path.c_str());

		EXPECT_EQ(read.error, FileError::QueueFull);
	}

	TEST_F(FileIoTest, ShutdownFinishesEveryQueuedRead)
	{
		constexpr u32 COUNT = 32;

		const std::string path = write("late.bin", 256, 9);

		FileIo io;
		io.init();

		FileRead reads[COUNT];
		jobs::Counter done{COUNT};

		for (FileRead& read : reads)
		{
			read = {.path = path.c_str(), .memory = &memory::heap(MemoryTag::Assets)};
			io.read(read, done);
		}

		io.shutdown(); // drains first, then joins
		jobs::wait(done);

		for (FileRead& read : reads)
		{
			EXPECT_EQ(read.error, FileError::None);
			EXPECT_TRUE(matches(read.bytes, 256, 9));
			read.release();
		}
	}
}
