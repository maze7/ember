#include "ember/gpu/common.h"
#include "ember/sync/thread.h"
#include "platform/vulkan/wsi.h"
#include <ember/core/common.h>
#include <ember/core/profile.h>
#include <ember/gpu/device.h>
#include <gpu/vulkan/common.h>
#include <gpu/vulkan/device_state.h>
#include <gpu/vulkan/resources.h>
#include <vulkan/vulkan_core.h>

namespace ember::gpu
{
	namespace
	{
		/// One device at a time: pool indicies are bindless slots, so two devices sharing
		/// handle types would alias each other's heaps. Mirrors the Platform claim guard.
		constinit std::atomic<bool> s_device_claimed{false};

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

		/// Appends `next` to a pNext chain and advances the tail.
		inline void chain_append(VkBaseOutStructure*& tail, void* next) noexcept
		{
			tail->pNext = static_cast<VkBaseOutStructure*>(next);
			tail		= tail->pNext;
		}

		/**
		 * Every feature struct ember queries or enables, as plain members.
		 * The REQUIRED_* tables point into these; build_feature_chain() wires the pNext chain.
		 *
		 * A new extension is one member here, one conditional in build_feature_chain(), and its
		 * table entries.
		 */
		struct FeatureSet
		{
			VkPhysicalDeviceFeatures2 features2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
			VkPhysicalDeviceVulkan11Features vulkan11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
			VkPhysicalDeviceVulkan12Features vulkan12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
			VkPhysicalDeviceVulkan13Features vulkan13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
			VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};

			// Copying a wired set would dangle its internal chain; keep it impossible.
			FeatureSet() noexcept					 = default;
			FeatureSet(const FeatureSet&)			 = delete;
			FeatureSet& operator=(const FeatureSet&) = delete;
		};

		/**
		 * Wires the chain and returns its head, ready for vkGetPhysicalDeviceFeatures2 or
		 * VkDeviceCreateInfo::pNext. Wire each instance one. Extension structs join only when the
		 * adapter offers the extension: for device creation, a feature struct without its extension
		 * enabled is a validation error.
		 */
		[[nodiscard]] VkPhysicalDeviceFeatures2* build_feature_chain(FeatureSet& set, bool with_mesh_shader) noexcept
		{
			auto* tail = reinterpret_cast<VkBaseOutStructure*>(&set.features2);

			chain_append(tail, &set.vulkan11);
			chain_append(tail, &set.vulkan12);
			chain_append(tail, &set.vulkan13);

			if (with_mesh_shader)
				chain_append(tail, &set.mesh_shader);

			return &set.features2;
		}

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
		constexpr FeatureRef<Features10> REQUIRED_10[] = {
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

		constexpr FeatureRef<Features11> REQUIRED_11[] = {
			// Roadmap 2022 feature set
			EMBER_FEATURE(Features11, shaderDrawParameters),
		};

		constexpr FeatureRef<Features12> REQUIRED_12[] = {
			// 1.2 core-mandatory
			EMBER_FEATURE(Features12, timelineSemaphore),
			EMBER_FEATURE(Features12, hostQueryReset),
			// Roadmap 2022 feature set: the bindless heap and friends
			EMBER_FEATURE(Features12, descriptorIndexing),
			EMBER_FEATURE(Features12, runtimeDescriptorArray),
			EMBER_FEATURE(Features12, descriptorBindingPartiallyBound),
			EMBER_FEATURE(Features12, descriptorBindingSampledImageUpdateAfterBind),
			EMBER_FEATURE(Features12, descriptorBindingStorageImageUpdateAfterBind),
			EMBER_FEATURE(Features12, descriptorBindingStorageBufferUpdateAfterBind),
			EMBER_FEATURE(Features12, descriptorBindingUpdateUnusedWhilePending),
			EMBER_FEATURE(Features12, shaderSampledImageArrayNonUniformIndexing),
			EMBER_FEATURE(Features12, shaderStorageBufferArrayNonUniformIndexing),
			EMBER_FEATURE(Features12, shaderStorageImageArrayNonUniformIndexing),
			EMBER_FEATURE(Features12, scalarBlockLayout),
			/// GPU-driven rendering pulls vertices and draw records through device addresses;
			/// D3D12 mandattes GPUVAs, so bufferDeviceAddress is required.
			EMBER_FEATURE(Features12, bufferDeviceAddress),
		};

		constexpr FeatureRef<Features13> REQUIRED_13[] = {
			// 1.3 core-mandatory
			EMBER_FEATURE(Features13, dynamicRendering),
			EMBER_FEATURE(Features13, synchronization2),
			EMBER_FEATURE(Features13, maintenance4),
		};

#undef EMBER_FEATURE

		/**
		 * Scans a whole table before answering, so an under-spec adapter logs its complete gap
		 * list in one boot instead of one feature per attempt.
		 */
		template <typename S, size_t N>
		[[nodiscard]] bool
		check_features(const S& available, const FeatureRef<S> (&required)[N], const char* adapter_name) noexcept
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

		template <typename S, size_t N> void enable_features(S& enabled, const FeatureRef<S> (&required)[N]) noexcept
		{
			for (const FeatureRef<S>& feature : required)
				enabled.*feature.flag = VK_TRUE;
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
				// Breaks inside the offending vkCmd*/vkCreate* call: the call stack is the diagnosis.
				EMBER_ASSERT(!debug.break_on_error && "Vulkan validation error");
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

			if (debug.break_on_error)
				EMBER_DEBUG_BREAK();

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

			if (def.enable_validation)
			{
				if (has_layer(layers, VALIDATION_LAYER))
				{
					enabled_layers.push_back(VALIDATION_LAYER);

					// The layer contributes extensions of its own (VK_EXT_validation_features).
					append_instance_extensions(VALIDATION_LAYER, extensions);
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

			if (def.enable_sync_validation)
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

		/// Everything adapter selection learns and device creation consumes.
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
		[[nodiscard]] bool query_adapter(const Backend& state, VkPhysicalDevice handle, AdapterInfo& out) noexcept
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

			if (state.context.platform != nullptr && !has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
			{
				EMBER_INFO("vulkan: skipping {}: no {}", name, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
				return false;
			}

			out.mesh_shader	  = has_extension(extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
			out.memory_budget = has_extension(extensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

			// Features: reject on missing required ones, logging every gap in a single pass.
			FeatureSet available{};
			vkGetPhysicalDeviceFeatures2(handle, build_feature_chain(available, out.mesh_shader));

			bool features_ok  = true;
			features_ok		 &= check_features(available.features2.features, REQUIRED_10, name);
			features_ok		 &= check_features(available.vulkan11, REQUIRED_11, name);
			features_ok		 &= check_features(available.vulkan12, REQUIRED_12, name);
			features_ok		 &= check_features(available.vulkan13, REQUIRED_13, name);

			if (!features_ok)
				return false;

			// Extension present but its features aren't: treat as absent.
			if (out.mesh_shader &&
				(available.mesh_shader.meshShader != VK_TRUE || available.mesh_shader.taskShader != VK_TRUE))
				out.mesh_shader = false;

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

				if (state.context.platform != nullptr &&
					!platform::vk::presentation_supported(state.context.instance, handle, i))
					continue;

				out.graphics_family = i;
				break;
			}

			if (out.graphics_family == VK_QUEUE_FAMILY_IGNORED)
			{
				EMBER_INFO(
					"vulkan: skipping {}: no graphics+compute{} queue family",
					name,
					state.context.platform != nullptr ? "+present" : "");
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
		[[nodiscard]] bool select_adapter(const Backend& backend, const DeviceDef& def, AdapterInfo& out) noexcept
		{
			const auto adapters = enumerate<VkPhysicalDevice>(
				[&backend](u32* count, VkPhysicalDevice* data)
				{ return vkEnumeratePhysicalDevices(backend.context.instance, count, data); });

			if (adapters.empty())
			{
				EMBER_ERROR("vulkan: no physical devices");
				return false;
			}

			int best_score = -1;

			for (VkPhysicalDevice handle : adapters)
			{
				AdapterInfo candidate{};

				if (!query_adapter(backend, handle, candidate))
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

			FeatureSet available{};
			vkGetPhysicalDeviceFeatures2(adapter.handle, build_feature_chain(available, adapter.mesh_shader));

			FeatureSet enabled{};
			enable_features(enabled.features2.features, REQUIRED_10);
			enable_features(enabled.vulkan11, REQUIRED_11);
			enable_features(enabled.vulkan12, REQUIRED_12);
			enable_features(enabled.vulkan13, REQUIRED_13);

			// Optional features, enabled when present. Plain code, not a table: each has a
			// heterogeneous target (user-facing caps vs backend-only flags) that a uniform
			// table can't express without machinery outweighing four entries.
			if (available.vulkan12.drawIndirectCount == VK_TRUE)
			{
				enabled.vulkan12.drawIndirectCount = VK_TRUE;
				ctx.caps.indirect_count			   = true;
			}

			if (available.vulkan12.samplerFilterMinmax == VK_TRUE)
			{
				enabled.vulkan12.samplerFilterMinmax = VK_TRUE;
				ctx.caps.sampler_minmax				 = true;
			}

			if (available.vulkan12.bufferDeviceAddress == VK_TRUE)
			{
				enabled.vulkan12.bufferDeviceAddress = VK_TRUE;
				ctx.caps.buffer_device_address		 = true;
			}

			if (available.features2.features.fillModeNonSolid == VK_TRUE)
			{
				enabled.features2.features.fillModeNonSolid = VK_TRUE;
				ctx.caps.wireframe							= true;
			}

			const char* extensions[3];
			u32 extension_count = 0;

			if (ctx.platform != nullptr)
				extensions[extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

			// Extension-gated features: the extension string and its feature structs travel
			// together, which is why mesh shaders live here and not in the tables.
			if (adapter.mesh_shader)
			{
				extensions[extension_count++]  = VK_EXT_MESH_SHADER_EXTENSION_NAME;
				enabled.mesh_shader.taskShader = VK_TRUE;
				enabled.mesh_shader.meshShader = VK_TRUE;
				ctx.caps.mesh_shaders		   = true;
			}

			if (adapter.memory_budget)
			{
				extensions[extension_count++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
			}

			VkDeviceCreateInfo info{
				.sType					 = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
				.pNext					 = build_feature_chain(enabled, adapter.mesh_shader),
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
		[[nodiscard]] bool create_allocator(Context& ctx) noexcept
		{
			VmaVulkanFunctions functions{};
			functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
			functions.vkGetDeviceProcAddr	= vkGetDeviceProcAddr;

			// Assignment, not designated init: VMA's member order is not stable across versions.
			VmaAllocatorCreateInfo info{};
			info.physicalDevice	  = ctx.adapter;
			info.device			  = ctx.device;
			info.instance		  = ctx.instance;
			info.pVulkanFunctions = &functions;
			info.vulkanApiVersion = vk::API_VERSION;

			if (ctx.caps.buffer_device_address)
				info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

			if (ctx.caps.memory_budget)
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
			caps.max_constant_block_bytes		  = std::min<u32>(limits.maxUniformBufferRange, D3D12_CONSTANT_BLOCK_LIMIT);
			caps.copy_row_pitch_alignment		  = static_cast<u32>(limits.optimalBufferCopyRowPitchAlignment);
			caps.copy_offset_alignment			  = static_cast<u32>(limits.optimalBufferCopyOffsetAlignment);

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

		[[nodiscard]] bool create_frame_resources(Backend& backend) noexcept
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
			VkResult result		 = vkCreateSemaphore(backend.context.device, &semaphore_info, nullptr, &timeline);

			if (result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: timeline semaphore creation failed: {}", vk::result_name(result));
				return false;
			}

			backend.timeline = timeline;
			vk::set_name(
				backend.context, VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<u64>(timeline), "ember.frame_timeline");

			for (u32 i = 0; i < backend.frames_in_flight; ++i)
			{
				FrameSlot& slot = backend.slots[i];
				VkCommandPoolCreateInfo pool_info{
					.sType			  = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
					.queueFamilyIndex = backend.context.graphics.family,
				};

				VkCommandPool pool = VK_NULL_HANDLE;
				result			   = vkCreateCommandPool(backend.context.device, &pool_info, nullptr, &pool);

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
				result = vkAllocateCommandBuffers(backend.context.device, &allocation_info, &commands);
				if (result != VK_SUCCESS)
				{
					EMBER_ERROR("vulkan: frame command-buffer allocation failed: {}", vk::result_name(result));
					return false;
				}

				slot.commands = commands;
			}

			return true;
		}
	}

	Device::Device(const DeviceDef& def) noexcept
	{
		EMBER_PROFILE_FUNCTION_C(PROFILE_COLOR_RENDER);
		EMBER_ASSERT(::ember::gpu::is_valid(def));

		if (s_device_claimed.exchange(true, std::memory_order_acq_rel))
		{
			EMBER_ERROR("gpu: only one Device may exist at a time");
			return;
		}

		m_state					  = memory::new_object<Backend>(MemoryTag::Graphics);
		m_state->context.platform = def.platform;
		m_state->frames_in_flight = def.frames_in_flight;
		m_state->resources.reserve(def.limits);

		// Create Vulkan instance (loader, layers, WSI extensions, debug messenger)
		if (!create_instance(m_state->context, def))
		{
			shutdown();
			return;
		}

		// Adapter. Filter on required feature set, then score by preference.
		AdapterInfo adapter{};
		if (!select_adapter(*m_state, def, adapter))
		{
			shutdown();
			return;
		}

		m_state->context.adapter = adapter.handle;
		m_state->properties		 = adapter.properties;
		vkGetPhysicalDeviceMemoryProperties(m_state->context.adapter, &m_state->memory_properties);

		// Logical device, queues, allocator
		if (!create_device(m_state->context, adapter) || !create_allocator(m_state->context))
		{
			shutdown();
			return;
		}

		fill_caps(m_state->context.caps, adapter);

		if (!create_frame_resources(*m_state))
		{
			shutdown();
			return;
		}

		EMBER_INFO(
			"vulkan: {} ({}) | {} | Vulkan {}.{}.{} | {} MB local{}{} | validation {}",
			m_state->context.caps.adapter_name,
			enum_names<AdapterKind>()[static_cast<u32>(m_state->context.caps.adapter_kind)],
			adapter.driver,
			VK_API_VERSION_MAJOR(m_state->context.caps.api_version),
			VK_API_VERSION_MINOR(m_state->context.caps.api_version),
			VK_API_VERSION_PATCH(m_state->context.caps.api_version),
			m_state->context.caps.device_local_bytes / (1024 * 1024),
			m_state->context.caps.host_visible_device_local ? " (ReBAR)" : "",
			m_state->context.caps.mesh_shaders ? " | mesh shaders" : "",
			m_state->validation ? "on" : "off");
	}

	Device::~Device() noexcept { shutdown(); }

	void Device::shutdown() noexcept
	{
		EMBER_GPU_GUARD();

		// Partial initialization may not have created a device yet.
		if (m_state->context.device != VK_NULL_HANDLE)
			(void)vkDeviceWaitIdle(m_state->context.device);

		// Destroy surviving user resources while m_state is still published.
		if (m_state->context.device != VK_NULL_HANDLE)
		{
			destroy_resources();
			vk::drain_deferred_destroys(*m_state, UINT64_MAX);
		}

		// Destroy vulkan device & all resources that depend on it
		Backend* dead = std::exchange(m_state, nullptr);
		if (dead->context.device != VK_NULL_HANDLE)
		{
			for (FrameSlot& slot : dead->slots)
				if (slot.pool != VK_NULL_HANDLE)
					vkDestroyCommandPool(dead->context.device, slot.pool, nullptr);

			if (dead->timeline != VK_NULL_HANDLE)
				vkDestroySemaphore(dead->context.device, dead->timeline, nullptr);

			if (dead->context.allocator != VK_NULL_HANDLE)
				vmaDestroyAllocator(dead->context.allocator);

			vkDestroyDevice(dead->context.device, nullptr);
		}

		// Destroy debug messenger
		if (dead->context.messenger != VK_NULL_HANDLE)
			vkDestroyDebugUtilsMessengerEXT(dead->context.instance, dead->context.messenger, nullptr);

		// Destroy vulkan instance
		if (dead->context.instance != VK_NULL_HANDLE)
			vkDestroyInstance(dead->context.instance, nullptr);

		volkFinalize();

		if (dead->context.platform != nullptr)
			platform::vk::release_loader();

		memory::delete_object(MemoryTag::Graphics, dead);
		s_device_claimed.store(false, std::memory_order_release);
	}

	void Device::destroy_resources() noexcept
	{
		EMBER_GPU_GUARD();

		// Destroy swapchains
		auto& swapchains = m_state->resources.swapchains;
		for (auto it = swapchains.begin(); it != swapchains.end();)
		{
			const SwapchainHandle handle = it.handle();
			++it;
			EMBER_WARN("gpu: swapchain leaked at device destruction");
			destroy(handle);
		}

		// Destroy buffers
		auto& buffers = m_state->resources.buffers;
		for (auto it = buffers.begin(); it != buffers.end();)
		{
			const BufferHandle handle = it.handle();
			++it;
			EMBER_WARN("gpu: buffer leaked at device destruction");
			destroy(handle);
		}
	}

	void Device::wait_idle() noexcept
	{
		EMBER_GPU_GUARD();
		EMBER_PROFILE_SCOPE_C("gpu: wait_idle", PROFILE_COLOR_WAIT);

		vk::note_result(*m_state, vkDeviceWaitIdle(m_state->context.device));

		// Idle means everything signaled: even entries stamped for a submit that never
		// happened (an open frame at teardown) are safe now.
		vk::drain_deferred_destroys(*m_state, UINT64_MAX);
	}

	const DeviceCaps& Device::caps() const noexcept
	{
		// A falsy Device still answers caps(): all-zero caps read as "nothing supported", the
		// least surprising thing guard omitted user code can observe.
		static constinit DeviceCaps s_null_caps{};
		return m_state != nullptr ? m_state->context.caps : s_null_caps;
	}

	bool Device::device_lost() const noexcept
	{
		// acquire pairs with note_result's exchange: a true here happens-after the loss.
		return m_state != nullptr && m_state->lost.load(std::memory_order_acquire);
	}

	u32 Device::validation_error_count() noexcept
	{
		// Tests read these after teardown has joined.
		return debug_state().errors.load(std::memory_order_relaxed);
	}

	u32 Device::validation_warning_count() noexcept { return debug_state().warnings.load(std::memory_order_relaxed); }

	FrameInfo Device::begin_frame() noexcept
	{
		EMBER_GPU_GUARD({});
		EMBER_ASSERT(!m_state->frame_open && "begin_frame called twice without end_frame");

		u32 slot	   = static_cast<u32>(m_state->frame_index % m_state->frames_in_flight);
		u64 wait_value = m_state->slots[slot].submitted;

		/// The one wait that makes everything safe to reuse: this slot's previous submit has
		/// fully retired, so its command pools, deletion bucket and ring slice are free.
		/// Host-side wait: parks the thread, no queue round-trip.
		if (wait_value != 0)
		{
			EMBER_PROFILE_SCOPE_C("gpu: wait_frame", PROFILE_COLOR_WAIT);

			VkSemaphoreWaitInfo wait_info{
				.sType			= VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
				.semaphoreCount = 1,
				.pSemaphores	= &m_state->timeline,
				.pValues		= &wait_value,
			};

			vk::note_result(*m_state, vkWaitSemaphores(m_state->context.device, &wait_info, UINT64_MAX));
		}

		EMBER_VK_CHECK(vkResetCommandPool(m_state->context.device, m_state->slots[slot].pool, 0));

		// The wait above proved wait_value completed; the graveyard rides the frame pacing
		// and needs no extra queries.
		vk::drain_deferred_destroys(*m_state, wait_value);
		m_state->pending_present_count = 0;
		m_state->frame_open			   = true;

		return {.frame_index = static_cast<u32>(m_state->frame_index), .slot = slot};
	}

	void Device::end_frame() noexcept
	{
		EMBER_GPU_GUARD();
		EMBER_ASSERT(m_state->frame_open && "end_frame without begin_frame");

		const u32 slot	= static_cast<u32>(m_state->frame_index % m_state->frames_in_flight);
		const u64 value = ++m_state->timeline_value;

		VkCommandBuffer cmd = m_state->slots[slot].commands;

		const VkCommandBufferBeginInfo begin_info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		EMBER_VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

		// Placeholder visual until CommandList lands: clear every acquired backbuffer with a
		// slowly cycling hue. Animated on purpose — a static clear can't prove frame pacing.
		const f32 t = static_cast<f32>(m_state->frame_index) * 0.02f;
		const VkClearColorValue clear{
			.float32 = {
				0.5f + 0.5f * std::sin(t),
				0.5f + 0.5f * std::sin(t + 2.09f),
				0.5f + 0.5f * std::sin(t + 4.19f),
				1.0f,
			}};

		for (u32 i = 0; i < m_state->pending_present_count; ++i)
		{
			const vk::SwapchainData& data = m_state->resources.swapchains.get(m_state->pending_presents[i].swapchain);
			const vk::TextureHot& hot	  = m_state->resources.textures.get(data.images[data.acquired_image]);

			// Acquired contents are undefined; the acquire-semaphore wait below is scoped to
			// COLOR_ATTACHMENT_OUTPUT, which is why both barriers pivot on that stage.
			const VkImageMemoryBarrier2 to_color{
				.sType			  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask	  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				.srcAccessMask	  = VK_ACCESS_2_NONE,
				.dstStageMask	  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				.dstAccessMask	  = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
				.oldLayout		  = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout		  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.image			  = hot.image,
				.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};

			VkDependencyInfo dependency{
				.sType					 = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.imageMemoryBarrierCount = 1,
				.pImageMemoryBarriers	 = &to_color,
			};
			vkCmdPipelineBarrier2(cmd, &dependency);

			const VkRenderingAttachmentInfo color{
				.sType		 = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView	 = hot.sampled_view,
				.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.loadOp		 = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp	 = VK_ATTACHMENT_STORE_OP_STORE,
				.clearValue	 = {.color = clear},
			};

			const VkRenderingInfo rendering{
				.sType				  = VK_STRUCTURE_TYPE_RENDERING_INFO,
				.renderArea			  = {{0, 0}, data.extent},
				.layerCount			  = 1,
				.colorAttachmentCount = 1,
				.pColorAttachments	  = &color,
			};

			// A clear-only pass: dynamic rendering with zero draws is complete and valid.
			vkCmdBeginRendering(cmd, &rendering);
			vkCmdEndRendering(cmd);

			const VkImageMemoryBarrier2 to_present{
				.sType			  = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask	  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				.srcAccessMask	  = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
				.dstStageMask	  = VK_PIPELINE_STAGE_2_NONE, // the present semaphore takes over
				.dstAccessMask	  = VK_ACCESS_2_NONE,
				.oldLayout		  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.newLayout		  = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				.image			  = hot.image,
				.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};

			dependency.pImageMemoryBarriers = &to_present;
			vkCmdPipelineBarrier2(cmd, &dependency);
		}

		EMBER_VK_CHECK(vkEndCommandBuffer(cmd));

		// One submit: wait every acquire semaphore, run the frame's commands, signal the
		// timeline (CPU pacing) and every acquired image's present semaphore (WSI).
		VkSemaphoreSubmitInfo waits[MAX_SWAPCHAINS];
		VkSemaphoreSubmitInfo signals[MAX_SWAPCHAINS + 1];
		u32 wait_count	 = 0;
		u32 signal_count = 0;

		for (u32 i = 0; i < m_state->pending_present_count; ++i)
		{
			const vk::SwapchainData& data = m_state->resources.swapchains.get(m_state->pending_presents[i].swapchain);

			waits[wait_count++] = {
				.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = data.acquire_semaphores[slot],
				.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			};

			signals[signal_count++] = {
				.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				.semaphore = data.present_semaphores[data.acquired_image],
				.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			};
		}

		signals[signal_count++] = {
			.sType	   = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = m_state->timeline,
			.value	   = value,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};

		const VkCommandBufferSubmitInfo cmd_info{
			.sType		   = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			.commandBuffer = cmd,
		};

		const VkSubmitInfo2 submit_info{
			.sType					  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.waitSemaphoreInfoCount	  = wait_count,
			.pWaitSemaphoreInfos	  = waits,
			.commandBufferInfoCount	  = 1,
			.pCommandBufferInfos	  = &cmd_info,
			.signalSemaphoreInfoCount = signal_count,
			.pSignalSemaphoreInfos	  = signals,
		};

		vk::note_result(*m_state, vkQueueSubmit2(m_state->context.graphics.handle, 1, &submit_info, VK_NULL_HANDLE));

		// Batched present: every swapchain touched this frame in one call, results per entry.
		if (m_state->pending_present_count > 0)
		{
			VkSwapchainKHR swapchains[MAX_SWAPCHAINS];
			VkSemaphore present_waits[MAX_SWAPCHAINS];
			u32 image_indices[MAX_SWAPCHAINS];
			VkResult results[MAX_SWAPCHAINS];

			for (u32 i = 0; i < m_state->pending_present_count; ++i)
			{
				const vk::SwapchainData& data =
					m_state->resources.swapchains.get(m_state->pending_presents[i].swapchain);

				swapchains[i]	 = data.swapchain;
				present_waits[i] = data.present_semaphores[data.acquired_image];
				image_indices[i] = m_state->pending_presents[i].image_index;
			}

			const VkPresentInfoKHR present_info{
				.sType				= VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
				.waitSemaphoreCount = m_state->pending_present_count,
				.pWaitSemaphores	= present_waits,
				.swapchainCount		= m_state->pending_present_count,
				.pSwapchains		= swapchains,
				.pImageIndices		= image_indices,
				.pResults			= results,
			};

			vk::note_result(*m_state, vkQueuePresentKHR(m_state->context.graphics.handle, &present_info));

			// Per-swapchain outcome: OUT_OF_DATE/SUBOPTIMAL here means recreate at next acquire.
			for (u32 i = 0; i < m_state->pending_present_count; ++i)
				if (results[i] == VK_ERROR_OUT_OF_DATE_KHR || results[i] == VK_SUBOPTIMAL_KHR)
					m_state->resources.swapchains.get(m_state->pending_presents[i].swapchain).needs_recreate = true;
		}

		m_state->slots[slot].submitted = value;
		m_state->frame_open			   = false;
		++m_state->frame_index;
	}
}
