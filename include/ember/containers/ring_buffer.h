#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>

#include <array>
#include <concepts>
#include <iterator>
#include <type_traits>
#include <utility>

namespace ember
{
	/**
	 * The last CAPACITY values of a numbered series, in fixed storage.
	 *
	 * Values are numbered in the order they were pushed, from 0, and the buffer holds the window
	 * [first_sequence, next_sequence): a push claims the next number and evicts the value CAPACITY
	 * behind it, a pop drops the oldest. A value is found by its number while it is inside the
	 * window, and iteration walks the window oldest to newest.
	 *
	 * Slots are constructed once, with the buffer, and live as long as it does. A push hands back
	 * the slot for the caller to fill and constructs and destroys nothing, so a value that owns
	 * memory keeps it across reuse: a frame's input snapshot keeps its text buffers. Values that
	 * cannot be default constructed are built by a generator, once per slot.
	 *
	 * Not thread safe.
	 */
	template <class T, u32 N> class RingBuffer final
	{
		static_assert(N != 0, "a ring needs at least one slot");

	public:
		static constexpr u32 CAPACITY = N;

		/// Every slot value initialized.
		constexpr RingBuffer() noexcept
			requires std::default_initializable<T>
			: m_slots{}
		{
		}

		/// Every slot built by make(slot), for values that take arguments, references included.
		/// The generator returns T by value and the value is built in place.
		template <class F>
			requires std::invocable<F&, u32> && std::same_as<std::invoke_result_t<F&, u32>, T>
		explicit constexpr RingBuffer(F&& make) noexcept : m_slots(build(make, std::make_index_sequence<N>{}))
		{
		}

		/**
		 * Claims the slot for the next number, evicting the value CAPACITY behind it, and hands it
		 * back holding whatever it held before, for the caller to fill.
		 */
		[[nodiscard]] constexpr T& push() noexcept
		{
			if (size() == N)
				++m_first;

			return m_slots[slot_of(m_next++)];
		}

		/// push() with the value assigned in.
		template <class U>
			requires std::assignable_from<T&, U&&>
		constexpr T& push(U&& value) noexcept
		{
			T& slot = push();
			slot	= std::forward<U>(value);
			return slot;
		}

		/// Drops the oldest value. Its slot keeps what it held until a push reuses it.
		constexpr void pop() noexcept
		{
			EMBER_ASSERT(!empty() && "pop on an empty ring");
			++m_first;
		}

		/// Forgets every value; numbering restarts at 0. Slots keep what they held.
		constexpr void clear() noexcept
		{
			m_first = 0;
			m_next	= 0;
		}

		[[nodiscard]] constexpr u32 size() const noexcept { return static_cast<u32>(m_next - m_first); }
		[[nodiscard]] constexpr bool empty() const noexcept { return m_next == m_first; }
		[[nodiscard]] constexpr bool full() const noexcept { return size() == N; }

		/// The window: the oldest value's number and the number the next push takes.
		[[nodiscard]] constexpr u64 first_sequence() const noexcept { return m_first; }
		[[nodiscard]] constexpr u64 next_sequence() const noexcept { return m_next; }

		/// By number; null outside the window.
		[[nodiscard]] constexpr T* find(u64 sequence) noexcept
		{
			return holds(sequence) ? &m_slots[slot_of(sequence)] : nullptr;
		}

		[[nodiscard]] constexpr const T* find(u64 sequence) const noexcept
		{
			return holds(sequence) ? &m_slots[slot_of(sequence)] : nullptr;
		}

		/// By age: 0 is the oldest value held, size() - 1 the newest.
		[[nodiscard]] constexpr T& operator[](u32 age) noexcept
		{
			EMBER_ASSERT(age < size());
			return m_slots[slot_of(m_first + age)];
		}

		[[nodiscard]] constexpr const T& operator[](u32 age) const noexcept
		{
			EMBER_ASSERT(age < size());
			return m_slots[slot_of(m_first + age)];
		}

		/// The oldest value held.
		[[nodiscard]] constexpr T& front() noexcept
		{
			EMBER_ASSERT(!empty());
			return m_slots[slot_of(m_first)];
		}

		[[nodiscard]] constexpr const T& front() const noexcept
		{
			EMBER_ASSERT(!empty());
			return m_slots[slot_of(m_first)];
		}

		/// The newest value held.
		[[nodiscard]] constexpr T& back() noexcept
		{
			EMBER_ASSERT(!empty());
			return m_slots[slot_of(m_next - 1)];
		}

		[[nodiscard]] constexpr const T& back() const noexcept
		{
			EMBER_ASSERT(!empty());
			return m_slots[slot_of(m_next - 1)];
		}

		/**
		 * The slots as laid out and where the oldest value sits in them, for a consumer that reads
		 * the raw array with a count and an offset (ImGui::PlotLines, with size() as the count).
		 * That walk is right while the buffer is full or has never popped; a pop leaves a gap the
		 * count cannot describe, so such a consumer only pushes.
		 */
		[[nodiscard]] constexpr Span<const T> storage() const noexcept { return {m_slots.data(), N}; }
		[[nodiscard]] constexpr u32 storage_offset() const noexcept { return slot_of(m_first); }

		/// Walks the window oldest to newest. sequence() names the value under the iterator.
		template <class Ring, class Ref> class Iterator
		{
		public:
			using iterator_category = std::forward_iterator_tag;
			using value_type		= std::remove_cv_t<T>;
			using difference_type	= std::ptrdiff_t;
			using pointer			= std::remove_reference_t<Ref>*;
			using reference			= Ref;

			constexpr Iterator() noexcept = default;
			constexpr Iterator(Ring& ring, u64 sequence) noexcept : m_ring(&ring), m_sequence(sequence) {}

			[[nodiscard]] constexpr reference operator*() const noexcept
			{
				return m_ring->m_slots[slot_of(m_sequence)];
			}
			[[nodiscard]] constexpr pointer operator->() const noexcept { return &**this; }
			[[nodiscard]] constexpr u64 sequence() const noexcept { return m_sequence; }

			constexpr Iterator& operator++() noexcept
			{
				++m_sequence;
				return *this;
			}

			constexpr Iterator operator++(int) noexcept
			{
				Iterator copy = *this;
				++m_sequence;
				return copy;
			}

			[[nodiscard]] constexpr bool operator==(const Iterator&) const noexcept = default;

		private:
			Ring* m_ring   = nullptr;
			u64 m_sequence = 0;
		};

		using iterator		 = Iterator<RingBuffer, T&>;
		using const_iterator = Iterator<const RingBuffer, const T&>;

		[[nodiscard]] constexpr iterator begin() noexcept { return {*this, m_first}; }
		[[nodiscard]] constexpr iterator end() noexcept { return {*this, m_next}; }
		[[nodiscard]] constexpr const_iterator begin() const noexcept { return {*this, m_first}; }
		[[nodiscard]] constexpr const_iterator end() const noexcept { return {*this, m_next}; }

	private:
		[[nodiscard]] static constexpr u32 slot_of(u64 sequence) noexcept { return static_cast<u32>(sequence % N); }

		[[nodiscard]] constexpr bool holds(u64 sequence) const noexcept
		{
			return sequence >= m_first && sequence < m_next;
		}

		template <class F, size_t... I>
		[[nodiscard]] static constexpr std::array<T, N> build(F& make, std::index_sequence<I...>) noexcept
		{
			return {{make(static_cast<u32>(I))...}};
		}

		std::array<T, N> m_slots;
		u64 m_first = 0; // the oldest value held
		u64 m_next	= 0; // the number the next push takes
	};

	namespace detail
	{
		// Evaluated by the compiler in every build; constant evaluation also proves the covered
		// paths free of undefined behaviour.
		constexpr bool ring_buffer_self_test()
		{
			RingBuffer<u32, 4> ring;
			bool ok = ring.empty() && ring.size() == 0 && ring.find(0) == nullptr;

			for (u32 i = 0; i < 6; ++i)
				ring.push(i * 10);

			ok = ok && ring.full() && ring.size() == 4 && ring.first_sequence() == 2 && ring.next_sequence() == 6;
			ok = ok && ring.find(1) == nullptr && *ring.find(2) == 20 && *ring.find(5) == 50 && ring.find(6) == nullptr;
			ok = ok && ring.front() == 20 && ring.back() == 50 && ring[1] == 30;

			u32 sum = 0;
			for (const u32 value : ring)
				sum += value;
			ok = ok && sum == 20 + 30 + 40 + 50;

			ring.pop();
			ok = ok && ring.size() == 3 && ring.front() == 30 && ring.storage_offset() == 3;

			ring.clear();
			ok = ok && ring.empty() && ring.next_sequence() == 0;
			return ok;
		}

		static_assert(ring_buffer_self_test(), "ember::RingBuffer self-test failed");
	}
}
