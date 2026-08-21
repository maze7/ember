#include <ember/containers/span.h>
#include <ember/core/profile.h>
#include <ember/memory/common.h>
#include <gpu/validation.h>
#include <gpu/vulkan/backend.h>
#include <gpu/vulkan/common.h>
#include <memory_resource>
#include <platform/vulkan/wsi.h>
#include <vulkan/vulkan_core.h>

namespace ember::gpu::vk
{
	namespace
	{
		constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

		[[nodiscard]] std::pmr::memory_resource* graphics_heap() noexcept { return &memory::heap(MemoryTag::Graphics); }

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

		/// Everything adapter selection learns and device creation consumes.
		struct AdapterInfo
		{
			VkPhysicalDevice handle = VK_NULL_HANDLE;
			VkPhysicalDeviceProperties properties{};

			u32 graphics_family = VK_QUEUE_FAMILY_IGNORED;
			u32 compute_family	= VK_QUEUE_FAMILY_IGNORED; // dedicated (no graphics); optional
			u32 transfer_family = VK_QUEUE_FAMILY_IGNORED; // dedicated (no graphics/compute); optional

			bool mesh_shader   = false;
			bool memory_budget = false;
			u32 subgroup_size  = 0;
			char driver[128]   = {};
		};

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
			u32 count = 0;

			if (vkEnumerateInstanceExtensionProperties(layer, &count, nullptr) != VK_SUCCESS || count == 0)
				return;

			size_t base = out.size();
			out.resize(base + count);

			if (vkEnumerateInstanceExtensionProperties(layer, &count, out.data() + base) != VK_SUCCESS)
				out.resize(base);
		}

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

			return VK_FALSE; // never abort the call: the layer's job is to report
		}

		/**
		 * Loader -> layers/extensions -> instance -> volk instance table -> debug messenger.
		 *
		 * Validation is optional-with-warning: ship machines don't have the layer installed,
		 * a missing layer must never stop the game from booting.
		 */
		[[nodiscard]] bool create_instance(DeviceBackend& backend, const DeviceDef& def) noexcept
		{
			/// Loader. With a Platform, SDL owns the Vulkan library so our calls and its surface
			/// creation share one loader instance; headless boots dlopen it via volk (tests, CI).
			if (backend.platform != nullptr)
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
				EMBER_ERROR("vulkan: no Vulkan loader on this system ({})", result_name(result));
				return false;
			}

			u32 loader_version = volkGetInstanceVersion();
			if (loader_version < API_VERSION)
			{
				EMBER_ERROR(
					"vulkan: loader supports {}.{}, Vulkan 1.3 is required",
					VK_API_VERSION_MAJOR(loader_version),
					VK_API_VERSION_MINOR(loader_version));
				return false;
			}

			Vector<VkLayerProperties> layers(graphics_heap());
			u32 layer_count = 0;

			if (vkEnumerateInstanceLayerProperties(&layer_count, nullptr) == VK_SUCCESS && layer_count != 0)
			{
				layers.resize(layer_count);

				if (vkEnumerateInstanceLayerProperties(&layer_count, layers.data()) != VK_SUCCESS)
					layers.clear();
			}

			Vector<VkExtensionProperties> extensions(graphics_heap());
			append_instance_extensions(nullptr, extensions);

			Vector<const char*> enabled_layers(graphics_heap());
			Vector<const char*> enabled_extensions(graphics_heap());

			if (def.enable_validation)
			{
				if (has_layer(layers, VALIDATION_LAYER))
				{
					enabled_layers.push_back(VALIDATION_LAYER);
					backend.validation = true;

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
				backend.debug_utils = true;
			}

			if (backend.platform != nullptr)
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

			if (backend.debug_utils)
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

			if (backend.validation && def.enable_sync_validation)
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
				.apiVersion			= API_VERSION,
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

			if (auto result = vkCreateInstance(&info, nullptr, &backend.instance); result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: vkCreateInstance failed: {}", result_name(result));
				backend.instance = VK_NULL_HANDLE;
				return false;
			}

			// instance-level entry points only; device ones load per device in create_device,
			// skipping the loader's runtime dispatch trampoline entirely.
			volkLoadInstanceOnly(backend.instance);

			if (backend.debug_utils)
			{
				messenger_info.pNext = nullptr;

				if (vkCreateDebugUtilsMessengerEXT(backend.instance, &messenger_info, nullptr, &backend.messenger) !=
					VK_SUCCESS)
				{
					EMBER_WARN("vulkan: debug messenger creation failed; continuing without it");
					backend.messenger = VK_NULL_HANDLE;
				}
			}

			return true;
		}

		/// Rejects adapters that cannot run ember's contract. Fills `out` for the ones that can.
		[[nodiscard]] bool
		query_adapter(const DeviceBackend& backend, VkPhysicalDevice handle, AdapterInfo& out) noexcept
		{
			out.handle = handle;
			vkGetPhysicalDeviceProperties(handle, &out.properties);

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

			Vector<VkExtensionProperties> extensions(graphics_heap());
			u32 count = 0;

			if (vkEnumerateDeviceExtensionProperties(handle, nullptr, &count, nullptr) == VK_SUCCESS && count != 0)
			{
				extensions.resize(count);

				if (vkEnumerateDeviceExtensionProperties(handle, nullptr, &count, extensions.data()) != VK_SUCCESS)
					extensions.clear();
			}

			if (backend.platform != nullptr && !has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
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
			Vector<VkQueueFamilyProperties> families(&memory::heap(MemoryTag::Graphics));
			vkGetPhysicalDeviceQueueFamilyProperties(handle, &count, nullptr);
			families.resize(count);
			vkGetPhysicalDeviceQueueFamilyProperties(handle, &count, families.data());

			for (u32 i = 0; i < count; ++i)
			{
				constexpr VkQueueFlags GRAPHICS_COMPUTE = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;

				if ((families[i].queueFlags & GRAPHICS_COMPUTE) != GRAPHICS_COMPUTE)
					continue;

				if (backend.platform != nullptr && !platform::vk::presentation_supported(backend.instance, handle, i))
					continue;

				out.graphics_family = i;
				break;
			}

			if (out.graphics_family == VK_QUEUE_FAMILY_IGNORED)
			{
				EMBER_INFO(
					"vulkan: skipping {}: no graphics+compute{} queue family",
					name,
					backend.platform != nullptr ? "+present" : "");
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
		[[nodiscard]] bool select_adapter(const DeviceBackend& backend, const DeviceDef& def, AdapterInfo& out) noexcept
		{
			u32 count = 0;

			if (vkEnumeratePhysicalDevices(backend.instance, &count, nullptr) != VK_SUCCESS || count == 0)
			{
				EMBER_ERROR("vulkan: no physical devices");
				return false;
			}

			Vector<VkPhysicalDevice> adapters(graphics_heap());
			adapters.resize(count);

			if (vkEnumeratePhysicalDevices(backend.instance, &count, adapters.data()) != VK_SUCCESS)
				return false;

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
		[[nodiscard]] bool create_device(vk::DeviceBackend& backend, const AdapterInfo& adapter) noexcept
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
				backend.caps.indirect_count		   = true;
			}

			if (available.vulkan12.samplerFilterMinmax == VK_TRUE)
			{
				enabled.vulkan12.samplerFilterMinmax = VK_TRUE;
				backend.caps.sampler_minmax			 = true;
			}

			if (available.vulkan12.bufferDeviceAddress == VK_TRUE)
			{
				enabled.vulkan12.bufferDeviceAddress = VK_TRUE;
				backend.buffer_device_address		 = true;
			}

			if (available.features2.features.fillModeNonSolid == VK_TRUE)
			{
				enabled.features2.features.fillModeNonSolid = VK_TRUE;
				backend.caps.wireframe						= true;
			}

			const char* extensions[3];
			u32 extension_count = 0;

			if (backend.platform != nullptr)
				extensions[extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

			// Extension-gated features: the extension string and its feature structs travel
			// together, which is why mesh shaders live here and not in the tables.
			if (adapter.mesh_shader)
			{
				extensions[extension_count++]  = VK_EXT_MESH_SHADER_EXTENSION_NAME;
				enabled.mesh_shader.taskShader = VK_TRUE;
				enabled.mesh_shader.meshShader = VK_TRUE;
				backend.caps.mesh_shaders	   = true;
			}

			if (adapter.memory_budget)
			{
				extensions[extension_count++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
				backend.memory_budget		  = true;
			}

			VkDeviceCreateInfo info{
				.sType					 = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
				.pNext					 = build_feature_chain(enabled, adapter.mesh_shader),
				.queueCreateInfoCount	 = queue_count,
				.pQueueCreateInfos		 = queue_infos,
				.enabledExtensionCount	 = extension_count,
				.ppEnabledExtensionNames = extensions,
			};

			if (auto result = vkCreateDevice(adapter.handle, &info, nullptr, &backend.device); result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: vkCreateDevice failed: {}", vk::result_name(result));
				backend.device = VK_NULL_HANDLE;
				return false;
			}

			// Direct device-level entry points: no per-call dispatch through the loader.
			volkLoadDevice(backend.device);

			const auto fetch_queue = [&](u32 family, vk::Queue& queue)
			{
				queue.family = family;
				vkGetDeviceQueue(backend.device, family, 0, &queue.handle);
			};

			fetch_queue(adapter.graphics_family, backend.graphics);

			if (adapter.compute_family != VK_QUEUE_FAMILY_IGNORED)
				fetch_queue(adapter.compute_family, backend.compute);
			else
				backend.compute = backend.graphics;

			if (adapter.transfer_family != VK_QUEUE_FAMILY_IGNORED)
				fetch_queue(adapter.transfer_family, backend.transfer);
			else
				backend.transfer = backend.graphics;

			vk::set_name(backend, VK_OBJECT_TYPE_DEVICE, (u64)backend.device, "ember.device");
			vk::set_name(backend, VK_OBJECT_TYPE_QUEUE, (u64)backend.graphics.handle, "ember.queue.graphics");

			return true;
		}

		/// VMA fetches every entry point through volk's two loaders; nothing links libvulkan.
		[[nodiscard]] bool create_allocator(DeviceBackend& backend) noexcept
		{
			VmaVulkanFunctions functions{};
			functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
			functions.vkGetDeviceProcAddr	= vkGetDeviceProcAddr;

			// Assignment, not designated init: VMA's member order is not stable across versions.
			VmaAllocatorCreateInfo info{};
			info.physicalDevice	  = backend.adapter;
			info.device			  = backend.device;
			info.instance		  = backend.instance;
			info.pVulkanFunctions = &functions;
			info.vulkanApiVersion = API_VERSION;

			if (backend.buffer_device_address)
				info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

			if (backend.memory_budget)
				info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;

			if (const VkResult result = vmaCreateAllocator(&info, &backend.allocator); result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: vmaCreateAllocator failed: {}", result_name(result));
				backend.allocator = VK_NULL_HANDLE;
				return false;
			}

			return true;
		}

		/**
		 * Everything user code and the backend consult at runtime, derived once. Optional-feature
		 * caps (wireframe, indirect_count, sampler_minmax, mesh_shaders) were already set while
		 * enabling them in create_device.
		 */
		void fill_caps(DeviceBackend& backend, const AdapterInfo& adapter) noexcept
		{
			DeviceCaps& caps					 = backend.caps;
			const VkPhysicalDeviceLimits& limits = backend.properties.limits;

			std::snprintf(caps.adapter_name, sizeof(caps.adapter_name), "%s", backend.properties.deviceName);
			caps.vendor_id	  = backend.properties.vendorID;
			caps.device_id	  = backend.properties.deviceID;
			caps.api_version  = backend.properties.apiVersion;
			caps.adapter_kind = to_adapter_kind(backend.properties.deviceType);

			// Memory: total device-local, and whether a large DEVICE_LOCAL|HOST_VISIBLE type
			// exists. A 256 MB heap is the pre-ReBAR BAR window; anything larger is ReBAR/SAM or
			// unified memory, which lets the transient ring live in VRAM.
			const VkPhysicalDeviceMemoryProperties& mem = backend.memory_properties;

			for (u32 i = 0; i < mem.memoryHeapCount; ++i)
				if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
					caps.device_local_bytes += mem.memoryHeaps[i].size;

			for (u32 i = 0; i < mem.memoryTypeCount; ++i)
			{
				constexpr VkMemoryPropertyFlags LOCAL_VISIBLE =
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

				const VkMemoryType& type = mem.memoryTypes[i];

				if ((type.propertyFlags & LOCAL_VISIBLE) == LOCAL_VISIBLE &&
					mem.memoryHeaps[type.heapIndex].size > 256_mb)
					caps.host_visible_device_local = true;
			}

			caps.constant_buffer_offset_alignment = static_cast<u32>(limits.minUniformBufferOffsetAlignment);
			caps.storage_buffer_offset_alignment  = static_cast<u32>(limits.minStorageBufferOffsetAlignment);
			caps.max_constant_block_bytes		  = std::min<u32>(limits.maxUniformBufferRange, 64u * 1024u);
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

			caps.ray_tracing = false; // reserved

			// Contract sanity: guaranteed by spec minimums, asserted so a broken driver is loud.
			EMBER_ASSERT(limits.maxPushConstantsSize >= PUSH_CONSTANT_BYTES);
			EMBER_ASSERT(limits.maxBoundDescriptorSets >= 2);
		}
	}

	bool boot(DeviceBackend& backend, const DeviceDef& raw_def) noexcept
	{
		EMBER_PROFILE_FUNCTION_C(PROFILE_COLOR_RENDER);

		// Cheap copy (PODs + pointers). From here on, every def field is in contract.
		const DeviceDef def = validated(raw_def);

		backend.platform		 = def.platform;
		backend.frames_in_flight = def.frames_in_flight;
		backend.resources.reserve(def.limits);

		// Vulkan instance (loader, layers, WSI extensions, debug messenger)
		if (!create_instance(backend, def))
		{
			shutdown(backend);
			return false;
		}

		// Adapter. filter on required feature set, then score by preference.
		AdapterInfo adapter{};
		if (!select_adapter(backend, def, adapter))
		{
			shutdown(backend);
			return false;
		}

		backend.adapter	   = adapter.handle;
		backend.properties = adapter.properties;
		vkGetPhysicalDeviceMemoryProperties(backend.adapter, &backend.memory_properties);

		// logical device, queues, allocator
		if (!create_device(backend, adapter) || !create_allocator(backend))
		{
			shutdown(backend);
			return false;
		}

		// PSO cache is an optimization, not a requirement. Failure degrades, not aborts.
		// create_pipeline_cache(backend);
		fill_caps(backend, adapter);

		/// Reserve resource pools at full capacity. A handle's index is its bindless
		/// slot, so pools never grow, hot pointers never move, and creation never allocates
		/// during a frame.
		backend.resources.reserve(def.limits);

		/// The frame timeline semaphore: a monotonically increasing u64. begin_frame()
		/// waits the value its slot signalled frames_in_flight frames ago.
		VkSemaphoreTypeCreateInfo semaphore_type_info{
			.sType		   = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
			.pNext		   = nullptr,
			.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
			.initialValue  = 0,
		};

		VkSemaphoreCreateInfo semaphore_info{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			.pNext = &semaphore_type_info,
		};

		if (auto result = vkCreateSemaphore(backend.device, &semaphore_info, nullptr, &backend.timeline);
			result != VK_SUCCESS)
		{
			EMBER_ERROR("vulkan: timeline semaphore creation failed: {}", vk::result_name(result));
			backend.timeline = VK_NULL_HANDLE;
			shutdown(backend);
			return false;
		}

		vk::set_name(
			backend, VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<u64>(backend.timeline), "ember.frame_timeline");

		for (u32 i = 0; i < backend.frames_in_flight; ++i)
		{
			VkCommandPoolCreateInfo pool_info{
				.sType			  = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.queueFamilyIndex = backend.graphics.family,
			};

			if (vkCreateCommandPool(backend.device, &pool_info, nullptr, &backend.slots[i].pool) != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: frame command pool creation failed");
				shutdown(backend);
				return false;
			}

			VkCommandBufferAllocateInfo alloc_info{
				.sType				= VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool		= backend.slots[i].pool,
				.level				= VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1,
			};
			EMBER_VK_CHECK(vkAllocateCommandBuffers(backend.device, &alloc_info, &backend.slots[i].commands));
		}

		EMBER_INFO(
			"vulkan: {} ({}) | {} | Vulkan {}.{}.{} | {} MB local{}{} | validation {}",
			backend.caps.adapter_name,
			enum_names<AdapterKind>()[(u32)backend.caps.adapter_kind],
			adapter.driver,
			VK_API_VERSION_MAJOR(backend.caps.api_version),
			VK_API_VERSION_MINOR(backend.caps.api_version),
			VK_API_VERSION_PATCH(backend.caps.api_version),
			backend.caps.device_local_bytes / (1024 * 1024),
			backend.caps.host_visible_device_local ? " (ReBAR)" : "",
			backend.caps.mesh_shaders ? " | mesh shaders" : "",
			backend.validation ? "on" : "off");

		return true;
	}

	void shutdown(DeviceBackend& backend) noexcept
	{
		EMBER_PROFILE_FUNCTION_C(PROFILE_COLOR_RENDER);

		if (backend.device != VK_NULL_HANDLE)
		{
			(void)vkDeviceWaitIdle(backend.device);

			for (FrameSlot& slot : backend.slots)
			{
				if (slot.pool != VK_NULL_HANDLE)
					vkDestroyCommandPool(backend.device, slot.pool, nullptr); // frees its buffers
				slot = {};
			}

			if (backend.timeline != VK_NULL_HANDLE)
				vkDestroySemaphore(backend.device, backend.timeline, nullptr);

			if (backend.pipeline_cache != VK_NULL_HANDLE)
				vkDestroyPipelineCache(backend.device, backend.pipeline_cache, nullptr);

			// After pooled resources and the allocator, VMA should hold zero allocations; it reports
			// leaks through the validation messenger if we missed any.
			if (backend.allocator != VK_NULL_HANDLE)
				vmaDestroyAllocator(backend.allocator);

			vkDestroyDevice(backend.device, nullptr);
		}

		if (backend.messenger != VK_NULL_HANDLE)
			vkDestroyDebugUtilsMessengerEXT(backend.instance, backend.messenger, nullptr);

		if (backend.instance != VK_NULL_HANDLE)
			vkDestroyInstance(backend.instance, nullptr);

		// Null the handles rather than trusting immediate deletion: shutdown() must be re-entrant
		// for the boot-failure path, and a nulled backend makes any use-after-shutdown fail loudly
		// at the first guarded call instead of in the driver.
		backend.timeline	   = VK_NULL_HANDLE;
		backend.pipeline_cache = VK_NULL_HANDLE;
		backend.allocator	   = VK_NULL_HANDLE;
		backend.device		   = VK_NULL_HANDLE;
		backend.adapter		   = VK_NULL_HANDLE;
		backend.messenger	   = VK_NULL_HANDLE;
		backend.instance	   = VK_NULL_HANDLE;
		backend.graphics = backend.compute = backend.transfer = {};

		// Clears volk's function table; unloads the library only when volk dlopen'd it.
		// Safe when boot never got that far.
		volkFinalize();

		// Balance the SDL loader reference from create_instance. SDL refcounts internally,
		// so this is a guarded no-op when the reference was never taken.
		if (backend.platform)
			platform::vk::release_loader();
	}
}
