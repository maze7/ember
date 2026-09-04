#pragma once

#include <ember/core/bitmask.h>
#include <ember/core/common.h>
#include <ember/core/handle.h>

namespace ember::render
{
	/**
	 * Renderer domain handles.
	 *
	 * Same weak-reference scheme as the GPU layer: a handle's index is also the
	 * object's slot in every GPU scene table, so shaders and CPU code agree on
	 * identity without a remap. The tag structs stay incomplete; only the owning
	 * pools can dereference one.
	 *
	 * Objects use a u32 component because scenes outgrow the u16 index space.
	 * Geometry and materials are asset-scale and stay at u16.
	 */
	struct RenderObject;
	struct Geometry;
	struct Material;

	using RenderObjectHandle = Handle<RenderObject, u32>;
	using GeometryHandle	 = Handle<Geometry, u16>;
	using MaterialHandle	 = Handle<Material, u16>;

	/**
	 * Bit i marks membership of layer i. Views select layers, objects belong to them;
	 * a view draws an object when the masks intersect. Zero means the object is invisible
	 * everywhere, which is how destroyed slots read on the GPU.
	 */
	using LayerMask = u32;

	inline constexpr LayerMask LAYER_DEFAULT = 1u;
	inline constexpr LayerMask LAYER_ALL	 = ~0u;

	enum class ObjectFlags : u32
	{
		None		= 0,
		CastsShadow = 1u << 0,
		Billboard	= 1u << 1, // vertex shader rebuilds rotation from the view basis
		AlphaTest	= 1u << 2, // routes to the cutout visibility bucket for the sprite family
	};

	EMBER_ENUM_BITWISE_OPS(ObjectFlags, u32);
}
