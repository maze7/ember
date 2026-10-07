#include <ember/assets/asset.h>
#include <ember/assets/texture_asset.h>
#include <ember/core/bits.h>
#include <ember/core/filesystem.h>
#include <ember/core/logger.h>
#include <ember/gpu/device.h>
#include <ember/jobs/file_io.h>

#include <assets/file_watcher.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>

namespace
{
	// Steady clock nanoseconds, for the debounce. Only differences mean anything.
	[[nodiscard]] ember::u64 now_ns() noexcept
	{
		return static_cast<ember::u64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
				.count());
	}
}

namespace ember::detail
{
	void unreferenced(AssetSlot& slot) noexcept { slot.manager->unreferenced(slot); }

	void wait(AssetSlot& slot) noexcept { slot.manager->wait(slot); }
}

namespace ember
{
	void AssetLoad::depends_on(StringView path) noexcept
	{
		String name(m_heap);

		// Outside the root and every mount, no watch would ever report a change to it.
		if (!m_manager->name_of(path, name))
			return;

		const AssetId id = asset_id(name);

		if (std::find(m_dependencies.begin(), m_dependencies.end(), id) == m_dependencies.end())
			m_dependencies.push_back(id);
	}

	void AssetLoad::notify_written(StringView name) noexcept { m_manager->notify_written(name); }

	Result<fs::FileData, fs::FileError> AssetLoad::read(StringView name) noexcept
	{
		AssetManager::Resolved resolved(*m_heap);

		if (!m_manager->resolve(name, resolved))
			return tl::unexpected(fs::FileError{.code = fs::FileErrorCode::NotFound, .op = fs::FileOp::ResolvePath});

		// Made of it, whether or not the read works: a file that arrives later must bring this asset.
		depends_on(resolved.name);
		return m_manager->read_source(*resolved.source, resolved.local);
	}

	AssetManager::AssetManager() noexcept
		: m_root(&memory::heap(MemoryTag::Assets)), m_mounts(&memory::heap(MemoryTag::Assets)),
		  m_slots(MemoryTag::Assets), m_by_id(&memory::heap(MemoryTag::Assets)),
		  m_file_dependencies(&memory::heap(MemoryTag::Assets)), m_fresh(&memory::heap(MemoryTag::Assets)),
		  m_refresh(&memory::heap(MemoryTag::Assets)), m_retired(&memory::heap(MemoryTag::Assets)),
		  m_due(&memory::heap(MemoryTag::Assets)), m_requests(MemoryTag::Assets), m_unreferenced(MemoryTag::Assets),
		  m_changes(MemoryTag::Assets), m_pending(&memory::heap(MemoryTag::Assets)),
		  m_changed(&memory::heap(MemoryTag::Assets))
	{
	}

	AssetManager::~AssetManager() noexcept { shutdown(); }

	void AssetManager::init(gpu::Device& gpu, const AssetManagerDef& def) noexcept
	{
		EMBER_ASSERT(m_gpu == nullptr && "init runs once");
		EMBER_ASSERT(def.max_assets != 0 && def.max_assets <= SlotPool::MAX_CAPACITY);
		EMBER_ASSERT(def.max_changes >= 2);

		m_gpu				= &gpu;
		m_heap				= &memory::heap(MemoryTag::Assets);
		m_reload_delay_ns	= u64{def.reload_delay_ms} * 1'000'000;
		m_loader_count		= def.loaders != 0 ? def.loaders : std::max(1u, jobs::worker_count() / 2);
		m_record_changes	= def.record_changes;
		m_deliver_to_mounts = def.deliver_to_mounts;

		// The root is resolved once, here, and kept with its separator: every load joins onto it
		// and every change the watcher reports begins with it, so the two name a file the same
		// way whatever the working directory does later.
		if (!fs::absolute(def.root[0] != '\0' ? def.root : ".", m_root))
		{
			EMBER_WARN("asset root '{}' could not be resolved; loads will use it as given", def.root);
			m_root = def.root;
		}

		if (m_root.empty() || m_root.back() != '/')
			m_root += '/';

		m_root_source = memory::new_object<DirectorySource>(MemoryTag::Assets, StringView(m_root));

		// A slot is queued once at a time and unreferenced once at a time, so a cell per slot means
		// neither queue can ever be full.
		m_slots.init(def.max_assets);
		m_requests.init(std::bit_ceil(def.max_assets));
		m_unreferenced.init(std::bit_ceil(def.max_assets));
		m_changes.init(std::bit_ceil(def.max_changes));

		register_type<TextureAsset>("texture");

		// Last, so the first event it can deliver finds a manager ready to take it.
		if (def.hot_reload)
		{
			m_watcher = memory::new_object<detail::FileWatcher>(MemoryTag::Assets, *this);

			if (!m_watcher->watch(m_root, {}))
			{
				memory::delete_object(MemoryTag::Assets, m_watcher);
				m_watcher = nullptr;
			}
		}
	}

	void AssetManager::shutdown() noexcept
	{
		if (m_gpu == nullptr)
			return;

		// First, so no event lands on a registry that is being emptied.
		memory::delete_object(MemoryTag::Assets, m_watcher);
		m_watcher = nullptr;

		// Not wait_idle(): what the loaders finished is about to be unloaded, not published.
		jobs::wait(m_loaders);

		// In rounds: the assets nothing holds, then the ones only those were holding, until a round
		// frees nothing. A payload's references are what hold the assets it was made of, and they go
		// when it is unloaded, which is outside the lock, since an unload may call the manager.
		for (bool any = true; any;)
		{
			{
				std::lock_guard lock(m_lock);

				for (auto it = m_slots.begin(); it != m_slots.end();)
				{
					const SlotHandle handle = it.handle();
					++it;

					Slot& slot = *m_slots.get(handle);
					EMBER_ASSERT(!slot.queued.load(std::memory_order_relaxed) && "a load outlived its loader");

					if (slot.refs.load(std::memory_order_acquire) != 0)
						continue;

					m_by_id.erase(slot.id);
					free_slot(slot, handle);
				}

				m_due = std::move(m_retired);
				m_retired.clear();
			}

			// Everything retired now is past every reader: no grace.
			for (const Retired& retired : m_due)
				release(retired);

			any = !m_due.empty();
			m_due.clear();
		}

		// What is left is held from outside, by a reference that will outlive the manager.
		{
			std::lock_guard lock(m_lock);
			EMBER_ASSERT(m_slots.empty() && "an AssetRef outlives the asset manager");

			for (auto it = m_slots.begin(); it != m_slots.end();)
			{
				const SlotHandle handle = it.handle();
				++it;
				free_slot(*m_slots.get(handle), handle);
			}

			m_due = std::move(m_retired);
			m_retired.clear();
		}

		for (const Retired& retired : m_due)
			release(retired);

		m_due.clear();
		m_by_id.clear();
		m_file_dependencies.clear();
		m_fresh.clear();
		m_refresh.clear();
		m_pending.clear();
		m_changed.clear();

		// The sources go last: an unload above may still have read through one.
		for (Mount& mount : m_mounts)
			if (mount.owned)
				memory::delete_object(MemoryTag::Assets, static_cast<DirectorySource*>(mount.source));

		m_mounts.clear();
		memory::delete_object(MemoryTag::Assets, m_root_source);
		m_root_source = nullptr;

		m_type_count = 0;
		m_gpu		 = nullptr;
		m_heap		 = nullptr;
	}

	u16 AssetManager::add_type(const Type& type) noexcept
	{
		// Loads of the types before it may be running: each holds its own entry, which stays put.
		EMBER_ASSERT(m_type_count < MAX_TYPES && "more asset types than MAX_TYPES");

		m_types[m_type_count] = type;
		return m_type_count++;
	}

	void AssetManager::mount(StringView prefix, StringView directory) noexcept
	{
		String absolute(m_heap);

		if (!fs::absolute(directory, absolute))
		{
			EMBER_ERROR("asset mount '{}': '{}' is not a usable path", prefix, directory);
			return;
		}

		if (absolute.back() != '/')
			absolute += '/';

		mount_source(prefix, *memory::new_object<DirectorySource>(MemoryTag::Assets, StringView(absolute)), true);
	}

	void AssetManager::mount(StringView prefix, AssetSource& source) noexcept { mount_source(prefix, source, false); }

	void AssetManager::mount_source(StringView prefix, AssetSource& source, bool owned) noexcept
	{
		EMBER_ASSERT(m_gpu != nullptr && "mount after init");
		EMBER_ASSERT(m_slots.empty() && "mount before the first load");
		EMBER_ASSERT(!prefix.empty() && prefix.find('/') == StringView::npos && prefix != "." && prefix != "..");

		// A source the OS cannot watch still serves its files; its saves, if it has any, go unnoticed.
		const StringView directory = source.watch_directory();
		if (m_watcher != nullptr && !directory.empty())
			(void)m_watcher->watch(directory, prefix);

		m_mounts.push_back({.prefix = String(prefix, m_heap), .source = &source, .owned = owned});
	}

	AssetServices AssetManager::services(const Type& type) const noexcept { return {*m_gpu, *m_heap, type.context}; }

	bool AssetManager::resolve(StringView path, Resolved& out) const noexcept
	{
		// A name is relative and stays below what it names. Normalising it first means two spellings
		// of one file meet at one id, and one that climbs out is refused rather than resolved.
		String& name = out.name;
		if (path.empty() || fs::is_absolute(path) || !fs::normalize_lexical(path, name) || name.empty() ||
			name == "." || name == ".." || name.starts_with("../"))
			return false;

		out.source	= m_root_source;
		out.mounted = false;
		out.local	= name;

		for (const Mount& mount : m_mounts)
		{
			if (name.size() > mount.prefix.size() && name.starts_with(mount.prefix) && name[mount.prefix.size()] == '/')
			{
				out.source	= mount.source;
				out.mounted = true;
				out.local.assign(StringView(name).substr(mount.prefix.size() + 1));
				break;
			}
		}

		// A source without files, a pack, leaves the file empty: nothing a loader needs.
		if (!out.source->file_of(out.local, out.file))
			out.file.clear();

		return true;
	}

	bool AssetManager::name_of(StringView file, String& name) const noexcept
	{
		if (!fs::is_absolute(file))
		{
			Resolved resolved(*m_heap);
			if (!resolve(file, resolved))
				return false;

			name = std::move(resolved.name);
			return true;
		}

		String rest(m_heap);

		for (const Mount& mount : m_mounts)
		{
			if (mount.source->name_of(file, rest))
			{
				name = mount.prefix;
				name += '/';
				name += rest;
				return true;
			}
		}

		return m_root_source->name_of(file, name);
	}

	bool AssetManager::is_mounted(StringView name) const noexcept
	{
		Resolved resolved(*m_heap);
		return resolve(name, resolved) && resolved.mounted;
	}

	AssetManager::Slot* AssetManager::request(u16 type, StringView path, u32 parent) noexcept
	{
		EMBER_ASSERT(type != NO_TYPE && "register the type before loading it");
		EMBER_ASSERT(m_gpu != nullptr && "load before init");

		// The name, its source and the file first, outside the lock.
		Resolved resolved(*m_heap);

		if (!resolve(path, resolved))
		{
			EMBER_ERROR("asset '{}': not a path inside the asset root or a mount", path);
			return nullptr;
		}

		const AssetId id = asset_id(resolved.name);

		std::lock_guard lock(m_lock);

		Slot* slot = nullptr;
		SlotHandle handle;

		// The reference is counted here, under the lock, so a pump that finds the slot queued as
		// unreferenced sees the count and leaves it alone.
		if (const auto it = m_by_id.find(id); it != m_by_id.end())
		{
			handle = SlotHandle::from_bits(it->second);
			slot   = m_slots.get(handle);
			EMBER_ASSERT(slot->type == type && "one asset, one type");

			slot->refs.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			handle = m_slots.emplace(type, id, std::move(resolved.name), std::move(resolved.local),
									 std::move(resolved.file), resolved.source);

			if (handle.is_null())
			{
				EMBER_ERROR("asset '{}': registry full ({} assets)", path, m_slots.capacity());
				return nullptr;
			}

			slot		  = m_slots.get(handle);
			slot->manager = this;
			slot->handle  = handle.to_bits();
			slot->refs.store(1, std::memory_order_relaxed);

			m_by_id.emplace(id, handle.to_bits());
			queue_load(*slot, handle);
		}

		// A loader asking for an asset hears when it loads or reloads. Bits are never zero, since a
		// generation never is, so zero can mean no one asked.
		if (parent != 0 && parent != handle.to_bits() &&
			std::find(slot->dependents.begin(), slot->dependents.end(), parent) == slot->dependents.end())
			slot->dependents.push_back(parent);

		return slot;
	}

	void AssetManager::unreferenced(detail::AssetSlot& slot) noexcept
	{
		// One entry per slot: a drop while one is queued is covered by it, and the pump clears the
		// flag before it looks at the count, so a drop after that queues again.
		if (slot.unreferenced.exchange(true, std::memory_order_acq_rel))
			return;

		// Never full by construction; a refused push is the pump mid pop, which always finishes.
		for (u32 spins = 0; !m_unreferenced.try_push(slot.handle); ++spins)
			detail::cpu_relax(spins);
	}

	void AssetManager::wait(detail::AssetSlot& base) noexcept
	{
		Slot& slot = static_cast<Slot&>(base);

		if (m_types[slot.type].publish == nullptr)
		{
			jobs::wait(slot.first_load);
			return;
		}

		// Only the owner thread's pump publishes this type, so a job parked here would hold up the
		// frame that pump waits for. The owner thread waits for the loaders and publishes what they
		// finished itself, again while doing so sets more loads going.
		EMBER_ASSERT(jobs::is_main() && "wait for a type that publishes on the owner thread there; poll it elsewhere");

		while (slot.state.load(std::memory_order_acquire) == AssetState::Loading)
		{
			wait_idle();

			if (m_running.load(std::memory_order_acquire) == 0)
				break;
		}
	}

	void AssetManager::free_slot(Slot& slot, SlotHandle handle) noexcept
	{
		// Both payloads outlive the slot by the grace: the live one for a pointer a frame took, the
		// fresh one because it is simplest to send everything the same way.
		if (void* payload = slot.payload.exchange(nullptr, std::memory_order_acq_rel))
			retire(payload, slot.type);

		if (slot.fresh != nullptr)
			retire(std::exchange(slot.fresh, nullptr), slot.type);

		// A first payload the pump never published leaves the first load open. Nobody holds a
		// reference to wait on it, but the counter must balance before it goes.
		if (slot.state.load(std::memory_order_relaxed) == AssetState::Loading)
		{
			slot.state.store(AssetState::Failed, std::memory_order_relaxed);
			jobs::signal(slot.first_load);
		}

		const u32 bits = handle.to_bits();
		std::erase_if(m_file_dependencies,
					  [bits](const FileDependency& dependency) { return dependency.slot == bits; });

		(void)m_slots.erase(handle);
	}

	void AssetManager::notify_changed(StringView name) noexcept { queue_named(name, false); }

	void AssetManager::notify_written(StringView name) noexcept { queue_named(name, true); }

	void AssetManager::queue_named(StringView name, bool settled) noexcept
	{
		Change change{.id = asset_id(name), .time_ns = now_ns(), .size = 0, .settled = settled, .name = {}};

		if (name.size() < sizeof(change.name))
		{
			std::memcpy(change.name, name.data(), name.size());
			change.size = static_cast<u16>(name.size());
		}

		record_change(change);
	}

	void AssetManager::notify_changed(AssetId id) noexcept
	{
		record_change({.id = id, .time_ns = now_ns(), .size = 0, .settled = false, .name = {}});
	}

	void AssetManager::record_change(const Change& change) noexcept
	{
		// A refused push is usually the pump mid pop; a queue that is really full drops the
		// change, and the next save of that file brings another.
		for (u32 spins = 0; !m_changes.try_push(change); ++spins)
		{
			if (spins == 64)
			{
				EMBER_WARN("asset change queue full; a reload was dropped");
				return;
			}

			detail::cpu_relax(spins);
		}
	}

	void AssetManager::take_changed(Vector<AssetChange>& out) noexcept
	{
		EMBER_ASSERT(jobs::is_main() && "take_changed() on the owner thread, after pump()");

		// Copied, not swapped: the caller's vector may live on another heap. It gets the changes and nothing else.
		out.clear();
		for (AssetChange& change : m_changed)
			out.push_back(
				{.name = String(change.name, out.get_allocator()), .id = change.id, .mounted = change.mounted});

		m_changed.clear();
	}

	bool AssetManager::deliver(StringView name, Span<const u8> bytes) noexcept
	{
		// The same rule a load obeys: relative, normalised, below the root or a mount; and a mount
		// only when asked, since a mount is usually someone else's files.
		Resolved resolved(*m_heap);
		if (!resolve(name, resolved) || (resolved.mounted && !m_deliver_to_mounts))
			return false;

		struct Delivery
		{
			AssetManager* manager;
			AssetSource* source;
			String name;
			String local;
			Vector<u8> bytes;
			Result<void, fs::FileError> result;
		};

		Delivery* delivery = memory::new_object<Delivery>(
			MemoryTag::Assets, this, resolved.source, std::move(resolved.name), std::move(resolved.local),
			Vector<u8>(bytes.begin(), bytes.end(), m_heap), Result<void, fs::FileError>{});

		// A job, so the caller never waits on the disk: it hands the bytes to the source on the IO
		// thread, parks, and then names the file to the manager exactly as the watcher would.
		jobs::kick({.fn =
						[](void* data) noexcept
					{
						Delivery& delivery = *static_cast<Delivery*>(data);
						jobs::Counter done;

						const auto submitted = jobs::submit_io(
							{
								.fn =
									[](void* data) noexcept
								{
									Delivery& delivery = *static_cast<Delivery*>(data);
									delivery.result	   = delivery.source->deliver(delivery.local, delivery.bytes);
								},
								.data	  = &delivery,
								.name	  = "deliver asset",
								.priority = jobs::JobPriority::Low,
							},
							done);

						if (submitted)
							jobs::wait(done);

						if (submitted && delivery.result)
							delivery.manager->notify_written(delivery.name);
						else if (submitted)
							EMBER_ERROR("asset '{}': delivery failed ({})", delivery.name,
										enum_name(delivery.result.error().code));
						else
							EMBER_ERROR("asset '{}': delivery refused ({})", delivery.name,
										enum_name(submitted.error()));

						memory::delete_object(MemoryTag::Assets, &delivery);
					},
					.data	  = delivery,
					.name	  = "deliver asset",
					.priority = jobs::JobPriority::Low});

		return true;
	}

	std::optional<u64> AssetManager::file_hash(StringView name) noexcept
	{
		Resolved resolved(*m_heap);
		if (!resolve(name, resolved))
			return std::nullopt;

		const auto data = resolved.source->read(resolved.local, *m_heap);
		if (!data)
			return std::nullopt;

		return hash_bytes(data->bytes());
	}

	void AssetManager::reload(AssetId id) noexcept
	{
		// The asset the file is, and every asset made from it. A dead slot's loader is about to free
		// it and nobody wants it any more; every other slot stays put while this thread holds it,
		// since only the pump, on this thread, frees the living.
		Vector<Slot*> slots(m_heap);

		{
			std::lock_guard lock(m_lock);

			const auto add = [&](u32 bits) noexcept
			{
				Slot* slot = m_slots.get(SlotHandle::from_bits(bits));

				if (slot != nullptr && !slot->dead.load(std::memory_order_acquire) &&
					std::find(slots.begin(), slots.end(), slot) == slots.end())
					slots.push_back(slot);
			};

			if (const auto it = m_by_id.find(id); it != m_by_id.end())
				add(it->second);

			for (const FileDependency& dependency : m_file_dependencies)
				if (dependency.file == id)
					add(dependency.slot);
		}

		for (Slot* slot : slots)
		{
			// Dirty before queued: a loader that is running sees the flag at the end of its pass and
			// goes round again, and one that is not gets queued here.
			slot->dirty.store(true, std::memory_order_release);
			queue_load(*slot, SlotHandle::from_bits(slot->handle));
		}
	}

	void AssetManager::pump(u64 frame_index) noexcept
	{
		m_frame.store(frame_index, std::memory_order_release);

		{
			std::lock_guard lock(m_lock);

			// Assets nothing references. A load() since the drop revived the slot, and the count
			// says so; a load still running owns the slot and frees it when it is done.
			u32 bits = 0;
			while (m_unreferenced.try_pop(bits))
			{
				const SlotHandle handle = SlotHandle::from_bits(bits);
				Slot* slot				= m_slots.get(handle);

				if (slot == nullptr)
					continue;

				slot->unreferenced.store(false, std::memory_order_release);

				if (slot->refs.load(std::memory_order_acquire) != 0)
					continue;

				m_by_id.erase(slot->id);

				if (slot->queued.load(std::memory_order_acquire))
					slot->dead.store(true, std::memory_order_release);
				else
					free_slot(*slot, handle);
			}

			// Retired payloads past the grace.
			for (u32 i = 0; i < m_retired.size();)
			{
				if (m_retired[i].frame + RETIRE_GRACE > frame_index)
				{
					++i;
					continue;
				}

				m_due.push_back(m_retired[i]);
				m_retired[i] = m_retired.back();
				m_retired.pop_back();
			}
		}

		// No stage is running: first payloads publish, reloads fold into the live ones, and then
		// the payloads that loaded any of them hear of it.
		apply_fresh(true);

		Vector<u32> refresh(m_heap);

		{
			std::lock_guard lock(m_lock);
			refresh.swap(m_refresh);
		}

		std::sort(refresh.begin(), refresh.end());
		refresh.erase(std::unique(refresh.begin(), refresh.end()), refresh.end());

		for (const u32 bits : refresh)
		{
			Slot* slot = nullptr;

			// Looked up under the lock, where a dead slot is seen before its loader can free it.
			{
				std::lock_guard lock(m_lock);
				slot = m_slots.get(SlotHandle::from_bits(bits));

				if (slot != nullptr && slot->dead.load(std::memory_order_acquire))
					slot = nullptr;
			}

			// One still waiting to publish is asked again at its publish; this is for the published.
			if (slot == nullptr)
				continue;

			const Type& type = m_types[slot->type];
			void* live		 = slot->payload.load(std::memory_order_relaxed);

			if (live != nullptr && type.refresh != nullptr)
			{
				AssetServices services = this->services(type);
				type.refresh(services, live);
			}
		}

		for (const Retired& retired : m_due)
			release(retired);

		m_due.clear();

		// Debounce: the watcher reports every step of an editor's save; the asset reloads once the
		// file has been quiet for reload_delay. A settled change, a file written whole by this process
		// or delivered to it, is due at once, and the watcher's report of that same write, which comes
		// within the delay, is nothing new.
		const u64 now = now_ns();

		std::erase_if(m_settled, [&](const Settled& settled) { return now - settled.time_ns >= m_reload_delay_ns; });

		Change change;
		while (m_changes.try_pop(change))
		{
			const auto it = std::find_if(m_pending.begin(), m_pending.end(),
										 [&](const Change& pending) { return pending.id == change.id; });

			if (it != m_pending.end())
			{
				it->time_ns = change.time_ns;
				it->settled |= change.settled;
				continue;
			}

			const bool echo =
				!change.settled && std::any_of(m_settled.begin(), m_settled.end(),
											   [&](const Settled& settled) { return settled.id == change.id; });
			if (!echo)
				m_pending.push_back(change);
		}

		for (u32 i = 0; i < m_pending.size();)
		{
			if (!m_pending[i].settled && now - m_pending[i].time_ns < m_reload_delay_ns)
			{
				++i;
				continue;
			}

			const Change& change = m_pending[i];
			reload(change.id);

			if (change.settled)
				m_settled.push_back({.id = change.id, .time_ns = now});

			// Kept for take_changed(), name and all: a change reported by id alone has nothing to send on.
			if (m_record_changes && change.size != 0)
			{
				const StringView name(change.name, change.size);
				m_changed.push_back({.name = String(name, m_heap), .id = change.id, .mounted = is_mounted(name)});
			}

			m_pending[i] = m_pending.back();
			m_pending.pop_back();
		}
	}

	void AssetManager::wait_idle() noexcept
	{
		jobs::wait(m_loaders);

		// A boot or level load on the owner thread holds up the pump that would publish what the
		// loaders finished for types that publish there, so it publishes them itself. Reloads keep
		// waiting for the pump: frames may be reading the payloads they fold into.
		if (jobs::is_main())
			apply_fresh(false);
	}

	AssetManager::Stats AssetManager::stats() const noexcept
	{
		std::lock_guard lock(m_lock);

		Stats stats{.assets = m_slots.size(), .loaders = m_running.load(std::memory_order_relaxed)};

		for (const Slot& slot : m_slots)
		{
			switch (slot.state.load(std::memory_order_relaxed))
			{
				case AssetState::Loading:
					++stats.loading;
					break;
				case AssetState::Failed:
					++stats.failed;
					break;
				case AssetState::Loaded:
					break;
			}
		}

		return stats;
	}

	void AssetManager::queue_load(Slot& slot, SlotHandle handle) noexcept
	{
		// One load per slot at a time. A second request while one is queued or running only marks
		// the slot dirty, and the loader goes round again; that is what keeps reloads in order.
		if (slot.queued.exchange(true, std::memory_order_acq_rel))
			return;

		// A slot is queued at most once and the queue holds one cell per slot, so a refused push
		// is only ever a loader mid pop; it always finishes.
		for (u32 spins = 0; !m_requests.try_push({handle.to_bits()}); ++spins)
			detail::cpu_relax(spins);

		wake_loader();
	}

	void AssetManager::wake_loader() noexcept
	{
		u32 running = m_running.load(std::memory_order_relaxed);

		while (running < m_loader_count)
		{
			if (!m_running.compare_exchange_weak(running, running + 1, std::memory_order_acq_rel))
				continue;

			// Low priority: a load that runs late costs a frame of fallback art, never a stall.
			jobs::kick({.fn		  = [](void* data) noexcept { static_cast<AssetManager*>(data)->run_loader(); },
						.data	  = this,
						.name	  = "asset loader",
						.priority = jobs::JobPriority::Low},
					   &m_loaders);
			return;
		}
	}

	void AssetManager::run_loader() noexcept
	{
		Request request;

		while (m_requests.try_pop(request))
		{
			const SlotHandle handle = SlotHandle::from_bits(request.slot);

			// A queued slot is never freed underneath its loader, so the lookup cannot miss.
			load_one(*m_slots.get(handle), handle);
		}

		m_running.fetch_sub(1, std::memory_order_acq_rel);

		// A request pushed after the last pop would otherwise wait for the next load() to come along.
		if (m_requests.size_hint() != 0)
			wake_loader();
	}

	void AssetManager::load_one(Slot& slot, SlotHandle handle) noexcept
	{
		const Type& type = m_types[slot.type];

		do
		{
			slot.dirty.store(false, std::memory_order_relaxed);

			void* payload = nullptr;

			// A slot nobody wants any more still gets its publish, so a waiter wakes and the slot
			// can be freed below; only the file work is skipped.
			if (!slot.dead.load(std::memory_order_acquire))
			{
				fs::FileData bytes;

				if (type.reads_own_files || read_file(slot, bytes))
				{
					payload = m_heap->allocate(type.size, type.align);

					// The load owns the bytes from here: they die with it unless the loader takes them.
					AssetLoad load(*this, handle.to_bits(), slot.path, slot.file, std::move(bytes),
								   slot.payload.load(std::memory_order_acquire), type.context, *m_gpu, *m_heap);

					const bool loaded = type.load(load, payload);

					if (!loaded)
					{
						EMBER_ERROR("asset '{}': {} load failed", slot.path, type.name);
						m_heap->deallocate(payload, type.size, type.align);
						payload = nullptr;
					}

					// A load that worked names everything it was made from. One that failed may have
					// stopped before it found them all, so it adds to what the last good one named
					// instead: fixing the file it broke on must still bring the next attempt.
					depend(handle, {load.m_dependencies.data(), load.m_dependencies.size()}, loaded);
				}
			}

			publish(slot, handle, payload);
		} while (slot.dirty.exchange(false, std::memory_order_acq_rel));

		// The hand-off with the pump is under the lock: either the pump found the load still queued
		// and left the slot to it, or the loader lets go first and the pump frees the slot itself.
		bool again = false;

		{
			std::lock_guard lock(m_lock);

			if (slot.dead.load(std::memory_order_acquire))
			{
				free_slot(slot, handle);
				return;
			}

			slot.queued.store(false, std::memory_order_release);
			again = slot.dirty.load(std::memory_order_acquire); // a change between the last check and here
		}

		if (again)
			queue_load(slot, handle);
	}

	bool AssetManager::read_file(const Slot& slot, fs::FileData& out) noexcept
	{
		auto read = read_source(*slot.source, slot.local);

		if (!read)
		{
			const fs::FileError& error = read.error();
			EMBER_ERROR("asset '{}': read failed ({} in {}, native {})", slot.path, enum_name(error.code),
						enum_name(error.op), error.native_code);
			return false;
		}

		out = std::move(read.value());
		return true;
	}

	Result<fs::FileData, fs::FileError> AssetManager::read_source(AssetSource& source, StringView local) noexcept
	{
		// Submitted to the IO thread and waited for on this fiber. The worker underneath runs other
		// jobs meanwhile, and this code resumes wherever one is free.
		struct Read
		{
			AssetSource* source;
			StringView local;
			Heap* heap;
			Result<fs::FileData, fs::FileError> result;
		};

		Read read{&source, local, m_heap, {}};
		jobs::Counter done;

		const auto submitted = jobs::submit_io(
			{
				.fn =
					[](void* data) noexcept
				{
					Read& read	= *static_cast<Read*>(data);
					read.result = read.source->read(read.local, *read.heap);
				},
				.data	  = &read,
				.name	  = "read asset",
				.priority = jobs::JobPriority::Low,
			},
			done);

		if (!submitted)
		{
			EMBER_ERROR("asset read refused ({})", enum_name(submitted.error()));
			return tl::unexpected(fs::FileError{.code = fs::FileErrorCode::Busy, .op = fs::FileOp::Read});
		}

		jobs::wait(done);
		return std::move(read.result);
	}

	void AssetManager::publish(Slot& slot, SlotHandle handle, void* payload) noexcept
	{
		const Type& type = m_types[slot.type];
		std::lock_guard lock(m_lock);

		// A failure is news only while no load has made a payload: it was the first load, and it
		// failed. One made earlier may still be waiting for the pump.
		if (payload == nullptr)
		{
			if (!slot.produced && slot.state.load(std::memory_order_relaxed) == AssetState::Loading)
			{
				slot.state.store(AssetState::Failed, std::memory_order_release);
				jobs::signal(slot.first_load);
			}

			return;
		}

		slot.produced = true;

		// A first payload that nothing else has to finish goes out at once: nobody can have read a
		// payload that was not there. After a failed first load, that is a reload too.
		if (type.publish == nullptr && slot.payload.load(std::memory_order_relaxed) == nullptr)
		{
			slot.payload.store(payload, std::memory_order_release);

			if (slot.state.exchange(AssetState::Loaded, std::memory_order_acq_rel) == AssetState::Loading)
				jobs::signal(slot.first_load);

			notify_dependents(slot);
			return;
		}

		// Everything else reaches the world at the pump: a reload folds in between frames, and the
		// first payload of a type that publishes on the owner thread is published there. One
		// nobody has seen yet is simply replaced by a fresher one.
		if (slot.fresh != nullptr)
			retire(std::exchange(slot.fresh, nullptr), slot.type);
		else
			m_fresh.push_back(handle.to_bits());

		slot.fresh = payload;
	}

	void AssetManager::apply_fresh(bool between_frames) noexcept
	{
		struct Apply
		{
			Slot* slot;
			void* fresh;
		};

		for (;;)
		{
			Apply applies[64]; // per pass; the rest wait for the next one, or the next pump
			u32 count = 0;

			{
				std::lock_guard lock(m_lock);

				u32 kept = 0;
				for (const u32 bits : m_fresh)
				{
					Slot* slot = m_slots.get(SlotHandle::from_bits(bits));

					// Gone, or dead with its loader about to free it, payload and all.
					if (slot == nullptr || slot->fresh == nullptr || slot->dead.load(std::memory_order_acquire))
						continue;

					// A reload folds between frames only: a frame may be reading the live payload. The
					// slot cannot go away meanwhile, since only this thread frees the living.
					const bool reload = slot->payload.load(std::memory_order_relaxed) != nullptr;

					if (count == std::size(applies) || (reload && !between_frames))
					{
						m_fresh[kept++] = bits;
						continue;
					}

					applies[count++] = {slot, std::exchange(slot->fresh, nullptr)};
				}

				m_fresh.resize(kept);
			}

			if (count == 0)
				return;

			u32 published = 0;

			for (u32 i = 0; i < count; ++i)
			{
				Slot& slot			   = *applies[i].slot;
				void* fresh			   = applies[i].fresh;
				const Type& type	   = m_types[slot.type];
				AssetServices services = this->services(type);

				// The fold: the live payload changes under nobody. What the type leaves in `fresh` is
				// spent and goes the way of every retired payload.
				if (void* live = slot.payload.load(std::memory_order_relaxed))
				{
					type.reload(services, live, fresh);

					std::lock_guard lock(m_lock);
					retire(fresh, slot.type);
					notify_dependents(slot);
					continue;
				}

				// A first payload, finished here because only this thread may finish it. Not yet means
				// it waits for an asset it loaded; a newer payload staged meanwhile supersedes it.
				if (type.publish != nullptr && !type.publish(services, fresh))
				{
					std::lock_guard lock(m_lock);

					if (slot.fresh == nullptr)
					{
						slot.fresh = fresh;
						m_fresh.push_back(slot.handle);
					}
					else
					{
						retire(fresh, slot.type);
					}

					continue;
				}

				slot.payload.store(fresh, std::memory_order_release);

				if (slot.state.exchange(AssetState::Loaded, std::memory_order_acq_rel) == AssetState::Loading)
					jobs::signal(slot.first_load);

				std::lock_guard lock(m_lock);
				notify_dependents(slot);
				++published;
			}

			// One published here may be what another was waiting for: go round until nothing moves.
			if (published == 0)
				return;
		}
	}

	void AssetManager::notify_dependents(Slot& slot) noexcept
	{
		// Dependents that went away drop out here, rather than being chased down when they go.
		u32 kept = 0;

		for (const u32 bits : slot.dependents)
		{
			if (!m_slots.contains(SlotHandle::from_bits(bits)))
				continue;

			slot.dependents[kept++] = bits;
			m_refresh.push_back(bits);
		}

		slot.dependents.resize(kept);
	}

	void AssetManager::depend(SlotHandle handle, Span<const AssetId> files, bool replace) noexcept
	{
		const u32 bits = handle.to_bits();
		std::lock_guard lock(m_lock);

		if (replace)
			std::erase_if(m_file_dependencies,
						  [bits](const FileDependency& dependency) { return dependency.slot == bits; });

		for (const AssetId file : files)
		{
			const auto same = [&](const FileDependency& dependency)
			{ return dependency.file == file && dependency.slot == bits; };

			if (std::none_of(m_file_dependencies.begin(), m_file_dependencies.end(), same))
				m_file_dependencies.push_back({.file = file, .slot = bits});
		}
	}

	void AssetManager::retire(void* payload, u16 type) noexcept
	{
		m_retired.push_back({.payload = payload, .type = type, .frame = m_frame.load(std::memory_order_relaxed)});
	}

	void AssetManager::release(const Retired& retired) noexcept
	{
		const Type& type	   = m_types[retired.type];
		AssetServices services = this->services(type);

		type.unload(services, retired.payload);
		m_heap->deallocate(retired.payload, type.size, type.align);
	}
}
