#include <ember/net/file_courier.h>

#include <algorithm>
#include <cstring>

namespace ember::net
{
	Span<const u8> encode_file_message(const FileMessage& message, BulkBuffer& buffer) noexcept
	{
		serialize::WriteStream stream(buffer.bytes.data(), MAX_BULK_BYTES);

		// serialize() both writes and reads, so it takes a value it may change: it gets a copy.
		FileMessage copy				  = message;
		[[maybe_unused]] const bool wrote = copy.serialize(stream);
		EMBER_ASSERT(wrote);

		stream.Flush();
		return {buffer.bytes.data(), static_cast<size_t>(stream.GetBytesProcessed())};
	}

	bool decode_file_message(Span<const u8> bytes, FileMessage& out) noexcept
	{
		if (bytes.empty() || bytes.size() > MAX_BULK_BYTES)
			return false;

		// Copied into a buffer with the slack the reader's 64 bit windows need past the last byte.
		BulkBuffer buffer;
		std::memcpy(buffer.bytes.data(), bytes.data(), bytes.size());

		serialize::ReadStream stream(buffer.bytes.data(), static_cast<int>(bytes.size()));
		return out.serialize(stream);
	}

	FileSender::FileSender(MemoryTag tag) noexcept
		: m_tag(tag), m_wants(&memory::heap(tag)), m_offers(&memory::heap(tag)), m_files(&memory::heap(tag)),
		  m_batch(&memory::heap(tag))
	{
	}

	void FileSender::queue(StringView name, u64 hash, Vector<u8>&& bytes) noexcept
	{
		EMBER_ASSERT(!name.empty() && name.size() <= MAX_FILE_NAME && bytes.size() <= MAX_FILE_BYTES);

		// A newer version of a file not yet begun takes the old one's place, so a save a second
		// after another costs one transfer, not two.
		for (Pending& pending : m_files)
		{
			if (pending.name == name)
			{
				pending.hash  = hash;
				pending.size  = static_cast<u32>(bytes.size());
				pending.bytes = std::move(bytes);
				return;
			}
		}

		m_files.push_back({.name  = String(name, &memory::heap(m_tag)),
						   .hash  = hash,
						   .size  = static_cast<u32>(bytes.size()),
						   .bytes = std::move(bytes)});
	}

	void FileSender::offer(StringView name, u32 size, u64 hash) noexcept
	{
		EMBER_ASSERT(!name.empty() && name.size() <= MAX_FILE_NAME);

		m_offers.push_back({.name  = String(name, &memory::heap(m_tag)),
							.hash  = hash,
							.size  = size,
							.bytes = Vector<u8>(&memory::heap(m_tag))});
	}

	void FileSender::want(StringView name, u64 hash) noexcept
	{
		EMBER_ASSERT(!name.empty() && name.size() <= MAX_FILE_NAME);

		m_wants.push_back({.name  = String(name, &memory::heap(m_tag)),
						   .hash  = hash,
						   .size  = 0,
						   .bytes = Vector<u8>(&memory::heap(m_tag))});
	}

	u32 FileSender::pending_count() const noexcept
	{
		const u32 in_batch = m_batch.empty() ? 0 : static_cast<u32>(m_batch.size()) - m_index;
		return static_cast<u32>(m_offers.size() + m_files.size()) + in_batch;
	}

	u64 FileSender::pending_bytes() const noexcept
	{
		u64 bytes = 0;
		for (const Pending& pending : m_files)
			bytes += pending.size;

		for (u32 i = m_index; i < m_batch.size(); ++i)
			bytes += m_batch[i].bytes.size() - (i == m_index ? m_offset : 0);

		return bytes;
	}

	FileMessage FileSender::compose() noexcept
	{
		FileMessage message;

		// Wants first: a joiner asking for files should not wait behind what it is already being sent.
		if (!m_wants.empty())
		{
			message.kind = FileMessageKind::Want;
			std::memcpy(message.name, m_wants.front().name.data(), m_wants.front().name.size());
			message.hash = m_wants.front().hash;
			return message;
		}

		// A new batch: offers before files, so a joiner hears what is on hand before anything arrives.
		if (m_batch.empty())
		{
			Vector<Pending>& source = !m_offers.empty() ? m_offers : m_files;
			m_batch_offer			= !m_offers.empty();
			m_batch.swap(source);
			m_stage	 = Stage::Begin;
			m_index	 = 0;
			m_offset = 0;
		}

		switch (m_stage)
		{
			case Stage::Begin:
				message.kind  = FileMessageKind::Begin;
				message.count = static_cast<u32>(m_batch.size());
				message.offer = m_batch_offer;
				message.token = m_token;
				return message;

			case Stage::File:
			{
				const Pending& pending = m_batch[m_index];
				message.kind		   = FileMessageKind::File;
				std::memcpy(message.name, pending.name.data(), pending.name.size());
				message.size = pending.size;
				message.hash = pending.hash;
				return message;
			}

			case Stage::Data:
			{
				const Pending& pending = m_batch[m_index];
				message.kind		   = FileMessageKind::Data;
				message.size		   = std::min<u32>(FILE_DATA_BYTES, pending.size - m_offset);
				message.data		   = const_cast<u8*>(pending.bytes.data()) + m_offset;
				return message;
			}

			case Stage::End:
				message.kind = FileMessageKind::End;
				return message;
		}

		return message;
	}

	Span<const u8> FileSender::next() noexcept
	{
		if (m_composed)
			return {m_buffer.bytes.data(), m_composed_bytes};

		if (idle())
			return {};

		const FileMessage message  = compose();
		const Span<const u8> bytes = encode_file_message(message, m_buffer);
		m_composed				   = true;
		m_composed_bytes		   = static_cast<u32>(bytes.size());
		return bytes;
	}

	void FileSender::sent() noexcept
	{
		EMBER_ASSERT(m_composed && "sent() follows a next() that handed something out");
		m_composed = false;

		if (!m_wants.empty())
		{
			m_wants.erase(m_wants.begin());
			return;
		}

		switch (m_stage)
		{
			case Stage::Begin:
				m_stage = Stage::File;
				return;

			case Stage::File:
				// An offer, or an empty file, is complete with its File message.
				if (m_batch_offer || m_batch[m_index].size == 0)
					break;

				m_stage	 = Stage::Data;
				m_offset = 0;
				return;

			case Stage::Data:
				m_offset += std::min<u32>(FILE_DATA_BYTES, m_batch[m_index].size - m_offset);
				if (m_offset < m_batch[m_index].size)
					return;
				break;

			case Stage::End:
				m_batch.clear();
				m_stage = Stage::Begin;
				return;
		}

		// On to the next file, or the End.
		m_stage = ++m_index < m_batch.size() ? Stage::File : Stage::End;
	}

	FileReceiver::FileReceiver(MemoryTag tag) noexcept
		: m_tag(tag), m_name(&memory::heap(tag)), m_bytes(&memory::heap(tag))
	{
	}

	Vector<u8> FileReceiver::take_bytes() noexcept
	{
		Vector<u8> bytes(&memory::heap(m_tag));
		bytes.swap(m_bytes);
		return bytes;
	}

	f32 FileReceiver::progress() const noexcept
	{
		if (m_state != State::InFile || m_size == 0)
			return 1.0f;

		return static_cast<f32>(m_received) / static_cast<f32>(m_size);
	}

	FileReceiver::Outcome FileReceiver::receive(Span<const u8> bytes) noexcept
	{
		FileMessage message;

		// Data lands straight in the file's buffer, after what has arrived.
		if (m_state == State::InFile)
		{
			message.data	 = m_bytes.data() + m_received;
			message.capacity = m_size - m_received;
		}

		if (!decode_file_message(bytes, message))
			return Outcome::Malformed;

		switch (message.kind)
		{
			case FileMessageKind::Want:
				if (m_state != State::Idle)
					return Outcome::Malformed;

				m_name.assign(message.name);
				m_hash = message.hash;
				m_size = 0;
				return Outcome::Want;

			case FileMessageKind::Begin:
				if (m_state != State::Idle)
					return Outcome::Malformed;

				m_state	   = State::Batch;
				m_token	   = message.token;
				m_offer	   = message.offer;
				m_expected = message.count;
				return Outcome::Nothing;

			case FileMessageKind::File:
				if (m_state != State::Batch || m_expected == 0 || message.name[0] == '\0')
					return Outcome::Malformed;

				--m_expected;
				m_name.assign(message.name);
				m_size	   = message.size;
				m_hash	   = message.hash;
				m_received = 0;

				if (m_offer)
					return Outcome::Offered;

				if (m_size == 0)
				{
					m_bytes.clear();
					return Outcome::File;
				}

				m_bytes.assign(m_size, 0);
				m_state = State::InFile;
				return Outcome::Nothing;

			case FileMessageKind::Data:
				if (m_state != State::InFile)
					return Outcome::Malformed;

				// The decode refused more than fits, so this never runs past the end.
				m_received += message.size;
				if (m_received < m_size)
					return Outcome::Nothing;

				m_state = State::Batch;
				return Outcome::File;

			case FileMessageKind::End:
				if (m_state != State::Batch || m_expected != 0)
					return Outcome::Malformed;

				m_state = State::Idle;
				m_token = 0;
				m_offer = false;
				return Outcome::End;

			case FileMessageKind::Count:
				break;
		}

		return Outcome::Malformed;
	}
}
