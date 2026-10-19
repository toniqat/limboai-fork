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
#include "task_tree.h"

#ifdef LIMBOAI_MODULE
#include "core/input/input_event.h"
#include "core/templates/hash_map.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/graph_edit.h"
#include "scene/gui/graph_node.h"
#include "scene/gui/label.h"
#include "scene/gui/split_container.h"
#include "scene/animation/tween.h"
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
#include <godot_cpp/classes/tween.hpp>
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
		Ref<HSMStateSpec> spec;
		Ref<HSMStateSpec> machine;
		int depth = 0;
	};

	// A machine that holds states is drawn as one node with those states standing inside
	// it, so its geometry is worked out from what it holds rather than authored: `content`
	// is the empty child that reserves the room, `inner` is what stands in that room, and
	// `rect` is what the last fit made of it.
	struct Nest {
		GraphNode *node = nullptr;
		Control *content = nullptr;
		Ref<HSMStateSpec> spec;
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
	Ref<HSMStateSpec> merged_root;

	// What a right-click on a node offers. `MENU_REMOVE_TRANSITION` is not here: the
	// submenu carries one item per outgoing edge and its id is that edge's row index.
	enum MenuItem {
		MENU_CONNECT,
		MENU_ADD_CHILD,
		MENU_REMOVE,
		MENU_OPEN_TREE,
	};

	VBoxContainer *list_box;
	// No toolbar of its own: what could stand here -- add a state, add a machine, remove one --
	// is on the main screen's toolbar, which is the one place that already knows whether the
	// open document is a machine at all. Two rows of the same three buttons a finger apart is
	// two places to keep right.
	Tree *state_list;
	// The graph and the tree preview share the right half. Two panes rather than one that
	// swaps: the tree a state holds is a thing standing *inside* that state, and a picture
	// that took the graph's place would hide where it sits.
	HSplitContainer *right_split;
	HSMGraphEdit *graph;
	// The glide a double-clicked list row starts; a second one cuts the first short.
	Ref<Tween> focus_tween;
	// The tree the picked state holds. Read-only -- see TaskTree::set_editable().
	VBoxContainer *tree_box;
	Label *tree_title;
	Button *tree_open_btn;
	TaskTree *tree_view;
	// What the preview is showing, which is also what the title's button opens. Kept because
	// the button outlives the click that filled the pane.
	Ref<BehaviorTree> previewed_tree;
	PopupMenu *node_menu;
	PopupMenu *transition_menu;
	// A right-click that landed on an edge rather than on a node. It is its own menu because
	// one line can stand for more than one row, so what it offers is a list of rows and not
	// the node menu with a submenu hanging off it.
	PopupMenu *line_menu;
	// A right-click on the list's top row, which is the file rather than a state: what can
	// be done to the machine as a whole, and where the file is.
	PopupMenu *file_menu;
	// Which node the open menu is about. The menu outlives the click that opened it.
	StringName menu_node;
	// And which machine the open line menu's rows belong to, for the same reason.
	Ref<HSMStateSpec> line_machine;
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
	HashMap<StringName, Ref<HSMStateSpec>> anystate_machines;

	// Deepest first, which is the order a fit has to take: a nest is measured against what
	// stands in it, and one of those may be a nest itself.
	Vector<Nest> nests;
	// Node name -> where that node's nest sits in `nests`.
	HashMap<StringName, int> nest_of;

	bool updating = false;
	// While the view is the one moving nodes. A nest hands a drag down to what it holds,
	// and a fit moves the nest itself -- without this the second would read as the first.
	bool adjusting = false;
	// While a node's panels are being emptied of their margins. Putting a style override on
	// is a theme change, and a theme change is what asks for the trim.
	bool trimming = false;

	// ── Watching one that runs ──
	// The debugger draws with this same view. A machine on disk and the same machine on the
	// map have to be one picture, or reading the second is a thing to learn twice -- and the
	// only screen that can answer "where is it right now" is the one the game is feeding.
	// What is given up for it is every gesture that writes.
	bool read_only = false;
	// An edge that was taken lately, and how much of the glow is left. It fades on its own
	// rather than being switched off by the next one: two states swapping back and forth is
	// the commonest picture a machine makes, and one line switching on and off reads as a
	// fault rather than as movement.
	struct Pulse {
		StringName from;
		StringName to;
		float amount = 0.0;
	};
	Vector<Pulse> pulses;

	static StringName _node_name_for(const Ref<HSMStateSpec> &p_state);
	static StringName _anystate_node_name_for(const Ref<HSMStateSpec> &p_machine);

	void _hide_ports(GraphNode *p_node);
	void _trim_body(GraphNode *p_node);
	void _hollow_nest(GraphNode *p_node);
	GraphNode *_make_node(const Ref<HSMStateSpec> &p_state, const Ref<HSMStateSpec> &p_machine, int p_depth, int p_row, bool p_nests, Control **r_content);
	GraphNode *_make_anystate_node(const Ref<HSMStateSpec> &p_machine, int p_depth, int p_row);
	// Where a node stands. A nest's own size only reaches the node a layout pass later, so
	// the figure the fit worked out is the one that answers until then.
	Rect2 _drawn_rect(GraphNode *p_node) const;
	// Sizes every nest around what it holds and puts it where that lands. Deferred because
	// it reads the insets off a node that has been laid out at least once; `p_retry` is how
	// many times it may wait for that.
	void _fit_nests(int p_retry = 0);
	// A fit that the picture was not ready for is still owed. Kept rather than retried
	// harder: what it is waiting for may be hours away -- see _fit_nests().
	bool fit_pending = false;
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
	Vector<GraphNode *> _populate_graph(const Ref<HSMStateSpec> &p_machine, int p_depth, int &r_row);
	void _wire_graph(const Ref<HSMStateSpec> &p_machine);

	void _on_list_item_selected();
	void _on_list_item_activated();
	void _focus_graph_on(const StringName &p_name);
	void _on_graph_node_selected(Node *p_node);
	void _on_graph_node_gui_input(const Ref<InputEvent> &p_event, const StringName &p_name);
	void _on_graph_end_node_move();

	void _drill_into(const Ref<HSMStateSpec> &p_state);
	// Fills the right pane with the tree `p_state` holds, or hides the pane when it holds
	// none. Showing a tree is not opening it: this one never takes the screen away from the
	// machine, and the button in its title is the only thing here that leaves.
	void _preview_tree(const Ref<HSMStateSpec> &p_state);
	void _on_open_tree_pressed();
	void _inspect(const Ref<HSMStateSpec> &p_drawn);

	Ref<HSMStateSpec> _selected_spec() const;
	Ref<HSMStateSpec> _machine_of(const Ref<HSMStateSpec> &p_state) const;
	// The machine a drawn node belongs to, whether it is a state or an ANYSTATE. Hands back
	// a null state for the latter, which is also what a transition leaving it says.
	Ref<HSMStateSpec> _machine_for_node(const StringName &p_name, Ref<HSMStateSpec> &r_state) const;
	Ref<HSMStateSpec> _authored_of(const Ref<HSMStateSpec> &p_drawn, bool p_create);
	StringName _unique_child_name(const Ref<HSMStateSpec> &p_machine) const;
	void _detach_state(const Ref<HSMStateSpec> &p_machine, const Ref<HSMStateSpec> &p_state);

	bool _remove_drawn(const Ref<HSMStateSpec> &p_drawn);
	// `p_as_machine` gives the new state a child of its own. A state with children *is* a
	// machine -- there is no separate type and no flag -- so the two buttons differ by that
	// one child and nothing else.
	void _add_child_state(const Ref<HSMStateSpec> &p_drawn_parent, bool p_as_machine = false);
	// The row in *this* file that a drawn row stands for, or nothing when it came from the
	// base. Matched on the event too: which of two rows between the same pair this is, is
	// the one thing a pair of endpoints cannot say.
	Ref<HSMTransition> _authored_transition(const Ref<HSMStateSpec> &p_machine, const Ref<HSMTransition> &p_row);
	// The drawn machine with this instance id: the merged root or any nested machine.
	Ref<HSMStateSpec> _drawn_machine_by_id(uint64_t p_id) const;
	bool _remove_transition(const Ref<HSMStateSpec> &p_machine, const Ref<HSMTransition> &p_row);
	// Every drawn row running between two nodes, in either direction -- which is what one
	// line on the graph stands for.
	Vector<int> _rows_between(const Ref<HSMStateSpec> &p_machine, const Ref<HSMStateSpec> &p_a, const Ref<HSMStateSpec> &p_b) const;

	// ── Undo ──
	// Snapshots rather than one entry per kind of edit. An edit here is not one call: adding
	// a row to an inherited machine also grows this file a stub for each end it names, and
	// spelling that out per operation would mean writing the stub rule twice -- once to do it
	// and once to take it back. A machine is a dozen small resources, so a copy of the whole
	// authored tree is the cheaper thing to be exactly right about.
	Ref<HSMStateSpec> _snapshot() const;
	void _commit_edit(const String &p_name, const Ref<HSMStateSpec> &p_before);
	void _restore_root(const Ref<HierarchicalStateMachine> &p_target, const Ref<HSMStateSpec> &p_root);

	void _open_node_menu(const StringName &p_name);
	void _on_node_menu_pressed(int p_id);
	void _on_transition_menu_pressed(int p_id);
	void _on_connection_picked(const StringName &p_from, const StringName &p_to);
	void _on_connection_menu_requested(const StringName &p_from, const StringName &p_to);
	void _on_line_menu_pressed(int p_id);
	void _on_list_item_mouse_selected(const Vector2 &p_pos, int p_button);
	void _on_file_menu_pressed(int p_id);
	void _begin_connect_from(const StringName &p_name);
	void _on_connect_to_requested(const StringName &p_from, const StringName &p_to);

	void _on_delete_nodes_request(const TypedArray<StringName> &p_nodes);

	void _fade_pulses(double p_delta);
	// The same chain, said in the list. Rows are marked rather than selected: selecting one
	// is a thing the reader does, it carries the Inspector and the tree pane with it, and a
	// machine that took the selection every time it moved would take them from under a hand
	// that had just put them somewhere.
	void _mark_list(const Vector<uint64_t> &p_ids);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void edit_hsm(const Ref<HierarchicalStateMachine> &p_hsm);
	Ref<HierarchicalStateMachine> get_edited_hsm() const { return hsm; }
	void refresh();

	// What the main toolbar drives when a machine is the open document. Each acts on the
	// picked state, and adding falls back to the root -- a machine always has one, so the
	// button never has nothing to act on.
	void add_state();
	void add_machine();
	void remove_selected();

	// Turns off everything that writes: the menus, the connect gesture, the delete request,
	// the Inspector, and the remembering of where a node was dragged to. What is left is
	// exactly the picture and the picking. Set once, by whoever built the view.
	void set_read_only(bool p_read_only);
	bool is_read_only() const { return read_only; }

	// Fills `p_list` under `p_parent` with the states inside `p_machine`, one row each, in the
	// same colors and icons as the state list. Static so that the LimboAI tab's All view can
	// show a machine's outline without standing up a whole view for it.
	static void populate_state_list(Tree *p_list, const Ref<HSMStateSpec> &p_machine, TreeItem *p_parent);

	// What the HSMStateSpec inspector lists for a state of the open file. Answered from the
	// merged tree, so inherited rows show too. Returns {"outgoing": [...], "incoming": [...]},
	// each line a {"text", "machine", "row", "inherited"} Dictionary where `machine` is the
	// drawn machine's instance id and `row` the index in its transitions; outgoing lines are
	// in the order LimboHSM evaluates polled rows (priority descending, the state's own rows
	// before ANYSTATE rows, then file order). Empty when the state is not on screen.
	Dictionary describe_state_transitions(const Ref<HSMStateSpec> &p_authored) const;
	// Picks the edge a row is drawn as and inspects the row, the same as clicking the line
	// would, except that the row is named exactly (a click on a line that stands for two rows
	// cannot say which one was meant).
	void select_transition(int64_t p_machine_id, int p_row);

	// Where the machine is standing, outermost first. Everything off the chain is dimmed:
	// the question a running machine is being asked is "where is it", and one lit line
	// answers it better than a dozen equally bright boxes. An empty chain lights all of them
	// again, which is what "it is not running" looks like.
	void set_active_chain(const Vector<Ref<HSMStateSpec>> &p_chain);
	// One edge, just taken. Both ends may be missing (an entry into a machine has no state
	// it came from), and then nothing glows.
	void pulse_transition(const Ref<HSMStateSpec> &p_from, const Ref<HSMStateSpec> &p_to);

	HSMEditorView();
	~HSMEditorView();
};

#endif // HSM_EDITOR_VIEW_H

#endif // ! TOOLS_ENABLED
