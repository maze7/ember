#include "internal.h"

#include <fmt/format.h>

#include <iterator>

/**
 * ember.d.luau, written from the binding: every component as an extern type with its fields, the
 * entity with a property per component and its verbs, the world with its typed queries, and every
 * library, global, enum and constant a pack installed, with the signatures they gave. luau-lsp reads
 * it, so a script that type checks in the editor is one the running game accepts.
 */
namespace ember::script
{
	namespace
	{
		[[nodiscard]] const char* luau_type(FieldKind kind) noexcept
		{
			switch (kind)
			{
				case FieldKind::Bool:
					return "boolean";
				case FieldKind::Int:
				case FieldKind::Float:
				case FieldKind::Enum:
				case FieldKind::Layers:
					return "number";
				case FieldKind::Vec2:
				case FieldKind::Vec2i8:
					return "vector";
				case FieldKind::Shape:
					return "Shape";
				case FieldKind::Text:
					return "string";
				case FieldKind::EntityRef:
					return "Entity?";
				case FieldKind::Name:
					return "Name";
				default:
					return "any";
			}
		}

		[[nodiscard]] StringView trim(StringView text) noexcept
		{
			while (!text.empty() && text.front() == ' ')
				text.remove_prefix(1);
			while (!text.empty() && text.back() == ' ')
				text.remove_suffix(1);
			return text;
		}

		/** "(a: T, b: U) -> R" into "a: T, b: U" and "R"; a signature with no arrow returns "()". */
		void split_signature(StringView signature, StringView& params, StringView& ret) noexcept
		{
			signature		   = trim(signature);
			const size_t arrow = signature.rfind("->");
			StringView head	   = arrow == StringView::npos ? signature : trim(signature.substr(0, arrow));
			ret				   = arrow == StringView::npos ? StringView("()") : trim(signature.substr(arrow + 2));

			if (!head.empty() && head.front() == '(' && head.back() == ')')
				head = head.substr(1, head.size() - 2);
			params = trim(head);
		}

		template <class... Args> void line(String& out, fmt::format_string<Args...> format, Args&&... args)
		{
			fmt::format_to(std::back_inserter(out), format, std::forward<Args>(args)...);
			out += '\n';
		}

		/** A method of an extern type: "function name(self, params): ret". */
		void method_line(String& out, StringView name, StringView signature)
		{
			StringView params, ret;
			split_signature(signature, params, ret);
			if (params.empty())
				line(out, "\tfunction {}(self): {}", name, ret);
			else
				line(out, "\tfunction {}(self, {}): {}", name, params, ret);
		}
	}

	void write_definitions_text(const Binding& binding, String& out)
	{
		out += "--!strict\n"
			   "-- ember.d.luau: what this game's scripts may use, written by the engine from its binding when the\n"
			   "-- game starts. Point luau-lsp at it and leave it be: the next start writes it again.\n\n";

		out += "export type Component<T> = { name: string, __phantom: T }\n\n";
		out += "declare extern type Filter with\nend\n\n";
		out += "--- A name as the engine keeps it: a text's hash. Made by name(\"idle\"), read from a Name field, and\n"
			   "--- taken wherever a string is: play(), spawn(), sound. Compare it with ==; print it to see its text.\n"
			   "declare extern type Name with\nend\n\n";

		out += "declare extern type Shape with\n"
			   "\tkind: number\n"
			   "\tcenter: vector\n"
			   "\thalf: vector\n"
			   "\taxis: vector\n"
			   "\tfunction turned(self, direction: vector): Shape\n"
			   "\tfunction at(self, position: vector): Shape\n"
			   "end\n\n";
		out += "declare shape: {\n"
			   "\tbox: (size: vector, center: vector?) -> Shape,\n"
			   "\tcircle: (radius: number, center: vector?) -> Shape,\n"
			   "}\n\n";
		out += "declare function layers(...: number): number\n\n";

		// The dice an entity rolls with, the declarations a module makes and the units their values take.
		out += "declare extern type Rng with\n"
			   "\tfunction range(self, n: number): number\n"
			   "\tfunction chance(self, p: number): boolean\n"
			   "\tfunction float(self): number\n"
			   "end\n\n";
		out +=
			"--- A cue as a show handler gets it: an event raised on the entity, drawn at the moment it happened.\n"
			"export type Cue = { name: string, tick: number, by: Shown?, n: number, late: number }\n"
			"export type EventArgs = { by: Entity?, n: number? }\n"
			"--- An event as a story's reaction or wait_for gets it.\n"
			"export type Event = { name: string, tick: number, by: Entity?, n: number }\n"
			"--- A story: run reads top to bottom and waits where it says (wait, wait_until, wait_for); on are "
			"reactions,\n"
			"--- and one that returns \"restart\" starts run over. prefab attaches it to every entity of that prefab.\n"
			"--- A story of reactions alone leaves run out.\n"
			"export type StoryDef = {\n"
			"\tprefab: string?,\n"
			"\tstage: string?,\n"
			"\tloop: boolean?,\n"
			"\trun: ((e: Entity) -> ())?,\n"
			"\ton: { [string]: (e: Entity, ...Event) -> ...any }?,\n"
			"}\n"
			"--- A modifier on an entity, as its tick and e:modifier() see it. left is ticks; 0 for one that lasts.\n"
			"export type ModifierState = { name: string, stacks: number, power: number, left: number }\n"
			"--- What a modifier does to a stat, per stack: adds first, then multiplies.\n"
			"export type StatRow = { add: number?, mul: number? }\n"
			"--- What lasts on an entity: e:inflict() puts it on, e:cure() takes it off, and every machine that "
			"simulates\n"
			"--- the entity counts it down. tick runs every `every` ticks where its directory says; stats change what\n"
			"--- e:stat() and the game's Host::stat() read; show is the client's, entered and left as it comes and "
			"goes.\n"
			"export type ModifierDef = {\n"
			"\tlasts: number?, -- ticks; none lasts until cured\n"
			"\tstacking: (\"refresh\" | \"add\" | \"keep\")?,\n"
			"\tmax_stacks: number?,\n"
			"\tpower: number?, -- what e:inflict() gives when it says none\n"
			"\tevery: number?,\n"
			"\tstage: string?,\n"
			"\ttick: ((e: Entity, m: ModifierState) -> ())?,\n"
			"\tstats: { [string]: StatRow }?,\n"
			"\tshow: ShowDef?,\n"
			"}\n"
			"export type InflictArgs = { ticks: number?, stacks: number?, power: number? }\n"
			"--- A client's debug UI: the game draws it in a window of its own and runs frame inside, once a frame,\n"
			"--- handing it state, a table whose values a reload keeps.\n"
			"export type PanelDef<S> = { state: S?, frame: (state: S) -> () }\n"
			"--- What clients show: at a state's entry and exit, every frame, and at its marks and the cues raised on\n"
			"--- the entity, by name. A handler sees the entity as a Shown: the client's verbs, none of the game's.\n"
			"export type ShowDef = {\n"
			"\tenter: ((e: Shown) -> ())?,\n"
			"\texit: ((e: Shown) -> ())?,\n"
			"\tupdate: ((e: Shown) -> ())?,\n"
			"\t[string]: (e: Shown, ...Cue) -> (),\n"
			"}\n\n";
		out +=
			"export type Kind = \"Sim\" | \"Server\" | \"Client\" | \"Replicated\" | \"Interpolated\" | \"Predicted\" "
			"| \"OwnerOnly\"\n"
			"export type ComponentDef = { kind: (Kind | { Kind })?, [string]: any }\n"
			"export type PrefabDef = {\n"
			"\textends: string?,\n"
			"\tgroups: { [string]: { [string]: any } }?,\n"
			"\tstart: { string }?,\n"
			"\tevents: { [string]: { add: { string }?, remove: { string }? } }?,\n"
			"\t[string]: any,\n"
			"}\n"
			"--- Marks by name: each one's tick from the state's start.\n"
			"export type Marks = { [string]: number }\n"
			"--- One state. A handler names the next state by returning it, or returns nothing. S is the graph's "
			"state\n"
			"--- names when its handle is annotated Graph<State>: a misspelt name is then a type error.\n"
			"export type StateDef<S> = {\n"
			"\tenter: ((e: Entity) -> ...S?)?,\n"
			"\tupdate: ((e: Entity) -> ...S?)?,\n"
			"\tevery: number?, -- update every this many ticks\n"
			"\texit: ((e: Entity) -> ())?,\n"
			"\tmarks: (Marks | (e: Entity) -> Marks)?, -- or worked out from the entity as it enters\n"
			"\ton: { [string]: (e: Entity, ...Event) -> ...S? }?, -- marks and events alike; to \"state\" moves on\n"
			"\tignore: { string }?, -- events the graph answers that this state does not\n"
			"\tlength: (number | string | (e: Entity) -> number)?, -- ticks, a mark's name, or worked out\n"
			"\tnext: (S | (e: Entity) -> ...S?)?,\n"
			"\tshow: ShowDef?,\n"
			"\t-- The first form, still read: timeline = { [tick] = \"mark\" }, events = { event = \"state\" }, "
			"react.\n"
			"\ttimeline: { [number]: string }?,\n"
			"\tevents: { [string]: string }?,\n"
			"\treact: { [string]: (e: Entity, source: Entity?) -> ...any }?,\n"
			"}\n"
			"--- A graph's handle: `g.state \"name\" { ... }` declares a state; in a graph that extends another, it "
			"changes\n"
			"--- the inherited state key by key.\n"
			"export type Graph<S> = {\n"
			"\tname: string,\n"
			"\tstate: (name: S) -> (def: StateDef<S>) -> (),\n"
			"}\n"
			"--- What holds in every state, where the graph starts, and what it extends.\n"
			"export type GraphDef = {\n"
			"\tinitial: string?,\n"
			"\textends: string?,\n"
			"\tstage: string?,\n"
			"\ton: { [string]: (e: Entity, ...Event) -> ...any }?, -- in every state that neither answers nor ignores "
			"it\n"
			"\tupdate: ((e: Entity) -> ...any)?, -- every tick in every state, after the state's own\n"
			"\tevery: number?,\n"
			"\tshow: ShowDef?,\n"
			"\tstates: { [string]: StateDef<string> }?, -- the first form\n"
			"}\n"
			"--- A hit window: the entity's hitbox hits these layers from now, for so many ticks or until a mark, "
			"never\n"
			"--- past the state; once, it goes off after its first hit. From the entity's own graph's handlers.\n"
			"export type StrikeDef = { hits: number, shape: Shape?, ticks: number?, to: string?, once: boolean? }\n\n";
		out += "declare function component(name: string): (def: ComponentDef) -> ()\n"
			   "declare function prefab(name: string): (def: PrefabDef) -> ()\n"
			   "declare function stategraph(name: string): (def: GraphDef) -> Graph<any>\n"
			   "--- A handler that moves on: on = { hurt = to \"stunned\" }. The loader checks the name.\n"
			   "declare function to(state: string): (e: Entity, ...Event) -> ...any\n"
			   "declare function show(prefab: string): (def: ShowDef) -> ()\n"
			   "declare function story(name: string): (def: StoryDef) -> ()\n"
			   "declare function modifier(name: string): (def: ModifierDef) -> ()\n"
			   "declare function panel(name: string): <S>(def: PanelDef<S>) -> ()\n"
			   "declare function wait(ticks: number): ()\n"
			   "declare function wait_until<T>(check: () -> T?, every: number?): T\n"
			   "declare function wait_for(event: string): Event\n"
			   "--- The entity as a client shows it: for a client story that makes it look or sound like something.\n"
			   "declare function shown(e: Entity): Shown\n"
			   "declare function tiles(x: number): number\n"
			   "declare function ticks(n: number): number\n"
			   "declare function seconds(s: number): number\n"
			   "declare function count(n: number): number\n"
			   "declare function tick(t: number?): number\n"
			   "declare function int(n: number): number\n"
			   "declare function name(text: string): Name\n"
			   "declare function entity(): Entity?\n\n";

		for (const String& text : binding.extra_definitions())
		{
			out += text;
			out += "\n\n";
		}

		// Components: an extern type each, and the value of that name a query takes.
		for (const Exposed& exposed : binding.exposures())
		{
			if (exposed.info == nullptr)
				continue;

			const StringView name = exposed.info->name;
			line(out, "declare extern type {} with", name);
			for (const Field& field : exposed.fields)
				line(out, "\t{}: {}", field.name, luau_type(field.kind));
			for (const Method& method : exposed.methods)
				method_line(out, method.name, method.signature);
			out += "end\n";
			line(out, "declare {}: Component<{}>\n", name, name);
		}

		// Components as properties, typed as present: a script written for a prefab reads them straight; one that
		// is not sure asks has(), or tests the property, which is nil at run time when the entity lacks it.
		out += "declare extern type Entity with\n";
		for (const Exposed& exposed : binding.exposures())
			if (exposed.info != nullptr)
				line(out, "\t{}: {}", exposed.info->name, exposed.info->name);
		out += "\trng: Rng\n"
			   "\tfunction has(self, component: Component<any>): boolean\n"
			   "\tfunction add(self, component: Component<any>, fields: { [string]: any }?): ()\n"
			   "\tfunction remove(self, component: Component<any>): ()\n"
			   "\tfunction destroy(self): ()\n"
			   "\tfunction id(self): number\n"
			   "\tfunction play(self, clip: string | Name): ()\n"
			   "\tfunction event(self, name: string, args: (Entity | EventArgs)?): ()\n"
			   "\tfunction start(self, story: string): () -- runs the story on it, from the top\n"
			   "\tfunction stop(self, story: string?): () -- that story, or every one it was started on\n"
			   "\tfunction exists(self): boolean -- false once the entity is gone: the one question a kept handle "
			   "answers\n"
			   "\tfunction strike(self, def: StrikeDef): ()\n"
			   "\tfunction inflict(self, modifier: string, args: InflictArgs?): () -- again: as its stacking says\n"
			   "\tfunction cure(self, modifier: string?): () -- that one, or every one\n"
			   "\tfunction modifier(self, modifier: string): ModifierState? -- nil when it has none\n"
			   "\tfunction stat(self, stat: string, base: number?): number -- base as its modifiers change it\n"
			   "\tfunction prefab(self): string? -- what it was made from\n"
			   "\tstate: Stategraph -- its graph's state; in its graph's handlers, the slot being stepped\n";
		for (const Function& method : binding.entity_methods())
			if (has_any(method.where, ContextMask::Sim | ContextMask::Server))
				method_line(out, method.name, method.signature);
		out += "end\n\n";

		// The entity as a client shows it: read it, and make it look and sound like something.
		out += "--- The entity as a show handler sees it: its components to read, and the client's verbs on it.\n"
			   "declare extern type Shown with\n";
		for (const Exposed& exposed : binding.exposures())
			if (exposed.info != nullptr)
				line(out, "\t{}: {}", exposed.info->name, exposed.info->name);
		out += "\tfunction has(self, component: Component<any>): boolean\n"
			   "\tfunction id(self): number\n"
			   "\tfunction exists(self): boolean\n"
			   "\tfunction mine(self): boolean -- this client's own player: drawn ahead, predicted here\n"
			   "\tfunction flash(self, seconds: number): () -- the rig's flash overlay, from the handler's moment\n"
			   "\tfunction overlay(self, clip: string, seconds: number?): ()\n"
			   "\tfunction squash(self, x: number, y: number, seconds: number): ()\n"
			   "\tfunction lean(self, radians: number): () -- this frame; say it every frame\n"
			   "\tfunction hold(self, seconds: number): () -- a hitstop from the handler's moment\n"
			   "\tfunction sound(self, event: string, params: { [string]: number }?): () -- heard once, at the moment\n"
			   "\tfunction play(self, clip: string | Name): () -- its Animator, from the handler's moment\n"
			   "\tfunction modifier(self, modifier: string): ModifierState?\n"
			   "\tfunction stat(self, stat: string, base: number?): number\n"
			   "\tfunction prefab(self): string?\n";
		for (const Function& method : binding.entity_methods())
			if (has_any(method.where, ContextMask::Client))
				method_line(out, method.name, method.signature);
		out += "end\n\n";

		// The world: a query overload per arity, so the loop's variables are the components named.
		constexpr const char* LETTERS[] = {"A", "B", "C", "D", "E", "F"};
		out += "export type World = {\n\tquery: ";
		for (size_t arity = 1; arity <= std::size(LETTERS); ++arity)
		{
			if (arity > 1)
				out += "\n\t\t& ";
			out += "(<";
			for (size_t i = 0; i < arity; ++i)
				fmt::format_to(std::back_inserter(out), "{}{}", i > 0 ? ", " : "", LETTERS[i]);
			out += ">(self: World";
			for (size_t i = 0; i < arity; ++i)
				fmt::format_to(std::back_inserter(out), ", {}: Component<{}>", static_cast<char>('a' + i), LETTERS[i]);
			out += ", ...Filter) -> () -> (Entity";
			for (size_t i = 0; i < arity; ++i)
				fmt::format_to(std::back_inserter(out), ", {}", LETTERS[i]);
			out += "))";
		}
		out += ",\n\tspawn: (self: World, prefab: string, overrides: { [string]: { [string]: any } }?) -> Entity,\n";
		for (const Function& function : binding.world_functions())
		{
			StringView params, ret;
			split_signature(function.signature, params, ret);
			if (params.empty())
				line(out, "\t{}: (self: World) -> {},", function.name, ret);
			else
				line(out, "\t{}: (self: World, {}) -> {},", function.name, params, ret);
		}
		out += "}\n"
			   "declare world: World\n\n";
		out += "declare function without(...: Component<any>): Filter\n";

		out += "declare function system(stage: ";
		for (const StageName& stage : binding.stages())
			fmt::format_to(std::back_inserter(out), "\"{}\" | ", stage.name);
		out += "\"Present\", fn: () -> ()): ()\n";
		out += "declare function now(): number\n\n";

		for (const Library& library : binding.libraries())
		{
			line(out, "declare {}: {{", library.name);
			for (const Function& function : library.functions)
				line(out, "\t{}: {},", function.name, function.signature);
			out += "}\n\n";
		}

		for (const Function& function : binding.globals())
		{
			StringView params, ret;
			split_signature(function.signature, params, ret);
			line(out, "declare function {}({}): {}", function.name, params, ret);
		}
		if (!binding.globals().empty())
			out += '\n';

		for (const Enumeration& enumeration : binding.enumerations())
		{
			line(out, "declare {}: {{", enumeration.name);
			for (const Enumeration::Value& value : enumeration.values)
				line(out, "\t{}: number,", value.name);
			out += "}\n\n";
		}

		for (const Constant& constant : binding.constants())
			line(out, "declare {}: number", constant.name);
	}
}
