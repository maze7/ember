#pragma once

#include <ember/assets/asset.h>
#include <ember/assets/asset_loader.h>
#include <ember/assets/asset_source.h>
#include <ember/containers/span.h>
#include <ember/core/common.h>
#include <ember/sync/spin_mutex.h>

#include <utility>

namespace ember
{
	template <class T> class AssetRef;

	/// A source rooted at a prefix. The prefix must be empty or end in '/', and it must outlive
	/// the manager, so a literal is the usual choice.
	struct AssetMount
	{
		StringView prefix{};
		AssetSource* source = nullptr;
	};

	inline constexpr u32 MAX_ASSET_MOUNTS = 4;

	struct AssetManagerDef
	{
		AssetMount mounts[MAX_ASSET_MOUNTS] = {}; // first prefix that matches wins
		u32 max_assets						= 4096;
		u32 max_types						= 32;
		u32 max_in_flight					= 64; // load jobs alive at once; each holds a job counter until it lands
		u32 install_budget					= 8;  // landed loads published per update()
		jobs::JobPriority load_priority		= jobs::JobPriority::Low;
		bool use_jobs						= true; // false runs every load inside update(), for tools and tests
	};

	/**
	 * Owns every loaded asset, the table of records that name them, and the pipeline that moves
	 * bytes from a source into a published value.
	 *
	 * A record is one slot: path, type, the published value, a state and a revision, and a count
	 * of the strong references alive. load() finds or creates the record and hands back a strong
	 * reference; the value arrives on a later update(), and get() reads it with one acquire load.
	 * A record whose last reference drops is retired on the next update and its slot reused after
	 * that, so a weak handle kept past the retirement reads as no asset.
	 *
	 * A load is one job: it reads the file through the mount's source and runs prepare on a
	 * worker, and writes the result into a block only the owner touches once the batch completes.
	 * The owner keeps up to max_in_flight going, lands the finished ones under the install budget
	 * and releases their counters. A record reloaded while its load is in flight gets a second
	 * job, and the attempt number decides which result counts: a stale one is destroyed unseen.
	 * With use_jobs off the same steps run inline.
	 *
	 * Landing runs the type's hooks: refresh applies the payload to the live asset in place when
	 * it can, otherwise install builds a new asset and publishing swaps it in and bumps the
	 * revision. The old value waits in the graveyard for one update, which is what makes a pointer
	 * from get() valid until the next update() on any thread. Nothing is destroyed on the spot,
	 * and unload runs right before an asset is.
	 *
	 * Threading. Any thread may load, find, get, read a status or revision, and copy or drop a
	 * reference. The thread that constructed the manager owns it: update, wait, reload, type
	 * registration, events, and every hook other than prepare run there and nowhere else. The
	 * lock inside covers the path map and the queues for microseconds and is never held across a
	 * wait. The job system outlives the manager, and a load never touches frame memory.
	 *
	 * Capacities are fixed by the def. A full table logs and hands back a null reference, and a
	 * broken file becomes a Failed record that reads as null and says why, so content problems
	 * never take the game down.
	 */
	class AssetManager final
	{
	public:
		explicit AssetManager(const AssetManagerDef& def) noexcept;
		~AssetManager() noexcept;

		AssetManager(const AssetManager&)			 = delete;
		AssetManager& operator=(const AssetManager&) = delete;

		/// Owner. Every type loads through a registration made before its first load.
		template <class T> void register_type(const char* name) noexcept { register_raw(detail::static_type<T>(name)); }

		/// Owner. Registers a loader object for L::Asset; the manager owns the object from here on.
		template <class L> void register_loader(L loader, const char* name) noexcept
		{
			register_raw(detail::loader_type<L>(std::move(loader), name, m_memory));
		}

		/// A strong reference to the asset at path, loading it if nothing holds it yet. Returns at
		/// once; the value arrives on a later update(). Null, with a log line, for an unregistered
		/// type, a path already loaded as another type, or a full table.
		template <class T> [[nodiscard]] AssetRef<T> load(StringView path) noexcept;

		/// A strong reference to an asset already loaded under this id, or null.
		template <class T> [[nodiscard]] AssetRef<T> find(AssetId id) noexcept;

		/// The published value, or null while nothing is published or the handle is dead. Valid
		/// until the next update().
		template <class T> [[nodiscard]] const T* get(AssetHandle<T> handle) const noexcept
		{
			return static_cast<const T*>(get_raw(handle.raw));
		}

		[[nodiscard]] AssetStatus status(RawAssetHandle handle) const noexcept;
		[[nodiscard]] u32 revision(RawAssetHandle handle) const noexcept;
		[[nodiscard]] AssetId id(RawAssetHandle handle) const noexcept;

		/// Owner. Empty for a dead handle.
		[[nodiscard]] StringView path(RawAssetHandle handle) const noexcept;
		[[nodiscard]] StringView error_message(RawAssetHandle handle) const noexcept;

		/// Owner, once per frame. Drains retirements, lands finished loads under the install
		/// budget, starts queued ones and replaces the event list.
		void update() noexcept;

		/// Owner. Parks or blocks until the record has a value or a failure. Boot and tests.
		void wait(RawAssetHandle handle) noexcept;

		/// Owner. wait() for every live record. Boot.
		void wait_all() noexcept;

		/// Owner. Queues a fresh load; the current value stays published until the new one lands.
		void reload(AssetId id) noexcept;

		/// Owner. Everything published or failed since the last update().
		[[nodiscard]] Span<const AssetEvent> events() const noexcept { return Span<const AssetEvent>(m_events); }

		/// Owner. Records alive, retiring ones included.
		[[nodiscard]] u32 live_count() const noexcept;

		/// Owner. Loads kicked and not yet landed, and loads queued behind max_in_flight.
		[[nodiscard]] u32 in_flight_count() const noexcept { return static_cast<u32>(m_in_flight.size()); }
		[[nodiscard]] u32 pending_count() const noexcept;

	private:
		template <class T> friend class AssetRef;

		struct Record;
		struct Load;

		/// A value waiting one update before it dies; free_slot marks a retired record's last trip.
		struct Dead
		{
			void* value					  = nullptr;
			const detail::AssetType* type = nullptr;
			u32 slot					  = 0;
			bool free_slot				  = false;
		};

		void register_raw(const detail::AssetType& type) noexcept;
		[[nodiscard]] const detail::AssetType* find_type(const void* tag) const noexcept;

		[[nodiscard]] RawAssetHandle load_raw(StringView path, const detail::AssetType* type) noexcept;
		[[nodiscard]] RawAssetHandle find_raw(AssetId id, const detail::AssetType* type) noexcept;
		[[nodiscard]] const void* get_raw(RawAssetHandle handle) const noexcept;
		[[nodiscard]] Record* record_of(RawAssetHandle handle) const noexcept;
		[[nodiscard]] AssetContext context(const Record& record) noexcept;
		[[nodiscard]] AssetSource* resolve(StringView path, StringView& sub_path) const noexcept;
		[[nodiscard]] bool is_owner() const noexcept;

		void acquire(RawAssetHandle handle) noexcept;
		void release(RawAssetHandle handle) noexcept;

		void pump(u32 budget, bool clear_events) noexcept;
		void destroy_graveyard() noexcept;
		void retire_unreferenced() noexcept;
		void finish_loads(u32 budget) noexcept;
		void start_loads(u32 limit) noexcept;
		void load_inline(u32 budget) noexcept;

		[[nodiscard]] bool pop_pending(u32& slot) noexcept;
		void queue_front(u32 slot) noexcept;
		[[nodiscard]] Load* begin_load(u32 slot) noexcept;
		[[nodiscard]] Load* find_in_flight(u32 slot) const noexcept;
		void consume(Load& load) noexcept;
		void land(Record& record, u32 slot, Load& load) noexcept;
		void publish(Record& record, u32 slot, void* value) noexcept;
		void mark_published(Record& record, u32 slot, AssetEventType type) noexcept;
		void fail(Record& record, u32 slot, AssetError error, const char* message) noexcept;
		[[nodiscard]] static bool is_settled(const Record& record) noexcept;

		std::pmr::memory_resource& m_memory;
		AssetManagerDef m_def;
		u32 m_owner_thread = 0;
		u32 m_capacity	   = 0;

		Vector<detail::AssetType> m_types;
		Record* m_records = nullptr;

		// Under m_mutex: the map, the free list, the load queue and the zero list.
		mutable SpinMutex m_mutex;
		HashMap<AssetId, u32> m_slots;
		Vector<u32> m_free;
		Vector<u32> m_pending;
		Vector<u32> m_zero;

		// Owner only.
		Vector<Load*> m_in_flight;
		Vector<Dead> m_graveyard;
		Vector<AssetEvent> m_events;
	};

	/**
	 * Strong reference: the asset stays loaded while one exists. Sixteen bytes, copies from any
	 * thread, and holds no pointer to the value, so a reload never leaves it stale. Components
	 * hold one of these; nothing holds the value pointer across frames.
	 */
	template <class T> class AssetRef
	{
	public:
		AssetRef() noexcept = default;
		~AssetRef() noexcept { reset(); }

		AssetRef(const AssetRef& other) noexcept : m_manager(other.m_manager), m_handle(other.m_handle)
		{
			if (!m_handle.is_null())
				m_manager->acquire(m_handle.raw);
		}

		AssetRef(AssetRef&& other) noexcept : m_manager(other.m_manager), m_handle(other.m_handle)
		{
			other.m_manager = nullptr;
			other.m_handle	= {};
		}

		AssetRef& operator=(const AssetRef& other) noexcept
		{
			AssetRef copy(other);
			std::swap(m_manager, copy.m_manager);
			std::swap(m_handle, copy.m_handle);
			return *this;
		}

		AssetRef& operator=(AssetRef&& other) noexcept
		{
			if (this != &other)
			{
				reset();
				m_manager		= other.m_manager;
				m_handle		= other.m_handle;
				other.m_manager = nullptr;
				other.m_handle	= {};
			}

			return *this;
		}

		/// Null while loading, failed, or unset. Valid until the next update().
		[[nodiscard]] const T* get() const noexcept { return m_manager ? m_manager->get(m_handle) : nullptr; }

		[[nodiscard]] const T* operator->() const noexcept
		{
			const T* value = get();
			EMBER_ASSERT(value != nullptr);
			return value;
		}

		[[nodiscard]] explicit operator bool() const noexcept { return get() != nullptr; }

		[[nodiscard]] bool is_null() const noexcept { return m_handle.is_null(); }
		[[nodiscard]] AssetHandle<T> handle() const noexcept { return m_handle; }

		/// The weak handle, implicit from a reference that is kept. A temporary cannot convert: its
		/// record retires on the next update, so the handle would read as no asset straight away.
		operator AssetHandle<T>() const& noexcept { return m_handle; }
		operator AssetHandle<T>() && = delete;
		operator RawAssetHandle() const& noexcept { return m_handle.raw; }
		operator RawAssetHandle() && = delete;

		/// A null reference reads as Failed, the same as a dead handle.
		[[nodiscard]] AssetStatus status() const noexcept
		{
			return m_manager ? m_manager->status(m_handle.raw) : AssetStatus{.state = AssetState::Failed};
		}

		[[nodiscard]] u32 revision() const noexcept { return m_manager ? m_manager->revision(m_handle.raw) : 0; }
		[[nodiscard]] AssetId id() const noexcept { return m_manager ? m_manager->id(m_handle.raw) : NO_ASSET; }

		/// True once per publish: compares against seen and updates it. Start seen at zero and
		/// rebuild whatever was derived from the value each time this returns true.
		[[nodiscard]] bool changed(u32& seen) const noexcept
		{
			const u32 current = revision();
			if (current == seen)
				return false;

			seen = current;
			return true;
		}

		void reset() noexcept
		{
			if (!m_handle.is_null())
				m_manager->release(m_handle.raw);

			m_manager = nullptr;
			m_handle  = {};
		}

		/// Parks or blocks until the asset has a value or a failure and hands the reference back
		AssetRef& wait() & noexcept
		{
			if (m_manager != nullptr)
				m_manager->wait(m_handle.raw);

			return *this;
		}

		AssetRef&& wait() && noexcept
		{
			if (m_manager != nullptr)
				m_manager->wait(m_handle.raw);

			return std::move(*this);
		}

	private:
		friend class AssetManager;

		/// Adopts the reference the manager already counted.
		AssetRef(AssetManager* manager, AssetHandle<T> handle) noexcept
			: m_manager(handle.is_null() ? nullptr : manager), m_handle(handle)
		{
		}

		AssetManager* m_manager = nullptr;
		AssetHandle<T> m_handle{};
	};

	template <class T> AssetRef<T> AssetManager::load(StringView path) noexcept
	{
		return AssetRef<T>(this, AssetHandle<T>{load_raw(path, find_type(detail::type_tag<T>))});
	}

	template <class T> AssetRef<T> AssetManager::find(AssetId id) noexcept
	{
		return AssetRef<T>(this, AssetHandle<T>{find_raw(id, find_type(detail::type_tag<T>))});
	}
}
