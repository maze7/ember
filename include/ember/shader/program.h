#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/pipeline.h>
#include <ember/gpu/sampler.h>

namespace ember::shader
{
	/**
	 * What kind of file the program came from. A plain shader carries its own entry
	 * points (the cull kernel, upscale, imgui). A material included a domain prelude
	 * and had the domain's entry file appended by the compiler, so its entry points
	 * and its parameter record are the domain's. The prelude's include line is what
	 * decides.
	 */
	enum class Domain : u8
	{
		Plain,
		Surface, // material.slang: scene geometry, struct Material: IMaterial
		Screen,	 // screen.slang: fullscreen effects, struct Material : IScreenMaterial
		Count,
	};

	enum class Stage : u8
	{
		Vertex,
		Fragment,
		Compute,
		Count,
	};

	struct EntryPoint
	{
		String name;
		Stage stage = Stage::Vertex;
	};

	// Which scene pass draws a type. Declared on the struct with [Queue("...")]
	enum class MaterialQueue : u8
	{
		Opaque,
		Cutout,
		Transparent,
		Count,
	};

	// A field of the shader's Material struct. The texture kinds are the prelude's *Ref structs.
	enum class ParamKind : u8
	{
		Float,
		Float2,
		Float3,
		Float4,
		Int,
		Uint,
		Bool,
		Texture2D,
		TextureCube,
		Texture2DArray,
		Count,
	};

	// Bytes a parameter occupies in the record.
	[[nodiscard]] constexpr u32 param_size(ParamKind kind) noexcept
	{
		switch (kind)
		{
			case ParamKind::Float:
			case ParamKind::Int:
			case ParamKind::Uint:
			case ParamKind::Bool:
				return 4;
			case ParamKind::Float2:
			case ParamKind::Texture2D:
			case ParamKind::TextureCube:
			case ParamKind::Texture2DArray:
				return 8;
			case ParamKind::Float3:
				return 12;
			case ParamKind::Float4:
				return 16;
			case ParamKind::Count:
				break;
		}
		return 0;
	}

	// One parameter as reflection saw it, with the authoring attributes beside it.
	struct MaterialParam
	{
		String name;	// the field name: the key in .material files
		String display; // [Display], the name when absent
		String preset;	// [Default] text, parsed exactly like a file value; empty means zero or the fallback texture

		ParamKind kind = ParamKind::Float;
		u32 offset	   = 0;
		u32 size	   = 0;

		f32 range_min  = 0.0f; // [Range]
		f32 range_max  = 0.0f;
		bool has_range = false;
		bool color	   = false; // [Color]: a colour picker instead of four sliders
		bool srgb	   = true;	// textures: [Srgb(false)] marks data, not colour

		gpu::Filter filter	  = gpu::Filter::Linear;	  // textures: [Filter] and [Wrap] are the
		gpu::AddressMode wrap = gpu::AddressMode::Repeat; // sampler a material file can override
	};

	// The record: what a material's values encode into. Offsets are the shader's, in record order.
	struct MaterialLayout
	{
		Vector<MaterialParam> params;
		u32 stride = 0; // the struct's stride in its table, exactly as the shader indexes it
	};

	// Pipeline state and sizing the struct's attributes declared; the defaults are what an
	// undecorated struct gets.
	struct MaterialState
	{
		MaterialQueue queue	   = MaterialQueue::Opaque;
		gpu::CullMode cull	   = gpu::CullMode::Back;
		gpu::BlendPreset blend = gpu::BlendPreset::Opaque;
		bool depth_write	   = true;
		bool casts_shadow	   = true; // [Shadow(false)] keeps the type out of shadow passes
		i32 priority		   = 0;	   // [Priority]: draw order inside the queue, lower first
		u32 capacity		   = 256;  // [Capacity]: materials of this type the table holds
	};

	/**
	 * The compiler's product for one shader file, and what a cooked pair on disk holds: the
	 * SPIR-V in .spv, everything else in .layout. Every entry point of the file is in the one
	 * blob; a pipeline picks by name.
	 */
	struct Program
	{
		Vector<u8> bytecode;
		Vector<EntryPoint> entries;
		Domain domain = Domain::Plain;

		MaterialLayout layout; // empty for a plain program
		MaterialState state;

		Vector<String> includes; // every file the compile read, the shader itself first
		u64 hash = 0;			 // over the source, every include and the compiler: the cache key
	};

	[[nodiscard]] const MaterialParam* find_param(const MaterialLayout& layout, StringView name) noexcept;
	[[nodiscard]] const EntryPoint* find_entry(const Program& program, StringView name) noexcept;

	// Matches text against an enum's EMBER_ENUM_NAMES table.
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

	/**
	 * The .layout sidecar: everything but the SPIR-V, as settings text, so a cooked program reads
	 * with the same parser as a .material file and a new key never invalidates an old cook.
	 * Parameters are `param.<name>` tables; record order is offset order whatever the file's.
	 */
	void write_program_layout(const Program& program, String& out) noexcept;

	/// Reads a sidecar into every field of `out` but the SPIR-V. Unknown keys are ignored; a
	/// parameter without a kind, one past the stride, or an unknown enum is an error, in `error`.
	[[nodiscard]] bool read_program_layout(StringView text, Program& out, String* error = nullptr) noexcept;
}

namespace ember
{
	// The vocabulary layouts and .material files use for these enums. Defined here, once, for the
	// gpu enums too: nothing else spells them in data.
	EMBER_ENUM_NAMES(shader::Domain, "plain", "surface", "screen");
	EMBER_ENUM_NAMES(shader::Stage, "vertex", "fragment", "compute");
	EMBER_ENUM_NAMES(shader::MaterialQueue, "opaque", "cutout", "transparent");
	EMBER_ENUM_NAMES(shader::ParamKind, "float", "float2", "float3", "float4", "int", "uint", "bool", "texture2d",
					 "texturecube", "texture2darray");
	EMBER_ENUM_NAMES(gpu::CullMode, "none", "back", "front");
	EMBER_ENUM_NAMES(gpu::BlendPreset, "opaque", "alpha", "additive", "premultiplied");
	EMBER_ENUM_NAMES(gpu::Filter, "point", "linear");
	EMBER_ENUM_NAMES(gpu::AddressMode, "repeat", "mirror", "clamp", "border");
}
