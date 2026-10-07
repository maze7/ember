#include <ember/core/hash.h>
#include <ember/net/file_courier.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
	using namespace ember;
	using namespace ember::net;

	using Outcome = FileReceiver::Outcome;

	[[nodiscard]] Vector<u8> pattern(size_t size, u8 seed)
	{
		Vector<u8> bytes(size, &memory::heap(MemoryTag::Network));
		for (size_t i = 0; i < size; ++i)
			bytes[i] = static_cast<u8>(seed + i * 3);
		return bytes;
	}

	struct Arrived
	{
		Outcome outcome;
		std::string name;
		u32 size = 0;
		u64 hash = 0;
		std::vector<u8> bytes;
	};

	/** Everything the sender has, carried across one message at a time, as the lane would. */
	[[nodiscard]] std::vector<Arrived> carry(FileSender& sender, FileReceiver& receiver, u32 at_most = 100000)
	{
		std::vector<Arrived> arrived;

		for (u32 i = 0; i < at_most; ++i)
		{
			const Span<const u8> message = sender.next();
			if (message.empty())
				break;

			EXPECT_LE(message.size(), MAX_BULK_BYTES);

			// The same bytes again until sent(): a transport that refused gets the same message next tick.
			const Span<const u8> again = sender.next();
			EXPECT_EQ(again.data(), message.data());
			EXPECT_EQ(again.size(), message.size());

			const Outcome outcome = receiver.receive(message);
			sender.sent();

			if (outcome == Outcome::Nothing)
				continue;

			Arrived entry{.outcome = outcome,
						  .name	   = std::string(receiver.name()),
						  .size	   = receiver.size(),
						  .hash	   = receiver.hash()};

			if (outcome == Outcome::File)
			{
				const Vector<u8> bytes = receiver.take_bytes();
				entry.bytes.assign(bytes.begin(), bytes.end());
			}

			arrived.push_back(std::move(entry));

			if (outcome == Outcome::Malformed)
				break;
		}

		return arrived;
	}

	TEST(FileCourier, ABatchOfFilesArrivesWholeAndInOrder)
	{
		FileSender sender;
		FileReceiver receiver;

		const Vector<u8> big   = pattern(150'000, 1); // three Data messages
		const Vector<u8> small = pattern(10, 2);

		sender.queue("textures/tiles.png", hash_bytes(big), Vector<u8>(big));
		sender.queue("materials/a.material", hash_bytes(small), Vector<u8>(small));
		sender.queue("empty.txt", hash_bytes({}), Vector<u8>(&memory::heap(MemoryTag::Network)));

		EXPECT_EQ(sender.pending_count(), 3u);
		EXPECT_EQ(sender.pending_bytes(), 150'010u);

		const std::vector<Arrived> arrived = carry(sender, receiver);

		ASSERT_EQ(arrived.size(), 4u);
		EXPECT_EQ(arrived[0].outcome, Outcome::File);
		EXPECT_EQ(arrived[0].name, "textures/tiles.png");
		EXPECT_EQ(arrived[0].size, 150'000u);
		EXPECT_EQ(arrived[0].hash, hash_bytes(big));
		EXPECT_TRUE(std::equal(big.begin(), big.end(), arrived[0].bytes.begin(), arrived[0].bytes.end()));

		EXPECT_EQ(arrived[1].outcome, Outcome::File);
		EXPECT_EQ(arrived[1].name, "materials/a.material");
		EXPECT_TRUE(std::equal(small.begin(), small.end(), arrived[1].bytes.begin(), arrived[1].bytes.end()));

		EXPECT_EQ(arrived[2].outcome, Outcome::File);
		EXPECT_EQ(arrived[2].name, "empty.txt");
		EXPECT_TRUE(arrived[2].bytes.empty());

		EXPECT_EQ(arrived[3].outcome, Outcome::End);
		EXPECT_TRUE(sender.idle());
		EXPECT_FALSE(receiver.in_batch());
	}

	TEST(FileCourier, AnOfferIsAnsweredWithWantsWhichGoAheadOfEverything)
	{
		FileSender host;
		FileReceiver joiner;
		host.set_token(0x1234);

		host.offer("a.png", 100, 11);
		host.offer("b.png", 200, 22);

		const std::vector<Arrived> offered = carry(host, joiner);
		ASSERT_EQ(offered.size(), 3u);
		EXPECT_EQ(offered[0].outcome, Outcome::Offered);
		EXPECT_EQ(offered[0].name, "a.png");
		EXPECT_EQ(offered[0].size, 100u);
		EXPECT_EQ(offered[0].hash, 11u);
		EXPECT_EQ(offered[1].outcome, Outcome::Offered);
		EXPECT_EQ(offered[1].name, "b.png");
		EXPECT_EQ(offered[2].outcome, Outcome::End);

		// The joiner wants one of them, and has a file of its own queued behind the want.
		FileSender back;
		FileReceiver at_host;
		back.queue("upload.bin", 5, pattern(5, 9));
		back.want("b.png", 22);

		const std::vector<Arrived> answered = carry(back, at_host);
		ASSERT_EQ(answered.size(), 3u);
		EXPECT_EQ(answered[0].outcome, Outcome::Want);
		EXPECT_EQ(answered[0].name, "b.png");
		EXPECT_EQ(answered[0].hash, 22u);
		EXPECT_EQ(answered[1].outcome, Outcome::File);
		EXPECT_EQ(answered[1].name, "upload.bin");
		EXPECT_EQ(answered[2].outcome, Outcome::End);
	}

	TEST(FileCourier, TheTokenRidesOnTheBegin)
	{
		FileSender sender;
		FileReceiver receiver;
		sender.set_token(77);
		sender.queue("x.bin", 1, pattern(3, 1));

		EXPECT_EQ(receiver.receive(sender.next()), Outcome::Nothing); // Begin
		EXPECT_TRUE(receiver.in_batch());
		EXPECT_EQ(receiver.token(), 77u);
		EXPECT_FALSE(receiver.offering());
		sender.sent();

		(void)carry(sender, receiver);
		EXPECT_EQ(receiver.token(), 0u);
		EXPECT_FALSE(receiver.in_batch());
	}

	TEST(FileCourier, AFileQueuedAgainBeforeItLeftReplacesTheFirst)
	{
		FileSender sender;
		FileReceiver receiver;

		sender.queue("a.bin", 1, pattern(100, 1));
		sender.queue("a.bin", 2, pattern(40, 2));
		EXPECT_EQ(sender.pending_count(), 1u);
		EXPECT_EQ(sender.pending_bytes(), 40u);

		const std::vector<Arrived> arrived = carry(sender, receiver);
		ASSERT_EQ(arrived.size(), 2u);
		EXPECT_EQ(arrived[0].size, 40u);
		EXPECT_EQ(arrived[0].hash, 2u);
	}

	TEST(FileCourier, OffersGoBeforeFilesQueuedAfterThem)
	{
		FileSender sender;
		FileReceiver receiver;

		sender.queue("first.bin", 1, pattern(8, 1));
		sender.offer("offered.bin", 8, 2);

		const std::vector<Arrived> arrived = carry(sender, receiver);
		ASSERT_EQ(arrived.size(), 4u);
		EXPECT_EQ(arrived[0].outcome, Outcome::Offered);
		EXPECT_EQ(arrived[1].outcome, Outcome::End);
		EXPECT_EQ(arrived[2].outcome, Outcome::File);
		EXPECT_EQ(arrived[2].name, "first.bin");
		EXPECT_EQ(arrived[3].outcome, Outcome::End);
	}

	TEST(FileCourier, OutOfSequenceMessagesAreMalformed)
	{
		BulkBuffer buffer;

		{
			FileReceiver receiver;
			FileMessage data{.kind = FileMessageKind::Data, .size = 4};
			u8 bytes[4] = {};
			data.data	= bytes;
			EXPECT_EQ(receiver.receive(encode_file_message(data, buffer)), Outcome::Malformed) << "data before a batch";
		}

		{
			FileReceiver receiver;
			FileMessage file{.kind = FileMessageKind::File, .size = 4};
			std::memcpy(file.name, "x", 2);
			EXPECT_EQ(receiver.receive(encode_file_message(file, buffer)), Outcome::Malformed)
				<< "a file outside a batch";
		}

		{
			FileReceiver receiver;
			const FileMessage begin{.kind = FileMessageKind::Begin, .count = 2};
			EXPECT_EQ(receiver.receive(encode_file_message(begin, buffer)), Outcome::Nothing);
			EXPECT_EQ(receiver.receive(encode_file_message(begin, buffer)), Outcome::Malformed)
				<< "a batch inside a batch";
		}

		{
			FileReceiver receiver;
			const FileMessage begin{.kind = FileMessageKind::Begin, .count = 2};
			EXPECT_EQ(receiver.receive(encode_file_message(begin, buffer)), Outcome::Nothing);
			const FileMessage end{.kind = FileMessageKind::End};
			EXPECT_EQ(receiver.receive(encode_file_message(end, buffer)), Outcome::Malformed)
				<< "an end with files owed";
		}

		{
			// More data than the file announced: refused before a byte lands past the buffer.
			FileReceiver receiver;
			const FileMessage begin{.kind = FileMessageKind::Begin, .count = 1};
			EXPECT_EQ(receiver.receive(encode_file_message(begin, buffer)), Outcome::Nothing);

			FileMessage file{.kind = FileMessageKind::File, .size = 10};
			std::memcpy(file.name, "x.bin", 6);
			EXPECT_EQ(receiver.receive(encode_file_message(file, buffer)), Outcome::Nothing);

			u8 bytes[20] = {};
			FileMessage data{.kind = FileMessageKind::Data, .size = 20};
			data.data = bytes;
			EXPECT_EQ(receiver.receive(encode_file_message(data, buffer)), Outcome::Malformed);
		}

		{
			FileReceiver receiver;
			const u8 junk[] = {0xFF, 0xFF, 0xFF, 0xFF};
			EXPECT_EQ(receiver.receive(junk), Outcome::Malformed);
			EXPECT_EQ(receiver.receive({}), Outcome::Malformed);
		}
	}

	TEST(FileCourier, ProgressFollowsTheFileUnderWay)
	{
		FileSender sender;
		FileReceiver receiver;
		sender.queue("big.bin", 1, pattern(FILE_DATA_BYTES * 2, 1));

		EXPECT_EQ(receiver.receive(sender.next()), Outcome::Nothing); // Begin
		sender.sent();
		EXPECT_EQ(receiver.receive(sender.next()), Outcome::Nothing); // File
		sender.sent();
		EXPECT_FLOAT_EQ(receiver.progress(), 0.0f);
		EXPECT_EQ(receiver.receive(sender.next()), Outcome::Nothing); // half the Data
		sender.sent();
		EXPECT_FLOAT_EQ(receiver.progress(), 0.5f);
		EXPECT_EQ(receiver.receive(sender.next()), Outcome::File);
		sender.sent();
		EXPECT_FLOAT_EQ(receiver.progress(), 1.0f);
	}
}
