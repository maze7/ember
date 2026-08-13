#pragma once

#include <ember/core/common.h>

namespace ember
{
	inline constexpr size_t DEFAULT_ALIGNMENT = 16; // fits SIMD.

	/// Attribution for heap allocations. Each tag names one HeapResource instance
	/// (memory::heap(tag)), chosen once where a subsystem is wired; individual
	/// allocation sites never repeat the tag.
	enum class MemoryTag : u16
	{
		Unknown = 0,
		Engine,
		Graphics,
		Audio,
		Physics,
		ECS,
		Gameplay,
		Assets,
		Scripting,
		Network,
		Platform,
		Tools,
		Strings,
		Count
	};

	EMBER_ENUM_NAMES(
		MemoryTag,
		"Unknown",
		"Engine",
		"Graphics",
		"Audio",
		"Physics",
		"ECS",
		"Gameplay",
		"Assets",
		"Scripting",
		"Network",
		"Platform",
		"Tools",
		"Strings");

	/// Memory tag budgets are early warnings, not hard limits.
	inline constexpr std::array<size_t, static_cast<size_t>(MemoryTag::Count)> MEMORY_TAG_BUDGETS = {
		256_mb, // Unknown
		64_mb,	// Engine
		512_mb, // Graphics
		64_mb,	// Audio
		128_mb, // Physics
		128_mb, // ECS
		128_mb, // Gameplay
		512_mb, // Assets
		64_mb,	// Scripting
		64_mb,	// Network
		64_mb,	// Platform
		64_mb,	// Tools
		64_mb,	// Strings
	};
}
