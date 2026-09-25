#pragma once

#include <ember/assets/asset_id.h>
#include <ember/containers/mpmc_queue.h>
#include <ember/containers/pool.h>
#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/core/handle.h>
#include <ember/io/file.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>
#include <ember/memory/pmr/heap.h>
#include <ember/sync/spin_mutex.h>

#include <atomic>
#include <type_traits>
#include <utility>

namespace ember::gpu
{
	class Device;
}

namespace ember
{
	class AssetManager;

	enum class AssetState : u8
	{
		Loading, // the first load has not finished
		Loaded,	 // a payload exists; a reload may be in flight behind it
		Failed,	 // the first load failed; get() is null until a reload succeeds
	};

	namespace detail
	{
		/**
		 * What an AssetRef points at: the part of an asset's slot that references read. The manager
		 * writes every field at a defined moment (the first load publishes the payload once, pumps
		 * edit it in place between frames) and any thread reads them.
		 */
		struct AssetSlot
		{
			std::atomic<void*> payload{nullptr}; // the live T; null until the first load succeeds
			std::atomic<AssetState> state{AssetState::Loading};
			std::atomic<u32> refs{0};			   // AssetRefs alive; the last one to go hands the slot to the pump
			std::atomic<bool> unreferenced{false}; // queued for the pump's zero refs check

			// Signalled by the first load's outcome, loaded or failed, exactly once.
			jobs::Counter first_load{1};

			AssetManager* manager = nullptr;
			u32 handle			  = 0; // the registry handle's bits, for the unreferenced queue
		};

		/** The last reference let go: any thread, into the manager's queue. */
		void unreferenced(AssetSlot& slot) noexcept;

		class FileWatcher;
	}

	/**
	 * A counted reference to an asset.
	 *
	 * The asset stays loaded while any reference to it exists, and is unloaded at a pump, once the last
	 * reference dies. It is a live view; a reload edits the payload in place, between frames, and whatever
	 * the payload names on the GPU keeps its handle, so code that reads through the reference or stored a
	 * handle it gave out is never stale and never asked to check. Copying a reference is cheap and any
	 * thread may copy or drop one. The pointer it hands out is stable for the asset's life; the  bytes
	 * behind a payload's pointers can be replaced at a pump and are freed a couple of frames later, so
	 * read them through the reference, not from a copy kept across frames.
	 */
	template <class T> class AssetRef
	{
	public:
		AssetRef() noexcept = default;
		AssetRef(const AssetRef& other) noexcept : m_slot(other.m_slot) { acquire(); }
		AssetRef(AssetRef&& other) noexcept : m_slot(std::exchange(other.m_slot, nullptr)) {}
		~AssetRef() noexcept { release(); }

		AssetRef& operator=(AssetRef other) noexcept
		{
			std::swap(m_slot, other.m_slot);
			return *this;
		}

		/**
		 * True once the asset has a payload. A null reference, a failed asset and one still loading
		 * all read false; a failed one turns true if a later reload succeeds.
		 */
		[[nodiscard]] explicit operator bool() const noexcept { return get() != nullptr; }

		[[nodiscard]] const T* get() const noexcept
		{
			return m_slot != nullptr ? static_cast<const T*>(m_slot->payload.load(std::memory_order_acquire)) : nullptr;
		}

		[[nodiscard]] const T* operator->() const noexcept { return get(); }
		[[nodiscard]] const T& operator*() const noexcept { return *get(); }

		/** Failed for a null reference. */
		[[nodiscard]] AssetState state() const noexcept
		{
			return m_slot != nullptr ? m_slot->state.load(std::memory_order_acquire) : AssetState::Failed;
		}

		[[nodiscard]] bool is_null() const noexcept { return m_slot == nullptr; }

		/** Parks until the first load has finished, loaded or failed. Reloads never block anyone. */
		void wait() const noexcept
		{
			if (m_slot != nullptr)
				jobs::wait(m_slot->first_load);
		}

		void reset() noexcept { release(); }

	private:
		friend class AssetManager;

		struct Adopt
		{
		};

		/** The manager counted the reference already, under its lock. */
		AssetRef(detail::AssetSlot* slot, Adopt) noexcept : m_slot(slot) {}

		/** Relaxed on the way in: a copy is made from a reference that already holds the slot. */
		void acquire() noexcept
		{
			if (m_slot != nullptr)
				m_slot->refs.fetch_add(1, std::memory_order_relaxed);
		}

		/** The last release hands the slot to the pump, which must see every use of the payload done. */
		void release() noexcept
		{
			if (detail::AssetSlot* slot = std::exchange(m_slot, nullptr))
				if (slot->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
					detail::unreferenced(*slot);
		}

		detail::AssetSlot* m_slot = nullptr;
	};

	/**
	 * What a loader gets: the file's bytes and the services it may use. Runs on a worker, so
	 * everything here is any-thread.
	 */
	class AssetLoad final
	{
	public:
		AssetLoad(StringView path, Span<u8> bytes, const void* live, gpu::Device& gpu, Heap& heap) noexcept
			: m_path(path), m_bytes(bytes), m_live(live), m_gpu(&gpu), m_heap(&heap)
		{
		}

		/**
		 * The payload this load replaces; null on a first load. Read it, never write it, and read
		 * only what reload() leaves alone: the pump may be folding an earlier reload into it while
		 * this one runs. A type with GPU handles reads the handle here and updates the object
		 * behind it, so the reload lands where everyone is already looking.
		 */
		template <class T> [[nodiscard]] const T* live() const noexcept { return static_cast<const T*>(m_live); }

		/** Relative to the asset root, null terminated: fine as a debug name. */
		[[nodiscard]] StringView path() const noexcept { return m_path; }
		[[nodiscard]] Span<const u8> bytes() const noexcept { return {m_bytes.data(), m_bytes.size()}; }
		[[nodiscard]] gpu::Device& gpu() const noexcept { return *m_gpu; }
		[[nodiscard]] Heap& heap() const noexcept { return *m_heap; }

		/**
		 * Hands the file's bytes to the payload instead of copying them: a cooked asset is its
		 * file. They are io::FILE_ALIGNMENT aligned; unload returns them with
		 * heap.deallocate(data, size, io::FILE_ALIGNMENT).
		 */
		[[nodiscard]] Span<u8> take_bytes() noexcept
		{
			m_taken = true;
			return std::exchange(m_bytes, {});
		}

		[[nodiscard]] bool taken() const noexcept { return m_taken; }

	private:
		StringView m_path;
		Span<u8> m_bytes;
		const void* m_live = nullptr;
		gpu::Device* m_gpu;
		Heap* m_heap;
		bool m_taken = false;
	};

	/** What unload and reload may use. Both run on the owner thread between frames. */
	struct AssetServices
	{
		gpu::Device& gpu;
		Heap& heap;
	};

	/** A type that says how a reload folds into the live payload. */
	template <class T>
	concept HasAssetReload = requires(AssetServices& services, T& live, T& fresh) {
		{ T::reload(services, live, fresh) } noexcept -> std::same_as<void>;
	};

	/**
	 * A payload type: any nothrow default constructible struct with two static functions,
	 *
	 *   static bool load(AssetLoad&, T& out) noexcept;
	 *   static void unload(AssetServices&, T& asset) noexcept;
	 *
	 * and optionally a third, for how a reload reaches the live payload while frames read it:
	 *
	 *   static void reload(AssetServices&, T& live, T& fresh) noexcept;
	 *
	 * load() fills a default constructed T, on the first load and on every reload alike, so it
	 * never knows which it is. reload() runs between frames with both in hand and makes `live`
	 * become `fresh`, leaving in `fresh` whatever is now spent; the manager unloads `fresh` a few
	 * frames later. A type without reload() is swapped, which is right whenever the payload is
	 * plain data. A type that hands out GPU handles keeps them instead: it moves the new object
	 * behind the old handle (Device::replace_texture) so nothing that stored the handle changes.
	 */
	template <class T>
	concept AssetType = std::is_nothrow_default_constructible_v<T> && std::is_nothrow_destructible_v<T> &&
						(HasAssetReload<T> || std::is_nothrow_swappable_v<T>) &&
						requires(AssetLoad& load, AssetServices& services, T& asset) {
							{ T::load(load, asset) } noexcept -> std::same_as<bool>;
							{ T::unload(services, asset) } noexcept -> std::same_as<void>;
						};

	/** Hot reload is only enabled by default in dev builds. */
#if defined(NDEBUG)
	inline constexpr bool ASSET_HOT_RELOAD_DEFAULT = false;
#else
	inline constexpr bool ASSET_HOT_RELOAD_DEFAULT = true;
#endif

	struct AssetManagerDef
	{
		const char* root = "assets"; // joined in front of every path
		u32 max_assets	 = 4096;	 // slots; a handle's index is 16 bit
		u32 max_changes	 = 1024;	 // file changes waiting for a pump; more than that in one frame drop

		/**
		 * Loader jobs that run at once. Each parks during its read and decodes on a worker, so this
		 * bounds both the fibers loads hold and the workers decoding takes from the frame. Zero derives
		 * it from the worker count.
		 */
		u32 loaders = 0;

		/** Watch the root and reload assets whose files change. Costs a thread and an OS watch. */
		bool hot_reload = ASSET_HOT_RELOAD_DEFAULT;

		/**
		 * Time a changed file must be quiet before it reloads: editors write in several steps
		 * and the first event sees a half written file.
		 */
		u32 reload_delay_ms = 150;
	};

	/**
	 * The asset registry and its loaders, following the job system's one rule: everything is a job
	 * except the read.
	 *
	 * A load is one straight line of code on a loader job: submit the read and park, decode and
	 * create the GPU objects on whichever worker resumes it, publish. The first load publishes
	 * with one release store of the payload pointer, so a reference sees a whole payload or none.
	 * A reload builds a fresh payload the same way and hands it to pump(), which folds it into the
	 * live one between frames, when no stage is running, through the type's reload(); what that
	 * leaves behind is unloaded a couple of pumps later, once any pointer a frame copied out of
	 * the old contents has gone by. GPU objects keep their handles across a reload: the device
	 * moves the new object behind the old handle. Nothing that consumed the asset takes part.
	 *
	 * Lifetime is the references': the last AssetRef to go queues the slot, and the next pump
	 * unloads it. Hot reload is the watcher naming a changed file by AssetId, pump() waiting for
	 * it to go quiet, and the same load again. A reload that fails keeps the old payload, so a
	 * broken save never takes the game down.
	 *
	 * THREADING
	 *   load(): any thread. References: copy, drop, read, wait from any thread.
	 *   notify_changed(): any thread, the watcher's included.
	 *   pump(), wait_idle(), shutdown(): the owner thread, between frames.
	 */
	class AssetManager final
	{
	public:
		AssetManager() noexcept;
		~AssetManager() noexcept;

		AssetManager(const AssetManager&)			 = delete;
		AssetManager& operator=(const AssetManager&) = delete;

		/**
		 * Once per object, after the file thread and the device are up. Registers the engine's
		 * own types. The game registers its own before its first load.
		 */
		void init(io::FileIo& io, gpu::Device& gpu, const AssetManagerDef& def = {}) noexcept;

		/**
		 * Finishes every load in flight and unloads every asset. Every AssetRef must be gone by
		 * now; one that is not asserts.
		 */
		void shutdown() noexcept;

		/** Once per type, before its first load(). Types are process wide: one manager at a time. */
		template <AssetType T> void register_type(const char* name) noexcept
		{
			s_type_index<T> = add_type({
				.name  = name,
				.size  = sizeof(T),
				.align = alignof(T),
				.load =
					[](AssetLoad& load, void* payload) noexcept
				{
					T& asset = *new (payload) T{};

					if (T::load(load, asset))
						return true;

					asset.~T();
					return false;
				},
				.unload =
					[](AssetServices& services, void* payload) noexcept
				{
					T& asset = *static_cast<T*>(payload);
					T::unload(services, asset);
					asset.~T();
				},
				.reload =
					[](AssetServices& services, void* live, void* fresh) noexcept
				{
					if constexpr (HasAssetReload<T>)
						T::reload(services, *static_cast<T*>(live), *static_cast<T*>(fresh));
					else
						std::swap(*static_cast<T*>(live), *static_cast<T*>(fresh));
				},
			});
		}

		/**
		 * The asset at path, loading it if nothing holds it yet. Returns at  once; wait() on the
		 * reference for the first load, or read it when it turns true. The same path is the same
		 * asset while a reference to it lives; asking for it as another type is a bug and asserts.
		 *
		 * A null reference when the registry is full.
		 */
		template <AssetType T> [[nodiscard]] AssetRef<T> load(StringView path) noexcept
		{
			return AssetRef<T>(request(s_type_index<T>, path), typename AssetRef<T>::Adopt{});
		}

		/** A file changed on disk. Any thread; the watcher calls this. Unknown ids are ignored. */
		void notify_changed(AssetId id) noexcept;

		/**
		 * Once per frame on the owner thread, before the frame's update is kicked: unloads assets
		 * nothing references, folds finished reloads into their payloads, frees what earlier
		 * reloads left behind, and turns quiet file changes into reloads.
		 */
		void pump(u64 frame_index) noexcept;

		/** Parks until no loader is running. For boot and level loads; new requests restart loaders. */
		void wait_idle() noexcept;

		struct Stats
		{
			u32 assets	= 0;
			u32 loading = 0;
			u32 failed	= 0;
			u32 loaders = 0; // running now
		};

		/** Walks the registry under the lock: for debug UI, not per asset. */
		[[nodiscard]] Stats stats() const noexcept;

	private:
		template <class T> friend class AssetRef;
		friend void detail::unreferenced(detail::AssetSlot& slot) noexcept;

		static constexpr u16 NO_TYPE = 0xFFFF;

		/**
		 * Pumps a retired payload waits before it is unloaded. Two covers a bare pointer that
		 * travelled through frame memory, where no AssetRef can live: the update that took it
		 * and the render that update published it to, which runs one frame later.
		 */
		static constexpr u64 RETIRE_GRACE = 2;

		struct Type
		{
			const char* name									  = nullptr;
			u32 size											  = 0;
			u32 align											  = 0;
			bool (*load)(AssetLoad&, void*) noexcept			  = nullptr; // constructs, then T::load
			void (*unload)(AssetServices&, void*) noexcept		  = nullptr; // T::unload, then destroys
			void (*reload)(AssetServices&, void*, void*) noexcept = nullptr; // T::reload, or a swap
		};

		/**
		 * One asset: what references see, plus what only the manager and its loaders touch.
		 * Constructed by request() and destroyed by free_slot(), both under the lock.
		 */
		struct Slot : detail::AssetSlot
		{
			Slot(u16 type, AssetId id, String&& path) noexcept : type(type), id(id), path(std::move(path)) {}

			u16 type;
			AssetId id;
			String path;		   // root joined; the in flight read points at it
			void* fresh = nullptr; // a finished reload waiting for the pump; under the lock

			std::atomic<bool> queued{false}; // a load for this slot is queued or running
			std::atomic<bool> dirty{false};	 // changed again while queued: load once more
			std::atomic<bool> dead{false};	 // unreferenced while queued: the loader frees the slot
		};

		struct SlotTag;
		using SlotPool	 = Pool<SlotTag, Slot>;
		using SlotHandle = SlotPool::HandleType;

		/**
		 * Queue cells are value initialized; nested structs carry no default member initializers
		 * because those are not visible until the enclosing class is complete.
		 */
		struct Request
		{
			u32 slot; // SlotHandle bits
		};

		struct Change
		{
			AssetId id;
			u64 time_ns;
		};

		struct Retired
		{
			void* payload = nullptr;
			u16 type	  = NO_TYPE;
			u64 frame	  = 0; // pump index at retirement; unloaded RETIRE_GRACE pumps on
		};

		template <class T> inline static u16 s_type_index = NO_TYPE;

		[[nodiscard]] u16 add_type(const Type& type) noexcept;
		[[nodiscard]] Slot* request(u16 type, StringView path) noexcept;
		void unreferenced(detail::AssetSlot& slot) noexcept;
		void free_slot(Slot& slot, SlotHandle handle) noexcept; // under the lock

		void reload(AssetId id) noexcept;
		void queue_load(Slot& slot, SlotHandle handle) noexcept;
		void wake_loader() noexcept;
		void run_loader() noexcept;
		void load_one(Slot& slot, SlotHandle handle) noexcept;
		void publish(Slot& slot, SlotHandle handle, void* payload) noexcept;
		void retire(void* payload, u16 type) noexcept; // under the lock
		void release(const Retired& retired) noexcept;

		io::FileIo* m_io   = nullptr;
		gpu::Device* m_gpu = nullptr;
		Heap* m_heap	   = nullptr;
		String m_root;
		u32 m_relative		  = 0; // where a slot's path stops being the root
		u64 m_reload_delay_ns = 0;

		Vector<Type> m_types; // fixed once loads begin, so slots address it by index without the lock

		/// Slots, the path index, finished reloads and the retire list move under this lock; a
		/// request is rare next to a read through a reference, which never takes it.
		mutable SpinMutex m_lock;
		SlotPool m_slots;
		HashMap<AssetId, u32> m_by_id; // SlotHandle bits
		Vector<u32> m_fresh;		   // slots with a reload waiting for the pump
		Vector<Retired> m_retired;
		Vector<Retired> m_due; // pump only: taken out from under the lock, unloaded outside it

		MpmcQueue<Request> m_requests; // one cell per slot, so it can never be full
		MpmcQueue<u32> m_unreferenced; // slots whose last reference went; one entry per slot at most
		MpmcQueue<Change> m_changes;
		Vector<Change> m_pending; // pump only: changes waiting to go quiet

		std::atomic<u64> m_frame{0}; // the last pump's index, for retire stamps
		std::atomic<u32> m_running{0};
		u32 m_loader_count = 0;
		jobs::Counter m_loaders; // every loader job kicked, for wait_idle

		/// The OS watch on the root, when hot reload is on. Its thread only ever calls notify_changed().
		detail::FileWatcher* m_watcher = nullptr;
	};
}
