/**
 * hsm_resource.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#include "hsm_resource.h"

#include "../compat/resource.h"

#ifdef LIMBOAI_MODULE
#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#endif // LIMBOAI_GDEXTENSION

//**** HSMTransition

void HSMTransition::set_from(const Ref<HSMStateSpec> &p_state) {
	from = p_state;
	emit_changed();
}

Ref<HSMStateSpec> HSMTransition::get_from() const {
	return from;
}

void HSMTransition::set_to(const Ref<HSMStateSpec> &p_state) {
	to = p_state;
	emit_changed();
}

Ref<HSMStateSpec> HSMTransition::get_to() const {
	return to;
}

void HSMTransition::set_event(const StringName &p_event) {
	event = p_event;
	emit_changed();
}

void HSMTransition::set_note(const String &p_note) {
	note = p_note;
	emit_changed();
}

void HSMTransition::set_condition(const Ref<HSMCondition> &p_condition) {
	condition = p_condition;
	emit_changed();
}

void HSMTransition::set_priority(int p_priority) {
	priority = p_priority;
	emit_changed();
}

void HSMTransition::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_from", "state"), &HSMTransition::set_from);
	ClassDB::bind_method(D_METHOD("get_from"), &HSMTransition::get_from);
	ClassDB::bind_method(D_METHOD("set_to", "state"), &HSMTransition::set_to);
	ClassDB::bind_method(D_METHOD("get_to"), &HSMTransition::get_to);
	ClassDB::bind_method(D_METHOD("set_event", "event"), &HSMTransition::set_event);
	ClassDB::bind_method(D_METHOD("get_event"), &HSMTransition::get_event);
	ClassDB::bind_method(D_METHOD("set_note", "note"), &HSMTransition::set_note);
	ClassDB::bind_method(D_METHOD("get_note"), &HSMTransition::get_note);
	ClassDB::bind_method(D_METHOD("set_condition", "condition"), &HSMTransition::set_condition);
	ClassDB::bind_method(D_METHOD("get_condition"), &HSMTransition::get_condition);
	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &HSMTransition::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &HSMTransition::get_priority);
	ClassDB::bind_method(D_METHOD("is_polled"), &HSMTransition::is_polled);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "from", PROPERTY_HINT_RESOURCE_TYPE, "HSMStateSpec"), "set_from", "get_from");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "to", PROPERTY_HINT_RESOURCE_TYPE, "HSMStateSpec"), "set_to", "get_to");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "event"), "set_event", "get_event");
	// Before `note` so the Inspector reads in the order the row is decided: where to, on what
	// signal, under which condition, how urgently. The note explains, it does not decide.
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "condition", PROPERTY_HINT_RESOURCE_TYPE, "HSMCondition"), "set_condition", "get_condition");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "priority"), "set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "note", PROPERTY_HINT_MULTILINE_TEXT), "set_note", "get_note");
}

HSMTransition::HSMTransition() {
}

HSMTransition::~HSMTransition() {
}

//**** HSMStateSpec

void HSMStateSpec::set_state_name(const StringName &p_name) {
	state_name = p_name;
	emit_changed();
}

void HSMStateSpec::set_state_script(const Ref<Script> &p_script) {
	state_script = p_script;
	emit_changed();
	// The `state_props/` entries the Inspector shows are the script's exports, so they change
	// with it.
	notify_property_list_changed();
}

void HSMStateSpec::set_state_properties(const Dictionary &p_properties) {
	state_properties = p_properties;
	emit_changed();
}

namespace {

const char *STATE_PROPS_PREFIX = "state_props/";

// The script's exported properties: the ones a person can set in an Inspector and that a
// file would store. Groups and categories are layout, not properties, and are left out.
Vector<PropertyInfo> _script_exports(const Ref<Script> &p_script) {
	Vector<PropertyInfo> out;
	if (p_script.is_null()) {
		return out;
	}
#ifdef LIMBOAI_MODULE
	List<PropertyInfo> props;
	p_script->get_script_property_list(&props);
	for (const PropertyInfo &prop : props) {
#elif LIMBOAI_GDEXTENSION
	TypedArray<Dictionary> props = p_script->get_script_property_list();
	for (int i = 0; i < props.size(); i++) {
		PropertyInfo prop = PropertyInfo::from_dict(props[i]);
#endif
		if (prop.usage & (PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP)) {
			continue;
		}
		if (!(prop.usage & PROPERTY_USAGE_STORAGE) || !(prop.usage & PROPERTY_USAGE_EDITOR)) {
			continue;
		}
		out.push_back(prop);
	}
	return out;
}

Variant _script_default(const Ref<Script> &p_script, const StringName &p_name) {
	if (p_script.is_null()) {
		return Variant();
	}
#ifdef LIMBOAI_MODULE
	Variant ret;
	p_script->get_property_default_value(p_name, ret);
	return ret;
#elif LIMBOAI_GDEXTENSION
	return p_script->get_property_default_value(p_name);
#endif
}

// "state_props/max_suspend" -> "max_suspend", or empty when the name is not one of ours.
String _state_prop_name(const StringName &p_name) {
	String name = p_name;
	if (!name.begins_with(STATE_PROPS_PREFIX)) {
		return String();
	}
	return name.substr(String(STATE_PROPS_PREFIX).length());
}

bool _has_export(const Ref<Script> &p_script, const String &p_name) {
	Vector<PropertyInfo> exports = _script_exports(p_script);
	for (int i = 0; i < exports.size(); i++) {
		if (String(exports[i].name) == p_name) {
			return true;
		}
	}
	return false;
}

} // namespace

bool HSMStateSpec::_set(const StringName &p_name, const Variant &p_value) {
	String prop = _state_prop_name(p_name);
	if (prop.is_empty() || state_script.is_null()) {
		return false;
	}
	// Equal to the script default means "not overridden": the key goes, which is also what
	// the Inspector's revert arrow amounts to. A file then only holds the values that differ.
	Dictionary props = state_properties.duplicate();
	// Keys are written as String. A hand-written StringName key for the same property is
	// dropped so the dictionary never holds two values for one property.
	props.erase(StringName(prop));
	if (p_value == _script_default(state_script, prop)) {
		props.erase(prop);
	} else {
		props[prop] = p_value;
	}
	set_state_properties(props);
	return true;
}

bool HSMStateSpec::_get(const StringName &p_name, Variant &r_ret) const {
	String prop = _state_prop_name(p_name);
	if (prop.is_empty() || state_script.is_null()) {
		return false;
	}
	if (state_properties.has(prop)) {
		r_ret = state_properties[prop];
	} else if (state_properties.has(StringName(prop))) {
		r_ret = state_properties[StringName(prop)];
	} else {
		r_ret = _script_default(state_script, prop);
	}
	return true;
}

void HSMStateSpec::_get_property_list(List<PropertyInfo> *p_list) const {
	// Only for the Inspector. At runtime nothing reads these, and listing a script's exports
	// is not free.
	if (state_script.is_null() || !Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	Vector<PropertyInfo> exports = _script_exports(state_script);
	for (int i = 0; i < exports.size(); i++) {
		PropertyInfo prop = exports[i];
		prop.name = String(STATE_PROPS_PREFIX) + String(prop.name);
		// Shown and edited, never stored on its own: `state_properties` is the stored form.
		prop.usage = PROPERTY_USAGE_EDITOR;
		p_list->push_back(prop);
	}
}

bool HSMStateSpec::_property_can_revert(const StringName &p_name) const {
	String prop = _state_prop_name(p_name);
	return !prop.is_empty() && state_script.is_valid() && (state_properties.has(prop) || state_properties.has(StringName(prop)));
}

bool HSMStateSpec::_property_get_revert(const StringName &p_name, Variant &r_property) const {
	String prop = _state_prop_name(p_name);
	if (prop.is_empty() || state_script.is_null() || !_has_export(state_script, prop)) {
		return false;
	}
	r_property = _script_default(state_script, prop);
	return true;
}

void HSMStateSpec::set_behavior_tree(const Ref<BehaviorTree> &p_tree) {
	behavior_tree = p_tree;
	emit_changed();
}

void HSMStateSpec::set_children(const TypedArray<HSMStateSpec> &p_children) {
	children = p_children;
	emit_changed();
}

void HSMStateSpec::set_initial_child(const Ref<HSMStateSpec> &p_state) {
	initial_child = p_state;
	emit_changed();
}

Ref<HSMStateSpec> HSMStateSpec::get_initial_child() const {
	// Falling back to the first child keeps "a machine always has somewhere to start"
	// out of every caller. LimboHSM would otherwise refuse to activate.
	if (initial_child.is_valid()) {
		return initial_child;
	}
	if (children.size() > 0) {
		return children[0];
	}
	return nullptr;
}

void HSMStateSpec::set_transitions(const TypedArray<HSMTransition> &p_transitions) {
	transitions = p_transitions;
	emit_changed();
}

void HSMStateSpec::set_graph_position(const Vector2 &p_position) {
	graph_position = p_position;
	emit_changed();
}

void HSMStateSpec::set_graph_size(const Vector2 &p_size) {
	graph_size = p_size;
	emit_changed();
}

void HSMStateSpec::set_anystate_graph_position(const Vector2 &p_position) {
	anystate_graph_position = p_position;
	emit_changed();
}

StringName HSMStateSpec::resolve_base_type() const {
	if (state_script.is_valid()) {
		StringName base = state_script->get_instance_base_type();
		if (base == StringName()) {
			// Same reload dance the task editor does: a script that has never been
			// compiled answers with nothing.
			state_script->reload(true);
			base = state_script->get_instance_base_type();
		}
		if (base != StringName()) {
			return base;
		}
	}
	if (children.size() > 0) {
		return "LimboHSM";
	}
	if (behavior_tree.is_valid()) {
		return "BTState";
	}
	return "LimboState";
}

void HSMStateSpec::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_state_name", "name"), &HSMStateSpec::set_state_name);
	ClassDB::bind_method(D_METHOD("get_state_name"), &HSMStateSpec::get_state_name);
	ClassDB::bind_method(D_METHOD("set_state_script", "script"), &HSMStateSpec::set_state_script);
	ClassDB::bind_method(D_METHOD("get_state_script"), &HSMStateSpec::get_state_script);
	ClassDB::bind_method(D_METHOD("set_state_properties", "properties"), &HSMStateSpec::set_state_properties);
	ClassDB::bind_method(D_METHOD("get_state_properties"), &HSMStateSpec::get_state_properties);
	ClassDB::bind_method(D_METHOD("set_behavior_tree", "behavior_tree"), &HSMStateSpec::set_behavior_tree);
	ClassDB::bind_method(D_METHOD("get_behavior_tree"), &HSMStateSpec::get_behavior_tree);
	ClassDB::bind_method(D_METHOD("set_children", "children"), &HSMStateSpec::set_children);
	ClassDB::bind_method(D_METHOD("get_children"), &HSMStateSpec::get_children);
	ClassDB::bind_method(D_METHOD("set_initial_child", "state"), &HSMStateSpec::set_initial_child);
	ClassDB::bind_method(D_METHOD("get_initial_child"), &HSMStateSpec::get_initial_child);
	ClassDB::bind_method(D_METHOD("set_transitions", "transitions"), &HSMStateSpec::set_transitions);
	ClassDB::bind_method(D_METHOD("get_transitions"), &HSMStateSpec::get_transitions);
	ClassDB::bind_method(D_METHOD("set_graph_position", "position"), &HSMStateSpec::set_graph_position);
	ClassDB::bind_method(D_METHOD("get_graph_position"), &HSMStateSpec::get_graph_position);
	ClassDB::bind_method(D_METHOD("set_graph_size", "size"), &HSMStateSpec::set_graph_size);
	ClassDB::bind_method(D_METHOD("get_graph_size"), &HSMStateSpec::get_graph_size);
	ClassDB::bind_method(D_METHOD("set_anystate_graph_position", "position"), &HSMStateSpec::set_anystate_graph_position);
	ClassDB::bind_method(D_METHOD("get_anystate_graph_position"), &HSMStateSpec::get_anystate_graph_position);
	ClassDB::bind_method(D_METHOD("is_machine"), &HSMStateSpec::is_machine);
	ClassDB::bind_method(D_METHOD("resolve_base_type"), &HSMStateSpec::resolve_base_type);
	ClassDB::bind_method(D_METHOD("get_declared_initial_child"), &HSMStateSpec::get_declared_initial_child);
	// Neither is a property: both only mean something on the tree get_effective_root()
	// builds, and a merged tree is never the thing that gets written to a file.
	ClassDB::bind_method(D_METHOD("get_origin"), &HSMStateSpec::get_origin);
	ClassDB::bind_method(D_METHOD("is_inherited"), &HSMStateSpec::is_inherited);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "state_name"), "set_state_name", "get_state_name");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "state_script", PROPERTY_HINT_RESOURCE_TYPE, "Script"), "set_state_script", "get_state_script");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "state_properties"), "set_state_properties", "get_state_properties");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "behavior_tree", PROPERTY_HINT_RESOURCE_TYPE, "BehaviorTree"), "set_behavior_tree", "get_behavior_tree");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "children", PROPERTY_HINT_ARRAY_TYPE, RESOURCE_TYPE_HINT("HSMStateSpec")), "set_children", "get_children");
	// Read through the declared value, not get_initial_child(): the fall back to the first
	// child is an answer for whoever runs the machine, and writing it into a file would
	// turn "this file says nothing" into "this file says the first one" the moment it is
	// saved -- which is exactly what an inheriting file must be able to stay silent about.
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "initial_child", PROPERTY_HINT_RESOURCE_TYPE, "HSMStateSpec"), "set_initial_child", "get_declared_initial_child");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "transitions", PROPERTY_HINT_ARRAY_TYPE, RESOURCE_TYPE_HINT("HSMTransition")), "set_transitions", "get_transitions");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "graph_position"), "set_graph_position", "get_graph_position");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "graph_size"), "set_graph_size", "get_graph_size");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "anystate_graph_position"), "set_anystate_graph_position", "get_anystate_graph_position");
}

HSMStateSpec::HSMStateSpec() {
}

HSMStateSpec::~HSMStateSpec() {
}

//**** HierarchicalStateMachine

void HierarchicalStateMachine::set_description(const String &p_value) {
	description = p_value;
	emit_changed();
}

void HierarchicalStateMachine::set_root(const Ref<HSMStateSpec> &p_state) {
	root = p_state;
	emit_changed();
}

void HierarchicalStateMachine::set_base(const Ref<HierarchicalStateMachine> &p_base) {
	base = p_base;
	emit_changed();
}

void HierarchicalStateMachine::set_blackboard_plan(const Ref<BlackboardPlan> &p_plan) {
	blackboard_plan = p_plan;
	emit_changed();
}

Ref<BlackboardPlan> HierarchicalStateMachine::get_effective_blackboard_plan() const {
	if (blackboard_plan.is_valid()) {
		return blackboard_plan;
	}
	// Bounded walk: a `base` cycle is reported by get_effective_root(), not here.
	Ref<HierarchicalStateMachine> node = base;
	for (int depth = 0; node.is_valid() && depth < 64; depth++) {
		if (node->get_blackboard_plan().is_valid()) {
			return node->get_blackboard_plan();
		}
		node = node->get_base();
	}
	return Ref<BlackboardPlan>();
}

namespace {

// One drawn/built state and the authored state it came from. Kept in visit order so that
// transitions land in that order too: the base's rows come first, and what the child adds
// is layered on top -- which is the shape of inheritance, not an accident of iteration.
struct MergePair {
	Ref<HSMStateSpec> merged;
	Ref<HSMStateSpec> authored;
};

// Copies the values of one state. Children, transitions and initial_child are left out:
// they are references, and references are rebuilt afterwards through the identity map.
Ref<HSMStateSpec> _copy_values(const Ref<HSMStateSpec> &p_src) {
	Ref<HSMStateSpec> out;
	out.instantiate();
	out->set_state_name(p_src->get_state_name());
	out->set_state_script(p_src->get_state_script());
	// A copy, so that overlaying a child file's values never writes into the base file.
	out->set_state_properties(p_src->get_state_properties().duplicate());
	out->set_behavior_tree(p_src->get_behavior_tree());
	out->set_graph_position(p_src->get_graph_position());
	out->set_graph_size(p_src->get_graph_size());
	out->set_anystate_graph_position(p_src->get_anystate_graph_position());
	return out;
}

// Clones `p_src` and everything under it onto `p_dst_parent`.
void _graft(const Ref<HSMStateSpec> &p_src, const Ref<HSMStateSpec> &p_dst_parent, bool p_inherited,
		HashMap<uint64_t, Ref<HSMStateSpec>> &r_map, Vector<MergePair> &r_pairs) {
	if (p_src.is_null() || r_map.has(p_src->get_instance_id())) {
		return;
	}
	Ref<HSMStateSpec> copy = _copy_values(p_src);
	copy->set_inherited(p_inherited);
	if (!p_inherited) {
		copy->set_origin(p_src);
	}
	r_map[p_src->get_instance_id()] = copy;
	r_pairs.push_back(MergePair{ copy, p_src });

	if (p_dst_parent.is_valid()) {
		TypedArray<HSMStateSpec> siblings = p_dst_parent->get_children();
		siblings.append(copy);
		p_dst_parent->set_children(siblings);
	}

	TypedArray<HSMStateSpec> children = p_src->get_children();
	for (int i = 0; i < children.size(); i++) {
		_graft(children[i], copy, p_inherited, r_map, r_pairs);
	}
}

// Lays what the child file says about an inherited state on top of the copy of it.
void _overlay(const Ref<HSMStateSpec> &p_dst, const Ref<HSMStateSpec> &p_src,
		HashMap<uint64_t, Ref<HSMStateSpec>> &r_map, Vector<MergePair> &r_pairs) {
	if (p_dst.is_null() || p_src.is_null() || r_map.has(p_src->get_instance_id())) {
		return;
	}
	r_map[p_src->get_instance_id()] = p_dst;
	r_pairs.push_back(MergePair{ p_dst, p_src });
	// This is where an edit to the overlaid state has to go, even though the state itself
	// came from the base file.
	p_dst->set_origin(p_src);

	// Only what the child actually said overrides. An empty slot is silence, not "clear it":
	// a state named only to reach the ones under it must not blank the base's script.
	if (p_src->get_state_script().is_valid()) {
		p_dst->set_state_script(p_src->get_state_script());
	}
	if (p_src->get_behavior_tree().is_valid()) {
		p_dst->set_behavior_tree(p_src->get_behavior_tree());
	}
	// Per key: the child names only the values it changes, and the rest stay the base's.
	if (!p_src->get_state_properties().is_empty()) {
		Dictionary merged_props = p_dst->get_state_properties().duplicate();
		Dictionary src_props = p_src->get_state_properties();
		Array keys = src_props.keys();
		for (int i = 0; i < keys.size(); i++) {
			merged_props[keys[i]] = src_props[keys[i]];
		}
		p_dst->set_state_properties(merged_props);
	}
	if (p_src->get_graph_position() != Vector2()) {
		p_dst->set_graph_position(p_src->get_graph_position());
	}
	if (p_src->get_graph_size() != Vector2()) {
		p_dst->set_graph_size(p_src->get_graph_size());
	}
	if (p_src->get_anystate_graph_position() != Vector2()) {
		p_dst->set_anystate_graph_position(p_src->get_anystate_graph_position());
	}

	TypedArray<HSMStateSpec> src_children = p_src->get_children();
	for (int i = 0; i < src_children.size(); i++) {
		Ref<HSMStateSpec> child = src_children[i];
		if (child.is_null()) {
			continue;
		}
		Ref<HSMStateSpec> match;
		TypedArray<HSMStateSpec> dst_children = p_dst->get_children();
		for (int j = 0; j < dst_children.size(); j++) {
			Ref<HSMStateSpec> candidate = dst_children[j];
			if (candidate.is_valid() && candidate->get_state_name() == child->get_state_name()) {
				match = candidate;
				break;
			}
		}
		if (match.is_valid()) {
			_overlay(match, child, r_map, r_pairs);
		} else {
			_graft(child, p_dst, false, r_map, r_pairs);
		}
	}
}

} // namespace

Ref<HSMStateSpec> HierarchicalStateMachine::get_effective_root() const {
	if (base.is_null()) {
		return root;
	}
	// A file that reaches itself would never finish. The inspector can wire one, and a
	// resource does not refuse a cycle on its own.
	{
		HashMap<uint64_t, bool> seen;
		seen[get_instance_id()] = true;
		Ref<HierarchicalStateMachine> node = base;
		while (node.is_valid()) {
			if (seen.has(node->get_instance_id())) {
				ERR_PRINT(vformat("HierarchicalStateMachine: `base` comes back around: %s", get_path()));
				return root;
			}
			seen[node->get_instance_id()] = true;
			node = node->get_base();
		}
	}

	Ref<HSMStateSpec> base_root = base->get_effective_root();
	if (base_root.is_null()) {
		ERR_PRINT(vformat("HierarchicalStateMachine: `base` has no root state: %s", get_path()));
		return root;
	}

	HashMap<uint64_t, Ref<HSMStateSpec>> map;
	Vector<MergePair> pairs;

	Ref<HSMStateSpec> merged = _copy_values(base_root);
	merged->set_inherited(true);
	map[base_root->get_instance_id()] = merged;
	pairs.push_back(MergePair{ merged, base_root });
	TypedArray<HSMStateSpec> base_children = base_root->get_children();
	for (int i = 0; i < base_children.size(); i++) {
		_graft(base_children[i], merged, true, map, pairs);
	}

	if (root.is_valid()) {
		_overlay(merged, root, map, pairs);
	}

	// References last, once every state exists on both sides. Visit order puts the base's
	// rows before the child's, and the child's initial_child after the base's.
	for (int i = 0; i < pairs.size(); i++) {
		const MergePair &pair = pairs[i];
		TypedArray<HSMTransition> rows = pair.authored->get_transitions();
		TypedArray<HSMTransition> out = pair.merged->get_transitions();
		for (int j = 0; j < rows.size(); j++) {
			Ref<HSMTransition> row = rows[j];
			if (row.is_null() || row->get_to().is_null()) {
				continue;
			}
			Ref<HSMStateSpec> *to = map.getptr(row->get_to()->get_instance_id());
			if (to == nullptr) {
				ERR_PRINT(vformat("HierarchicalStateMachine: A transition of \"%s\" points at \"%s\", which is not in this machine.",
						pair.authored->get_state_name(), row->get_to()->get_state_name()));
				continue;
			}
			Ref<HSMStateSpec> from;
			if (row->get_from().is_valid()) {
				Ref<HSMStateSpec> *found = map.getptr(row->get_from()->get_instance_id());
				if (found == nullptr) {
					ERR_PRINT(vformat("HierarchicalStateMachine: A transition of \"%s\" leaves \"%s\", which is not in this machine.",
							pair.authored->get_state_name(), row->get_from()->get_state_name()));
					continue;
				}
				from = *found;
			}
			Ref<HSMTransition> copy;
			copy.instantiate();
			copy->set_from(from);
			copy->set_to(*to);
			copy->set_event(row->get_event());
			copy->set_note(row->get_note());
			// The condition by reference, not a copy: the merged tree is a picture of the
			// files, and LimboHSM duplicates every condition it builds from anyway. Sharing
			// it also keeps the editor able to match a merged row back to the authored one.
			copy->set_condition(row->get_condition());
			copy->set_priority(row->get_priority());
			out.append(copy);
		}
		pair.merged->set_transitions(out);

		Ref<HSMStateSpec> initial = pair.authored->get_declared_initial_child();
		if (initial.is_valid()) {
			Ref<HSMStateSpec> *found = map.getptr(initial->get_instance_id());
			if (found != nullptr) {
				pair.merged->set_initial_child(*found);
			}
		}
	}
	return merged;
}

void HierarchicalStateMachine::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_description", "description"), &HierarchicalStateMachine::set_description);
	ClassDB::bind_method(D_METHOD("get_description"), &HierarchicalStateMachine::get_description);
	ClassDB::bind_method(D_METHOD("set_root", "state"), &HierarchicalStateMachine::set_root);
	ClassDB::bind_method(D_METHOD("get_root"), &HierarchicalStateMachine::get_root);
	ClassDB::bind_method(D_METHOD("set_base", "base"), &HierarchicalStateMachine::set_base);
	ClassDB::bind_method(D_METHOD("get_base"), &HierarchicalStateMachine::get_base);
	ClassDB::bind_method(D_METHOD("get_effective_root"), &HierarchicalStateMachine::get_effective_root);
	ClassDB::bind_method(D_METHOD("set_blackboard_plan", "plan"), &HierarchicalStateMachine::set_blackboard_plan);
	ClassDB::bind_method(D_METHOD("get_blackboard_plan"), &HierarchicalStateMachine::get_blackboard_plan);
	ClassDB::bind_method(D_METHOD("get_effective_blackboard_plan"), &HierarchicalStateMachine::get_effective_blackboard_plan);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "description", PROPERTY_HINT_MULTILINE_TEXT), "set_description", "get_description");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "base", PROPERTY_HINT_RESOURCE_TYPE, "HierarchicalStateMachine"), "set_base", "get_base");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "root", PROPERTY_HINT_RESOURCE_TYPE, "HSMStateSpec"), "set_root", "get_root");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "blackboard_plan", PROPERTY_HINT_RESOURCE_TYPE, "BlackboardPlan"), "set_blackboard_plan", "get_blackboard_plan");
}

HierarchicalStateMachine::HierarchicalStateMachine() {
}

HierarchicalStateMachine::~HierarchicalStateMachine() {
}
