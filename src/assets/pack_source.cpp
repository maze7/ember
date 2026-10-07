#include <ember/assets/pack_source.h>
#include <ember/core/logger.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace ember
{
	namespace
	{
		constexpr u32 PACK_MAGIC	 = 0x4B504D45; // "EMPK" little endian
		constexpr u32 PACK_VERSION	 = 1;
		constexpr u32 HEADER_BYTES	 = 16;
		constexpr u32 ENTRY_BYTES	 = 32; // offset, size, hash, name bytes + padding; the name follows, padded to 8
		constexpr u32 BLOB_ALIGNMENT = 16;
		constexpr u32 MAX_NAME_BYTES = 1024;

		[[nodiscard]] fs::FileError malformed() noexcept
		{
			return {.code = fs::FileErrorCode::InvalidArgument, .op = fs::FileOp::Read};
		}

		[[nodiscard]] u64 padded(u64 size, u64 alignment) noexcept
		{
			return (size + alignment - 1) / alignment * alignment;
		}

		struct Reader
		{
			Span<const u8> bytes;
			size_t at = 0;

			template <class T> [[nodiscard]] bool take(T& out) noexcept
			{
				if (bytes.size() - at < sizeof(T))
					return false;

				std::memcpy(&out, bytes.data() + at, sizeof(T));
				at += sizeof(T);
				return true;
			}
		};

		template <class T> void put(Vector<u8>& out, const T& value) noexcept
		{
			const size_t at = out.size();
			out.resize(at + sizeof(T));
			std::memcpy(out.data() + at, &value, sizeof(T));
		}
	}

	PackSource::PackSource(MemoryTag tag) noexcept
		: m_tag(tag), m_entries(&memory::heap(tag)), m_index(&memory::heap(tag)), m_overlay(&memory::heap(tag))
	{
	}

	PackSource::~PackSource() noexcept
	{
		if (m_file)
			(void)m_file.close();
	}

	Result<void, fs::FileError> PackSource::open(StringView path) noexcept
	{
		auto opened = fs::open(path);
		if (!opened)
			return fail(opened.error());

		m_file = std::move(opened.value());

		u8 header[HEADER_BYTES];
		if (const auto read = m_file.read_exact_at(0, header); !read)
			return fail(read.error());

		Reader reader{header};
		u32 magic = 0, version = 0, count = 0, index_bytes = 0;
		(void)reader.take(magic);
		(void)reader.take(version);
		(void)reader.take(count);
		(void)reader.take(index_bytes);

		if (magic != PACK_MAGIC || version != PACK_VERSION || index_bytes > 64u << 20)
			return fail(malformed());

		Vector<u8> index(index_bytes, &memory::heap(m_tag));
		if (const auto read = m_file.read_exact_at(HEADER_BYTES, index); !read)
			return fail(read.error());

		const auto size = m_file.size();
		if (!size)
			return fail(size.error());

		Reader entries{index};
		m_entries.clear();
		m_index.clear();

		for (u32 i = 0; i < count; ++i)
		{
			Entry entry{.name = String(&memory::heap(m_tag))};
			u32 name_bytes = 0, pad = 0;

			if (!entries.take(entry.offset) || !entries.take(entry.size) || !entries.take(entry.hash) ||
				!entries.take(name_bytes) || !entries.take(pad) || name_bytes == 0 || name_bytes > MAX_NAME_BYTES ||
				entries.bytes.size() - entries.at < padded(name_bytes, 8) || entry.offset > size.value() ||
				entry.size > size.value() - entry.offset)
				return fail(malformed());

			entry.name.assign(reinterpret_cast<const char*>(entries.bytes.data() + entries.at), name_bytes);
			entries.at += padded(name_bytes, 8);

			m_index.emplace(asset_id(entry.name), static_cast<u32>(m_entries.size()));
			m_entries.push_back(std::move(entry));
		}

		return {};
	}

	Result<void, fs::FileError> PackSource::write(StringView path, Span<const PackEntry> entries) noexcept
	{
		Vector<u8> index(&memory::heap(MemoryTag::Assets));
		Vector<u8> out(&memory::heap(MemoryTag::Assets));

		// The index's size is known before any blob: entries are fixed size plus the padded name.
		u64 index_bytes = 0;
		for (const PackEntry& entry : entries)
		{
			if (entry.name.empty() || entry.name.size() > MAX_NAME_BYTES)
				return fail(fs::FileError{.code = fs::FileErrorCode::InvalidArgument, .op = fs::FileOp::Write});

			index_bytes += ENTRY_BYTES + padded(entry.name.size(), 8);
		}

		u64 offset = padded(HEADER_BYTES + index_bytes, BLOB_ALIGNMENT);

		for (const PackEntry& entry : entries)
		{
			put(index, offset);
			put(index, static_cast<u64>(entry.bytes.size()));
			put(index, hash_bytes(entry.bytes));
			put(index, static_cast<u32>(entry.name.size()));
			put(index, u32{0});

			const size_t at = index.size();
			index.resize(at + padded(entry.name.size(), 8), 0);
			std::memcpy(index.data() + at, entry.name.data(), entry.name.size());

			offset = padded(offset + entry.bytes.size(), BLOB_ALIGNMENT);
		}

		put(out, PACK_MAGIC);
		put(out, PACK_VERSION);
		put(out, static_cast<u32>(entries.size()));
		put(out, static_cast<u32>(index_bytes));
		out.insert(out.end(), index.begin(), index.end());
		out.resize(padded(out.size(), BLOB_ALIGNMENT), 0);

		for (const PackEntry& entry : entries)
		{
			out.insert(out.end(), entry.bytes.begin(), entry.bytes.end());
			out.resize(padded(out.size(), BLOB_ALIGNMENT), 0);
		}

		return fs::write_file_atomic(path, out, fs::WriteDurability::None);
	}

	u32 PackSource::overlaid() const noexcept
	{
		std::scoped_lock lock(m_lock);
		return static_cast<u32>(m_overlay.size());
	}

	bool PackSource::contains(StringView name) const noexcept
	{
		const AssetId id = asset_id(name);

		{
			std::scoped_lock lock(m_lock);
			if (m_overlay.contains(id))
				return true;
		}

		return m_index.contains(id);
	}

	Result<fs::FileData, fs::FileError> PackSource::read(StringView name, std::pmr::memory_resource& memory) noexcept
	{
		const AssetId id = asset_id(name);

		// The overlay first: what was delivered stands in for the pack's copy.
		{
			std::scoped_lock lock(m_lock);

			if (const auto it = m_overlay.find(id); it != m_overlay.end())
			{
				auto data = fs::FileData::allocate(it->second.bytes.size(), memory);
				if (data)
					std::memcpy(data->data(), it->second.bytes.data(), it->second.bytes.size());
				return data;
			}
		}

		const auto it = m_index.find(id);
		if (it == m_index.end())
			return fail(fs::FileError{.code = fs::FileErrorCode::NotFound, .op = fs::FileOp::Open});

		const Entry& entry = m_entries[it->second];

		auto data = fs::FileData::allocate(static_cast<size_t>(entry.size), memory);
		if (!data)
			return data;

		if (const auto read = m_file.read_exact_at(entry.offset, data->bytes()); !read)
			return fail(read.error());

		return data;
	}

	Result<void, fs::FileError> PackSource::deliver(StringView name, Span<const u8> bytes) noexcept
	{
		Overlaid overlaid{.name = String(name, &memory::heap(m_tag)), .bytes = Vector<u8>(bytes.begin(), bytes.end(), &memory::heap(m_tag))};

		std::scoped_lock lock(m_lock);
		m_overlay.insert_or_assign(asset_id(name), std::move(overlaid));
		return {};
	}

	Result<void, fs::FileError> PackSource::enumerate(StringView below, Vector<String>& out) noexcept
	{
		// A name is below a prefix when it starts with it and a separator, or the prefix is empty.
		const auto under = [below](StringView name)
		{ return below.empty() || (name.size() > below.size() && name.starts_with(below) && name[below.size()] == '/'); };

		for (const Entry& entry : m_entries)
			if (under(entry.name))
				out.push_back(String(entry.name, out.get_allocator()));

		std::scoped_lock lock(m_lock);
		for (const auto& [id, overlaid] : m_overlay)
			if (under(overlaid.name) && !m_index.contains(id))
				out.push_back(String(overlaid.name, out.get_allocator()));

		return {};
	}
}
