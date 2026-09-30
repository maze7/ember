#include <ember/core/json.h>
#include <ember/memory/memory.h>

#include <shader/reflection.h>
#include <shader/report.h>

namespace ember::shader
{
	using material::Domain;
	using material::is_texture;
	using material::Layout;
	using material::Param;
	using material::ParamKind;
	using material::Queue;
	using material::State;

	namespace
	{
		[[nodiscard]] StringView string_argument(slang::UserAttribute* attribute) noexcept
		{
			size_t length	 = 0;
			const char* text = attribute->getArgumentValueString(0, &length);
			return text != nullptr ? StringView(text, length) : StringView();
		}

		// Slang coerces arguments to the attribute struct's field types, so a bool arrives as an int
		// and [Range(0, 1)] as two floats.
		[[nodiscard]] i32 int_argument(slang::UserAttribute* attribute) noexcept
		{
			int value = 0;
			(void)attribute->getArgumentValueInt(0, &value);
			return value;
		}

		[[nodiscard]] f32 float_argument(slang::UserAttribute* attribute, u32 index) noexcept
		{
			float value = 0.0f;
			(void)attribute->getArgumentValueFloat(index, &value);
			return value;
		}

		/// Matches an attribute's text against an enum's names, listing them all when it misses: the
		/// error that turns a typo into a one-line fix.
		template <class E>
		bool parse_attribute(StringView text, E& out, StringView file, StringView what, Report& report) noexcept
		{
			if (parse_enum(text, out))
				return true;

			String accepted(&memory::heap(MemoryTag::Tools));
			for (const char* name : enum_names<E>())
			{
				if (!accepted.empty())
					accepted += ", ";
				accepted += name;
			}

			report.error(file, "{} '{}' is not one of: {}", what, text, accepted);
			return false;
		}

		/// The kind of a number field; false for widths the records do not hold.
		[[nodiscard]] bool number_kind(slang::TypeReflection::ScalarType scalar, ParamKind& out) noexcept
		{
			switch (scalar)
			{
				case slang::TypeReflection::ScalarType::Float32:
					out = ParamKind::Float;
					return true;
				case slang::TypeReflection::ScalarType::Int32:
					out = ParamKind::Int;
					return true;
				case slang::TypeReflection::ScalarType::UInt32:
					out = ParamKind::Uint;
					return true;
				case slang::TypeReflection::ScalarType::Bool:
					out = ParamKind::Bool;
					return true;
				default:
					return false;
			}
		}

		/**
		 * One field, with its attributes checked against its type. Every mismatch is reported, not
		 * just the first. False when the field's type cannot be a parameter at all.
		 */
		bool reflect_param(slang::VariableLayoutReflection* field, StringView owner, StringView file, Param& out,
						   Report& report) noexcept
		{
			slang::TypeLayoutReflection* layout = field->getTypeLayout();
			slang::TypeReflection* type			= layout->getType();

			out.name   = field->getName();
			out.offset = static_cast<u32>(field->getOffset());
			out.size   = static_cast<u32>(layout->getSize());

			switch (type->getKind())
			{
				case slang::TypeReflection::Kind::Scalar:
				case slang::TypeReflection::Kind::Vector:
					out.components = type->getKind() == slang::TypeReflection::Kind::Vector
										 ? static_cast<u8>(type->getElementCount())
										 : u8{1};

					if (!number_kind(type->getScalarType(), out.kind))
					{
						report.error(file, "{}.{}: parameters are 32-bit float, int, uint or bool", owner, out.name);
						return false;
					}
					break;

				case slang::TypeReflection::Kind::Struct:
				{
					const StringView name = type->getName();

					if (name == "Texture2DRef")
						out.kind = ParamKind::Texture2D;
					else if (name == "Texture2DArrayRef")
						out.kind = ParamKind::Texture2DArray;
					else if (name == "TextureCubeRef")
						out.kind = ParamKind::TextureCube;
					else
					{
						report.error(file, "{}.{} is a {}; parameters are numbers, vectors and texture refs", owner,
									 out.name, name);
						return false;
					}
					break;
				}

				default:
					report.error(file, "{}.{}: matrices and arrays are not parameters; use vectors", owner, out.name);
					return false;
			}

			slang::VariableReflection* variable = field->getVariable();
			bool sampler_state					= false;

			for (u32 i = 0; i < variable->getUserAttributeCount(); ++i)
			{
				slang::UserAttribute* attribute = variable->getUserAttributeByIndex(i);
				const StringView name			= attribute->getName();

				if (name == "Display")
					out.display = string_argument(attribute);
				else if (name == "Default")
					out.preset = string_argument(attribute);
				else if (name == "Color")
					out.color = true;
				else if (name == "Hdr")
					out.hdr = true;
				else if (name == "Linear")
					out.linear = sampler_state = true;
				else if (name == "Range")
				{
					out.has_range = true;
					out.range_min = float_argument(attribute, 0);
					out.range_max = float_argument(attribute, 1);
				}
				else if (name == "Filter")
				{
					sampler_state = true;
					(void)parse_attribute(string_argument(attribute), out.filter, file, "filter", report);
				}
				else if (name == "Wrap")
				{
					sampler_state = true;
					(void)parse_attribute(string_argument(attribute), out.wrap, file, "wrap", report);
				}
			}

			const bool texture = is_texture(out.kind);

			if (out.has_range && (texture || out.kind == ParamKind::Bool))
				report.error(file, "{}.{}: [Range] needs a number", owner, out.name);

			if (out.color && !(out.kind == ParamKind::Float && out.components >= 3))
				report.error(file, "{}.{}: [Color] needs a float3 or float4", owner, out.name);

			if (out.hdr && !out.color)
				report.error(file, "{}.{}: [Hdr] qualifies [Color]", owner, out.name);

			if (sampler_state && !texture)
				report.error(file, "{}.{}: [Filter], [Wrap] and [Linear] are for textures", owner, out.name);

			// A default is parsed now, exactly as a file's value will be, so a bad one fails the
			// type's compile instead of the first material that relies on it.
			if (!out.preset.empty())
			{
				u32 words[4]					= {};
				material::BuiltinTexture unused = material::BuiltinTexture::White;

				if (texture && !parse_enum(StringView(out.preset), unused))
					report.error(file, "{}.{}: [Default(\"{}\")] is not white, black, flat or error", owner, out.name,
								 out.preset);
				else if (!texture && !material::parse_value(out, out.preset, words))
					report.error(file, "{}.{}: [Default(\"{}\")] is not {} {} value{}", owner, out.name, out.preset,
								 out.components, enum_name(out.kind), out.components == 1 ? "" : "s");
			}

			return true;
		}
	}

	void reflect_record(slang::TypeLayoutReflection* record, StringView owner, StringView file, Layout& out,
						Report& report) noexcept
	{
		out.size = static_cast<u32>(record->getStride());

		for (u32 i = 0; i < record->getFieldCount(); ++i)
			(void)reflect_param(record->getFieldByIndex(i), owner, file, out.params.emplace_back(), report);
	}

	void reflect_instance(slang::TypeLayoutReflection* instance, StringView owner, StringView file, Layout& out,
						  Report& report) noexcept
	{
		String name(owner, &memory::heap(MemoryTag::Tools));
		name += ".Instance";

		u32 offset = 0;

		for (u32 i = 0; i < instance->getFieldCount(); ++i)
		{
			Param& param = out.params.emplace_back();

			if (!reflect_param(instance->getFieldByIndex(i), name, file, param, report))
				continue;

			if (is_texture(param.kind))
			{
				report.error(file, "{}.{}: per-object data holds numbers; a texture belongs in the material", name,
							 param.name);
				continue;
			}

			param.offset = offset;
			param.size	 = 4u * param.components;
			offset += param.size;
		}

		out.size = offset;

		if (offset > material::INSTANCE_BYTES)
			report.error(file, "{} is {} bytes; per-object data holds {} at most", name, offset,
						 material::INSTANCE_BYTES);
	}

	void reflect_state(slang::TypeReflection* type, Domain domain, StringView file, State& state, StringView& shading,
					   Report& report) noexcept
	{
		bool has_blend		 = false;
		bool has_depth_write = false;
		bool has_shadow		 = false;

		for (u32 i = 0; i < type->getUserAttributeCount(); ++i)
		{
			slang::UserAttribute* attribute = type->getUserAttributeByIndex(i);
			const StringView name			= attribute->getName();

			if (domain == Domain::Screen && name != "Blend")
			{
				report.error(file, "[{}] does not apply to a screen material", name);
				continue;
			}

			if (name == "Queue")
				(void)parse_attribute(string_argument(attribute), state.queue, file, "queue", report);
			else if (name == "Cull")
				(void)parse_attribute(string_argument(attribute), state.cull, file, "cull mode", report);
			else if (name == "Blend")
				has_blend = parse_attribute(string_argument(attribute), state.blend, file, "blend", report);
			else if (name == "DepthWrite")
			{
				has_depth_write	  = true;
				state.depth_write = int_argument(attribute) != 0;
			}
			else if (name == "Shadow")
			{
				has_shadow		   = true;
				state.casts_shadow = int_argument(attribute) != 0;
			}
			else if (name == "Priority")
				state.priority = int_argument(attribute);
			else if (name == "Shading")
				shading = string_argument(attribute);
		}

		if (domain == Domain::Screen)
		{
			// A screen material covers its target once: nothing to cull, depth test or shadow.
			state.cull		   = gpu::CullMode::None;
			state.depth_write  = false;
			state.casts_shadow = false;
			return;
		}

		// Transparent types blend over the finished world, so unless they say otherwise they
		// neither write depth nor cast shadows; everything else writes its colour outright.
		const bool transparent = state.queue == Queue::Transparent;

		if (!has_blend)
			state.blend = transparent ? gpu::BlendPreset::AlphaBlend : gpu::BlendPreset::Opaque;
		else if (!transparent)
			report.error(file, "[Blend] needs [Queue(\"transparent\")]; opaque and cutout types write their colour");
		else if (state.blend == gpu::BlendPreset::Opaque)
			report.error(file, "a transparent type blends: alpha, premultiplied or additive");

		if (!has_depth_write)
			state.depth_write = !transparent;

		if (!has_shadow)
			state.casts_shadow = !transparent;
	}
}
