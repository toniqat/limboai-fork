/**
 * limbo_debugger_plugin.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#include "limbo_debugger_plugin.h"

#include "../../bt/behavior_tree.h"
#include "../../compat/editor.h"
#include "../../compat/editor_paths.h"
#include "../../compat/editor_scale.h"
#include "../../compat/resource_loader.h"
#include "../../compat/translation.h"
#include "../../editor/debugger/behavior_tree_data.h"
#include "../../editor/debugger/behavior_tree_view.h"
#include "../../util/limbo_string_names.h"
#include "../../util/limbo_utility.h"

#ifdef LIMBOAI_MODULE
#include "core/io/config_file.h"
#include "editor/docks/filesystem_dock.h"
#include "scene/gui/separator.h"
#include "scene/gui/tab_container.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/file_system_dock.hpp>
#include <godot_cpp/classes/tab_container.hpp>
#include <godot_cpp/classes/v_separator.hpp>
#endif // LIMBOAI_GDEXTENSION

//**** LimboDebuggerTab

namespace {

// Rebuilds the machine the game reported into the resource the main screen edits, so that the
// same view can draw it. It is not the file the machine was built from: what is drawn has to
// be what is running, and a file may have been edited since the game was launched -- worse, a
// machine put together in code has no file at all.
Ref<HierarchicalStateMachine> _machine_from_report(const String &p_root_name, const Array &p_states, const Array &p_transitions,
		HashMap<String, Ref<HSMStateSpec>> &r_by_path, HashMap<uint64_t, String> &r_path_by_state) {
	Ref<HierarchicalStateMachine> machine;
	machine.instantiate();
	Ref<HSMStateSpec> root;
	root.instantiate();
	root->set_state_name(StringName(p_root_name));
	machine->set_root(root);

	r_by_path.clear();
	r_path_by_state.clear();
	// The root is the empty path: every state's path is written down from it, so the machine
	// itself is the one with nothing to say.
	r_by_path[String()] = root;
	r_path_by_state[root->get_instance_id()] = String();

	HashMap<String, String> parent_of;

	for (int i = 0; i < p_states.size(); i++) {
		Dictionary state = p_states[i];
		String path = state.get("path", String());
		if (path.is_empty()) {
			continue;
		}
		Ref<HSMStateSpec> spec;
		spec.instantiate();
		spec->set_state_name(StringName(state.get("name", path)));
		if (state.has("bt")) {
			String bt_path = state["bt"];
			Ref<BehaviorTree> tree = bt_path.is_empty() ? Ref<BehaviorTree>() : RESOURCE_LOAD(bt_path, "BehaviorTree");
			if (tree.is_null()) {
				// A tree built into the scene it runs in has no file to load, and one whose
				// file has since moved has none either. An empty stand-in still answers the
				// only question the picture asks a tree: whether there is one.
				tree.instantiate();
			}
			spec->set_behavior_tree(tree);
		}
		r_by_path[path] = spec;
		r_path_by_state[spec->get_instance_id()] = path;
		parent_of[path] = state.get("parent", String());
	}

	// Hung on their parents in the order they were reported, which is the order the machine
	// holds them in -- and that order is what the fallback layout puts them in rows by.
	for (int i = 0; i < p_states.size(); i++) {
		Dictionary state = p_states[i];
		String path = state.get("path", String());
		Ref<HSMStateSpec> spec = r_by_path.has(path) ? r_by_path[path] : Ref<HSMStateSpec>();
		if (spec.is_null()) {
			continue;
		}
		String parent_path = parent_of.has(path) ? parent_of[path] : String();
		Ref<HSMStateSpec> parent = r_by_path.has(parent_path) ? r_by_path[parent_path] : root;
		TypedArray<HSMStateSpec> children = parent->get_children();
		children.append(spec);
		parent->set_children(children);
		if (bool(state.get("initial", false))) {
			parent->set_initial_child(spec);
		}
	}

	for (int i = 0; i < p_transitions.size(); i++) {
		Array row = p_transitions[i];
		if (row.size() < 3) {
			continue;
		}
		String from_path = row[0];
		String to_path = row[1];
		if (!r_by_path.has(to_path)) {
			continue;
		}
		Ref<HSMStateSpec> to = r_by_path[to_path];
		// An empty `from` is ANYSTATE, which the picture puts a node of its own in for.
		Ref<HSMStateSpec> from;
		if (!from_path.is_empty()) {
			if (!r_by_path.has(from_path)) {
				continue;
			}
			from = r_by_path[from_path];
		}
		// Which machine owns the row is not reported and does not need to be: both ends of a
		// transition are children of it, so the destination's parent is the answer.
		String owner_path = parent_of.has(to_path) ? parent_of[to_path] : String();
		Ref<HSMStateSpec> owner = r_by_path.has(owner_path) ? r_by_path[owner_path] : root;

		Ref<HSMTransition> transition;
		transition.instantiate();
		transition->set_from(from);
		transition->set_to(to);
		transition->set_event(StringName(row[2]));
		if (row.size() > 4) {
			transition->set_priority(int(row[4]));
		}
		if (row.size() > 3 && !String(row[3]).is_empty()) {
			// Only the name travels: the condition is a script object living in the game, and
			// what the picture needs from it is something to call the row by.
			transition->set_note(vformat("Condition: %s", String(row[3])));
		}
		TypedArray<HSMTransition> rows = owner->get_transitions();
		rows.append(transition);
		owner->set_transitions(rows);
	}

	return machine;
}

// Lays the arrangement of the file a machine was built from over the machine that was
// reported. Only the three figures that say where things stand are taken: what states there
// are, how they join, and which of them it is standing in stay the game's to answer, so a
// file edited since launch can move the boxes about but cannot make the picture lie.
//
// Matched by the path of names down from the root, which is the one thing the two sides
// certainly agree on -- LimboHSM names every node after the state it built it from. A state
// the file no longer has simply keeps the fallback row, which is also what a machine put
// together in code gets for all of them.
void _apply_authored_layout(const String &p_source_path, HashMap<String, Ref<HSMStateSpec>> &r_by_path) {
	if (p_source_path.is_empty()) {
		return;
	}
	Ref<HierarchicalStateMachine> file = RESOURCE_LOAD(p_source_path, "HierarchicalStateMachine");
	if (file.is_null()) {
		return;
	}
	// Asked once and held: with a base, every call merges afresh and hands back a tree
	// unrelated to the one the call before it handed back.
	Ref<HSMStateSpec> root = file->get_effective_root();
	if (root.is_null()) {
		return;
	}
	for (KeyValue<String, Ref<HSMStateSpec>> &entry : r_by_path) {
		Ref<HSMStateSpec> authored = root;
		PackedStringArray names = entry.key.split("/", false);
		for (int i = 0; i < names.size() && authored.is_valid(); i++) {
			Ref<HSMStateSpec> found;
			TypedArray<HSMStateSpec> children = authored->get_children();
			for (int j = 0; j < children.size(); j++) {
				Ref<HSMStateSpec> child = children[j];
				if (child.is_valid() && String(child->get_state_name()) == names[i]) {
					found = child;
					break;
				}
			}
			authored = found;
		}
		if (authored.is_null()) {
			continue;
		}
		entry.value->set_graph_position(authored->get_graph_position());
		entry.value->set_graph_size(authored->get_graph_size());
		entry.value->set_anystate_graph_position(authored->get_anystate_graph_position());
	}
}

// A row goes white when its character's machine moves, and fades back. The same shape the
// graph's edges fade with, and for the same reason: a mark that is switched off by the next
// one reads as a fault when the next one is a tenth of a second later, which for a machine
// being poked at is most of them.
const float BLINK_SECONDS = 0.5;
// Slight. It is a row in a list, not a thing being pointed at -- the character being read is
// still the one with the selection on it, and a flash that outshone that would be answering
// "who is busy" over "who am I looking at".
const float BLINK_ALPHA = 0.3;

// What a path is called: its last name. The path itself is the tooltip -- it is what tells
// two of a kind apart, and it is also far too long to read at a glance in a list.
String _leaf_of(const String &p_path) {
	String leaf = p_path.get_file();
	return leaf.is_empty() ? p_path : leaf;
}

// The blackboard message, as limbo_debugger.cpp writes it: a row is [key, type, flags, text,
// children], and a path into a value joins the keys with this separator.
const char32_t BB_PATH_SEPARATOR = 0x1f;
const int BB_FLAG_BOUND = 1;
const int BB_FLAG_LINKED = 2;
const int BB_FLAG_CONTAINER = 4;
const int BB_FLAG_TRUNCATED = 8;

} // namespace

struct LimboDebuggerTab::AgentRowSorter {
	bool operator()(const LimboDebuggerTab::AgentRow &p_a, const LimboDebuggerTab::AgentRow &p_b) const {
		return p_a.agent_path < p_b.agent_path;
	}
};

void LimboDebuggerTab::_reset_controls() {
	blinks.clear();
	set_process(false);
	agent_list->clear();
	bt_view->clear();
	history->clear();
	bb_tree->clear();
	_clear_machine();
	alert_box->hide();
	info_message->set_text(TTR("Run project to start debugging."));
	info_message->show();
	resource_header->set_disabled(true);
	resource_header->set_text(TTR("Inactive"));
}

void LimboDebuggerTab::start_session() {
	blinks.clear();
	set_process(false);
	agent_list->clear();
	bt_view->clear();
	history->clear();
	_clear_machine();
	alert_box->hide();
	// Not "pick one": the first to turn up is picked on its own, so what this stands for is
	// the moment before there is anybody to pick.
	info_message->set_text(TTR("Waiting for a character with a state machine."));
	info_message->show();
	bb_tree->clear();
	session->send_message("limboai:start_session", Array());
	_send_blackboard_tracking();
}

void LimboDebuggerTab::stop_session() {
	active_bt_instances.clear();
	active_hsms.clear();
	agent_rows.clear();
	selected_agent = String();
	shown_hsm_id = 0;
	tracked_bt_id = 0;
	bt_pinned = false;
	_reset_controls();
	session->send_message("limboai:stop_session", Array());
}

void LimboDebuggerTab::update_active_bt_instances(const Array &p_data) {
	active_bt_instances.clear();
	for (int i = 0; i + 2 < p_data.size(); i += 3) {
		BTInstanceInfo info{ p_data[i], p_data[i + 1], p_data[i + 2] };
		active_bt_instances.push_back(info);
	}
	_rebuild_agent_rows();
}

void LimboDebuggerTab::update_active_hsms(const Array &p_data) {
	active_hsms.clear();
	for (int i = 0; i + 2 < p_data.size(); i += 3) {
		HSMInfo info{ p_data[i], p_data[i + 1], p_data[i + 2] };
		active_hsms.push_back(info);
	}
	_rebuild_agent_rows();
}

uint64_t LimboDebuggerTab::get_selected_bt_instance_id() {
	return tracked_bt_id;
}

void LimboDebuggerTab::update_behavior_tree(const Ref<BehaviorTreeData> &p_data) {
	resource_header->set_text(p_data->source_bt_path);
	resource_header->set_disabled(false);
	bt_view->update_tree(p_data);
	info_message->hide();
}

void LimboDebuggerTab::_show_alert(const String &p_message) {
	alert_message->set_text(p_message);
	alert_box->set_visible(!p_message.is_empty());
}

// -- The characters --------------------------------------------------------

void LimboDebuggerTab::_rebuild_agent_rows() {
	agent_rows.clear();
	HashMap<String, int> index_of;

	for (const HSMInfo &info : active_hsms) {
		String key = info.agent_path.is_empty() ? info.node_path : info.agent_path;
		if (!index_of.has(key)) {
			AgentRow row;
			row.agent_path = key;
			index_of[key] = agent_rows.size();
			agent_rows.push_back(row);
		}
		AgentRow &row = agent_rows.write[index_of[key]];
		row.hsm_id = info.hsm_id;
		row.hsm_node_path = info.node_path;
	}

	for (const BTInstanceInfo &info : active_bt_instances) {
		// A tree with no agent still gets a row, under the node that owns it: a list that
		// silently drops one is worse than a row with an awkward name on it.
		String key = info.agent_path.is_empty() ? info.owner_node_path : info.agent_path;
		if (!index_of.has(key)) {
			AgentRow row;
			row.agent_path = key;
			index_of[key] = agent_rows.size();
			agent_rows.push_back(row);
		}
		agent_rows.write[index_of[key]].bt_instances.push_back(info.instance_id);
	}

	// By path, which is unique and does not move. The numbering below counts along this
	// order, and an order that reshuffled would renumber the characters while being read.
	agent_rows.sort_custom<AgentRowSorter>();

	HashMap<String, int> total;
	for (const AgentRow &row : agent_rows) {
		String leaf = _leaf_of(row.agent_path);
		total[leaf] = total.has(leaf) ? total[leaf] + 1 : 1;
	}
	HashMap<String, int> seen;
	for (int i = 0; i < agent_rows.size(); i++) {
		String leaf = _leaf_of(agent_rows[i].agent_path);
		if (total[leaf] > 1) {
			// Every one of them is numbered, not all but the first: a lone "Ogre" standing
			// above "Ogre (2)" reads as the odd one out rather than as the first of three.
			seen[leaf] = seen.has(leaf) ? seen[leaf] + 1 : 1;
			agent_rows.write[i].label = leaf + vformat(" (%d)", seen[leaf]);
		} else {
			agent_rows.write[i].label = leaf;
		}
	}

	_update_agent_list(filter_players->get_text());

	// A tree that has only just been built may belong to the state being stood in. Nothing
	// else would fetch it: a machine that has not moved sends nothing.
	if (shown_hsm_id != 0) {
		_choose_tree();
	}
}

void LimboDebuggerTab::_update_agent_list(const String &p_filter) {
	agent_list->clear();
	String filter = p_filter.to_lower();
	int select_idx = -1;
	bool selection_filtered_out = false;
	for (const AgentRow &row : agent_rows) {
		if (!filter.is_empty() && !row.label.to_lower().contains(filter) && !row.agent_path.to_lower().contains(filter)) {
			if (row.agent_path == selected_agent) {
				selection_filtered_out = true;
			}
			continue;
		}
		int idx = agent_list->add_item(row.label);
		agent_list->set_item_metadata(idx, row.agent_path);
		// The name is what is read; the path is what tells two of the same name apart.
		agent_list->set_item_tooltip(idx, row.agent_path);
		if (row.agent_path == selected_agent) {
			select_idx = idx;
		}
	}

	if (select_idx > -1) {
		agent_list->select(select_idx);
		_show_alert("");
		return;
	}
	if (selection_filtered_out) {
		// Still on the map, just not in this list. Picking somebody else because a word was
		// typed in the filter box would be the filter changing what is being watched.
		_show_alert("");
		_track_hsm(0);
		_track_bt(0);
		return;
	}

	// Nothing is picked -- either nothing ever was, or whoever was being read has just died.
	// The two are one case: there is a list and nobody is being watched, and the machine that
	// answers that is the first row. Waiting to be told which one costs the first seconds of
	// every session, which are the seconds a machine does most of its moving in.
	bool was_watching = !selected_agent.is_empty();
	selected_agent = String();
	if (agent_list->get_item_count() > 0) {
		agent_list->select(0);
		// ItemList::select() is silent, so the work a click would have done is done by hand.
		_agent_selected(0);
		if (was_watching) {
			// Said afterwards, because picking clears it. Without it the pane has changed
			// what it is about without being asked to, and the lit row says which character
			// is being read but not that it is a different one.
			_show_alert(TTR("The character being watched is gone. Showing the first in the list."));
		}
		return;
	}

	_show_alert(was_watching ? TTR("The selected character is no longer present.") : String());
	_track_hsm(0);
	_track_bt(0);
}

void LimboDebuggerTab::update_hsm_activity(const Array &p_data) {
	if (p_data.is_empty()) {
		return;
	}
	uint64_t hsm_id = uint64_t(p_data[0]);
	for (const AgentRow &row : agent_rows) {
		if (row.hsm_id != hsm_id) {
			continue;
		}
		// Set, not added to. A machine that moved twice in a frame did not move twice as
		// brightly, and the reader is being told one thing either way.
		blinks[row.agent_path] = 1.0;
		set_process(true);
		return;
	}
}

// The fade, and the putting of it on the rows in one pass -- which is also what keeps a
// rebuilt list right: the mark belongs to a character, the row is only where that character
// happens to be standing this frame, and every frame asks again. Switched off the moment
// nothing is glowing, because a debugger tab left open is a tab left open for hours.
void LimboDebuggerTab::_fade_blinks(double p_delta) {
	Vector<String> done;
	for (KeyValue<String, float> &entry : blinks) {
		entry.value -= float(p_delta) / BLINK_SECONDS;
		if (entry.value <= 0.0) {
			done.push_back(entry.key);
		}
	}
	for (int i = 0; i < done.size(); i++) {
		blinks.erase(done[i]);
	}

	for (int i = 0; i < agent_list->get_item_count(); i++) {
		String path = agent_list->get_item_metadata(i);
		const float *amount = blinks.getptr(path);
		// Transparent is ItemList's own word for "no custom background", so the row goes back
		// to being drawn by the theme rather than being drawn white at zero alpha.
		agent_list->set_item_custom_bg_color(i, amount != nullptr ? Color(1, 1, 1, *amount * BLINK_ALPHA) : Color(0, 0, 0, 0));
	}
	if (blinks.is_empty()) {
		set_process(false);
	}
}

const LimboDebuggerTab::AgentRow *LimboDebuggerTab::_row_for(const String &p_agent_path) const {
	for (const AgentRow &row : agent_rows) {
		if (row.agent_path == p_agent_path) {
			return &row;
		}
	}
	return nullptr;
}

void LimboDebuggerTab::_agent_selected(int p_idx) {
	selected_agent = agent_list->get_item_metadata(p_idx);
	alert_box->hide();
	// A fresh pick answers "which tree" from scratch: whatever was up belonged to whoever
	// was being read before.
	bt_pinned = false;
	history->clear();

	const AgentRow *row = _row_for(selected_agent);
	if (row == nullptr) {
		_track_hsm(0);
		_track_bt(0);
		return;
	}
	_track_hsm(row->hsm_id);
	if (row->hsm_id == 0) {
		// Nothing will arrive to choose from, so the character's own tree is the answer now.
		_choose_tree();
	}
}

void LimboDebuggerTab::_filter_changed(String p_text) {
	_update_agent_list(p_text);
}

// -- The machine -----------------------------------------------------------

void LimboDebuggerTab::_clear_machine() {
	live_machine.unref();
	state_by_path.clear();
	path_by_state.clear();
	active_chain = PackedStringArray();
	hsm_view->edit_hsm(Ref<HierarchicalStateMachine>());
}

void LimboDebuggerTab::_track_hsm(uint64_t p_hsm_id) {
	if (p_hsm_id == shown_hsm_id) {
		return;
	}
	shown_hsm_id = p_hsm_id;
	_clear_machine();
	if (p_hsm_id == 0) {
		session->send_message("limboai:untrack_hsm", Array());
		return;
	}
	Array data;
	data.push_back(p_hsm_id);
	session->send_message("limboai:track_hsm", data);
}

void LimboDebuggerTab::update_hsm_structure(const Array &p_data) {
	if (p_data.size() < 5 || uint64_t(p_data[0]) != shown_hsm_id) {
		return;
	}
	const AgentRow *row = _row_for(selected_agent);
	String root_name = row != nullptr ? _leaf_of(row->hsm_node_path) : String("HSM");

	live_machine = _machine_from_report(root_name, p_data[1], p_data[2], state_by_path, path_by_state);
	// Before the view is handed it: a nest is sized around what stands in it, and what stands
	// in it is only in the right place once this has run.
	_apply_authored_layout(p_data[4], state_by_path);
	hsm_view->edit_hsm(live_machine);
	_apply_chain(p_data[3]);
	// Which tree is up is settled by this, message and all -- there is nothing left here to
	// hide or show once it has run.
	_choose_tree();
}

void LimboDebuggerTab::update_hsm_state(const Array &p_data) {
	if (p_data.size() < 4 || uint64_t(p_data[0]) != shown_hsm_id || live_machine.is_null()) {
		return;
	}
	String from = p_data[1];
	String to = p_data[2];

	// One line per transition, newest first. The graph draws a pair that goes both ways as
	// one line with a head at either end, so which of the two directions was just taken is
	// something only this list can say.
	history->add_item(vformat(String::utf8("%s  →  %s"),
			from.is_empty() ? TTR("(entered)") : _leaf_of(from), _leaf_of(to)));
	history->move_item(history->get_item_count() - 1, 0);
	if (history->get_item_count() > 200) {
		history->remove_item(history->get_item_count() - 1);
	}

	hsm_view->pulse_transition(
			state_by_path.has(from) ? state_by_path[from] : Ref<HSMStateSpec>(),
			state_by_path.has(to) ? state_by_path[to] : Ref<HSMStateSpec>());
	_apply_chain(p_data[3]);
	_choose_tree();
}

void LimboDebuggerTab::_apply_chain(const PackedStringArray &p_chain) {
	active_chain = p_chain;
	Vector<Ref<HSMStateSpec>> specs;
	for (int i = 0; i < p_chain.size(); i++) {
		if (state_by_path.has(p_chain[i])) {
			specs.push_back(state_by_path[p_chain[i]]);
		}
	}
	hsm_view->set_active_chain(specs);
}

// -- The tree --------------------------------------------------------------

uint64_t LimboDebuggerTab::_bt_instance_for_state(const String &p_state_path) const {
	const AgentRow *row = _row_for(selected_agent);
	if (row == nullptr || row->hsm_node_path.is_empty() || p_state_path.is_empty()) {
		return 0;
	}
	// The state's own node path: where the machine stands, plus where the state stands under
	// it. A tree instance is registered under the node that owns it, and for a BTState that
	// is the state itself -- so the two strings meet without either side having to guess.
	String full = row->hsm_node_path + String("/") + p_state_path;
	for (const BTInstanceInfo &info : active_bt_instances) {
		if (info.owner_node_path == full) {
			return info.instance_id;
		}
	}
	return 0;
}

void LimboDebuggerTab::_choose_tree() {
	if (bt_pinned) {
		return;
	}
	uint64_t id = 0;
	// Innermost first: the leaf is the state actually being turned, and a machine further
	// out is only the room it stands in.
	for (int i = active_chain.size() - 1; i >= 0 && id == 0; i--) {
		id = _bt_instance_for_state(active_chain[i]);
	}
	if (id == 0) {
		const AgentRow *row = _row_for(selected_agent);
		if (row != nullptr && !row->bt_instances.is_empty()) {
			id = row->bt_instances[0];
		}
	}
	_track_bt(id);
}

void LimboDebuggerTab::_track_bt(uint64_t p_instance_id) {
	if (p_instance_id == 0 && tracked_bt_id == 0) {
		// Nothing before and nothing now -- but what "nothing" stands for may have changed:
		// a character has just been picked whose machine holds no tree anywhere.
		info_message->set_text(TTR("No behavior tree to show."));
		info_message->show();
		return;
	}
	if (p_instance_id == tracked_bt_id) {
		return;
	}
	tracked_bt_id = p_instance_id;
	bt_view->clear();
	if (p_instance_id == 0) {
		session->send_message("limboai:untrack_bt_player", Array());
		resource_header->set_text(TTR("Inactive"));
		resource_header->set_disabled(true);
		// Said rather than left blank: an empty pane beside a machine that is plainly moving
		// reads as something that failed, and most states hold no tree at all.
		info_message->set_text(TTR("No behavior tree to show."));
		info_message->show();
		return;
	}
	info_message->set_text(TTR("Waiting for behavior tree update."));
	info_message->show();
	resource_header->set_text(TTR("Waiting for data"));
	resource_header->set_disabled(true);
	Array data;
	data.push_back(p_instance_id);
	session->send_message("limboai:track_bt_player", data);
}

void LimboDebuggerTab::_on_state_picked(const Ref<HSMStateSpec> &p_state) {
	if (p_state.is_null()) {
		// Nothing is picked, which is what clearing the view says on its way out. It is not
		// a request to change which tree is up.
		return;
	}
	const String *path = path_by_state.getptr(p_state->get_instance_id());
	if (path == nullptr) {
		return;
	}
	uint64_t id = _bt_instance_for_state(*path);
	if (id != 0) {
		// Asked for by hand, so it stays up until another is asked for -- otherwise the next
		// transition would take the pane away mid-read.
		bt_pinned = true;
		_track_bt(id);
		return;
	}
	bt_pinned = false;
	_choose_tree();
}

void LimboDebuggerTab::_window_visibility_changed(bool p_visible) {
	make_floating->set_visible(!p_visible);
}

void LimboDebuggerTab::_resource_header_pressed() {
	String bt_path = resource_header->get_text();
	if (bt_path.is_empty()) {
		return;
	}
	FileSystemDock *dock = EditorInterface::get_singleton()->get_file_system_dock();
	dock->navigate_to_path(bt_path.get_slice("::", 0));
	Ref<BehaviorTree> bt = RESOURCE_LOAD(bt_path, "BehaviorTree");
	ERR_FAIL_COND_MSG(!bt.is_valid(), "Failed to load BehaviorTree. Wrong resource path?");
	EditorInterface::get_singleton()->edit_resource(bt);
}

// -- The blackboard --------------------------------------------------------

void LimboDebuggerTab::_send_blackboard_tracking() {
	if (session.is_null() || !session->is_active()) {
		return;
	}
	if (!bb_box->is_visible()) {
		session->send_message("limboai:untrack_blackboard", Array());
		return;
	}
	PackedStringArray paths;
	for (const String &path : bb_expanded) {
		paths.push_back(path);
	}
	Array data;
	// The tree's interval, floored on the far side: a hundred rows every frame is a cost the
	// game would pay for a pane nobody reads that fast.
	data.push_back(int64_t(update_interval->get_value()));
	data.push_back(paths);
	session->send_message("limboai:track_blackboard", data);
}

void LimboDebuggerTab::_bb_toggled(bool p_open) {
	bb_box->set_visible(p_open);
	if (!p_open) {
		bb_tree->clear();
	}
	_send_blackboard_tracking();
}

void LimboDebuggerTab::_bb_interval_changed(double p_value) {
	_send_blackboard_tracking();
}

void LimboDebuggerTab::_bb_item_collapsed(TreeItem *p_item) {
	if (bb_syncing || p_item == nullptr) {
		return;
	}
	Variant scope_label = p_item->get_metadata(1);
	if (scope_label.get_type() == Variant::STRING) {
		if (p_item->is_collapsed()) {
			bb_folded_scopes.insert(scope_label);
		} else {
			bb_folded_scopes.erase(scope_label);
		}
		return;
	}
	Variant path = p_item->get_metadata(0);
	if (path.get_type() != Variant::STRING || !(int(p_item->get_metadata(1)) & BB_FLAG_CONTAINER)) {
		return;
	}
	if (p_item->is_collapsed()) {
		// The ones below go too: unfolding this again starts from the top, as a fresh look.
		String prefix = String(path) + String::chr(BB_PATH_SEPARATOR);
		Vector<String> gone;
		for (const String &expanded : bb_expanded) {
			if (expanded == String(path) || expanded.begins_with(prefix)) {
				gone.push_back(expanded);
			}
		}
		for (int i = 0; i < gone.size(); i++) {
			bb_expanded.erase(gone[i]);
		}
	} else {
		bb_expanded.insert(path);
	}
	_send_blackboard_tracking();
}

void LimboDebuggerTab::_bb_filter_changed(String p_text) {
	_bb_apply_filter();
}

// By the variable's name, on the top rows of every scope. What is inside a container stays
// as it is: it was unfolded to be read whole.
void LimboDebuggerTab::_bb_apply_filter() {
	TreeItem *root = bb_tree->get_root();
	if (root == nullptr) {
		return;
	}
	String filter = bb_filter->get_text().to_lower();
	for (TreeItem *scope = root->get_first_child(); scope != nullptr; scope = scope->get_next()) {
		for (TreeItem *row = scope->get_first_child(); row != nullptr; row = row->get_next()) {
			row->set_visible(filter.is_empty() || row->get_text(0).to_lower().contains(filter));
		}
	}
}

void LimboDebuggerTab::update_blackboard(const Array &p_data) {
	if (p_data.size() < 2 || !bb_box->is_visible()) {
		return;
	}
	Array scopes = p_data[1];
	bb_syncing = true;
	TreeItem *root = bb_tree->get_root();
	if (root == nullptr) {
		root = bb_tree->create_item();
	}

	HashSet<String> wanted;
	for (int i = 0; i < scopes.size(); i++) {
		Array scope = scopes[i];
		wanted.insert(String(scope[0]));
	}
	HashMap<String, TreeItem *> existing;
	Vector<TreeItem *> stale;
	for (TreeItem *item = root->get_first_child(); item != nullptr; item = item->get_next()) {
		String label = item->get_metadata(1);
		if (wanted.has(label)) {
			existing[label] = item;
		} else {
			stale.push_back(item);
		}
	}
	for (int i = 0; i < stale.size(); i++) {
		memdelete(stale[i]);
	}

	for (int i = 0; i < scopes.size(); i++) {
		Array scope = scopes[i];
		String label = scope[0];
		TreeItem *item = existing.has(label) ? existing[label] : bb_tree->create_item(root, i);
		if (item->get_index() != i) {
			item->move_before(root->get_child(i));
		}
		item->set_text(0, label);
		item->set_metadata(1, label);
		item->set_selectable(0, false);
		item->set_selectable(1, false);
		item->set_custom_color(0, get_theme_color(LW_NAME(accent_color), LW_NAME(Editor)));
		item->set_collapsed(bb_folded_scopes.has(label));
		_bb_sync_rows(item, scope[1], String());
	}
	bb_syncing = false;
	_bb_apply_filter();
}

void LimboDebuggerTab::_bb_sync_rows(TreeItem *p_parent, const Array &p_rows, const String &p_parent_path) {
	HashSet<String> wanted;
	for (int i = 0; i < p_rows.size(); i++) {
		Array row = p_rows[i];
		wanted.insert(String(row[0]));
	}
	// Placeholders and the "more" line have no path and are written afresh every time.
	HashMap<String, TreeItem *> existing;
	Vector<TreeItem *> stale;
	for (TreeItem *item = p_parent->get_first_child(); item != nullptr; item = item->get_next()) {
		if (item->get_metadata(0).get_type() == Variant::STRING && wanted.has(item->get_text(0))) {
			existing[item->get_text(0)] = item;
		} else {
			stale.push_back(item);
		}
	}
	for (int i = 0; i < stale.size(); i++) {
		memdelete(stale[i]);
	}

	const Color dim = get_theme_color("font_disabled_color", LW_NAME(Editor));
	for (int i = 0; i < p_rows.size(); i++) {
		Array row = p_rows[i];
		String key = row[0];
		Variant::Type type = Variant::Type(int(row[1]));
		int flags = row[2];
		String text = row[3];
		Array children = row[4];
		String path = p_parent_path.is_empty() ? key : p_parent_path + String::chr(BB_PATH_SEPARATOR) + key;

		TreeItem *item = existing.has(key) ? existing[key] : bb_tree->create_item(p_parent, i);
		if (item->get_index() != i) {
			item->move_before(p_parent->get_child(i));
		}
		item->set_text(0, key);
		item->set_metadata(0, path);
		item->set_metadata(1, flags);
		String type_name = Variant::get_type_name(type);
		item->set_icon(0, has_theme_icon(type_name, LW_NAME(EditorIcons)) ? get_theme_icon(type_name, LW_NAME(EditorIcons)) : Ref<Texture2D>());
		item->set_tooltip_text(0, vformat("%s: %s", key, type_name));

		String tooltip;
		if (flags & BB_FLAG_LINKED) {
			// The value is shown once, in the scope that holds it. Here only where to look.
			item->set_text(1, String::utf8("→ ") + text);
			item->set_custom_color(1, dim);
			tooltip = vformat(TTR("Linked: the same variable as \"%s\" in %s."), key, text);
		} else {
			item->set_text(1, text);
			item->clear_custom_color(1);
			tooltip = text;
		}
		if (flags & BB_FLAG_BOUND) {
			item->set_icon(1, get_theme_icon("Instance", LW_NAME(EditorIcons)));
			tooltip += String("\n") + TTR("Bound to a property: read through its getter, never written.");
		} else {
			item->set_icon(1, Ref<Texture2D>());
		}
		item->set_tooltip_text(1, tooltip);

		if (!(flags & BB_FLAG_CONTAINER)) {
			while (item->get_first_child() != nullptr) {
				memdelete(item->get_first_child());
			}
			continue;
		}
		if (!bb_expanded.has(path)) {
			// One placeholder, so the fold arrow is there to be clicked. The contents are only
			// asked for once it is.
			TreeItem *first = item->get_first_child();
			bool placeholder_only = first != nullptr && first->get_next() == nullptr && first->get_metadata(0).get_type() != Variant::STRING;
			if (!placeholder_only) {
				while (item->get_first_child() != nullptr) {
					memdelete(item->get_first_child());
				}
				TreeItem *placeholder = bb_tree->create_item(item);
				placeholder->set_text(0, "...");
				placeholder->set_selectable(0, false);
				placeholder->set_selectable(1, false);
			}
			item->set_collapsed(true);
			continue;
		}
		_bb_sync_rows(item, children, path);
		if (flags & BB_FLAG_TRUNCATED) {
			TreeItem *more = bb_tree->create_item(item);
			more->set_text(0, "...");
			more->set_text(1, TTR("(more entries not shown)"));
			more->set_custom_color(1, dim);
			more->set_selectable(0, false);
			more->set_selectable(1, false);
		}
		item->set_collapsed(false);
	}
}

void LimboDebuggerTab::_bind_methods() {
}

void LimboDebuggerTab::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PROCESS: {
			_fade_blinks(get_process_delta_time());
		} break;
		case NOTIFICATION_READY: {
			resource_header->connect(LW_NAME(pressed), callable_mp(this, &LimboDebuggerTab::_resource_header_pressed));
			filter_players->connect(LW_NAME(text_changed), callable_mp(this, &LimboDebuggerTab::_filter_changed));
			agent_list->connect(LW_NAME(item_selected), callable_mp(this, &LimboDebuggerTab::_agent_selected));
			hsm_view->connect("state_picked", callable_mp(this, &LimboDebuggerTab::_on_state_picked));
			update_interval->connect("value_changed", callable_mp(bt_view, &BehaviorTreeView::set_update_interval_msec));
			update_interval->connect("value_changed", callable_mp(this, &LimboDebuggerTab::_bb_interval_changed));
			bb_toggle->connect(LW_NAME(toggled), callable_mp(this, &LimboDebuggerTab::_bb_toggled));
			bb_filter->connect(LW_NAME(text_changed), callable_mp(this, &LimboDebuggerTab::_bb_filter_changed));
			bb_tree->connect(LW_NAME(item_collapsed), callable_mp(this, &LimboDebuggerTab::_bb_item_collapsed));

			Ref<ConfigFile> cf;
			cf.instantiate();
			String conf_path = LAYOUT_CONFIG_FILE();
			if (cf->load(conf_path) == OK) {
				Variant value = cf->get_value("LimboAI", "debugger_update_interval_msec", 0);
				update_interval->set_value(value);
				bb_toggle->set_pressed(cf->get_value("LimboAI", "debugger_blackboard_open", false));
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			Ref<ConfigFile> cf;
			cf.instantiate();
			String conf_path = LAYOUT_CONFIG_FILE();
			cf->load(conf_path);
			cf->set_value("LimboAI", "debugger_update_interval_msec", update_interval->get_value());
			cf->set_value("LimboAI", "debugger_blackboard_open", bb_toggle->is_pressed());
			cf->save(conf_path);
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			alert_icon->set_texture(get_theme_icon(LW_NAME(StatusWarning), LW_NAME(EditorIcons)));
			resource_header->set_button_icon(LimboUtility::get_singleton()->get_task_icon("BehaviorTree"));
		} break;
	}
}

void LimboDebuggerTab::setup(Ref<EditorDebuggerSession> p_session, CompatWindowWrapper *p_wrapper) {
	session = p_session;
	window_wrapper = p_wrapper;

	if (p_wrapper->is_window_available()) {
		make_floating = memnew(CompatScreenSelect);
		make_floating->set_flat(true);
		make_floating->set_tooltip_text(TTR("Make the LimboAI Debugger floating."));
		make_floating->connect(LW_NAME(request_open_in_screen), callable_mp(window_wrapper, &CompatWindowWrapper::enable_window_on_screen).bind(true));
		toolbar->add_child(make_floating);
		p_wrapper->connect(LW_NAME(window_visibility_changed), callable_mp(this, &LimboDebuggerTab::_window_visibility_changed));
	}

	_reset_controls();
}

// Four panes, left to right: who, what states it has, how they join, and what the one it is
// standing in is doing. They stand together because reading a character means reading all
// four at once -- with the machine in one tab and its tree in another, the thing joining
// "which state is it in" to "which branch is it turning" is the reader's own memory. A fifth,
// the blackboard, opens from the toolbar to the right of the tree.
LimboDebuggerTab::LimboDebuggerTab() {
	root_vb = memnew(VBoxContainer);
	add_child(root_vb);

	toolbar = memnew(HBoxContainer);
	root_vb->add_child(toolbar);

	resource_header = memnew(Button);
	toolbar->add_child(resource_header);
	resource_header->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
	resource_header->set_focus_mode(FOCUS_NONE);
	resource_header->add_theme_constant_override("hseparation", 8);
	resource_header->set_text(TTR("Inactive"));
	resource_header->set_tooltip_text(TTR("Debugged BehaviorTree resource.\nClick to open."));
	resource_header->set_disabled(true);

	Label *interval_label = memnew(Label);
	toolbar->add_child(interval_label);
	interval_label->set_text(TTR("Update Interval:"));
	interval_label->set_h_size_flags(SIZE_EXPAND | SIZE_SHRINK_END);

	update_interval = memnew(EditorSpinSlider);
	toolbar->add_child(update_interval);
	update_interval->set_min(0);
	update_interval->set_max(1000);
	update_interval->set_step(1.0);
	update_interval->set_suffix("ms");
	update_interval->set_custom_minimum_size(Vector2(100 * EDSCALE, 0));

	VSeparator *sep = memnew(VSeparator);
	toolbar->add_child(sep);

	bb_toggle = memnew(Button);
	bb_toggle->set_toggle_mode(true);
	bb_toggle->set_flat(true);
	bb_toggle->set_focus_mode(FOCUS_NONE);
	bb_toggle->set_text(TTR("Blackboard"));
	bb_toggle->set_tooltip_text(TTR("Show the blackboard of the tree being shown, scope by scope (read only)."));
	toolbar->add_child(bb_toggle);

	hsc = memnew(HSplitContainer);
	hsc->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	hsc->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	root_vb->add_child(hsc);

	VBoxContainer *list_box = memnew(VBoxContainer);
	hsc->add_child(list_box);

	filter_players = memnew(LineEdit);
	filter_players->set_placeholder(TTR("Filter Characters"));
	list_box->add_child(filter_players);

	agent_list = memnew(ItemList);
	agent_list->set_custom_minimum_size(Size2(160.0 * EDSCALE, 0.0));
	agent_list->set_h_size_flags(SIZE_FILL);
	agent_list->set_v_size_flags(SIZE_EXPAND_FILL);
	list_box->add_child(agent_list);

	HSplitContainer *right_split = memnew(HSplitContainer);
	right_split->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	right_split->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	hsc->add_child(right_split);

	// The machine and what it has been doing, one above the other. The history is a strip
	// rather than a pane of its own -- it is a handful of words per line -- and the split is
	// there so it can be pushed out of the way entirely when the graph is what matters.
	machine_split = memnew(VSplitContainer);
	machine_split->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	machine_split->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	right_split->add_child(machine_split);

	hsm_view = memnew(HSMEditorView);
	// Nothing here writes. These states stand for nodes in a running game, and there is no
	// file behind them for an edit to land in.
	hsm_view->set_read_only(true);
	machine_split->add_child(hsm_view);

	history = memnew(ItemList);
	history->set_custom_minimum_size(Size2(0, 60 * EDSCALE));
	machine_split->add_child(history);

	// The tree and, when open, the blackboard beside it: the variables are read against the
	// branch that is running, so the two stand side by side rather than one under the other.
	HSplitContainer *tree_split = memnew(HSplitContainer);
	tree_split->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	tree_split->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	right_split->add_child(tree_split);

	view_box = memnew(VBoxContainer);
	view_box->set_custom_minimum_size(Size2(240.0 * EDSCALE, 0.0));
	view_box->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	tree_split->add_child(view_box);

	bb_box = memnew(VBoxContainer);
	bb_box->set_custom_minimum_size(Size2(260.0 * EDSCALE, 0.0));
	bb_box->hide();
	tree_split->add_child(bb_box);

	bb_filter = memnew(LineEdit);
	bb_filter->set_placeholder(TTR("Filter Variables"));
	bb_filter->set_clear_button_enabled(true);
	bb_box->add_child(bb_filter);

	bb_tree = memnew(Tree);
	bb_tree->set_columns(2);
	bb_tree->set_column_titles_visible(true);
	bb_tree->set_column_title(0, TTR("Variable"));
	bb_tree->set_column_title(1, TTR("Value"));
	bb_tree->set_column_expand_ratio(1, 2);
	bb_tree->set_column_clip_content(0, true);
	bb_tree->set_column_clip_content(1, true);
	bb_tree->set_hide_root(true);
	bb_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	bb_box->add_child(bb_tree);

	bt_view = memnew(BehaviorTreeView);
	bt_view->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	bt_view->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	view_box->add_child(bt_view);

	alert_box = memnew(HBoxContainer);
	alert_box->hide();
	view_box->add_child(alert_box);

	alert_icon = memnew(TextureRect);
	alert_box->add_child(alert_icon);
	alert_icon->set_stretch_mode(TextureRect::STRETCH_KEEP_CENTERED);

	alert_message = memnew(Label);
	alert_box->add_child(alert_message);
	alert_message->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);

	info_message = memnew(Label);
	info_message->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	info_message->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	info_message->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	info_message->set_custom_minimum_size(Size2(100 * EDSCALE, 0));
	info_message->set_anchors_and_offsets_preset(PRESET_FULL_RECT, PRESET_MODE_KEEP_SIZE, 8 * EDSCALE);

	bt_view->add_child(info_message);
}

//**** LimboDebuggerPlugin

LimboDebuggerPlugin *LimboDebuggerPlugin::singleton = nullptr;

void LimboDebuggerPlugin::_window_visibility_changed(bool p_visible) {
}

#ifdef LIMBOAI_MODULE
void LimboDebuggerPlugin::setup_session(int p_session_id) {
#elif LIMBOAI_GDEXTENSION
void LimboDebuggerPlugin::_setup_session(int32_t p_session_id) {
#endif
	Ref<EditorDebuggerSession> session = get_session(p_session_id);
	ERR_FAIL_COND(session.is_null());

	CompatWindowWrapper *session_window = memnew(CompatWindowWrapper);
	session_window->set_window_title(vformat(TTR("%s - Godot Engine"), TTR("LimboAI Debugger")));
	session_window->set_margins_enabled(true);

	LimboDebuggerTab *tab = memnew(LimboDebuggerTab());
	tab->setup(session, session_window);
	tab->set_name("LimboAI");
	session_window->set_wrapped_control(tab);
	session_window->set_name("LimboAI");

	session_window->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	session_window->connect(LW_NAME(window_visibility_changed), callable_mp(this, &LimboDebuggerPlugin::_window_visibility_changed));

	session->connect(LW_NAME(started), callable_mp(tab, &LimboDebuggerTab::start_session));
	session->connect(LW_NAME(stopped), callable_mp(tab, &LimboDebuggerTab::stop_session));
	session->add_session_tab(session_window);

	session_windows[p_session_id] = session_window;
}

#ifdef LIMBOAI_MODULE
bool LimboDebuggerPlugin::capture(const String &p_message, const Array &p_data, int p_session_id) {
#elif LIMBOAI_GDEXTENSION
bool LimboDebuggerPlugin::_capture(const String &p_message, const Array &p_data, int32_t p_session_id) {
#endif
	ERR_FAIL_COND_V(!session_windows.has(p_session_id), false);
	LimboDebuggerTab *tab = Object::cast_to<LimboDebuggerTab>(session_windows[p_session_id]->get_wrapped_control());
	ERR_FAIL_NULL_V(tab, false);
	bool captured = true;
	if (p_message == "limboai:active_bt_players") {
		tab->update_active_bt_instances(p_data);
	} else if (p_message == "limboai:active_hsms") {
		tab->update_active_hsms(p_data);
	} else if (p_message == "limboai:hsm_structure") {
		tab->update_hsm_structure(p_data);
	} else if (p_message == "limboai:hsm_state") {
		tab->update_hsm_state(p_data);
	} else if (p_message == "limboai:hsm_activity") {
		tab->update_hsm_activity(p_data);
	} else if (p_message == "limboai:bt_update") {
		Ref<BehaviorTreeData> data = BehaviorTreeData::deserialize(p_data);
		if (data->bt_instance_id == tab->get_selected_bt_instance_id()) {
			tab->update_behavior_tree(data);
		}
	} else if (p_message == "limboai:blackboard") {
		tab->update_blackboard(p_data);
	} else {
		captured = false;
	}
	return captured;
}

#ifdef LIMBOAI_MODULE
bool LimboDebuggerPlugin::has_capture(const String &p_capture) const {
#elif LIMBOAI_GDEXTENSION
bool LimboDebuggerPlugin::_has_capture(const String &p_capture) const {
#endif
	return p_capture == "limboai";
}

CompatWindowWrapper *LimboDebuggerPlugin::get_first_session_window() const {
	ERR_FAIL_COND_V(session_windows.is_empty(), nullptr);
	return session_windows.begin()->value;
}

int LimboDebuggerPlugin::get_first_session_tab_index() const {
	ERR_FAIL_COND_V(session_windows.is_empty(), -1);
	CompatWindowWrapper *window_wrapper = session_windows.begin()->value;
	TabContainer *c = Object::cast_to<TabContainer>(window_wrapper->get_parent());
	ERR_FAIL_COND_V(c == nullptr, -1);
	return c->get_tab_idx_from_control(window_wrapper);
}

void LimboDebuggerPlugin::_bind_methods() {
}

LimboDebuggerPlugin::LimboDebuggerPlugin() {
	singleton = this;
}

LimboDebuggerPlugin::~LimboDebuggerPlugin() {
	singleton = nullptr;
}

#endif // ! TOOLS_ENABLED
