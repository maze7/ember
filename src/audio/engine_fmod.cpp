#include <ember/audio/engine.h>

#include <ember/core/logger.h>
#include <ember/memory/memory.h>

#include <fmod_errors.h>
#include <fmod_studio.hpp>

// From 2.04 FMOD says how far an event carries with its spatialisers' overrides counted, which earshot is judged by.
static_assert(FMOD_VERSION >= 0x00020400, "Ember::Audio needs FMOD 2.04 or later");

namespace ember::audio
{
	namespace
	{
		/** The parameters of one event the engine keeps ids for: more than that are set by none. */
		constexpr u32 MAX_PARAMS = 8;

		/**
		 * FMOD pans a sound wholly to one side once it is 30 degrees off the way the listener faces. A
		 * listener looking down from this many times a distance sees that distance to its side at 30.
		 */
		constexpr f32 FULL_PAN_HEIGHT = 1.7320508f;

		// FMOD's memory is the engine's: counted under the Audio tag, on the heap every thread shares.
		// FMOD allocates from its own threads, which the heap takes as they come.
		void* F_CALL fmod_alloc(unsigned int size, FMOD_MEMORY_TYPE, const char*)
		{
			return memory::heap(MemoryTag::Audio).allocate(size, 16);
		}

		void* F_CALL fmod_realloc(void* ptr, unsigned int size, FMOD_MEMORY_TYPE, const char*)
		{
			return memory::heap(MemoryTag::Audio).reallocate(ptr, size);
		}

		void F_CALL fmod_free(void* ptr, FMOD_MEMORY_TYPE, const char*)
		{
			memory::heap(MemoryTag::Audio).deallocate_unsized(ptr);
		}

		// The logging libraries only, which a Debug build links: what FMOD warns of goes to the engine's log.
		FMOD_RESULT F_CALL fmod_log(FMOD_DEBUG_FLAGS flags, const char*, int, const char* function, const char* message)
		{
			StringView text(message);
			while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
				text.remove_suffix(1);

			if ((flags & FMOD_DEBUG_LEVEL_ERROR) != 0)
				EMBER_ERROR("fmod: {}: {}", function, text);
			else
				EMBER_WARN("fmod: {}: {}", function, text);
			return FMOD_OK;
		}
	}

	struct Engine::Impl
	{
		struct ParamInfo
		{
			u64 name					= 0;
			FMOD_STUDIO_PARAMETER_ID id = {};
		};

		/** An event as its bank describes it, read once when the bank arrives. */
		struct EventInfo
		{
			FMOD::Studio::EventDescription* description = nullptr;
			FMOD::Studio::Bank* bank					= nullptr;
			f32 range									= 0.0f; // Studio units past which it is silent; 0 unknown
			bool spatial								= false;
			u8 param_count								= 0;
			ParamInfo params[MAX_PARAMS]				= {};
			u64 frame									= 0; // the last frame it started in, and how many times
			u32 starts									= 0;
		};

		struct BankInfo
		{
			String file;
			FMOD::Studio::Bank* bank = nullptr;
			bool loading			 = true;
			bool preload			 = true;
		};

		struct Held
		{
			FMOD::Studio::EventInstance* instance = nullptr;
			u64 event							  = 0;
			u16 generation						  = 1;
		};

		/** One event at a time: the music, the ambience. */
		struct Track
		{
			Event asked;
			Event playing;
			FMOD::Studio::EventInstance* instance = nullptr;
		};

		Impl() noexcept {}

		EngineDef def;
		FMOD::Studio::System* studio = nullptr;

		HashMap<u64, EventInfo> events{&memory::heap(MemoryTag::Audio)};
		HashMap<u64, FMOD_STUDIO_PARAMETER_ID> globals{&memory::heap(MemoryTag::Audio)};
		HashMap<u64, u32> unknown{&memory::heap(MemoryTag::Audio)}; // asked for and missing, each reported once
		Vector<BankInfo> banks{&memory::heap(MemoryTag::Audio)};
		Vector<Held> held{&memory::heap(MemoryTag::Audio)};
		Vector<u32> free_held{&memory::heap(MemoryTag::Audio)};

		Track music;
		Track ambience;
		Listener listener;
		u64 frame	   = 1;
		u32 held_count = 0;
		u32 loading	   = 0; // banks on their way
		u32 unloads	   = 0; // banks let go
		Stats stats;

		// The world's x runs east and its y south; FMOD's ground is x east and z north, with y up.
		[[nodiscard]] FMOD_VECTOR ground(glm::vec2 position) const noexcept
		{
			return {position.x / def.unit, 0.0f, -position.y / def.unit};
		}

		[[nodiscard]] FMOD_3D_ATTRIBUTES place(glm::vec2 position) const noexcept
		{
			FMOD_3D_ATTRIBUTES attributes = {};
			attributes.position			  = ground(position);
			attributes.forward			  = {0.0f, 0.0f, 1.0f};
			attributes.up				  = {0.0f, 1.0f, 0.0f};
			return attributes;
		}

		/** The event, or null: said once, by name, when every bank is in and none has it. */
		[[nodiscard]] EventInfo* find(Event event) noexcept
		{
			if (const auto found = events.find(event.id); found != events.end())
				return &found->second;

			// While a bank is still on its way, or before any was asked for, the event may yet come.
			if (!event || banks.empty() || loading != 0 || !unknown.emplace(event.id, 0u).second)
				return nullptr;

			++stats.unknown;
			if (event.path != nullptr)
				EMBER_WARN("audio: no loaded bank has {}", event.path);
			else
				EMBER_WARN("audio: no loaded bank has the event {:#018x}", event.id);
			return nullptr;
		}

		// By id: a parameter the event does not have is skipped, so an emitter may set what only some of
		// its sounds read.
		static void apply(FMOD::Studio::EventInstance& instance, const EventInfo& info,
						  Span<const ParamValue> params) noexcept
		{
			for (const ParamValue& value : params)
				for (u32 i = 0; i < info.param_count; ++i)
					if (info.params[i].name == value.param.id)
						(void)instance.setParameterByID(info.params[i].id, value.value);
		}

		/** Whether a one-shot may start: within earshot, and within the frame's budget. */
		[[nodiscard]] bool admit(EventInfo& info, const glm::vec2* position) noexcept
		{
			if (position != nullptr && info.spatial && info.range > 0.0f)
			{
				const glm::vec2 away = (*position - listener.focus) / def.unit;
				const f32 reach		 = info.range * (1.0f + def.cull_margin);
				if (away.x * away.x + away.y * away.y > reach * reach)
				{
					++stats.culled;
					return false;
				}
			}

			if (info.frame != frame)
			{
				info.frame	= frame;
				info.starts = 0;
			}

			if (stats.started >= def.max_starts || info.starts >= def.max_same)
			{
				++stats.dropped;
				return false;
			}

			++info.starts;
			++stats.started;
			return true;
		}

		void shoot(Event event, const glm::vec2* position, Span<const ParamValue> params) noexcept
		{
			EventInfo* info = find(event);
			if (info == nullptr || !admit(*info, position))
				return;

			FMOD::Studio::EventInstance* instance = nullptr;
			if (info->description->createInstance(&instance) != FMOD_OK)
				return;

			// A sound with no place is heard where sounds fade from: at full level, in the middle.
			const FMOD_3D_ATTRIBUTES attributes = place(position != nullptr ? *position : listener.focus);
			(void)instance->set3DAttributes(&attributes);
			apply(*instance, *info, params);
			(void)instance->start();
			(void)instance->release(); // FMOD frees it when it has played out
		}

		[[nodiscard]] Held* get(Voice voice) noexcept
		{
			const u32 index = (voice.bits & 0xFFFFu) - 1u;
			if (!voice || index >= held.size())
				return nullptr;

			Held& slot = held[index];
			return slot.instance != nullptr && slot.generation == (voice.bits >> 16) ? &slot : nullptr;
		}

		void release(Held& slot) noexcept
		{
			(void)slot.instance->release();
			slot.instance = nullptr;
			slot.event	  = 0;
			++slot.generation;
			free_held.push_back(static_cast<u32>(&slot - held.data()));
			--held_count;
		}

		/** Banks that have arrived are taken in, and those that failed are let go, with why. */
		void settle() noexcept
		{
			bool arrived = false;

			for (size_t i = 0; i < banks.size();)
			{
				BankInfo& info = banks[i];
				if (!info.loading)
				{
					++i;
					continue;
				}

				FMOD_STUDIO_LOADING_STATE state = FMOD_STUDIO_LOADING_STATE_ERROR;
				const FMOD_RESULT result		= info.bank->getLoadingState(&state);

				if (result == FMOD_OK && state == FMOD_STUDIO_LOADING_STATE_LOADING)
				{
					++i;
					continue;
				}

				--loading;

				if (result == FMOD_OK && state == FMOD_STUDIO_LOADING_STATE_LOADED)
				{
					info.loading = false;
					arrived		 = true;

					// Samples load behind the events, so the first play of each need not wait for the disk.
					if (info.preload)
						(void)info.bank->loadSampleData();

					++i;
					continue;
				}

				EMBER_ERROR("audio: cannot load {}: {}", StringView(info.file), FMOD_ErrorString(result));
				(void)info.bank->unload();
				banks.erase(banks.begin() + static_cast<std::ptrdiff_t>(i));
			}

			if (arrived)
				index();
		}

		/** Reads every loaded bank's events again: after a bank comes or goes. */
		void index() noexcept
		{
			events.clear();
			globals.clear();
			unknown.clear();

			Vector<FMOD::Studio::EventDescription*> list{&memory::heap(MemoryTag::Audio)};
			u32 pathless = 0;

			for (const BankInfo& loaded : banks)
			{
				int count = 0;
				if (loaded.loading || loaded.bank->getEventCount(&count) != FMOD_OK || count == 0)
					continue;

				list.resize(static_cast<size_t>(count));
				if (loaded.bank->getEventList(list.data(), count, &count) != FMOD_OK)
					continue;

				for (int i = 0; i < count; ++i)
					pathless += add(*list[static_cast<size_t>(i)], *loaded.bank) ? 0u : 1u;
			}

			// Paths come from the strings bank. While banks are still arriving it may be among them, and
			// this runs again when it is in.
			if (pathless != 0 && loading == 0)
				EMBER_WARN("audio: {} events have no path, so nothing can ask for them: load the strings bank too",
						   pathless);

			int count = 0;
			if (studio->getParameterDescriptionCount(&count) == FMOD_OK && count > 0)
			{
				Vector<FMOD_STUDIO_PARAMETER_DESCRIPTION> described{static_cast<size_t>(count),
																	&memory::heap(MemoryTag::Audio)};
				if (studio->getParameterDescriptionList(described.data(), count, &count) == FMOD_OK)
					for (int i = 0; i < count; ++i)
						globals[hash_text(described[static_cast<size_t>(i)].name)] =
							described[static_cast<size_t>(i)].id;
			}
		}

		/**
		 * Stops what plays from a bank that is about to go, or from any when null: FMOD would take it
		 * with the bank anyway, and warn. A track's event is asked for still, and update() starts it
		 * again once a bank has it; a held voice is stale from here.
		 */
		void silence(const FMOD::Studio::Bank* bank) noexcept
		{
			const auto from = [&](u64 event)
			{
				const auto found = events.find(event);
				return bank == nullptr || (found != events.end() && found->second.bank == bank);
			};

			for (Track* track : {&music, &ambience})
			{
				if (track->instance == nullptr || !from(track->playing.id))
					continue;

				(void)track->instance->stop(FMOD_STUDIO_STOP_IMMEDIATE);
				(void)track->instance->release();
				track->instance = nullptr;
				track->playing	= {};
			}

			for (Held& slot : held)
			{
				if (slot.instance == nullptr || !from(slot.event))
					continue;

				(void)slot.instance->stop(FMOD_STUDIO_STOP_IMMEDIATE);
				release(slot);
			}
		}

		/** False when the event has no path yet. */
		bool add(FMOD::Studio::EventDescription& description, FMOD::Studio::Bank& bank) noexcept
		{
			char path[512];
			int length = 0;
			if (description.getPath(path, static_cast<int>(sizeof(path)), &length) != FMOD_OK)
				return false;

			EventInfo info;
			info.description = &description;
			info.bank		 = &bank;
			(void)description.is3D(&info.spatial);

			// How far it is heard is what its spatialisers override its range to, when they do. A bank from
			// a Studio before 2.04 does not say, and reads 0: its events are never judged out of earshot,
			// since the range the event itself gives may be far short of the truth.
			f32 nearest = 0.0f;
			if (info.spatial)
				(void)description.getMinMaxDistance(&nearest, &info.range, true);

			int params = 0;
			(void)description.getParameterDescriptionCount(&params);
			for (int i = 0; i < params && info.param_count < MAX_PARAMS; ++i)
			{
				FMOD_STUDIO_PARAMETER_DESCRIPTION described;
				if (description.getParameterDescriptionByIndex(i, &described) != FMOD_OK)
					continue;

				// The game sets the event's own; the rest are FMOD's (distance) or everyone's (global).
				if (described.type != FMOD_STUDIO_PARAMETER_GAME_CONTROLLED ||
					(described.flags & FMOD_STUDIO_PARAMETER_GLOBAL) != 0)
					continue;

				info.params[info.param_count++] = {.name = hash_text(described.name), .id = described.id};
			}

			events[hash_text(path)] = info;
			return true;
		}

		void play_track(Track& track) noexcept
		{
			if (track.playing == track.asked && (track.instance != nullptr || !track.asked))
				return;

			if (track.instance != nullptr)
			{
				(void)track.instance->stop(FMOD_STUDIO_STOP_ALLOWFADEOUT);
				(void)track.instance->release();
				track.instance = nullptr;
				track.playing  = {};
			}

			if (!track.asked)
				return;

			// Not there yet, its bank still to come: asked again next frame.
			const auto found = events.find(track.asked.id);
			if (found == events.end())
			{
				(void)find(track.asked); // says so once, when no bank is on its way
				return;
			}

			if (found->second.description->createInstance(&track.instance) != FMOD_OK)
				return;

			(void)track.instance->start();
			track.playing = track.asked;
		}
	};

	Engine::Engine() noexcept = default;
	Engine::~Engine() noexcept { shutdown(); }

	bool Engine::init(const EngineDef& def) noexcept
	{
		EMBER_ASSERT(m_impl == nullptr && "the audio engine is up already");

		// Before FMOD makes anything: its memory, and where its warnings go.
		(void)FMOD::Memory_Initialize(nullptr, 0, &fmod_alloc, &fmod_realloc, &fmod_free);
		(void)FMOD::Debug_Initialize(FMOD_DEBUG_LEVEL_WARNING, FMOD_DEBUG_MODE_CALLBACK, &fmod_log);

		FMOD::Studio::System* studio = nullptr;
		if (const FMOD_RESULT result = FMOD::Studio::System::create(&studio); result != FMOD_OK)
		{
			EMBER_ERROR("audio: FMOD did not start: {}", FMOD_ErrorString(result));
			return false;
		}

		FMOD::System* core = nullptr;
		(void)studio->getCoreSystem(&core);
		(void)core->setSoftwareChannels(static_cast<int>(def.voices));

		const bool stepped = def.output == Output::Stepped;
		if (stepped)
			(void)core->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT);
		else if (def.output == Output::None)
			(void)core->setOutput(FMOD_OUTPUTTYPE_NOSOUND);

		FMOD_STUDIO_ADVANCEDSETTINGS advanced = {};
		advanced.cbsize						  = sizeof(advanced);
		advanced.commandqueuesize			  = def.command_bytes;
		(void)studio->setAdvancedSettings(&advanced);

		FMOD_STUDIO_INITFLAGS flags = stepped ? FMOD_STUDIO_INIT_SYNCHRONOUS_UPDATE : FMOD_STUDIO_INIT_NORMAL;
		if (def.live_update && !stepped)
			flags |= FMOD_STUDIO_INIT_LIVEUPDATE;

		const int channels = static_cast<int>(def.virtual_voices);
		FMOD_RESULT result = studio->initialize(channels, flags, FMOD_INIT_NORMAL, nullptr);

		// Live Update listens on a port, which a second copy of the game on this machine cannot have too.
		if (result != FMOD_OK && (flags & FMOD_STUDIO_INIT_LIVEUPDATE) != 0)
		{
			EMBER_WARN("audio: no Live Update ({}): starting without it", FMOD_ErrorString(result));
			flags &= ~static_cast<FMOD_STUDIO_INITFLAGS>(FMOD_STUDIO_INIT_LIVEUPDATE);
			result = studio->initialize(channels, flags, FMOD_INIT_NORMAL, nullptr);
		}

		// A machine with nothing to play through still runs the game: everything but the sound.
		if (result != FMOD_OK && def.output == Output::Device)
		{
			EMBER_WARN("audio: no output device ({}): running without sound", FMOD_ErrorString(result));
			(void)core->setOutput(FMOD_OUTPUTTYPE_NOSOUND);
			result = studio->initialize(channels, flags, FMOD_INIT_NORMAL, nullptr);
		}

		if (result != FMOD_OK)
		{
			EMBER_ERROR("audio: FMOD did not start: {}", FMOD_ErrorString(result));
			(void)studio->release();
			return false;
		}

		m_impl		   = memory::make_unique<Impl>(MemoryTag::Audio);
		m_impl->def	   = def;
		m_impl->studio = studio;
		m_impl->held.resize(def.held);
		for (u32 i = def.held; i-- > 0;)
			m_impl->free_held.push_back(i);

		listen({});
		return true;
	}

	void Engine::shutdown() noexcept
	{
		if (m_impl == nullptr)
			return;

		// Everything goes with the system: what plays, the banks it plays from, and FMOD's threads.
		m_impl->silence(nullptr);
		(void)m_impl->studio->unloadAll();
		(void)m_impl->studio->release();
		m_impl.reset();
	}

	void Engine::update() noexcept
	{
		if (m_impl == nullptr)
			return;

		Impl& impl = *m_impl;
		impl.settle();
		impl.play_track(impl.music);
		impl.play_track(impl.ambience);

		(void)impl.studio->update();
		++impl.frame;

		FMOD_STUDIO_BUFFER_USAGE usage;
		if (impl.studio->getBufferUsage(&usage) == FMOD_OK)
		{
			impl.stats.queue_peak	= static_cast<u32>(usage.studiocommandqueue.peakusage);
			impl.stats.queue_stalls = static_cast<u32>(usage.studiocommandqueue.stallcount);
		}

		impl.stats.started = 0;
		impl.stats.culled  = 0;
		impl.stats.dropped = 0;
	}

	bool Engine::load_bank(StringView file, bool preload) noexcept
	{
		if (m_impl == nullptr)
			return false;

		Impl& impl = *m_impl;
		unload_bank(file);

		// On FMOD's own threads: a load that waits for them holds its caller two of their update
		// periods, 40 ms, whatever the bank's size. Stepped has no such threads, and loads here.
		const bool stepped = impl.def.output == Output::Stepped;
		const FMOD_STUDIO_LOAD_BANK_FLAGS mode =
			stepped ? FMOD_STUDIO_LOAD_BANK_NORMAL : FMOD_STUDIO_LOAD_BANK_NONBLOCKING;

		const String path(file, &memory::heap(MemoryTag::Audio));
		FMOD::Studio::Bank* bank = nullptr;
		if (const FMOD_RESULT result = impl.studio->loadBankFile(path.c_str(), mode, &bank); result != FMOD_OK)
		{
			EMBER_ERROR("audio: cannot load {}: {}", file, FMOD_ErrorString(result));
			return false;
		}

		impl.banks.push_back({.file = path, .bank = bank, .loading = true, .preload = preload});
		++impl.loading;
		impl.settle();

		if (stepped && preload)
			(void)impl.studio->flushSampleLoading();
		return true;
	}

	void Engine::unload_bank(StringView file) noexcept
	{
		if (m_impl == nullptr)
			return;

		Impl& impl = *m_impl;
		for (size_t i = 0; i < impl.banks.size(); ++i)
		{
			if (StringView(impl.banks[i].file) != file)
				continue;

			if (impl.banks[i].loading)
				--impl.loading;

			// What plays from it stops first: its events' instances go with it.
			impl.silence(impl.banks[i].bank);
			++impl.unloads;

			(void)impl.banks[i].bank->unload();
			impl.banks.erase(impl.banks.begin() + static_cast<std::ptrdiff_t>(i));
			impl.index();
			return;
		}
	}

	u32 Engine::unloads() const noexcept { return m_impl != nullptr ? m_impl->unloads : 0; }

	bool Engine::has(Event event) const noexcept { return m_impl != nullptr && m_impl->events.contains(event.id); }

	f32 Engine::range(Event event) const noexcept
	{
		if (m_impl == nullptr)
			return 0.0f;

		const auto found = m_impl->events.find(event.id);
		return found != m_impl->events.end() ? found->second.range * m_impl->def.unit : 0.0f;
	}

	void Engine::play(Event event, Span<const ParamValue> params) noexcept
	{
		if (m_impl != nullptr)
			m_impl->shoot(event, nullptr, params);
	}

	void Engine::play(Event event, glm::vec2 position, Span<const ParamValue> params) noexcept
	{
		if (m_impl != nullptr)
			m_impl->shoot(event, &position, params);
	}

	Voice Engine::start(Event event, glm::vec2 position, Span<const ParamValue> params) noexcept
	{
		if (m_impl == nullptr)
			return {};

		Impl& impl			  = *m_impl;
		Impl::EventInfo* info = impl.find(event);
		if (info == nullptr || impl.free_held.empty())
			return {};

		FMOD::Studio::EventInstance* instance = nullptr;
		if (info->description->createInstance(&instance) != FMOD_OK)
			return {};

		const FMOD_3D_ATTRIBUTES attributes = impl.place(position);
		(void)instance->set3DAttributes(&attributes);
		Impl::apply(*instance, *info, params);
		(void)instance->start();

		const u32 index = impl.free_held.back();
		impl.free_held.pop_back();

		Impl::Held& slot = impl.held[index];
		slot.instance	 = instance;
		slot.event		 = event.id;
		++impl.held_count;
		return {.bits = static_cast<u32>(slot.generation) << 16 | (index + 1u)};
	}

	void Engine::stop(Voice voice, bool fade) noexcept
	{
		if (m_impl == nullptr)
			return;

		if (Impl::Held* slot = m_impl->get(voice))
		{
			(void)slot->instance->stop(fade ? FMOD_STUDIO_STOP_ALLOWFADEOUT : FMOD_STUDIO_STOP_IMMEDIATE);
			m_impl->release(*slot);
		}
	}

	void Engine::move(Voice voice, glm::vec2 position) noexcept
	{
		if (m_impl == nullptr)
			return;

		if (Impl::Held* slot = m_impl->get(voice))
		{
			const FMOD_3D_ATTRIBUTES attributes = m_impl->place(position);
			(void)slot->instance->set3DAttributes(&attributes);
		}
	}

	void Engine::set(Voice voice, Span<const ParamValue> params) noexcept
	{
		if (m_impl == nullptr)
			return;

		if (Impl::Held* slot = m_impl->get(voice))
			if (const auto found = m_impl->events.find(slot->event); found != m_impl->events.end())
				Impl::apply(*slot->instance, found->second, params);
	}

	bool Engine::playing(Voice voice) noexcept
	{
		if (m_impl == nullptr)
			return false;

		Impl::Held* slot = m_impl->get(voice);
		if (slot == nullptr)
			return false;

		// One whose event played out, or whose bank went, is let go here: its voice is stale after.
		FMOD_STUDIO_PLAYBACK_STATE state = FMOD_STUDIO_PLAYBACK_STOPPED;
		if (slot->instance->getPlaybackState(&state) == FMOD_OK && state != FMOD_STUDIO_PLAYBACK_STOPPED)
			return true;

		m_impl->release(*slot);
		return false;
	}

	void Engine::music(Event event) noexcept
	{
		if (m_impl != nullptr)
			m_impl->music.asked = event;
	}

	void Engine::ambience(Event event) noexcept
	{
		if (m_impl != nullptr)
			m_impl->ambience.asked = event;
	}

	void Engine::set_music(Param param, f32 value) noexcept
	{
		if (m_impl == nullptr || m_impl->music.instance == nullptr)
			return;

		if (const auto found = m_impl->events.find(m_impl->music.playing.id); found != m_impl->events.end())
		{
			const ParamValue one{.param = param, .value = value};
			Impl::apply(*m_impl->music.instance, found->second, Span<const ParamValue>(&one, 1));
		}
	}

	void Engine::set(Param global, f32 value) noexcept
	{
		if (m_impl == nullptr)
			return;

		if (const auto found = m_impl->globals.find(global.id); found != m_impl->globals.end())
			(void)m_impl->studio->setParameterByID(found->second, value);
	}

	void Engine::set_volume(StringView path, f32 volume) noexcept
	{
		if (m_impl == nullptr)
			return;

		const String name(path, &memory::heap(MemoryTag::Audio));
		if (path.starts_with("vca:"))
		{
			FMOD::Studio::VCA* vca = nullptr;
			if (m_impl->studio->getVCA(name.c_str(), &vca) == FMOD_OK)
				(void)vca->setVolume(volume);
		}
		else
		{
			FMOD::Studio::Bus* bus = nullptr;
			if (m_impl->studio->getBus(name.c_str(), &bus) == FMOD_OK)
				(void)bus->setVolume(volume);
		}
	}

	void Engine::set_paused(StringView bus_path, bool paused) noexcept
	{
		if (m_impl == nullptr)
			return;

		const String name(bus_path, &memory::heap(MemoryTag::Audio));
		FMOD::Studio::Bus* bus = nullptr;
		if (m_impl->studio->getBus(name.c_str(), &bus) == FMOD_OK)
			(void)bus->setPaused(paused);
	}

	void Engine::listen(const Listener& listener) noexcept
	{
		if (m_impl == nullptr)
			return;

		m_impl->listener = listener;

		// Looking straight down from above the middle of the screen, north up. FMOD pans by the angle
		// off the way the listener faces and by nothing else, so a sound's place across the screen is
		// its place between the speakers, and how far north or south it is changes nothing. Level falls
		// with distance along the ground from the focus, not from up here.
		FMOD_3D_ATTRIBUTES attributes = {};
		attributes.position			  = m_impl->ground(listener.position);
		attributes.position.y		  = listener.reach / m_impl->def.unit * FULL_PAN_HEIGHT;
		attributes.forward			  = {0.0f, -1.0f, 0.0f};
		attributes.up				  = {0.0f, 0.0f, 1.0f};

		const FMOD_VECTOR focus = m_impl->ground(listener.focus);
		(void)m_impl->studio->setListenerAttributes(0, &attributes, &focus);
	}

	const EngineDef& Engine::def() const noexcept
	{
		static const EngineDef none;
		return m_impl != nullptr ? m_impl->def : none;
	}

	Stats Engine::stats() const noexcept
	{
		if (m_impl == nullptr)
			return {};

		Stats stats	  = m_impl->stats;
		stats.loading = m_impl->loading;
		stats.banks	  = static_cast<u32>(m_impl->banks.size()) - m_impl->loading;
		stats.events  = static_cast<u32>(m_impl->events.size());
		stats.held	  = m_impl->held_count;
		stats.tracks = (m_impl->music.instance != nullptr ? 1u : 0u) + (m_impl->ambience.instance != nullptr ? 1u : 0u);

		FMOD_STUDIO_CPU_USAGE studio_cpu = {};
		FMOD_CPU_USAGE core_cpu			 = {};
		if (m_impl->studio->getCPUUsage(&studio_cpu, &core_cpu) == FMOD_OK)
		{
			stats.studio_cpu = studio_cpu.update;
			stats.mixer_cpu	 = core_cpu.dsp;
		}

		int current = 0;
		if (FMOD::Memory_GetStats(&current, nullptr, false) == FMOD_OK)
			stats.memory_bytes = static_cast<u32>(current);
		return stats;
	}
}
