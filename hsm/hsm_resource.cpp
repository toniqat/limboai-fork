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
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
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

void HSMTransition::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_from", "state"), &HSMTransition::set_from);
	ClassDB::bind_method(D_METHOD("get_from"), &HSMTransition::get_from);
	ClassDB::bind_method(D_METHOD("set_to", "state"), &HSMTransition::set_to);
	ClassDB::bind_method(D_METHOD("get_to"), &HSMTransition::get_to);
	ClassDB::bind_method(D_METHOD("set_event", "event"), &HSMTransition::set_event);
	ClassDB::bind_method(D_METHOD("get_event"), &HSMTransition::get_event);
	ClassDB::bind_method(D_METHOD("set_note", "note"), &HSMTransition::set_note);
	ClassDB::bind_method(D_METHOD("get_note"), &HSMTransition::get_note);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "from", PROPERTY_HINT_RESOURCE_TYPE, "HSMStateSpec"), "set_from", "get_from");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "to", PROPERTY_HINT_RESOURCE_TYPE, "HSMStateSpec"), "set_to", "get_to");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "event"), "set_event", "get_event");
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

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "description", PROPERTY_HINT_MULTILINE_TEXT), "set_description", "get_description");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "base", PROPERTY_HINT_RESOURCE_TYPE, "HierarchicalStateMachine"), "set_base", "get_base");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "root", PROPERTY_HINT_RESOURCE_TYPE, "HSMStateSpec"), "set_root", "get_root");
}

HierarchicalStateMachine::HierarchicalStateMachine() {
}

HierarchicalStateMachine::~HierarchicalStateMachine() {
}
