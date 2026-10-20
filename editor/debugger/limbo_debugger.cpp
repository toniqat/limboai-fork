/**
 * limbo_debugger.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#include "limbo_debugger.h"

#include "../../bt/bt_instance.h"
#include "../../bt/bt_state.h"
#include "../../compat/debugger.h"
#include "../../compat/object.h"
#include "../../compat/scene_tree.h"
#include "../../hsm/limbo_hsm.h"
#include "../../util/limbo_string_names.h"
#include "behavior_tree_data.h"

#ifdef LIMBOAI_MODULE
#include "core/os/time.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/time.hpp>
#endif // LIMBOAI_GDEXTENSION

//**** LimboDebugger

LimboDebugger *LimboDebugger::singleton = nullptr;

LimboDebugger::LimboDebugger() {
	singleton = this;
#if defined(DEBUG_ENABLED) && defined(LIMBOAI_MODULE)
	EngineDebugger::register_message_capture("limboai", EngineDebugger::Capture(nullptr, LimboDebugger::parse_message));
#elif defined(DEBUG_ENABLED) && defined(LIMBOAI_GDEXTENSION)
	EngineDebugger::get_singleton()->register_message_capture("limboai", callable_mp(this, &LimboDebugger::parse_message_gdext));
#endif
}

LimboDebugger::~LimboDebugger() {
	singleton = nullptr;
}

void LimboDebugger::initialize() {
	if (IS_DEBUGGER_ACTIVE()) {
		memnew(LimboDebugger);
	}
}

void LimboDebugger::deinitialize() {
	if (singleton) {
		memdelete(singleton);
	}
}

void LimboDebugger::_bind_methods() {
}

#ifdef DEBUG_ENABLED
Error LimboDebugger::parse_message(void *p_user, const String &p_msg, const Array &p_args, bool &r_captured) {
	r_captured = true;
	if (p_msg == "track_bt_player") {
		singleton->_track_tree(p_args[0]);
	} else if (p_msg == "untrack_bt_player") {
		singleton->_untrack_tree();
	} else if (p_msg == "track_hsm") {
		singleton->_track_hsm(p_args[0]);
	} else if (p_msg == "untrack_hsm") {
		singleton->_untrack_hsm();
	} else if (p_msg == "start_session") {
		singleton->session_active = true;
		singleton->_send_active_bt_players();
		singleton->_send_active_hsms();
	} else if (p_msg == "track_blackboard") {
		singleton->_track_blackboard(p_args);
	} else if (p_msg == "untrack_blackboard") {
		singleton->_untrack_blackboard();
	} else if (p_msg == "stop_session") {
		singleton->session_active = false;
		singleton->_untrack_blackboard();
	} else {
		r_captured = false;
	}
	return OK;
}

#ifdef LIMBOAI_GDEXTENSION
bool LimboDebugger::parse_message_gdext(const String &p_msg, const Array &p_args) {
	bool r_captured;
	LimboDebugger::parse_message(nullptr, p_msg, p_args, r_captured);
	return r_captured;
}
#endif // LIMBOAI_GDEXTENSION

void LimboDebugger::register_bt_instance(uint64_t p_instance_id) {
	ERR_FAIL_COND(p_instance_id == 0);
	if (!IS_DEBUGGER_ACTIVE()) {
		return;
	}

	BTInstance *inst = Object::cast_to<BTInstance>(OBJECT_DB_GET_INSTANCE(p_instance_id));
	ERR_FAIL_NULL(inst);
	ERR_FAIL_COND(!inst->is_instance_valid());

	if (active_bt_instances.has(p_instance_id)) {
		return;
	}

	if (!inst->is_connected(LW_NAME(freed), callable_mp(this, &LimboDebugger::unregister_bt_instance).bind(p_instance_id))) {
		inst->connect(LW_NAME(freed), callable_mp(this, &LimboDebugger::unregister_bt_instance).bind(p_instance_id));
	}

	active_bt_instances.insert(p_instance_id);
	if (session_active) {
		_send_active_bt_players();
	}
}

void LimboDebugger::unregister_bt_instance(uint64_t p_instance_id) {
	if (!active_bt_instances.has(p_instance_id)) {
		return;
	}

	if (tracked_instance_id == p_instance_id) {
		_untrack_tree();
	}
	active_bt_instances.erase(p_instance_id);

	if (session_active) {
		_send_active_bt_players();
	}
}

bool LimboDebugger::is_active() const {
	return IS_DEBUGGER_ACTIVE();
}

void LimboDebugger::_track_tree(uint64_t p_instance_id) {
	ERR_FAIL_COND(p_instance_id == 0);
	ERR_FAIL_COND(!active_bt_instances.has(p_instance_id));

	_untrack_tree();

	tracked_instance_id = p_instance_id;

	BTInstance *inst = Object::cast_to<BTInstance>(OBJECT_DB_GET_INSTANCE(p_instance_id));
	ERR_FAIL_NULL(inst);
	inst->connect(LW_NAME(updated), callable_mp(this, &LimboDebugger::_on_bt_instance_updated).bind(p_instance_id));
}

void LimboDebugger::_untrack_tree() {
	if (tracked_instance_id == 0) {
		return;
	}

	BTInstance *inst = Object::cast_to<BTInstance>(OBJECT_DB_GET_INSTANCE(tracked_instance_id));
	if (inst) {
		inst->disconnect(LW_NAME(updated), callable_mp(this, &LimboDebugger::_on_bt_instance_updated));
	}
	tracked_instance_id = 0;
}

// Three per instance, not two. The third is the agent, and it is the only thing that says
// *whose* tree this is: a tree owned by a BTState is owned by a node deep inside a machine,
// so the path the editor used to list was a path through the machinery rather than a name
// anyone would recognize. Grouping is done here rather than on the far side because a node
// path is a string over there, and taking the character out of a string is guesswork.
void LimboDebugger::_send_active_bt_players() {
	Array arr;
	for (uint64_t instance_id : active_bt_instances) {
		arr.append(instance_id);
		BTInstance *inst = Object::cast_to<BTInstance>(OBJECT_DB_GET_INSTANCE(instance_id));
		if (inst == nullptr) {
			ERR_PRINT("LimboDebugger::_send_active_bt_players: Registered BTInstance not found (no longer exists?).");
			arr.append(NodePath());
			arr.append(NodePath());
			continue;
		}
		Node *owner_node = inst->get_owner_node();
		arr.append(owner_node ? owner_node->get_path() : NodePath());
		// Asked of the root task rather than of the owner: a BTState and a BTPlayer are two
		// unrelated classes with an agent each, and every task already carries the one its
		// tree was instantiated for.
		Node *agent = inst->get_root_task().is_valid() ? inst->get_root_task()->get_agent() : nullptr;
		arr.append(agent ? agent->get_path() : NodePath());
	}
	EngineDebugger::get_singleton()->send_message("limboai:active_bt_players", arr);
}

// ── State machines ────────────────────────────────────────────────────────────

void LimboDebugger::register_hsm(uint64_t p_instance_id) {
	ERR_FAIL_COND(p_instance_id == 0);
	if (!IS_DEBUGGER_ACTIVE() || active_hsms.has(p_instance_id)) {
		return;
	}
	active_hsms.insert(p_instance_id);
	if (session_active) {
		_send_active_hsms();
	}
}

void LimboDebugger::unregister_hsm(uint64_t p_instance_id) {
	if (!active_hsms.has(p_instance_id)) {
		return;
	}
	if (tracked_hsm_id == p_instance_id) {
		_untrack_hsm();
	}
	active_hsms.erase(p_instance_id);
	if (session_active) {
		_send_active_hsms();
	}
}

void LimboDebugger::_send_active_hsms() {
	Array arr;
	for (uint64_t instance_id : active_hsms) {
		LimboHSM *hsm = Object::cast_to<LimboHSM>(OBJECT_DB_GET_INSTANCE(instance_id));
		if (hsm == nullptr || !hsm->is_inside_tree()) {
			continue;
		}
		arr.append(instance_id);
		// The machine's own path as well as the agent's: the agent names the row, and the
		// machine's path is what a state's path hangs off -- which is how a BTState in the
		// picture is joined to the tree instance registered above.
		arr.append(hsm->get_path());
		Node *agent = hsm->get_agent();
		arr.append(agent ? agent->get_path() : NodePath());
	}
	EngineDebugger::get_singleton()->send_message("limboai:active_hsms", arr);
}

void LimboDebugger::_track_hsm(uint64_t p_instance_id) {
	tracked_hsm_id = p_instance_id;
	if (tracked_hsm_id != 0) {
		_send_hsm_structure();
	}
}

void LimboDebugger::_untrack_hsm() {
	tracked_hsm_id = 0;
}

void LimboDebugger::_send_hsm_structure() {
	LimboHSM *root = Object::cast_to<LimboHSM>(OBJECT_DB_GET_INSTANCE(tracked_hsm_id));
	if (root == nullptr) {
		tracked_hsm_id = 0;
		return;
	}

	Array states;
	Array transitions;
	_describe_machine(root, root, states, transitions);

	Array arr;
	arr.append(tracked_hsm_id);
	arr.append(states);
	arr.append(transitions);
	// Where it stands right now rides along with the shape. Without it a machine picked
	// mid-run shows nothing lit until it happens to move, and a machine that has settled
	// somewhere may not move again at all.
	arr.append(_active_chain(root));
	// The file it was built from, if it was built from one. Not so that the editor can draw
	// that file instead -- what is drawn has to be what is running -- but so that it can look
	// up where each of these states was put by hand. A machine put together in code has none,
	// and its picture is laid out by the fallback rows.
	arr.append(root->get_hsm().is_valid() ? root->get_hsm()->get_path() : String());
	EngineDebugger::get_singleton()->send_message("limboai:hsm_structure", arr);
}

// A state is named by its path down from the root. Names only have to be unique among
// siblings, and an instance id means nothing to a reader -- a relative path is neither.
void LimboDebugger::_describe_machine(LimboHSM *p_root, LimboState *p_machine, Array &r_states, Array &r_transitions) {
	LimboHSM *machine = Object::cast_to<LimboHSM>(p_machine);
	for (int i = 0; i < p_machine->get_child_count(); i++) {
		LimboState *child = Object::cast_to<LimboState>(p_machine->get_child(i));
		if (child == nullptr) {
			continue;
		}
		Dictionary state;
		state["path"] = String(p_root->get_path_to(child));
		state["parent"] = p_machine == p_root ? String() : String(p_root->get_path_to(p_machine));
		state["name"] = String(child->get_name());
		state["initial"] = machine != nullptr && machine->get_initial_state() == child;

		BTState *bt_state = Object::cast_to<BTState>(child);
		if (bt_state != nullptr) {
			// The tree's own path, which is what the node writes under itself. A built-in
			// tree has none, and the picture then says only that there is one.
			state["bt"] = bt_state->get_behavior_tree().is_valid() ? bt_state->get_behavior_tree()->get_path() : String();
		}
		r_states.append(state);

		_describe_machine(p_root, child, r_states, r_transitions);
	}

	if (machine == nullptr) {
		return;
	}
	Array rows = machine->get_transition_rows();
	for (int i = 0; i < rows.size(); i++) {
		Array row = rows[i];
		LimboState *from = Object::cast_to<LimboState>(row[0]);
		LimboState *to = Object::cast_to<LimboState>(row[1]);
		if (to == nullptr) {
			continue;
		}
		Array out;
		// An empty `from` is ANYSTATE, which is the same empty string the editor's picture
		// puts a ✱ node in for. Which machine the row belongs to is not sent: both ends of a
		// transition are children of it, so the destination's parent already says.
		out.append(from != nullptr ? String(p_root->get_path_to(from)) : String());
		out.append(String(p_root->get_path_to(to)));
		out.append(String(row[2]));
		// Condition name and priority, after the original three so an older reader still
		// finds what it knows where it knows it.
		out.append(row.size() > 3 ? String(row[3]) : String());
		out.append(row.size() > 4 ? int(row[4]) : 0);
		r_transitions.append(out);
	}
}

PackedStringArray LimboDebugger::_active_chain(LimboHSM *p_root) {
	PackedStringArray chain;
	LimboState *state = p_root->get_active_state();
	while (state != nullptr) {
		chain.append(String(p_root->get_path_to(state)));
		LimboHSM *nested = Object::cast_to<LimboHSM>(state);
		if (nested == nullptr) {
			break;
		}
		state = nested->get_active_state();
	}
	return chain;
}

void LimboDebugger::hsm_state_changed(LimboHSM *p_machine, LimboState *p_from) {
	if (!session_active || p_machine == nullptr) {
		return;
	}
	LimboState *root_state = p_machine->get_root();
	LimboHSM *root = Object::cast_to<LimboHSM>(root_state);
	if (root == nullptr) {
		return;
	}
	uint64_t root_id = uint64_t(root->get_instance_id());
	if (!active_hsms.has(root_id)) {
		return;
	}

	// Everybody says *that* they moved; only the one being watched says what to. Two messages
	// rather than one widened, because the two are of different sizes and go to different
	// places: this one is a number, it is sent for every machine on the map, and all the far
	// side does with it is make a row blink. Without it the list of characters looks exactly
	// the same whether the map is busy or asleep, and the one thing a list of characters is
	// well placed to say is which of them are doing something.
	Array ping;
	ping.append(root_id);
	EngineDebugger::get_singleton()->send_message("limboai:hsm_activity", ping);

	if (root_id != tracked_hsm_id) {
		return;
	}

	Array arr;
	arr.append(tracked_hsm_id);
	LimboState *current = p_machine->get_active_state();
	// `p_from` is what the machine was actually standing on a moment ago, which is not
	// get_previous_active_state(): that one keeps its value while the machine is out, so a
	// machine stepped into a second time would report the state it left the *first* time and
	// draw an edge that was never taken. An empty `from` here is not ANYSTATE either -- it is
	// "this machine was just entered", and drawing those two the same would sprout a line out
	// of ✱ on every entry into a nested machine.
	arr.append(p_from != nullptr ? String(root->get_path_to(p_from)) : String());
	arr.append(current != nullptr ? String(root->get_path_to(current)) : String());
	arr.append(_active_chain(root));
	EngineDebugger::get_singleton()->send_message("limboai:hsm_state", arr);
}

void LimboDebugger::_on_bt_instance_updated(int _status, uint64_t p_instance_id) {
	if (p_instance_id != tracked_instance_id) {
		return;
	}
	BTInstance *inst = Object::cast_to<BTInstance>(OBJECT_DB_GET_INSTANCE(p_instance_id));
	ERR_FAIL_NULL(inst);
	Array arr = BehaviorTreeData::serialize(inst);
	EngineDebugger::get_singleton()->send_message("limboai:bt_update", arr);
}

// ── Blackboard ────────────────────────────────────────────────────────────────

namespace {

// Between the names of a path into a value: "record", then "record<US>ai". A character no key
// is going to contain, which a slash or a dot is not: record keys hold both.
const char32_t BB_PATH_SEPARATOR = 0x1f;
// How much of a container is sent once it is unfolded, and how deep unfolding may go. Both
// keep one careless click on a big dictionary from sending the whole save.
const int BB_MAX_CHILDREN = 200;
const int BB_MAX_DEPTH = 16;
// How much of a value is written out. The pane is a column, not a reader.
const int BB_MAX_TEXT = 160;
// Row flags, the third field of a row. Mirrored in limbo_debugger_plugin.cpp.
const int BB_FLAG_BOUND = 1;
const int BB_FLAG_LINKED = 2;
const int BB_FLAG_CONTAINER = 4;
const int BB_FLAG_TRUNCATED = 8;

String _clip(const String &p_text) {
	return p_text.length() > BB_MAX_TEXT ? p_text.substr(0, BB_MAX_TEXT) + String("...") : p_text;
}

// An object is said by what it is and, for a resource from a file, which file: a helper
// script shows up as its path rather than as one more "<GDScript#...>".
String _object_text(const Variant &p_value) {
	Object *obj = p_value.get_validated_object();
	if (obj == nullptr) {
		return String("null");
	}
	Resource *res = Object::cast_to<Resource>(obj);
	if (res != nullptr && !res->get_path().is_empty()) {
		return vformat("%s  %s", obj->get_class(), res->get_path());
	}
	return p_value.stringify();
}

} // namespace

void LimboDebugger::_track_blackboard(const Array &p_args) {
	bb_interval_msec = p_args.size() > 0 ? MAX(int64_t(p_args[0]), int64_t(100)) : 100;
	bb_expanded.clear();
	if (p_args.size() > 1) {
		PackedStringArray paths = p_args[1];
		for (int i = 0; i < paths.size(); i++) {
			bb_expanded.insert(paths[i]);
		}
	}
	// Due at once: an unfolded row should not wait out the interval to show what is in it.
	bb_last_sent_msec = 0;
	if (bb_tracking) {
		return;
	}
	SceneTree *tree = SCENE_TREE();
	ERR_FAIL_NULL(tree);
	bb_tracking = true;
	tree->connect("process_frame", callable_mp(this, &LimboDebugger::_on_process_frame));
}

void LimboDebugger::_untrack_blackboard() {
	if (!bb_tracking) {
		return;
	}
	bb_tracking = false;
	SceneTree *tree = SCENE_TREE();
	if (tree != nullptr && tree->is_connected("process_frame", callable_mp(this, &LimboDebugger::_on_process_frame))) {
		tree->disconnect("process_frame", callable_mp(this, &LimboDebugger::_on_process_frame));
	}
}

// Every frame rather than on a tree update, because a machine standing in a state with no
// tree has a blackboard too, and nothing else would ever send it.
void LimboDebugger::_on_process_frame() {
	if (!session_active || !bb_tracking) {
		return;
	}
	uint64_t now = Time::get_singleton()->get_ticks_msec();
	if (bb_last_sent_msec != 0 && now - bb_last_sent_msec < bb_interval_msec) {
		return;
	}
	bb_last_sent_msec = now;
	_send_blackboard();
}

// One row: [key, type, flags, text, children]. Children are rows of the same shape, filled in
// only for a container the editor has unfolded.
Array LimboDebugger::_describe_value(const String &p_key, const Variant &p_value, const String &p_path, int p_depth) const {
	int flags = 0;
	Array children;
	String text;
	if (p_value.get_type() == Variant::DICTIONARY || p_value.get_type() == Variant::ARRAY) {
		flags |= BB_FLAG_CONTAINER;
		bool is_dict = p_value.get_type() == Variant::DICTIONARY;
		Dictionary dict = is_dict ? Dictionary(p_value) : Dictionary();
		Array arr = is_dict ? Array() : Array(p_value);
		int size = is_dict ? dict.size() : arr.size();
		text = vformat("%s (%d)", is_dict ? "Dictionary" : "Array", size);
		if (p_depth < BB_MAX_DEPTH && bb_expanded.has(p_path)) {
			Array keys = is_dict ? dict.keys() : Array();
			int count = MIN(size, BB_MAX_CHILDREN);
			for (int i = 0; i < count; i++) {
				String key = is_dict ? String(keys[i]) : itos(i);
				Variant value = is_dict ? dict[keys[i]] : arr[i];
				children.append(_describe_value(key, value, p_path + String::chr(BB_PATH_SEPARATOR) + key, p_depth + 1));
			}
			if (count < size) {
				flags |= BB_FLAG_TRUNCATED;
			}
		}
	} else if (p_value.get_type() == Variant::OBJECT) {
		text = _object_text(p_value);
	} else if (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME) {
		text = String("\"") + String(p_value) + String("\"");
	} else {
		text = p_value.stringify();
	}
	Array row;
	row.append(p_key);
	row.append(int(p_value.get_type()));
	row.append(flags);
	row.append(_clip(text));
	row.append(children);
	return row;
}

// [source id, scopes]; a scope is [label, rows], innermost first. A variable that shares its
// data with one further out (a plan mapping or the same-name link) is listed where it is
// declared, flagged linked, with the label of the outermost scope sharing it in place of the
// value: the value itself is sent once, there.
void LimboDebugger::_send_blackboard() {
	uint64_t source_id = 0;
	Ref<Blackboard> bb;
	String first_label;
	BTInstance *inst = tracked_instance_id != 0 ? Object::cast_to<BTInstance>(OBJECT_DB_GET_INSTANCE(tracked_instance_id)) : nullptr;
	if (inst != nullptr && inst->get_blackboard().is_valid()) {
		source_id = tracked_instance_id;
		bb = inst->get_blackboard();
		first_label = vformat("Tree  %s", inst->get_source_bt_path().get_file());
	} else {
		LimboHSM *hsm = tracked_hsm_id != 0 ? Object::cast_to<LimboHSM>(OBJECT_DB_GET_INSTANCE(tracked_hsm_id)) : nullptr;
		if (hsm != nullptr && hsm->get_blackboard().is_valid()) {
			source_id = tracked_hsm_id;
			bb = hsm->get_blackboard();
		}
	}

	Vector<Ref<Blackboard>> chain;
	for (Ref<Blackboard> scope = bb; scope.is_valid(); scope = scope->get_parent()) {
		chain.push_back(scope);
	}
	Vector<String> labels;
	for (int i = 0; i < chain.size(); i++) {
		if (i == chain.size() - 1) {
			labels.push_back("Root");
		} else if (i == 0 && !first_label.is_empty()) {
			labels.push_back(first_label);
		} else {
			labels.push_back(vformat("Scope %d", i));
		}
	}

	Array scopes;
	for (int i = 0; i < chain.size(); i++) {
		// Sorted, so a row stays where it was between updates and is found where the eye
		// expects it.
		TypedArray<StringName> listed = chain[i]->list_vars();
		Vector<String> names;
		for (int n = 0; n < listed.size(); n++) {
			names.push_back(String(StringName(listed[n])));
		}
		names.sort();

		Array rows;
		for (int n = 0; n < names.size(); n++) {
			const BBVariable *var = chain[i]->get_local_variable(names[n]);
			if (var == nullptr) {
				continue;
			}
			int shared_with = -1;
			for (int j = i + 1; j < chain.size(); j++) {
				const BBVariable *outer = chain[j]->get_local_variable(names[n]);
				if (outer != nullptr && outer->shares_data_with(*var)) {
					shared_with = j;
				}
			}
			Array row;
			if (shared_with >= 0) {
				row.append(names[n]);
				row.append(int(var->get_type()));
				row.append(BB_FLAG_LINKED | (var->is_bound() ? BB_FLAG_BOUND : 0));
				row.append(labels[shared_with]);
				row.append(Array());
			} else {
				row = _describe_value(names[n], var->get_value(), names[n], 0);
				if (var->is_bound()) {
					row[2] = int(row[2]) | BB_FLAG_BOUND;
				}
			}
			rows.append(row);
		}
		Array scope;
		scope.append(labels[i]);
		scope.append(rows);
		scopes.append(scope);
	}

	Array arr;
	arr.append(source_id);
	arr.append(scopes);
	EngineDebugger::get_singleton()->send_message("limboai:blackboard", arr);
}

#endif // ! DEBUG_ENABLED
