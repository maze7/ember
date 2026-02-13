#pragma once

#include "core/hash.h"
#include "graphics/enums/vertex_type.h"
#include <numeric>
#include <span>

namespace Ember
{
	class VertexFormat
	{
	public:
		// A struct describing a single element within the vertex layout.
		struct Element
		{
			int index = 0;
			VertexType type = VertexType::None;
			bool normalized = true;

			// Auto-generate all comparison operators.
			auto operator<=>(const Element&) const = default;
		};

		// Once constructed, a VertexFormat is immutable.
		std::vector<Element> elements;
		u32 stride;

		VertexFormat() = default;

		/**
		 * @brief Constructs a VertexFormat from a list of elements and an optional stride.
		 * @param elements A span of elements describing the vertex layout.
		 * @param stride Optionall stried. If not provided, it's calculated from the elements.
		 */
		VertexFormat(std::span<const Element> elements_, std::optional<u32> stride_override = std::nullopt)
			: elements(elements_.begin(), elements_.end())
			, stride(stride_override.value_or(calculate_stride(elements))) {}

		/**
		 * @brief Constructs a VertexFormat from a list of elements and an optional stride.
		 * @param elements A span of elements describing the vertex layout.
		 * @param stride Optionall stried. If not provided, it's calculated from the elements.
		 */
		VertexFormat(std::initializer_list<const Element> elements_, std::optional<u32> stride_override = std::nullopt)
			: elements(elements_.begin(), elements_.end())
			, stride(stride_override.value_or(calculate_stride(elements))) {}

		/**
		 * @brief A factory to create a VertexFormat from a C++ Vertex struct.
		 * The stride is automatically determined by sizeof(TVertex).
		 * @tparam TVertex The C++ struct representing a single vertex.
		 * @param elements An initializer list describing the elements.
		 * @return A new VertexFormat instance.
		 */
		template <typename TVertex>
		static VertexFormat create(std::initializer_list<Element> elements) {
			static_assert(std::is_standard_layout_v<TVertex>, "Vertex type must have a standard memory layout.");
			return VertexFormat(elements, sizeof(TVertex));
		}

		// The compiler generates a perfect member-wise equality comparison.
		bool operator==(const VertexFormat&) const = default;

		VertexFormat& operator=(const VertexFormat& other) = default;

	private:
		// Helper to calculate the stride if one isn't provided by the user.
		static u32 calculate_stride(const std::vector<Element> elems) {
			return std::accumulate(elems.begin(), elems.end(), 0u,
				[](u32 sum, const Element& el) {
					return sum + vertex_type_size(el.type);
				}
			);
		}
	};
}

// This allows VertexFormat to be used as a key in std::unordered_map
namespace std
{
	template<>
	struct hash<Ember::VertexFormat>
	{
		size_t operator()(const Ember::VertexFormat& vf) const noexcept {
			auto hash = Ember::combined_hash(vf.stride);
			for (const auto& el : vf.elements) {
				hash = Ember::combined_hash(hash, el.index, el.type, el.normalized);
			}

			return hash;
		}
	};
}
