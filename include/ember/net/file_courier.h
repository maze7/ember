#pragma once

#include <ember/core/common.h>
#include <ember/memory/memory.h>
#include <ember/net/serialize.h>
#include <ember/net/transport.h>

namespace ember::net
{
	/**
	 * Files over the bulk lane: how a host's saves reach the machines it plays with, and how a
	 * joiner catches up. A FileSender turns files into bulk messages, one batch at a time, and a
	 * FileReceiver turns them back into whole files, in the order sent, which the lane guarantees.
	 * Neither touches a disk or knows what a name means: the asset manager does, through deliver().
	 *
	 * A batch is Begin, then per file a File and as many Data messages as its size needs, then End.
	 * An offer batch carries File messages alone, names and hashes with no Data: the receiver
	 * answers with a Want for each it lacks, and the sender queues those files whole. Want is the
	 * one message that travels against the flow.
	 */
	inline constexpr u32 MAX_FILE_NAME	 = 240;		  // bytes of a name, as assets are named
	inline constexpr u32 MAX_FILE_BYTES	 = 64u << 20; // one file, whole
	inline constexpr u32 MAX_BATCH_FILES = 4096;
	inline constexpr u32 FILE_DATA_BYTES = MAX_BULK_BYTES - 64; // of one Data message: a bulk message less the framing

	enum class FileMessageKind : u8
	{
		Begin, // a batch of `count` files follows; `offer` means names and hashes only
		File,  // one file: name, size, hash; then, unless offered, `size` bytes in Data messages
		Data,  // the next `size` bytes of the file under way
		End,   // the batch is complete
		Want,  // receiver to sender: send this offered file whole
		Count
	};

	/**
	 * One courier message. Data points at the sender's bytes on the way out and at the receiver's
	 * buffer on the way in, so no message holds a copy of a file's bytes.
	 */
	struct FileMessage
	{
		FileMessageKind kind		 = FileMessageKind::End;
		u32 count					 = 0;	  // Begin: files in the batch
		bool offer					 = false; // Begin: names and hashes only
		u64 token					 = 0;	  // Begin: an upload's credential; zero from a server
		char name[MAX_FILE_NAME + 1] = {};	  // File, Want
		u32 size					 = 0;	  // File: the whole file; Data: bytes in `data`
		u64 hash					 = 0;	  // File, Want: hash_bytes() of the whole file

		u8* data	 = nullptr; // Data: where the bytes are (writing) or go (reading)
		u32 capacity = 0;		// Data, reading: how many may go there; a message with more fails

		template <class Stream> bool serialize(Stream& stream)
		{
			serialize_enum(stream, kind, FileMessageKind::Count);

			switch (kind)
			{
				case FileMessageKind::Begin:
					serialize_int(stream, count, 1, MAX_BATCH_FILES);
					serialize_bool(stream, offer);
					serialize_uint64(stream, token);
					return true;

				case FileMessageKind::File:
				case FileMessageKind::Want:
					serialize_string(stream, name, sizeof(name));
					serialize_int(stream, size, 0, MAX_FILE_BYTES);
					serialize_uint64(stream, hash);
					return true;

				case FileMessageKind::Data:
					serialize_int(stream, size, 1, FILE_DATA_BYTES);
					if (Stream::IsReading && size > capacity)
						return false;
					serialize_bytes(stream, data, size);
					return true;

				case FileMessageKind::End:
					return true;

				case FileMessageKind::Count:
					break;
			}

			return false;
		}
	};

	/** Memory for one bulk message: what the writer fills, what the reader copies into. */
	struct BulkBuffer
	{
		alignas(8) std::array<u8, MAX_BULK_BYTES + PacketBuffer::SLACK> bytes = {};
	};

	/** A message as bytes for send_bulk(), written into buffer. */
	[[nodiscard]] Span<const u8> encode_file_message(const FileMessage& message, BulkBuffer& buffer) noexcept;

	/**
	 * The message the bytes hold. A Data message's bytes land where `out.data` points, up to
	 * `out.capacity`, which the caller sets before the call. False for anything malformed.
	 */
	[[nodiscard]] bool decode_file_message(Span<const u8> bytes, FileMessage& out) noexcept;

	/**
	 * The sending end for one peer: files queued whole, offers, and wants, leaving in order as
	 * bulk messages. pump() hands out one message at a time, so the caller paces on the transport's
	 * pending bytes and never outruns its send buffer.
	 */
	class FileSender final
	{
	public:
		explicit FileSender(MemoryTag tag = MemoryTag::Network) noexcept;

		/** A file to send whole. The same name queued again before its send began replaces the first. */
		void queue(StringView name, u64 hash, Vector<u8>&& bytes) noexcept;

		/** An offer: what the receiver may ask for. Offers go in a batch of their own, before files queued after them.
		 */
		void offer(StringView name, u32 size, u64 hash) noexcept;

		/** Asks the far end for a file it offered. Goes out ahead of anything queued. */
		void want(StringView name, u64 hash) noexcept;

		/** The credential a Begin carries from here: for a client that uploads. */
		void set_token(u64 token) noexcept { m_token = token; }

		/**
		 * The next message, as bytes for send_bulk(), valid until the next call; empty when nothing
		 * is pending. The caller sends it and calls sent() when the transport took it, or tries the
		 * same message again later when it did not.
		 */
		[[nodiscard]] Span<const u8> next() noexcept;

		/** The message next() handed out went: move on. */
		void sent() noexcept;

		[[nodiscard]] bool idle() const noexcept
		{
			return m_wants.empty() && m_offers.empty() && m_files.empty() && m_batch.empty();
		}

		/** Files and offers not yet fully sent. */
		[[nodiscard]] u32 pending_count() const noexcept;

		/** Bytes of file data not yet sent. */
		[[nodiscard]] u64 pending_bytes() const noexcept;

	private:
		struct Pending
		{
			String name;
			u64 hash = 0;
			u32 size = 0;
			Vector<u8> bytes; // empty for an offer
		};

		enum class Stage : u8
		{
			Begin,
			File,
			Data,
			End,
		};

		[[nodiscard]] FileMessage compose() noexcept;

		MemoryTag m_tag;
		u64 m_token = 0;

		Vector<Pending> m_wants;  // ahead of everything, one message each
		Vector<Pending> m_offers; // the next offer batch
		Vector<Pending> m_files;  // the next file batch

		// The batch under way: taken from m_offers or m_files when Begin goes out.
		Vector<Pending> m_batch;
		bool m_batch_offer = false;
		Stage m_stage	   = Stage::Begin;
		u32 m_index		   = 0; // the file in m_batch under way
		u32 m_offset	   = 0; // bytes of it sent

		BulkBuffer m_buffer;
		bool m_composed		 = false; // m_buffer holds the message to send; sent() clears it
		u32 m_composed_bytes = 0;
	};

	/** The receiving end: messages in, whole files out, in the order they were sent. */
	class FileReceiver final
	{
	public:
		enum class Outcome : u8
		{
			Nothing,   // taken in; nothing complete yet
			Offered,   // an offered file: name(), size(), hash(); the caller may want() it
			File,	   // a whole file: name(), size(), hash(), take_bytes()
			End,	   // the batch is complete
			Want,	   // the far end asks for an offered file: name(), hash()
			Malformed, // out of sequence, or too much: a broken or hostile peer
		};

		explicit FileReceiver(MemoryTag tag = MemoryTag::Network) noexcept;

		[[nodiscard]] Outcome receive(Span<const u8> message) noexcept;

		[[nodiscard]] StringView name() const noexcept { return m_name; }
		[[nodiscard]] u32 size() const noexcept { return m_size; }
		[[nodiscard]] u64 hash() const noexcept { return m_hash; }

		/** The file just completed; empty after it is taken. */
		[[nodiscard]] Vector<u8> take_bytes() noexcept;

		/** The open batch's Begin: its credential and whether it offers. Zero and false outside one. */
		[[nodiscard]] u64 token() const noexcept { return m_token; }
		[[nodiscard]] bool offering() const noexcept { return m_offer; }

		/** True while a batch is open. */
		[[nodiscard]] bool in_batch() const noexcept { return m_state != State::Idle; }

		/** Of the file under way, 0 to 1; 1 when none is. */
		[[nodiscard]] f32 progress() const noexcept;

	private:
		enum class State : u8
		{
			Idle,	// between batches
			Batch,	// a Begin arrived; a File, or End, is next
			InFile, // a File arrived; its Data is next
		};

		MemoryTag m_tag;
		State m_state  = State::Idle;
		u64 m_token	   = 0;
		bool m_offer   = false;
		u32 m_expected = 0; // files the batch announced, less those complete
		String m_name;
		u32 m_size	   = 0;
		u64 m_hash	   = 0;
		u32 m_received = 0; // bytes of the file under way
		Vector<u8> m_bytes;
	};
}

namespace ember
{
	EMBER_ENUM_NAMES(net::FileMessageKind, "Begin", "File", "Data", "End", "Want");
	EMBER_ENUM_NAMES(net::FileReceiver::Outcome, "Nothing", "Offered", "File", "End", "Want", "Malformed");
}
