#pragma once

#include <ember/core/common.h>

#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <type_traits>

/**
 * EMBER_LIFETIMEBOUND
 *
 * Marks a constructor parameter whose referent must outlive the constructed object.
 * Clang (-Wdangling) and MSVC (17.2+, code analysis) diagnose violations at compile time:
 *
 *     Span<const u32> s = {1, 2, 3};   // warning: temporary array dies at end of statement
 *
 * This turns the one dangerous pattern Span permits (binding an initializer list to a
 * long-lived local) into a compiler diagnostic instead of silent use-after-free.
 */
#ifndef EMBER_LIFETIMEBOUND
	#if defined(__clang__)
		#define EMBER_LIFETIMEBOUND [[clang::lifetimebound]]
	#elif defined(_MSC_VER) && _MSC_VER >= 1932
		#define EMBER_LIFETIMEBOUND [[msvc::lifetimebound]]
	#else
		#define EMBER_LIFETIMEBOUND
	#endif
#endif

namespace ember
{
	template <typename T> class Span;

	namespace detail
	{
		template <typename T> inline constexpr bool is_span_v		   = false;
		template <typename T> inline constexpr bool is_span_v<Span<T>> = true;

		/**
		 * Element compatibility rule (same as std::span): From* must convert to To* as
		 * pointers-to-array. This admits exactly the safe qualification conversions
		 * (T -> const T) and rejects everything that would break pointer arithmetic,
		 * e.g. Derived -> Base (different object stride) or unrelated types.
		 */
		template <typename From, typename To>
		concept SpanElementCompatible = std::is_convertible_v<From (*)[], To (*)[]>;

		/**
		 * Anything with data()/size() members and a compatible element type: Vector,
		 * std::array, String, std::span, custom engine containers, ...
		 *
		 * Member-based detection is deliberate: it avoids pulling in <ranges>, which costs
		 * real compile time in a header this widely included, and every contiguous container
		 * we care about exposes these members anyway.
		 *
		 * Raw arrays and Spans are excluded; dedicated constructors handle those.
		 */
		template <typename C, typename T>
		concept SpanCompatibleContainer =
			!is_span_v<std::remove_cvref_t<C>> && !std::is_array_v<std::remove_cvref_t<C>> &&
			requires(C& c) {
				requires std::is_pointer_v<decltype(c.data())>;
				requires std::is_convertible_v<decltype(c.size()), size_t>;
				requires SpanElementCompatible<std::remove_pointer_t<decltype(c.data())>, T>;
			};
	}

	/**
	 * Non-owning view of a contiguous array (pointer + count). Ember's replacement for
	 * std::span, adding the one thing std::span lacks before C++26: construction from a
	 * braced initializer list.
	 *
	 * It makes C++20 designated-initializer resource descriptors carry array data with
	 * zero heap allocations and zero copies:
	 *
	 *     device.create_buffer({
	 *         .debug_name   = "quad_ib",
	 *         .initial_data = {0u, 1u, 2u, 2u, 1u, 3u},   // Span<const u32>
	 *     });
	 *
	 * Deliberate differences from std::span:
	 *   - Braced initializer lists construct Span<const T> (backed by a stack temporary).
	 *   - Dynamic extent only
	 *   - Element access is bounds-checked with EMBER_ASSERT (compiles out in release).
	 *   - Iterators are raw pointers: no debug-iterator overhead in draw-loop code.
	 *
	 * OWNERSHIP & LIFETIME
	 *   A Span never owns memory and never extends a lifetime. Treat it strictly as a
	 *   parameter/return view: consume it before the underlying storage dies. A Span built
	 *   from an initializer list is valid only for the current full-expression, which is
	 *   why resource-creation APIs take descriptors as `const Def&&`: a const rvalue
	 *   reference binds only to temporaries, so a descriptor (and the initializer-list
	 *   arrays its Spans reference) cannot be stashed in a named variable and passed later.
	 *   Never store a Span in a long-lived object; copy the data or store a handle instead.
	 */
	template <typename T> class Span
	{
	public:
		using element_type	   = T;
		using value_type	   = std::remove_cv_t<T>;
		using size_type		   = size_t;
		using difference_type  = ptrdiff_t;
		using pointer		   = T*;
		using reference		   = T&;
		using iterator		   = T*;
		using reverse_iterator = std::reverse_iterator<iterator>;

		/// Empty view: data() == nullptr, size() == 0. (`.field = {}` in a descriptor.)
		constexpr Span() noexcept = default;

		/// From pointer + element count. (ptr, 0) is a valid empty view.
		EMBER_FINLINE constexpr Span(pointer data, size_type count) noexcept : m_data(data), m_size(count)
		{
			EMBER_ASSERT(data != nullptr || count == 0);
		}

		/// From a [first, last) pointer pair.
		EMBER_FINLINE constexpr Span(pointer first, pointer last) noexcept
			: m_data(first), m_size(static_cast<size_type>(last - first))
		{
			EMBER_ASSERT(first <= last);
		}

		/// From a C array; the count is deduced at compile time.
		/// Note: for string literals this includes the terminating '\0'.
		template <size_t N>
		EMBER_FINLINE constexpr Span(T (&array)[N]) noexcept : m_data(array), m_size(N)
		{
		}

		/// From any contiguous container with data()/size() (Vector, std::array, String, ...).
		template <typename Container>
			requires(detail::SpanCompatibleContainer<Container, T> &&
					 (std::is_lvalue_reference_v<Container> || std::is_const_v<T>))
		EMBER_FINLINE constexpr Span(Container&& container EMBER_LIFETIMEBOUND) //
			noexcept(noexcept(container.data()) && noexcept(container.size()))
			: m_data(container.data()), m_size(static_cast<size_type>(container.size()))
		{
		}

		/// From an initializer list
		EMBER_FINLINE constexpr Span(std::initializer_list<value_type> list EMBER_LIFETIMEBOUND) noexcept
			requires(std::is_const_v<T>)
			: Span(list.begin(), list.size())
		{
		}

		/// Span<T> -> Span<const T>. The concept permits only qualification conversions.
		template <typename U>
			requires(detail::SpanElementCompatible<U, T>)
		EMBER_FINLINE constexpr Span(Span<U> other) noexcept : m_data(other.data()), m_size(other.size())
		{
		}

		// Spans are cheap POD values; copying is trivial and intended.
		constexpr Span(const Span&) noexcept			= default;
		constexpr Span& operator=(const Span&) noexcept = default;

		[[nodiscard]] EMBER_FINLINE constexpr pointer data() const noexcept { return m_data; }
		[[nodiscard]] EMBER_FINLINE constexpr size_type size() const noexcept { return m_size; }
		[[nodiscard]] EMBER_FINLINE constexpr size_type size_bytes() const noexcept { return m_size * sizeof(T); }
		[[nodiscard]] EMBER_FINLINE constexpr bool empty() const noexcept { return m_size == 0; }

		[[nodiscard]] EMBER_FINLINE constexpr reference operator[](size_type index) const noexcept
		{
			EMBER_ASSERT(index < m_size);
			return m_data[index];
		}

		[[nodiscard]] EMBER_FINLINE constexpr reference front() const noexcept
		{
			EMBER_ASSERT(m_size != 0);
			return m_data[0];
		}

		[[nodiscard]] EMBER_FINLINE constexpr reference back() const noexcept
		{
			EMBER_ASSERT(m_size != 0);
			return m_data[m_size - 1];
		}

		[[nodiscard]] EMBER_FINLINE constexpr iterator begin() const noexcept { return m_data; }
		[[nodiscard]] EMBER_FINLINE constexpr iterator end() const noexcept { return m_data + m_size; }

		[[nodiscard]] EMBER_FINLINE constexpr reverse_iterator rbegin() const noexcept
		{
			return reverse_iterator(end());
		}

		[[nodiscard]] EMBER_FINLINE constexpr reverse_iterator rend() const noexcept
		{
			return reverse_iterator(begin());
		}

		/// First `count` elements.
		[[nodiscard]] EMBER_FINLINE constexpr Span first(size_type count) const noexcept
		{
			EMBER_ASSERT(count <= m_size);
			return Span(m_data, count);
		}

		/// Last `count` elements.
		[[nodiscard]] EMBER_FINLINE constexpr Span last(size_type count) const noexcept
		{
			EMBER_ASSERT(count <= m_size);
			return Span(m_data + (m_size - count), count);
		}

		/// Elements [offset, offset + count).
		[[nodiscard]] EMBER_FINLINE constexpr Span subspan(size_type offset, size_type count) const noexcept
		{
			EMBER_ASSERT(offset <= m_size);
			EMBER_ASSERT(count <= m_size - offset); // overflow-safe form of `offset + count <= size`
			return Span(m_data + offset, count);
		}

		/// Elements [offset, size()).
		[[nodiscard]] EMBER_FINLINE constexpr Span subspan(size_type offset) const noexcept
		{
			EMBER_ASSERT(offset <= m_size);
			return Span(m_data + offset, m_size - offset);
		}

		// No operator==, same as std::span: equality on views is ambiguous (same memory vs
		// same contents). Compare data()/size() or use std::ranges::equal explicitly.

	private:
		pointer m_data	 = nullptr;
		size_type m_size = 0;
	};

	template <typename T, size_t N> Span(T (&)[N]) -> Span<T>;

	/// `Span{1, 2, 3}` deduces Span<const int>.
	template <typename T> Span(std::initializer_list<T>) -> Span<const T>;

	/// Prefer parentheses when deducing from containers: `Span(vec)`, not `Span{vec}`.
	/// Braces route through the initializer-list guide (same pitfall as std::vector).
	template <typename Container>
		requires(requires(Container& c) {
			c.data();
			c.size();
		})
	Span(Container&&) -> Span<std::remove_pointer_t<decltype(std::declval<Container&>().data())>>;

	template <typename T> [[nodiscard]] EMBER_FINLINE Span<const std::byte> as_bytes(Span<T> s) noexcept
	{
		return Span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size_bytes());
	}

	template <typename T>
		requires(!std::is_const_v<T>)
	[[nodiscard]] EMBER_FINLINE Span<std::byte> as_writable_bytes(Span<T> s) noexcept
	{
		return Span<std::byte>(reinterpret_cast<std::byte*>(s.data()), s.size_bytes());
	}

	namespace detail
	{
		// Layout / ABI: two registers, memcpy-safe, no destructor. Draw-loop critical.
		static_assert(std::is_trivially_copyable_v<Span<const u32>>);
		static_assert(std::is_trivially_destructible_v<Span<const u32>>);
		static_assert(sizeof(Span<u32>) == sizeof(void*) + sizeof(size_t));

		// Const-correctness: widening is implicit, shedding const is impossible, and
		// initializer lists only ever produce const views.
		static_assert(std::is_convertible_v<Span<u32>, Span<const u32>>);
		static_assert(!std::is_constructible_v<Span<u32>, Span<const u32>>);
		static_assert(!std::is_constructible_v<Span<u32>, std::initializer_list<u32>>);

		// Evaluated by the compiler in every build. Constant evaluation also proves the
		// covered paths are UB-free: out-of-bounds access or reading a dead temporary
		// would fail the static_assert.
		constexpr bool span_self_test()
		{
			constexpr u32 values[] = {1, 2, 3, 4};

			const Span<const u32> s = values;

			bool ok = s.size() == 4 && !s.empty() && s.front() == 1 && s.back() == 4 && s[2] == 3;
			ok		= ok && s.first(2).back() == 2 && s.last(3).front() == 2;
			ok		= ok && s.subspan(1, 2).front() == 2 && s.subspan(2).size() == 2;
			ok		= ok && Span<const u32>{}.empty();

			// Initializer-list Spans must be consumed inside the full-expression that
			// created them; the lambda parameter models the descriptor-argument pattern.
			// Binding one to a local and reading it on the next line would (correctly)
			// fail constant evaluation.
			const auto sum = [](Span<const u32> list)
			{
				u32 total = 0;
				for (const u32 v : list)
					total += v;
				return total;
			};
			ok = ok && sum({10, 20, 30}) == 60;

			return ok;
		}

		static_assert(span_self_test(), "ember::Span self-test failed");
	}
}
