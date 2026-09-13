#pragma once

#include <ember/core/bits.h>
#include <ember/core/common.h>
#include <ember/memory/memory.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace ember
{
	/**
	 * Bounded multi-producer multi-consumer queue after Dmitry Vyukov.
	 *
	 * Every cell carries a sequence number that says whose turn it is. A producer holding
	 * ticket t may fill cell t & mask once its sequence equals t; filling it publishes
	 * sequence t + 1, which is what the consumer holding ticket  t waits for; consuming
	 * publishes t + capacity for the producer of the  next lap. Tickets are claimed with a
	 * compare-exchange on the enqueue or dequeue positon, so no two threads ever touch one
	 * cell in the same role, and values are published through the sequence alone: a release
	 * sotre on one side, an acquire load on the other. No lock, no allocation after init,
	 * no ABA problem, because a ticket is never reused within a lap.
	 *
	 * try_push reports full and try_pop reports empty as of that instant, and both can also
	 * fail while another thread sits between claiming a ticket and publishing its cell: a
	 * producer mid push makes the queue look empty at its cell, and a consumer mid pop makes
	 * it look full at that cell one lap later, whatever the count. A caller returning
	 * something it owns retries, since the other side always finishes; a caller deciding
	 * full or empty compares size_hint against capacity first.
	 *
	 * T is trivially copyable and default constructible: values move as plain copies under
	 * the protocol. Capacity is a power of two. The memory resource must outlive the queue.
	 */
	template <typename T> class MpmcQueue
	{
		static_assert(std::is_trivially_copyable_v<T>, "MpmcQueue values are copied as bytes");
		static_assert(std::is_default_constructible_v<T>, "MpmcQueue cells are value initialised");

	public:
		explicit MpmcQueue(MemoryTag tag = MemoryTag::Unknown) noexcept : m_resource(&memory::heap(tag)) {}
		explicit MpmcQueue(std::pmr::memory_resource& resource) noexcept : m_resource(&resource) {}
		~MpmcQueue() noexcept;

		MpmcQueue(const MpmcQueue&) = delete;
		MpmcQueue& operator=(const MpmcQueue&) = delete;

		/// Allocates the cells. Runs once; capacity is a power of two of at least 2.
		void init(size_t capacity) noexcept;

		[[nodiscard]] bool try_push(const T& value) noexcept;
		[[nodiscard]] bool try_pop(T& out) noexcept;

		[[nodiscard]] size_t capacity() const noexcept { return m_capacity; }

		/// Distance between the two positions at the moment of the call. Claimed but
		/// unpublished cells count, so this is a hint for stats and idle decisions.
		[[nodiscard]] size_t size_hint() const noexcept;

	private:
		struct Cell
		{
			std::atomic<size_t> sequence;
			T value;
		};

		std::pmr::memory_resource* m_resource = nullptr;
		Cell* m_cells = nullptr;
		size_t m_mask = 0;
		size_t m_capacity = 0;

		alignas(EMBER_CACHE_LINE) std::atomic<size_t> m_enqueue{0};
		alignas(EMBER_CACHE_LINE) std::atomic<size_t> m_dequeue{0};
	};

	template <typename T> MpmcQueue<T>::~MpmcQueue() noexcept
	{
		if (m_cells != nullptr)
			m_resource->deallocate(m_cells, m_capacity * sizeof(Cell), EMBER_CACHE_LINE);
	}

	template <typename T> void MpmcQueue<T>::init(size_t capacity) noexcept
	{
		EMBER_ASSERT(m_cells == nullptr && "init runs once");
		EMBER_ASSERT(capacity >= 2 && is_power_of_two(capacity));

		m_cells = static_cast<Cell*>(m_resource->allocate(capacity * sizeof(Cell), EMBER_CACHE_LINE));
		m_mask = capacity - 1;
		m_capacity = capacity;

		for (size_t index = 0; index < capacity; ++index)
		{
			Cell* cell = std::construct_at(m_cells + index);
			cell->sequence.store(index, std::memory_order_relaxed);
		}

		m_enqueue.store(0, std::memory_order_relaxed);
		m_dequeue.store(0, std::memory_order_relaxed);
	}

	template <typename T> bool MpmcQueue<T>::try_push(const T& value) noexcept
	{
		Cell* cell = nullptr;
		size_t pos = m_enqueue.load(std::memory_order_relaxed);

		for (;;)
		{
			cell = m_cells + (pos & m_mask);

			const size_t sequence = cell->sequence.load(std::memory_order_acquire);
			const intptr_t diff = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(pos);

			if (diff == 0)
			{
				if (m_enqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
					break;
			}
			else if (diff < 0)
				return false;
			else
				pos = m_enqueue.load(std::memory_order_relaxed);
		}

		cell->value = value;
		cell->sequence.store(pos + 1, std::memory_order_release);
		return true;
	}

	template <typename T> bool MpmcQueue<T>::try_pop(T& out) noexcept
	{
		Cell* cell = nullptr;
		size_t pos = m_dequeue.load(std::memory_order_relaxed);

		for (;;)
		{
			cell = m_cells + (pos & m_mask);

			const size_t sequence = cell->sequence.load(std::memory_order_acquire);
			const intptr_t diff = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(pos + 1);

			if (diff == 0)
			{
				if (m_dequeue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
					break;
			}
			else if (diff < 0)
				return false;
			else
				pos = m_dequeue.load(std::memory_order_relaxed);
		}

		out = cell->value;
		cell->sequence.store(pos + m_capacity, std::memory_order_release);
		return true;
	}

	template <typename T> size_t MpmcQueue<T>::size_hint() const noexcept
	{
		const size_t enqueue = m_enqueue.load(std::memory_order_relaxed);
		const size_t dequeue = m_dequeue.load(std::memory_order_relaxed);
		return enqueue >= dequeue ? enqueue - dequeue : 0;
	}
}
