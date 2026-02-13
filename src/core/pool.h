#pragma once

#include "common.h"
#include "handle.h"

namespace Ember
{
	/**
	 * @brief A high-performance associative container for storing objects with stable handles.
	 *
	 * A Pool provides O(1) insertion, erasure, and lookup while maintaining stable handles
	 * (keys) to its elements, even when other elements are removed. Iteration is cache-friendly
	 * as all elements are stored contiguously in memory.
	 *
	 * This makes it ideal for situations where you need to store objects like entities, resources,
	 * or particles and refer to them by a safe, persistent identifier.
	 *
	 * @tparam T The type of elements to store. Must be movable.
	 * @tparam KeyT The type forwarded to the Handle<T>, defaults to T.
	 * @tparam KeyIndexType The type forwarded to the Handle for index/generation, determines the bit size of a Handle.
	 */
	template<class T, class TKey, class KeyIndexType = u32>
	class Pool
	{
	public:
		// Standard container typedefs
		using handle_type = Handle<TKey, KeyIndexType>;

		// Typedefs for data
		using value_type = T;
		using size_type = std::vector<T>::size_type;
		using difference_type = std::vector<T>::difference_type;
		using reference = std::vector<T>::reference;
		using const_reference = std::vector<T>::const_reference;
		using pointer = std::vector<T>::pointer;
		using const_pointer = std::vector<T>::const_pointer;
		using iterator = std::vector<T>::iterator;
		using const_iterator = std::vector<T>::const_iterator;

		/**
		 * @brief Returns the number of elements in the container.
		 * @return The number of elements.
		 */
		[[nodiscard]] size_type size() const noexcept {
			return m_count;
		}

		/**
		 * @brief Returns the maximium number of elements the container can hold.
		 * @return Maximum number of elements.
		 */
		[[nodiscard]] size_type max_size() const noexcept {
			return m_data.max_size();
		}

		/**
		 * @brief Returns the number of elements that can be held in currently allocated storage.
		 * @return The capacity of the currently allocated storage.
		 */
		[[nodiscard]] size_type capacity() const noexcept {
			return m_data.capacity();
		}

		/**
		 * @brief Inserts an element into the container by moving it.
		 * @param value The value to insert
		 * @return The key (Handle) to the newly inserted element.
		 */
		[[nodiscard]] handle_type insert(T&& value) {
			const auto data_index = static_cast<KeyIndexType>(m_data.size());
			KeyIndexType slot_index;

			if (m_freelist_head != FREELIST_SENTINEL) {
				slot_index = m_freelist_head;
				m_freelist_head = m_slots[slot_index].get_next_free();
			} else {
				slot_index = static_cast<KeyIndexType>(m_slots.size());
				m_slots.emplace_back();
			}

			m_slots[slot_index].generation++;
			m_slots[slot_index].set_data_index(data_index);

			m_data.emplace_back(std::move(value));
			m_indices.push_back(slot_index);
			m_count++;

			return handle_type{
				.index = slot_index,
				.generation = m_slots[slot_index].generation,
			};
		}

		/**
		 * @brief Inserts an element into the container by copying it.
		 * @param value The alue to insert.
		 * @return The key (Handle) to the newly inserted element.
		 */
		[[nodiscard]] handle_type insert(const T& value) {
			const auto data_index = static_cast<KeyIndexType>(m_data.size());
			KeyIndexType slot_index;

			if (m_freelist_head != FREELIST_SENTINEL) {
				slot_index = m_freelist_head;
				m_freelist_head = m_slots[slot_index].get_next_free();
			} else {
				slot_index = static_cast<KeyIndexType>(m_slots.size());
				m_slots.emplace_back();
			}

			m_slots[slot_index].generation++;
			m_slots[slot_index].set_data_index(data_index);

			m_data.emplace_back(value);
			m_indices.push_back(slot_index);
			m_count++;

			return handle_type{
				.index = slot_index,
				.generation = m_slots[slot_index].generation,
			};
		}

		/**
		 * @brief Constructs an element with arguments.
		 * @param args Arguments for data construction
		 * @return The key (Handle) to the newly constructed element.
		 */
		template <class... Args>
		[[nodiscard]] handle_type emplace(Args... args) {
			const auto data_index = static_cast<KeyIndexType>(m_data.size());
			KeyIndexType slot_index;

			if (m_freelist_head != FREELIST_SENTINEL) {
				slot_index = m_freelist_head;
				m_freelist_head = m_slots[slot_index].get_next_free();
			} else {
				slot_index = static_cast<KeyIndexType>(m_slots.size());
				m_slots.emplace_back();
			}

			m_slots[slot_index].generation++;
			m_slots[slot_index].set_data_index(data_index);
			m_data.emplace_back(std::forward<decltype(args)>(args)...);
			m_indices.push_back(slot_index);
			m_count++;

			return handle_type{
				.index = slot_index,
				.generation = m_slots[slot_index].generation,
			};
		}

		/**
		 * @brief Erases an element from the container.
		 * @param key The key (Handle) of the element to erase.
		 * @return True if the element was erased, false if the key was invalid.
		 */
		bool erase(handle_type key) {
			if (!contains(key)) {
				return false;
			}

			const KeyIndexType slot_index_to_erase = key.index;
			Slot& slot_to_erase = m_slots[slot_index_to_erase];
			const KeyIndexType data_index_to_erase = slot_to_erase.get_data_index();

			// Swap-and-pop data
			m_data[data_index_to_erase] = std::move(m_data.back());
			m_data.pop_back();

			// Update the index for the element that was moved
			const auto last_data_index = static_cast<KeyIndexType>(m_data.size());
			if (data_index_to_erase != last_data_index) {
				const KeyIndexType slot_index_of_moved_element = m_indices.back();
				m_indices[data_index_to_erase] = slot_index_of_moved_element;
				m_slots[slot_index_of_moved_element].set_data_index(data_index_to_erase);
			}
			m_indices.pop_back();

			// Add the now-erased slot to the front of the free list
			slot_to_erase.set_next_free(m_freelist_head);
			m_freelist_head = slot_index_to_erase;

			m_count--;
			return true;
		}

		/**
		 * @brief Clears the container, removing all elements.
		 */
		void clear() noexcept {
			m_data.clear();
			m_slots.clear();
			m_indices.clear();
			m_freelist_head = FREELIST_SENTINEL;
			m_count = 0;
		}

		/**
		 * @brief Checks if a key is valid and points to an active element.
		 * @param key The key to check.
		 * @return True if the key is valid, false otherwise.
		 */
		[[nodiscard]] bool contains(handle_type key) const noexcept{
		return (key.index) < m_slots.size() &&
				m_slots[key.index].generation == key.generation &&
				m_slots[key.index].is_occupied();
		}

		/**
		 * @brief Retrieves a pointer to a element using its key.
		 * @param key The key of the element.
		 * @return A pointer to the element if the key is valid, nullptr otherwise.
		 */
		[[nodiscard]] T* get(handle_type key) noexcept {
			if (!contains(key)) {
				return nullptr;
			}

			return &m_data[m_slots[key.index].get_data_index()];
		}

		/**
		 * @brief Retrieves a const pointer to an element using its key.
		 * @param key The key of the element.
		 * @return A const pointer to the element if the key is valid, nullptr otherwise.
		 */
		[[nodiscard]] const T* get(handle_type key) const noexcept {
			if (!contains(key)) {
				return nullptr;
			}

			return &m_data[m_slots[key.index].get_data_index()];
		}

		/**
		 * @brief Accesses an element using its key (with bounds checking).
		 * @param key The key of the element.
		 * @return A reference to the element.
		 * @throws std::out_of_range if the key is invalid.
		 */
		T& at(handle_type key) {
			if (T* ptr = get(key)) {
				return *ptr;
			}

			throw std::out_of_range("Invalid slot_map key");
		}

		/**
		 * @brief Accesses an element using its key (with bounds checking).
		 * @param key The key of the element.
		 * @return A const reference to the element.
		 * @throws std::out_of_range if the key is invalid.
		 */
		const T& at(handle_type key) const {
			if (T* ptr = get(key)) {
				return *ptr;
			}

			throw std::out_of_range("Invalid slot_map key");
		}

		/**
		 * @brief Accesses an element using its key (no bounds checking).
		 * @param key The key of the element.
		 * @return A reference to the element.
		 * @warning Undefined behaviour if the key is invalid.
		 */
		T& operator[](handle_type key) {
			EMBER_ASSERT(contains(key) && "Invalid slot_map key");
			return m_data[m_slots[key.index].get_data_index()];
		}

		/**
		 * @brief Accesses a element using its key (no bounds checking).
		 * @param key The key of the element.
		 * @return A reference to the element.
		 * @warning Undefined behaviour if the key is invalid.
		 */
		const T& operator[](handle_type key) const {
			EMBER_ASSERT(contains(key) && "Invalid slot_map key");
			return m_data[m_slots[key.index].get_data_index()];
		}

		/**
		 * @brief Returns an iterator to the beginning of the element sequence.
		 * @return Iterator to the first element.
		 */
		iterator begin() noexcept { return m_data.begin(); }
		const_iterator begin() const noexcept { return m_data.begin(); }
		const_iterator cbegin() const noexcept { return m_data.cbegin(); }

		/**
		 * @brief Returns an iterator to the end of the element sequence.
		 * @return Iterator to the element following the last element.
		 */
		iterator end() noexcept { return m_data.end(); }
		const_iterator end() const noexcept { return m_data.end(); }
		const_iterator cend() const noexcept { return m_data.cend(); }

	private:
		// A sentinel value to mark the end of the free list.
		static constexpr u32 FREELIST_SENTINEL = std::numeric_limits<u32>::max();
		// The most significant bit is used to tag a slot as free.
		static constexpr KeyIndexType FREELIST_TAG = KeyIndexType(1) << (sizeof(KeyIndexType) * 8 - 1);

		// Each slot holds either an index to the data array (if occupied) or an index to the next free slot (if free).
		struct Slot
		{
			KeyIndexType generation{0};
			KeyIndexType packed_state{0}; // Holds either data_index OR (next_free | FREE_LIST_TAG)

			[[nodiscard]] bool is_occupied() const noexcept {
				return (packed_state & FREELIST_TAG) == 0;
			}

			[[nodiscard]] KeyIndexType get_data_index() const noexcept {
				EMBER_ASSERT(is_occupied() && "Attempted to get data_index from a free slot");
				return packed_state;
			}

			[[nodiscard]] KeyIndexType get_next_free() const noexcept {
				EMBER_ASSERT(!is_occupied() && "Attempted to get next_free from an occupied slot");
				return packed_state & ~FREELIST_TAG;
			}

			void set_data_index(KeyIndexType index) noexcept {
				EMBER_ASSERT((index & FREELIST_TAG) == 0 && "Index is too large and collides with the tag bit");
				packed_state = index;
			}

			void set_next_free(KeyIndexType index) noexcept {
				packed_state = index | FREELIST_TAG;
			}
		};

		// Contiguous storage for all objects.
		std::vector<Slot> m_slots;
		// Densely packed data array
		std::vector<T> m_data;
		// Maps data index back to slot index.
		std::vector<u32> m_indices;
		// Head of the embedded free list.
		u32 m_freelist_head{FREELIST_SENTINEL};
		// Number of active elements.
		u32 m_count{0};
	};
}

/**
 * These articles were referenced when implementing this data structure:
 *
 * Niklas Gray
 * Building a Data-Oriented Entity System (part 1), 2014
 * http://bitsquid.blogspot.com/2014/08/building-data-oriented-entity-system.html
 *
 *
 * Noel Llopis
 * Managing Data Relationships, 2010
 * https://gamesfromwithin.com/managing-data-relationships
 *
 * Stefan Reinalter
 * Adventures in data-oriented design - Part 3c: External References, 2013
 * https://blog.molecular-matters.com/2013/07/24/adventures-in-data-oriented-design-part-3c-external-references/
 *
 * Niklas Gray
 * Managing Decoupling Part 4 - The ID Lookup Table, 2011
 * https://bitsquid.blogspot.com/2011/09/managing-decoupling-part-4-id-lookup.html
 *
 * Sander Mertens
 * Making the most of ECS identifiers, 2020
 * https://ajmmertens.medium.com/doing-a-lot-with-a-little-ecs-identifiers-25a72bd2647
 *
 * Michele Caini
 * ECS back and forth. Part 9 - Sparse sets and EnTT, 2020
 * https://skypjack.github.io/2020-08-02-ecs-baf-part-9/
 *
 * Andre Weissflog
 * Handles are the better pointers, 2018
 * https://floooh.github.io/2018/06/17/handles-vs-pointers.html
 *
 * Allan Deutsch
 * C++Now 2017: "The Slot Map Data Structure", 2017
 * https://www.youtube.com/watch?v=SHaAR7XPtNU
 *
 * Jeff Gates
 * Init, Update, Draw - Data Arrays, 2012
 * https://greysphere.tumblr.com/post/31601463396/data-arrays
 */
