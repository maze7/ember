#include <ember/assets/asset.h>
#include <ember/assets/texture_asset.h>
#include <ember/core/bits.h>
#include <ember/core/logger.h>
#include <ember/gpu/device.h>

#include <assets/file_watcher.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <mutex>

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
}

namespace ember
{
	AssetManager::AssetManager() noexcept
		: m_root(&memory::heap(MemoryTag::Assets)), m_types(&memory::heap(MemoryTag::Assets)),
		  m_slots(MemoryTag::Assets), m_by_id(&memory::heap(MemoryTag::Assets)),
		  m_fresh(&memory::heap(MemoryTag::Assets)), m_retired(&memory::heap(MemoryTag::Assets)),
		  m_due(&memory::heap(MemoryTag::Assets)), m_requests(MemoryTag::Assets), m_unreferenced(MemoryTag::Assets),
		  m_changes(MemoryTag::Assets), m_pending(&memory::heap(MemoryTag::Assets))
	{
	}

	AssetManager::~AssetManager() noexcept { shutdown(); }

	void AssetManager::init(io::FileIo& io, gpu::Device& gpu, const AssetManagerDef& def) noexcept
	{
		EMBER_ASSERT(m_io == nullptr && "init runs once");
		EMBER_ASSERT(def.max_assets != 0 && def.max_assets <= SlotPool::MAX_CAPACITY);
		EMBER_ASSERT(def.max_changes >= 2);

		m_io			  = &io;
		m_gpu			  = &gpu;
		m_heap			  = &memory::heap(MemoryTag::Assets);
		m_root			  = def.root;
		m_relative		  = m_root.empty() ? 0 : static_cast<u32>(m_root.size() + 1);
		m_reload_delay_ns = u64{def.reload_delay_ms} * 1'000'000;
		m_loader_count	  = def.loaders != 0 ? def.loaders : std::max(1u, jobs::worker_count() / 2);

		// A slot is queued once at a time and unreferenced once at a time, so a cell per slot means
		// neither queue can ever be full.
		m_slots.init(def.max_assets);
		m_requests.init(std::bit_ceil(def.max_assets));
		m_unreferenced.init(std::bit_ceil(def.max_assets));
		m_changes.init(std::bit_ceil(def.max_changes));
		m_types.reserve(32);

		register_type<TextureAsset>("texture");

		// Last, so the first event it can deliver finds a manager ready to take it.
		if (def.hot_reload)
		{
			m_watcher = memory::new_object<detail::FileWatcher>(MemoryTag::Assets, *this, StringView(m_root));

			if (!m_watcher->start())
			{
				memory::delete_object(MemoryTag::Assets, m_watcher);
				m_watcher = nullptr;
			}
		}
	}

	void AssetManager::shutdown() noexcept
	{
		if (m_io == nullptr)
			return;

		// First, so no event lands on a registry that is being emptied.
		memory::delete_object(MemoryTag::Assets, m_watcher);
		m_watcher = nullptr;

		wait_idle();

		{
			std::lock_guard lock(m_lock);

			for (auto it = m_slots.begin(); it != m_slots.end();)
			{
				const SlotHandle handle = it.handle();
				++it;

				Slot& slot = *m_slots.get(handle);
				EMBER_ASSERT(!slot.queued.load(std::memory_order_relaxed) && "a load survived wait_idle");
				EMBER_ASSERT(slot.refs.load(std::memory_order_acquire) == 0 &&
							 "an AssetRef outlives the asset manager");

				m_by_id.erase(slot.id);
				free_slot(slot, handle);
			}

			m_due = std::move(m_retired);
			m_retired.clear();
		}

		// Everything is retired now and nothing may read it: no grace.
		for (const Retired& retired : m_due)
			release(retired);

		m_due.clear();
		m_fresh.clear();
		m_pending.clear();
		m_types.clear();
		m_io   = nullptr;
		m_gpu  = nullptr;
		m_heap = nullptr;
	}

	u16 AssetManager::add_type(const Type& type) noexcept
	{
		EMBER_ASSERT(m_types.size() < NO_TYPE);
		EMBER_ASSERT(m_slots.empty() && "register every type before the first load");

		m_types.push_back(type);
		return static_cast<u16>(m_types.size() - 1);
	}

	AssetManager::Slot* AssetManager::request(u16 type, StringView path) noexcept
	{
		EMBER_ASSERT(type != NO_TYPE && "register the type before loading it");
		EMBER_ASSERT(m_io != nullptr && "load before init");

		const AssetId id = asset_id(path);

		std::lock_guard lock(m_lock);

		// The reference is counted here, under the lock, so a pump that finds the slot queued as
		// unreferenced sees the count and leaves it alone.
		if (const auto it = m_by_id.find(id); it != m_by_id.end())
		{
			Slot* slot = m_slots.get(SlotHandle::from_bits(it->second));
			EMBER_ASSERT(slot->type == type && "one asset, one type");

			slot->refs.fetch_add(1, std::memory_order_relaxed);
			return slot;
		}

		String full(m_heap);
		full.reserve(m_root.size() + 1 + path.size());
		full += m_root;
		if (!m_root.empty())
			full += '/';
		full += path;

		const SlotHandle handle = m_slots.emplace(type, id, std::move(full));

		if (handle.is_null())
		{
			EMBER_ERROR("asset '{}': registry full ({} assets)", path, m_slots.capacity());
			return nullptr;
		}

		Slot* slot	  = m_slots.get(handle);
		slot->manager = this;
		slot->handle  = handle.to_bits();
		slot->refs.store(1, std::memory_order_relaxed);

		m_by_id.emplace(id, handle.to_bits());
		queue_load(*slot, handle);
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

	void AssetManager::free_slot(Slot& slot, SlotHandle handle) noexcept
	{
		// Both payloads outlive the slot by the grace: the live one for a pointer a frame took, the
		// fresh one because it is simplest to send everything the same way.
		if (void* payload = slot.payload.exchange(nullptr, std::memory_order_acq_rel))
			retire(payload, slot.type);

		if (slot.fresh != nullptr)
			retire(std::exchange(slot.fresh, nullptr), slot.type);

		(void)m_slots.erase(handle);
	}

	void AssetManager::notify_changed(AssetId id) noexcept
	{
		const Change change{.id = id, .time_ns = now_ns()};

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

	void AssetManager::reload(AssetId id) noexcept
	{
		Slot* slot = nullptr;
		SlotHandle handle;

		{
			std::lock_guard lock(m_lock);

			const auto it = m_by_id.find(id);
			if (it == m_by_id.end())
				return;

			handle = SlotHandle::from_bits(it->second);
			slot   = m_slots.get(handle);
		}

		// Dirty before queued: a loader that is running sees the flag at the end of its pass and
		// goes round again, and one that is not gets queued here.
		slot->dirty.store(true, std::memory_order_release);
		queue_load(*slot, handle);
	}

	void AssetManager::pump(u64 frame_index) noexcept
	{
		m_frame.store(frame_index, std::memory_order_release);

		struct Apply
		{
			Slot* slot;
			void* fresh;
		};

		Apply applies[64]; // per pump; the rest wait a frame
		u32 apply_count = 0;

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

			// Finished reloads, taken out to be applied without the lock: the slot cannot go away
			// meanwhile, since only this thread frees slots.
			u32 kept = 0;
			for (const u32 fresh_bits : m_fresh)
			{
				Slot* slot = m_slots.get(SlotHandle::from_bits(fresh_bits));

				if (slot == nullptr || slot->fresh == nullptr)
					continue;

				if (apply_count == std::size(applies))
				{
					m_fresh[kept++] = fresh_bits;
					continue;
				}

				applies[apply_count++] = {slot, std::exchange(slot->fresh, nullptr)};
			}
			m_fresh.resize(kept);

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

		// The fold: no stage is running, so the live payload can change under nobody. What the
		// type leaves in `fresh` is spent and goes the way of every retired payload.
		AssetServices services{*m_gpu, *m_heap};

		for (u32 i = 0; i < apply_count; ++i)
		{
			const Type& type = m_types[applies[i].slot->type];
			type.reload(services, applies[i].slot->payload.load(std::memory_order_relaxed), applies[i].fresh);

			std::lock_guard lock(m_lock);
			retire(applies[i].fresh, applies[i].slot->type);
		}

		for (const Retired& retired : m_due)
			release(retired);

		m_due.clear();

		// Debounce: the watcher reports every step of an editor's save; the asset reloads once the
		// file has been quiet for reload_delay.
		Change change;
		while (m_changes.try_pop(change))
		{
			const auto it = std::find_if(m_pending.begin(), m_pending.end(),
										 [&](const Change& pending) { return pending.id == change.id; });

			if (it != m_pending.end())
				it->time_ns = change.time_ns;
			else
				m_pending.push_back(change);
		}

		const u64 now = now_ns();

		for (u32 i = 0; i < m_pending.size();)
		{
			if (now - m_pending[i].time_ns < m_reload_delay_ns)
			{
				++i;
				continue;
			}

			reload(m_pending[i].id);
			m_pending[i] = m_pending.back();
			m_pending.pop_back();
		}
	}

	void AssetManager::wait_idle() noexcept { jobs::wait(m_loaders); }

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
		const Type& type		  = m_types[slot.type];
		const StringView relative = StringView(slot.path).substr(m_relative);

		do
		{
			slot.dirty.store(false, std::memory_order_relaxed);

			void* payload = nullptr;

			// A slot nobody wants any more still gets its publish, so a waiter wakes and the slot
			// can be freed below; only the file work is skipped.
			if (!slot.dead.load(std::memory_order_acquire))
			{
				// The read: submitted to the IO thread, waited for on this fiber. The worker
				// underneath runs other jobs meanwhile and this code resumes wherever one is free.
				io::FileRead read{.path = slot.path.c_str(), .memory = m_heap};
				jobs::Counter done{1};

				m_io->read(read, done);
				jobs::wait(done);

				if (read.error != io::FileError::None)
				{
					EMBER_ERROR("asset '{}': read failed ({})", relative,
								enum_names<io::FileError>()[static_cast<u32>(read.error)]);
				}
				else
				{
					payload = m_heap->allocate(type.size, type.align);

					AssetLoad load(relative, read.bytes, slot.payload.load(std::memory_order_acquire), *m_gpu, *m_heap);

					if (!type.load(load, payload))
					{
						EMBER_ERROR("asset '{}': {} load failed", relative, type.name);
						m_heap->deallocate(payload, type.size, type.align);
						payload = nullptr;
					}

					// The file's bytes die with the decode unless the loader took them.
					if (!load.taken())
						read.release();
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

	void AssetManager::publish(Slot& slot, SlotHandle handle, void* payload) noexcept
	{
		const bool first = slot.state.load(std::memory_order_relaxed) == AssetState::Loading;

		if (payload != nullptr)
		{
			if (slot.payload.load(std::memory_order_relaxed) == nullptr)
			{
				// Nobody can have read a payload that was not there: the pointer goes out at once.
				slot.payload.store(payload, std::memory_order_release);
				slot.state.store(AssetState::Loaded, std::memory_order_release);
			}
			else
			{
				// Frames are reading the live one: the pump folds this in between them. A fresh
				// payload nobody saw yet is simply replaced by a fresher one.
				std::lock_guard lock(m_lock);

				if (slot.fresh != nullptr)
					retire(std::exchange(slot.fresh, nullptr), slot.type);

				slot.fresh = payload;
				m_fresh.push_back(handle.to_bits());
			}
		}
		else if (first)
		{
			slot.state.store(AssetState::Failed, std::memory_order_release);
		}

		// Only the first outcome wakes waiters, loaded or failed, exactly once.
		if (first)
			jobs::signal(slot.first_load);
	}

	void AssetManager::retire(void* payload, u16 type) noexcept
	{
		m_retired.push_back({.payload = payload, .type = type, .frame = m_frame.load(std::memory_order_relaxed)});
	}

	void AssetManager::release(const Retired& retired) noexcept
	{
		const Type& type = m_types[retired.type];
		AssetServices services{*m_gpu, *m_heap};

		type.unload(services, retired.payload);
		m_heap->deallocate(retired.payload, type.size, type.align);
	}
}
