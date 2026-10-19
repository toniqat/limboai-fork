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
#include "../compat/object.h"
#include "../compat/variant.h"
#include "../editor/debugger/limbo_debugger.h"

#ifdef LIMBOAI_MODULE
#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#endif // LIMBOAI_GDEXTENSION

VARIANT_ENUM_CAST(LimboHSM::UpdateMode);

void LimboHSM::set_hsm(const Ref<HierarchicalStateMachine> &p_hsm) {
	hsm = p_hsm;
}

LimboState *LimboHSM::_create_state_node(const Ref<HSMStateSpec> &p_spec) {
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
	_apply_state_properties(state, p_spec);

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

void LimboHSM::_apply_state_properties(LimboState *p_state, const Ref<HSMStateSpec> &p_spec) {
	// After the script, so its exported properties exist; before the machine is initialized,
	// so _setup() already sees the values. The dictionary is read, never written: every node
	// built from the file gets its own values from it.
	Dictionary props = p_spec->get_state_properties();
	if (props.is_empty()) {
		return;
	}
	// One warning per state and property, not per machine built: a file shared by fifty NPCs
	// would otherwise say the same thing fifty times.
	static HashSet<String> warned;
	Array keys = props.keys();
	for (int i = 0; i < keys.size(); i++) {
		StringName name = StringName(String(keys[i]));
		if (!OBJECT_HAS_PROPERTY(p_state, name)) {
			String key = vformat("%s.%s", p_spec->get_state_name(), name);
			if (!warned.has(key)) {
				warned.insert(key);
				WARN_PRINT(vformat("LimboHSM: State \"%s\" has no property \"%s\"; the value in state_properties is ignored.", p_spec->get_state_name(), name));
			}
			continue;
		}
		// Deep-copied: arrays, dictionaries and packed arrays travel by reference in a
		// Variant, so a node that appended to its array would otherwise append to the file's
		// and to every other node built from it.
		p_state->set(name, props[keys[i]].duplicate(true));
	}
}

void LimboHSM::_instantiate_states(const Ref<HSMStateSpec> &p_spec, LimboHSM *p_machine, HashMap<uint64_t, LimboState *> &r_nodes) {
	ERR_FAIL_COND(p_spec.is_null());
	ERR_FAIL_NULL(p_machine);

	TypedArray<HSMStateSpec> children = p_spec->get_children();
	for (int i = 0; i < children.size(); i++) {
		Ref<HSMStateSpec> spec = children[i];
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

	Ref<HSMStateSpec> initial = p_spec->get_initial_child();
	if (initial.is_valid()) {
		LimboState *const *found = r_nodes.getptr(initial->get_instance_id());
		if (found != nullptr) {
			p_machine->set_initial_state(*found);
		} else {
			ERR_PRINT(vformat("LimboHSM: Initial state of \"%s\" is not one of its children.", p_spec->get_state_name()));
		}
	}
}

void LimboHSM::_wire_transitions(const Ref<HSMStateSpec> &p_spec, LimboHSM *p_machine, const HashMap<uint64_t, LimboState *> &p_nodes) {
	ERR_FAIL_COND(p_spec.is_null());
	ERR_FAIL_NULL(p_machine);

	TypedArray<HSMTransition> rows = p_spec->get_transitions();
	for (int i = 0; i < rows.size(); i++) {
		Ref<HSMTransition> row = rows[i];
		if (row.is_null()) {
			continue;
		}
		Ref<HSMStateSpec> to_spec = row->get_to();
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
		Ref<HSMStateSpec> from_spec = row->get_from();
		if (from_spec.is_valid()) {
			LimboState *const *found = p_nodes.getptr(from_spec->get_instance_id());
			if (found == nullptr) {
				ERR_PRINT(vformat("LimboHSM: Origin \"%s\" of a transition of \"%s\" was not built.", from_spec->get_state_name(), p_spec->get_state_name()));
				continue;
			}
			from = *found;
		}

		// The file's condition is shared by every machine built from it, so each machine asks
		// a copy of its own. Deep, so that whatever the condition holds in its exported
		// sub-resources is this machine's too; the script itself is external and stays shared.
		Ref<HSMCondition> condition;
		if (row->get_condition().is_valid()) {
			Ref<Resource> copy = row->get_condition()->duplicate(true);
			condition = Ref<HSMCondition>(Object::cast_to<HSMCondition>(copy.ptr()));
			if (condition.is_null()) {
				ERR_PRINT(vformat("LimboHSM: The condition of a transition of \"%s\" could not be copied.", p_spec->get_state_name()));
				continue;
			}
		}
		p_machine->add_transition(from, *to, row->get_event(), Callable(), condition, row->get_priority());
	}

	TypedArray<HSMStateSpec> children = p_spec->get_children();
	for (int i = 0; i < children.size(); i++) {
		Ref<HSMStateSpec> spec = children[i];
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
	Ref<HSMStateSpec> root = hsm->get_effective_root();
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

#ifdef DEBUG_ENABLED
	// Only a root machine is announced, and only while it is running. A nested one is not a
	// second entry in the list -- it is part of the picture of the root, which is the thing
	// the editor asks for by name.
	if (LimboDebugger::get_singleton() != nullptr) {
		if (active) {
			LimboDebugger::get_singleton()->register_hsm(get_instance_id());
		} else {
			LimboDebugger::get_singleton()->unregister_hsm(get_instance_id());
		}
	}
#endif // DEBUG_ENABLED
}

void LimboHSM::change_active_state(LimboState *p_state) {
	ERR_FAIL_NULL(p_state);
	ERR_FAIL_COND_MSG(!is_active(), "LimboHSM: Unable to change active state when HSM is not active.");
	ERR_FAIL_COND_MSG(p_state->get_parent() != this, "LimboHSM: Unable to perform transition to a state that is not a child of this HSM.");

	// What this machine was standing on a moment ago, which `previous_active` stops being the
	// moment the machine is left and stepped into again -- it keeps its value while out. The
	// debugger draws an edge out of it, and an edge is a thing that was actually taken.
	LimboState *left = active_state;

	if (active_state) {
		active_state->_exit();
		previous_active = active_state;
	}

	active_state = p_state;
	active_state->_enter();

	emit_signal(LW_NAME(active_state_changed), active_state, previous_active);

#ifdef DEBUG_ENABLED
	// Every machine tells, nested ones included, and all but the tracked root drop out on the
	// debugger's first line. Hooking `active_state_changed` per machine instead would mean
	// connecting and disconnecting as the tracked root changed -- and how a machine is nested
	// is itself a thing that only exists while it runs.
	if (LimboDebugger::get_singleton() != nullptr) {
		LimboDebugger::get_singleton()->hsm_state_changed(this, left);
	}
#endif // DEBUG_ENABLED
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

// 1. This machine's own _update() (its script), which may dispatch. A change of state made
//    there ends the tick, as it always has: the new child is not updated this tick.
// 2. The polled rows of the active state are asked; see _poll_transitions(). A polled change
//    is made at once, and the state just entered is updated in this same tick.
// 3. If neither changed the state, the active child is updated.
//
// Why a polled change updates the new child straight away: nested machines chain polled rows
// (root -> Daily -> Job -> GuardPost), and updating the new child is what lets its own polled
// rows run. Waiting a tick per level would leave the machine standing for a frame or more in
// states it is only passing through, and the leaf's tree would start late. One polled change
// per machine per update still holds: the new child is updated, this machine is not asked again.
void LimboHSM::_update(double p_delta) {
	if (active) {
		ERR_FAIL_NULL(active_state);
		LimboState *last_active_state = active_state;
		LimboState::_update(p_delta);
		if (last_active_state != active_state) {
			return;
		}
		// A change already queued by a dispatch during this update wins: the event was the
		// more specific request, and asking the polled rows now would only argue with it.
		if (next_active == nullptr) {
			// Whether or not a row was taken, the child that is active now is the one to
			// update: the state just entered when one was, the same one as before otherwise.
			_poll_transitions();
		}
		active_state->_update(p_delta);
	}
}

const Vector<int> &LimboHSM::_get_polled_order(LimboState *p_from_state) {
	uint64_t key = uint64_t(p_from_state->get_instance_id());
	Vector<int> *cached = polled_order.getptr(key);
	if (cached != nullptr) {
		return *cached;
	}

	// The rows that apply, then a stable insertion sort on (priority desc, specific first).
	// Insertion order is kept for ties, which is what makes "file order" the last tie-break
	// without storing it. The lists are a handful of rows, so the quadratic sort is the
	// cheap one.
	ObjectID from_id = ObjectID(p_from_state->get_instance_id());
	Vector<int> order;
	for (int i = 0; i < polled.size(); i++) {
		const Transition &t = polled[i];
		if (t.from_state != from_id && t.from_state != ObjectID()) {
			continue;
		}
		bool t_any = t.from_state == ObjectID();
		int pos = order.size();
		while (pos > 0) {
			const Transition &prev = polled[order[pos - 1]];
			bool prev_any = prev.from_state == ObjectID();
			bool goes_before = t.priority > prev.priority || (t.priority == prev.priority && !t_any && prev_any);
			if (!goes_before) {
				break;
			}
			pos -= 1;
		}
		order.insert(pos, i);
	}
	polled_order[key] = order;
	return *polled_order.getptr(key);
}

// The first row whose condition passes decides, and nothing below it is asked:
// - its target is the active state: stay (a lower row never overrides a higher true one);
// - the leaving state and the target's guard allow it: take it;
// - otherwise: blocked, stay.
// That is "desired state" semantics: the table answers "where should this machine be right
// now", and the machine moves there as soon as it is allowed to.
bool LimboHSM::_poll_transitions() {
	if (polled.is_empty() || active_state == nullptr) {
		return false;
	}
	// A copy of the indices rather than a reference: a condition is user code, and user code
	// may add a row, which drops the cached order out from under the loop.
	Vector<int> order = _get_polled_order(active_state);
	for (int i = 0; i < order.size(); i++) {
		if (order[i] >= polled.size()) {
			break;
		}
		// Copied for the same reason: the row must outlive anything the condition does.
		Transition t = polled[order[i]];
		if (!t.is_allowed()) {
			continue;
		}
		LimboState *to_state = Object::cast_to<LimboState>(OBJECT_DB_GET_INSTANCE(uint64_t(t.to_state)));
		if (to_state == nullptr) {
			// The target was freed. Nothing to stand on, so the row is as good as absent.
			continue;
		}
		if (to_state == active_state) {
			return false;
		}
		if (!_is_transition_permitted(to_state, t.priority)) {
			return false;
		}
		// Now, not queued through `next_active` like a dispatched event: step 1 is over and
		// nothing else is mid-change, so the switch is safe here, and making it now is what
		// lets the caller update the state just entered in the same tick. The debugger hears of
		// it from change_active_state() like any other transition.
		change_active_state(to_state);
		return true;
	}
	return false;
}

bool LimboHSM::_is_transition_permitted(LimboState *p_to_state, int p_priority) {
	if (active_state != nullptr && !active_state->_can_exit(p_to_state, p_priority)) {
		return false;
	}
	if (!p_to_state->guard_callable.is_valid()) {
		return true;
	}
	Variant ret;

#ifdef LIMBOAI_MODULE
	Callable::CallError ce;
	p_to_state->guard_callable.callp(nullptr, 0, ret, ce);
	if (unlikely(ce.error != Callable::CallError::CALL_OK)) {
		ERR_PRINT_ONCE("LimboHSM: Error calling substate's guard callable: " + Variant::get_callable_error_text(p_to_state->guard_callable, nullptr, 0, ce));
	}
#elif LIMBOAI_GDEXTENSION
	ret = p_to_state->guard_callable.call();
#endif

	if (unlikely(ret.get_type() != Variant::BOOL)) {
		ERR_PRINT_ONCE(vformat("State guard callable %s returned non-boolean value (%s).", p_to_state->guard_callable, p_to_state));
		return true;
	}
	return bool(ret);
}

void LimboHSM::_take_transition(LimboState *p_to_state) {
	if (!updating) {
		change_active_state(p_to_state);
	} else if (!next_active) {
		// Only set next_active if we are not already in the process of changing states.
		next_active = p_to_state;
	}
}

void LimboHSM::_bind_condition(const Ref<HSMCondition> &p_condition) {
	if (p_condition.is_null() || p_condition->is_bound() || agent == nullptr) {
		// Not yet initialized: _initialize() binds every condition once the blackboard and
		// the agent exist.
		return;
	}
	p_condition->bind_to(this, agent, blackboard);
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

void LimboHSM::add_transition(LimboState *p_from_state, LimboState *p_to_state, const StringName &p_event, const Callable &p_guard, const Ref<HSMCondition> &p_condition, int p_priority) {
	ERR_FAIL_COND_MSG(p_from_state != nullptr && p_from_state->get_parent() != this, "LimboHSM: Unable to add a transition from a state that is not an immediate child of mine.");
	ERR_FAIL_COND_MSG(p_to_state == nullptr, "LimboHSM: Unable to add a transition to a null state.");
	ERR_FAIL_COND_MSG(p_to_state->get_parent() != this, "LimboHSM: Unable to add a transition to a state that is not an immediate child of mine.");

	if (p_event == StringName()) {
		// Polled: no key, several may leave the same state, and the order is worked out on
		// first use.
		Transition t;
		t.from_state = p_from_state != nullptr ? ObjectID(p_from_state->get_instance_id()) : ObjectID();
		t.to_state = ObjectID(p_to_state->get_instance_id());
		t.guard = p_guard;
		t.condition = p_condition;
		t.priority = p_priority;
		polled.push_back(t);
		polled_order.clear();
		_bind_condition(p_condition);
		return;
	}

	TransitionKey key = Transition::make_key(p_from_state, p_event);
	ERR_FAIL_COND_MSG(transitions.has(key), "LimboHSM: Unable to add another transition with the same event and origin.");
	// Note: Explicit ObjectID casting needed for GDExtension.
	Transition t;
	t.from_state = p_from_state != nullptr ? ObjectID(p_from_state->get_instance_id()) : ObjectID();
	t.to_state = ObjectID(p_to_state->get_instance_id());
	t.event = p_event;
	t.guard = p_guard;
	t.condition = p_condition;
	t.priority = p_priority;
	transitions[key] = t;
	_bind_condition(p_condition);
}

void LimboHSM::remove_transition(LimboState *p_from_state, const StringName &p_event) {
	ERR_FAIL_COND_MSG(p_from_state != nullptr && p_from_state->get_parent() != this, "LimboHSM: Unable to remove a transition from a state that is not an immediate child of mine.");
	ERR_FAIL_COND_MSG(p_event == StringName(), "LimboHSM: Unable to remove a transition due to empty event string.");

	TransitionKey key = Transition::make_key(p_from_state, p_event);
	ERR_FAIL_COND_MSG(!transitions.has(key), "LimboHSM: Unable to remove a transition that does not exist.");
	transitions.erase(key);
}

#ifdef DEBUG_ENABLED
Array LimboHSM::_describe_row(const Transition &p_transition) {
	Array row;
	// Objects rather than paths: which node a state is, is a thing this side knows for
	// certain, and where it stands relative to the root is a question only the root can
	// answer -- and only the debugger has the root in hand.
	row.push_back(p_transition.from_state != ObjectID() ? Variant(OBJECT_DB_GET_INSTANCE(uint64_t(p_transition.from_state))) : Variant());
	row.push_back(OBJECT_DB_GET_INSTANCE(uint64_t(p_transition.to_state)));
	row.push_back(p_transition.event);
	// Appended after the original three so a reader that only knows those keeps working.
	row.push_back(p_transition.condition.is_valid() ? p_transition.condition->get_display_name() : String());
	row.push_back(p_transition.priority);
	return row;
}

Array LimboHSM::get_transition_rows() const {
	Array rows;
	for (const KeyValue<TransitionKey, Transition> &E : transitions) {
		if (!E.value.is_valid()) {
			continue;
		}
		rows.push_back(_describe_row(E.value));
	}
	for (int i = 0; i < polled.size(); i++) {
		if (polled[i].is_valid()) {
			rows.push_back(_describe_row(polled[i]));
		}
	}
	return rows;
}
#endif // DEBUG_ENABLED

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
		int priority = 0;

		Transition transition;
		_get_transition(active_state, p_event, transition);
		if (transition.is_valid() && transition.is_allowed()) {
			to_state = Object::cast_to<LimboState>(ObjectDB::get_instance(transition.to_state));
			priority = transition.priority;
		}
		if (to_state == nullptr) {
			// Get ANYSTATE transition.
			transition = Transition();
			_get_transition(nullptr, p_event, transition);
			if (transition.is_valid() && transition.is_allowed()) {
				to_state = Object::cast_to<LimboState>(ObjectDB::get_instance(transition.to_state));
				priority = transition.priority;
				if (to_state == active_state) {
					// Transitions to self are not allowed with ANYSTATE.
					to_state = nullptr;
				}
			}
		}
		// The leaving state is asked too (with the row's priority), the same as for a polled
		// row: a state that has declared it is busy is busy whatever asked it to leave.
		if (to_state != nullptr && _is_transition_permitted(to_state, priority)) {
			_take_transition(to_state);
			event_consumed = true;
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

	// Last, so that a condition's _setup() finds the blackboard populated by every plan on
	// the way down, the same as a state's _setup() does.
	for (KeyValue<TransitionKey, Transition> &E : transitions) {
		_bind_condition(E.value.condition);
	}
	for (int i = 0; i < polled.size(); i++) {
		_bind_condition(polled[i].condition);
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
#ifdef DEBUG_ENABLED
			// A reparent takes the deferred road below, which leaves `active` standing -- so
			// set_active() is never called on the way back in and the announcement above never
			// happens. Announcing is idempotent, so saying it twice on the pooling road costs
			// nothing; not saying it here loses the machine from the list for good.
			if (is_root() && is_active() && LimboDebugger::get_singleton() != nullptr) {
				LimboDebugger::get_singleton()->register_hsm(get_instance_id());
			}
#endif // DEBUG_ENABLED
		} break;
		case NOTIFICATION_EXIT_TREE: {
#ifdef DEBUG_ENABLED
			// Out of the tree is out of the list, whether it is a despawn or a reparent: what
			// the list is for is picking a machine on the map, and one that is nowhere is not
			// on it. The road back in is above.
			if (is_root() && LimboDebugger::get_singleton() != nullptr) {
				LimboDebugger::get_singleton()->unregister_hsm(get_instance_id());
			}
#endif // DEBUG_ENABLED
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
	ClassDB::bind_method(D_METHOD("add_transition", "from_state", "to_state", "event", "guard", "condition", "priority"), &LimboHSM::add_transition, DEFVAL(Callable()), DEFVAL(Ref<HSMCondition>()), DEFVAL(0));
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
