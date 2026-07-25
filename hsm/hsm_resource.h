/**
 * hsm_resource.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifndef HSM_RESOURCE_H
#define HSM_RESOURCE_H

#include "../bt/behavior_tree.h"

#ifdef LIMBOAI_MODULE
#include "core/io/resource.h"
#include "core/object/script_language.h"
#include "core/variant/typed_array.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/variant/typed_array.hpp>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

// A state machine described by a resource file, so that it has an original to draw
// and edit while the game is not running -- the same thing BehaviorTree is for BT.
//
// The three classes live in one header because HSMTransition refers to HSMState and
// HSMState owns HSMTransition: splitting them would need a forward declaration whose
// only purpose is to undo the split.

class HSMState;

// One edge. A null `from` means ANYSTATE -- "there is no state being left".
class HSMTransition : public Resource {
	GDCLASS(HSMTransition, Resource);

private:
	Ref<HSMState> from;
	Ref<HSMState> to;
	StringName event;
	String note;

protected:
	static void _bind_methods();

public:
	void set_from(const Ref<HSMState> &p_state);
	Ref<HSMState> get_from() const;

	void set_to(const Ref<HSMState> &p_state);
	Ref<HSMState> get_to() const;

	void set_event(const StringName &p_event);
	StringName get_event() const { return event; }

	void set_note(const String &p_note);
	String get_note() const { return note; }

	HSMTransition();
	~HSMTransition();
};

// One state. A state that has children is a machine -- there is no separate type for
// nesting, which is what makes "a state may contain a behavior tree" and "a state may
// contain other states" the same sentence.
class HSMState : public Resource {
	GDCLASS(HSMState, Resource);

private:
	StringName state_name = "State";
	Ref<Script> state_script;
	Ref<BehaviorTree> behavior_tree;
	TypedArray<HSMState> children;
	Ref<HSMState> initial_child;
	TypedArray<HSMTransition> transitions;
	Vector2 graph_position;

protected:
	static void _bind_methods();

public:
	void set_state_name(const StringName &p_name);
	StringName get_state_name() const { return state_name; }

	void set_state_script(const Ref<Script> &p_script);
	Ref<Script> get_state_script() const { return state_script; }

	void set_behavior_tree(const Ref<BehaviorTree> &p_tree);
	Ref<BehaviorTree> get_behavior_tree() const { return behavior_tree; }

	void set_children(const TypedArray<HSMState> &p_children);
	TypedArray<HSMState> get_children() const { return children; }

	void set_initial_child(const Ref<HSMState> &p_state);
	Ref<HSMState> get_initial_child() const;

	void set_transitions(const TypedArray<HSMTransition> &p_transitions);
	TypedArray<HSMTransition> get_transitions() const { return transitions; }

	void set_graph_position(const Vector2 &p_position);
	Vector2 get_graph_position() const { return graph_position; }

	// A state with children is a machine. Nothing else marks it.
	bool is_machine() const { return children.size() > 0; }

	// Which node class this state becomes. The script wins when there is one, because
	// the script's own base already answers the question -- deriving it twice would let
	// the two answers disagree.
	StringName resolve_base_type() const;

	HSMState();
	~HSMState();
};

// The file. Holds the root state, whose children become the children of the LimboHSM
// node that runs it.
class HierarchicalStateMachine : public Resource {
	GDCLASS(HierarchicalStateMachine, Resource);

private:
	String description;
	Ref<HSMState> root;

protected:
	static void _bind_methods();

#ifdef LIMBOAI_GDEXTENSION
	String _to_string() const { return "<" + get_class() + "#" + itos(get_instance_id()) + ">"; }
#endif

public:
#ifdef LIMBOAI_MODULE
	virtual bool editor_can_reload_from_file() override { return false; }
#endif

	void set_description(const String &p_value);
	String get_description() const { return description; }

	void set_root(const Ref<HSMState> &p_state);
	Ref<HSMState> get_root() const { return root; }

	HierarchicalStateMachine();
	~HierarchicalStateMachine();
};

#endif // HSM_RESOURCE_H
