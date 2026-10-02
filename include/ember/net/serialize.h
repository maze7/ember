#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/net/sequence.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <type_traits>

/**
 * serialize (Glenn Fiedler, github.com/mas-bandwidth/serialize, vendored in third_party/serialize):
 * The wire format of ember::net and of every game type that crosses it. Include it through this header,
 * never directly, so every file sees one configuration.
 *
 * A type crosses the wire through one member template that reads, writes, and measure:
 *
 *     struct Command
 *     {
 *         glm::i8vec2 move = {};
 *         u8 buttons       = 0;
 *
 *         template <class Stream> bool serialize(Stream& stream)
 *         {
 *             serialize_int(stream, move.x, -127, 127);
 *             serialize_int(stream, move.y, -127, 127);
 *             serialize_bits(stream, buttons, 4);
 *             return true;
 *         }
 *     };
 *
 * Reads distrust the bytes: a value out of range, a short packet or a dirty padding fails the read,
 * the macro returns false out of serialize(), the stream stays failed, and the packet is dropped.
 *
 * Writes are trusted: serialize only asserts them, so in a release build a write past the end of
 * the buffer corrupts memory. A builder measures what it is about to write (measure_bits) and checks
 * that it fits before it writes; serialize streams cannot take a write back.
 *
 * Upstream names the member Serialize, and serialize_object calls that. The engine names it serialize,
 * like the rest of its methods, and nests objects with a plain call:
 *
 * 		if (!position.serialize(stream))
 * 			return false;
 */
#ifdef SERIALIZE_H
	#error "include serialize through <ember/net/serialize.h> only, so every file sees one configuration"
#endif

#define serialize_assert(condition) EMBER_ASSERT(condition)

#include <serialize.h>

// serialize also has read_ and write_ macros, for types that read and write in separate functions.
// The engine writes one serialize() per type and never uses them, and their plain names break other
// libraries (fmt's write_int) in every file that includes both, so they go.
#undef read_align
#undef write_align
#undef read_bits
#undef write_bits
#undef read_bool
#undef write_bool
#undef read_bytes
#undef write_bytes
#undef read_double
#undef write_double
#undef read_fixed
#undef write_fixed
#undef read_float
#undef write_float
#undef read_int
#undef write_int
#undef read_int128
#undef write_int128
#undef read_int64
#undef write_int64
#undef read_int_relative
#undef write_int_relative
#undef read_object
#undef write_object
#undef read_string
#undef write_string
#undef read_uint128
#undef write_uint128
#undef read_uint16
#undef write_uint16
#undef read_uint32
#undef write_uint32
#undef read_uint64
#undef write_uint64
#undef read_uint8
#undef write_uint8
#undef read_wstring
#undef write_wstring

/**
 * An enum whose values run from 0 to count - 1, in the bits that range needs. A value past the end
 * fails the read, so with the enum's Count as the count, a peer built with values this build does
 * not know is refused instead of aliased.
 *
 *     serialize_enum(stream, shape, Shape::Count);
 */
#define serialize_enum(stream, value, count)                                                                           \
	do                                                                                                                 \
	{                                                                                                                  \
		using EmberSerializeEnum_ = std::remove_cvref_t<decltype(value)>;                                              \
		static_assert(std::is_enum_v<EmberSerializeEnum_>, "serialize_enum takes an enum");                            \
		int32_t ember_serialize_enum_raw_ = 0;                                                                         \
		if (Stream::IsWriting)                                                                                         \
			ember_serialize_enum_raw_ = static_cast<int32_t>(value);                                                   \
		serialize_int(stream, ember_serialize_enum_raw_, 0, static_cast<int32_t>(count) - 1);                          \
		if (Stream::IsReading)                                                                                         \
			value = static_cast<EmberSerializeEnum_>(ember_serialize_enum_raw_);                                       \
	} while (0)

/**
 * A bitmask enum in `bits` bits. A set bit outside `known` fails the read, so a peer cannot switch
 * on flags this build does not have.
 *
 *     serialize_flags(stream, command.held, 4, Actions::All);
 */
#define serialize_flags(stream, value, bits, known)                                                                    \
	do                                                                                                                 \
	{                                                                                                                  \
		using EmberSerializeFlags_ = std::remove_cvref_t<decltype(value)>;                                             \
		static_assert(std::is_enum_v<EmberSerializeFlags_>, "serialize_flags takes an enum");                          \
		uint64_t ember_serialize_flags_raw_			= 0;                                                               \
		const uint64_t ember_serialize_flags_known_ = static_cast<uint64_t>(known);                                    \
		if (Stream::IsWriting)                                                                                         \
		{                                                                                                              \
			ember_serialize_flags_raw_ = static_cast<uint64_t>(value);                                                 \
			EMBER_ASSERT((ember_serialize_flags_raw_ & ~ember_serialize_flags_known_) == 0 && "unknown flag set");     \
		}                                                                                                              \
		serialize_bits(stream, ember_serialize_flags_raw_, bits);                                                      \
		if (Stream::IsReading)                                                                                         \
		{                                                                                                              \
			if ((ember_serialize_flags_raw_ & ~ember_serialize_flags_known_) != 0)                                     \
				return serialize::serialize_fail(stream);                                                              \
			value = static_cast<EmberSerializeFlags_>(ember_serialize_flags_raw_);                                     \
		}                                                                                                              \
	} while (0)

/**
 * A float, bit for bit, that must be finite: NaN and infinity off the wire fail the read instead of
 * reaching a simulation. Prefer serialize_compressed_float or serialize_fixed for gameplay values;
 * they cost fewer bits and cannot carry either.
 */
#define serialize_finite_float(stream, value)                                                                          \
	do                                                                                                                 \
	{                                                                                                                  \
		float ember_serialize_float_ = 0.0f;                                                                           \
		if (Stream::IsWriting)                                                                                         \
		{                                                                                                              \
			EMBER_ASSERT(std::isfinite(value) && "readers refuse non-finite floats");                                  \
			ember_serialize_float_ = (value);                                                                          \
		}                                                                                                              \
		serialize_float(stream, ember_serialize_float_);                                                               \
		if (Stream::IsReading)                                                                                         \
		{                                                                                                              \
			if (!std::isfinite(ember_serialize_float_))                                                                \
				return serialize::serialize_fail(stream);                                                              \
			value = ember_serialize_float_;                                                                            \
		}                                                                                                              \
	} while (0)

namespace ember::net
{
	/**
	 * Packet memory both serialize and streams accept. The writer stores whole qwords, so it writes into
	 * MAX_PACKET_BYTES, a multiple of 8; the reader loads 64 bit windows, so 8 readable bytes must follow a packet's
	 * last byte. One buffer of MAX_PACKET_BYTES plus thyat slack serves either way.
	 */
	struct PacketBuffer
	{
		static constexpr u32 SLACK = 8;

		alignas(8) std::array<u8, MAX_PACKET_BYTES + SLACK> bytes = {};
	};

	/** A stream that writes at most one packet into buffer. Flush() it before sending. */
	[[nodiscard]] inline serialize::WriteStream packet_writer(PacketBuffer& buffer) noexcept
	{
		return serialize::WriteStream(buffer.bytes.data(), MAX_PACKET_BYTES);
	}

	/** What a writer wrote, once flushed. */
	[[nodiscard]] inline Span<const u8> written(const PacketBuffer& buffer,
												const serialize::WriteStream& stream) noexcept
	{
		return {buffer.bytes.data(), static_cast<size_t>(stream.GetBytesProcessed())};
	}

	/**
	 * Opens bytes off the wire for reading: copies them into buffer, which adds the slack the reader needs,
	 * and points the stream there. False, when the stream failed, for more than a packet.
	 */
	[[nodiscard]] inline bool packet_reader(serialize::ReadStream& stream, PacketBuffer& buffer,
											Span<const u8> bytes) noexcept
	{
		if (bytes.size() > MAX_PACKET_BYTES)
		{
			stream.Initialize(buffer.bytes.data(), 0);
			return stream.Fail();
		}

		return stream.InitializePadded(buffer.bytes.data(), static_cast<int64_t>(buffer.bytes.size()), bytes.data(),
									   static_cast<int64_t>(bytes.size()));
	}

	/** Bits a value takes on the wire: its serialize() run through a MeasureStream. */
	template <class T> [[nodiscard]] u32 measure_bits(const T& value) noexcept
	{
		serialize::MeasureStream stream;
		[[maybe_unused]] const bool measured = const_cast<T&>(value).serialize(stream); // measuring reads nothing
		EMBER_ASSERT(measured);
		return static_cast<u32>(stream.GetBitsProcessed());
	}

	/** Writes a value through its serialize(), which leaves the value alone when writing. */
	template <class T> bool write(serialize::WriteStream& stream, const T& value) noexcept
	{
		return const_cast<T&>(value).serialize(stream);
	}

	/** Reads a value through its serialize(). On false the value may be partly read; drop it. */
	template <class T> [[nodiscard]] bool read(serialize::ReadStream& stream, T& value) noexcept
	{
		return value.serialize(stream);
	}
}
