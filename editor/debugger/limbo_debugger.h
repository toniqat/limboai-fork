/**
 * limbo_debugger.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifndef LIMBO_DEBUGGER_H
#define LIMBO_DEBUGGER_H

#ifdef LIMBOAI_MODULE
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/templates/hash_set.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/templates/hash_set.hpp>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

// Declared rather than included: limbo_hsm.cpp includes this header to announce itself, and
// pulling the machine in here would make the two headers depend on each other for nothing --
// only the bodies below ever touch a machine.
class LimboHSM;
class LimboState;

class LimboDebugger : public Object {
	GDCLASS(LimboDebugger, Object);

private:
	static LimboDebugger *singleton;

	LimboDebugger();

public:
	static void initialize();
	static void deinitialize();
	_FORCE_INLINE_ static LimboDebugger *get_singleton() { return singleton; }

	~LimboDebugger();

protected:
	static void _bind_methods();

#ifdef DEBUG_ENABLED
private:
	HashSet<uint64_t> active_bt_instances;
	uint64_t tracked_instance_id = 0;
	// Root machines only, and only while they are running. What the editor picks from is a
	// list of the characters on the map, and a machine nested inside another is not one of
	// them -- it is part of the picture of the root.
	HashSet<uint64_t> active_hsms;
	uint64_t tracked_hsm_id = 0;
	bool session_active = false;

	void _track_tree(uint64_t p_instance_id);
	void _untrack_tree();
	void _send_active_bt_players();

	void _track_hsm(uint64_t p_instance_id);
	void _untrack_hsm();
	void _send_active_hsms();
	// The whole shape of the tracked machine: its states, its transitions, and where it
	// stands right now. Once per pick, not per frame -- a machine changes on an event, and
	// the events are what `hsm_state` below is.
	void _send_hsm_structure();
	static void _describe_machine(LimboHSM *p_root, LimboState *p_machine, Array &r_states, Array &r_transitions);
	// The root's active state, then that one's if it is a machine, and so on down. The leaf
	// alone would lose how many machines were passed through to reach it, and "Wary inside
	// Combat" is two facts the picture needs both of.
	static PackedStringArray _active_chain(LimboHSM *p_root);

	void _on_bt_instance_updated(int status, uint64_t p_instance_id);

public:
	static Error parse_message(void *p_user, const String &p_msg, const Array &p_args, bool &r_captured);
#ifdef LIMBOAI_GDEXTENSION
	bool parse_message_gdext(const String &p_msg, const Array &p_args);
#endif

	void register_bt_instance(uint64_t p_instance_id);
	void unregister_bt_instance(uint64_t p_instance_id);

	void register_hsm(uint64_t p_instance_id);
	void unregister_hsm(uint64_t p_instance_id);
	// Every machine says so, nested ones included, and all but the tracked root drop out on
	// the first line. `p_from` is handed in rather than read back off the machine -- see the
	// call site in LimboHSM::change_active_state() and the note in the body.
	void hsm_state_changed(LimboHSM *p_machine, LimboState *p_from);

	bool is_active() const;

#endif // ! DEBUG_ENABLED
};
#endif // LIMBO_DEBUGGER_H
