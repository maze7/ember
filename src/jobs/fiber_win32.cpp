#if defined(EMBER_PLATFORM_WINDOWS)
	// Win32 fibers. The OS owns the stacks and the saved registers; the seam maps onto
	// CreateFiberEx, ConvertThreadToFiberEx and SwitchToFiber.
	#include <ember/memory/memory.h>
	#include <jobs/fiber.h>

	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <windows.h>

namespace ember::jobs
{
	struct Fiber
	{
		void* handle	 = nullptr;
		FiberEntry entry = nullptr;
		void* arg		 = nullptr;
		bool thread		 = false; // adopted thread, undone with ConvertFiberToThread
	};
}

namespace
{
	using namespace ember;

	void CALLBACK fiber_main(void* param)
	{
		auto& fiber = *static_cast<jobs::Fiber*>(param);
		fiber.entry(fiber.arg);
		EMBER_UNREACHABLE_ASSERT();
	}
}

namespace ember::jobs
{
	Fiber* fiber_create(const FiberDef& def) noexcept
	{
		EMBER_ASSERT(def.entry != nullptr);
		EMBER_ASSERT(def.stack_size != 0);

		Fiber* fiber  = memory::new_object<Fiber>(MemoryTag::Engine);
		fiber->entry  = def.entry;
		fiber->arg	  = def.arg;
		fiber->handle = CreateFiberEx(def.stack_size, def.stack_size, FIBER_FLAG_FLOAT_SWITCH, fiber_main, fiber);

		if (fiber->handle == nullptr)
		{
			memory::delete_object(MemoryTag::Engine, fiber);
			return nullptr;
		}

		return fiber;
	}

	void fiber_destroy(Fiber* fiber) noexcept
	{
		if (fiber == nullptr)
			return;

		EMBER_ASSERT(!fiber->thread);
		EMBER_ASSERT(fiber->handle != GetCurrentFiber());
		DeleteFiber(fiber->handle);
		memory::delete_object(MemoryTag::Engine, fiber);
	}

	Fiber* fiber_adopt_thread() noexcept
	{
		EMBER_ASSERT(!IsThreadAFiber());

		void* handle = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
		if (handle == nullptr)
			return nullptr;

		Fiber* fiber  = memory::new_object<Fiber>(MemoryTag::Engine);
		fiber->handle = handle;
		fiber->thread = true;
		return fiber;
	}

	void fiber_release_thread(Fiber* fiber) noexcept
	{
		if (fiber == nullptr)
			return;

		EMBER_ASSERT(fiber->thread);
		EMBER_ASSERT(fiber->handle == GetCurrentFiber());
		(void)ConvertFiberToThread();
		memory::delete_object(MemoryTag::Engine, fiber);
	}

	void fiber_switch(Fiber* from, Fiber* to) noexcept
	{
		EMBER_ASSERT(from != nullptr && to != nullptr && from != to);
		EMBER_ASSERT(from->handle == GetCurrentFiber());
		(void)from;
		SwitchToFiber(to->handle);
	}
}

#endif
