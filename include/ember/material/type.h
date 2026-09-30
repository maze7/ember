#pragma once

#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/gpu/pipeline.h>
#include <ember/gpu/sampler.h>

namespace ember::material
{
	/// Which interface the author's struct implements, and so which entry module draws it.
	enum class Domain : u8
	{
		Surface, // IMaterial: scene geometry, through surface.slang
		Screen,	 // IScreenMaterial: the whole target, through screen.slang
		Count,
	};

	/// The entry points in a type's bytecode. The domain fixes them, surface types holding all three
	/// and screen types the first two, so a type carries no list of its own: a pass builds its
	/// pipeline from the bytecode and one of these names.
	inline constexpr const char* VERTEX_ENTRY = "vs_main";
	inline constexpr const char* COLOR_ENTRY  = "fs_main";
	inline constexpr const char* DEPTH_ENTRY  = "fs_depth";

	/// The pass family that draws a surface type.
	enum class Queue : u8
	{
		Opaque,
		Cutout,
		Transparent,
		Count,
	};

	/// A parameter's element type. Numbers are 32-bit scalars or vectors of Param::components; the
	/// texture kinds are the authoring API's *Ref structs, a bindless texture and sampler index pair.
	enum class ParamKind : u8
	{
		Float,
		Int,
		Uint,
		Bool,
		Texture2D,
		Texture2DArray,
		TextureCube,
		Count,
	};

	[[nodiscard]] constexpr bool is_texture(ParamKind kind) noexcept
	{
		return kind >= ParamKind::Texture2D && kind < ParamKind::Count;
	}

	/// The textures a [Default] may name, and a .material file may name instead of a path.
	/// The registry owns one of each.
	enum class BuiltinTexture : u8
	{
		White,
		Black,
		Flat,
		Error,
		Count,
	};

	/// Bytes of a *Ref in a record: the texture's bindless index, then the sampler's.
	inline constexpr u32 TEXTURE_REF_BYTES = 8;

	/// Bytes of per-object data a type may declare as `struct Instance`: the budget the authoring
	/// model promises, which the scene's instance table strides by. render.slang's INSTANCE_STRIDE
	/// equals it.
	inline constexpr u32 INSTANCE_BYTES = 32;

	/// A cooked type is two files with one stem: its bytecode, and the .type file beside it that
	/// describes it. The cook writes both; read_cooked() takes both.
	inline constexpr const char* SPIRV_EXTENSION = ".spv";
	inline constexpr const char* TYPE_EXTENSION	 = ".type";

	/// One field of a record, as reflection saw it, with its authoring attributes beside it.
	struct Param
	{
		String name;	// the field's name: the key in .material files and in set() calls
		String display; // [Display]; empty when absent, and the inspector shows the name
		String preset;	// [Default], parsed exactly like a file value; empty means zero, or white

		ParamKind kind = ParamKind::Float;
		u8 components  = 1; // 1..4 for numbers, 1 for textures
		u32 offset	   = 0; // bytes from the start of the record
		u32 size	   = 0;

		bool color	   = false; // [Color]: files hold sRGB, the record linear
		bool hdr	   = false; // [Hdr]: with [Color], linear and unbounded in files too
		bool linear	   = false; // [Linear]: a texture holding data, not colour
		bool has_range = false; // [Range]
		f32 range_min  = 0.0f;
		f32 range_max  = 0.0f;

		// Textures: the sampler an instance gets unless its file says otherwise, in the GPU's own
		// terms. The registry expands the pair into a gpu::SamplerDef.
		gpu::Filter filter	  = gpu::Filter::Linear;
		gpu::AddressMode wrap = gpu::AddressMode::Repeat;
	};

	/// A record: what values encode into, fields in declaration order.
	struct Layout
	{
		Vector<Param> params;
		u32 size = 0; // a material record's stride in its table; an Instance packed size
	};

	/// The type's say in its pipelines, with the defaults resolved. It is not a pipeline: a pass
	/// adds what a material cannot know (its targets, the depth test, whether this pass writes depth
	/// at all) and builds the gpu::GraphicsPipelineDef from both.
	struct State
	{
		Queue queue			   = Queue::Opaque;
		gpu::CullMode cull	   = gpu::CullMode::Back;
		gpu::BlendPreset blend = gpu::BlendPreset::Opaque;
		bool depth_write	   = true;
		bool casts_shadow	   = true;
		i32 priority		   = 0;
		String shading; // the linked shading model's struct; empty for screen types
	};

	/// A material type as the compiler produces it and a cooked pair carries it: the bytecode in
	/// .spv and everything else in the .type file beside it. The registry creates its type from this.
	struct Type
	{
		Domain domain = Domain::Surface;
		String name; // the author's struct, which is also what RenderDoc shows

		/// Words rather than bytes, so the blob always meets SPIR-V's four byte alignment.
		Vector<u32> spirv;

		Layout record;
		Layout instance; // no params when the type declares no Instance
		State state;

		/// Every file the compile read, the type's own first. Hot reload watches these;
		/// a cooked type has none.
		Vector<String> dependencies;

		/// hash_type() at compile time: equal hashes are equal types.
		u64 hash = 0;

		[[nodiscard]] Span<const u8> bytecode() const noexcept
		{
			return {reinterpret_cast<const u8*>(spirv.data()), spirv.size() * sizeof(u32)};
		}
	};

	/**
	 * Parses a number parameter's value the way files write it, "0.5" or "1 0.8 0.6 1", into the
	 * bit patterns of its components: exactly `components` values of the parameter's kind, bools
	 * spelt true, false, 1 or 0. Colours come back as written; the encoder linearises them. The
	 * compiler runs every [Default] through this, so a bad default fails the type's compile rather
	 * than the first material that relies on it.
	 */
	[[nodiscard]] bool parse_value(const Param& param, StringView text, u32 (&words)[4]) noexcept;

	[[nodiscard]] const Param* find_param(const Layout& layout, StringView name) noexcept;

	/**
	 * Folds everything but the dependencies into one value. A reload whose hash matches the live
	 * type changed nothing observable (an edited comment) and can stop there, before any pipeline
	 * is rebuilt.
	 */
	[[nodiscard]] u64 hash_type(const Type& type) noexcept;

	/**
	 * The .type file: everything but the SPIR-V and the dependencies, as JSON, so it reads in a diff
	 * and loads with the parser .material files use. False only when a value cannot be written.
	 */
	[[nodiscard]] bool write_type(const Type& type, String& out) noexcept;

	/**
	 * Reads a .type file into `out`, leaving its SPIR-V alone. Unknown keys are ignored so an older
	 * reader survives a newer cook; anything that would let a record be misread (a parameter past
	 * the stride, a size its kind cannot have, an unknown enum) is refused with the reason in
	 * `error`.
	 */
	[[nodiscard]] bool read_type(StringView text, Type& out, String& error) noexcept;

	/**
	 * Joins a cooked pair into `out`: a .type file's text and the bytecode cooked with it, as a
	 * loader reads them from disk or a module embeds them. The stored hash covers the bytecode, so a
	 * pair that does not belong together (a stale .spv beside a fresh .type, a torn copy) is refused
	 * here, before any record is written through the wrong layout.
	 */
	[[nodiscard]] bool read_cooked(StringView type_file, Span<const u8> spirv, Type& out, String& error) noexcept;
}

namespace ember
{
	// How cooked files and material attributes spell these. The gpu enums spell theirs beside them.
	EMBER_ENUM_NAMES(material::Domain, "surface", "screen");
	EMBER_ENUM_NAMES(material::Queue, "opaque", "cutout", "transparent");
	EMBER_ENUM_NAMES(material::ParamKind, "float", "int", "uint", "bool", "texture2d", "texture2darray", "texturecube");
	EMBER_ENUM_NAMES(material::BuiltinTexture, "white", "black", "flat", "error");
}
