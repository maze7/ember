#include <ember/anim/sample.h>

#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ember::anim
{
	namespace
	{
		[[nodiscard]] constexpr u32 index(Channel channel) noexcept { return static_cast<u32>(channel); }

		/** y runs down the screen, so a positive angle turns clockwise. Most parts never turn, and skip the trig. */
		[[nodiscard]] glm::vec2 rotate(glm::vec2 v, f32 angle) noexcept
		{
			if (angle == 0.0f)
				return v;

			const f32 c = std::cos(angle);
			const f32 s = std::sin(angle);
			return {v.x * c - v.y * s, v.x * s + v.y * c};
		}

		/** The clock as what began at `since` sees it: still for as long as a hitstop it was caught in held it. */
		[[nodiscard]] f64 held(const Animator& animator, f64 since, f64 time) noexcept
		{
			const f64 from = animator.held_from;
			if (since >= from || animator.held_until <= from)
				return time;
			return time - std::clamp(time - from, 0.0, animator.held_until - from);
		}

		/** Milliseconds since a clip was asked to start, at its speed, less what a hitstop held it. */
		[[nodiscard]] f32 elapsed(const Animator& animator, const Animator::Played& played, f64 time) noexcept
		{
			return static_cast<f32>((held(animator, played.since, time) - played.since) * 1000.0) * played.speed;
		}

		/** Where in a clip `ms` falls: wrapped when it loops, held at its ends when not. */
		[[nodiscard]] f32 wrap(f32 ms, f32 length, bool loop) noexcept
		{
			if (length <= 0.0f)
				return 0.0f;
			if (!loop)
				return std::clamp(ms, 0.0f, length);

			const f32 t = std::fmod(ms, length);
			return t < 0.0f ? t + length : t;
		}

		/** A track's value at t, held before its first key and after its last. `from` stands in for a first key of
		 * "current". */
		[[nodiscard]] f32 value_at(const Clip& clip, const Track& track, f32 t, f32 from) noexcept
		{
			const Key* keys	 = clip.keys.data() + track.first;
			const auto value = [&](u32 i) { return i == 0 && keys[0].from_current ? from : keys[i].value; };

			if (track.count == 0)
				return from;
			if (t <= keys[0].ms)
				return value(0);

			for (u32 i = 1; i < track.count; ++i)
			{
				if (t < keys[i].ms)
				{
					const f32 span = keys[i].ms - keys[i - 1].ms;
					const f32 u	   = span > 0.0f ? (t - keys[i - 1].ms) / span : 1.0f;
					return value(i - 1) + (keys[i].value - value(i - 1)) * ease(keys[i].ease, u);
				}
			}

			return value(track.count - 1);
		}

		/** The sprite a sprite track shows at t: its last key at or before t, or its first before any. */
		[[nodiscard]] Name sprite_at(const Clip& clip, const SpriteTrack& track, f32 t) noexcept
		{
			const SpriteKey* keys = clip.sprite_keys.data() + track.first;
			Name sprite			  = track.count > 0 ? keys[0].sprite : 0;
			for (u32 k = 1; k < track.count && keys[k].ms <= t; ++k)
				sprite = keys[k].sprite;
			return sprite;
		}

		/** A clip's length: its own, or where its last key or flipbook frame ends on the sheets its layers show. */
		[[nodiscard]] f32 length_of(const Clip& clip, const Sheet* const* sheets) noexcept
		{
			if (clip.length >= 0.0f)
				return clip.length;

			f32 end = 0.0f;
			for (const Track& track : clip.tracks)
				if (track.count > 0)
					end = std::max(end, clip.keys[track.first + track.count - 1].ms);
			for (const SpriteTrack& track : clip.sprite_tracks)
				if (track.count > 0)
					end = std::max(end, clip.sprite_keys[track.first + track.count - 1].ms);
			for (const Flipbook& book : clip.flipbooks)
			{
				const Sheet* sheet		 = sheets != nullptr ? sheets[book.layer] : nullptr;
				const Sequence* sequence = sheet != nullptr ? find(sheet->sequences, book.sequence) : nullptr;
				end = std::max(end, book.at + book.ms * static_cast<f32>(sequence != nullptr ? sequence->count : 1));
			}
			return end;
		}

		/** One layer's entries in a list sorted by layer, starting where the last layer's ended. */
		template <class T>
		[[nodiscard]] Span<const T> of_layer(const Vector<T>& items, size_t& cursor, u32 layer) noexcept
		{
			while (cursor < items.size() && items[cursor].layer < layer)
				++cursor;

			const size_t first = cursor;
			while (cursor < items.size() && items[cursor].layer == layer)
				++cursor;

			return {items.data() + first, cursor - first};
		}

		/** Where a layer sits from its space's origin: x and y, then `orbit` degrees round, `radius` out. */
		[[nodiscard]] glm::vec2 offset(const f32* v) noexcept
		{
			const glm::vec2 at = {v[index(Channel::X)], v[index(Channel::Y)]};
			return at + rotate({v[index(Channel::Radius)], 0.0f}, glm::radians(v[index(Channel::Orbit)]));
		}

		/** Overlays combine with what plays: offsets and turns add, scales and show multiply, flash takes the brighter.
		 */
		void combine(Channel channel, f32& to, f32 value) noexcept
		{
			switch (channel)
			{
				case Channel::ScaleX:
				case Channel::ScaleY:
				case Channel::Show:
					to *= value;
					break;
				case Channel::Flash:
					to = std::max(to, value);
					break;
				default:
					to += value;
					break;
			}
		}

		/** Where a rig stands and how it is turned: the entity's origin, or the layer it is mounted on. */
		struct Space
		{
			glm::vec2 origin = {};
			f32 angle		 = 0.0f;
		};

		/**
		 * A layer as placed this frame: what the layers socketed on it, and a rig mounted on it, read.
		 * No defaults, so a rig's array of them costs nothing until each layer writes its own.
		 */
		struct Placed
		{
			glm::vec2 position;
			f32 angle;
			glm::vec2 scale;
			const Sheet* sheet;
			const Sprite* sprite;

			/** One of its sheet's points, where it is now. */
			[[nodiscard]] glm::vec2 point(glm::vec2 at) const noexcept
			{
				return position + rotate((at - sprite->pivot) * scale, angle);
			}
		};

		/** The clip a rig plays this frame, and how far in. */
		struct Playing
		{
			const Animator::Playhead* head = nullptr;
			Animator::Played played;
			const Clip* clip = nullptr;
			f32 length		 = 0.0f;
			f32 t			 = 0.0f;
		};

		/** Where a rig's layers have got to in its clip's tracks, which are sorted by layer. */
		struct Cursors
		{
			size_t tracks		 = 0;
			size_t sprite_tracks = 0;
			size_t flipbooks	 = 0;
		};

		/** A layer's channels and sprite this frame, before it is placed. */
		struct Keyed
		{
			f32 v[CHANNELS]		 = {};
			const Sprite* sprite = nullptr;
			u16 sprite_index	 = 0;
		};

		/** One entity's sampling: what it reads, and the pose it writes. */
		class Sampler
		{
		public:
			Sampler(const Source& source, const Animator& animator, const Look& look, f64 now, f64 previous,
					Pose& pose) noexcept
				: m_source(source), m_animator(animator), m_look(look), m_now(now), m_previous(previous), m_pose(pose),
				  m_mirror(animator.facing.x < 0.0f)
			{
			}

			void run() noexcept
			{
				m_pose.part_count  = 0;
				m_pose.point_count = 0;
				m_pose.event_count = 0;
				m_pose.sound_count = 0;

				// Leaning clockwise on the screen is leaning the other way in a mirrored rig.
				const f32 lean = m_mirror ? -m_animator.lean : m_animator.lean;
				if (const Rig* rig = m_source.rig(m_animator.rig_id))
					sample_rig(*rig, m_animator.rig_id, 0, {.angle = lean}, 0.0f);

				std::copy_n(m_animator.inputs, m_animator.input_count, m_pose.inputs);
				m_pose.input_count = m_animator.input_count;
			}

		private:
			/**
			 * A rig's layers in order, each keyed by the clip and the overlays, placed on its socket or the
			 * rig's origin, and drawn: its sprite, or the rig the look mounts in its slot. The entity's own
			 * layers order back to front by whole numbers, a mounted rig's parts just after their mount's.
			 */
			void sample_rig(const Rig& rig, u16 rig_id, Name mount, Space space, f32 order) noexcept
			{
				// A file's rig has no more, its parser refuses them; one built in code that does draws nothing.
				const u32 layer_count = static_cast<u32>(rig.layers.size());
				EMBER_ASSERT(layer_count <= MAX_LAYERS && "a rig has at most MAX_LAYERS layers");
				if (layer_count > MAX_LAYERS) [[unlikely]]
					return;

				// Each layer's, written in order before anything reads it.
				u16 sheet_ids[MAX_LAYERS];
				const Sheet* sheets[MAX_LAYERS];
				for (u32 i = 0; i < layer_count; ++i)
					sheets[i] = sheet_for(rig_id, rig.layers[i].slot, mount, sheet_ids[i]);

				const Playing playing = play(rig, mount, sheets);

				// A rig that follows an input turns with it; facing west, the input is mirrored in.
				if (rig.turn != 0)
				{
					const f32 turn = m_animator.input(rig.turn);
					space.angle += m_mirror ? std::numbers::pi_v<f32> - turn : turn;
				}

				Cursors cursors;
				Placed placed[MAX_LAYERS];

				for (u32 i = 0; i < layer_count; ++i)
				{
					const Layer& layer = rig.layers[i];
					const Keyed keyed  = key(rig, i, playing, sheets, cursors, mount == 0);
					placed[i]		   = place(layer, keyed, placed, i, space, sheets[i]);

					if (mount == 0)
						add_points(layer, placed[i]);

					if (mount == 0 && draw_mounted(layer, placed[i], order + static_cast<f32>(i), order))
						continue;

					if (keyed.sprite != nullptr && keyed.v[index(Channel::Show)] >= 0.5f)
						emit(keyed, placed[i], sheet_ids[i],
							 mount == 0 ? order + static_cast<f32>(i) : order + 0.01f * static_cast<f32>(i + 1));
				}
			}

			/** A layer's rest values, then the clip's keys for it, then the overlays; and the sprite it shows. */
			[[nodiscard]] Keyed key(const Rig& rig, u32 i, const Playing& playing, const Sheet* const* sheets,
									Cursors& cursors, bool overlays) const noexcept
			{
				const Layer& layer = rig.layers[i];
				const Sheet* sheet = sheets[i];

				Keyed keyed;
				std::copy(std::begin(layer.rest), std::end(layer.rest), keyed.v);
				Name sprite = layer.sprite;

				if (const Clip* clip = playing.clip)
				{
					for (const Track& track : of_layer(clip->tracks, cursors.tracks, i))
						keyed.v[index(track.channel)] =
							value_at(*clip, track, playing.t, current(rig, playing, track, sheets));

					for (const SpriteTrack& track : of_layer(clip->sprite_tracks, cursors.sprite_tracks, i))
						sprite = sprite_at(*clip, track, playing.t);

					for (const Flipbook& book : of_layer(clip->flipbooks, cursors.flipbooks, i))
						keyed.sprite = frame_at(sheet, book, playing.t, keyed.sprite_index);
				}

				if (overlays)
					overlay(rig, i, keyed.v);

				if (keyed.sprite == nullptr && sheet != nullptr && sprite != 0)
					if (const u16* found = find(sheet->names, sprite))
						keyed.sprite = &sheet->sprites[keyed.sprite_index = *found];

				return keyed;
			}

			/** Where a keyed layer is: on its socket or its rig's origin, turned with its rig. */
			[[nodiscard]] static Placed place(const Layer& layer, const Keyed& keyed, const Placed* placed, u32 i,
											  Space space, const Sheet* sheet) noexcept
			{
				return {
					.position = origin(layer, placed, i, space) + rotate(offset(keyed.v), space.angle),
					.angle	  = space.angle + glm::radians(keyed.v[index(Channel::Angle)]),
					.scale	  = {keyed.v[index(Channel::ScaleX)], keyed.v[index(Channel::ScaleY)]},
					.sheet	  = sheet,
					.sprite	  = keyed.sprite,
				};
			}

			/**
			 * The rig the look mounts in a layer's slot, drawn from where the layer is, at its order;
			 * above its socket, a sorting layer's rig draws behind the rest of its own. False when the
			 * slot holds no rig.
			 */
			bool draw_mounted(const Layer& layer, const Placed& here, f32 at, f32 order) noexcept
			{
				const Look::Slot* slot = m_look.find(layer.slot);
				if (slot == nullptr || !slot->rig)
					return false;

				if (const Rig* inner = m_source.rig(slot->id))
				{
					const u8 first = m_pose.part_count;
					sample_rig(*inner, slot->id, layer.slot, {here.position, here.angle}, at);

					if (layer.sort_y)
						move_order(first, here.position.y, at, order - 1.0f);
				}
				return true;
			}

			/** The sheet a slot shows: the look's, else the rig's default. */
			[[nodiscard]] const Sheet* sheet_for(u16 rig_id, Name slot, Name mount, u16& id) const noexcept
			{
				const Look::Slot* chosen = m_look.find(mount != 0 ? join(mount, slot) : slot);
				id = chosen != nullptr ? (chosen->rig ? NO_ID : chosen->id) : m_source.default_sheet(rig_id, slot);
				return m_source.sheet(id);
			}

			/** The clip a rig's playhead plays, how far in, and the events it passed since the frame before. */
			[[nodiscard]] Playing play(const Rig& rig, Name mount, const Sheet* const* sheets) noexcept
			{
				Playing playing;
				playing.head   = m_animator.find(mount);
				playing.played = playing.head != nullptr ? playing.head->now : Animator::Played{};
				playing.clip   = rig.playing(playing.played.clip);

				if (playing.clip != nullptr)
				{
					playing.length = length_of(*playing.clip, sheets);
					playing.t = wrap(into(*playing.clip, playing.played, playing.length, m_now), playing.length,
									 playing.clip->loop);
					fire(rig, *playing.clip, playing.played, playing.length, mount);
				}
				return playing;
			}

			/** Each event a clip passed since the frame before, once, however short the frame. */
			void fire(const Rig& rig, const Clip& clip, const Animator::Played& played, f32 length, Name mount) noexcept
			{
				if (clip.events.empty() || length <= 0.0f)
					return;

				const f32 now = into(clip, played, length, m_now);
				const f32 previous =
					clip.input != 0 ? sampled(clip, length, now) : elapsed(m_animator, played, m_previous);

				for (const Event& event : clip.events)
				{
					const bool passed =
						clip.loop ? std::floor((now - event.ms) / length) > std::floor((previous - event.ms) / length)
								  : previous < event.ms && event.ms <= now;

					if (!passed)
						continue;

					if (m_pose.event_count < Pose::EVENTS)
						m_pose.events[m_pose.event_count++] = mount != 0 ? join(mount, event.name) : event.name;

					// What the event sounds like is its own rig's to say: a sword brings its swing's sound.
					if (const Cue* cue = find(rig.sounds, event.name);
						cue != nullptr && m_pose.sound_count < Pose::SOUNDS)
						m_pose.sounds[m_pose.sound_count++] = cue->sound;
				}
			}

			/** How far into a clip, in ms: on the clock since it began, or as far through as the input playing it. */
			[[nodiscard]] f32 into(const Clip& clip, const Animator::Played& played, f32 length, f64 time) const noexcept
			{
				return clip.input != 0 ? m_animator.input(clip.input) * (length / clip.span)
									   : elapsed(m_animator, played, time);
			}

			/**
			 * Where a clip an input plays was the frame before: that input as the pose was last sampled with it.
			 * First seen, where it is now, so it fires nothing it did not pass.
			 */
			[[nodiscard]] f32 sampled(const Clip& clip, f32 length, f32 now) const noexcept
			{
				for (u32 i = 0; i < m_pose.input_count; ++i)
					if (m_pose.inputs[i].name == clip.input)
						return m_pose.inputs[i].value * (length / clip.span);
				return now;
			}

			/**
			 * Where a track that starts from "current" starts: its channel as the clip this one replaced
			 * had it at the moment this one began, worked out one clip further back when that one started
			 * from "current" too. The layer's rest when the history runs out or leaves the channel alone.
			 */
			[[nodiscard]] f32 current(const Rig& rig, const Playing& playing, const Track& track,
									  const Sheet* const* sheets) const noexcept
			{
				const f32 rest = rig.layers[track.layer].rest[index(track.channel)];
				if (playing.head == nullptr || !playing.clip->keys[track.first].from_current)
					return rest;
				return before(rig, *playing.head, 0, playing.played.since, track, sheets, rest);
			}

			/** The channel as the clip `depth` back in the history had it at `moment`. */
			[[nodiscard]] f32 before(const Rig& rig, const Animator::Playhead& head, u32 depth, f64 moment,
									 const Track& track, const Sheet* const* sheets, f32 rest) const noexcept
			{
				if (depth == Animator::Playhead::HISTORY)
					return rest;

				const Animator::Played& played = head.previous[depth];
				const Clip* clip			   = played.clip != 0 ? rig.clip(played.clip) : nullptr;
				if (clip == nullptr)
					return rest;

				for (const Track& earlier : clip->tracks)
				{
					if (earlier.layer != track.layer || earlier.channel != track.channel)
						continue;

					const f32 length = length_of(*clip, sheets);
					const f32 at	 = wrap(into(*clip, played, length, moment), length, clip->loop);
					const f32 from = clip->keys[earlier.first].from_current
										 ? before(rig, head, depth + 1, played.since, track, sheets, rest)
										 : rest;
					return value_at(*clip, earlier, at, from);
				}
				return rest;
			}

			/** The overlays on the entity's own rig, for one of its layers, until each has run its course. */
			void overlay(const Rig& rig, u32 layer, f32* v) const noexcept
			{
				for (u32 o = 0; o < m_animator.overlay_count; ++o)
				{
					const Animator::Played& played = m_animator.overlays[o];
					const Clip* clip			   = rig.clip(played.clip);
					if (clip == nullptr)
						continue;

					// On the clock, or as far through its seconds as the clock is, when it was asked to last them.
					const f32 length = length_of(*clip, nullptr);
					const f64 now	 = held(m_animator, played.since, m_now);
					const f32 t		 = played.seconds > 0.0f
										   ? static_cast<f32>((now - played.since) / played.seconds) * length
										   : elapsed(m_animator, played, m_now);
					if (t < 0.0f || t > length)
						continue;

					for (const Track& track : clip->tracks)
						if (track.layer == layer)
							combine(track.channel, v[index(track.channel)], value_at(*clip, track, t, 0.0f));
				}
			}

			/** A flipbook's frame at t, held on its first before `at` and its last after the end. */
			[[nodiscard]] static const Sprite* frame_at(const Sheet* sheet, const Flipbook& book, f32 t,
														u16& sprite) noexcept
			{
				const Sequence* sequence = sheet != nullptr ? find(sheet->sequences, book.sequence) : nullptr;
				if (sequence == nullptr || sequence->count == 0)
					return nullptr;

				const i32 frame = static_cast<i32>(std::floor((t - book.at) / book.ms));
				sprite			= static_cast<u16>(sequence->first + std::clamp(frame, 0, sequence->count - 1));
				return &sheet->sprites[sprite];
			}

			/** A layer's space starts at its socket, a point on an earlier layer's sprite, else at its rig's. */
			[[nodiscard]] static glm::vec2 origin(const Layer& layer, const Placed* placed, u32 i, Space space) noexcept
			{
				if (layer.parent == NO_ID || layer.parent >= i)
					return space.origin;

				const Placed& parent = placed[layer.parent];
				if (parent.sprite == nullptr)
					return space.origin;

				const glm::vec2* at = find(parent.sheet->points, layer.socket);
				return at != nullptr ? parent.point(*at) : space.origin;
			}

			/** A layer's points, where gameplay finds them: "body.heel". */
			void add_points(const Layer& layer, const Placed& placed) noexcept
			{
				if (placed.sprite == nullptr)
					return;

				for (const Named<glm::vec2>& at : placed.sheet->points)
				{
					if (m_pose.point_count == Pose::POINTS)
						return;

					glm::vec2 position = placed.point(at.value);
					if (m_mirror)
						position.x = -position.x;
					m_pose.points[m_pose.point_count++] = {join(layer.name, at.name), position};
				}
			}

			/** Moves the parts drawn since `first` that stand above `y` from order `from` to order `to`. */
			void move_order(u8 first, f32 y, f32 from, f32 to) noexcept
			{
				for (u8 p = first; p < m_pose.part_count; ++p)
					if (m_pose.parts[p].position.y < y)
						m_pose.parts[p].order += to - from;
			}

			/** A layer's sprite into the pose, mirrored when facing west. */
			void emit(const Keyed& keyed, const Placed& here, u16 sheet, f32 order) noexcept
			{
				if (m_pose.part_count == Pose::PARTS)
					return;

				m_pose.parts[m_pose.part_count++] = {
					.sheet	  = sheet,
					.sprite	  = keyed.sprite_index,
					.position = {m_mirror ? -here.position.x : here.position.x, here.position.y},
					.angle	  = m_mirror ? -here.angle : here.angle,
					.scale	  = here.scale,
					.flash	  = std::clamp(keyed.v[index(Channel::Flash)], 0.0f, 1.0f),
					.order	  = order,
					.mirror	  = m_mirror,
				};
			}

			const Source& m_source;
			const Animator& m_animator;
			const Look& m_look;
			f64 m_now;
			f64 m_previous;
			Pose& m_pose;
			bool m_mirror;
		};
	}

	void sample(const Source& source, const Animator& animator, const Look& look, f64 now, f64 previous,
				Pose& pose) noexcept
	{
		Sampler(source, animator, look, now, previous, pose).run();
	}
}
