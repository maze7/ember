#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>

#include <memory_resource>

// yyjson stays behind this header: its node types are opaque here and json.cpp is the one file
// that includes yyjson.h.
struct yyjson_doc;
struct yyjson_val;
struct yyjson_mut_doc;
struct yyjson_mut_val;

namespace ember
{
	/// Matches text against an enum's EMBER_ENUM_NAMES table: "point" reads gpu::Filter::Nearest.
	template <class E> [[nodiscard]] bool parse_enum(StringView text, E& out) noexcept
	{
		constexpr auto names = enum_names<E>();

		for (size_t i = 0; i < names.size(); ++i)
		{
			if (text == names[i])
			{
				out = static_cast<E>(i);
				return true;
			}
		}

		return false;
	}

	struct JsonError
	{
		u32 line			= 0; // 1 based; 0 when nothing went wrong
		u32 column			= 0;
		const char* message = nullptr; // static text, never freed
	};

	/// How strictly a file is read. Cooked files and anything a tool wrote are Strict; files
	/// people write by hand are Relaxed, which allows comments and trailing commas.
	enum class JsonRead : u8
	{
		Strict,
		Relaxed,
	};

	class JsonMembers;
	class JsonElements;

	namespace detail
	{
		// Walks yyjson's flat node layout: a container's children follow it in memory, a key is
		// followed by its value, and next_sibling steps over a subtree.
		[[nodiscard]] yyjson_val* first_child(yyjson_val* container) noexcept;
		[[nodiscard]] yyjson_val* next_sibling(yyjson_val* node) noexcept;
		[[nodiscard]] u32 child_count(yyjson_val* container) noexcept;
		[[nodiscard]] StringView key_text(yyjson_val* key) noexcept;
	}

	/**
	 * A node of a parsed Json, or nothing. One pointer, copied freely, valid as long as the Json
	 * it came from. Every read is null safe and type safe: a missing key, a wrong type or a
	 * number out of range reads false and leaves the output alone, so a chain such as
	 * root["albedo"]["filter"].read(filter) needs no checks between its steps.
	 */
	class JsonValue
	{
	public:
		JsonValue() noexcept = default;
		explicit JsonValue(yyjson_val* value) noexcept : m_value(value) {}

		[[nodiscard]] bool is_null() const noexcept { return m_value == nullptr; }
		[[nodiscard]] explicit operator bool() const noexcept { return m_value != nullptr; }

		[[nodiscard]] bool is_object() const noexcept;
		[[nodiscard]] bool is_array() const noexcept;
		[[nodiscard]] bool is_string() const noexcept;
		[[nodiscard]] bool is_number() const noexcept;
		[[nodiscard]] bool is_bool() const noexcept;

		/// A member of an object or an element of an array; nothing for anything else.
		[[nodiscard]] JsonValue operator[](StringView key) const noexcept;
		[[nodiscard]] JsonValue operator[](u32 index) const noexcept;

		/// Members of an object or elements of an array; zero for anything else.
		[[nodiscard]] u32 size() const noexcept;

		[[nodiscard]] bool read(bool& out) const noexcept;
		[[nodiscard]] bool read(i32& out) const noexcept;
		[[nodiscard]] bool read(u32& out) const noexcept;
		[[nodiscard]] bool read(i64& out) const noexcept;
		[[nodiscard]] bool read(u64& out) const noexcept;
		[[nodiscard]] bool read(f32& out) const noexcept;
		[[nodiscard]] bool read(f64& out) const noexcept;
		[[nodiscard]] bool read(StringView& out) const noexcept; // points into the Json

		/// The value, or `fallback` when there is none of that type.
		template <class T> [[nodiscard]] T read_or(T fallback) const noexcept
		{
			T value = fallback;
			return read(value) ? value : fallback;
		}

		/// A string matched against an enum's EMBER_ENUM_NAMES table.
		template <class E> [[nodiscard]] bool read_enum(E& out) const noexcept
		{
			StringView text;
			return read(text) && parse_enum(text, out);
		}

		/// Numbers from a number or an array of numbers, stopping at out's size or at the first
		/// element that is not one. Returns how many were read.
		[[nodiscard]] u32 numbers(Span<f32> out) const noexcept;

		/// Range for loops: `for (auto [key, value] : object.members())` and
		/// `for (JsonValue element : array.elements())`. Empty for anything else.
		[[nodiscard]] JsonMembers members() const noexcept;
		[[nodiscard]] JsonElements elements() const noexcept;

		[[nodiscard]] yyjson_val* native() const noexcept { return m_value; }

	private:
		yyjson_val* m_value = nullptr;
	};

	struct JsonMember
	{
		StringView key;
		JsonValue value;
	};

	class JsonMembers
	{
	public:
		class Iterator
		{
		public:
			Iterator(yyjson_val* key, u32 remaining) noexcept : m_key(key), m_remaining(remaining) {}

			[[nodiscard]] JsonMember operator*() const noexcept
			{
				return {detail::key_text(m_key), JsonValue(detail::next_sibling(m_key))};
			}

			Iterator& operator++() noexcept
			{
				m_key = --m_remaining != 0 ? detail::next_sibling(detail::next_sibling(m_key)) : nullptr;
				return *this;
			}

			[[nodiscard]] bool operator==(const Iterator&) const noexcept = default;

		private:
			yyjson_val* m_key;
			u32 m_remaining;
		};

		explicit JsonMembers(yyjson_val* object) noexcept : m_object(object) {}

		[[nodiscard]] Iterator begin() const noexcept
		{
			const u32 count = detail::child_count(m_object);
			return {count != 0 ? detail::first_child(m_object) : nullptr, count};
		}

		[[nodiscard]] Iterator end() const noexcept { return {nullptr, 0}; }

	private:
		yyjson_val* m_object;
	};

	class JsonElements
	{
	public:
		class Iterator
		{
		public:
			Iterator(yyjson_val* element, u32 remaining) noexcept : m_element(element), m_remaining(remaining) {}

			[[nodiscard]] JsonValue operator*() const noexcept { return JsonValue(m_element); }

			Iterator& operator++() noexcept
			{
				m_element = --m_remaining != 0 ? detail::next_sibling(m_element) : nullptr;
				return *this;
			}

			[[nodiscard]] bool operator==(const Iterator&) const noexcept = default;

		private:
			yyjson_val* m_element;
			u32 m_remaining;
		};

		explicit JsonElements(yyjson_val* array) noexcept : m_array(array) {}

		[[nodiscard]] Iterator begin() const noexcept
		{
			const u32 count = detail::child_count(m_array);
			return {count != 0 ? detail::first_child(m_array) : nullptr, count};
		}

		[[nodiscard]] Iterator end() const noexcept { return {nullptr, 0}; }

	private:
		yyjson_val* m_array;
	};

	/**
	 * A parsed JSON file. One allocation of exactly the size the text needs, from the memory
	 * resource handed to parse(): an Arena for a load that reads and forgets, the heap for a
	 * tree that is kept. Values point into it, so it outlives every JsonValue taken from it.
	 */
	class Json
	{
	public:
		Json() noexcept = default;
		~Json() noexcept { reset(); }

		Json(const Json&)			 = delete;
		Json& operator=(const Json&) = delete;

		Json(Json&& other) noexcept { steal(other); }

		Json& operator=(Json&& other) noexcept
		{
			if (this != &other)
			{
				reset();
				steal(other);
			}
			return *this;
		}

		/**
		 * Parses `text`, which need not outlive the call. False with the line, column and reason
		 * in `error`. Strict is the standard; Relaxed also takes comments and trailing commas.
		 */
		[[nodiscard]] bool parse(StringView text, std::pmr::memory_resource& memory, JsonRead mode = JsonRead::Strict,
								 JsonError* error = nullptr) noexcept;

		/// The top level value; nothing before a successful parse.
		[[nodiscard]] JsonValue root() const noexcept;

		void reset() noexcept;

	private:
		void steal(Json& other) noexcept
		{
			m_doc		  = other.m_doc;
			m_block		  = other.m_block;
			m_block_size  = other.m_block_size;
			m_memory	  = other.m_memory;
			other.m_doc	  = nullptr;
			other.m_block = nullptr;
		}

		yyjson_doc* m_doc					= nullptr;
		void* m_block						= nullptr;
		size_t m_block_size					= 0;
		std::pmr::memory_resource* m_memory = nullptr;
	};

	class JsonWriter;

	/**
	 * A node of a tree being written. Setting a key that exists replaces its value. Nothing on
	 * a null node does anything, so a chain of writes needs no checks either.
	 */
	class JsonNode
	{
	public:
		JsonNode() noexcept = default;

		[[nodiscard]] bool is_null() const noexcept { return m_value == nullptr; }

		// Members of an object.
		void set(StringView key, StringView value) noexcept;
		void set(StringView key, const char* value) noexcept { set(key, StringView(value)); }
		void set(StringView key, bool value) noexcept;
		void set(StringView key, i32 value) noexcept;
		void set(StringView key, u32 value) noexcept;
		void set(StringView key, i64 value) noexcept;
		void set(StringView key, u64 value) noexcept;
		void set(StringView key, f32 value) noexcept; // written with float precision: 0.65f is "0.65"
		void set(StringView key, f64 value) noexcept;
		[[nodiscard]] JsonNode object(StringView key) noexcept; // a new, empty object member
		[[nodiscard]] JsonNode array(StringView key) noexcept;

		// Elements of an array.
		void push(StringView value) noexcept;
		void push(const char* value) noexcept { push(StringView(value)); }
		void push(bool value) noexcept;
		void push(i32 value) noexcept;
		void push(u32 value) noexcept;
		void push(i64 value) noexcept;
		void push(u64 value) noexcept;
		void push(f32 value) noexcept;
		void push(f64 value) noexcept;
		[[nodiscard]] JsonNode push_object() noexcept;
		[[nodiscard]] JsonNode push_array() noexcept;

	private:
		friend class JsonWriter;

		JsonNode(JsonWriter* writer, yyjson_mut_val* value) noexcept : m_writer(writer), m_value(value) {}

		[[nodiscard]] yyjson_mut_doc* doc() const noexcept; // null on a null node
		void put(StringView key, yyjson_mut_val* value) noexcept;
		void append(yyjson_mut_val* value) noexcept;

		JsonWriter* m_writer	= nullptr;
		yyjson_mut_val* m_value = nullptr;
	};

	/**
	 * Builds a JSON tree and writes it as text: the cooker's sidecars, an inspector's save-back,
	 * anything the engine hands to a file. Memory comes from the resource given at construction.
	 */
	class JsonWriter
	{
	public:
		explicit JsonWriter(std::pmr::memory_resource& memory) noexcept;
		~JsonWriter() noexcept;

		JsonWriter(const JsonWriter&)			 = delete;
		JsonWriter& operator=(const JsonWriter&) = delete;

		/// Starts the tree as an empty object or array and returns it. Replaces an earlier root.
		[[nodiscard]] JsonNode root_object() noexcept;
		[[nodiscard]] JsonNode root_array() noexcept;

		/**
		 * Starts the tree as `base` with `patch` applied by RFC 7386, JSON Merge Patch: an object
		 * in the patch merges into the object it meets, any other value replaces what was there,
		 * and null removes the key. A material file over its parent is exactly this.
		 */
		[[nodiscard]] JsonNode root_merged(JsonValue base, JsonValue patch) noexcept;

		/// The tree as text, two space indents, one trailing newline. Replaces `out`; false when
		/// there is no tree or a value cannot be written (a non-finite double).
		[[nodiscard]] bool write(String& out) const noexcept;

	private:
		friend class JsonNode;

		struct Impl;
		Impl* m_impl = nullptr;
	};
}
