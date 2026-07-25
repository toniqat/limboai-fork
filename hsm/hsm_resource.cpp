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
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/core/class_db.hpp>
#endif // LIMBOAI_GDEXTENSION

//**** HSMTransition

void HSMTransition::set_from(const Ref<HSMState> &p_state) {
	from = p_state;
	emit_changed();
}

Ref<HSMState> HSMTransition::get_from() const {
	return from;
}

void HSMTransition::set_to(const Ref<HSMState> &p_state) {
	to = p_state;
	emit_changed();
}

Ref<HSMState> HSMTransition::get_to() const {
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

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "from", PROPERTY_HINT_RESOURCE_TYPE, "HSMState"), "set_from", "get_from");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "to", PROPERTY_HINT_RESOURCE_TYPE, "HSMState"), "set_to", "get_to");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "event"), "set_event", "get_event");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "note", PROPERTY_HINT_MULTILINE_TEXT), "set_note", "get_note");
}

HSMTransition::HSMTransition() {
}

HSMTransition::~HSMTransition() {
}

//**** HSMState

void HSMState::set_state_name(const StringName &p_name) {
	state_name = p_name;
	emit_changed();
}

void HSMState::set_state_script(const Ref<Script> &p_script) {
	state_script = p_script;
	emit_changed();
}

void HSMState::set_behavior_tree(const Ref<BehaviorTree> &p_tree) {
	behavior_tree = p_tree;
	emit_changed();
}

void HSMState::set_children(const TypedArray<HSMState> &p_children) {
	children = p_children;
	emit_changed();
}

void HSMState::set_initial_child(const Ref<HSMState> &p_state) {
	initial_child = p_state;
	emit_changed();
}

Ref<HSMState> HSMState::get_initial_child() const {
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

void HSMState::set_transitions(const TypedArray<HSMTransition> &p_transitions) {
	transitions = p_transitions;
	emit_changed();
}

void HSMState::set_graph_position(const Vector2 &p_position) {
	graph_position = p_position;
	emit_changed();
}

StringName HSMState::resolve_base_type() const {
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

void HSMState::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_state_name", "name"), &HSMState::set_state_name);
	ClassDB::bind_method(D_METHOD("get_state_name"), &HSMState::get_state_name);
	ClassDB::bind_method(D_METHOD("set_state_script", "script"), &HSMState::set_state_script);
	ClassDB::bind_method(D_METHOD("get_state_script"), &HSMState::get_state_script);
	ClassDB::bind_method(D_METHOD("set_behavior_tree", "behavior_tree"), &HSMState::set_behavior_tree);
	ClassDB::bind_method(D_METHOD("get_behavior_tree"), &HSMState::get_behavior_tree);
	ClassDB::bind_method(D_METHOD("set_children", "children"), &HSMState::set_children);
	ClassDB::bind_method(D_METHOD("get_children"), &HSMState::get_children);
	ClassDB::bind_method(D_METHOD("set_initial_child", "state"), &HSMState::set_initial_child);
	ClassDB::bind_method(D_METHOD("get_initial_child"), &HSMState::get_initial_child);
	ClassDB::bind_method(D_METHOD("set_transitions", "transitions"), &HSMState::set_transitions);
	ClassDB::bind_method(D_METHOD("get_transitions"), &HSMState::get_transitions);
	ClassDB::bind_method(D_METHOD("set_graph_position", "position"), &HSMState::set_graph_position);
	ClassDB::bind_method(D_METHOD("get_graph_position"), &HSMState::get_graph_position);
	ClassDB::bind_method(D_METHOD("is_machine"), &HSMState::is_machine);
	ClassDB::bind_method(D_METHOD("resolve_base_type"), &HSMState::resolve_base_type);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "state_name"), "set_state_name", "get_state_name");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "state_script", PROPERTY_HINT_RESOURCE_TYPE, "Script"), "set_state_script", "get_state_script");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "behavior_tree", PROPERTY_HINT_RESOURCE_TYPE, "BehaviorTree"), "set_behavior_tree", "get_behavior_tree");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "children", PROPERTY_HINT_ARRAY_TYPE, RESOURCE_TYPE_HINT("HSMState")), "set_children", "get_children");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "initial_child", PROPERTY_HINT_RESOURCE_TYPE, "HSMState"), "set_initial_child", "get_initial_child");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "transitions", PROPERTY_HINT_ARRAY_TYPE, RESOURCE_TYPE_HINT("HSMTransition")), "set_transitions", "get_transitions");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "graph_position"), "set_graph_position", "get_graph_position");
}

HSMState::HSMState() {
}

HSMState::~HSMState() {
}

//**** HierarchicalStateMachine

void HierarchicalStateMachine::set_description(const String &p_value) {
	description = p_value;
	emit_changed();
}

void HierarchicalStateMachine::set_root(const Ref<HSMState> &p_state) {
	root = p_state;
	emit_changed();
}

void HierarchicalStateMachine::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_description", "description"), &HierarchicalStateMachine::set_description);
	ClassDB::bind_method(D_METHOD("get_description"), &HierarchicalStateMachine::get_description);
	ClassDB::bind_method(D_METHOD("set_root", "state"), &HierarchicalStateMachine::set_root);
	ClassDB::bind_method(D_METHOD("get_root"), &HierarchicalStateMachine::get_root);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "description", PROPERTY_HINT_MULTILINE_TEXT), "set_description", "get_description");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "root", PROPERTY_HINT_RESOURCE_TYPE, "HSMState"), "set_root", "get_root");
}

HierarchicalStateMachine::HierarchicalStateMachine() {
}

HierarchicalStateMachine::~HierarchicalStateMachine() {
}
