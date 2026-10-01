#include <ember/net/command_stream.h>

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace
{
	using namespace ember;
	using namespace ember::net;

	constexpr u8 HELD	 = 1 << 0;
	constexpr u8 PRESSED = 1 << 1;

	struct Input
	{
		i8 move	   = 0;
		u8 buttons = 0;

		bool operator==(const Input&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_int(stream, move, -100, 100);
			serialize_bits(stream, buttons, 2);
			return true;
		}
	};

	// A stand-in keeps what is held and drops what was a one-tick press.
	void repeat_command(Input& input) { input.buttons &= ~PRESSED; }

	// A press that came too late lands on the next tick instead of nowhere.
	void merge_late_command(Input& next, const Input& late) { next.buttons |= late.buttons & PRESSED; }

	// The biggest command there is, sixteen numbers that never repeat: the worst a packet can face.
	struct Wide
	{
		std::array<u32, MAX_COMMAND_BYTES / 4> values = {};

		bool operator==(const Wide&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			for (u32& value : values)
				serialize_bits(stream, value, 32);
			return true;
		}
	};

	// A command whose default is not all zeroes, with a float that needs its alignment.
	struct Aimed
	{
		f32 aim	  = 0.25f;
		i8 facing = 1;

		bool operator==(const Aimed&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_float(stream, aim);
			serialize_int(stream, facing, -1, 1);
			return true;
		}
	};

	// Raw bits, the way a hostile client would put them on the wire.
	struct Raw
	{
		std::vector<std::pair<u32, u32>> fields; // value, bits

		template <class Stream> bool serialize(Stream& stream)
		{
			for (auto& [value, bits] : fields)
				serialize_bits(stream, value, bits);
			return true;
		}
	};

	const CommandCodec CODEC = command_codec<Input>();
	constexpr f64 DT		 = 1.0 / 60.0;

	// One packet as it travels: the bytes a writer flushed. It keeps a size, not a span, so a copy never
	// points into the original.
	struct Packet
	{
		PacketBuffer buffer;
		u32 size = 0;

		[[nodiscard]] Span<const u8> data() const { return {buffer.bytes.data(), size}; }

		// Opens the packet for reading; every reader gets its own copy, as each arrival would.
		[[nodiscard]] bool open(serialize::ReadStream& stream, PacketBuffer& scratch) const
		{
			return packet_reader(stream, scratch, data());
		}
	};

	[[nodiscard]] Packet section(CommandSender& sender, Sequence packet, u8 epoch = 0, u32 reserve_bits = 0)
	{
		Packet out;
		serialize::WriteStream stream = packet_writer(out.buffer);
		sender.write(stream, packet, epoch, reserve_bits);
		stream.Flush();
		out.size = static_cast<u32>(written(out.buffer, stream).size());
		return out;
	}

	template <class T> [[nodiscard]] Packet raw(const T& value)
	{
		Packet out;
		serialize::WriteStream stream = packet_writer(out.buffer);
		EXPECT_TRUE(write(stream, value));
		stream.Flush();
		out.size = static_cast<u32>(written(out.buffer, stream).size());
		return out;
	}

	[[nodiscard]] bool deliver(CommandQueue& queue, const Packet& packet, f64 arrival)
	{
		PacketBuffer scratch;
		serialize::ReadStream stream;
		return packet.open(stream, scratch) && queue.read(stream, arrival);
	}

	TEST(CommandCodec, FindsTheGamesHooksAndRepeatsCostOneBit)
	{
		EXPECT_EQ(CODEC.size, sizeof(Input));
		EXPECT_NE(CODEC.repeat, nullptr);
		EXPECT_NE(CODEC.merge_late, nullptr);

		const Input a = {.move = -40, .buttons = HELD};

		serialize::MeasureStream measure;
		ASSERT_TRUE(CODEC.measure(measure, &a, nullptr));
		EXPECT_EQ(measure.GetBitsProcessed(), 1 + 8 + 2) << "the repeat flag, then the command";

		Packet packet;
		serialize::WriteStream stream = packet_writer(packet.buffer);
		ASSERT_TRUE(CODEC.write(stream, &a, nullptr));
		const i64 first = stream.GetBitsProcessed();
		ASSERT_TRUE(CODEC.write(stream, &a, &a));
		EXPECT_EQ(stream.GetBitsProcessed() - first, 1) << "a repeat is one bit";
		stream.Flush();
		packet.size = static_cast<u32>(stream.GetBytesProcessed());

		PacketBuffer scratch;
		serialize::ReadStream reader;
		ASSERT_TRUE(packet.open(reader, scratch));

		Input x, y;
		ASSERT_TRUE(CODEC.read(reader, &x, nullptr));
		ASSERT_TRUE(CODEC.read(reader, &y, &x));
		EXPECT_EQ(x, a);
		EXPECT_EQ(y, a);

		// "Same as before" with nothing before is not something a writer produces.
		const Packet same = raw(Raw{.fields = {{1, 1}}});
		serialize::ReadStream bad;
		ASSERT_TRUE(same.open(bad, scratch));
		EXPECT_FALSE(CODEC.read(bad, &x, nullptr));
	}

	TEST(CommandSender, CarriesEveryUnacknowledgedCommandUntilAPacketWithItArrives)
	{
		CommandSender sender(CODEC);
		CommandQueue queue(CODEC);

		for (Tick tick = 1; tick <= 5; ++tick)
			sender.push(tick, Input{.move = static_cast<i8>(tick)}, 0.0f);

		// Packet 0 is lost; packet 1 carries all five again.
		(void)section(sender, 0);
		sender.on_notice({.sequence = 0, .delivered = false});
		EXPECT_EQ(sender.acknowledged(), NO_TICK);

		ASSERT_TRUE(deliver(queue, section(sender, 1), 0.0));
		EXPECT_EQ(queue.newest(), 5u);

		sender.on_notice({.sequence = 1, .delivered = true});
		EXPECT_EQ(sender.acknowledged(), 5u);

		// With everything acknowledged a packet carries no commands at all.
		const Packet empty = section(sender, 2);
		EXPECT_EQ(empty.size, 2u) << "the epoch and a zero count";
	}

	TEST(CommandSender, CapsAPacketToTheNewestCommands)
	{
		CommandSender sender(CODEC);
		CommandQueue queue(CODEC);

		for (Tick tick = 1; tick <= 100; ++tick)
			sender.push(tick, Input{.move = static_cast<i8>(tick % 100)}, 0.0f);

		ASSERT_TRUE(deliver(queue, section(sender, 0), 0.0));

		Input out;
		EXPECT_EQ(queue.take(68, out, 0.0).source, CommandSource::Empty) << "older than the newest 32: not sent";
		EXPECT_EQ(queue.take(69, out, 0.0).source, CommandSource::Received);
		EXPECT_EQ(out.move, 69);
	}

	TEST(CommandSender, APacketNeverOverflows)
	{
		// 32 commands of 64 bytes that never repeat need 16 KiB, twice a packet.
		const CommandCodec codec = command_codec<Wide>();
		CommandSender sender(codec);

		for (Tick tick = 1; tick <= 40; ++tick)
		{
			Wide wide;
			for (u32 i = 0; i < wide.values.size(); ++i)
				wide.values[i] = tick * 1000 + i;
			sender.push(tick, wide, 0.0f);
		}

		for (const u32 reserve : {0u, 1000u, 4000u})
		{
			// The header first, as a real packet would have it.
			Packet packet;
			serialize::WriteStream stream = packet_writer(packet.buffer);
			Connection connection;
			(void)connection.write_header(stream, 0.0);

			sender.write(stream, 0, 0, reserve);
			EXPECT_LE(stream.GetBitsProcessed() + reserve, MAX_PACKET_BYTES * 8) << "reserve " << reserve;
			stream.Flush();
			packet.size = static_cast<u32>(written(packet.buffer, stream).size());

			// What fits is the newest, in order, ending at the last pushed.
			CommandQueue queue(codec);
			PacketBuffer scratch;
			serialize::ReadStream reader;
			ASSERT_TRUE(packet.open(reader, scratch));
			Sequence sequence = 0;
			ASSERT_EQ(Connection().read_header(reader, sequence, 0.0), Connection::Arrival::Fresh);
			ASSERT_TRUE(queue.read(reader, 0.0));
			EXPECT_EQ(queue.newest(), 40u);

			u32 carried = 0;
			Tick oldest = NO_TICK;
			Wide out;
			for (Tick tick = 9; tick <= 40; ++tick)
			{
				if (queue.take(tick, out, 0.0).source == CommandSource::Received)
				{
					EXPECT_EQ(out.values[3], tick * 1000 + 3);
					oldest = oldest == NO_TICK ? tick : oldest;
					++carried;
				}
			}

			EXPECT_GT(carried, 4u);
			EXPECT_LT(carried, 32u) << "not everything fits, so the oldest were left out";
			EXPECT_EQ(oldest + carried - 1, 40u) << "what was carried is the newest, without gaps";
		}
	}

	TEST(CommandSender, TicksTheClockJumpedOverTravelAsGaps)
	{
		CommandSender sender(CODEC);
		CommandQueue queue(CODEC);

		sender.push(1, Input{.move = 7}, 0.0f);
		sender.push(4, Input{.move = 7}, 0.0f); // jumped over 2 and 3

		ASSERT_TRUE(deliver(queue, section(sender, 0), 0.0));

		Input out;
		EXPECT_EQ(queue.take(1, out, 0.0).source, CommandSource::Received);
		EXPECT_EQ(queue.take(2, out, 0.0).source, CommandSource::Repeated);
		EXPECT_EQ(queue.take(3, out, 0.0).source, CommandSource::Repeated);
		EXPECT_EQ(queue.take(4, out, 0.0).source, CommandSource::Received);
	}

	TEST(CommandSender, ATickAlreadySentKeepsItsFirstCommand)
	{
		CommandSender sender(CODEC);

		sender.push(10, Input{.move = 1}, 0.0f);
		sender.push(10, Input{.move = 2}, 0.0f); // after the clock jumped back

		Input out;
		ASSERT_TRUE(sender.find(10, out));
		EXPECT_EQ(out.move, 1);
		EXPECT_FALSE(sender.find(11, out));
	}

	TEST(CommandSender, TheSectionLayoutIsFixed)
	{
		// Replays and debugging tools read sections written by other builds: pin the bits.
		CommandSender sender(CODEC);
		sender.push(5, Input{.move = 3, .buttons = HELD}, 1.0f);
		sender.push(6, Input{.move = 3, .buttons = HELD}, 1.0f);

		// epoch 2 | count 2 | first 5 | present, view 16, new, move 3 + 100, held | present, view 16,
		// repeat. Least significant bit first.
		const std::vector<u8> expected = {0x02, 0x42, 0x01, 0x00, 0x00, 0x40, 0x08, 0x70, 0x56, 0x08, 0x08};
		const Packet packet			   = section(sender, 0, 2);
		EXPECT_EQ(std::vector<u8>(packet.data().begin(), packet.data().end()), expected);
	}

	TEST(CommandQueue, HandsOutEachTicksCommandWithTheViewItWasAimedAt)
	{
		CommandSender sender(CODEC);
		CommandQueue queue(CODEC, {.tick_seconds = DT});

		const Input input = {.move = -3, .buttons = HELD};
		sender.push(20, input, 6.5f);
		ASSERT_TRUE(deliver(queue, section(sender, 0), 1.0));

		Input out;
		const TakenCommand taken = queue.take(20, out, 1.0 + 2.0 * DT);

		EXPECT_EQ(taken.source, CommandSource::Received);
		EXPECT_EQ(out, input);
		EXPECT_FLOAT_EQ(taken.view_delay, 6.5f);
		ASSERT_TRUE(queue.has_timing());
		EXPECT_NEAR(queue.timing().mean, 2.0f, 1e-4f) << "it waited two ticks";
		EXPECT_EQ(queue.misses(), 0u);
	}

	TEST(CommandQueue, AMissingCommandRunsOnAStandInAndALatePressStillLands)
	{
		CommandSender sender(CODEC);
		CommandQueue queue(CODEC, {.tick_seconds = DT});

		const Input held	= {.move = 5, .buttons = HELD};
		const Input pressed = {.move = 5, .buttons = HELD | PRESSED};

		sender.push(1, pressed, 0.0f);
		ASSERT_TRUE(deliver(queue, section(sender, 0), 0.0));
		sender.on_notice({.sequence = 0, .delivered = true});

		Input out;
		ASSERT_EQ(queue.take(1, out, 0.01).source, CommandSource::Received);
		EXPECT_EQ(out.buttons, HELD | PRESSED);

		// Tick 2's command, another press, is late: the tick runs on tick 1's, held but not pressed again.
		const TakenCommand stand_in = queue.take(2, out, 0.02);
		EXPECT_EQ(stand_in.source, CommandSource::Repeated);
		EXPECT_EQ(out.buttons, HELD) << "a stand-in never repeats a press";
		EXPECT_EQ(queue.misses(), 1u);

		// It arrives a tick late, is measured as late, and lands on tick 3.
		const f32 before = queue.timing().mean;
		sender.push(2, pressed, 0.0f);
		sender.push(3, held, 0.0f);
		ASSERT_TRUE(deliver(queue, section(sender, 1), 0.02 + DT));
		EXPECT_NEAR(queue.timing().mean, before + (-1.0f - before) / 32.0f, 1e-4f) << "one tick late";

		ASSERT_EQ(queue.take(3, out, 0.02 + DT + 0.001).source, CommandSource::Received);
		EXPECT_EQ(out.buttons, HELD | PRESSED) << "the late press is merged, once";

		// Repeats of the late command change nothing.
		ASSERT_TRUE(deliver(queue, section(sender, 2), 0.1));
		sender.push(4, held, 0.0f);
		ASSERT_TRUE(deliver(queue, section(sender, 3), 0.1));
		queue.take(4, out, 0.11);
		EXPECT_EQ(out.buttons, HELD);
	}

	TEST(CommandQueue, BeforeAnythingArrivesATickRunsOnTheDefaultCommand)
	{
		const CommandCodec codec = command_codec<Aimed>();
		CommandSender sender(codec);
		CommandQueue queue(codec);

		Aimed out = {.aim = -9.0f, .facing = -1};
		EXPECT_EQ(queue.take(1, out, 0.0).source, CommandSource::Empty);
		EXPECT_EQ(out, Aimed{}) << "a default command, not zeroed bytes";

		// The stream keeps commands as bytes at any offset; a float inside travels all the same.
		const Aimed aimed = {.aim = 1.5f, .facing = -1};
		sender.push(2, aimed, 0.0f);
		ASSERT_TRUE(deliver(queue, section(sender, 0), 0.0));
		EXPECT_EQ(queue.take(2, out, 0.0).source, CommandSource::Received);
		EXPECT_EQ(out, aimed);
		EXPECT_EQ(queue.take(3, out, 0.0).source, CommandSource::Repeated);
		EXPECT_EQ(out, aimed);
	}

	TEST(CommandQueue, TheSpreadOfArrivalsIsMeasured)
	{
		CommandSender sender(CODEC);
		CommandQueue steady(CODEC, {.tick_seconds = DT});
		CommandQueue jittery(CODEC, {.tick_seconds = DT});

		// Every command is taken at its tick; steady ones arrive two ticks ahead, jittery ones one or
		// three ticks ahead in turn.
		Input out;
		for (Tick tick = 1; tick <= 600; ++tick)
		{
			sender.push(tick, Input{}, 0.0f);
			const Packet packet = section(sender, static_cast<Sequence>(tick));
			sender.on_notice({.sequence = static_cast<Sequence>(tick), .delivered = true});

			const f64 due = tick * DT;
			ASSERT_TRUE(deliver(steady, packet, due - 2.0 * DT));
			ASSERT_TRUE(deliver(jittery, packet, due - (tick % 2 == 0 ? 1.0 : 3.0) * DT));
			steady.take(tick, out, due);
			jittery.take(tick, out, due);
		}

		EXPECT_NEAR(steady.timing().mean, 2.0f, 0.01f);
		EXPECT_NEAR(steady.timing().deviation, 0.0f, 0.01f);
		EXPECT_NEAR(jittery.timing().mean, 2.0f, 0.1f);
		EXPECT_NEAR(jittery.timing().deviation, 1.0f, 0.1f);
		EXPECT_EQ(jittery.misses(), 0u);
	}

	TEST(CommandQueue, TheFirstCopyOfACommandWins)
	{
		CommandSender a(CODEC);
		CommandSender b(CODEC);
		CommandQueue queue(CODEC);

		a.push(9, Input{.move = 1}, 0.0f);
		b.push(9, Input{.move = 2}, 0.0f);

		ASSERT_TRUE(deliver(queue, section(a, 0), 0.0));
		ASSERT_TRUE(deliver(queue, section(b, 0), 0.0));

		Input out;
		queue.take(9, out, 0.0);
		EXPECT_EQ(out.move, 1);
	}

	TEST(CommandQueue, ACommandTooFarAheadIsIgnored)
	{
		CommandSender sender(CODEC);
		CommandQueue queue(CODEC);

		Input out;
		queue.take(10, out, 0.0);

		sender.push(10 + COMMAND_HISTORY + 1, Input{.move = 1}, 0.0f);
		ASSERT_TRUE(deliver(queue, section(sender, 0), 0.0));
		EXPECT_EQ(queue.newest(), NO_TICK);
	}

	TEST(CommandQueue, ANewEpochRestartsTheMeasurementAndAnOldOneIsNotMeasured)
	{
		CommandSender sender(CODEC);
		CommandQueue queue(CODEC, {.tick_seconds = DT});
		Input out;

		sender.push(1, Input{}, 0.0f);
		ASSERT_TRUE(deliver(queue, section(sender, 0, 3), 0.0));
		queue.take(1, out, 10.0 * DT);
		EXPECT_NEAR(queue.timing().mean, 10.0f, 1e-3f);
		EXPECT_EQ(queue.timing().epoch, 3);

		sender.push(2, Input{}, 0.0f);
		ASSERT_TRUE(deliver(queue, section(sender, 1, 4), 1.0));
		EXPECT_EQ(queue.timing().epoch, 4);
		EXPECT_FALSE(queue.has_timing()) << "nothing measured on the new clock yet";

		// A straggler from epoch 3 still delivers its command but is not measured.
		sender.push(3, Input{}, 0.0f);
		ASSERT_TRUE(deliver(queue, section(sender, 2, 3), 1.0));
		queue.take(2, out, 1.0 + DT);
		EXPECT_NEAR(queue.timing().mean, 1.0f, 1e-3f);
		EXPECT_EQ(queue.take(3, out, 1.0 + 2.0 * DT).source, CommandSource::Received);
		EXPECT_NEAR(queue.timing().mean, 1.0f, 1e-3f) << "the epoch 3 command was not measured";
	}

	TEST(CommandQueue, ALateCommandFromAnOldEpochIsNotMeasured)
	{
		CommandSender old_clock(CODEC);
		CommandSender new_clock(CODEC);
		CommandQueue queue(CODEC, {.tick_seconds = DT});
		Input out;

		// Tick 1 runs on a stand-in; meanwhile the client's clock jumped, and epoch 4 is already here.
		EXPECT_EQ(queue.take(1, out, 0.0).source, CommandSource::Empty);
		new_clock.push(2, Input{}, 0.0f);
		ASSERT_TRUE(deliver(queue, section(new_clock, 0, 4), 0.0));

		// Tick 1's command straggles in from epoch 3, ten ticks late: it describes the old clock.
		old_clock.push(1, Input{}, 0.0f);
		ASSERT_TRUE(deliver(queue, section(old_clock, 0, 3), 10.0 * DT));
		EXPECT_FALSE(queue.has_timing()) << "the old clock's lateness is not the new clock's";

		EXPECT_EQ(queue.take(2, out, 2.0 * DT).source, CommandSource::Received);
		EXPECT_NEAR(queue.timing().mean, 2.0f, 1e-3f);
	}

	TEST(CommandQueue, MalformedSectionsAreRefused)
	{
		CommandQueue queue(CODEC);

		// epoch, then a count of 33 where 32 is the most, and 33 ticks the clock jumped over.
		Raw too_many = {.fields = {{0, 8}, {33, 6}, {100, 32}}};
		too_many.fields.resize(too_many.fields.size() + 33, {0, 1});
		EXPECT_FALSE(deliver(queue, raw(too_many), 0.0));

		// epoch, count 3, first tick 100, then nothing.
		EXPECT_FALSE(deliver(queue, raw(Raw{.fields = {{0, 8}, {3, 6}, {100, 32}}}), 0.0));

		// epoch, count 1, first tick 0: ticks start at 1.
		EXPECT_FALSE(deliver(queue, raw(Raw{.fields = {{0, 8}, {1, 6}, {0, 32}, {0, 1}}}), 0.0));

		// epoch, count 2, a first tick whose second would wrap past the end of time.
		EXPECT_FALSE(deliver(queue, raw(Raw{.fields = {{0, 8}, {2, 6}, {0xffff'ffff, 32}, {0, 1}, {0, 1}}}), 0.0));

		EXPECT_EQ(queue.newest(), NO_TICK);
	}

	TEST(CommandQueue, ASectionThatFailsHalfwayKeepsNothing)
	{
		CommandQueue queue(CODEC);

		// Ticks 10 and 11 decode; tick 12's move is 120, outside the command's -100 to 100.
		const Packet packet = raw(Raw{.fields = {
										  {0, 8},		 // epoch
										  {3, 6},		 // count
										  {10, 32},		 // first tick
										  {1, 1},		 // 10: present
										  {0, 12},		 //     view
										  {0, 1},		 //     not a repeat
										  {100 + 5, 8},	 //     move 5, as an offset from -100
										  {1, 2},		 //     buttons
										  {1, 1},		 // 11: present
										  {0, 12},		 //     view
										  {1, 1},		 //     a repeat of 10
										  {1, 1},		 // 12: present
										  {0, 12},		 //     view
										  {0, 1},		 //     not a repeat
										  {100 + 120, 8} //     move 120: out of range
									  }});

		EXPECT_FALSE(deliver(queue, packet, 0.0));
		EXPECT_EQ(queue.newest(), NO_TICK) << "ticks 10 and 11 were not kept either";

		Input out;
		EXPECT_EQ(queue.take(10, out, 0.0).source, CommandSource::Empty);
	}

	TEST(CommandQueue, TimingRoundTripsThroughAPacket)
	{
		CommandSender sender(CODEC);
		CommandQueue queue(CODEC, {.tick_seconds = DT});

		const auto report = [&queue]
		{
			Packet packet;
			serialize::WriteStream stream = packet_writer(packet.buffer);
			write_command_timing(stream, queue);
			stream.Flush();
			packet.size = static_cast<u32>(written(packet.buffer, stream).size());
			return packet;
		};

		PacketBuffer scratch;
		serialize::ReadStream reader;
		ASSERT_TRUE(report().open(reader, scratch));

		CommandTiming timing;
		bool present = true;
		ASSERT_TRUE(read_command_timing(reader, timing, present));
		EXPECT_FALSE(present) << "nothing measured, nothing reported";

		sender.push(1, Input{}, 0.0f);
		ASSERT_TRUE(deliver(queue, section(sender, 0, 9), 0.0));
		Input out;
		queue.take(1, out, 1.5 * DT);

		serialize::ReadStream back;
		ASSERT_TRUE(report().open(back, scratch));
		ASSERT_TRUE(read_command_timing(back, timing, present));
		ASSERT_TRUE(present);
		EXPECT_EQ(timing.epoch, 9);
		EXPECT_NEAR(timing.mean, 1.5f, 1.0f / 32.0f);
		EXPECT_NEAR(timing.deviation, 0.5f, 1.0f / 32.0f);
	}
}
