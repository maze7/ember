#include <ember/gpu/buffer.h>
#include <ember/gpu/sampler.h>
#include <ember/gpu/texture.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/descriptor_heap.h>

namespace ember::gpu::vk
{
	void DescriptorHeap::reset_slot(const Context& ctx, u32 slot, u8 mask) noexcept
	{
		// TODO(Callan): implement this tomorrow. Just getting the project building
		// for an EOD commit
	}
}
