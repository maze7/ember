#pragma once

#include <ember/net/serialize.h>
#include <ember/net/tick.h>
#include <ember/net/transport.h>

#include <array>

namespace ember::net
{
	/**
	 * ember::net's wire format: the connection header, the command section, the timing report
	 * and the messages below. Bumped whenever any of them changes, so builds that would misread
	 * each other refuse each other at the handshake instead.
	 */
	inline constexpr u32 PROTOCOL_VERSION = 1;

	/** The largest game message the endpoints carry: a packet, less the message's own framing. */
	inline constexpr u32 MAX_MESSAGE_BYTES = MAX_PACKET_BYTES - 8;

	enum class MessageKind : u8
	{
		Hello,	 // client to server, reliable channel: which protocol the client speaks
		Welcome, // server to client: accepted, client slot & server's clock
		Game,	 // either way, once welcomed: the game's own bytes
		Count
	};

	struct Hello
	{
		u32 engine_protocol = PROTOCOL_VERSION;
		u32 game_protocol	= 0; // the game's own wire version: its commands, state and messages

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, engine_protocol, 32);
			serialize_bits(stream, game_protocol, 32);
			return true;
		}
	};

	struct Welcome
	{
		u8 slot			 = 0;		// the client's seat on the server, for the rest of the session
		Tick server_tick = NO_TICK; // the tick the server was about to simulate when it answered
		u16 held		 = 0;		// half the milliseconds the server held the Hello before answering

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_bits(stream, slot, 8);
			serialize_bits(stream, server_tick, 32);
			serialize_bits(stream, held, 16);
			return true;
		}
	};

	/**
	 * One message on the endpoint's reliable channel: its kind, then the message. Relable sends
	 * arrive once and in order, so the channel needs no sequence of its own; a message that does
	 * not decode means a broken or hostile peer, and costs it the connection.
	 */
	struct Message
	{
		MessageKind kind = MessageKind::Game;
		Hello hello		 = {};
		Welcome welcome	 = {};
		u32 size		 = 0; // Game: bytes of payload

		std::array<u8, MAX_MESSAGE_BYTES> payload = {};

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_enum(stream, kind, MessageKind::Count);

			switch (kind)
			{
				case MessageKind::Hello:
					return hello.serialize(stream);
				case MessageKind::Welcome:
					return welcome.serialize(stream);
				case MessageKind::Game:
					serialize_int(stream, size, 0, MAX_MESSAGE_BYTES);
					serialize_bytes(stream, payload.data(), size);
					return true;
				case MessageKind::Count:
					break;
			}

			return false;
		}
	};

	/** Sends a message on the reliable channel. */
	inline void send_message(Transport& transport, PeerId peer, const Message& message, f64 now) noexcept
	{
		PacketBuffer buffer;
		serialize::WriteStream stream = packet_writer(buffer);

		[[maybe_unused]] const bool wrote = write(stream, message);
		EMBER_ASSERT(wrote);

		stream.Flush();
		transport.send(peer, written(buffer, stream), Delivery::Reliable, now);
	}

	/** Reads one back; false when the bytes are not a message. */
	[[nodiscard]] inline bool read_message(Span<const u8> bytes, Message& message) noexcept
	{
		PacketBuffer buffer;
		serialize::ReadStream stream;
		return packet_reader(stream, buffer, bytes) && read(stream, message);
	}
}

namespace ember
{
	EMBER_ENUM_NAMES(net::MessageKind, "Hello", "Welcome", "Game");
}
