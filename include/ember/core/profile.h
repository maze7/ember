#pragma once

#include <ember/core/common.h>

/**
 * Engine profiling facade.
 *
 * Wraps the Tracy client so engine and game code never include tracy headers directly.
 * When profiling is disabled every macro compiles to nothing, so instrumentation can
 * (and should) be left in shipping code paths.
 *
 * Activation is automatic: EMBER_USE_TRACY comes from ember's CMake.
 *
 * Rules of use:
 * 	- One zone macro (SCOPE / FUNCTION / SCOPE_DYNAMIC) per C++ scope. Open a nested { }
 * 	  block for finer-grained zones.
 * - Name strings passed to SCOPE, FRAME_*, PLOT and LOCKABLE macros must be string literals.
 * - SCOPE_DYNAMIC accepts runtime strings; it is a little more expensive, so prefer the
 * 	 literal variants on hot paths.
 */
#if EMBER_USE_TRACY
	#include <tracy/Tracy.hpp>

namespace ember
{
	// 0xRRGGBB zone colors, one per subsystem, for readable captures.
	inline constexpr u32 PROFILE_COLOR_FRAME	= 0x546e7a;
	inline constexpr u32 PROFILE_COLOR_INPUT	= 0x00acc1;
	inline constexpr u32 PROFILE_COLOR_GAMEPLAY = 0x43a047;
	inline constexpr u32 PROFILE_COLOR_PHYSICS	= 0xf4511e;
	inline constexpr u32 PROFILE_COLOR_RENDER	= 0x8e24aa;
	inline constexpr u32 PROFILE_COLOR_AUDIO	= 0xfdd835;
	inline constexpr u32 PROFILE_COLOR_MEMORY	= 0x6d4c41;
	inline constexpr u32 PROFILE_COLOR_IO		= 0x3949ab;
	inline constexpr u32 PROFILE_COLOR_NETWORK	= 0x1e88e5;
	inline constexpr u32 PROFILE_COLOR_WAIT		= 0xb71c1c; // stalls, lock waits, sleeps
}

	/// Frame boundaries. Call EMBER_PROFILE_FRAME() once per frame, after present.
	/// The _N variants delimit secondary frame sets (e.g. "Server Tick").
	#define EMBER_PROFILE_FRAME() FrameMark
	#define EMBER_PROFILE_FRAME_N(name) FrameMarkNamed(name)
	#define EMBER_PROFILE_FRAME_START(name) FrameMarkStart(name)
	#define EMBER_PROFILE_FRAME_END(name) FrameMarkEnd(name)

	/// CPU Zones (RAII, closes at end of enclosing scope).
	#define EMBER_PROFILE_FUNCTION() ZoneScoped
	#define EMBER_PROFILE_FUNCTION_C(color) ZoneScopedC(color)
	#define EMBER_PROFILE_SCOPE(name) ZoneScopedN(name)
	#define EMBER_PROFILE_SCOPE_C(name, color) ZoneScopedNC(name, color)
	#define EMBER_PROFILE_SCOPE_DYNAMIC(name) ZoneTransientN(___ember_profile_zone, name, true)

	/// Per-zone annotations; attach runtime data to the innermost open zone.
	#define EMBER_PROFILE_ZONE_TEXT(txt, size) ZoneText(txt, size)
	#define EMBER_PROFILE_ZONE_NAME(txt, size) ZoneName(txt, size)
	#define EMBER_PROFILE_ZONE_VALUE(value) ZoneValue(value)

	/// Call once at the top of every engine-created thread.
	#define EMBER_PROFILE_THREAD(name) ::tracy::SetThreadName(name)

	/// Plots: graphed counters (entity counts, draw calls, arena high-water marks...).
	#define EMBER_PROFILE_PLOT(name, value) TracyPlot(name, value)
	#define EMBER_PROFILE_PLOT_CONFIG_NUMBER(name)                                                                     \
		TracyPlotConfig(name, ::tracy::PlotFormatType::Number, false, true, 0)
	#define EMBER_PROFILE_PLOT_CONFIG_MEMORY(name)                                                                     \
		TracyPlotConfig(name, ::tracy::PlotFormatType::Memory, false, true, 0)
	#define EMBER_PROFILE_PLOT_CONFIG_PERCENT(name)                                                                    \
		TracyPlotConfig(name, ::tracy::PlotFormatType::Percentage, false, true, 0)

	/// Timeline messages and build metadata.
	#define EMBER_PROFILE_MESSAGE(txt, size) TracyMessage(txt, size)
	#define EMBER_PROFILE_MESSAGE_L(literal) TracyMessageL(literal)
	#define EMBER_PROFILE_APP_INFO(txt, size) TracyAppInfo(txt, size)

	/// Lock contention tracking. Declare with LOCKABLE, take LOCKABLE_BASE(T)&
	/// in function signatures, and LOCK_MARK inside critical sections you want
	/// attributed on the timeline.
	#define EMBER_PROFILE_LOCKABLE(type, var) TracyLockable(type, var)
	#define EMBER_PROFILE_LOCKABLE_N(type, var, desc) TracyLockableN(type, var, desc)
	#define EMBER_PROFILE_LOCKABLE_BASE(type) LockableBase(type)
	#define EMBER_PROFILE_LOCK_MARK(var) LockMark(var)

	/// Custom allocator hooks (named pools show up as separate memory maps).
	#define EMBER_PROFILE_ALLOC(ptr, size) TracyAlloc(ptr, size)
	#define EMBER_PROFILE_FREE(ptr) TracyFree(ptr)
	#define EMBER_PROFILE_ALLOC_N(ptr, size, name) TracyAllocN(ptr, size, name)
	#define EMBER_PROFILE_FREE_N(ptr, name) TracyFreeN(ptr, name)
#else
	#define EMBER_PROFILE_FRAME() ((void)0)
	#define EMBER_PROFILE_FRAME_N(name) ((void)0)
	#define EMBER_PROFILE_FRAME_START(name) ((void)0)
	#define EMBER_PROFILE_FRAME_END(name) ((void)0)
	#define EMBER_PROFILE_FUNCTION() ((void)0)
	#define EMBER_PROFILE_FUNCTION_C(color) ((void)0)
	#define EMBER_PROFILE_SCOPE(name) ((void)0)
	#define EMBER_PROFILE_SCOPE_C(name, color) ((void)0)
	#define EMBER_PROFILE_SCOPE_DYNAMIC(name) ((void)0)
	#define EMBER_PROFILE_ZONE_TEXT(txt, size) ((void)0)
	#define EMBER_PROFILE_ZONE_NAME(txt, size) ((void)0)
	#define EMBER_PROFILE_ZONE_VALUE(value) ((void)0)
	#define EMBER_PROFILE_THREAD(name) ((void)0)
	#define EMBER_PROFILE_PLOT(name, value) ((void)0)
	#define EMBER_PROFILE_PLOT_CONFIG_NUMBER(name) ((void)0)
	#define EMBER_PROFILE_PLOT_CONFIG_MEMORY(name) ((void)0)
	#define EMBER_PROFILE_PLOT_CONFIG_PERCENT(name) ((void)0)
	#define EMBER_PROFILE_MESSAGE(txt, size) ((void)0)
	#define EMBER_PROFILE_MESSAGE_L(literal) ((void)0)
	#define EMBER_PROFILE_APP_INFO(txt, size) ((void)0)
	// Fall back to a plain object so declarations still compile.
	#define EMBER_PROFILE_LOCKABLE(type, var) type var
	#define EMBER_PROFILE_LOCKABLE_N(type, var, desc) type var
	#define EMBER_PROFILE_LOCKABLE_BASE(type) type
	#define EMBER_PROFILE_LOCK_MARK(var) ((void)0)
	#define EMBER_PROFILE_ALLOC(ptr, size) ((void)0)
	#define EMBER_PROFILE_FREE(ptr) ((void)0)
	#define EMBER_PROFILE_ALLOC_N(ptr, size, name) ((void)0)
	#define EMBER_PROFILE_FREE_N(ptr, name) ((void)0)
#endif // EMBER_USE_TRACY
