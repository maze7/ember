#include <ember/core/json.h>
#include <ember/memory/memory.h>

#include <fmt/format.h>
#include <yyjson.h>

#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace ember
{
	namespace detail
	{
		yyjson_val* first_child(yyjson_val* container) noexcept { return unsafe_yyjson_get_first(container); }
		yyjson_val* next_sibling(yyjson_val* node) noexcept { return unsafe_yyjson_get_next(node); }

		u32 child_count(yyjson_val* container) noexcept
		{
			return static_cast<u32>(yyjson_is_obj(container) ? yyjson_obj_size(container) : yyjson_arr_size(container));
		}

		StringView key_text(yyjson_val* key) noexcept { return {yyjson_get_str(key), yyjson_get_len(key)}; }
	}

	bool JsonValue::is_object() const noexcept { return yyjson_is_obj(m_value); }
	bool JsonValue::is_array() const noexcept { return yyjson_is_arr(m_value); }
	bool JsonValue::is_string() const noexcept { return yyjson_is_str(m_value); }
	bool JsonValue::is_number() const noexcept { return yyjson_is_num(m_value); }
	bool JsonValue::is_bool() const noexcept { return yyjson_is_bool(m_value); }

	JsonValue JsonValue::operator[](StringView key) const noexcept
	{
		return JsonValue(yyjson_obj_getn(m_value, key.data(), key.size()));
	}

	JsonValue JsonValue::operator[](u32 index) const noexcept { return JsonValue(yyjson_arr_get(m_value, index)); }

	u32 JsonValue::size() const noexcept { return detail::child_count(m_value); }

	bool JsonValue::read(bool& out) const noexcept
	{
		if (!yyjson_is_bool(m_value))
			return false;

		out = yyjson_get_bool(m_value);
		return true;
	}

	bool JsonValue::read(i64& out) const noexcept
	{
		if (yyjson_is_sint(m_value))
		{
			out = yyjson_get_sint(m_value);
			return true;
		}

		if (yyjson_is_uint(m_value) && yyjson_get_uint(m_value) <= static_cast<u64>(std::numeric_limits<i64>::max()))
		{
			out = static_cast<i64>(yyjson_get_uint(m_value));
			return true;
		}

		return false;
	}

	bool JsonValue::read(u64& out) const noexcept
	{
		if (yyjson_is_uint(m_value))
		{
			out = yyjson_get_uint(m_value);
			return true;
		}

		if (yyjson_is_sint(m_value) && yyjson_get_sint(m_value) >= 0)
		{
			out = static_cast<u64>(yyjson_get_sint(m_value));
			return true;
		}

		return false;
	}

	bool JsonValue::read(i32& out) const noexcept
	{
		i64 wide = 0;
		if (!read(wide) || wide < std::numeric_limits<i32>::min() || wide > std::numeric_limits<i32>::max())
			return false;

		out = static_cast<i32>(wide);
		return true;
	}

	bool JsonValue::read(u32& out) const noexcept
	{
		u64 wide = 0;
		if (!read(wide) || wide > std::numeric_limits<u32>::max())
			return false;

		out = static_cast<u32>(wide);
		return true;
	}

	bool JsonValue::read(f64& out) const noexcept
	{
		if (!yyjson_is_num(m_value))
			return false;

		out = yyjson_get_num(m_value);
		return true;
	}

	bool JsonValue::read(f32& out) const noexcept
	{
		f64 wide = 0.0;
		if (!read(wide))
			return false;

		out = static_cast<f32>(wide);
		return true;
	}

	bool JsonValue::read(StringView& out) const noexcept
	{
		if (!yyjson_is_str(m_value))
			return false;

		out = {yyjson_get_str(m_value), yyjson_get_len(m_value)};
		return true;
	}

	u32 JsonValue::numbers(Span<f32> out) const noexcept
	{
		if (out.empty())
			return 0;

		if (yyjson_is_num(m_value))
		{
			out[0] = static_cast<f32>(yyjson_get_num(m_value));
			return 1;
		}

		u32 count = 0;

		for (const JsonValue element : elements())
		{
			if (count == out.size() || !element.read(out[count]))
				break;
			++count;
		}

		return count;
	}

	JsonMembers JsonValue::members() const noexcept { return JsonMembers(yyjson_is_obj(m_value) ? m_value : nullptr); }
	JsonElements JsonValue::elements() const noexcept
	{
		return JsonElements(yyjson_is_arr(m_value) ? m_value : nullptr);
	}

	// ---- Json -------------------------------------------------------------------------------------

	namespace
	{
		constexpr size_t BLOCK_ALIGNMENT = 16;

		void fail(JsonError* error, u32 line, u32 column, const char* message) noexcept
		{
			if (error != nullptr)
				*error = {.line = line, .column = column, .message = message};
		}
	}

	bool Json::parse(StringView text, std::pmr::memory_resource& memory, JsonRead mode, JsonError* error) noexcept
	{
		reset();

		if (error != nullptr)
			*error = {};

		if (text.empty())
		{
			fail(error, 1, 1, "empty text");
			return false;
		}

		const yyjson_read_flag flags = mode == JsonRead::Relaxed
										   ? YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS
										   : YYJSON_READ_NOFLAG;

		// yyjson tells how much a parse of this text can need at most; one block of that size
		// holds the copy of the text, every node and the document itself.
		const size_t size = yyjson_read_max_memory_usage(text.size(), flags);
		void* block		  = memory.allocate(size, BLOCK_ALIGNMENT);

		yyjson_alc allocator;
		if (!yyjson_alc_pool_init(&allocator, block, size))
		{
			memory.deallocate(block, size, BLOCK_ALIGNMENT);
			fail(error, 1, 1, "text too small to parse"); // the pool needs a few words; a document has more
			return false;
		}

		yyjson_read_err read_error;
		yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(text.data()), text.size(), flags, &allocator, &read_error);

		if (doc == nullptr)
		{
			size_t line	  = 0;
			size_t column = 0;
			size_t offset = 0;
			yyjson_locate_pos(text.data(), text.size(), read_error.pos, &line, &column, &offset);
			fail(error, static_cast<u32>(line), static_cast<u32>(column), read_error.msg);
			memory.deallocate(block, size, BLOCK_ALIGNMENT);
			return false;
		}

		m_doc		 = doc;
		m_block		 = block;
		m_block_size = size;
		m_memory	 = &memory;
		return true;
	}

	JsonValue Json::root() const noexcept { return JsonValue(yyjson_doc_get_root(m_doc)); }

	void Json::reset() noexcept
	{
		if (m_block == nullptr)
			return;

		// The document lives inside the block, so freeing it only returns pool memory.
		yyjson_doc_free(m_doc);
		m_memory->deallocate(m_block, m_block_size, BLOCK_ALIGNMENT);

		m_doc		 = nullptr;
		m_block		 = nullptr;
		m_block_size = 0;
		m_memory	 = nullptr;
	}

	namespace
	{
		// A size word in front of every block, so free() can give the resource its size back.
		constexpr size_t BLOCK_HEADER = 16;

		void* writer_alloc(void* context, size_t size) noexcept
		{
			auto& memory = *static_cast<std::pmr::memory_resource*>(context);
			void* block	 = memory.allocate(size + BLOCK_HEADER, BLOCK_HEADER);
			std::memcpy(block, &size, sizeof(size));
			return static_cast<u8*>(block) + BLOCK_HEADER;
		}

		void writer_free(void* context, void* pointer) noexcept
		{
			if (pointer == nullptr)
				return;

			auto& memory = *static_cast<std::pmr::memory_resource*>(context);
			u8* block	 = static_cast<u8*>(pointer) - BLOCK_HEADER;
			size_t size	 = 0;
			std::memcpy(&size, block, sizeof(size));
			memory.deallocate(block, size + BLOCK_HEADER, BLOCK_HEADER);
		}

		void* writer_realloc(void* context, void* pointer, size_t old_size, size_t size) noexcept
		{
			void* next = writer_alloc(context, size);

			if (pointer != nullptr)
			{
				std::memcpy(next, pointer, old_size < size ? old_size : size);
				writer_free(context, pointer);
			}

			return next;
		}
	}

	struct JsonWriter::Impl
	{
		std::pmr::memory_resource* memory = nullptr;
		yyjson_alc allocator{};
		yyjson_mut_doc* doc = nullptr;
	};

	JsonWriter::JsonWriter(std::pmr::memory_resource& memory) noexcept
	{
		m_impl			  = memory::new_object<Impl>(memory);
		m_impl->memory	  = &memory;
		m_impl->allocator = {writer_alloc, writer_realloc, writer_free, &memory};
		m_impl->doc		  = yyjson_mut_doc_new(&m_impl->allocator);
	}

	JsonWriter::~JsonWriter() noexcept
	{
		yyjson_mut_doc_free(m_impl->doc);
		memory::delete_object(*m_impl->memory, m_impl);
	}

	JsonNode JsonWriter::root_object() noexcept
	{
		yyjson_mut_val* object = yyjson_mut_obj(m_impl->doc);
		yyjson_mut_doc_set_root(m_impl->doc, object);
		return {this, object};
	}

	JsonNode JsonWriter::root_array() noexcept
	{
		yyjson_mut_val* array = yyjson_mut_arr(m_impl->doc);
		yyjson_mut_doc_set_root(m_impl->doc, array);
		return {this, array};
	}

	JsonNode JsonWriter::root_merged(JsonValue base, JsonValue patch) noexcept
	{
		yyjson_mut_val* merged = yyjson_merge_patch(m_impl->doc, base.native(), patch.native());
		yyjson_mut_doc_set_root(m_impl->doc, merged);
		return {this, merged};
	}

	bool JsonWriter::write(String& out) const noexcept
	{
		out.clear();

		size_t length = 0;
		yyjson_write_err error;
		char* text =
			yyjson_mut_write_opts(m_impl->doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &m_impl->allocator, &length, &error);

		if (text == nullptr)
			return false;

		out.assign(text, length);
		out += '\n';
		m_impl->allocator.free(m_impl->allocator.ctx, text);
		return true;
	}

	namespace
	{
		/// A float as the double its shortest text names, so 0.65f is written as 0.65 and not as the
		/// 0.6499999761581421 a plain conversion would print. Non-finite values are null.
		[[nodiscard]] yyjson_mut_val* float_value(yyjson_mut_doc* doc, f32 value) noexcept
		{
			if (!std::isfinite(value))
				return yyjson_mut_null(doc);

			char text[32];
			const auto formatted = fmt::format_to_n(text, sizeof(text), "{}", value);

			f64 wide = 0.0;
			std::from_chars(text, text + formatted.size, wide);
			return yyjson_mut_real(doc, wide);
		}
	}

	yyjson_mut_doc* JsonNode::doc() const noexcept { return m_writer != nullptr ? m_writer->m_impl->doc : nullptr; }

	void JsonNode::put(StringView key, yyjson_mut_val* value) noexcept
	{
		if (m_value == nullptr || value == nullptr)
			return;

		yyjson_mut_obj_put(m_value, yyjson_mut_strncpy(doc(), key.data(), key.size()), value);
	}

	void JsonNode::append(yyjson_mut_val* value) noexcept
	{
		if (m_value != nullptr && value != nullptr)
			yyjson_mut_arr_append(m_value, value);
	}

	void JsonNode::set(StringView key, StringView value) noexcept
	{
		put(key, yyjson_mut_strncpy(doc(), value.data(), value.size()));
	}

	void JsonNode::set(StringView key, bool value) noexcept { put(key, yyjson_mut_bool(doc(), value)); }
	void JsonNode::set(StringView key, i32 value) noexcept { put(key, yyjson_mut_sint(doc(), value)); }
	void JsonNode::set(StringView key, u32 value) noexcept { put(key, yyjson_mut_uint(doc(), value)); }
	void JsonNode::set(StringView key, i64 value) noexcept { put(key, yyjson_mut_sint(doc(), value)); }
	void JsonNode::set(StringView key, u64 value) noexcept { put(key, yyjson_mut_uint(doc(), value)); }
	void JsonNode::set(StringView key, f32 value) noexcept { put(key, float_value(doc(), value)); }
	void JsonNode::set(StringView key, f64 value) noexcept { put(key, yyjson_mut_real(doc(), value)); }

	JsonNode JsonNode::object(StringView key) noexcept
	{
		yyjson_mut_val* object = yyjson_mut_obj(doc());
		put(key, object);
		return {m_writer, m_value != nullptr ? object : nullptr};
	}

	JsonNode JsonNode::array(StringView key) noexcept
	{
		yyjson_mut_val* array = yyjson_mut_arr(doc());
		put(key, array);
		return {m_writer, m_value != nullptr ? array : nullptr};
	}

	void JsonNode::push(StringView value) noexcept { append(yyjson_mut_strncpy(doc(), value.data(), value.size())); }

	void JsonNode::push(bool value) noexcept { append(yyjson_mut_bool(doc(), value)); }
	void JsonNode::push(i32 value) noexcept { append(yyjson_mut_sint(doc(), value)); }
	void JsonNode::push(u32 value) noexcept { append(yyjson_mut_uint(doc(), value)); }
	void JsonNode::push(i64 value) noexcept { append(yyjson_mut_sint(doc(), value)); }
	void JsonNode::push(u64 value) noexcept { append(yyjson_mut_uint(doc(), value)); }
	void JsonNode::push(f32 value) noexcept { append(float_value(doc(), value)); }
	void JsonNode::push(f64 value) noexcept { append(yyjson_mut_real(doc(), value)); }

	JsonNode JsonNode::push_object() noexcept
	{
		yyjson_mut_val* object = yyjson_mut_obj(doc());
		append(object);
		return {m_writer, m_value != nullptr ? object : nullptr};
	}

	JsonNode JsonNode::push_array() noexcept
	{
		yyjson_mut_val* array = yyjson_mut_arr(doc());
		append(array);
		return {m_writer, m_value != nullptr ? array : nullptr};
	}
}
