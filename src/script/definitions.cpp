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
		out += "export type Kind = \"Sim\" | \"Server\" | \"Client\" | \"Replicated\" | \"Interpolated\" | \"Predicted\" | \"OwnerOnly\"\n"
			   "export type ComponentDef = { kind: (Kind | { Kind })?, [string]: any }\n"
			   "export type PrefabDef = {\n"
			   "\textends: string?,\n"
			   "\tgroups: { [string]: { [string]: any } }?,\n"
			   "\tstart: { string }?,\n"
			   "\tevents: { [string]: { add: { string }?, remove: { string }? } }?,\n"
			   "\t[string]: any,\n"
			   "}\n"
			   "-- A handler names the next state by returning it, or returns nothing: hence the packs.\n"
			   "export type StateDef = {\n"
			   "\tevery: number?,\n"
			   "\tenter: ((e: Entity) -> ...any)?,\n"
			   "\tupdate: ((e: Entity) -> ...any)?,\n"
			   "\texit: ((e: Entity) -> ())?,\n"
			   "\ttimeline: { [number]: string }?,\n"
			   "\ton: { [string]: (e: Entity) -> () }?,\n"
			   "\tlength: number?,\n"
			   "\tnext: (string | (e: Entity) -> ...any)?,\n"
			   "\tevents: { [string]: string }?,\n"
			   "\treact: { [string]: (e: Entity, source: Entity?) -> ...any }?,\n"
			   "}\n"
			   "export type StategraphDef = { initial: string, stage: string?, states: { [string]: StateDef } }\n\n";
		out += "declare function component(name: string): (def: ComponentDef) -> ()\n"
			   "declare function prefab(name: string): (def: PrefabDef) -> ()\n"
			   "declare function stategraph(name: string): (def: StategraphDef) -> ()\n"
			   "declare function tiles(x: number): number\n"
			   "declare function ticks(n: number): number\n"
			   "declare function seconds(s: number): number\n"
			   "declare function count(n: number): number\n"
			   "declare function tick(t: number?): number\n"
			   "declare function int(n: number): number\n\n";

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
			   "\tfunction play(self, clip: string): ()\n"
			   "\tfunction event(self, name: string, source: Entity?): ()\n";
		for (const Function& method : binding.entity_methods())
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
		out += ",\n\tspawn: (self: World, prefab: string, overrides: { [string]: { [string]: any } }?) -> (),\n";
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
