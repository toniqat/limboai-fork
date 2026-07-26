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
#include "hsm_graph_edit.h"
#include "hsm_nest_node.h"

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
#include <godot_cpp/classes/graph_element.hpp>
#include <godot_cpp/classes/graph_node.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_split_container.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
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

	// A machine that holds states is drawn as one node with those states standing inside
	// it, so its geometry is worked out from what it holds rather than authored: `content`
	// is the empty child that reserves the room, `inner` is what stands in that room, and
	// `rect` is what the last fit made of it.
	struct Nest {
		GraphNode *node = nullptr;
		Control *content = nullptr;
		Ref<HSMState> spec;
		Vector<GraphNode *> inner;
		// Where the node stood when the fit last touched it, which is what turns the next
		// move into a delta to hand down.
		Vector2 last_offset;
		Rect2 rect;
		// What the theme puts around the room: the title bar and the panel's margins. Read
		// off a laid-out node rather than guessed, and kept because a resize has to work the
		// same figures back the other way.
		Vector2 inset_tl;
		Vector2 inset_br;
		// The smallest the box may be and still hold what stands in it. A resize is clamped
		// to it, which is the whole of "it may be made smaller, but not so small that it
		// hides what it is a box around".
		Size2 min_size;
	};

	Ref<HierarchicalStateMachine> hsm;
	// What is drawn: the file's own states with whatever it inherits already merged in.
	// Every edit is made on the authored state behind a drawn one, never on the drawn one.
	Ref<HSMState> merged_root;

	// What a right-click on a node offers. `MENU_REMOVE_TRANSITION` is not here: the
	// submenu carries one item per outgoing edge and its id is that edge's row index.
	enum MenuItem {
		MENU_CONNECT,
		MENU_ADD_CHILD,
		MENU_REMOVE,
		MENU_OPEN_TREE,
	};

	VBoxContainer *list_box;
	Label *list_title;
	HBoxContainer *toolbar;
	Button *add_btn;
	Button *del_btn;
	Tree *state_list;
	HSMGraphEdit *graph;
	PopupMenu *node_menu;
	PopupMenu *transition_menu;
	// A right-click that landed on an edge rather than on a node. It is its own menu because
	// one line can stand for more than one row, so what it offers is a list of rows and not
	// the node menu with a submenu hanging off it.
	PopupMenu *line_menu;
	// Which node the open menu is about. The menu outlives the click that opened it.
	StringName menu_node;
	// And which machine the open line menu's rows belong to, for the same reason.
	Ref<HSMState> line_machine;
	// Handed to every undo action as its context, which is what puts them all in the global
	// history. A resource has no history of its own, and the states being edited are
	// sub-resources with no path at all -- the task editor carries the same dummy for it.
	Object *history_context = nullptr;
	// 1x1 and transparent. The slot has to stay -- a connection is a thing a port has --
	// but its dot would sit apart from a line that leaves the border, reading as something
	// that ought to be joined and is not. The color is still handed over: an outgoing port
	// is what decides a line's color, so painting it away would take the line, not the dot.
	Ref<Texture2D> blank_port;

	// Node name (which is the state's instance id as a string) -> state.
	HashMap<StringName, DrawnState> drawn;
	// Instance id -> node name, for wiring connections and syncing selection.
	HashMap<uint64_t, StringName> node_names;
	// ANYSTATE node name -> the machine it belongs to. It is not a state, so it is in
	// neither map above, and a right-click on it still has to find its machine.
	HashMap<StringName, Ref<HSMState>> anystate_machines;

	// Deepest first, which is the order a fit has to take: a nest is measured against what
	// stands in it, and one of those may be a nest itself.
	Vector<Nest> nests;
	// Node name -> where that node's nest sits in `nests`.
	HashMap<StringName, int> nest_of;

	bool updating = false;
	// While the view is the one moving nodes. A nest hands a drag down to what it holds,
	// and a fit moves the nest itself -- without this the second would read as the first.
	bool adjusting = false;

	static StringName _node_name_for(const Ref<HSMState> &p_state);
	static StringName _anystate_node_name_for(const Ref<HSMState> &p_machine);

	void _hide_ports(GraphNode *p_node);
	GraphNode *_make_node(const Ref<HSMState> &p_state, const Ref<HSMState> &p_machine, int p_depth, int p_row, bool p_nests, Control **r_content);
	GraphNode *_make_anystate_node(const Ref<HSMState> &p_machine, int p_depth, int p_row);
	// Where a node stands. A nest's own size only reaches the node a layout pass later, so
	// the figure the fit worked out is the one that answers until then.
	Rect2 _drawn_rect(GraphNode *p_node) const;
	// Sizes every nest around what it holds and puts it where that lands. Deferred because
	// it reads the insets off a node that has been laid out at least once; `p_retry` is how
	// many times it may wait for that.
	void _fit_nests(int p_retry = 0);
	// The middle of a nest, in the node's own coordinates, which is where it stops answering
	// to the mouse. Worked out from the same fit: what is not the chrome is the room, and the
	// room belongs to what stands in it.
	void _update_nest_hole(const Nest &p_nest);
	void _on_nest_moved(const StringName &p_name);
	// A nest is drawn under what it holds, and GraphEdit lifts whatever is clicked to the
	// front -- so clicking one buries its own contents. Put back rather than prevented: the
	// lift is GraphEdit's, and the order it should end in is ours.
	void _enforce_nest_order();
	void _on_nest_raised();
	void _on_nest_resize_request(const Vector2 &p_size, const StringName &p_name);
	void _on_nest_resize_end(const Vector2 &p_size, const StringName &p_name);
	// Draws everything inside `p_machine` and hands back the nodes that stand for its
	// direct contents. A nested machine is its own node here -- there is nothing else to
	// hand back, since it is the thing the rest stand inside.
	Vector<GraphNode *> _populate_graph(const Ref<HSMState> &p_machine, int p_depth, int &r_row);
	void _wire_graph(const Ref<HSMState> &p_machine);
	void _populate_list(const Ref<HSMState> &p_machine, TreeItem *p_parent);

	void _on_list_item_selected();
	void _on_list_item_activated();
	void _on_graph_node_selected(Node *p_node);
	void _on_graph_node_gui_input(const Ref<InputEvent> &p_event, const StringName &p_name);
	void _on_graph_end_node_move();

	void _drill_into(const Ref<HSMState> &p_state);
	void _inspect(const Ref<HSMState> &p_drawn);

	Ref<HSMState> _selected_spec() const;
	Ref<HSMState> _machine_of(const Ref<HSMState> &p_state) const;
	// The machine a drawn node belongs to, whether it is a state or an ANYSTATE. Hands back
	// a null state for the latter, which is also what a transition leaving it says.
	Ref<HSMState> _machine_for_node(const StringName &p_name, Ref<HSMState> &r_state) const;
	Ref<HSMState> _authored_of(const Ref<HSMState> &p_drawn, bool p_create);
	StringName _unique_child_name(const Ref<HSMState> &p_machine) const;
	void _detach_state(const Ref<HSMState> &p_machine, const Ref<HSMState> &p_state);

	bool _remove_drawn(const Ref<HSMState> &p_drawn);
	void _add_child_state(const Ref<HSMState> &p_drawn_parent);
	// The row in *this* file that a drawn row stands for, or nothing when it came from the
	// base. Matched on the event too: which of two rows between the same pair this is, is
	// the one thing a pair of endpoints cannot say.
	Ref<HSMTransition> _authored_transition(const Ref<HSMState> &p_machine, const Ref<HSMTransition> &p_row);
	bool _remove_transition(const Ref<HSMState> &p_machine, const Ref<HSMTransition> &p_row);
	// Every drawn row running between two nodes, in either direction -- which is what one
	// line on the graph stands for.
	Vector<int> _rows_between(const Ref<HSMState> &p_machine, const Ref<HSMState> &p_a, const Ref<HSMState> &p_b) const;

	// ── Undo ──
	// Snapshots rather than one entry per kind of edit. An edit here is not one call: adding
	// a row to an inherited machine also grows this file a stub for each end it names, and
	// spelling that out per operation would mean writing the stub rule twice -- once to do it
	// and once to take it back. A machine is a dozen small resources, so a copy of the whole
	// authored tree is the cheaper thing to be exactly right about.
	Ref<HSMState> _snapshot() const;
	void _commit_edit(const String &p_name, const Ref<HSMState> &p_before);
	void _restore_root(const Ref<HierarchicalStateMachine> &p_target, const Ref<HSMState> &p_root);

	void _open_node_menu(const StringName &p_name);
	void _on_node_menu_pressed(int p_id);
	void _on_transition_menu_pressed(int p_id);
	void _on_connection_picked(const StringName &p_from, const StringName &p_to);
	void _on_connection_menu_requested(const StringName &p_from, const StringName &p_to);
	void _on_line_menu_pressed(int p_id);
	void _begin_connect_from(const StringName &p_name);
	void _on_connect_to_requested(const StringName &p_from, const StringName &p_to);

	void _on_add_pressed();
	void _on_delete_pressed();
	void _on_delete_nodes_request(const TypedArray<StringName> &p_nodes);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void edit_hsm(const Ref<HierarchicalStateMachine> &p_hsm);
	Ref<HierarchicalStateMachine> get_edited_hsm() const { return hsm; }
	void refresh();

	HSMEditorView();
	~HSMEditorView();
};

#endif // HSM_EDITOR_VIEW_H

#endif // ! TOOLS_ENABLED
