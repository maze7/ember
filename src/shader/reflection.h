#pragma once

#include <ember/core/common.h>
#include <ember/material/type.h>

#include <slang.h>

namespace ember::shader
{
	class Report;

	/**
	 * The record a type's values encode into, as its entry points index it: std430, the rules of
	 * the StructuredBuffer the type's table is read through. `owner` names the struct in errors.
	 */
	void reflect_record(slang::TypeLayoutReflection* record, StringView owner, StringView file, material::Layout& out,
						Report& report) noexcept;

	/**
	 * The per-object data a type declares as a nested `struct Instance`. The material API reads it
	 * with ByteAddressBuffer.Load<T>, which packs fields in declaration order with no padding, so
	 * the offsets are computed here instead of taken from std430 reflection, which would pad a
	 * float3 to 16. Numbers only, material::INSTANCE_BYTES at most.
	 */
	void reflect_instance(slang::TypeLayoutReflection* instance, StringView owner, StringView file,
						  material::Layout& out, Report& report) noexcept;

	/**
	 * Pipeline state from the type's attributes, with the defaults its domain and queue imply. The
	 * shading model comes back as written, empty when the type named none, for the compiler to
	 * match against the models it knows.
	 */
	void reflect_state(slang::TypeReflection* type, material::Domain domain, StringView file, material::State& state,
					   StringView& shading, Report& report) noexcept;
}
