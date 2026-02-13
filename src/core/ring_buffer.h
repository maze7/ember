#pragma once

#include "core/common.h"
#include <array>
#include <iterator>
#include <concepts>

template<typename T, size_t Capacity>
class RingBuffer {
public:
    static_assert(Capacity > 0, "RingBuffer capacity must be > 0");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity should be power of 2");

    using value_type = T;
    static constexpr size_t capacity = Capacity;

    // ========================================
    // Iterator (oldest to newest)
    // ========================================
    template<bool IsConst>
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = T;
        using pointer = std::conditional_t<IsConst, const T*, T*>;
        using reference = std::conditional_t<IsConst, const T&, T&>;
        using buffer_ptr = std::conditional_t<IsConst, const RingBuffer*, RingBuffer*>;

        constexpr Iterator() = default;
        constexpr Iterator(buffer_ptr buf, size_t pos, size_t remaining)
            : m_buffer(buf), m_pos(pos), m_remaining(remaining) {}

        constexpr reference operator*() const noexcept {
            return m_buffer->m_buffer[m_pos];
        }

        constexpr pointer operator->() const noexcept {
            return &m_buffer->m_buffer[m_pos];
        }

        constexpr Iterator& operator++() noexcept {
            m_pos = (m_pos + 1) & (Capacity - 1);
            --m_remaining;
            return *this;
        }

        constexpr Iterator operator++(int) noexcept {
            Iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        [[nodiscard]] constexpr bool operator==(const Iterator& other) const noexcept {
            return m_remaining == other.m_remaining;
        }

        [[nodiscard]] constexpr bool operator!=(const Iterator& other) const noexcept {
            return !(*this == other);
        }

    private:
        buffer_ptr m_buffer = nullptr;
        size_t m_pos = 0;
        size_t m_remaining = 0;
    };

    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    // ========================================
    // Range support
    // ========================================
    [[nodiscard]] constexpr iterator begin() noexcept {
        size_t start = (m_write_index + Capacity - m_count) & (Capacity - 1);
        return iterator(this, start, m_count);
    }

    [[nodiscard]] constexpr iterator end() noexcept {
        return iterator(this, 0, 0);
    }

    [[nodiscard]] constexpr const_iterator begin() const noexcept {
        size_t start = (m_write_index + Capacity - m_count) & (Capacity - 1);
        return const_iterator(this, start, m_count);
    }

    [[nodiscard]] constexpr const_iterator end() const noexcept {
        return const_iterator(this, 0, 0);
    }

    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] constexpr const_iterator cend() const noexcept { return end(); }

    // ========================================
    // Reverse iteration (newest to oldest)
    // ========================================
    template<bool IsConst>
    class ReverseIterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = T;
        using pointer = std::conditional_t<IsConst, const T*, T*>;
        using reference = std::conditional_t<IsConst, const T&, T&>;
        using buffer_ptr = std::conditional_t<IsConst, const RingBuffer*, RingBuffer*>;

        constexpr ReverseIterator() = default;
        constexpr ReverseIterator(buffer_ptr buf, size_t offset, size_t remaining)
            : m_buffer(buf), m_offset(offset), m_remaining(remaining) {}

        constexpr reference operator*() const noexcept {
            size_t idx = (m_buffer->m_write_index + Capacity - 1 - m_offset) & (Capacity - 1);
            return m_buffer->m_buffer[idx];
        }

        constexpr pointer operator->() const noexcept {
            return &(**this);
        }

        constexpr ReverseIterator& operator++() noexcept {
            ++m_offset;
            --m_remaining;
            return *this;
        }

        constexpr ReverseIterator operator++(int) noexcept {
            ReverseIterator tmp = *this;
            ++(*this);
            return tmp;
        }

        [[nodiscard]] constexpr bool operator==(const ReverseIterator& other) const noexcept {
            return m_remaining == other.m_remaining;
        }

        [[nodiscard]] constexpr bool operator!=(const ReverseIterator& other) const noexcept {
            return !(*this == other);
        }

    private:
        buffer_ptr m_buffer = nullptr;
        size_t m_offset = 0;
        size_t m_remaining = 0;
    };

    using reverse_iterator = ReverseIterator<false>;
    using const_reverse_iterator = ReverseIterator<true>;

    // Reverse range wrapper
    struct ReverseRange {
        RingBuffer& buf;
        [[nodiscard]] constexpr reverse_iterator begin() noexcept {
            return reverse_iterator(&buf, 0, buf.m_count);
        }
        [[nodiscard]] constexpr reverse_iterator end() noexcept {
            return reverse_iterator(&buf, 0, 0);
        }
    };

    struct ConstReverseRange {
        const RingBuffer& buf;
        [[nodiscard]] constexpr const_reverse_iterator begin() const noexcept {
            return const_reverse_iterator(&buf, 0, buf.m_count);
        }
        [[nodiscard]] constexpr const_reverse_iterator end() const noexcept {
            return const_reverse_iterator(&buf, 0, 0);
        }
    };

    [[nodiscard]] constexpr ReverseRange reversed() noexcept { return {*this}; }
    [[nodiscard]] constexpr ConstReverseRange reversed() const noexcept { return {*this}; }

    // ========================================
    // Core operations
    // ========================================
    constexpr void push(const T& value) noexcept {
        m_buffer[m_write_index] = value;
        m_write_index = (m_write_index + 1) & (Capacity - 1);
        if (m_count < Capacity) ++m_count;
    }

    constexpr void push(T&& value) noexcept {
        m_buffer[m_write_index] = std::move(value);
        m_write_index = (m_write_index + 1) & (Capacity - 1);
        if (m_count < Capacity) ++m_count;
    }

    [[nodiscard]] constexpr T& operator[](size_t index) noexcept {
        return m_buffer[index & (Capacity - 1)];
    }

    [[nodiscard]] constexpr const T& operator[](size_t index) const noexcept {
        return m_buffer[index & (Capacity - 1)];
    }

    [[nodiscard]] constexpr T* back(size_t offset = 0) noexcept {
        if (offset >= m_count) return nullptr;
        size_t idx = (m_write_index + Capacity - 1 - offset) & (Capacity - 1);
        return &m_buffer[idx];
    }

    [[nodiscard]] constexpr const T* back(size_t offset = 0) const noexcept {
        if (offset >= m_count) return nullptr;
        size_t idx = (m_write_index + Capacity - 1 - offset) & (Capacity - 1);
        return &m_buffer[idx];
    }

    [[nodiscard]] constexpr T* front() noexcept {
        if (m_count == 0) return nullptr;
        size_t idx = (m_write_index + Capacity - m_count) & (Capacity - 1);
        return &m_buffer[idx];
    }

    [[nodiscard]] constexpr const T* front() const noexcept {
        if (m_count == 0) return nullptr;
        size_t idx = (m_write_index + Capacity - m_count) & (Capacity - 1);
        return &m_buffer[idx];
    }

    [[nodiscard]] constexpr size_t size() const noexcept { return m_count; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_count == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return m_count == Capacity; }

    constexpr void clear() noexcept {
        m_write_index = 0;
        m_count = 0;
    }

private:
    template<bool> friend class Iterator;
    template<bool> friend class ReverseIterator;

    std::array<T, Capacity> m_buffer{};
    size_t m_write_index = 0;
    size_t m_count = 0;
};
