#include <ember/core/profile.h>
#include <ember/ecs/system.h>
#include <ember/ecs/world.h>
#include <ember/jobs/job_system.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <iterator>

namespace ember::ecs
{
	namespace
	{
		static_assert(std::atomic_ref<u32>::required_alignment <= alignof(u32), "a node counts down in place");

		[[nodiscard]] bool runs_here(const SystemInfo& system, Role role) noexcept
		{
			switch (system.where)
			{
				case Where::Server:
					return role != Role::Client;
				case Where::Client:
					return role != Role::Server;
				default:
					return true;
			}
		}

		/** What two systems both touch, one of them writing it: why the later one waits. */
		[[nodiscard]] String shared(const SystemInfo& earlier, const SystemInfo& later)
		{
			String names;
			for (const AccessInfo& mine : earlier.access)
			{
				for (const AccessInfo& theirs : later.access)
				{
					if (mine.type != theirs.type || (!mine.write && !theirs.write))
						continue;

					if (!names.empty())
						names += ", ";
					names += mine.name;
				}
			}
			return names;
		}
	}

	void Commands::destroy(Entity entity) noexcept
	{
		push(
			stream(),
			+[](World& world, SystemContext&, const u8* bytes) noexcept
			{
				detail::DestroyPayload payload;
				std::memcpy(&payload, bytes, sizeof(payload));
				if (world.registry.valid(payload.entity))
					world.registry.destroy(payload.entity);
			},
			detail::DestroyPayload{.entity = entity});
	}

	namespace
	{
		void apply_add_raw(World& world, SystemContext& context, const u8* bytes) noexcept
		{
			detail::RawPayload payload;
			std::memcpy(&payload, bytes, sizeof(payload));

			const Entity entity = context.resolve(payload.target);
			if (!world.registry.valid(entity))
				return;

			const ComponentInfo& info = world.components()[payload.component];
			if (lives_in(info.kind, world.role()))
				info.emplace(info, world.registry, entity, bytes + sizeof(payload));
		}

		void apply_remove_raw(World& world, SystemContext& context, const u8* bytes) noexcept
		{
			detail::RawPayload payload;
			std::memcpy(&payload, bytes, sizeof(payload));

			const Entity entity = context.resolve(payload.target);
			if (world.registry.valid(entity))
				{
				const ComponentInfo& info = world.components()[payload.component];
				info.remove(info, world.registry, entity);
			}
		}
	}

	void Commands::push_raw(detail::CommandStream& into, detail::ApplyFn apply, const detail::RawPayload& payload,
							const void* bytes, u32 size) noexcept
	{
		const size_t at	 = into.bytes.size();
		const u32 total = static_cast<u32>(sizeof(payload)) + size;
		into.bytes.resize(at + sizeof(apply) + sizeof(total) + total);
		std::memcpy(into.bytes.data() + at, &apply, sizeof(apply));
		std::memcpy(into.bytes.data() + at + sizeof(apply), &total, sizeof(total));
		std::memcpy(into.bytes.data() + at + sizeof(apply) + sizeof(total), &payload, sizeof(payload));
		if (size > 0)
			std::memcpy(into.bytes.data() + at + sizeof(apply) + sizeof(total) + sizeof(payload), bytes, size);
	}

	void Commands::add(Entity entity, ComponentId component, const void* value) noexcept
	{
		const World& world		  = m_context->world();
		const ComponentInfo& info = world.components()[component];
		if (!lives_in(info.kind, world.role()))
			return;
		push_raw(stream(), &apply_add_raw, {.target = {.entity = entity, .spawned = {}}, .component = component}, value,
				 info.size);
	}

	void Commands::add(Spawned spawned, ComponentId component, const void* value) noexcept
	{
		const World& world		  = m_context->world();
		const ComponentInfo& info = world.components()[component];
		if (!lives_in(info.kind, world.role()))
			return;
		push_raw(stream(), &apply_add_raw, {.target = {.entity = NO_ENTITY, .spawned = spawned}, .component = component},
				 value, info.size);
	}

	void Commands::remove(Entity entity, ComponentId component) noexcept
	{
		push_raw(stream(), &apply_remove_raw, {.target = {.entity = entity, .spawned = {}}, .component = component},
				 nullptr, 0);
	}

	u32 Commands::segment() noexcept
	{
		if (m_segment != OWN)
			return m_segment;

		// The system's own commands, made from inside its split loop, would race the loop's ranges.
		EMBER_ASSERT(!m_context->splitting() && "inside parallel_each, make commands with the Commands& it passes fn");
		return m_context->own_segment();
	}

	detail::CommandStream& Commands::stream() noexcept { return m_context->segment(segment()); }

	SystemContext::SystemContext(World& world, const SystemInfo* system) noexcept
		: m_world(world), m_commands(*this), m_name(system != nullptr ? system->name.data() : "commands"),
		  m_simulates(system != nullptr ? system->phase == Phase::Simulate : world.role() != Role::Client),
		  m_structural(system == nullptr || system->structural)
	{
	}

	u32 SystemContext::own_segment() noexcept
	{
		// After a split loop that is its last range's stretch: either way, after everything made before.
		return m_used > 0 ? m_used - 1 : claim(1);
	}

	u32 SystemContext::claim(u32 count) noexcept
	{
		const u32 first = m_used;
		m_used += count;
		if (m_segments.size() < m_used)
			m_segments.resize(m_used);
		return first;
	}

	bool SystemContext::loop_writes(entt::id_type type) const noexcept
	{
		for (const entt::id_type written : m_loop_writes)
		{
			if (written == type)
				return true;
		}
		return false;
	}

	void SystemContext::split(u32 count, u32 grain, detail::RangeFn fn, void* data,
							  Span<const entt::id_type> writes) noexcept
	{
		EMBER_ASSERT(!m_splitting && "parallel_each inside parallel_each: split the outer loop only");
		if (count == 0)
			return;

		struct Range
		{
			u32 begin		   = 0;
			u32 end			   = 0;
			u32 segment		   = 0;
			detail::RangeFn fn = nullptr;
			void* data		   = nullptr;
		};

		// Even ranges of about grain items, the first leftover ones an item longer: the same split for the
		// same count, whoever runs it. Shuffled splits as finely as it can, the better to catch a range
		// that leans on another.
		const RunMode mode = m_world.mode();
		const u32 per	   = mode == RunMode::Shuffled ? 1 : std::max(grain, 1u);
		const u32 used	   = std::min(1 + (count - 1) / per, jobs::MAX_RANGE_JOBS);
		const u32 first	   = claim(used);

		Range ranges[jobs::MAX_RANGE_JOBS];
		u32 begin = 0;
		for (u32 i = 0; i < used; ++i)
		{
			const u32 end = begin + count / used + (i < count % used ? 1 : 0);
			ranges[i]	  = {.begin = begin, .end = end, .segment = first + i, .fn = fn, .data = data};
			begin		  = end;
		}

		m_splitting	  = true;
		m_loop_writes = writes;
		if (mode == RunMode::Jobs && used > 1)
		{
			jobs::JobDef defs[jobs::MAX_RANGE_JOBS];
			for (u32 i = 0; i < used; ++i)
			{
				defs[i] = {
					.fn =
						[](void* job)
					{
						const Range& range = *static_cast<const Range*>(job);
						range.fn(range.begin, range.end, range.segment, range.data);
					},
					.data = &ranges[i],
					.name = m_name,
				};
			}

			jobs::Counter done;
			jobs::kick(Span<const jobs::JobDef>(defs, used), &done);
			jobs::wait(done);
		}
		else
		{
			u32 order[jobs::MAX_RANGE_JOBS];
			for (u32 i = 0; i < used; ++i)
				order[i] = i;

			if (mode == RunMode::Shuffled)
			{
				for (u32 i = used; i > 1; --i)
					std::swap(order[i - 1], order[m_world.random() % i]);
			}

			for (u32 i = 0; i < used; ++i)
			{
				const Range& range = ranges[order[i]];
				range.fn(range.begin, range.end, range.segment, range.data);
			}
		}
		m_splitting	  = false;
		m_loop_writes = {};
	}

	Entity SystemContext::resolve(const detail::Target& target) noexcept
	{
		if (target.entity != NO_ENTITY)
			return target.entity;

		if (target.spawned.segment >= m_used)
			return NO_ENTITY;

		const Vector<Entity>& spawned = m_segments[target.spawned.segment].spawned;
		return target.spawned.index < spawned.size() ? spawned[target.spawned.index] : NO_ENTITY;
	}

	void SystemContext::apply() noexcept
	{
		// Stretch by stretch, each command in the order it was made: a function to run, its payload's
		// size, the payload.
		for (u32 segment = 0; segment < m_used; ++segment)
		{
			detail::CommandStream& stream = m_segments[segment];
			stream.spawned.reserve(stream.spawns);
			for (size_t at = 0; at < stream.bytes.size();)
			{
				detail::ApplyFn apply = nullptr;
				u32 size			  = 0;
				std::memcpy(&apply, stream.bytes.data() + at, sizeof(apply));
				std::memcpy(&size, stream.bytes.data() + at + sizeof(apply), sizeof(size));

				const size_t payload = at + sizeof(apply) + sizeof(size);
				apply(m_world, *this, stream.bytes.data() + payload);
				at = payload + size;
			}
		}

		// Emptied, not freed: the next stage's commands reuse the memory.
		for (u32 segment = 0; segment < m_used; ++segment)
		{
			m_segments[segment].bytes.clear();
			m_segments[segment].spawned.clear();
			m_segments[segment].spawns = 0;
		}
		m_used = 0;
	}

	World::World(const Registry& game, Role role) noexcept : m_role(role), m_registry(game), m_outside(*this, nullptr)
	{
		EMBER_ASSERT(role < Role::Count);

		// Every storage exists before anything runs: EnTT makes one on first use, and two systems running
		// at once must never both be first.
		for (const ComponentInfo& info : components().all())
			info.assure(info, registry);
		(void)registry.storage<Simulated>();
		(void)registry.storage<PrefabRef>();

		// Each phase's systems for this role, in stage order, registration order within a stage.
		for (u32 phase = 0; phase < static_cast<u32>(Phase::Count); ++phase)
		{
			Vector<Stage>& stages = m_stages[phase];
			for (const SystemInfo& system : m_registry.systems())
			{
				if (static_cast<u32>(system.phase) != phase || !runs_here(system, role))
					continue;

				// The storages its views read, those of components the game never registered too.
				system.prepare(registry);

				auto stage = std::find_if(stages.begin(), stages.end(),
										  [&](const Stage& existing) { return existing.index == system.stage; });
				if (stage == stages.end())
				{
					stage		 = stages.emplace(std::upper_bound(stages.begin(), stages.end(), system.stage,
																   [](u8 index, const Stage& existing)
																   { return index < existing.index; }));
					stage->index = system.stage;
				}

				Node& node	 = stage->nodes.emplace_back();
				node.system	 = &system;
				node.context = memory::make_unique<SystemContext>(MemoryTag::ECS, *this, &system);
			}
		}

		// Each stage's graph. Nothing moves from here on, so the nodes may point at their stage.
		for (Vector<Stage>& stages : m_stages)
		{
			for (Stage& stage : stages)
			{
				plan(stage);
				for (Node& node : stage.nodes)
				{
					node.world = this;
					node.stage = &stage;
				}
			}
		}
	}

	World::~World() noexcept = default;

	const Components& World::components() const noexcept { return m_registry.component_types(); }

	const Prefabs& World::prefabs() const noexcept { return m_registry.prefab_types(); }

	void World::plan(Stage& stage) noexcept
	{
		// EnTT's flow makes the graph: a system waits for an earlier one when they touch the same type and
		// either writes it, and a wait that a longer chain of them already implies is dropped.
		entt::flow flow;
		for (u32 i = 0; i < stage.nodes.size(); ++i)
		{
			flow.bind(static_cast<entt::id_type>(i));

			// An exclusive system is a barrier: it waits for everything before it, and everything after waits for it.
			if (stage.nodes[i].system->exclusive)
				flow.sync();

			for (const AccessInfo& access : stage.nodes[i].system->access)
			{
				if (access.write)
					flow.rw(access.type);
				else
					flow.ro(access.type);
			}
		}

		const entt::flow::graph_type graph = flow.graph();
		for (const auto [from, to] : graph.edges())
		{
			stage.nodes[from].next.push_back(static_cast<u32>(to));
			stage.nodes[to].after.push_back(static_cast<u32>(from));
			++stage.nodes[to].waits_for;
		}
	}

	void World::run(Phase phase) noexcept
	{
		// A stage's jobs are waited for on the scheduler, which only main and jobs can do: they have a fiber
		// to park. Any other thread hands the phase to a job and sleeps until the job hands the world back.
		if (m_mode == RunMode::Jobs && jobs::worker_index() == jobs::NO_WORKER)
		{
			if (jobs::worker_count() > 1)
			{
				run_as_job(phase);
				return;
			}

			// No worker but main, which takes a job only when it waits itself: one system at a time here,
			// which makes the same world.
			m_mode = RunMode::Serial;
			run_stages(phase);
			m_mode = RunMode::Jobs;
			return;
		}

		run_stages(phase);
	}

	void World::run_as_job(Phase phase) noexcept
	{
		constexpr const char* NAMES[] = {"world input", "world simulate", "world present"};

		m_handed = phase;
		jobs::kick({
			.fn =
				[](void* data)
			{
				World& world = *static_cast<World*>(data);
				world.run_stages(world.m_handed);
				world.m_handed_back.release();
			},
			.data = this,
			.name = NAMES[static_cast<u32>(phase)],
		});

		// On the operating system, not the scheduler: this thread has no queue of its own to serve meanwhile.
		m_handed_back.acquire();
	}

	void World::run_stages(Phase phase) noexcept
	{
		for (Stage& stage : m_stages[static_cast<u32>(phase)])
		{
			if (m_mode == RunMode::Jobs)
				run_jobs(stage);
			else if (m_mode == RunMode::Shuffled)
				run_shuffled(stage);
			else
			{
				for (Node& node : stage.nodes)
					run_inline(node);
			}

			// The stage's structural changes, in registration order: the same whatever ran at once.
			for (Node& node : stage.nodes)
				node.context->apply();
		}
	}

	void World::run_inline(Node& node) noexcept
	{
		EMBER_PROFILE_FIBER_SCOPE_C("system", PROFILE_COLOR_GAMEPLAY);
		EMBER_PROFILE_FIBER_ZONE_NAME(node.system->name.data(), node.system->name.size());
		node.system->run(*node.world, *node.context);
	}

	void World::run_jobs(Stage& stage) noexcept
	{
		// One system has nobody to run beside: no job to kick.
		if (stage.nodes.size() == 1)
		{
			run_inline(stage.nodes[0]);
			return;
		}

		EMBER_ASSERT(jobs::worker_index() != jobs::NO_WORKER && "a stage's jobs are kicked from main or a job");

		jobs::Counter done;
		stage.done = &done;

		for (Node& node : stage.nodes)
			node.waiting = node.waits_for;

		for (Node& node : stage.nodes)
		{
			if (node.waits_for == 0)
				jobs::kick({.fn = &run_node, .data = &node, .name = node.system->name.data()}, &done);
		}

		jobs::wait(done);
		stage.done = nullptr;
	}

	void World::run_node(void* data) noexcept
	{
		Node& node = *static_cast<Node*>(data);
		node.system->run(*node.world, *node.context);

		// The last of a system's parents to finish starts it, on the counter the stage waits on. This job
		// still holds its own count there, so the count cannot reach zero in between.
		for (u32 next : node.next)
		{
			Node& child = node.stage->nodes[next];
			if (std::atomic_ref<u32>(child.waiting).fetch_sub(1, std::memory_order_acq_rel) == 1)
				jobs::kick({.fn = &run_node, .data = &child, .name = child.system->name.data()}, node.stage->done);
		}
	}

	void World::run_shuffled(Stage& stage) noexcept
	{
		// Any order the graph allows: each time, a random pick among the systems whose waits are over.
		Vector<u32> ready;
		for (u32 i = 0; i < stage.nodes.size(); ++i)
		{
			stage.nodes[i].waiting = stage.nodes[i].waits_for;
			if (stage.nodes[i].waiting == 0)
				ready.push_back(i);
		}

		while (!ready.empty())
		{
			const size_t pick = random() % ready.size();
			Node& node		  = stage.nodes[ready[pick]];
			ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(pick));

			run_inline(node);
			for (u32 next : node.next)
			{
				if (--stage.nodes[next].waiting == 0)
					ready.push_back(next);
			}
		}
	}

	u64 World::random() noexcept
	{
		// SplitMix64: small, and the same numbers on every platform.
		u64 z = (m_random += 0x9e3779b97f4a7c15ull);
		z	  = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z	  = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		return z ^ (z >> 31);
	}

	const Prefab& World::prefab_of(PrefabId id) const noexcept
	{
		const auto retuned = m_retuned.find(id);
		return retuned != m_retuned.end() ? retuned->second : prefabs()[id];
	}

	void World::retune_prefab(PrefabId id, Vector<PrefabComponent> components) noexcept
	{
		const Prefab& base = prefabs()[id];
		Prefab retuned;
		retuned.name	   = base.name;
		retuned.id		   = base.id;
		retuned.definition = base.definition;
		retuned.components = std::move(components);
		m_retuned.insert_or_assign(id, std::move(retuned));
	}

	void World::instantiate(Entity entity, const Prefab& given) noexcept
	{
		const Components& types = components();
		const Prefab& prefab	= prefab_of(given.id);

		for (const PrefabComponent& component : prefab.components)
		{
			const ComponentInfo& info = types[component.id];
			if (lives_in(info.kind, m_role))
				info.emplace(info, registry, entity, component.value.data());
		}
	}

	Entity World::create(const Prefab& prefab, bool simulated) noexcept
	{
		const Entity entity = registry.create();
		instantiate(entity, prefab);
		registry.emplace<PrefabRef>(entity, PrefabRef{.id = prefab.id});
		if (simulated)
			registry.emplace<Simulated>(entity);
		return entity;
	}

	String World::describe(Phase phase) const
	{
		constexpr const char* PHASES[] = {"Input", "Simulate", "Present"};
		constexpr const char* WHERE[]  = {"everywhere", "server", "client"};

		String out;
		for (const Stage& stage : m_stages[static_cast<u32>(phase)])
		{
			const SystemInfo& first = *stage.nodes.front().system;
			if (phase == Phase::Simulate && first.stage_name != nullptr)
				fmt::format_to(std::back_inserter(out), "{}.{}\n", PHASES[static_cast<u32>(phase)],
							   first.stage_name(stage.index));
			else if (phase == Phase::Simulate)
				fmt::format_to(std::back_inserter(out), "{}.{}\n", PHASES[static_cast<u32>(phase)], stage.index);
			else
				fmt::format_to(std::back_inserter(out), "{}\n", PHASES[static_cast<u32>(phase)]);

			for (const Node& node : stage.nodes)
			{
				const SystemInfo& system = *node.system;

				String reads;
				String writes;
				for (const AccessInfo& access : system.access)
					fmt::format_to(std::back_inserter(access.write ? writes : reads), " {}", access.name);
				if (system.structural)
					writes += " +commands";
				if (system.exclusive)
					writes += " +exclusive";

				String after;
				for (u32 parent : node.after)
				{
					const SystemInfo& earlier = *stage.nodes[parent].system;
					fmt::format_to(std::back_inserter(after), "{}{} ({})", after.empty() ? "  after: " : ", ",
								   earlier.name, shared(earlier, system));
				}

				fmt::format_to(std::back_inserter(out), "  {:<28} {:<10} reads:{}  writes:{}{}\n", system.name,
							   WHERE[static_cast<u32>(system.where)], reads, writes, after);
			}
		}

		return out;
	}
}
