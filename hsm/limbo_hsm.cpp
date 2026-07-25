/**
 * limbo_hsm.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#include "limbo_hsm.h"

#include "../bt/bt_state.h"
#include "../compat/variant.h"

#ifdef LIMBOAI_MODULE
#include "core/config/engine.h"
#include "core/object/class_db.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#endif // LIMBOAI_GDEXTENSION

VARIANT_ENUM_CAST(LimboHSM::UpdateMode);

void LimboHSM::set_hsm(const Ref<HierarchicalStateMachine> &p_hsm) {
	hsm = p_hsm;
}

LimboState *LimboHSM::_create_state_node(const Ref<HSMState> &p_spec) {
	ERR_FAIL_COND_V(p_spec.is_null(), nullptr);

	StringName base_type = p_spec->resolve_base_type();
	ERR_FAIL_COND_V_MSG(!ClassDB::class_exists(base_type), nullptr,
			vformat("LimboHSM: State \"%s\" resolves to an unknown type \"%s\".", p_spec->get_state_name(), base_type));
	ERR_FAIL_COND_V_MSG(!ClassDB::is_parent_class(base_type, "LimboState"), nullptr,
			vformat("LimboHSM: State \"%s\" resolves to \"%s\", which is not a LimboState.", p_spec->get_state_name(), base_type));
	// A state that owns children must become a machine -- otherwise the children have
	// nowhere to hang and the transitions between them have no owner.
	ERR_FAIL_COND_V_MSG(p_spec->is_machine() && !ClassDB::is_parent_class(base_type, "LimboHSM"), nullptr,
			vformat("LimboHSM: State \"%s\" has children but resolves to \"%s\", which is not a LimboHSM.", p_spec->get_state_name(), base_type));

	Variant inst = ClassDB::instantiate(base_type);
	Object *obj = inst;
	LimboState *state = Object::cast_to<LimboState>(obj);
	if (state == nullptr) {
		VARIANT_DELETE_IF_OBJECT(inst);
		ERR_FAIL_V_MSG(nullptr, vformat("LimboHSM: Failed to create state \"%s\" of type \"%s\".", p_spec->get_state_name(), base_type));
	}

	state->set_name(p_spec->get_state_name());
	if (p_spec->get_state_script().is_valid()) {
		state->set_script(p_spec->get_state_script());
	}

	LimboHSM *nested = Object::cast_to<LimboHSM>(state);
	if (nested != nullptr) {
		// A nested machine is driven by its parent's update(). Letting it tick itself
		// would run it twice in one frame, so this is not something a file gets to say.
		nested->set_update_mode(UpdateMode::MANUAL);
	}

	BTState *bt_state = Object::cast_to<BTState>(state);
	if (bt_state != nullptr) {
		// A node created at runtime has no owner, and BTState falls back to get_owner()
		// to establish the scene root -- without this it refuses to instantiate its tree.
		// The root machine is that root: when the same machine is built from a scene
		// instead, its states are owned by exactly this node.
		bt_state->set_scene_root_hint(get_owner() != nullptr ? get_owner() : this);
		if (p_spec->get_behavior_tree().is_valid()) {
			bt_state->set_behavior_tree(p_spec->get_behavior_tree());
		}
	}
	return state;
}

void LimboHSM::_instantiate_states(const Ref<HSMState> &p_spec, LimboHSM *p_machine, HashMap<uint64_t, LimboState *> &r_nodes) {
	ERR_FAIL_COND(p_spec.is_null());
	ERR_FAIL_NULL(p_machine);

	TypedArray<HSMState> children = p_spec->get_children();
	for (int i = 0; i < children.size(); i++) {
		Ref<HSMState> spec = children[i];
		if (spec.is_null()) {
			ERR_PRINT(vformat("LimboHSM: State \"%s\" has an empty child slot at %d.", p_spec->get_state_name(), i));
			continue;
		}
		// The same state resource appearing twice would give one node two parents; it is
		// also how a cycle in the file shows up, and a cycle would never return.
		if (r_nodes.has(spec->get_instance_id())) {
			ERR_PRINT(vformat("LimboHSM: State \"%s\" appears more than once.", spec->get_state_name()));
			continue;
		}

		LimboState *node = _create_state_node(spec);
		if (node == nullptr) {
			continue;
		}
		r_nodes[spec->get_instance_id()] = node;
		p_machine->add_child(node);

		if (spec->is_machine()) {
			_instantiate_states(spec, Object::cast_to<LimboHSM>(node), r_nodes);
		}
	}

	Ref<HSMState> initial = p_spec->get_initial_child();
	if (initial.is_valid()) {
		LimboState *const *found = r_nodes.getptr(initial->get_instance_id());
		if (found != nullptr) {
			p_machine->set_initial_state(*found);
		} else {
			ERR_PRINT(vformat("LimboHSM: Initial state of \"%s\" is not one of its children.", p_spec->get_state_name()));
		}
	}
}

void LimboHSM::_wire_transitions(const Ref<HSMState> &p_spec, LimboHSM *p_machine, const HashMap<uint64_t, LimboState *> &p_nodes) {
	ERR_FAIL_COND(p_spec.is_null());
	ERR_FAIL_NULL(p_machine);

	TypedArray<HSMTransition> rows = p_spec->get_transitions();
	for (int i = 0; i < rows.size(); i++) {
		Ref<HSMTransition> row = rows[i];
		if (row.is_null()) {
			continue;
		}
		Ref<HSMState> to_spec = row->get_to();
		if (to_spec.is_null()) {
			ERR_PRINT(vformat("LimboHSM: A transition of \"%s\" has no destination.", p_spec->get_state_name()));
			continue;
		}
		LimboState *const *to = p_nodes.getptr(to_spec->get_instance_id());
		if (to == nullptr) {
			ERR_PRINT(vformat("LimboHSM: Destination \"%s\" of a transition of \"%s\" was not built.", to_spec->get_state_name(), p_spec->get_state_name()));
			continue;
		}

		// An empty `from` is ANYSTATE -- there is no state being left.
		LimboState *from = nullptr;
		Ref<HSMState> from_spec = row->get_from();
		if (from_spec.is_valid()) {
			LimboState *const *found = p_nodes.getptr(from_spec->get_instance_id());
			if (found == nullptr) {
				ERR_PRINT(vformat("LimboHSM: Origin \"%s\" of a transition of \"%s\" was not built.", from_spec->get_state_name(), p_spec->get_state_name()));
				continue;
			}
			from = *found;
		}
		p_machine->add_transition(from, *to, row->get_event());
	}

	TypedArray<HSMState> children = p_spec->get_children();
	for (int i = 0; i < children.size(); i++) {
		Ref<HSMState> spec = children[i];
		if (spec.is_null() || !spec->is_machine()) {
			continue;
		}
		LimboState *const *node = p_nodes.getptr(spec->get_instance_id());
		if (node == nullptr) {
			continue;
		}
		_wire_transitions(spec, Object::cast_to<LimboHSM>(*node), p_nodes);
	}
}

void LimboHSM::build_from_resource() {
	ERR_FAIL_COND_MSG(hsm.is_null(), "LimboHSM: No state machine resource to build from.");
	ERR_FAIL_COND_MSG(built_from_resource, "LimboHSM: Already built from a resource.");

	// The merged root, not the file's own: a machine that inherits gets its base's states
	// laid out first and its own on top. Without a base the two are the same object.
	Ref<HSMState> root = hsm->get_effective_root();
	ERR_FAIL_COND_MSG(root.is_null(), vformat("LimboHSM: State machine resource has no root state: %s", hsm->get_path()));

	built_from_resource = true;

	// The root state is this node -- it is not instantiated, only its children are.
	HashMap<uint64_t, LimboState *> nodes;
	nodes[root->get_instance_id()] = this;
	_instantiate_states(root, this, nodes);
	_wire_transitions(root, this, nodes);
}

void LimboHSM::set_active(bool p_active) {
	ERR_FAIL_COND_MSG(agent == nullptr, "LimboHSM is not initialized.");
	ERR_FAIL_COND_MSG(p_active && initial_state == nullptr, "LimboHSM has no initial substate candidate.");
	ERR_FAIL_COND_MSG(!is_root(), "LimboHSM.set_active() should only be called on the root state.");

	if (active == p_active) {
		return;
	}

	active = p_active;
	is_initiating_update = p_active;

	if (active) {
		_enter();
	} else {
		_exit();
	}
}

void LimboHSM::change_active_state(LimboState *p_state) {
	ERR_FAIL_NULL(p_state);
	ERR_FAIL_COND_MSG(!is_active(), "LimboHSM: Unable to change active state when HSM is not active.");
	ERR_FAIL_COND_MSG(p_state->get_parent() != this, "LimboHSM: Unable to perform transition to a state that is not a child of this HSM.");

	if (active_state) {
		active_state->_exit();
		previous_active = active_state;
	}

	active_state = p_state;
	active_state->_enter();

	emit_signal(LW_NAME(active_state_changed), active_state, previous_active);
}

void LimboHSM::_enter() {
	ERR_FAIL_COND_MSG(get_child_count() == 0, "LimboHSM has no candidate for initial substate.");
	ERR_FAIL_COND(active_state != nullptr);
	ERR_FAIL_COND_MSG(initial_state == nullptr, "LimboHSM: Initial state is not set.");

	LimboState::_enter();
	change_active_state(initial_state);
}

void LimboHSM::_exit() {
	ERR_FAIL_COND(active_state == nullptr);
	active_state->_exit();
	active_state = nullptr;
	LimboState::_exit();
}

void LimboHSM::_update(double p_delta) {
	if (active) {
		ERR_FAIL_NULL(active_state);
		LimboState *last_active_state = active_state;
		LimboState::_update(p_delta);
		if (last_active_state == active_state) {
			active_state->_update(p_delta);
		}
	}
}

void LimboHSM::update(double p_delta) {
	updating = true;
	_update(p_delta);
	updating = false;
	if (next_active) {
		change_active_state(next_active);
		next_active = nullptr;
	}
}

void LimboHSM::add_transition(LimboState *p_from_state, LimboState *p_to_state, const StringName &p_event, const Callable &p_guard) {
	ERR_FAIL_COND_MSG(p_from_state != nullptr && p_from_state->get_parent() != this, "LimboHSM: Unable to add a transition from a state that is not an immediate child of mine.");
	ERR_FAIL_COND_MSG(p_to_state == nullptr, "LimboHSM: Unable to add a transition to a null state.");
	ERR_FAIL_COND_MSG(p_to_state->get_parent() != this, "LimboHSM: Unable to add a transition to a state that is not an immediate child of mine.");
	ERR_FAIL_COND_MSG(p_event == StringName(), "LimboHSM: Failed to add transition due to empty event string.");

	TransitionKey key = Transition::make_key(p_from_state, p_event);
	ERR_FAIL_COND_MSG(transitions.has(key), "LimboHSM: Unable to add another transition with the same event and origin.");
	// Note: Explicit ObjectID casting needed for GDExtension.
	transitions[key] = {
		p_from_state != nullptr ? ObjectID(p_from_state->get_instance_id()) : ObjectID(),
		ObjectID(p_to_state->get_instance_id()),
		p_event,
		p_guard
	};
}

void LimboHSM::remove_transition(LimboState *p_from_state, const StringName &p_event) {
	ERR_FAIL_COND_MSG(p_from_state != nullptr && p_from_state->get_parent() != this, "LimboHSM: Unable to remove a transition from a state that is not an immediate child of mine.");
	ERR_FAIL_COND_MSG(p_event == StringName(), "LimboHSM: Unable to remove a transition due to empty event string.");

	TransitionKey key = Transition::make_key(p_from_state, p_event);
	ERR_FAIL_COND_MSG(!transitions.has(key), "LimboHSM: Unable to remove a transition that does not exist.");
	transitions.erase(key);
}

void LimboHSM::_get_transition(LimboState *p_from_state, const StringName &p_event, Transition &r_transition) const {
	ERR_FAIL_COND_MSG(p_from_state != nullptr && p_from_state->get_parent() != this, "LimboHSM: Unable to get a transition from a state that is not an immediate child of this HSM.");
	ERR_FAIL_COND_MSG(p_event == StringName(), "LimboHSM: Unable to get a transition with an empty event string.");

	TransitionKey key = Transition::make_key(p_from_state, p_event);
	if (transitions.has(key)) {
		r_transition = transitions[key];
	}
}

LimboState *LimboHSM::get_leaf_state() const {
	LimboHSM *hsm = const_cast<LimboHSM *>(this);
	while (hsm->active_state != nullptr && hsm->active_state->is_class("LimboHSM")) {
		hsm = Object::cast_to<LimboHSM>(hsm->active_state);
	}
	if (hsm->active_state) {
		return hsm->active_state;
	} else {
		return hsm;
	}
}

void LimboHSM::set_initial_state(LimboState *p_state) {
	ERR_FAIL_COND(p_state == nullptr || !p_state->is_class("LimboState"));
	initial_state = Object::cast_to<LimboState>(p_state);
}

bool LimboHSM::_dispatch(const StringName &p_event, const Variant &p_cargo) {
	ERR_FAIL_COND_V(p_event == StringName(), false);

	bool event_consumed = false;

	if (active_state) {
		event_consumed = active_state->_dispatch(p_event, p_cargo);
	}

	if (!event_consumed) {
		event_consumed = LimboState::_dispatch(p_event, p_cargo);
	}

	if (!event_consumed && active_state) {
		LimboState *to_state = nullptr;

		Transition transition;
		_get_transition(active_state, p_event, transition);
		if (transition.is_valid() && transition.is_allowed()) {
			to_state = Object::cast_to<LimboState>(ObjectDB::get_instance(transition.to_state));
		}
		if (to_state == nullptr) {
			// Get ANYSTATE transition.
			_get_transition(nullptr, p_event, transition);
			if (transition.is_valid() && transition.is_allowed()) {
				to_state = Object::cast_to<LimboState>(ObjectDB::get_instance(transition.to_state));
				if (to_state == active_state) {
					// Transitions to self are not allowed with ANYSTATE.
					to_state = nullptr;
				}
			}
		}
		if (to_state != nullptr) {
			bool permitted = true;
			if (to_state->guard_callable.is_valid()) {
				Variant ret;

#ifdef LIMBOAI_MODULE
				Callable::CallError ce;
				to_state->guard_callable.callp(nullptr, 0, ret, ce);
				if (unlikely(ce.error != Callable::CallError::CALL_OK)) {
					ERR_PRINT_ONCE("LimboHSM: Error calling substate's guard callable: " + Variant::get_callable_error_text(to_state->guard_callable, nullptr, 0, ce));
				}
#elif LIMBOAI_GDEXTENSION
				ret = to_state->guard_callable.call();
#endif

				if (unlikely(ret.get_type() != Variant::BOOL)) {
					ERR_PRINT_ONCE(vformat("State guard callable %s returned non-boolean value (%s).", to_state->guard_callable, to_state));
				} else {
					permitted = bool(ret);
				}
			}
			if (permitted) {
				if (!updating) {
					change_active_state(to_state);
				} else if (!next_active) {
					// Only set next_active if we are not already in the process of changing states.
					next_active = to_state;
				}
				event_consumed = true;
			}
		}
	}

	if (!event_consumed && p_event == EVENT_FINISHED && !(get_parent() && get_parent()->is_class("LimboState"))) {
		_exit();
	}

	return event_consumed;
}

void LimboHSM::initialize(Node *p_agent, const Ref<Blackboard> &p_parent_scope) {
	ERR_FAIL_COND(p_agent == nullptr);
	ERR_FAIL_COND_MSG(!is_root(), "LimboHSM: initialize() must be called on the root HSM.");

	_initialize(p_agent, p_parent_scope);

	if (initial_state == nullptr) {
		initial_state = Object::cast_to<LimboState>(get_child(0));
	}
}

void LimboHSM::_initialize(Node *p_agent, const Ref<Blackboard> &p_blackboard) {
	ERR_FAIL_COND(p_agent == nullptr);
	ERR_FAIL_COND_MSG(agent != nullptr, "LimboAI: HSM already initialized.");
	ERR_FAIL_COND_MSG(get_child_count() == 0, "Cannot initialize LimboHSM: no candidate for initial substate.");

	if (initial_state == nullptr) {
		initial_state = Object::cast_to<LimboState>(get_child(0));
		ERR_FAIL_COND_MSG(initial_state == nullptr, "LimboHSM: Child at index 0 is not a LimboState.");
	}

	LimboState::_initialize(p_agent, p_blackboard);

	for (int i = 0; i < get_child_count(); i++) {
		LimboState *c = Object::cast_to<LimboState>(get_child(i));
		if (unlikely(c == nullptr)) {
			ERR_PRINT(vformat("LimboHSM: Child at index %d is not a LimboState.", i));
		} else {
			c->_initialize(agent, blackboard);
		}
	}
}

void LimboHSM::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == LW_NAME(update_mode) && !is_root()) {
		// Hide update_mode for non-root HSMs.
		p_property.usage = PROPERTY_USAGE_NONE;
	}
}

void LimboHSM::_exit_if_not_inside_tree() {
	if (is_active() && !is_inside_tree()) {
		_exit();
	}
}

void LimboHSM::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			// Built here rather than when the resource is assigned: the owner may set it
			// before the node is in the tree, and states must not be created twice.
			if (hsm.is_valid() && !built_from_resource && !Engine::get_singleton()->is_editor_hint()) {
				build_from_resource();
			}
		} break;
		case NOTIFICATION_POST_ENTER_TREE: {
			if (was_active && is_root()) {
				// Re-activate the root HSM if it was previously active.
				// Typically, this happens when the node is re-entered scene repeatedly (such as with object pooling).
				set_active(true);
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (is_root()) {
				// Exit the state machine if the root HSM is no longer in the scene tree (except when being reparented).
				// This ensures that resources and signal connections are released if active.
				was_active = is_active();
				if (is_active()) {
					// Check if the HSM node is being deleted.
					bool is_being_deleted = false;
					Node *node = this;
					while (node) {
						if (node->is_queued_for_deletion()) {
							is_being_deleted = true;
							break;
						}
						node = node->get_parent();
					}

					if (is_being_deleted) {
						// Exit the state machine immediately if the HSM is being deleted.
						_exit();
					} else {
						// Use deferred mode to prevent exiting during Node re-parenting.
						// This allows the HSM to remain active when it (or one of its parents) is reparented.
						callable_mp(this, &LimboHSM::_exit_if_not_inside_tree).call_deferred();
					}
				}
			}
		} break;
		case NOTIFICATION_PROCESS: {
			if (is_initiating_update && update_mode == UpdateMode::IDLE) {
				update(get_process_delta_time());
			}
		} break;
		case NOTIFICATION_PHYSICS_PROCESS: {
			if (is_initiating_update && update_mode == UpdateMode::PHYSICS) {
				update(get_physics_process_delta_time());
			}
		} break;
	}
}

void LimboHSM::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_update_mode", "mode"), &LimboHSM::set_update_mode);
	ClassDB::bind_method(D_METHOD("get_update_mode"), &LimboHSM::get_update_mode);

	ClassDB::bind_method(D_METHOD("set_hsm", "hsm"), &LimboHSM::set_hsm);
	ClassDB::bind_method(D_METHOD("get_hsm"), &LimboHSM::get_hsm);
	ClassDB::bind_method(D_METHOD("build_from_resource"), &LimboHSM::build_from_resource);
	ClassDB::bind_method(D_METHOD("is_built_from_resource"), &LimboHSM::is_built_from_resource);

	ClassDB::bind_method(D_METHOD("set_initial_state", "state"), &LimboHSM::set_initial_state);
	ClassDB::bind_method(D_METHOD("get_initial_state"), &LimboHSM::get_initial_state);

	ClassDB::bind_method(D_METHOD("get_active_state"), &LimboHSM::get_active_state);
	ClassDB::bind_method(D_METHOD("get_previous_active_state"), &LimboHSM::get_previous_active_state);
	ClassDB::bind_method(D_METHOD("get_leaf_state"), &LimboHSM::get_leaf_state);
	ClassDB::bind_method(D_METHOD("set_active", "active"), &LimboHSM::set_active);
	ClassDB::bind_method(D_METHOD("update", "delta"), &LimboHSM::update);
	ClassDB::bind_method(D_METHOD("add_transition", "from_state", "to_state", "event", "guard"), &LimboHSM::add_transition, DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("remove_transition", "from_state", "event"), &LimboHSM::remove_transition);
	ClassDB::bind_method(D_METHOD("has_transition", "from_state", "event"), &LimboHSM::has_transition);
	ClassDB::bind_method(D_METHOD("anystate"), &LimboHSM::anystate);
	ClassDB::bind_method(D_METHOD("initialize", "agent", "parent_scope"), &LimboHSM::initialize, Variant());
	ClassDB::bind_method(D_METHOD("change_active_state", "state"), &LimboHSM::change_active_state);

	BIND_ENUM_CONSTANT(IDLE);
	BIND_ENUM_CONSTANT(PHYSICS);
	BIND_ENUM_CONSTANT(MANUAL);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "update_mode", PROPERTY_HINT_ENUM, "Idle, Physics, Manual"), "set_update_mode", "get_update_mode");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "hsm", PROPERTY_HINT_RESOURCE_TYPE, "HierarchicalStateMachine"), "set_hsm", "get_hsm");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "ANYSTATE", PROPERTY_HINT_RESOURCE_TYPE, "LimboState", 0), "", "anystate");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "initial_state", PROPERTY_HINT_RESOURCE_TYPE, "LimboState", 0), "set_initial_state", "get_initial_state");

	ADD_SIGNAL(MethodInfo("active_state_changed",
			PropertyInfo(Variant::OBJECT, "current", PROPERTY_HINT_RESOURCE_TYPE, "LimboState"),
			PropertyInfo(Variant::OBJECT, "previous", PROPERTY_HINT_RESOURCE_TYPE, "LimboState")));
}

LimboHSM::LimboHSM() {
	update_mode = UpdateMode::PHYSICS;
	active_state = nullptr;
	previous_active = nullptr;
	next_active = nullptr;
	initial_state = nullptr;
}
