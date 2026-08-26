#include "gpu/vulkan/destroy_queue.h"
#include <ember/core/common.h>
#include <ember/gpu/device.h>
#include <ember/memory/memory.h>
#include <gpu/vulkan/backend.h>
#include <platform/vulkan/wsi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vulkan/vulkan_core.h>

namespace ember::gpu
{
	namespace
	{
		/**
		 * The Vulkan feature pNext chain as a value type.
		 *
		 * Wires the core and extension feature structs into their pNext chain at construction.
		 * The chain is self-referential, so copying or moving a wired set would dangle it.
		 *
		 * check_required() and enable_required() walk the same REQUIRED_* tables, so what adapter
		 * selection verifies and what device creation enable can never drift. Optional feature bits
		 * are the caller's policy.
		 */
		class FeatureChain
		{
		public:
			/**
			 * Extension structs join the chain only when the adapter offers the extension.
			 * A chained feature struct without its extension enabled is a validation error at
			 * device creation.
			 */
			FeatureChain() noexcept
			{
				auto* tail = reinterpret_cast<VkBaseOutStructure*>(&m_features2);

				tail = append(tail, &m_vulkan11);
				tail = append(tail, &m_vulkan12);
				tail = append(tail, &m_vulkan13);
			}

			/** A wired chain points into itself; a copy would point into the original. */
			FeatureChain(const FeatureChain&)			 = delete;
			FeatureChain& operator=(const FeatureChain&) = delete;

			/** Fills every chained struct from the adapter in one call. */
			void query(VkPhysicalDevice adapter) noexcept { vkGetPhysicalDeviceFeatures2(adapter, &m_features2); }

			/**
			 * True when the adapter has every required feature; logs each gap by name so an under-spec adapter
			 * reports its complete list in one boot.
			 */
			[[nodiscard]] bool check_required(const char* adapter_name) const noexcept
			{
				bool ok	 = true;
				ok		&= check(m_features2.features, REQUIRED_10, adapter_name);
				ok		&= check(m_vulkan11, REQUIRED_11, adapter_name);
				ok		&= check(m_vulkan12, REQUIRED_12, adapter_name);
				ok		&= check(m_vulkan13, REQUIRED_13, adapter_name);
				return ok;
			}

			/** Sets every required bit. The same tables check_required() reads. */
			void enable_required() noexcept
			{
				enable(m_features2.features, REQUIRED_10);
				enable(m_vulkan11, REQUIRED_11);
				enable(m_vulkan12, REQUIRED_12);
				enable(m_vulkan13, REQUIRED_13);
			}

			[[nodiscard]] const VkPhysicalDeviceFeatures2* head() const noexcept { return &m_features2; }

			// Feature bits, named after their REQUIRED_* tables. Mutable on purpose.
			[[nodiscard]] VkPhysicalDeviceFeatures& features10() noexcept { return m_features2.features; }
			[[nodiscard]] VkPhysicalDeviceVulkan11Features& vulkan11() noexcept { return m_vulkan11; }
			[[nodiscard]] VkPhysicalDeviceVulkan12Features& vulkan12() noexcept { return m_vulkan12; }
			[[nodiscard]] VkPhysicalDeviceVulkan13Features& vulkan13() noexcept { return m_vulkan13; }

			[[nodiscard]] const VkPhysicalDeviceFeatures& features10() const noexcept { return m_features2.features; }
			[[nodiscard]] const VkPhysicalDeviceVulkan11Features& vulkan11() const noexcept { return m_vulkan11; }
			[[nodiscard]] const VkPhysicalDeviceVulkan12Features& vulkan12() const noexcept { return m_vulkan12; }
			[[nodiscard]] const VkPhysicalDeviceVulkan13Features& vulkan13() const noexcept { return m_vulkan13; }

		private:
			/// One required feature: a typed pointer-to-member plus its name for diagnostics.
			template <typename S> struct FeatureRef
			{
				VkBool32 S::* flag;
				const char* name;
			};

			using Features10 = VkPhysicalDeviceFeatures;
			using Features11 = VkPhysicalDeviceVulkan11Features;
			using Features12 = VkPhysicalDeviceVulkan12Features;
			using Features13 = VkPhysicalDeviceVulkan13Features;

			// Stringizes each member exactly once
#define EMBER_FEATURE(S, m)                                                                                            \
	FeatureRef<S> { &S::m, #m }

			/**
			 * The required feature set, one table per chain struct. An adapter missing any entry
			 * is rejected by name. The same tables drive enabling at device creation, so the check
			 * and the enable can never drift. Grouping comments say *why* each feature is required.
			 */
			static constexpr FeatureRef<Features10> REQUIRED_10[] = {
				// Roadmap 2022 feature set
				EMBER_FEATURE(Features10, samplerAnisotropy),
				EMBER_FEATURE(Features10, depthClamp),
				EMBER_FEATURE(Features10, depthBiasClamp),
				EMBER_FEATURE(Features10, independentBlend),
				EMBER_FEATURE(Features10, imageCubeArray),
				EMBER_FEATURE(Features10, fragmentStoresAndAtomics),
				EMBER_FEATURE(Features10, fullDrawIndexUint32),
				EMBER_FEATURE(Features10, drawIndirectFirstInstance),
				EMBER_FEATURE(Features10, shaderStorageImageExtendedFormats),
				// Desktop-universal, required by ember
				EMBER_FEATURE(Features10, multiDrawIndirect),
				EMBER_FEATURE(Features10, textureCompressionBC),
				EMBER_FEATURE(Features10, shaderStorageImageReadWithoutFormat),
				EMBER_FEATURE(Features10, shaderStorageImageWriteWithoutFormat),
			};

			static constexpr FeatureRef<Features11> REQUIRED_11[] = {
				// Roadmap 2022 feature set
				EMBER_FEATURE(Features11, shaderDrawParameters),
			};

			static constexpr FeatureRef<Features12> REQUIRED_12[] = {
				// 1.2 core-mandatory
				EMBER_FEATURE(Features12, timelineSemaphore),
				EMBER_FEATURE(Features12, hostQueryReset),
				// Roadmap 2022 feature set: the bindless heap and friends
				EMBER_FEATURE(Features12, descriptorIndexing),
				EMBER_FEATURE(Features12, runtimeDescriptorArray),
				EMBER_FEATURE(Features12, descriptorBindingPartiallyBound),
				EMBER_FEATURE(Features12, descriptorBindingVariableDescriptorCount),
				EMBER_FEATURE(Features12, descriptorBindingSampledImageUpdateAfterBind),
				EMBER_FEATURE(Features12, descriptorBindingStorageImageUpdateAfterBind),
				EMBER_FEATURE(Features12, descriptorBindingStorageBufferUpdateAfterBind),
				EMBER_FEATURE(Features12, descriptorBindingUpdateUnusedWhilePending),
				EMBER_FEATURE(Features12, shaderSampledImageArrayNonUniformIndexing),
				EMBER_FEATURE(Features12, shaderStorageBufferArrayNonUniformIndexing),
				EMBER_FEATURE(Features12, shaderStorageImageArrayNonUniformIndexing),
				EMBER_FEATURE(Features12, scalarBlockLayout),
				// GPU-driven rendering pulls vertices and draw records through device
				// addresses, and D3D12 mandates GPUVAs: optionality here would be a lie.
				EMBER_FEATURE(Features12, bufferDeviceAddress),
			};

			static constexpr FeatureRef<Features13> REQUIRED_13[] = {
				// 1.3 core-mandatory
				EMBER_FEATURE(Features13, dynamicRendering),
				EMBER_FEATURE(Features13, synchronization2),
				EMBER_FEATURE(Features13, maintenance4),
			};

#undef EMBER_FEATURE

			[[nodiscard]] static VkBaseOutStructure* append(VkBaseOutStructure* tail, void* next) noexcept
			{
				tail->pNext = static_cast<VkBaseOutStructure*>(next);
				return tail->pNext;
			}

			/**
			 * Scans the whole table before answering, so an under-spec adapter logs
			 * its complete gap list in one boot instead of one feature per attempt.
			 */
			template <typename S, size_t N>
			[[nodiscard]] static bool
			check(const S& available, const FeatureRef<S> (&required)[N], const char* adapter_name) noexcept
			{
				bool ok = true;

				for (const FeatureRef<S>& feature : required)
				{
					if (available.*feature.flag != VK_TRUE)
					{
						EMBER_INFO("vulkan: skipping {}: missing feature {}", adapter_name, feature.name);
						ok = false;
					}
				}

				return ok;
			}

			template <typename S, size_t N> static void enable(S& enabled, const FeatureRef<S> (&required)[N]) noexcept
			{
				for (const FeatureRef<S>& feature : required)
					enabled.*feature.flag = VK_TRUE;
			}

			VkPhysicalDeviceFeatures2 m_features2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			VkPhysicalDeviceVulkan11Features m_vulkan11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
			VkPhysicalDeviceVulkan12Features m_vulkan12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
			VkPhysicalDeviceVulkan13Features m_vulkan13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
		};

		/// Convenience wrapper for Vulkan's count-then-fetch dance.
		template <class T, class F> [[nodiscard]] Vector<T> enumerate(F&& fetch) noexcept
		{
			Vector<T> out(&memory::heap(MemoryTag::Graphics));
			u32 count = 0;

			if (fetch(&count, nullptr) != VK_SUCCESS || count == 0)
				return out;

			out.resize(count);

			if (fetch(&count, out.data()) != VK_SUCCESS)
				out.clear();

			return out;
		}

		[[nodiscard]] bool has_layer(Span<const VkLayerProperties> layers, const char* name) noexcept
		{
			return std::any_of(
				layers.begin(),
				layers.end(),
				[name](const VkLayerProperties& layer) { return std::strcmp(layer.layerName, name) == 0; });
		}

		[[nodiscard]] bool has_extension(Span<const VkExtensionProperties> extensions, const char* name) noexcept
		{
			return std::any_of(
				extensions.begin(),
				extensions.end(),
				[name](const VkExtensionProperties& ext) { return std::strcmp(ext.extensionName, name) == 0; });
		}

		/// Appends the instance extensions exposed by `layer` (null = loader + implicit layers)
		void append_instance_extensions(const char* layer, Vector<VkExtensionProperties>& out) noexcept
		{
			const auto found = enumerate<VkExtensionProperties>(
				[layer](u32* count, VkExtensionProperties* data)
				{ return vkEnumerateInstanceExtensionProperties(layer, count, data); });

			out.insert(out.end(), found.begin(), found.end());
		}

		VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
			VkDebugUtilsMessageSeverityFlagBitsEXT severity,
			VkDebugUtilsMessageTypeFlagsEXT /*types*/,
			const VkDebugUtilsMessengerCallbackDataEXT* data,
			void* user) noexcept
		{
			DebugState& debug = *static_cast<DebugState*>(user);
			const char* id	  = data->pMessageIdName != nullptr ? data->pMessageIdName : "?";

			if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
			{
				debug.errors.fetch_add(1, std::memory_order_relaxed);
				EMBER_ERROR("vulkan[{}]: {}", id, data->pMessage);

				// Breaks inside the offending vkCmd*/vkCreate* call: the call stack is
				// the diagnosis. Works in every build, not just ones with asserts.
				if (debug.break_on_error)
					EMBER_DEBUG_BREAK();
			}
			else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
			{
				debug.warnings.fetch_add(1, std::memory_order_relaxed);
				EMBER_WARN("vulkan[{}]: {}", id, data->pMessage);
			}
			else
			{
				EMBER_TRACE("vulkan[{}]: {}", id, data->pMessage);
			}

			return VK_FALSE; // never abort the call: the layer's job is to report
		}

		constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

		/// Loader -> layers/extensions -> instance -> volk instance table -> debug messenger.
		[[nodiscard]] bool create_instance(Context& ctx, const DeviceDef& def) noexcept
		{
			/// Loader. With a Platform*, SDL owns the Vulkan library so our calls and its
			/// surface creation share one loader instance; headless boots dlopen it via volk.
			if (ctx.platform != nullptr)
			{
				const auto proc = platform::vk::get_instance_proc_addr();

				if (proc == nullptr)
				{
					EMBER_ERROR("vulkan: platform provided no Vulkan loader");
					return false;
				}

				volkInitializeCustom(proc);
			}
			else if (auto result = volkInitialize(); result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: no Vulkan loader on this system ({})", vk::result_name(result));
				return false;
			}

			u32 loader_version = volkGetInstanceVersion();
			if (loader_version < vk::API_VERSION)
			{
				EMBER_ERROR(
					"vulkan: loader supports {}.{}, Vulkan 1.3 is required",
					VK_API_VERSION_MAJOR(loader_version),
					VK_API_VERSION_MINOR(loader_version));
				return false;
			}

			const auto layers = enumerate<VkLayerProperties>(vkEnumerateInstanceLayerProperties);

			Vector<VkExtensionProperties> extensions(&memory::heap(MemoryTag::Graphics));
			append_instance_extensions(nullptr, extensions);

			Vector<const char*> enabled_layers(&memory::heap(MemoryTag::Graphics));
			Vector<const char*> enabled_extensions(&memory::heap(MemoryTag::Graphics));

			// Boot-local by the audit: nothing reads this after create_instance returns,
			// so the layer's presence is logged here and stored nowhere.
			bool validation = false;

			if (def.enable_validation)
			{
				if (has_layer(layers, VALIDATION_LAYER))
				{
					enabled_layers.push_back(VALIDATION_LAYER);
					validation = true;

					// The layer contributes extensions of its own (VK_EXT_validation_features).
					append_instance_extensions(VALIDATION_LAYER, extensions);

					EMBER_INFO("vulkan: validation enabled{}", def.enable_sync_validation ? " (+synchronization)" : "");
				}
				else
				{
					EMBER_WARN("vulkan: {} not installed, running without validation", VALIDATION_LAYER);
				}
			}

			// Debug utils stays on in every build when available: object names and pass labels
			// in RenderDoc/Nsight cost nothing measurable.
			if (has_extension(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
			{
				enabled_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
				ctx.debug_utils = true;
			}

			// Window-system extensions are a hard requirement only when a window system
			// exists; a headless boot legitimately has none.
			if (ctx.platform != nullptr)
			{
				const Span<const char* const> wsi = platform::vk::instance_extensions();

				if (wsi.empty())
				{
					EMBER_ERROR("vulkan: platform reports no Vulkan window-system support");
					return false;
				}

				for (const char* name : wsi)
				{
					if (!has_extension(extensions, name))
					{
						EMBER_ERROR("vulkan: window system needs {} which this loader lacks", name);
						return false;
					}

					enabled_extensions.push_back(name);
				}
			}

			// pNext chain. Passing the messenger info to vkCreateInstance makes instance
			// creation and destruction themselves validated; the same struct creates the
			// persistent messenger below.
			const void* next = nullptr;

			VkDebugUtilsMessengerCreateInfoEXT messenger_info{
				.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
				.messageSeverity =
					VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
				.messageType =
					VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
				.pfnUserCallback = debug_callback,
				.pUserData		 = &debug_state(),
			};

			if (ctx.debug_utils)
			{
				messenger_info.pNext = next;
				next				 = &messenger_info;
			}

			const VkValidationFeatureEnableEXT sync_validation[] = {
				VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};

			VkValidationFeaturesEXT validation_features{
				.sType						   = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
				.enabledValidationFeatureCount = 1,
				.pEnabledValidationFeatures	   = sync_validation,
			};

			// Sync validation configures the layer, so it is pointless without it.
			if (validation && def.enable_sync_validation)
			{
				if (has_extension(extensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME))
				{
					enabled_extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
					validation_features.pNext = next;
					next					  = &validation_features;
				}
				else
				{
					EMBER_WARN(
						"vulkan: synchronization validation requested but {} is unavailable",
						VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
				}
			}

			VkApplicationInfo app_info{
				.sType				= VK_STRUCTURE_TYPE_APPLICATION_INFO,
				.pApplicationName	= def.app_name,
				.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
				.pEngineName		= "Ember",
				.engineVersion		= VK_MAKE_VERSION(0, 1, 0),
				.apiVersion			= vk::API_VERSION,
			};

			VkInstanceCreateInfo info{
				.sType					 = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
				.pNext					 = next,
				.pApplicationInfo		 = &app_info,
				.enabledLayerCount		 = static_cast<u32>(enabled_layers.size()),
				.ppEnabledLayerNames	 = enabled_layers.data(),
				.enabledExtensionCount	 = static_cast<u32>(enabled_extensions.size()),
				.ppEnabledExtensionNames = enabled_extensions.data(),
			};

			if (auto result = vkCreateInstance(&info, nullptr, &ctx.instance); result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: vkCreateInstance failed: {}", vk::result_name(result));
				ctx.instance = VK_NULL_HANDLE;
				return false;
			}

			// instance-level entry points only; device ones load per device in create_device,
			// skipping the loader's runtime dispatch trampoline entirely.
			volkLoadInstanceOnly(ctx.instance);

			if (ctx.debug_utils)
			{
				messenger_info.pNext = nullptr;

				if (vkCreateDebugUtilsMessengerEXT(ctx.instance, &messenger_info, nullptr, &ctx.messenger) !=
					VK_SUCCESS)
				{
					EMBER_WARN("vulkan: debug messenger creation failed; continuing without it");
					ctx.messenger = VK_NULL_HANDLE;
				}
			}

			return true;
		}

		/// Everything adapter selection learns and boot consumes. Boot scratch: dies when
		/// boot() returns; nothing here survives into Context except through caps.
		struct AdapterInfo
		{
			VkPhysicalDevice handle = VK_NULL_HANDLE;
			VkPhysicalDeviceProperties properties{};
			VkPhysicalDeviceMemoryProperties memory_properties{};

			u32 graphics_family = VK_QUEUE_FAMILY_IGNORED;
			u32 compute_family	= VK_QUEUE_FAMILY_IGNORED; // dedicated (no graphics); optional
			u32 transfer_family = VK_QUEUE_FAMILY_IGNORED; // dedicated (no graphics/compute); optional

			bool mesh_shader	= false;
			bool memory_budget	= false;
			bool wireframe		= false; // fillModeNonSolid
			bool indirect_count = false; // vulkan12.drawIndirectCount
			bool sampler_minmax = false; // vulkan12.samplerFilterMinmax

			u32 subgroup_size											   = 0;
			char driver[VK_MAX_DRIVER_NAME_SIZE + VK_MAX_DRIVER_INFO_SIZE] = {};
		};

		[[nodiscard]] AdapterKind to_adapter_kind(VkPhysicalDeviceType type) noexcept
		{
			switch (type)
			{
				case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
					return AdapterKind::Integrated;
				case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
					return AdapterKind::Discrete;
				case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
					return AdapterKind::Virtual;
				case VK_PHYSICAL_DEVICE_TYPE_CPU:
					return AdapterKind::Cpu;
				default:
					return AdapterKind::Other;
			}
		}

		/// Higher wins; strictly-greater comparison keeps enumeration order on ties (deterministic).
		[[nodiscard]] int adapter_score(AdapterKind kind, AdapterPreference preference) noexcept
		{
			switch (preference)
			{
				case AdapterPreference::Any:
					return 1;

				case AdapterPreference::Integrated:
					switch (kind)
					{
						case AdapterKind::Integrated:
							return 4;
						case AdapterKind::Discrete:
							return 3;
						case AdapterKind::Virtual:
							return 2;
						case AdapterKind::Cpu:
							return 1;
						default:
							return 0;
					}

				case AdapterPreference::Discrete:
				default:
					switch (kind)
					{
						case AdapterKind::Discrete:
							return 4;
						case AdapterKind::Integrated:
							return 3;
						case AdapterKind::Virtual:
							return 2;
						case AdapterKind::Cpu:
							return 1;
						default:
							return 0;
					}
			}
		}

		[[nodiscard]] u32 find_queue_family(
			Span<const VkQueueFamilyProperties> families, VkQueueFlags required, VkQueueFlags forbidden) noexcept
		{
			for (u32 i = 0; i < families.size(); ++i)
			{
				const VkQueueFlags flags = families[i].queueFlags;

				if ((flags & required) == required && (flags & forbidden) == 0)
					return i;
			}

			return VK_QUEUE_FAMILY_IGNORED;
		}

		/// Rejects adapters that cannot run ember's contract. Fills `out` for the ones that can.
		[[nodiscard]] bool query_adapter(const Context& ctx, VkPhysicalDevice handle, AdapterInfo& out) noexcept
		{
			out.handle = handle;
			vkGetPhysicalDeviceProperties(handle, &out.properties);
			vkGetPhysicalDeviceMemoryProperties(handle, &out.memory_properties);

			const char* name = out.properties.deviceName;

			if (out.properties.apiVersion < vk::API_VERSION)
			{
				EMBER_INFO(
					"vulkan: skipping {}: Vulkan {}.{} < 1.3",
					name,
					VK_API_VERSION_MAJOR(out.properties.apiVersion),
					VK_API_VERSION_MINOR(out.properties.apiVersion));
				return false;
			}

			const auto extensions = enumerate<VkExtensionProperties>(
				[handle](u32* count, VkExtensionProperties* data)
				{ return vkEnumerateDeviceExtensionProperties(handle, nullptr, count, data); });

			if (ctx.platform != nullptr && !has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
			{
				EMBER_INFO("vulkan: skipping {}: no {}", name, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
				return false;
			}

			out.mesh_shader	  = has_extension(extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
			out.memory_budget = has_extension(extensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

			// Features: reject on missing required ones, logging every gap in a single pass.
			FeatureChain available{};
			available.query(handle);

			if (!available.check_required(name))
				return false;

			// Optional features, recorded while the FeatureChain query is available.
			out.wireframe	   = available.features10().fillModeNonSolid == VK_TRUE;
			out.indirect_count = available.vulkan12().drawIndirectCount == VK_TRUE;
			out.sampler_minmax = available.vulkan12().samplerFilterMinmax == VK_TRUE;

			// Queues. The main family must do graphics+compute (universal on desktop) and, when a
			// window system exists, present: a separate present queue would buy queue-ownership
			// transfers every frame for zero real-world benefit on PC.
			const auto families = enumerate<VkQueueFamilyProperties>(
				[handle](u32* count, VkQueueFamilyProperties* data)
				{
					vkGetPhysicalDeviceQueueFamilyProperties(handle, count, data);
					return VK_SUCCESS; // void query: adapted to the shared protocol
				});

			for (u32 i = 0; i < families.size(); ++i)
			{
				constexpr VkQueueFlags GRAPHICS_COMPUTE = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;

				if ((families[i].queueFlags & GRAPHICS_COMPUTE) != GRAPHICS_COMPUTE)
					continue;

				if (ctx.platform != nullptr && !platform::vk::presentation_supported(ctx.instance, handle, i))
					continue;

				out.graphics_family = i;
				break;
			}

			if (out.graphics_family == VK_QUEUE_FAMILY_IGNORED)
			{
				EMBER_INFO(
					"vulkan: skipping {}: no graphics+compute{} queue family",
					name,
					ctx.platform != nullptr ? "+present" : "");
				return false;
			}

			// Dedicated families for future async work; absent on some adapters, and that's fine.
			out.compute_family = find_queue_family(families, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
			out.transfer_family =
				find_queue_family(families, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);

			// Properties beyond 1.0: subgroup size for caps, driver name/info for the boot log.
			VkPhysicalDeviceDriverProperties driver{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
			};
			VkPhysicalDeviceVulkan11Properties props11{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
				.pNext = &driver,
			};
			VkPhysicalDeviceProperties2 props2{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
				.pNext = &props11,
			};
			vkGetPhysicalDeviceProperties2(handle, &props2);

			out.subgroup_size = props11.subgroupSize;
			std::snprintf(out.driver, sizeof(out.driver), "%s %s", driver.driverName, driver.driverInfo);

			return true;
		}

		/**
		 * Filter-then-score: query_adapter rejects anything that cannot run the contract, then
		 * the best-scoring survivor wins. Strictly-greater keeps enumeration order on ties.
		 */
		[[nodiscard]] bool select_adapter(const Context& ctx, const DeviceDef& def, AdapterInfo& out) noexcept
		{
			const auto adapters =
				enumerate<VkPhysicalDevice>([&ctx](u32* count, VkPhysicalDevice* data)
											{ return vkEnumeratePhysicalDevices(ctx.instance, count, data); });

			if (adapters.empty())
			{
				EMBER_ERROR("vulkan: no physical devices");
				return false;
			}

			int best_score = -1;

			for (VkPhysicalDevice handle : adapters)
			{
				AdapterInfo candidate{};

				if (!query_adapter(ctx, handle, candidate))
					continue;

				const int score = adapter_score(to_adapter_kind(candidate.properties.deviceType), def.adapter);

				// Copy-on-better on purpose: boot reads the winner's full AdapterInfo.
				if (score > best_score)
				{
					best_score = score;
					out		   = candidate;
				}
			}

			if (best_score < 0)
			{
				EMBER_ERROR("vulkan: no adapter satisfies the Vulkan 1.3 feature set ember requires");
				return false;
			}

			return true;
		}

		/**
		 * Logical device with exactly the required tables plus available optionals — never
		 * "everything the adapter has": unused features (robustBufferAccess, ...) can cost real
		 * GPU time. One queue per distinct family; dedicated compute/transfer families are
		 * created now so async work needs no boot changes later.
		 */
		[[nodiscard]] bool create_device(Context& ctx, const AdapterInfo& adapter) noexcept
		{
			const f32 priority = 1.0f;
			VkDeviceQueueCreateInfo queue_infos[3]{};
			u32 queue_count = 0;

			// present_family == graphics_family by the selection contract, so three slots suffice.
			const auto add_queue = [&](u32 family)
			{
				if (family == VK_QUEUE_FAMILY_IGNORED)
					return;

				for (u32 i = 0; i < queue_count; ++i)
					if (queue_infos[i].queueFamilyIndex == family)
						return;

				queue_infos[queue_count++] = {
					.sType			  = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
					.queueFamilyIndex = family,
					.queueCount		  = 1,
					.pQueuePriorities = &priority,
				};
			};

			add_queue(adapter.graphics_family);
			add_queue(adapter.compute_family);
			add_queue(adapter.transfer_family);

			// Required features come from the same tables that vetted the adapter;
			// optional availability was recorded in AdapterInfo by query_adapter.
			FeatureChain enabled{};
			enabled.enable_required();

			if (adapter.indirect_count)
				enabled.vulkan12().drawIndirectCount = VK_TRUE;

			if (adapter.sampler_minmax)
				enabled.vulkan12().samplerFilterMinmax = VK_TRUE;

			if (adapter.wireframe)
				enabled.features10().fillModeNonSolid = VK_TRUE;

			const char* extensions[3];
			u32 extension_count = 0;

			if (ctx.platform != nullptr)
				extensions[extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

			if (adapter.memory_budget)
				extensions[extension_count++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;

			VkDeviceCreateInfo info{
				.sType					 = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
				.pNext					 = enabled.head(),
				.queueCreateInfoCount	 = queue_count,
				.pQueueCreateInfos		 = queue_infos,
				.enabledExtensionCount	 = extension_count,
				.ppEnabledExtensionNames = extensions,
			};

			if (auto result = vkCreateDevice(adapter.handle, &info, nullptr, &ctx.device); result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: vkCreateDevice failed: {}", vk::result_name(result));
				ctx.device = VK_NULL_HANDLE;
				return false;
			}

			// Direct device-level entry points: no per-call dispatch through the loader.
			volkLoadDevice(ctx.device);

			const auto fetch_queue = [&](u32 family, Queue& queue)
			{
				queue.family = family;
				vkGetDeviceQueue(ctx.device, family, 0, &queue.handle);
			};

			fetch_queue(adapter.graphics_family, ctx.graphics);

			if (adapter.compute_family != VK_QUEUE_FAMILY_IGNORED)
				fetch_queue(adapter.compute_family, ctx.compute);
			else
				ctx.compute = ctx.graphics;

			if (adapter.transfer_family != VK_QUEUE_FAMILY_IGNORED)
				fetch_queue(adapter.transfer_family, ctx.transfer);
			else
				ctx.transfer = ctx.graphics;

			vk::set_name(ctx, VK_OBJECT_TYPE_DEVICE, (u64)ctx.device, "ember.device");
			vk::set_name(ctx, VK_OBJECT_TYPE_QUEUE, (u64)ctx.graphics.handle, "ember.queue.graphics");

			return true;
		}

		/// VMA fetches every entry point through volk's two loaders; nothing links libvulkan.
		[[nodiscard]] bool create_allocator(Context& ctx, const AdapterInfo& adapter) noexcept
		{
			VmaVulkanFunctions functions{};
			functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
			functions.vkGetDeviceProcAddr	= vkGetDeviceProcAddr;
			functions.vkCreateImage			= vkCreateImage;

			// Assignment, not designated init: VMA's member order is not stable across versions.
			VmaAllocatorCreateInfo info{};
			info.flags			  = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
			info.physicalDevice	  = ctx.adapter;
			info.device			  = ctx.device;
			info.instance		  = ctx.instance;
			info.pVulkanFunctions = &functions;
			info.vulkanApiVersion = vk::API_VERSION;

			if (adapter.memory_budget)
				info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;

			if (const VkResult result = vmaCreateAllocator(&info, &ctx.allocator); result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: vmaCreateAllocator failed: {}", vk::result_name(result));
				ctx.allocator = VK_NULL_HANDLE;
				return false;
			}

			return true;
		}

		/**
		 * Everything user code and the backend consult at runtime, derived once from what
		 * adapter selection learned. Sole writer of DeviceCaps — a pure AdapterInfo ->
		 * DeviceCaps transform: no device, no handles, no Vulkan calls.
		 */
		void fill_caps(DeviceCaps& caps, const AdapterInfo& adapter) noexcept
		{
			const VkPhysicalDeviceLimits& limits = adapter.properties.limits;

			std::snprintf(
				caps.adapter_name,
				sizeof(caps.adapter_name),
				"%.*s",
				static_cast<int>(sizeof(caps.adapter_name) - 1),
				adapter.properties.deviceName);
			caps.vendor_id	  = adapter.properties.vendorID;
			caps.device_id	  = adapter.properties.deviceID;
			caps.api_version  = adapter.properties.apiVersion;
			caps.adapter_kind = to_adapter_kind(adapter.properties.deviceType);

			// Memory: total device-local, and whether a large DEVICE_LOCAL|HOST_VISIBLE type
			// exists. A 256 MB heap is the pre-ReBAR BAR window; anything larger is ReBAR/SAM
			// or unified memory, which lets the transient ring live in VRAM.
			const VkPhysicalDeviceMemoryProperties& mem = adapter.memory_properties;

			u64 device_local_bytes = 0;
			for (u32 i = 0; i < mem.memoryHeapCount; ++i)
				if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
					device_local_bytes += mem.memoryHeaps[i].size;
			caps.device_local_bytes = device_local_bytes;

			for (u32 i = 0; i < mem.memoryTypeCount; ++i)
			{
				constexpr VkMemoryPropertyFlags LOCAL_VISIBLE =
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

				const VkMemoryType& type = mem.memoryTypes[i];

				if ((type.propertyFlags & LOCAL_VISIBLE) == LOCAL_VISIBLE &&
					mem.memoryHeaps[type.heapIndex].size > 256_mb)
					caps.host_visible_device_local = true;
			}

			// 64 KB is D3D12's constant-buffer-view size limit; clamping now keeps constant
			// blocks portable to the DX12 backend unchanged.
			constexpr u32 D3D12_CONSTANT_BLOCK_LIMIT = 64u * 1024u;

			caps.constant_buffer_offset_alignment = static_cast<u32>(limits.minUniformBufferOffsetAlignment);
			caps.storage_buffer_offset_alignment  = static_cast<u32>(limits.minStorageBufferOffsetAlignment);
			caps.max_constant_block_bytes = std::min<u32>(limits.maxUniformBufferRange, D3D12_CONSTANT_BLOCK_LIMIT);
			caps.copy_row_pitch_alignment = static_cast<u32>(limits.optimalBufferCopyRowPitchAlignment);
			caps.copy_offset_alignment	  = static_cast<u32>(limits.optimalBufferCopyOffsetAlignment);

			caps.max_texture_2d		   = limits.maxImageDimension2D;
			caps.max_texture_3d		   = limits.maxImageDimension3D;
			caps.max_texture_layers	   = limits.maxImageArrayLayers;
			caps.max_color_attachments = std::min(limits.maxColorAttachments, MAX_COLOR_ATTACHMENTS);
			caps.max_anisotropy		   = static_cast<u32>(limits.maxSamplerAnisotropy);
			caps.subgroup_size		   = adapter.subgroup_size;

			caps.timestamps			 = limits.timestampComputeAndGraphics == VK_TRUE && limits.timestampPeriod > 0.0f;
			caps.timestamp_period_ns = limits.timestampPeriod;

			// Optional features: availability recorded by query_adapter, enabled by
			// create_device, reported here. One query, no drift.
			caps.wireframe		= adapter.wireframe;
			caps.indirect_count = adapter.indirect_count;
			caps.sampler_minmax = adapter.sampler_minmax;
			caps.mesh_shaders	= adapter.mesh_shader;

			caps.ray_tracing = false; // reserved

			// Contract sanity: guaranteed by spec minimums, asserted so a broken driver is loud.
			EMBER_ASSERT(limits.maxPushConstantsSize >= PUSH_CONSTANT_BYTES);
			EMBER_ASSERT(limits.maxBoundDescriptorSets >= 2);
		}

		[[nodiscard]] bool create_frame_resources(const Context& ctx, FrameState& frame) noexcept
		{
			VkSemaphoreTypeCreateInfo type_info{
				.sType		   = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
				.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
				.initialValue  = 0,
			};

			VkSemaphoreCreateInfo semaphore_info{
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
				.pNext = &type_info,
			};

			VkSemaphore timeline = VK_NULL_HANDLE;
			VkResult result		 = vkCreateSemaphore(ctx.device, &semaphore_info, nullptr, &timeline);

			if (result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: timeline semaphore creation failed: {}", vk::result_name(result));
				return false;
			}

			frame.timeline = timeline;
			vk::set_name(ctx, VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<u64>(timeline), "ember.frame_timeline");

			for (u32 i = 0; i < ctx.frames_in_flight; ++i)
			{
				FrameSlot& slot = frame.slots[i];
				VkCommandPoolCreateInfo pool_info{
					.sType			  = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
					.queueFamilyIndex = ctx.graphics.family,
				};

				VkCommandPool pool = VK_NULL_HANDLE;
				result			   = vkCreateCommandPool(ctx.device, &pool_info, nullptr, &pool);

				if (result != VK_SUCCESS)
				{
					EMBER_ERROR("vulkan: frame command pool creation failed: {}", vk::result_name(result));
					return false;
				}

				// Publish immediately so centralized rollback can destroy it.
				slot.pool = pool;
				VkCommandBufferAllocateInfo allocation_info{
					.sType				= VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
					.commandPool		= pool,
					.level				= VK_COMMAND_BUFFER_LEVEL_PRIMARY,
					.commandBufferCount = 1,
				};

				VkCommandBuffer commands = VK_NULL_HANDLE;
				result					 = vkAllocateCommandBuffers(ctx.device, &allocation_info, &commands);
				if (result != VK_SUCCESS)
				{
					EMBER_ERROR("vulkan: frame command-buffer allocation failed: {}", vk::result_name(result));
					return false;
				}

				slot.commands = commands;
			}

			return true;
		}

		/**
		 * DeviceLimits' documented contract ("each must be <= 65535", samplers within the
		 * bindless heap, swapchains within the fixed present arrays), enforced at the one
		 * boundary where a def enters the system. is_valid(def) asserts the same bounds in
		 * debug builds; the clamps keep release builds inside the arrays.
		 */
		[[nodiscard]] DeviceLimits sanitize(DeviceLimits limits) noexcept
		{
			const auto clamp_limit = [](u32& value, u32 max, const char* what)
			{
				if (value > max)
				{
					EMBER_WARN("gpu: DeviceLimits::{} clamped from {} to {}", what, value, max);
					value = max;
				}
			};

			constexpr u32 MAX_POOL_CAPACITY = std::numeric_limits<u16>::max();

			clamp_limit(limits.max_buffers, MAX_POOL_CAPACITY, "max_buffers");
			clamp_limit(limits.max_textures, MAX_POOL_CAPACITY, "max_textures");
			clamp_limit(limits.max_samplers, MAX_BINDLESS_SAMPLERS, "max_samplers");
			clamp_limit(limits.max_graphics_pipelines, MAX_POOL_CAPACITY, "max_graphics_pipelines");
			clamp_limit(limits.max_compute_pipelines, MAX_POOL_CAPACITY, "max_compute_pipelines");
			clamp_limit(limits.max_swapchains, MAX_SWAPCHAINS, "max_swapchains");

			return limits;
		}
	}

	namespace vk
	{
		bool boot(Backend& backend, const DeviceDef& def) noexcept
		{
			Context& ctx	  = backend.context; // the one mutable Context reference in the codebase
			FrameState& frame = backend.frame;

			ctx.platform		 = def.platform;
			ctx.frames_in_flight = std::clamp(def.frames_in_flight, 1u, MAX_FRAMES_IN_FLIGHT);

			debug_state().break_on_error = def.break_on_validation_error;

			backend.resources.reserve(sanitize(def.limits));

			if (!create_instance(ctx, def))
				return false;

			AdapterInfo adapter{};
			if (!select_adapter(ctx, def, adapter))
				return false;

			ctx.adapter = adapter.handle;

			if (!create_device(ctx, adapter) || !create_allocator(ctx, adapter))
				return false;

			fill_caps(ctx.caps, adapter);

			if (!create_frame_resources(ctx, frame))
				return false;

			if (!vk::transient_boot(backend, def.transient_ring_bytes))
				return false;

			if (!vk::staging_boot(backend, def.staging_ring_bytes))
				return false;

			// The DestroyQueue uses the frame's timeline_value to determine when resources
			// can be freed safely (timeline_value + 1 signal).
			backend.destroy_queue.bind(backend.frame);

			EMBER_INFO(
				"vulkan: {} ({}) | {} | Vulkan {}.{}.{} | {} MB local{}{}",
				ctx.caps.adapter_name,
				enum_names<AdapterKind>()[static_cast<u32>(ctx.caps.adapter_kind)],
				adapter.driver,
				VK_API_VERSION_MAJOR(ctx.caps.api_version),
				VK_API_VERSION_MINOR(ctx.caps.api_version),
				VK_API_VERSION_PATCH(ctx.caps.api_version),
				ctx.caps.device_local_bytes / (1024 * 1024),
				ctx.caps.host_visible_device_local ? " (ReBAR)" : "",
				ctx.caps.mesh_shaders ? " | mesh shaders" : "");

			return true;
		}

		void destroy_boot_state(Backend& backend) noexcept
		{
			Context& ctx = backend.context;

			if (ctx.device != VK_NULL_HANDLE)
			{
				vk::staging_destroy(ctx, backend.staging);

				for (FrameSlot& slot : backend.frame.slots)
					if (slot.pool != VK_NULL_HANDLE)
						vkDestroyCommandPool(ctx.device, slot.pool, nullptr);

				if (backend.frame.timeline != VK_NULL_HANDLE)
					vkDestroySemaphore(ctx.device, backend.frame.timeline, nullptr);

				if (ctx.allocator != VK_NULL_HANDLE)
					vmaDestroyAllocator(ctx.allocator);

				vkDestroyDevice(ctx.device, nullptr);
			}

			if (ctx.messenger != VK_NULL_HANDLE)
				vkDestroyDebugUtilsMessengerEXT(ctx.instance, ctx.messenger, nullptr);

			if (ctx.instance != VK_NULL_HANDLE)
				vkDestroyInstance(ctx.instance, nullptr);

			volkFinalize(); // safe when volk never initialized: it just nulls pointers

			if (ctx.platform != nullptr)
				platform::vk::release_loader();

			// Leave no dangling handles behind for a stray late reader.
			ctx			  = {};
			backend.frame = {};
		}
	}
}
