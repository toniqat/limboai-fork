/**
 * hsm_condition.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifndef HSM_CONDITION_H
#define HSM_CONDITION_H

#include "../blackboard/blackboard.h"

#ifdef LIMBOAI_MODULE
#include "core/io/resource.h"
#include "scene/main/node.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

// The question a transition asks before it is taken: "is it time to go there?".
//
// A resource rather than a Callable, because it has to live in the .tres beside the row it
// guards: a Callable names a method on an object that only exists while the game runs, and a
// file can only point at things that exist on disk. Being a resource also lets the editor show
// the condition and its exported settings in the Inspector, the same way a BTTask is shown.
//
// One file is shared by every machine built from it, so a running machine never asks the
// condition in the file. LimboHSM duplicates it when it builds, and binds the copy to itself:
// `blackboard`, `agent` and `machine` are then the ones of that one machine, and whatever the
// script keeps between checks belongs to that machine alone.
class HSMCondition : public Resource {
	GDCLASS(HSMCondition, Resource);

private:
	Ref<Blackboard> blackboard;
	Node *agent = nullptr;
	// The LimboHSM that owns the row and evaluates it. Typed as Node so this header does not
	// have to know the machine, which already has to know this class.
	Node *machine = nullptr;
	bool bound = false;

protected:
	static void _bind_methods();

	virtual String _generate_name();
	virtual void _setup() {}
	// Without a script there is nothing to ask, and a placeholder that quietly passed would
	// take the transition on its own. A condition that was never written never passes.
	virtual bool _check() { return false; }

	GDVIRTUAL0RC(String, _generate_name);
	GDVIRTUAL0(_setup);
	GDVIRTUAL0RC(bool, _check);

#ifdef LIMBOAI_GDEXTENSION
	String _to_string() const { return "<" + get_class() + "#" + itos(get_instance_id()) + ">"; }
#endif

public:
	Ref<Blackboard> get_blackboard() const { return blackboard; }
	Node *get_agent() const { return agent; }
	Node *get_machine() const { return machine; }
	bool is_bound() const { return bound; }

	// Hands the condition the machine it now answers for, then runs `_setup()` once. Called by
	// LimboHSM when it is initialized, or straight away for a row added to a machine that
	// already is. Binding twice is refused: `_setup()` is promised to run once.
	void bind_to(Node *p_machine, Node *p_agent, const Ref<Blackboard> &p_blackboard);

	// The name the editor shows for this condition: the script's `_generate_name()` when the
	// script is a tool script, else the script file name in PascalCase, else the class name.
	String get_display_name();

	// Asks the condition. Side-effect free by convention: the machine may ask every row from
	// the active state on every update, and how often that happens is not the condition's
	// business.
	bool check();

	HSMCondition();
};

#endif // HSM_CONDITION_H
