#include <ember/assets/asset_source.h>
#include <ember/memory/memory.h>

namespace ember
{
	namespace
	{
		[[nodiscard]] Heap& assets_heap() noexcept { return memory::heap(MemoryTag::Assets); }
	}

	DirectorySource::DirectorySource(StringView directory) noexcept : m_directory(directory, &assets_heap())
	{
		EMBER_ASSERT(!m_directory.empty() && m_directory.back() == '/');
	}

	bool DirectorySource::contains(StringView name) const noexcept
	{
		String file(&assets_heap());
		if (!file_of(name, file))
			return false;

		const auto found = fs::exists(file);
		return found && found.value();
	}

	Result<fs::FileData, fs::FileError> DirectorySource::read(StringView name,
															  std::pmr::memory_resource& memory) noexcept
	{
		String file(&assets_heap());
		if (!file_of(name, file))
			return fail(fs::FileError{.code = fs::FileErrorCode::InvalidPath, .op = fs::FileOp::ResolvePath});

		return fs::read_file(file, memory);
	}

	Result<void, fs::FileError> DirectorySource::deliver(StringView name, Span<const u8> bytes) noexcept
	{
		String file(&assets_heap());
		if (!file_of(name, file))
			return fail(fs::FileError{.code = fs::FileErrorCode::InvalidPath, .op = fs::FileOp::ResolvePath});

		// A file that is new here may be the first in its directory.
		if (const auto made = fs::create_directories(fs::parent(file)); !made)
			return fail(made.error());

		// Atomic, so a reader never sees half of it; durability is the next build's concern, not a
		// hot reload's.
		return fs::write_file_atomic(file, bytes, fs::WriteDurability::None);
	}

	bool DirectorySource::file_of(StringView name, String& out) const noexcept
	{
		return !name.empty() && fs::join(out, m_directory, name).has_value();
	}

	bool DirectorySource::name_of(StringView file, String& out) const noexcept
	{
		String normal(&assets_heap());
		if (!fs::is_absolute(file) || !fs::normalize_lexical(file, normal))
			return false;

		if (normal.size() <= m_directory.size() || !normal.starts_with(m_directory))
			return false;

		out.assign(StringView(normal).substr(m_directory.size()));
		return true;
	}

	namespace
	{
		/** Walks `directory` and its subdirectories, naming each file `prefix/<path below directory>`. */
		Result<void, fs::FileError> walk(StringView directory, StringView prefix, Vector<String>& out) noexcept
		{
			Vector<String> subdirectories(out.get_allocator());

			const auto visited = fs::enumerate(
				directory,
				[&](const fs::DirectoryEntry& entry) noexcept
				{
					String name(prefix, out.get_allocator());
					if (!name.empty())
						name += '/';
					name += entry.name;

					if (entry.type == fs::FileType::Directory)
						subdirectories.push_back(std::move(name));
					else if (entry.type == fs::FileType::Regular)
						out.push_back(std::move(name));

					return fs::Visit::Continue;
				});
			if (!visited)
				return visited;

			for (const String& subdirectory : subdirectories)
			{
				String path(out.get_allocator());
				if (const auto joined = fs::join(path, directory, StringView(subdirectory).substr(prefix.empty() ? 0 : prefix.size() + 1)); !joined)
					return joined;
				if (const auto walked = walk(path, subdirectory, out); !walked)
					return walked;
			}

			return {};
		}
	}

	Result<void, fs::FileError> DirectorySource::enumerate(StringView below, Vector<String>& out) noexcept
	{
		String directory(out.get_allocator());
		if (const auto joined = fs::join(directory, m_directory, below); !joined)
			return joined;

		return walk(directory, below, out);
	}
}
