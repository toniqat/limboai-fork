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
// The three classes live in one header because HSMTransition refers to HSMStateSpec and
// HSMStateSpec owns HSMTransition: splitting them would need a forward declaration whose
// only purpose is to undo the split.

class HSMStateSpec;

// One edge. A null `from` means ANYSTATE -- "there is no state being left".
class HSMTransition : public Resource {
	GDCLASS(HSMTransition, Resource);

private:
	Ref<HSMStateSpec> from;
	Ref<HSMStateSpec> to;
	StringName event;
	String note;

protected:
	static void _bind_methods();

public:
	void set_from(const Ref<HSMStateSpec> &p_state);
	Ref<HSMStateSpec> get_from() const;

	void set_to(const Ref<HSMStateSpec> &p_state);
	Ref<HSMStateSpec> get_to() const;

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
class HSMStateSpec : public Resource {
	GDCLASS(HSMStateSpec, Resource);

private:
	StringName state_name = "State";
	Ref<Script> state_script;
	Ref<BehaviorTree> behavior_tree;
	TypedArray<HSMStateSpec> children;
	Ref<HSMStateSpec> initial_child;
	TypedArray<HSMTransition> transitions;
	Vector2 graph_position;
	// Only a machine's node has one, and only when it was resized by hand: its size is
	// otherwise worked out from the states standing in it. Zero means "whatever fits",
	// which is why it is not a size the editor writes on every layout.
	Vector2 graph_size;

	// Transient, and only ever set on the tree HierarchicalStateMachine::get_effective_root()
	// builds -- neither is a property, so neither is written to a file. `origin` is the
	// state *in the file being edited* that this one stands for, which is where an edit
	// has to land; `inherited` says a base file contributed it. A state that is both is
	// one the child overlays: the name and script are the base's, the children are not.
	Ref<HSMStateSpec> origin;
	bool inherited = false;

protected:
	static void _bind_methods();

public:
	void set_state_name(const StringName &p_name);
	StringName get_state_name() const { return state_name; }

	void set_state_script(const Ref<Script> &p_script);
	Ref<Script> get_state_script() const { return state_script; }

	void set_behavior_tree(const Ref<BehaviorTree> &p_tree);
	Ref<BehaviorTree> get_behavior_tree() const { return behavior_tree; }

	void set_children(const TypedArray<HSMStateSpec> &p_children);
	TypedArray<HSMStateSpec> get_children() const { return children; }

	void set_initial_child(const Ref<HSMStateSpec> &p_state);
	Ref<HSMStateSpec> get_initial_child() const;

	void set_transitions(const TypedArray<HSMTransition> &p_transitions);
	TypedArray<HSMTransition> get_transitions() const { return transitions; }

	void set_graph_position(const Vector2 &p_position);
	Vector2 get_graph_position() const { return graph_position; }

	void set_graph_size(const Vector2 &p_size);
	Vector2 get_graph_size() const { return graph_size; }

	// The declared value, without get_initial_child()'s fall back to the first child.
	// Merging needs to tell "this file says nothing" from "this file says the first one".
	Ref<HSMStateSpec> get_declared_initial_child() const { return initial_child; }

	// A state with children is a machine. Nothing else marks it.
	bool is_machine() const { return children.size() > 0; }

	// Which node class this state becomes. The script wins when there is one, because
	// the script's own base already answers the question -- deriving it twice would let
	// the two answers disagree.
	StringName resolve_base_type() const;

	void set_origin(const Ref<HSMStateSpec> &p_state) { origin = p_state; }
	Ref<HSMStateSpec> get_origin() const { return origin; }

	void set_inherited(bool p_inherited) { inherited = p_inherited; }
	bool is_inherited() const { return inherited; }

	HSMStateSpec();
	~HSMStateSpec();
};

// The file. Holds the root state, whose children become the children of the LimboHSM
// node that runs it.
class HierarchicalStateMachine : public Resource {
	GDCLASS(HierarchicalStateMachine, Resource);

private:
	String description;
	Ref<HSMStateSpec> root;
	Ref<HierarchicalStateMachine> base;

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

	void set_root(const Ref<HSMStateSpec> &p_state);
	Ref<HSMStateSpec> get_root() const { return root; }

	void set_base(const Ref<HierarchicalStateMachine> &p_base);
	Ref<HierarchicalStateMachine> get_base() const { return base; }

	// The machine as it actually runs: the base's states, plus whatever this file adds
	// on top. Without a base this is `root` itself -- there is nothing to merge, and
	// handing back a copy would make every edit land on something about to be thrown away.
	//
	// A child file can only add. What it says about a state the base already has, it says
	// by naming it: inside one file a state is identified by reference, and across the
	// boundary by name, because a sub-resource of another file cannot be referenced at all.
	//
	// WARNING: with a base this merges afresh on every call, so two calls hand back two
	// unrelated trees. Ask once and hold the answer -- comparing a state from one against
	// a transition from the other silently matches nothing, and a machine that inherits
	// nothing returns `root` itself, so the mistake only shows on the ones that do.
	Ref<HSMStateSpec> get_effective_root() const;

	HierarchicalStateMachine();
	~HierarchicalStateMachine();
};

#endif // HSM_RESOURCE_H
