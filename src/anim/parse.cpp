#include <ember/anim/parse.h>

#include <ember/core/json.h>
#include <ember/memory/memory.h>

#include <fmt/format.h>
#include <glm/common.hpp>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace ember::anim
{
	namespace
	{
		constexpr StringView EASE_NAMES[] = {
			"step",		 "linear",		 "quad_in",		"quad_out",	  "quad_in_out",  "cubic_in",
			"cubic_out", "cubic_in_out", "quart_in",	"quart_out",  "quart_in_out", "sine_in",
			"sine_out",	 "sine_in_out",	 "expo_in",		"expo_out",	  "expo_in_out",  "back_in",
			"back_out",	 "back_in_out",	 "elastic_out", "bounce_out",
		};
		static_assert(std::size(EASE_NAMES) == static_cast<size_t>(Ease::Count));

		constexpr StringView CHANNEL_NAMES[] = {"x",	   "y",		  "orbit", "radius", "angle",
												"scale_x", "scale_y", "flash", "show"};
		static_assert(std::size(CHANNEL_NAMES) == CHANNELS);

		/** Every refusal: false, with what to fix in `error`. */
		template <class... Args> bool fail(String& error, fmt::format_string<Args...> format, Args&&... args)
		{
			const std::string text = fmt::format(format, std::forward<Args>(args)...);
			error.assign(text.data(), text.size());
			return false;
		}

		[[nodiscard]] bool read_json(StringView text, Json& json, String& error)
		{
			JsonError at;
			if (!json.parse(text, memory::heap(MemoryTag::Assets), JsonRead::Relaxed, &at))
				return fail(error, "{}:{}: {}", at.line, at.column, at.message);

			if (!json.root().is_object())
				return fail(error, "the file is not an object");
			return true;
		}

		/** [x, y], or one number for both. */
		[[nodiscard]] glm::vec2 read_vec2(JsonValue value, glm::vec2 fallback)
		{
			f32 numbers[2] = {fallback.x, fallback.y};
			if (value.numbers(numbers) == 1)
				numbers[1] = numbers[0];
			return {numbers[0], numbers[1]};
		}

		[[nodiscard]] bool read_ease(StringView text, Ease& out)
		{
			const auto* found = std::find(std::begin(EASE_NAMES), std::end(EASE_NAMES), text);
			out				  = static_cast<Ease>(found - std::begin(EASE_NAMES));
			return found != std::end(EASE_NAMES);
		}

		/** Where a sprite's opaque texels are, from the corner of its cell: what lifts it off the ground when turned.
		 */
		[[nodiscard]] glm::vec4 measure(const Image& image, u32 x0, u32 y0, u32 width, u32 height)
		{
			u32 left = width, top = height, right = 0, bottom = 0;

			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					if (image.rgba[(size_t{y0 + y} * image.extent.width + x0 + x) * 4 + 3] == 0)
						continue;

					left   = std::min(left, x);
					top	   = std::min(top, y);
					right  = std::max(right, x + 1);
					bottom = std::max(bottom, y + 1);
				}
			}

			return right > left ? glm::vec4(left, top, right, bottom) : glm::vec4(0.0f);
		}

		/** Builds a rig from its file: the layers, then the clips, each over a copy of its base. */
		class RigReader
		{
		public:
			RigReader(Rig& rig, String& error) noexcept : m_rig(rig), m_error(error) {}

			[[nodiscard]] bool read(JsonValue root)
			{
				for (const auto [slot, sheet] : root["slots"].members())
				{
					StringView path;
					if (!sheet.read(path))
						return fail(m_error, "slot '{}' names a sheet's path", slot);
					m_rig.slots.push_back({name(slot), String(path)});
				}

				StringView turn;
				if (root["turn"].read(turn))
					m_rig.turn = name(turn);

				for (const JsonValue layer : root["layers"].elements())
					if (!read_layer(layer))
						return false;

				for (const auto [clip_name, value] : root["clips"].members())
					if (!read_clip(clip_name, value))
						return false;

				return true;
			}

		private:
			[[nodiscard]] i32 layer_index(StringView layer) const
			{
				const Name key = name(layer);
				for (size_t i = 0; i < m_rig.layers.size(); ++i)
					if (m_rig.layers[i].name == key)
						return static_cast<i32>(i);
				return -1;
			}

			[[nodiscard]] bool read_layer(JsonValue value)
			{
				StringView layer_name;
				if (!value["name"].read(layer_name) || layer_name.find('.') != StringView::npos)
					return fail(m_error, "every layer has a \"name\", without dots");
				if (m_rig.layers.size() == MAX_LAYERS)
					return fail(m_error, "layer '{}': a rig has at most {} layers", layer_name, MAX_LAYERS);

				Layer layer;
				layer.name	 = name(layer_name);
				layer.slot	 = name(value["slot"].read_or(layer_name));
				layer.sprite = value["sprite"] ? name(value["sprite"].read_or(StringView())) : 0;
				layer.sort_y = value["sort"].read_or(StringView()) == "y";

				StringView socket;
				if (value["socket"].read(socket))
				{
					const size_t dot = socket.find('.');
					const i32 parent = dot == StringView::npos ? -1 : layer_index(socket.substr(0, dot));
					if (parent < 0)
						return fail(m_error, "layer '{}' sits on '{}': name an earlier layer's point, as \"body.hand\"",
									layer_name, socket);

					layer.parent = static_cast<u16>(parent);
					layer.socket = name(socket.substr(dot + 1));
				}

				for (u32 c = 0; c < CHANNELS; ++c)
					(void)value[CHANNEL_NAMES[c]].read(layer.rest[c]);

				if (value["scale"])
				{
					const glm::vec2 scale						  = read_vec2(value["scale"], {1.0f, 1.0f});
					layer.rest[static_cast<u32>(Channel::ScaleX)] = scale.x;
					layer.rest[static_cast<u32>(Channel::ScaleY)] = scale.y;
				}

				m_rig.layers.push_back(layer);
				return true;
			}

			[[nodiscard]] bool read_clip(StringView clip_name, JsonValue value)
			{
				Clip clip;

				StringView base;
				if (value["base"].read(base))
				{
					const Clip* from = m_rig.clip(name(base));
					if (from == nullptr)
						return fail(m_error, "clip '{}' builds on '{}', which must come before it", clip_name, base);
					clip = *from;
				}

				clip.name = name(clip_name);
				(void)value["length"].read(clip.length);
				(void)value["loop"].read(clip.loop);

				StringView input;
				if (value["input"].read(input))
					clip.input = name(input);
				(void)value["span"].read(clip.span);
				if (clip.span <= 0.0f)
					return fail(m_error, "clip '{}' has a span of {}: how much of its input plays it through, above 0",
								clip_name, clip.span);

				for (const auto [layer, book] : value["frames"].members())
					if (!read_flipbook(clip, layer, book))
						return false;

				for (const auto [target, keys] : value["tracks"].members())
					if (!read_track(clip, target, keys))
						return false;

				// Events listed replace the base's, as a layer's frames or track does.
				if (value["events"])
					clip.events.clear();

				for (const JsonValue element : value["events"].elements())
				{
					StringView event;
					Event read;
					if (!element[0u].read(read.ms) || !element[1u].read(event))
						return fail(m_error, "clip '{}': events are [ms, \"name\"]", clip_name);

					read.name = name(event);
					clip.events.push_back(read);
				}

				// Sampling walks a clip's tracks once, layer by layer, and its events by time.
				const auto by_layer = [](const auto& a, const auto& b) { return a.layer < b.layer; };
				std::stable_sort(clip.tracks.begin(), clip.tracks.end(), by_layer);
				std::stable_sort(clip.sprite_tracks.begin(), clip.sprite_tracks.end(), by_layer);
				std::stable_sort(clip.flipbooks.begin(), clip.flipbooks.end(), by_layer);
				std::stable_sort(clip.events.begin(), clip.events.end(),
								 [](const Event& a, const Event& b) { return a.ms < b.ms; });

				m_rig.clips.push_back(std::move(clip));
				return true;
			}

			[[nodiscard]] bool read_flipbook(Clip& clip, StringView layer_name, JsonValue book)
			{
				const i32 layer = layer_index(layer_name);
				StringView sequence;
				if (layer < 0 || !book["sprites"].read(sequence))
					return fail(
						m_error,
						"frames are {{\"layer\": {{\"sprites\": \"sequence\", \"ms\": 100}}}}, and '{}' is no layer",
						layer_name);

				const Flipbook read{
					.layer	  = static_cast<u16>(layer),
					.sequence = name(sequence),
					.ms		  = book["ms"].read_or(100.0f),
					.at		  = book["at"].read_or(0.0f),
				};
				if (!(read.ms > 0.0f))
					return fail(m_error, "frames of '{}': a frame lasts longer than 0 ms", layer_name);

				std::erase_if(clip.flipbooks, [&](const Flipbook& f) { return f.layer == layer; });
				clip.flipbooks.push_back(read);
				return true;
			}

			/** "blade.orbit": [[ms, value, "ease"], ...], where a value is a number, [x, y] for scale, or a sprite's
			 * name. */
			[[nodiscard]] bool read_track(Clip& clip, StringView target, JsonValue keys)
			{
				const size_t dot = target.rfind('.');
				const i32 layer	 = dot == StringView::npos ? -1 : layer_index(target.substr(0, dot));
				if (layer < 0)
					return fail(m_error, "track '{}': tracks are \"layer.channel\", and that names no layer", target);

				const StringView channel = target.substr(dot + 1);
				if (channel == "sprite")
					return read_sprite_track(clip, static_cast<u16>(layer), target, keys);

				u32 first = 0;
				u32 lanes = 1;
				if (channel == "scale")
				{
					first = static_cast<u32>(Channel::ScaleX);
					lanes = 2;
				}
				else
				{
					const auto* found = std::find(std::begin(CHANNEL_NAMES), std::end(CHANNEL_NAMES), channel);
					if (found == std::end(CHANNEL_NAMES))
						return fail(
							m_error,
							"track '{}': the channels are x, y, orbit, radius, angle, scale, flash, show and sprite",
							target);
					first = static_cast<u32>(found - std::begin(CHANNEL_NAMES));
				}

				for (u32 lane = 0; lane < lanes; ++lane)
				{
					const Channel channel_lane = static_cast<Channel>(first + lane);
					std::erase_if(clip.tracks,
								  [&](const Track& t) { return t.layer == layer && t.channel == channel_lane; });

					Track track{.layer	 = static_cast<u16>(layer),
								.channel = channel_lane,
								.first	 = static_cast<u32>(clip.keys.size())};
					for (const JsonValue element : keys.elements())
					{
						Key key;
						f32 values[2]		   = {};
						const u32 numbers	   = element[1u].numbers(values);
						key.from_current	   = track.count == 0 && element[1u].read_or(StringView()) == "current";
						const StringView eased = element[2u].read_or(StringView("linear"));

						if (!element[0u].read(key.ms) || (numbers == 0 && !key.from_current))
							return fail(
								m_error,
								"track '{}': keys are [ms, value, \"ease\"], and a first value may be \"current\"",
								target);
						if (!read_ease(eased, key.ease))
							return fail(m_error, "track '{}': '{}' is no ease", target, eased);
						if (track.count > 0 && key.ms < clip.keys.back().ms)
							return fail(m_error, "track '{}': keys run in time order, and {} ms comes after {}", target,
										key.ms, clip.keys.back().ms);

						key.value = numbers == 2 ? values[lane] : values[0];
						if (channel_lane == Channel::Show)
							key.ease = Ease::Step;

						clip.keys.push_back(key);
						++track.count;
					}

					if (track.count == 0)
						return fail(m_error, "track '{}' has no keys", target);
					clip.tracks.push_back(track);
				}
				return true;
			}

			[[nodiscard]] bool read_sprite_track(Clip& clip, u16 layer, StringView target, JsonValue keys)
			{
				std::erase_if(clip.sprite_tracks, [&](const SpriteTrack& t) { return t.layer == layer; });

				SpriteTrack track{.layer = layer, .first = static_cast<u32>(clip.sprite_keys.size())};
				for (const JsonValue element : keys.elements())
				{
					SpriteKey key;
					StringView sprite;
					if (!element[0u].read(key.ms) || !element[1u].read(sprite))
						return fail(m_error, "track '{}': sprite keys are [ms, \"sprite\"]", target);
					if (track.count > 0 && key.ms < clip.sprite_keys.back().ms)
						return fail(m_error, "track '{}': keys run in time order, and {} ms comes after {}", target,
									key.ms, clip.sprite_keys.back().ms);

					key.sprite = name(sprite);
					clip.sprite_keys.push_back(key);
					++track.count;
				}

				if (track.count == 0)
					return fail(m_error, "track '{}' has no keys", target);
				clip.sprite_tracks.push_back(track);
				return true;
			}

			Rig& m_rig;
			String& m_error;
		};
	}

	bool sheet_image(StringView text, String& path, String& error) noexcept
	{
		Json json;
		if (!read_json(text, json, error))
			return false;

		StringView image;
		if (!json.root()["image"].read(image))
			return fail(error, "a sheet names its \"image\"");

		path.assign(image.data(), image.size());
		return true;
	}

	bool parse_sheet(StringView text, const Image& image, Sheet& out, String& error) noexcept
	{
		Json json;
		if (!read_json(text, json, error))
			return false;

		const JsonValue root   = json.root();
		const glm::vec2 extent = {static_cast<f32>(image.extent.width), static_cast<f32>(image.extent.height)};
		const glm::vec2 cell   = glm::floor(read_vec2(root["cell"], extent));
		if (cell.x < 1.0f || cell.y < 1.0f || cell.x > extent.x || cell.y > extent.y)
			return fail(error, "a \"cell\" of {}x{} does not fit the {}x{} image", cell.x, cell.y, extent.x, extent.y);

		const u32 columns = image.extent.width / static_cast<u32>(cell.x);
		const u32 cells	  = columns * (image.extent.height / static_cast<u32>(cell.y));

		out.extent			  = image.extent;
		const glm::vec2 pivot = read_vec2(root["pivot"], cell * 0.5f);

		(void)root["shadow"].read(out.casts_shadow);

		for (const auto [point, at] : root["points"].members())
			out.points.push_back({name(point), read_vec2(at, {})});

		for (const auto [sequence, indices] : root["sprites"].members())
		{
			const u16 first = static_cast<u16>(out.sprites.size());

			for (const JsonValue element : indices.elements())
			{
				u32 index = cells;
				if (!element.read(index) || index >= cells)
					return fail(error, "sprite '{}' names cell {}, and the image has {} of {}x{}", sequence,
								element.read_or(-1), cells, cell.x, cell.y);

				const u32 frame = static_cast<u32>(out.sprites.size() - first);
				const u32 x		= index % columns * static_cast<u32>(cell.x);
				const u32 y		= index / columns * static_cast<u32>(cell.y);

				out.sprites.push_back({
					.min	= {static_cast<f32>(x), static_cast<f32>(y)},
					.size	= cell,
					.pivot	= pivot,
					.opaque = measure(image, x, y, static_cast<u32>(cell.x), static_cast<u32>(cell.y)),
				});

				// Frames are sequence_0, sequence_1... and the sequence's own name is its first.
				const u16 sprite	  = static_cast<u16>(out.sprites.size() - 1);
				const std::string key = fmt::format("{}_{}", sequence, frame);
				out.names.push_back({name(key), sprite});
				if (frame == 0)
					out.names.push_back({name(sequence), sprite});
			}

			out.sequences.push_back({name(sequence), {first, static_cast<u16>(out.sprites.size() - first)}});
		}

		return true;
	}

	bool parse_rig(StringView text, Rig& out, String& error) noexcept
	{
		Json json;
		if (!read_json(text, json, error))
			return false;

		return RigReader(out, error).read(json.root());
	}
}
