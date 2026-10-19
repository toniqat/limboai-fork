/**
 * limbo_hsm.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifndef LIMBO_HSM_H
#define LIMBO_HSM_H

#include "hsm_resource.h"
#include "limbo_state.h"

#define TransitionKey Pair<uint64_t, StringName>

class LimboHSM : public LimboState {
	GDCLASS(LimboHSM, LimboState);

public:
	enum UpdateMode : unsigned int {
		IDLE, // automatically call update() during NOTIFICATION_PROCESS
		PHYSICS, // automatically call update() during NOTIFICATION_PHYSICS
		MANUAL, // manually update state machine: user must call update(delta)
	};

private:
	struct TransitionKeyHasher {
		static uint32_t hash(const TransitionKey &P) {
			uint64_t h1 = HashMapHasherDefault::hash(P.first);
			uint64_t h2 = HashMapHasherDefault::hash(P.second);
			return hash_one_uint64((h1 << 32) | h2);
		}
	};

	struct Transition {
		ObjectID from_state;
		ObjectID to_state;
		StringName event;
		Callable guard;
		// Asked like `guard`, after it. Already this machine's own copy, bound to it.
		Ref<HSMCondition> condition;
		// Handed to the leaving state's _can_exit(); for a polled row it is also the order.
		int priority = 0;

		inline bool is_valid() const { return to_state != ObjectID(); }

		inline bool is_allowed() const {
			return (guard.is_null() || guard.call()) && (condition.is_null() || condition->check());
		}

		static _FORCE_INLINE_ TransitionKey make_key(LimboState *p_from_state, const StringName &p_event) {
			return TransitionKey(
					p_from_state != nullptr ? uint64_t(p_from_state->get_instance_id()) : 0,
					p_event);
		}
	};

	UpdateMode update_mode;
	LimboState *initial_state;
	LimboState *active_state;
	LimboState *previous_active;
	LimboState *next_active;
	bool updating = false;
	bool was_active = false;
	bool is_initiating_update = false;

	HashMap<TransitionKey, Transition, TransitionKeyHasher> transitions;

	// Rows with no event, asked on every update instead of waiting for a dispatch. Kept apart
	// from `transitions` because several of them may leave the same state (one per
	// condition), which a key of (from, event) cannot hold. In the order they were added,
	// which for a machine built from a file is the file order.
	Vector<Transition> polled;
	// Per leaving state (instance id), the indices into `polled` that apply to it, already in
	// evaluation order: priority descending, then that state's own rows before ANYSTATE rows,
	// then the order they were added. Filled the first time a state is polled and dropped
	// whenever a polled row is added or removed, so the sort is paid once per state rather
	// than once per update.
	HashMap<uint64_t, Vector<int>> polled_order;

	Ref<HierarchicalStateMachine> hsm;
	bool built_from_resource = false;

	void _get_transition(LimboState *p_from_state, const StringName &p_event, Transition &r_transition) const;
	const Vector<int> &_get_polled_order(LimboState *p_from_state);
	// Asks the active state's polled rows and takes the first that passes. True when that led
	// to a change of state, which is always made at once (never queued in `next_active`), so
	// the caller can update the state just entered in the same tick.
	bool _poll_transitions();
	// The leaving state's _can_exit() and the target's set_guard() callable, the two checks
	// every transition passes through whatever started it.
	bool _is_transition_permitted(LimboState *p_to_state, int p_priority);
	// Changes now, or after the update when one is running: the same immediacy a dispatched
	// event gets.
	void _take_transition(LimboState *p_to_state);
	void _bind_condition(const Ref<HSMCondition> &p_condition);
#ifdef DEBUG_ENABLED
	static Array _describe_row(const Transition &p_transition);
#endif // DEBUG_ENABLED
	void _exit_if_not_inside_tree();

	LimboState *_create_state_node(const Ref<HSMStateSpec> &p_spec);
	void _apply_state_properties(LimboState *p_state, const Ref<HSMStateSpec> &p_spec);
	void _instantiate_states(const Ref<HSMStateSpec> &p_spec, LimboHSM *p_machine, HashMap<uint64_t, LimboState *> &r_nodes);
	void _wire_transitions(const Ref<HSMStateSpec> &p_spec, LimboHSM *p_machine, const HashMap<uint64_t, LimboState *> &p_nodes);

protected:
	static void _bind_methods();

	void _notification(int p_what);
	void _validate_property(PropertyInfo &p_property) const;

	virtual void _initialize(Node *p_agent, const Ref<Blackboard> &p_blackboard) override;
	virtual bool _dispatch(const StringName &p_event, const Variant &p_cargo = Variant()) override;

	virtual void _enter() override;
	virtual void _exit() override;
	virtual void _update(double p_delta) override;

public:
	void set_update_mode(UpdateMode p_mode) { update_mode = p_mode; }
	UpdateMode get_update_mode() const { return update_mode; }

	void set_hsm(const Ref<HierarchicalStateMachine> &p_hsm);
	Ref<HierarchicalStateMachine> get_hsm() const { return hsm; }

	// Builds child states and transitions from the resource. Called automatically when
	// the node becomes ready with a resource set; safe to call once only.
	void build_from_resource();
	bool is_built_from_resource() const { return built_from_resource; }

	void set_active(bool p_active);

	void change_active_state(LimboState *p_state);

	LimboState *get_active_state() const { return active_state; }
	LimboState *get_previous_active_state() const { return previous_active; }
	LimboState *get_leaf_state() const;

	void set_initial_state(LimboState *p_state);
	LimboState *get_initial_state() const { return initial_state; }

	virtual void initialize(Node *p_agent, const Ref<Blackboard> &p_parent_scope = nullptr);

	void update(double p_delta);

	// An empty `p_event` adds a POLLED row: it is asked on every update while `p_from_state` is
	// active (or always, for ANYSTATE), and `p_condition` decides whether it is taken -- a null
	// condition always passes. Several polled rows may leave the same state; see _update() for
	// the order they are asked in. With an event, `p_condition` is an extra guard asked when the
	// event arrives. The condition is used as given (not copied) and is bound to this machine.
	void add_transition(LimboState *p_from_state, LimboState *p_to_state, const StringName &p_event, const Callable &p_guard = Callable(), const Ref<HSMCondition> &p_condition = Ref<HSMCondition>(), int p_priority = 0);
	void remove_transition(LimboState *p_from_state, const StringName &p_event);
	bool has_transition(LimboState *p_from_state, const StringName &p_event) const { return transitions.has(Transition::make_key(p_from_state, p_event)); }

#ifdef DEBUG_ENABLED
	// Every transition this machine holds, as [from, to, event, condition_name, priority],
	// where a nil `from` is ANYSTATE, an empty event is a polled row and an empty
	// condition_name means no condition. The table is private because nothing in a running
	// machine ever walks it -- only the debugger does, and reading it straight is what spares
	// the editor from having to infer an edge from the two states it happened to see on either
	// side of one. Guards are left out: a Callable is not a thing a picture can show.
	Array get_transition_rows() const;
#endif // DEBUG_ENABLED

	LimboState *anystate() const { return nullptr; }

	LimboHSM();
};

#endif // LIMBO_HSM_H
