#include <ember/core/bitmask.h>
#include <ember/net/serialize.h>

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace
{
	using namespace ember;
	using namespace ember::net;

	enum class Shape : u8
	{
		Circle,
		Box,
		Capsule,
		Count
	};

	enum class Buttons : u8
	{
		None = 0,
		Fire = 1 << 0,
		Dash = 1 << 1,
		All	 = Fire | Dash,
	};

	EMBER_ENUM_BITWISE_OPS(Buttons, u8);

	// A game type described once, for every stream.
	struct Command
	{
		i8 move_x		= 0;
		i8 move_y		= 0;
		Buttons buttons = Buttons::None;
		Shape shape		= Shape::Circle;
		f32 aim			= 0.0f;
		u32 sequence	= 0;

		bool operator==(const Command&) const = default;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_int(stream, move_x, -127, 127);
			serialize_int(stream, move_y, -127, 127);
			serialize_flags(stream, buttons, 3, Buttons::All);
			serialize_enum(stream, shape, Shape::Count);
			serialize_finite_float(stream, aim);
			serialize_bits(stream, sequence, 32);
			return true;
		}
	};

	struct JustShape
	{
		Shape shape = Shape::Box;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_enum(stream, shape, Shape::Count);
			return true;
		}
	};

	struct JustButtons
	{
		Buttons buttons = Buttons::None;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_flags(stream, buttons, 3, Buttons::All);
			return true;
		}
	};

	struct JustFloat
	{
		f32 value = 1.0f;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_finite_float(stream, value);
			return true;
		}
	};

	// Raw bits, the way a hostile peer would put them on the wire.
	struct Raw
	{
		u32 value = 0;
		u32 bits  = 0;

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, value, bits);
			return true;
		}
	};

	// A written packet. It keeps a size, not a span, so a copy never points into the original.
	struct Bytes
	{
		PacketBuffer buffer;
		u32 size = 0;

		[[nodiscard]] Span<const u8> data() const { return {buffer.bytes.data(), size}; }
	};

	template <class T> [[nodiscard]] Bytes written(const T& value)
	{
		Bytes out;
		serialize::WriteStream stream = packet_writer(out.buffer);
		EXPECT_TRUE(write(stream, value));
		stream.Flush();
		out.size = static_cast<u32>(net::written(out.buffer, stream).size());
		return out;
	}

	TEST(Serialize, ATypeRoundTripsAndMeasuresWhatItWrites)
	{
		const Command sent = {.move_x	= -127,
							  .move_y	= 64,
							  .buttons	= Buttons::Dash | Buttons::Fire,
							  .shape	= Shape::Capsule,
							  .aim		= -1.5f,
							  .sequence = 70000};

		const Bytes bytes = written(sent);
		EXPECT_EQ(measure_bits(sent), 8u + 8u + 3u + 2u + 32u + 32u);
		EXPECT_EQ(bytes.data().size(), (measure_bits(sent) + 7) / 8);

		PacketBuffer buffer;
		serialize::ReadStream stream;
		ASSERT_TRUE(packet_reader(stream, buffer, bytes.data()));

		Command received;
		ASSERT_TRUE(read(stream, received));
		EXPECT_EQ(received, sent);
	}

	TEST(Serialize, AnEnumPastItsCountIsRefusedAndLeftAlone)
	{
		// Two bits saying 3, where Shape stops at 2: a newer peer's value this build does not know.
		const Bytes bytes = written(Raw{.value = 3, .bits = 2});

		PacketBuffer buffer;
		serialize::ReadStream stream;
		ASSERT_TRUE(packet_reader(stream, buffer, bytes.data()));

		JustShape value;
		EXPECT_FALSE(read(stream, value));
		EXPECT_EQ(value.shape, Shape::Box) << "a refused read leaves the value as it was";
	}

	TEST(Serialize, AFlagThisBuildDoesNotKnowIsRefused)
	{
		const Bytes bytes = written(Raw{.value = 0b100, .bits = 3});

		PacketBuffer buffer;
		serialize::ReadStream stream;
		ASSERT_TRUE(packet_reader(stream, buffer, bytes.data()));

		JustButtons value;
		EXPECT_FALSE(read(stream, value));
		EXPECT_EQ(value.buttons, Buttons::None);
	}

	TEST(Serialize, NonFiniteFloatsAreRefused)
	{
		for (const f32 bad : {std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity(),
							  -std::numeric_limits<f32>::infinity()})
		{
			const Bytes bytes = written(Raw{.value = std::bit_cast<u32>(bad), .bits = 32});

			PacketBuffer buffer;
			serialize::ReadStream stream;
			ASSERT_TRUE(packet_reader(stream, buffer, bytes.data()));

			JustFloat value;
			EXPECT_FALSE(read(stream, value));
			EXPECT_EQ(value.value, 1.0f);
		}
	}

	TEST(Serialize, AnExactlySizedPayloadIsReadWithoutTouchingPastIt)
	{
		const Bytes bytes = written(Command{.sequence = 9});

		// Exactly the bytes, on the heap, nothing after them: ASan sees any read past the end.
		const std::vector<u8> exact(bytes.data().begin(), bytes.data().end());

		PacketBuffer buffer;
		serialize::ReadStream stream;
		ASSERT_TRUE(packet_reader(stream, buffer, exact));

		Command received;
		ASSERT_TRUE(read(stream, received));
		EXPECT_EQ(received.sequence, 9u);
	}

	TEST(Serialize, MoreThanAPacketIsRefused)
	{
		const std::vector<u8> huge(MAX_PACKET_BYTES + 1, 0);

		PacketBuffer buffer;
		serialize::ReadStream stream;
		EXPECT_FALSE(packet_reader(stream, buffer, huge));

		Raw raw{.bits = 1};
		EXPECT_FALSE(read(stream, raw)) << "the stream is left failed, not pointed at nothing";
	}

	TEST(Serialize, AShortPacketFailsAndTheStreamStaysFailed)
	{
		const Bytes bytes = written(Raw{.value = 0xffff, .bits = 16});

		PacketBuffer buffer;
		serialize::ReadStream stream;
		ASSERT_TRUE(packet_reader(stream, buffer, bytes.data()));

		Raw wide{.bits = 32};
		EXPECT_FALSE(read(stream, wide)) << "32 bits from a 2 byte packet";

		Raw narrow{.bits = 1};
		EXPECT_FALSE(read(stream, narrow)) << "failure is terminal, though one bit would fit";
	}

	TEST(Serialize, HostileBytesNeverReadOutOfBounds)
	{
		std::mt19937 random(1234);
		u32 accepted = 0;

		for (u32 round = 0; round < 5000; ++round)
		{
			std::vector<u8> junk(random() % 24);
			for (u8& byte : junk)
				byte = static_cast<u8>(random());

			PacketBuffer buffer;
			serialize::ReadStream stream;
			ASSERT_TRUE(packet_reader(stream, buffer, junk));

			Command command;
			if (read(stream, command))
			{
				++accepted;
				EXPECT_TRUE(std::isfinite(command.aim));
				EXPECT_LT(static_cast<u8>(command.shape), static_cast<u8>(Shape::Count));
				EXPECT_EQ(static_cast<u8>(command.buttons) & ~static_cast<u8>(Buttons::All), 0);
			}
		}

		EXPECT_GT(accepted, 0u) << "some random packets are valid commands";
	}
}
