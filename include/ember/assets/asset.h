#pragma once

#include <ember/assets/asset_source.h>
#include <ember/containers/mpmc_queue.h>
#include <ember/containers/pool.h>
#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/core/filesystem.h>
#include <ember/core/handle.h>
#include <ember/core/hash.h>
#include <ember/jobs/job_system.h>
#include <ember/memory/memory.h>
#include <ember/memory/pmr/heap.h>
#include <ember/sync/spin_mutex.h>

#include <array>
#include <atomic>
#include <optional>
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

	/** A file below the root or a mount that changed and went quiet: what pump() reloaded for. */
	struct AssetChange
	{
		String name; // as assets name it: "textures/tiles.png", "ember/materials/sprite.slang"
		AssetId id	 = 0;
		bool mounted = false; // under a mount rather than the root: a game decides whether those travel
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

			// Signalled once, by the first load's outcome: its payload published, or its failure.
			jobs::Counter first_load{1};

			AssetManager* manager = nullptr;
			u32 handle			  = 0; // the registry handle's bits, for the unreferenced queue
		};

		/** The last reference let go: any thread, into the manager's queue. */
		void unreferenced(AssetSlot& slot) noexcept;

		/** AssetRef::wait(), which the manager answers: some types publish on the owner thread. */
		void wait(AssetSlot& slot) noexcept;

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
	 * behind a payload's pointers can be replaced at a pump and are freed at the pump after, so
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

		/**
		 * Parks until the first load has finished, loaded or failed. Reloads never block anyone. A
		 * type that publishes on the owner thread (see AssetType) is waited for on that thread only,
		 * which publishes what is due rather than wait for a pump it is holding up.
		 */
		void wait() const noexcept
		{
			if (m_slot != nullptr)
				detail::wait(*m_slot);
		}

		void reset() noexcept { release(); }

	private:
		friend class AssetManager;
		friend class AssetLoad;

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
		AssetLoad(AssetManager& manager, u32 slot, StringView path, StringView file, fs::FileData&& data,
				  const void* live, void* context, gpu::Device& gpu, Heap& heap) noexcept
			: m_manager(&manager), m_slot(slot), m_path(path), m_file(file), m_data(std::move(data)), m_live(live),
			  m_context(context), m_gpu(&gpu), m_heap(&heap), m_dependencies(&heap)
		{
		}

		/**
		 * The payload this load replaces; null on a first load. Read it, never write it, and read
		 * only what reload() leaves alone: the pump may be folding an earlier reload into it while
		 * this one runs. A type with GPU handles reads the handle here and updates the object
		 * behind it, so the reload lands where everyone is already looking.
		 */
		template <class T> [[nodiscard]] const T* live() const noexcept { return static_cast<const T*>(m_live); }

		/** The asset's name: below the root, or under a mount's prefix. Null terminated: fine as a debug name. */
		[[nodiscard]] StringView path() const noexcept { return m_path; }

		/** The file behind the name, absolute: where a type that reads its own files starts from. */
		[[nodiscard]] StringView file() const noexcept { return m_file; }

		/** Empty for a type that reads its own files. */
		[[nodiscard]] Span<const u8> bytes() const noexcept { return m_data.bytes(); }
		[[nodiscard]] gpu::Device& gpu() const noexcept { return *m_gpu; }
		[[nodiscard]] Heap& heap() const noexcept { return *m_heap; }

		/** What the type was registered with: the system its payloads belong to. */
		template <class C> [[nodiscard]] C& context() const noexcept
		{
			EMBER_ASSERT(m_context != nullptr && "register the type with its context");
			return *static_cast<C*>(m_context);
		}

		/**
		 * Hands the file's bytes to the payload instead of copying them: a cooked asset is its
		 * file. They free themselves with the FileData, so a payload that keeps one has nothing
		 * to do at unload. Bytes left here die with the load.
		 */
		[[nodiscard]] fs::FileData take_bytes() noexcept { return std::move(m_data); }

		/**
		 * Another asset this one is made of, requested and never waited for: a loader that parked
		 * on another asset would hold a loader slot that asset may need, and enough of them would
		 * hold every slot. Keep the reference in the payload. When the asset loads or reloads, this
		 * one hears it on the owner thread: its publish() is asked again, then its refresh() runs.
		 */
		template <class T> [[nodiscard]] AssetRef<T> load(StringView path) noexcept;

		/**
		 * A file this asset was made from that is no asset itself, such as a shader it imported: a
		 * change to it reloads this asset. An absolute path under the root or a mount, or a name
		 * below them; any other file is outside every watch, and is ignored.
		 */
		void depends_on(StringView path) noexcept;

		/**
		 * Another file by name, through whichever source serves it, read on the IO thread while this
		 * loader parks: the image a sheet measures, the pair a cooked type is read from. A change to
		 * it reloads this asset, as depends_on() would arrange. Names are as assets are named, below
		 * the root or a mount; one no source holds reads as NotFound.
		 */
		[[nodiscard]] Result<fs::FileData, fs::FileError> read(StringView name) noexcept;

		/**
		 * A file this load wrote whole, by name, as a cook writes its pair: it reloads, and is recorded
		 * for take_changed(), at the next pump rather than after the quiet time a save gets, and the
		 * watcher's own report of the write is not counted again.
		 */
		void notify_written(StringView name) noexcept;

	private:
		friend class AssetManager;

		AssetManager* m_manager;
		u32 m_slot; // the loading slot's handle bits: the parent of whatever it loads
		StringView m_path;
		StringView m_file;
		fs::FileData m_data;
		const void* m_live = nullptr;
		void* m_context	   = nullptr;
		gpu::Device* m_gpu;
		Heap* m_heap;
		Vector<AssetId> m_dependencies;
	};

	/** What unload, reload, publish and refresh may use. All four run on the owner thread. */
	class AssetServices final
	{
	public:
		AssetServices(gpu::Device& gpu, Heap& heap, void* context) noexcept : gpu(gpu), heap(heap), m_context(context)
		{
		}

		gpu::Device& gpu;
		Heap& heap;

		/** What the type was registered with. */
		template <class C> [[nodiscard]] C& context() const noexcept
		{
			EMBER_ASSERT(m_context != nullptr && "register the type with its context");
			return *static_cast<C*>(m_context);
		}

	private:
		void* m_context;
	};

	/** A type that says how a reload folds into the live payload. */
	template <class T>
	concept HasAssetReload = requires(AssetServices& services, T& live, T& fresh) {
		{ T::reload(services, live, fresh) } noexcept -> std::same_as<void>;
	};

	/** A type whose payloads are finished on the owner thread before anyone sees them. */
	template <class T>
	concept HasAssetPublish = requires(AssetServices& services, T& asset) {
		{ T::publish(services, asset) } noexcept -> std::same_as<bool>;
	};

	/** A type that hears when an asset it loaded has loaded or reloaded. */
	template <class T>
	concept HasAssetRefresh = requires(AssetServices& services, T& asset) {
		{ T::refresh(services, asset) } noexcept -> std::same_as<void>;
	};

	/** A type whose loader reads its own files. */
	template <class T>
	concept ReadsOwnFiles = requires { requires T::READS_OWN_FILES; };

	/**
	 * A payload type: any nothrow default constructible struct with two static functions,
	 *
	 *   static bool load(AssetLoad&, T& out) noexcept;
	 *   static void unload(AssetServices&, T& asset) noexcept;
	 *
	 * and optionally, one for each need a type may have beyond them:
	 *
	 *   static void reload(AssetServices&, T& live, T& fresh) noexcept;
	 *   static bool publish(AssetServices&, T& asset) noexcept;
	 *   static void refresh(AssetServices&, T& asset) noexcept;
	 *   static constexpr bool READS_OWN_FILES = true;
	 *
	 * load() fills a default constructed T, on the first load and on every reload alike, so it
	 * never knows which it is. reload() runs between frames with both in hand and makes `live`
	 * become `fresh`, leaving in `fresh` whatever is now spent; the manager unloads `fresh` a frame
	 * later. A type without reload() is swapped, which is right whenever the payload is
	 * plain data. A type that hands out GPU handles keeps them instead: it moves the new object
	 * behind the old handle (Device::replace_texture) so nothing that stored the handle changes.
	 *
	 * publish() is for a payload finished by a system only the owner thread may change, such as a
	 * registry of GPU tables: the first payload waits at the pump, which calls publish() and lets
	 * the payload be seen once it returns true. False means not yet (an asset it loaded has still
	 * to arrive), and the pump asks again after that asset has. Only the owner thread can wait for
	 * such a type: the pump it would wait for is its own.
	 *
	 * refresh() runs between frames once an asset this one loaded through AssetLoad::load() has
	 * loaded or reloaded: how a payload binds what arrives after it, without a loader ever waiting.
	 *
	 * READS_OWN_FILES leaves the reading to load(), for an asset whose name is not its file: a
	 * shader the compiler reads with its imports, or a cooked pair standing in for a source.
	 *
	 * unload() gets every payload a load made, including one dropped before publish() let it out.
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
		const char* root = "assets"; // resolved against the working directory once, at init
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

		/** Keep the quiet changes pump() reloads for, by name, for take_changed(): a host that syncs clients. */
		bool record_changes = false;

		/** deliver() may write under a mount, not only the root: a mount is usually someone else's checkout. */
		bool deliver_to_mounts = false;
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
	 * leaves behind is unloaded a pump later, once any pointer a frame copied out of
	 * the old contents has gone by. GPU objects keep their handles across a reload: the device
	 * moves the new object behind the old handle. Nothing that consumed the asset takes part.
	 *
	 * Assets are made of assets: a loader requests the ones it needs and never waits for them. A
	 * payload that must be finished on the owner thread is published by the pump instead of the
	 * loader, once what it needs has arrived, and a published one is refreshed when something it
	 * loaded arrives or reloads. So a material that names a texture and a shader type loads in any
	 * order with them, and no loader holds a slot another load needs.
	 *
	 * Lifetime is the references': the last AssetRef to go queues the slot, and the next pump
	 * unloads it; a payload's own references go with it, a pump or two later. Hot reload is the
	 * watcher naming a changed file by AssetId, pump() waiting for it to go quiet, and the same
	 * load again, for the asset the file is and for every asset that said it depends on the file.
	 * A reload that fails keeps the old payload, so a broken save never takes the game down.
	 *
	 * Names are paths below the root, or below a mount: another source served under a prefix, such
	 * as the engine's shaders in a dev build, or a pack. Every source is an AssetSource; the root is a
	 * DirectorySource of its own. Loaders read by name through the manager and never learn which
	 * source answered, so a game's files may live in a directory while it is made and in a pack when
	 * it ships, with no loader the wiser. A directory is watched along with the root.
	 *
	 * Files reach a source from another machine too: deliver() hands bytes to the source a name
	 * belongs to and then reloads as a save would, and take_changed() names what the watcher saw go
	 * quiet, so a host can send its saves on. Together they are hot reload across machines.
	 *
	 * THREADING
	 *   load(): any thread. References: copy, drop, read from any thread; wait as the type allows.
	 *   notify_changed(), deliver(): any thread, the watcher's included.
	 *   register_type(): the owner thread, before anything loads the type.
	 *   mount(): the owner thread, before the first load.
	 *   pump(), wait_idle(), take_changed(), file_hash(), shutdown(): the owner thread, between frames.
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
		 * own types. A game registers its own any time before it loads one: in its init(), say.
		 */
		void init(gpu::Device& gpu, const AssetManagerDef& def = {}) noexcept;

		/**
		 * Finishes every load in flight and unloads every asset, each after the assets holding it.
		 * Every AssetRef outside a payload must be gone by now; one that is not asserts.
		 */
		void shutdown() noexcept;

		/**
		 * Once per type, before anything loads it; loads of other types may already be running.
		 * Types are process wide: one manager at a time.
		 * `context` is what its loads and hooks reach through context<C>(): the system its payloads
		 * belong to, which must outlive every payload of the type.
		 */
		template <AssetType T> void register_type(const char* name, void* context = nullptr) noexcept
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
				.publish		 = publish_thunk<T>(),
				.refresh		 = refresh_thunk<T>(),
				.context		 = context,
				.reads_own_files = ReadsOwnFiles<T>,
			});
		}

		/**
		 * Serves the files under `directory` as `prefix/...`, and watches them with the root when hot
		 * reload is on. Before the first load, like register_type(). A mount hides a directory of the
		 * same name below the root.
		 */
		void mount(StringView prefix, StringView directory) noexcept;

		/**
		 * Serves a source of the game's own, a pack say, as `prefix/...`. The source outlives the
		 * manager. Before the first load.
		 */
		void mount(StringView prefix, AssetSource& source) noexcept;

		/**
		 * The asset at path, loading it if nothing holds it yet. Returns at  once; wait() on the
		 * reference for the first load, or read it when it turns true. The same path is the same
		 * asset while a reference to it lives; asking for it as another type is a bug and asserts.
		 *
		 * A null reference when the registry is full.
		 */
		template <AssetType T> [[nodiscard]] AssetRef<T> load(StringView path) noexcept
		{
			return AssetRef<T>(request(s_type_index<T>, path, 0), typename AssetRef<T>::Adopt{});
		}

		/**
		 * A file changed on disk, by the name an asset loads it by. Any thread; the watcher calls
		 * this. A name nothing loaded reloads nothing, but is still recorded for take_changed().
		 */
		void notify_changed(StringView name) noexcept;

		/** The same by id alone: reloads as above, and is recorded without a name, which take_changed() skips. */
		void notify_changed(AssetId id) noexcept;

		/**
		 * A file written whole by this process, or delivered from another: nothing of it is half
		 * written, so it reloads at the next pump instead of waiting reload_delay for quiet, and the
		 * watcher's report of the same write, which follows within that delay, is dropped. Any thread.
		 */
		void notify_written(StringView name) noexcept;

		/**
		 * The changes pump() has reloaded for since the last call, taken out into `out`, which holds
		 * them and nothing else after: a host hands them on to other machines. Owner thread, after
		 * pump(). Empty unless AssetManagerDef::record_changes.
		 */
		void take_changed(Vector<AssetChange>& out) noexcept;

		/**
		 * Bytes for a name that arrived from another machine: handed to the source the name belongs
		 * to, on the IO thread, and then reloaded as a save would be. False at once for a name no
		 * source serves, one that climbs out, or one under a mount unless def.deliver_to_mounts. The
		 * bytes are copied. Any thread.
		 */
		[[nodiscard]] bool deliver(StringView name, Span<const u8> bytes) noexcept;

		/**
		 * hash_bytes() of what a name reads as now, or nothing when it cannot be read: how a joiner
		 * tells which of a host's files it already has. Reads on the calling thread: for a few names
		 * at a time, on the owner thread.
		 */
		[[nodiscard]] std::optional<u64> file_hash(StringView name) noexcept;

		/** True for a name under a mount rather than the root. */
		[[nodiscard]] bool is_mounted(StringView name) const noexcept;

		/**
		 * Every name below `prefix` ("scripts", "ember/materials"), as assets are named, appended to `out`: the
		 * root's or the mount's source lists it. Owner thread, at a start; a source that cannot list itself,
		 * or a prefix nothing serves, gives an error and adds nothing.
		 */
		[[nodiscard]] Result<void, fs::FileError> enumerate(StringView prefix, Vector<String>& out) noexcept;

		/**
		 * Once per frame on the owner thread, before the frame's update is kicked: unloads assets
		 * nothing references, publishes what finished for types that publish here, folds finished
		 * reloads into their payloads, refreshes the payloads those touched, frees what earlier
		 * reloads left behind, and turns quiet file changes into reloads.
		 */
		void pump(u64 frame_index) noexcept;

		/**
		 * Parks until no loader is running. For boot and level loads; new requests restart loaders.
		 * On the owner thread it then publishes what finished for types that publish there, so a
		 * boot that waits here has every asset it asked for, and everything those asked for.
		 */
		void wait_idle() noexcept;

		/** The root, absolute and ending in a separator. */
		[[nodiscard]] StringView root() const noexcept { return m_root; }

		/** True while saves on disk reload assets. */
		[[nodiscard]] bool watching() const noexcept { return m_watcher != nullptr; }

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
		friend class AssetLoad;
		friend void detail::unreferenced(detail::AssetSlot& slot) noexcept;
		friend void detail::wait(detail::AssetSlot& slot) noexcept;

		static constexpr u16 NO_TYPE = 0xFFFF;

		/// The most types a manager holds: the engine's handful and a game's own.
		static constexpr u16 MAX_TYPES = 64;

		/**
		 * Pumps a retired payload waits before it is unloaded. One covers a bare pointer a frame's
		 * stages took out of it, where no AssetRef can live: whatever is retired while a frame is
		 * under way, or at the pump before it, is still there when that frame ends.
		 */
		static constexpr u64 RETIRE_GRACE = 1;

		struct Type
		{
			const char* name									  = nullptr;
			u32 size											  = 0;
			u32 align											  = 0;
			bool (*load)(AssetLoad&, void*) noexcept			  = nullptr; // constructs, then T::load
			void (*unload)(AssetServices&, void*) noexcept		  = nullptr; // T::unload, then destroys
			void (*reload)(AssetServices&, void*, void*) noexcept = nullptr; // T::reload, or a swap
			bool (*publish)(AssetServices&, void*) noexcept		  = nullptr; // null: the loader publishes
			void (*refresh)(AssetServices&, void*) noexcept		  = nullptr; // null: nothing to rebind
			void* context										  = nullptr;
			bool reads_own_files								  = false;
		};

		template <class T> static constexpr auto publish_thunk() noexcept
		{
			if constexpr (HasAssetPublish<T>)
				return +[](AssetServices& services, void* payload) noexcept
				{ return T::publish(services, *static_cast<T*>(payload)); };
			else
				return static_cast<bool (*)(AssetServices&, void*) noexcept>(nullptr);
		}

		template <class T> static constexpr auto refresh_thunk() noexcept
		{
			if constexpr (HasAssetRefresh<T>)
				return +[](AssetServices& services, void* payload) noexcept
				{ T::refresh(services, *static_cast<T*>(payload)); };
			else
				return static_cast<void (*)(AssetServices&, void*) noexcept>(nullptr);
		}

		/**
		 * One asset: what references see, plus what only the manager and its loaders touch.
		 * Constructed by request() and destroyed by free_slot(), both under the lock.
		 */
		struct Slot : detail::AssetSlot
		{
			Slot(u16 type, AssetId id, String&& path, String&& local, String&& file, AssetSource* source) noexcept
				: type(type), id(id), path(std::move(path)), local(std::move(local)), file(std::move(file)),
				  source(source), dependents(this->path.get_allocator())
			{
			}

			u16 type;
			AssetId id;
			String path;		   // the name: below the root or under a mount's prefix
			String local;		   // the name below its source: the prefix taken off
			String file;		   // absolute, for a type that reads its own files; empty when the source has none
			AssetSource* source;   // what reads it; outlives the slot
			void* fresh = nullptr; // a payload waiting for the pump: a reload, or a first to publish; under the lock

			// Under the lock: the slots that loaded this one through AssetLoad::load(), which hear
			// when it loads or reloads, and whether any load of it has made a payload yet.
			Vector<u32> dependents;
			bool produced = false;

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

		/// The name rides along for take_changed(); longer than the cell holds, it is reported by id alone.
		/// `settled`: written whole, so due at the next pump rather than after the quiet time.
		struct Change
		{
			AssetId id;
			u64 time_ns;
			u16 size;
			bool settled;
			char name[237];
		};

		static_assert(sizeof(Change) == 256);

		/** A settled change the pump took: the watcher's report of the same write is dropped for a while. */
		struct Settled
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

		/** A source served under a name that is not below the root. */
		struct Mount
		{
			String prefix;		 // without a separator: "ember"
			AssetSource* source; // the game's, or a DirectorySource of this manager's own
			bool owned;			 // made by mount(prefix, directory): deleted at shutdown
		};

		/** A name taken apart: its source, what it is called there, and the file behind it if any. */
		struct Resolved
		{
			explicit Resolved(Heap& heap) noexcept : name(&heap), local(&heap), file(&heap) {}

			String name;  // normalised, as assets are named
			String local; // below the source
			String file;  // absolute; empty for a source without files
			AssetSource* source = nullptr;
			bool mounted		= false;
		};

		/** A file that is no asset, and a slot made from it. */
		struct FileDependency
		{
			AssetId file;
			u32 slot; // SlotHandle bits
		};

		template <class T> inline static u16 s_type_index = NO_TYPE;

		[[nodiscard]] u16 add_type(const Type& type) noexcept;
		[[nodiscard]] AssetServices services(const Type& type) const noexcept;
		[[nodiscard]] bool resolve(StringView path, Resolved& out) const noexcept;
		[[nodiscard]] bool name_of(StringView file, String& name) const noexcept;
		void mount_source(StringView prefix, AssetSource& source, bool owned) noexcept;
		[[nodiscard]] Result<fs::FileData, fs::FileError> read_source(AssetSource& source,
																	  StringView local) noexcept; // parks the caller
		void queue_named(StringView name, bool settled) noexcept;
		void record_change(const Change& change) noexcept;
		[[nodiscard]] Slot* request(u16 type, StringView path, u32 parent) noexcept;
		void unreferenced(detail::AssetSlot& slot) noexcept;
		void wait(detail::AssetSlot& slot) noexcept;
		void free_slot(Slot& slot, SlotHandle handle) noexcept; // under the lock

		void reload(AssetId id) noexcept;
		void queue_load(Slot& slot, SlotHandle handle) noexcept;
		void wake_loader() noexcept;
		void run_loader() noexcept;
		void load_one(Slot& slot, SlotHandle handle) noexcept;
		[[nodiscard]] bool read_file(const Slot& slot, fs::FileData& out) noexcept; // parks the loader
		void publish(Slot& slot, SlotHandle handle, void* payload) noexcept;
		void apply_fresh(bool between_frames) noexcept;
		void notify_dependents(Slot& slot) noexcept; // under the lock
		void depend(SlotHandle handle, Span<const AssetId> files, bool replace) noexcept;
		void retire(void* payload, u16 type) noexcept; // under the lock
		void release(const Retired& retired) noexcept;

		gpu::Device* m_gpu = nullptr;
		Heap* m_heap	   = nullptr;
		String m_root;
		DirectorySource* m_root_source = nullptr; // the root as a source, owned
		u64 m_reload_delay_ns		   = 0;
		bool m_record_changes		   = false;
		bool m_deliver_to_mounts	   = false;

		// Slots, loaders and waiters read a type by index without the lock, and a loader holds on to
		// one for a whole load, so the table grows in place and never moves: a type added while
		// loads run disturbs none of them.
		std::array<Type, MAX_TYPES> m_types = {};
		u16 m_type_count					= 0;

		// Fixed once loads begin: every request resolves its path through them without the lock.
		Vector<Mount> m_mounts;

		/// Slots, the path index, the dependency lists, pending payloads and the retire list move
		/// under this lock; a request is rare next to a read through a reference, which never takes it.
		mutable SpinMutex m_lock;
		SlotPool m_slots;
		HashMap<AssetId, u32> m_by_id; // SlotHandle bits
		Vector<FileDependency> m_file_dependencies;
		Vector<u32> m_fresh;   // slots with a payload waiting for the pump
		Vector<u32> m_refresh; // slots to refresh at the next pump
		Vector<Retired> m_retired;
		Vector<Retired> m_due; // pump only: taken out from under the lock, unloaded outside it

		MpmcQueue<Request> m_requests; // one cell per slot, so it can never be full
		MpmcQueue<u32> m_unreferenced; // slots whose last reference went; one entry per slot at most
		MpmcQueue<Change> m_changes;
		Vector<Change> m_pending;	   // pump only: changes waiting to go quiet
		Vector<Settled> m_settled;	   // pump only: settled changes taken lately, whose echoes are dropped
		Vector<AssetChange> m_changed; // owner thread: what pump() reloaded for, until take_changed()

		std::atomic<u64> m_frame{0}; // the last pump's index, for retire stamps
		std::atomic<u32> m_running{0};
		u32 m_loader_count = 0;
		jobs::Counter m_loaders; // every loader job kicked, for wait_idle

		/// The OS watch on the root and the mounts, when hot reload is on. Its thread only ever
		/// calls notify_changed().
		detail::FileWatcher* m_watcher = nullptr;
	};

	template <class T> AssetRef<T> AssetLoad::load(StringView path) noexcept
	{
		return AssetRef<T>(m_manager->request(AssetManager::s_type_index<T>, path, m_slot),
						   typename AssetRef<T>::Adopt{});
	}
}
