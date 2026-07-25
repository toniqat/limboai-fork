/**
 * hsm_editor_view.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#ifndef HSM_EDITOR_VIEW_H
#define HSM_EDITOR_VIEW_H

#include "../hsm/hsm_resource.h"

#ifdef LIMBOAI_MODULE
#include "core/input/input_event.h"
#include "core/templates/hash_map.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/graph_edit.h"
#include "scene/gui/graph_node.h"
#include "scene/gui/label.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tree.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/box_container.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/graph_edit.hpp>
#include <godot_cpp/classes/graph_node.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_split_container.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/templates/hash_map.hpp>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

// The HSM half of the main screen: the state list on the left, the graph on the right.
//
// Unlike the BT half, the thing being drawn is a hierarchy of resources rather than a
// tree of tasks, so the two panes answer different questions -- the list answers "what
// states are there", the graph answers "how does one become another".
class HSMEditorView : public HSplitContainer {
	GDCLASS(HSMEditorView, HSplitContainer);

private:
	// One drawn node. `machine` is the state that owns the transitions between them,
	// which is what makes a connection legal: both ends are always siblings.
	struct DrawnState {
		Ref<HSMState> spec;
		Ref<HSMState> machine;
		int depth = 0;
	};

	Ref<HierarchicalStateMachine> hsm;

	VBoxContainer *list_box;
	Label *list_title;
	HBoxContainer *toolbar;
	Button *add_btn;
	Button *del_btn;
	Tree *state_list;
	GraphEdit *graph;

	// Node name (which is the state's instance id as a string) -> state.
	HashMap<StringName, DrawnState> drawn;
	// Instance id -> node name, for wiring connections and syncing selection.
	HashMap<uint64_t, StringName> node_names;

	bool updating = false;

	static StringName _node_name_for(const Ref<HSMState> &p_state);
	static StringName _anystate_node_name_for(const Ref<HSMState> &p_machine);

	GraphNode *_make_node(const Ref<HSMState> &p_state, const Ref<HSMState> &p_machine, int p_depth, int p_row);
	GraphNode *_make_anystate_node(const Ref<HSMState> &p_machine, int p_depth, int p_row);
	void _populate_graph(const Ref<HSMState> &p_machine, int p_depth, int &r_row);
	void _wire_graph(const Ref<HSMState> &p_machine);
	void _populate_list(const Ref<HSMState> &p_machine, TreeItem *p_parent);

	void _on_list_item_selected();
	void _on_list_item_activated();
	void _on_graph_node_selected(Node *p_node);
	void _on_graph_node_gui_input(const Ref<InputEvent> &p_event, const StringName &p_name);
	void _on_graph_end_node_move();

	void _drill_into(const Ref<HSMState> &p_state);

	Ref<HSMState> _selected_spec() const;
	Ref<HSMState> _machine_of(const Ref<HSMState> &p_state) const;
	StringName _unique_child_name(const Ref<HSMState> &p_machine) const;
	void _detach_state(const Ref<HSMState> &p_machine, const Ref<HSMState> &p_state);

	void _on_add_pressed();
	void _on_delete_pressed();
	void _on_connection_request(const StringName &p_from, int p_from_port, const StringName &p_to, int p_to_port);
	void _on_disconnection_request(const StringName &p_from, int p_from_port, const StringName &p_to, int p_to_port);
	void _on_delete_nodes_request(const TypedArray<StringName> &p_nodes);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void edit_hsm(const Ref<HierarchicalStateMachine> &p_hsm);
	Ref<HierarchicalStateMachine> get_edited_hsm() const { return hsm; }
	void refresh();

	HSMEditorView();
};

#endif // HSM_EDITOR_VIEW_H

#endif // ! TOOLS_ENABLED
