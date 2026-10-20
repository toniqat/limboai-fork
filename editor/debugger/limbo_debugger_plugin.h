/**
 * limbo_debugger_plugin.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#ifndef LIMBO_DEBUGGER_PLUGIN_H
#define LIMBO_DEBUGGER_PLUGIN_H

#include "../../compat/compat_window_wrapper.h"
#include "../../editor/debugger/behavior_tree_data.h"
#include "../../editor/debugger/behavior_tree_view.h"
#include "../../editor/hsm_editor_view.h"
#include "../../hsm/hsm_resource.h"

#ifdef LIMBOAI_MODULE
#include "core/object/object.h"
#include "core/typedefs.h"
#include "editor/debugger/editor_debugger_plugin.h"
#include "editor/gui/editor_spin_slider.h"
#include "editor/gui/window_wrapper.h"
#include "scene/gui/box_container.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/split_container.h"
#include "scene/gui/texture_rect.h"
#include "scene/gui/tree.h"
#include "core/templates/hash_map.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/editor_debugger_plugin.hpp>
#include <godot_cpp/classes/editor_debugger_session.hpp>
#include <godot_cpp/classes/editor_spin_slider.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_split_container.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/v_split_container.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#endif // LIMBOAI_GDEXTENSION

class LimboDebuggerTab : public PanelContainer {
	GDCLASS(LimboDebuggerTab, PanelContainer);

private:
	// A tree, and the two nodes that place it: the one that owns it, which is where in a
	// machine it hangs, and the agent, which is whose it is.
	struct BTInstanceInfo {
		uint64_t instance_id = 0;
		String owner_node_path;
		String agent_path;
	};

	struct HSMInfo {
		uint64_t hsm_id = 0;
		String node_path;
		String agent_path;
	};

	// One row of the leftmost list: a character on the map, not a resource. The two lists
	// above arrive keyed by agent, and joining them here is what turns "seven trees and
	// three machines" back into "three characters".
	struct AgentRow {
		String agent_path;
		String label;
		uint64_t hsm_id = 0;
		String hsm_node_path;
		Vector<uint64_t> bt_instances;
	};

	// Defined in the .cpp: it is one line of comparison, and putting it here would put the
	// ordering of the list in the file that says what the list is made of.
	struct AgentRowSorter;

	Vector<BTInstanceInfo> active_bt_instances;
	Vector<HSMInfo> active_hsms;
	Vector<AgentRow> agent_rows;
	// By path, not by row index: the list is rebuilt whenever anything comes or goes, and an
	// index would then point at whoever moved into that place.
	String selected_agent;

	// The machine as reported a moment ago, built into the same resource the main screen
	// edits so that the same view can draw it. Held because the graph is drawn from it and
	// because a state that gets picked has to be found in it again.
	Ref<HierarchicalStateMachine> live_machine;
	HashMap<String, Ref<HSMStateSpec>> state_by_path;
	HashMap<uint64_t, String> path_by_state;
	uint64_t shown_hsm_id = 0;
	// Where the machine is standing, outermost first. Kept because it answers a second
	// question besides which boxes to light: which tree is the one being turned right now.
	PackedStringArray active_chain;
	// Which tree the game is relaying. Answers the plugin, which drops an update belonging
	// to any other -- a tree keeps arriving for a moment after the ask to stop.
	uint64_t tracked_bt_id = 0;
	// Set when a state was picked by hand, so that the machine moving on does not take the
	// pane away from the tree that was being read.
	bool bt_pinned = false;
	// How much white is left on a row, by character. Keyed by path and not by row index for
	// the same reason `selected_agent` is: the list is rebuilt whenever anything comes or
	// goes, and an index would then be lighting up whoever moved into that place.
	HashMap<String, float> blinks;

	Ref<EditorDebuggerSession> session;
	VBoxContainer *root_vb = nullptr;
	HBoxContainer *toolbar = nullptr;
	HSplitContainer *hsc = nullptr;
	Label *info_message = nullptr;
	ItemList *agent_list = nullptr;
	VSplitContainer *machine_split = nullptr;
	HSMEditorView *hsm_view = nullptr;
	ItemList *history = nullptr;
	BehaviorTreeView *bt_view = nullptr;
	VBoxContainer *view_box = nullptr;
	HBoxContainer *alert_box = nullptr;
	TextureRect *alert_icon = nullptr;
	Label *alert_message = nullptr;
	LineEdit *filter_players = nullptr;
	Button *resource_header = nullptr;
	Button *make_floating = nullptr;
	EditorSpinSlider *update_interval = nullptr;
	CompatWindowWrapper *window_wrapper = nullptr;

	// The blackboard column: the scopes of the tree being shown (or of the machine, when the
	// state it stands in has none), innermost first, read only. The game sends it only while
	// the column is open.
	Button *bb_toggle = nullptr;
	VBoxContainer *bb_box = nullptr;
	LineEdit *bb_filter = nullptr;
	Tree *bb_tree = nullptr;
	// Unfolded containers, by path of names from the variable down. Kept across characters
	// and trees, so "record > ai" stays open while the watched character changes.
	HashSet<String> bb_expanded;
	// Folded scopes, by label.
	HashSet<String> bb_folded_scopes;
	// Set while the rows are being written, when Tree reports folds the code made itself.
	bool bb_syncing = false;

	void _reset_controls();
	void _show_alert(const String &p_message);
	// Rebuilds the rows out of the two lists and puts them on screen. One call for both
	// because a row is a join of the two: doing it per message would show a character with
	// a machine and no tree for however long the other message is behind.
	void _rebuild_agent_rows();
	void _update_agent_list(const String &p_filter);
	void _agent_selected(int p_idx);
	const AgentRow *_row_for(const String &p_agent_path) const;
	void _clear_machine();
	// Asks the game for one machine and one tree, or for none. Both go through here so that
	// what was asked for and what is on screen cannot drift apart.
	void _track_hsm(uint64_t p_hsm_id);
	void _track_bt(uint64_t p_instance_id);
	// The tree hanging off a state, found by node path: the game reported where each state
	// sits under the machine, and a tree instance by the node that owns it.
	uint64_t _bt_instance_for_state(const String &p_state_path) const;
	// The tree to show when nobody has picked one: the one belonging to the state the
	// machine is standing in, and failing that the character's first.
	void _choose_tree();
	void _on_state_picked(const Ref<HSMStateSpec> &p_state);
	void _apply_chain(const PackedStringArray &p_chain);
	void _filter_changed(String p_text);
	void _fade_blinks(double p_delta);
	void _window_visibility_changed(bool p_visible);
	void _resource_header_pressed();

	// Asks the game to start or stop sending the blackboard, with the interval and the
	// unfolded paths. Called whenever any of the three changes.
	void _send_blackboard_tracking();
	void _bb_toggled(bool p_open);
	void _bb_interval_changed(double p_value);
	void _bb_item_collapsed(TreeItem *p_item);
	void _bb_filter_changed(String p_text);
	// Writes the reported rows under p_parent, reusing the items already there so the
	// scroll position, the selection and the folds survive an update.
	void _bb_sync_rows(TreeItem *p_parent, const Array &p_rows, const String &p_parent_path);
	void _bb_apply_filter();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void start_session();
	void stop_session();
	void update_active_bt_instances(const Array &p_data);
	void update_active_hsms(const Array &p_data);
	void update_hsm_structure(const Array &p_data);
	void update_hsm_state(const Array &p_data);
	// One machine moved, and that is the whole message. It arrives for every character on the
	// map, the watched one included -- what it is for is the ones nobody is watching.
	void update_hsm_activity(const Array &p_data);
	BehaviorTreeView *get_behavior_tree_view() const { return bt_view; }
	uint64_t get_selected_bt_instance_id();
	void update_behavior_tree(const Ref<BehaviorTreeData> &p_data);
	void update_blackboard(const Array &p_data);

	void setup(Ref<EditorDebuggerSession> p_session, CompatWindowWrapper *p_wrapper);
	LimboDebuggerTab();
};

class LimboDebuggerPlugin : public EditorDebuggerPlugin {
	GDCLASS(LimboDebuggerPlugin, EditorDebuggerPlugin);

private:
	static LimboDebuggerPlugin *singleton;

	HashMap<int, CompatWindowWrapper *> session_windows;

	void _window_visibility_changed(bool p_visible);

protected:
	static void _bind_methods();

public:
	static _FORCE_INLINE_ LimboDebuggerPlugin *get_singleton() { return singleton; }

#ifdef LIMBOAI_MODULE
	void setup_session(int p_session_id) override;
	bool has_capture(const String &p_capture) const override;
	bool capture(const String &p_message, const Array &p_data, int p_session_id) override;
#elif LIMBOAI_GDEXTENSION
	void _setup_session(int32_t p_idx) override;
	bool _has_capture(const String &p_capture) const override;
	bool _capture(const String &p_message, const Array &p_data, int32_t p_session_id) override;
#endif

	CompatWindowWrapper *get_first_session_window() const;
	int get_first_session_tab_index() const;

	LimboDebuggerPlugin();
	~LimboDebuggerPlugin();
};

#endif // LIMBO_DEBUGGER_PLUGIN_H

#endif // ! TOOLS_ENABLED
