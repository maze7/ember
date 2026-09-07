#if defined(EMBER_PLATFORM_LINUX) || defined(EMBER_PLATFORM_MACOS)
	// POSIX fibers. Stacks come from virtual_memory with an inaccessible guard page below them,
	// the register state lives in FiberContext and fiber_x86_64_sysv.S moves between contexts.
	#include <ember/core/bits.h>
	#include <ember/memory/memory.h>
	#include <ember/memory/virtual_memory.h>
	#include <jobs/fiber.h>

	#include <cstddef>

	#if defined(__x86_64__)
		#include <xmmintrin.h>
	#endif

namespace ember::jobs
{
	#if defined(__x86_64__)
	// Saved register set. Offsets are shared with fiber_x86_64_sysv.S.
	struct alignas(16) FiberContext
	{
		u64 rsp			= 0;
		u64 rip			= 0;
		u64 rbx			= 0;
		u64 rbp			= 0;
		u64 r12			= 0;
		u64 r13			= 0;
		u64 r14			= 0;
		u64 r15			= 0;
		u32 mxcsr		= 0;
		u16 x87_control = 0;
	};

	static_assert(offsetof(FiberContext, rsp) == 0);
	static_assert(offsetof(FiberContext, rip) == 8);
	static_assert(offsetof(FiberContext, rbx) == 16);
	static_assert(offsetof(FiberContext, rbp) == 24);
	static_assert(offsetof(FiberContext, r12) == 32);
	static_assert(offsetof(FiberContext, r13) == 40);
	static_assert(offsetof(FiberContext, r14) == 48);
	static_assert(offsetof(FiberContext, r15) == 56);
	static_assert(offsetof(FiberContext, mxcsr) == 64);
	static_assert(offsetof(FiberContext, x87_control) == 68);
	static_assert(sizeof(FiberContext) == 80);
	#elif defined(__aarch64__)
	// Saved register set. Offsets are shared with fiber_aarch64_aapcs.S.
	struct alignas(16) FiberContext
	{
		u64 sp	 = 0;
		u64 lr	 = 0;
		u64 x19	 = 0;
		u64 x20	 = 0;
		u64 x21	 = 0;
		u64 x22	 = 0;
		u64 x23	 = 0;
		u64 x24	 = 0;
		u64 x25	 = 0;
		u64 x26	 = 0;
		u64 x27	 = 0;
		u64 x28	 = 0;
		u64 x29	 = 0;
		u64 d8	 = 0;
		u64 d9	 = 0;
		u64 d10	 = 0;
		u64 d11	 = 0;
		u64 d12	 = 0;
		u64 d13	 = 0;
		u64 d14	 = 0;
		u64 d15	 = 0;
		u64 fpcr = 0;
	};

	static_assert(offsetof(FiberContext, sp) == 0);
	static_assert(offsetof(FiberContext, lr) == 8);
	static_assert(offsetof(FiberContext, x19) == 16);
	static_assert(offsetof(FiberContext, x28) == 88);
	static_assert(offsetof(FiberContext, x29) == 96);
	static_assert(offsetof(FiberContext, d8) == 104);
	static_assert(offsetof(FiberContext, d15) == 160);
	static_assert(offsetof(FiberContext, fpcr) == 168);
	static_assert(sizeof(FiberContext) == 176);
	#else
		#error "Unsupported POSIX fiber architecture"
	#endif

	struct Fiber
	{
		FiberContext context;
		void* stack_base  = nullptr; // reservation start, guard page first
		size_t stack_size = 0;		 // whole reservation, 0 for an adopted thread
	};
}

extern "C"
{
	void ember_fiber_switch(ember::jobs::FiberContext* from, ember::jobs::FiberContext* to) noexcept;
	void ember_fiber_trampoline() noexcept;
}

namespace
{
	using namespace ember;

	#if defined(__x86_64__)
	[[nodiscard]] u16 read_x87_control() noexcept
	{
		u16 control = 0;
		__asm__ __volatile__("fnstcw %0" : "=m"(control));
		return control;
	}

	void init_context(jobs::FiberContext& context, uintptr_t stack_top, jobs::FiberEntry entry, void* arg) noexcept
	{
		context.rsp			= align_down(stack_top, uintptr_t{16});
		context.rip			= reinterpret_cast<u64>(&ember_fiber_trampoline);
		context.r12			= reinterpret_cast<u64>(arg);
		context.r13			= reinterpret_cast<u64>(entry);
		context.mxcsr		= _mm_getcsr();
		context.x87_control = read_x87_control();
	}
	#elif defined(__aarch64__)
	[[nodiscard]] u64 read_fpcr() noexcept
	{
		u64 value = 0;
		__asm__ __volatile__("mrs %0, fpcr" : "=r"(value));
		return value;
	}

	void init_context(jobs::FiberContext& context, uintptr_t stack_top, jobs::FiberEntry entry, void* arg) noexcept
	{
		context.sp	 = align_down(stack_top, uintptr_t{16});
		context.lr	 = reinterpret_cast<u64>(&ember_fiber_trampoline);
		context.x19	 = reinterpret_cast<u64>(arg);
		context.x20	 = reinterpret_cast<u64>(entry);
		context.fpcr = read_fpcr();
	}
	#endif
}

namespace ember::jobs
{
	Fiber* fiber_create(const FiberDef& def) noexcept
	{
		EMBER_ASSERT(def.entry != nullptr);
		EMBER_ASSERT(def.stack_size != 0);

		const size_t guard	= virtual_memory::page_size();
		const size_t usable = virtual_memory::round_to_page_size(def.stack_size);
		const size_t total	= virtual_memory::round_to_allocation_granularity(usable + guard);

		u8* base = static_cast<u8*>(virtual_memory::reserve(total));
		if (base == nullptr)
			return nullptr;

		if (!virtual_memory::commit(base + guard, total - guard))
		{
			(void)virtual_memory::release(base, total);
			return nullptr;
		}

		Fiber* fiber	  = memory::new_object<Fiber>(MemoryTag::Engine);
		fiber->stack_base = base;
		fiber->stack_size = total;
		init_context(fiber->context, reinterpret_cast<uintptr_t>(base) + total, def.entry, def.arg);
		return fiber;
	}

	void fiber_destroy(Fiber* fiber) noexcept
	{
		if (fiber == nullptr)
			return;

		EMBER_ASSERT(fiber->stack_base != nullptr);
		(void)virtual_memory::release(fiber->stack_base, fiber->stack_size);
		memory::delete_object(MemoryTag::Engine, fiber);
	}

	Fiber* fiber_adopt_thread() noexcept { return memory::new_object<Fiber>(MemoryTag::Engine); }

	void fiber_release_thread(Fiber* fiber) noexcept
	{
		if (fiber == nullptr)
			return;

		EMBER_ASSERT(fiber->stack_base == nullptr);
		memory::delete_object(MemoryTag::Engine, fiber);
	}

	void fiber_switch(Fiber* from, Fiber* to) noexcept
	{
		EMBER_ASSERT(from != nullptr && to != nullptr && from != to);
		ember_fiber_switch(&from->context, &to->context);
	}
}

#endif
