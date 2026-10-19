/**
 * hsm_editor_view.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#include "hsm_editor_view.h"

#include "../compat/editor.h"
#include "../compat/editor_scale.h"
#include "../compat/limbo_compat.h"
#include "../compat/translation.h"
#include "../util/limbo_string_names.h"
#include "../util/limbo_utility.h"

#ifdef LIMBOAI_MODULE
#include "core/input/input_event.h"
#include "core/string/string_name.h"
#include "scene/gui/control.h"
#include "editor/docks/filesystem_dock.h"
#include "scene/resources/style_box_flat.h"
#include "servers/display_server.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/file_system_dock.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/tree_item.hpp>
#include <godot_cpp/classes/undo_redo.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#endif // LIMBOAI_GDEXTENSION

namespace {

// Colors say what a state *is* -- there is no other place on the graph that answers it.
const Color COLOR_PLAIN = Color(0.55, 0.62, 0.72);
const Color COLOR_MACHINE = Color(0.42, 0.72, 0.55);
const Color COLOR_TREE = Color(0.66, 0.50, 0.82);
const Color COLOR_ANYSTATE = Color(0.78, 0.62, 0.35);

const int COLUMN_WIDTH = 300;
const int ROW_HEIGHT = 120;

// The room a machine's node leaves around the states standing in it -- and, since the box is
// laid over them, the smallest it may be made. Half a grid square at GraphEdit's own
// snapping distance of 20: enough that the border still reads as drawn around the states
// rather than as clipped to them, and little enough that the states inside read as the
// machine's contents rather than as a group standing somewhere in a larger empty box.
//
// A state's shadow and a bowed edge are not kept inside the box at this width -- they run
// over the border. The room is not what holds them; it is only what the border is offset by.
//
// Where that node's border runs and how tall its title bar is are not here -- they are read
// back off the node itself, so the theme decides them and this does not have to guess.
const int NEST_PADDING = 10;

// The strip inside a nest's other three edges that still answers to the mouse. Everything
// further in is the room, and the room belongs to what stands in it: a press there has to
// reach a state or an edge, not the box they are drawn inside.
//
// It is the resizer that sets the width, not the eye. GraphElement takes the resizer's own
// icon as its hotzone and puts it in the bottom-right corner, so a band narrower than that
// icon would leave part of the handle over the hole -- a corner that resizes along its outer
// edge and does nothing along its inner one.
const int NEST_GRAB_BAND = 16;

// Inherited states are drawn, because a machine that showed only what its own file adds
// would be a picture of a diff rather than of the machine. They are dimmed because the
// file being edited is not where they can be changed.
const float INHERITED_ALPHA = 0.55;

// What a state that a running machine is not standing in is dimmed to. The same reading as
// above and deliberately the same figure: in both cases the box is drawn because the machine
// has it, and dimmed because it is not the one being talked about.
const float INACTIVE_ALPHA = 0.55;

// How long an edge stays lit after it was taken. Long enough to catch out of the corner of an
// eye, short enough that a machine flipping between two states does not simply leave both
// lines on -- at which point the glow stops meaning "just now".
const float PULSE_SECONDS = 0.8;

// ── Copying an authored tree ──────────────────────────────────────────────────

// One state and the copy standing for it, kept so that the references can be rebuilt once
// every state on both sides exists -- the same two passes the merge in hsm_resource.cpp
// takes, and for the same reason: a transition names states that may not be copied yet.
struct ClonePair {
	Ref<HSMStateSpec> src;
	Ref<HSMStateSpec> copy;
};

Ref<HSMStateSpec> _mapped(const HashMap<uint64_t, Ref<HSMStateSpec>> &p_map, const Ref<HSMStateSpec> &p_state) {
	if (p_state.is_null()) {
		return nullptr;
	}
	const Ref<HSMStateSpec> *found = p_map.getptr(p_state->get_instance_id());
	// A reference to something outside the tree is handed on as it stands. It is already
	// broken, and a copy that quietly dropped it would make an undo lose more than it
	// restores.
	return found != nullptr ? *found : p_state;
}

Ref<HSMStateSpec> _clone_state(const Ref<HSMStateSpec> &p_src, HashMap<uint64_t, Ref<HSMStateSpec>> &r_map, Vector<ClonePair> &r_pairs) {
	Ref<HSMStateSpec> copy;
	copy.instantiate();
	// Carried over, and this is the point of doing the copy by hand at all: the id is what
	// names a sub-resource in the file, so a copy that took a fresh one would rename every
	// state in the machine and put the whole file in the next diff.
	copy->set_scene_unique_id(p_src->get_scene_unique_id());
	copy->set_state_name(p_src->get_state_name());
	copy->set_state_script(p_src->get_state_script());
	// Duplicated: the dictionary is a value, and a snapshot that shared it would be changed by
	// the very edit it is meant to undo.
	copy->set_state_properties(p_src->get_state_properties().duplicate());
	copy->set_behavior_tree(p_src->get_behavior_tree());
	copy->set_graph_position(p_src->get_graph_position());
	copy->set_graph_size(p_src->get_graph_size());
	copy->set_anystate_graph_position(p_src->get_anystate_graph_position());
	r_map[p_src->get_instance_id()] = copy;
	r_pairs.push_back(ClonePair{ p_src, copy });

	TypedArray<HSMStateSpec> children = p_src->get_children();
	TypedArray<HSMStateSpec> out;
	for (int i = 0; i < children.size(); i++) {
		Ref<HSMStateSpec> child = children[i];
		if (child.is_null() || r_map.has(child->get_instance_id())) {
			continue;
		}
		out.append(_clone_state(child, r_map, r_pairs));
	}
	copy->set_children(out);
	return copy;
}

// A tree that shares nothing with the one it was made from.
//
// Not duplicate(true): a resource sitting in an Array comes back as the same object, so
// `children` and `transitions` would point straight back at the states the copy is meant to
// be safe from -- and the first edit after an undo would reach into the undo history.
Ref<HSMStateSpec> _clone_tree(const Ref<HSMStateSpec> &p_root) {
	if (p_root.is_null()) {
		return nullptr;
	}
	HashMap<uint64_t, Ref<HSMStateSpec>> map;
	Vector<ClonePair> pairs;
	Ref<HSMStateSpec> root = _clone_state(p_root, map, pairs);

	for (int i = 0; i < pairs.size(); i++) {
		const ClonePair &pair = pairs[i];
		TypedArray<HSMTransition> rows = pair.src->get_transitions();
		TypedArray<HSMTransition> out;
		for (int j = 0; j < rows.size(); j++) {
			Ref<HSMTransition> row = rows[j];
			if (row.is_null()) {
				continue;
			}
			Ref<HSMTransition> copy;
			copy.instantiate();
			copy->set_scene_unique_id(row->get_scene_unique_id());
			copy->set_from(_mapped(map, row->get_from()));
			copy->set_to(_mapped(map, row->get_to()));
			copy->set_event(row->get_event());
			copy->set_note(row->get_note());
			// Shared, not copied. The condition is a sub-resource with exported settings of its
			// own, and those are edited (and undone) by the Inspector on that very object; a
			// snapshot that swapped it for a copy would cut the Inspector's history loose.
			copy->set_condition(row->get_condition());
			copy->set_priority(row->get_priority());
			out.append(copy);
		}
		pair.copy->set_transitions(out);
		// The declared value, not get_initial_child(): the fall back to the first child is
		// an answer, and a copy that wrote it down would turn silence into a decision.
		pair.copy->set_initial_child(_mapped(map, pair.src->get_declared_initial_child()));
	}
	return root;
}

} // namespace

StringName HSMEditorView::_node_name_for(const Ref<HSMStateSpec> &p_state) {
	// The instance id, not the state name: names are only unique among siblings, and
	// GraphEdit needs one name per node across the whole graph.
	return StringName(itos(p_state->get_instance_id()));
}

StringName HSMEditorView::_anystate_node_name_for(const Ref<HSMStateSpec> &p_machine) {
	return StringName("any_" + itos(p_machine->get_instance_id()));
}

void HSMEditorView::_hide_ports(GraphNode *p_node) {
	p_node->add_theme_icon_override("port", blank_port);
}

// The body of a node with nothing in it is still not flush with the title: the panel
// keeps margins of its own above and below whatever stands there, and with nothing
// standing there the two meet as one band of empty node. Emptied here rather than
// assumed away -- the numbers belong to the theme, and only the theme knows them.
//
// Emptied, not closed up. A box needs room to finish being a box: the bottom keeps as
// much as its own rounded corners and border need to be drawn, or the panel is cut off
// square where it should curve back in -- and the line that is not there is also the line
// that does not light up when the node is picked, so the highlight goes round three sides
// and stops. That floor is asked of the stylebox rather than picked, because the corner
// it has to clear is the theme's number and no one else's.
//
// Read on `theme_changed` and not at build time for two reasons, and the first is that
// there is no theme to read yet: a Control answers with the default one until it is in
// the tree, and these are made before they are added. The second is that an editor theme
// is switched while the view is open, and a copy taken once would keep the old colors.
void HSMEditorView::_trim_body(GraphNode *p_node) {
	if (trimming) {
		// Putting an override on is itself a theme change.
		return;
	}
	trimming = true;
	// Every panel the node draws with, not just the resting one: the size a Control asks
	// for is worked out from one of them, and a state that gained a band of empty room the
	// moment it was picked would read as the graph moving under the click.
	const char *panels[] = { "panel", "panel_selected", "panel_focus" };
	for (int i = 0; i < 3; i++) {
		// Cleared before any of them is read. With an override in place the getter answers
		// with our own copy, and a copy trimmed from a trimmed copy never sees the theme
		// again.
		REMOVE_STYLEBOX_OVERRIDE(p_node, StringName(panels[i]));
	}

	// One floor for all three and not one each: the three are the same box in different
	// colors, but a picked one whose border is a hair thicker would ask for a taller node
	// than the resting one, and the graph would twitch under the click that picked it.
	//
	// The height is the corner and nothing on top of it -- that is as little as a box can
	// be given and still be closed, and any more is the empty band this whole thing is for
	// taking away. What is bought with the room instead is the *arc*: given exactly the
	// radius it asks for, the curve is the entire height and the side has no straight run
	// left, so it is already turning where it meets the title bar's square corner and the
	// two lines read as one crossing the other. So the corner is drawn a little smaller
	// than the theme's, by as much as the border that draws it -- the thickness the eye is
	// asked to follow round the turn -- and the difference becomes that straight run.
	float floor = 0.0;
	float straight = 2 * EDSCALE;
	for (int i = 0; i < 3; i++) {
		StringName name = StringName(panels[i]);
		if (!p_node->has_theme_stylebox(name, "GraphNode")) {
			continue;
		}
		Ref<StyleBox> base = p_node->get_theme_stylebox(name, "GraphNode");
		Ref<StyleBoxFlat> flat = base;
		if (flat.is_null()) {
			// Some other kind of box draws by rules this cannot read. It keeps what it asked
			// for below; only the room above the body goes.
			floor = MAX(floor, base->get_margin(SIDE_BOTTOM));
			continue;
		}
		floor = MAX(floor, MAX(flat->get_corner_radius(CORNER_BOTTOM_LEFT), flat->get_corner_radius(CORNER_BOTTOM_RIGHT)));
		floor = MAX(floor, flat->get_border_width(SIDE_BOTTOM));
		straight = MAX(straight, flat->get_border_width(SIDE_BOTTOM));
		straight = MAX(straight, MAX(flat->get_border_width(SIDE_LEFT), flat->get_border_width(SIDE_RIGHT)));
	}

	for (int i = 0; i < 3; i++) {
		StringName name = StringName(panels[i]);
		if (!p_node->has_theme_stylebox(name, "GraphNode")) {
			continue;
		}
		Ref<StyleBox> trimmed = p_node->get_theme_stylebox(name, "GraphNode")->duplicate();
		Ref<StyleBoxFlat> flat = trimmed;
		if (flat.is_valid()) {
			// Only the two that are being asked to fit. The top pair is under the title bar
			// and squared off there already.
			int radius = int(MAX(floor - straight, 0.0f));
			flat->set_corner_radius(CORNER_BOTTOM_LEFT, radius);
			flat->set_corner_radius(CORNER_BOTTOM_RIGHT, radius);
		}
		// The top needs nothing: that edge is under the title bar, which draws its own.
		trimmed->set_content_margin(SIDE_TOP, 0);
		trimmed->set_content_margin(SIDE_BOTTOM, floor);
		ADD_STYLEBOX_OVERRIDE(p_node, name, trimmed);
	}
	trimming = false;
}

// A machine that holds states is drawn as a node with those states standing *inside* it,
// which is what `p_nests` asks for. Depth alone does not say what is in what: one column
// further right and unrelated look the same, so the states of `Combat` read as siblings of
// the root's own.
//
// It is the node that grows rather than a frame put around it. A GraphFrame is a
// GraphElement, not a GraphNode -- it has no slots and so no ports, and a transition can
// only end at a port -- so a frame can never be the thing an edge arrives at, and a frame
// *plus* the machine's own node draws the same machine twice, once as a title bar and once
// as a box inside it. The node it already has is made big enough to be the box.
//
// What it says inside is then the states themselves. The word "machine (4)" was standing
// in for a picture that could not be drawn; there is no reason to keep the caption once
// the picture is there.
GraphNode *HSMEditorView::_make_node(const Ref<HSMStateSpec> &p_state, const Ref<HSMStateSpec> &p_machine, int p_depth, int p_row, bool p_nests, Control **r_content) {
	GraphNode *node = nullptr;
	if (p_nests) {
		// A box, and a box is a thing with a hole in it -- see HSMNestNode. It is also the
		// only kind of node here that can be resized, because it is the only one whose size
		// means anything: the others are as big as their title.
		HSMNestNode *nest = memnew(HSMNestNode);
		nest->set_resizable(!read_only);
		node = nest;
	} else {
		node = memnew(GraphNode);
	}
	// Nothing here is moved by hand while the machine belongs to a running game. Dragging one
	// used to be allowed on the grounds that untangling a picture is reading it, but there is
	// nowhere for the untangling to go: it lasts until the machine is picked again, and the
	// arrangement it is undoing is the file's own -- which the graph now shows. A gesture that
	// takes and does not keep is read as one that keeps.
	node->set_draggable(!read_only);
	node->set_name(_node_name_for(p_state));
	node->set_title(String(p_state->get_state_name()));

	Color color = COLOR_PLAIN;
	if (p_state->is_machine()) {
		color = COLOR_MACHINE;
		// Nothing written: being a machine *is* holding states, and they are drawn in here.
	} else if (p_state->get_behavior_tree().is_valid()) {
		color = COLOR_TREE;
		// Which tree it holds is not written either. It used to be the file name, and a
		// name needs a body to stand in -- so the one kind of state that had something to
		// say gave every *other* node a strip of empty room to match it. Which tree it is
		// is read in the pane on the right, and that it holds one at all is the icon the
		// list puts beside it.
		node->set_tooltip_text(TTR("Its behavior tree is shown on the right."));
	}

	// A GraphNode needs at least one child control before slot 0 exists, so the body is an
	// empty one rather than none at all.
	Control *spacer = memnew(Control);
	// A Control stops the mouse by default, and this one is laid over the whole body of
	// its node -- including, in a nest, the room every state inside it stands in. Nothing
	// is drawn here and nothing is clicked here; the node behind it decides.
	spacer->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	node->add_child(spacer);
	if (p_nests) {
		// A nest is the one node whose body is a picture -- the states standing in it --
		// so it keeps a floor to be grown from.
		spacer->set_custom_minimum_size(Size2(0, 4) * EDSCALE);
		if (r_content != nullptr) {
			*r_content = spacer;
		}
	} else {
		// Everywhere else the body is nothing, and nothing is drawn no taller than nothing.
		node->connect("theme_changed", callable_mp(this, &HSMEditorView::_trim_body).bind(node));
	}
	node->set_slot(0, true, 0, color, true, 0, color);
	_hide_ports(node);

	if (p_state->is_inherited()) {
		node->set_self_modulate(Color(1, 1, 1, INHERITED_ALPHA));
		node->set_tooltip_text(TTR("Inherited from the base machine. Change it there."));
	}

	// Drilling in and the context menu are the node's own gestures, not the graph's:
	// GraphEdit has no "node activated" of its own, and the `gui_input` signal is emitted
	// before the node's own dragging handler sees the press.
	node->connect("gui_input", callable_mp(this, &HSMEditorView::_on_graph_node_gui_input).bind(node->get_name()));

	Vector2 pos = p_state->get_graph_position();
	if (pos == Vector2()) {
		pos = Vector2(p_depth * COLUMN_WIDTH, p_row * ROW_HEIGHT);
	}
	node->set_position_offset(pos);
	return node;
}

GraphNode *HSMEditorView::_make_anystate_node(const Ref<HSMStateSpec> &p_machine, int p_depth, int p_row) {
	GraphNode *node = memnew(GraphNode);
	node->set_draggable(!read_only);
	node->set_name(_anystate_node_name_for(p_machine));
	node->set_title(String::utf8("✱ ANYSTATE"));

	// It used to say "no state is being left" here. That is not what it *is*, it is what
	// cannot be done to it, and where that belongs is the moment someone tries: aiming an
	// edge at this node marks it refused before the click lands.
	Control *spacer = memnew(Control);
	spacer->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	node->add_child(spacer);
	node->connect("theme_changed", callable_mp(this, &HSMEditorView::_trim_body).bind(node));
	// Only an outgoing port: ANYSTATE is never a destination.
	node->set_slot(0, false, 0, COLOR_ANYSTATE, true, 0, COLOR_ANYSTATE);
	_hide_ports(node);

	node->connect("gui_input", callable_mp(this, &HSMEditorView::_on_graph_node_gui_input).bind(node->get_name()));

	// Kept by the machine, for the same reason a state's is kept by the state: it was put
	// there by hand, and a picture that forgets half of what was arranged in it is a picture
	// nobody arranges twice.
	Vector2 pos = p_machine->get_anystate_graph_position();
	if (pos == Vector2()) {
		pos = Vector2(p_depth * COLUMN_WIDTH, p_row * ROW_HEIGHT);
	}
	node->set_position_offset(pos);
	return node;
}

Vector<GraphNode *> HSMEditorView::_populate_graph(const Ref<HSMStateSpec> &p_machine, int p_depth, int &r_row) {
	Vector<GraphNode *> elements;
	ERR_FAIL_COND_V(p_machine.is_null(), elements);

	// ANYSTATE only stands where a machine actually has such a row -- an empty one on
	// every level would be noise on levels that never use it.
	bool has_anystate = false;
	TypedArray<HSMTransition> rows = p_machine->get_transitions();
	for (int i = 0; i < rows.size(); i++) {
		Ref<HSMTransition> row = rows[i];
		if (row.is_valid() && row->get_from().is_null() && row->get_to().is_valid()) {
			has_anystate = true;
			break;
		}
	}
	if (has_anystate) {
		GraphNode *any = _make_anystate_node(p_machine, p_depth, r_row);
		graph->add_child(any);
		anystate_machines[any->get_name()] = p_machine;
		elements.push_back(any);
		r_row += 1;
	}

	TypedArray<HSMStateSpec> children = p_machine->get_children();
	for (int i = 0; i < children.size(); i++) {
		Ref<HSMStateSpec> spec = children[i];
		if (spec.is_null()) {
			continue;
		}
		StringName node_name = _node_name_for(spec);
		if (node_names.has(spec->get_instance_id())) {
			// The same resource twice would be two nodes with one name.
			continue;
		}

		// A nest reaches above what it holds by a title bar and below it by a border, so the
		// fallback layout leaves a row empty on either side. Without it that border lands on
		// the sibling above or below and reads as taking it in.
		bool holds = spec->is_machine();
		if (holds && r_row > 0) {
			r_row += 1;
		}

		Control *content = nullptr;
		GraphNode *node = _make_node(spec, p_machine, p_depth, r_row, holds, &content);
		graph->add_child(node);

		DrawnState entry;
		entry.spec = spec;
		entry.machine = p_machine;
		entry.depth = p_depth;
		drawn[node_name] = entry;
		node_names[spec->get_instance_id()] = node_name;
		// A nest takes no row of its own: the room it needs is the room its states take,
		// and it is laid over them rather than above them.
		if (!holds) {
			r_row += 1;
		}

		if (holds) {
			Vector<GraphNode *> inner = _populate_graph(spec, p_depth + 1, r_row);
			if (inner.is_empty() || content == nullptr) {
				// Nothing was drawn inside after all (the same resource twice), so there is
				// nothing to be a nest around.
				r_row += 1;
			} else {
				// A nest is a backdrop for what stands in it. Where in the child list that
				// puts it is _enforce_nest_order()'s to say, and it is said again every time
				// GraphEdit lifts something -- so it is not said here.
				Nest nest;
				nest.node = node;
				nest.content = content;
				nest.spec = spec;
				nest.inner = inner;
				nest_of[node_name] = nests.size();
				nests.push_back(nest);
				node->connect("position_offset_changed", callable_mp(this, &HSMEditorView::_on_nest_moved).bind(node_name));
				node->connect("raise_request", callable_mp(this, &HSMEditorView::_on_nest_raised));
				node->connect("resize_request", callable_mp(this, &HSMEditorView::_on_nest_resize_request).bind(node_name));
				node->connect("resize_end", callable_mp(this, &HSMEditorView::_on_nest_resize_end).bind(node_name));
				r_row += 1;
			}
		}
		elements.push_back(node);
	}
	return elements;
}

// ── A machine's node, sized around what stands in it ──────────────────────────

Rect2 HSMEditorView::_drawn_rect(GraphNode *p_node) const {
	const int *index = nest_of.getptr(p_node->get_name());
	if (index != nullptr && nests[*index].rect.size.x > 0.0) {
		// A nest that has already been fitted this pass. Its node only takes that size a
		// layout later, so asking the node would measure the picture before the change.
		return nests[*index].rect;
	}
	Size2 size = p_node->get_size();
	if (size.x < 1.0 || size.y < 1.0) {
		size = p_node->get_combined_minimum_size();
	}
	return Rect2(p_node->get_position_offset(), size);
}

// Deepest first, so a nest inside a nest is already the size it will be when the one
// around it measures what it holds.
//
// The insets are read off the node rather than worked out: where the empty child sits
// inside its node *is* the title bar plus the panel's margins, and those come from the
// theme. That reading needs a node that has been laid out once, which is why this is
// deferred and why it waits for one when it arrives too early.
void HSMEditorView::_fit_nests(int p_retry) {
	if (nests.is_empty()) {
		fit_pending = false;
		return;
	}
	// A view nobody is looking at has no sizes to read: a Control that is not visible in the
	// tree never has its minimum size applied, so every node here measures zero and the waiting
	// above would spend its four tries against a picture that was never going to be laid out.
	// In the debugger that is the ordinary case rather than a corner of one -- a character is
	// picked the moment the game registers its first machine, and the panel is opened some time
	// after that -- and what it left behind was a machine drawn as a box the size of its own
	// title, standing at the origin with the states it is a box around scattered elsewhere.
	// So the fit is not given up here; it is set aside until there is something to measure.
	if (!is_visible_in_tree()) {
		fit_pending = true;
		return;
	}
	for (int i = 0; i < nests.size(); i++) {
		Nest &nest = nests.write[i];
		if (nest.node == nullptr || nest.content == nullptr) {
			continue;
		}
		Vector2 top_left = nest.content->get_position();
		Size2 node_size = nest.node->get_size();
		if (node_size.y < 1.0 || top_left.y < 1.0) {
			// Still owed either way: the tries run out within a few frames, and being shown
			// again is the one thing that can change the answer after they have.
			fit_pending = true;
			if (p_retry < 4) {
				callable_mp(this, &HSMEditorView::_fit_nests).bind(p_retry + 1).call_deferred();
			}
			return;
		}
		Vector2 bottom_right = node_size - top_left - nest.content->get_size();

		Rect2 bounds;
		bool first = true;
		for (int j = 0; j < nest.inner.size(); j++) {
			Rect2 rect = _drawn_rect(nest.inner[j]);
			bounds = first ? rect : bounds.merge(rect);
			first = false;
		}
		if (first) {
			continue;
		}

		Vector2 padding = Vector2(NEST_PADDING, NEST_PADDING) * EDSCALE;
		nest.inset_tl = top_left;
		nest.inset_br = bottom_right;
		nest.min_size = top_left + bounds.size + padding * 2.0 + bottom_right;

		// Bigger than the floor only if someone asked for it. The file says nothing about the
		// size of a box that was never resized, and it must keep saying nothing: the floor
		// moves whenever a state inside does, and a size written down on every layout would
		// pin the box to whatever the states happened to be doing that day.
		Size2 wanted = nest.spec.is_valid() ? Size2(nest.spec->get_graph_size()) : Size2();
		Size2 size = Size2(MAX(nest.min_size.x, wanted.x), MAX(nest.min_size.y, wanted.y));

		nest.content->set_custom_minimum_size(size - top_left - bottom_right);

		Vector2 offset = bounds.position - padding - top_left;
		nest.rect = Rect2(offset, size);
		// The view is moving it, not a drag: what it holds must not be handed the delta.
		adjusting = true;
		nest.node->set_position_offset(offset);
		nest.node->set_size(size);
		adjusting = false;
		nest.last_offset = offset;
		_update_nest_hole(nest);
	}
	fit_pending = false;
	// The rings a running machine is drawn with are laid down where the nodes stood, and the
	// nodes have just been moved. Nothing else moves them once a graph is up -- a read-only
	// view is the only one that has rings, and nothing on it can be dragged.
	graph->redraw_overlay();
	// Deferred, unlike the one in refresh(): a fit also arrives from the end of a drag, and
	// that is GraphEdit part-way through reading a press. Shuffling its children there is not
	// something to find out about the hard way.
	callable_mp(this, &HSMEditorView::_enforce_nest_order).call_deferred();
}

void HSMEditorView::_update_nest_hole(const Nest &p_nest) {
	HSMNestNode *node = Object::cast_to<HSMNestNode>(p_nest.node);
	if (node == nullptr) {
		return;
	}
	float band = float(NEST_GRAB_BAND) * EDSCALE;
	// Down from where the body starts, which is under the title bar, and in from the other
	// three edges. Not `inset_tl.x` in from the sides: the panel's own margin is a couple of
	// pixels, and a couple of pixels is not something a mouse can be aimed at.
	Rect2 hole(band, p_nest.inset_tl.y, p_nest.rect.size.x - band * 2.0, p_nest.rect.size.y - p_nest.inset_tl.y - band);
	if (hole.size.x <= 0.0 || hole.size.y <= 0.0) {
		node->set_hole(Rect2());
		return;
	}
	node->set_hole(hole);
}

// Outermost first, because each move puts one in front of the ones moved before it and
// `nests` is deepest first. What lands above them all is everything that is not a nest --
// the states themselves -- and GraphEdit's own `connections_layer`, so the lines stay on top
// of the boxes and under nothing.
void HSMEditorView::_enforce_nest_order() {
	for (int i = 0; i < nests.size(); i++) {
		GraphNode *node = nests[i].node;
		if (node != nullptr && node->get_parent() == graph) {
			graph->move_child(node, 0);
		}
	}
}

void HSMEditorView::_on_nest_raised() {
	// Deferred: GraphEdit does its lifting from the same press, and this has to be what
	// happens last rather than what happens first.
	callable_mp(this, &HSMEditorView::_enforce_nest_order).call_deferred();
}

// While the resizer is being dragged. GraphElement asks rather than acts, so the clamp is
// simply the answer given back -- there is no size the node passes through and is pulled out
// of afterwards.
void HSMEditorView::_on_nest_resize_request(const Vector2 &p_size, const StringName &p_name) {
	const int *index = nest_of.getptr(p_name);
	if (index == nullptr) {
		return;
	}
	Nest &nest = nests.write[*index];
	if (nest.node == nullptr || nest.content == nullptr) {
		return;
	}
	Size2 size = Size2(MAX(p_size.x, nest.min_size.x), MAX(p_size.y, nest.min_size.y));
	nest.content->set_custom_minimum_size(size - nest.inset_tl - nest.inset_br);
	adjusting = true;
	nest.node->set_size(size);
	adjusting = false;
	nest.rect.size = size;
	_update_nest_hole(nest);
}

void HSMEditorView::_on_nest_resize_end(const Vector2 &p_size, const StringName &p_name) {
	const int *index = nest_of.getptr(p_name);
	if (index == nullptr || hsm.is_null()) {
		return;
	}
	Nest &nest = nests.write[*index];
	if (nest.spec.is_null()) {
		return;
	}
	Size2 size = Size2(MAX(p_size.x, nest.min_size.x), MAX(p_size.y, nest.min_size.y)).round();
	// Back at the floor is back to saying nothing. Writing the floor down instead would fix
	// the box at today's contents, and the next state moved inside it would be cut off by a
	// size that was never actually asked for.
	if (size.x <= nest.min_size.x + 1.0 && size.y <= nest.min_size.y + 1.0) {
		size = Size2();
	}
	if (nest.spec->get_graph_size() == size) {
		return;
	}
	Ref<HSMStateSpec> before = _snapshot();
	Ref<HSMStateSpec> authored = _authored_of(nest.spec, true);
	if (authored.is_null()) {
		return;
	}
	nest.spec->set_graph_size(size);
	authored->set_graph_size(size);
	_commit_edit(TTR("Resize State Machine"), before);
	emit_signal("hsm_edited");
}

// Dragging a machine takes what is in it along -- it is drawn as one thing, so it moves as
// one. Only the direct contents are pushed: a nest among them gets its own turn here and
// pushes what *it* holds, so a state two levels down is moved once and not twice.
//
// Anything already selected is left alone. GraphEdit moves every selected element by the
// same drag, so pushing it again would move it twice as far as the cursor.
void HSMEditorView::_on_nest_moved(const StringName &p_name) {
	if (adjusting) {
		return;
	}
	const int *index = nest_of.getptr(p_name);
	if (index == nullptr) {
		return;
	}
	Nest &nest = nests.write[*index];
	if (nest.node == nullptr) {
		return;
	}
	Vector2 offset = nest.node->get_position_offset();
	Vector2 delta = offset - nest.last_offset;
	nest.last_offset = offset;
	nest.rect.position = offset;
	if (delta.is_zero_approx()) {
		return;
	}
	for (int i = 0; i < nest.inner.size(); i++) {
		GraphNode *node = nest.inner[i];
		if (node == nullptr || node->is_selected()) {
			continue;
		}
		node->set_position_offset(node->get_position_offset() + delta);
	}
}

void HSMEditorView::_wire_graph(const Ref<HSMStateSpec> &p_machine) {
	ERR_FAIL_COND(p_machine.is_null());

	TypedArray<HSMTransition> rows = p_machine->get_transitions();
	for (int i = 0; i < rows.size(); i++) {
		Ref<HSMTransition> row = rows[i];
		if (row.is_null() || row->get_to().is_null()) {
			continue;
		}
		const StringName *to = node_names.getptr(row->get_to()->get_instance_id());
		if (to == nullptr) {
			continue;
		}
		StringName from;
		if (row->get_from().is_valid()) {
			const StringName *found = node_names.getptr(row->get_from()->get_instance_id());
			if (found == nullptr) {
				continue;
			}
			from = *found;
		} else {
			from = _anystate_node_name_for(p_machine);
		}
		if (from == *to) {
			// A self-loop would be drawn on top of itself; the list still holds it.
			continue;
		}
		graph->connect_node(from, 0, *to, 0);
	}

	TypedArray<HSMStateSpec> children = p_machine->get_children();
	for (int i = 0; i < children.size(); i++) {
		Ref<HSMStateSpec> spec = children[i];
		if (spec.is_valid() && spec->is_machine()) {
			_wire_graph(spec);
		}
	}
}

void HSMEditorView::populate_state_list(Tree *p_list, const Ref<HSMStateSpec> &p_machine, TreeItem *p_parent) {
	ERR_FAIL_COND(p_machine.is_null());

	// The initial child leads the list. It carries no mark of its own, so being the top
	// row is the whole of "where does this machine start" -- and the declared order does
	// not answer that on its own: merging appends what a child file adds, which leaves
	// the boss machine's `Pattern` at the bottom of the very state it starts in.
	// The graph and the file keep their own order; this one belongs to the list.
	TypedArray<HSMStateSpec> declared = p_machine->get_children();
	Ref<HSMStateSpec> initial = p_machine->get_initial_child();
	Vector<Ref<HSMStateSpec>> children;
	for (int i = 0; i < declared.size(); i++) {
		Ref<HSMStateSpec> spec = declared[i];
		if (spec.is_null()) {
			continue;
		}
		if (spec == initial && !children.is_empty()) {
			children.insert(0, spec);
		} else {
			children.push_back(spec);
		}
	}

	for (int i = 0; i < children.size(); i++) {
		const Ref<HSMStateSpec> &spec = children[i];
		TreeItem *item = p_list->create_item(p_parent);
		item->set_text(0, String(spec->get_state_name()));
		item->set_metadata(0, Variant(int64_t(spec->get_instance_id())));

		// What kind of state it is, said as a picture beside the name rather than as words
		// after it. It used to read "Wary · BT", which is a caption -- it grows with the
		// name, it is read left to right like the name, and it says in text what the three
		// icons already in this editor say everywhere else.
		Color color = COLOR_PLAIN;
		String icon_name = "LimboState";
		if (spec->is_machine()) {
			color = COLOR_MACHINE;
			icon_name = "LimboHSM";
		} else if (spec->get_behavior_tree().is_valid()) {
			color = COLOR_TREE;
			icon_name = "BTState";
		}
		if (spec->is_inherited()) {
			color.a = INHERITED_ALPHA;
		}
		bool tinted = color != COLOR_PLAIN;
		if (tinted) {
			item->set_custom_color(0, color);
		}
		Ref<Texture2D> icon = LimboUtility::get_singleton()->get_task_icon(icon_name);
		if (icon.is_valid()) {
			item->set_icon(0, icon);
			// Held to the row's own height. These are the addon's files in an extension
			// build, imported at whatever scale they were, and one that came in larger
			// would set the height of every row in the list.
			item->set_icon_max_width(0, 16 * EDSCALE);
			// Whatever the name was given and nothing else: the icon and the word are one
			// label, and an inherited state that dimmed only half of itself would read as
			// half inherited. A name left to the theme leaves its icon alone too.
			item->set_icon_modulate(0, tinted ? color : Color(1, 1, 1));
		}

		if (spec->is_machine()) {
			populate_state_list(p_list, spec, item);
			item->set_collapsed(false);
		}
	}
}

void HSMEditorView::refresh() {
	updating = true;

	// The nodes it was aimed between are about to be deleted, and a half-made edge that
	// survives a rebuild points at whatever inherits the name.
	graph->cancel_connect();
	// Before the connections go, not after: dropping the pick turns its highlight back off,
	// and there is nothing left to turn off once the list has been emptied.
	graph->clear_picked_connection();
	graph->clear_connections();
	// Only the elements we put there. GraphEdit keeps a non-internal `connections_layer`
	// child of its own (and HSMGraphEdit an `overlay`), and removing either takes
	// the lines with it -- after which every node move errors out. Neither is a
	// GraphElement, which is what makes that cast the filter.
	Vector<Node *> ours;
	for (int i = 0; i < graph->get_child_count(); i++) {
		GraphElement *element = Object::cast_to<GraphElement>(graph->get_child(i));
		if (element != nullptr) {
			ours.push_back(element);
		}
	}
	for (int i = 0; i < ours.size(); i++) {
		graph->remove_child(ours[i]);
		memdelete(ours[i]);
	}
	state_list->clear();
	drawn.clear();
	node_names.clear();
	anystate_machines.clear();
	nests.clear();
	nest_of.clear();
	// The nodes those edges ran between are gone, and an activity left standing would be
	// handed to whichever connection inherits the pair of names.
	pulses.clear();
	// Same reasoning, one layer up: the rings name nodes that have just been deleted. The
	// chain is put back by whoever is feeding this view, in the same frame.
	graph->set_lit_nodes(Vector<StringName>());
	set_process(false);
	merged_root.unref();

	if (hsm.is_null() || hsm->get_effective_root().is_null()) {
		updating = false;
		return;
	}

	// Recomputed here rather than kept: a merged tree is a picture of two files at one
	// moment, and both of them can change between refreshes.
	merged_root = hsm->get_effective_root();
	Ref<HSMStateSpec> root = merged_root;

	int row = 0;
	_populate_graph(root, 0, row);
	_enforce_nest_order();
	_wire_graph(root);

	TreeItem *tree_root = state_list->create_item();
	// The file, not the root state's name: which machine is on screen is what the top row
	// answers, and every one of them has a root called much the same thing. A machine put
	// together in code -- and one rebuilt from what the game reported -- has no file, and
	// there the state's name is the only name there is.
	String file = hsm->get_path().get_file();
	tree_root->set_text(0, file.is_empty() ? String(root->get_state_name()) : file);
	tree_root->set_metadata(0, Variant(int64_t(root->get_instance_id())));
	tree_root->set_icon(0, LimboUtility::get_singleton()->get_task_icon("LimboHSM"));
	tree_root->set_icon_max_width(0, 16 * EDSCALE);
	populate_state_list(state_list, root, tree_root);

	updating = false;

	// After the nodes exist and not before: a nest is measured against them, and a node
	// that has never been laid out has no size to measure.
	fit_pending = true;
	callable_mp(this, &HSMEditorView::_fit_nests).bind(0).call_deferred();
}

// ── Undo ──────────────────────────────────────────────────────────────────────

Ref<HSMStateSpec> HSMEditorView::_snapshot() const {
	return hsm.is_valid() ? _clone_tree(hsm->get_root()) : Ref<HSMStateSpec>();
}

// Called with the edit already made, so the action is committed without running its do side.
// Running it would swap the live tree for a copy of itself, and every Ref the caller is still
// holding -- the state it just added, the row it just wired -- would be pointing at the tree
// that was thrown away.
void HSMEditorView::_commit_edit(const String &p_name, const Ref<HSMStateSpec> &p_before) {
	if (hsm.is_null() || p_before.is_null()) {
		return;
	}
	EditorUndoRedoManager *undo_redo = EditorInterface::get_singleton()->get_editor_undo_redo();
	ERR_FAIL_NULL(undo_redo);
	undo_redo->create_action(p_name, UndoRedo::MERGE_DISABLE, history_context);
	undo_redo->force_fixed_history();
	undo_redo->add_do_method(this, "_restore_root", hsm, _snapshot());
	undo_redo->add_undo_method(this, "_restore_root", hsm, p_before);
	undo_redo->commit_action(false);
}

void HSMEditorView::_restore_root(const Ref<HierarchicalStateMachine> &p_target, const Ref<HSMStateSpec> &p_root) {
	if (p_target.is_null()) {
		return;
	}
	// A copy of the snapshot rather than the snapshot: it stays in the undo history, and the
	// next edit would otherwise be made on the very tree an undo has to hand back.
	p_target->set_root(_clone_tree(p_root));
	if (p_target != hsm) {
		// Another file's turn in the history came round while this view is showing something
		// else. The file is still put right; there is just nothing here to redraw.
		return;
	}
	refresh();
	emit_signal("hsm_edited");
}

void HSMEditorView::edit_hsm(const Ref<HierarchicalStateMachine> &p_hsm) {
	if (hsm != p_hsm) {
		// The preview belongs to a pick made in the machine being left. A refresh does not
		// clear it -- an edit to the machine is not a reason for the tree beside it to blink
		// away -- but another file is.
		_preview_tree(Ref<HSMStateSpec>());
	}
	hsm = p_hsm;
	if (hsm.is_valid() && hsm->get_root().is_null()) {
		// A file created from the FileSystem dock arrives empty. Giving it a root here
		// rather than in the constructor keeps "an empty file" loadable and still leaves
		// the editor with something to draw.
		Ref<HSMStateSpec> root;
		root.instantiate();
		root->set_state_name("HSM");
		hsm->set_root(root);
	}
	refresh();
}

void HSMEditorView::_on_list_item_selected() {
	if (updating) {
		return;
	}
	TreeItem *item = state_list->get_selected();
	if (item == nullptr) {
		return;
	}
	uint64_t id = uint64_t(int64_t(item->get_metadata(0)));
	const StringName *name = node_names.getptr(id);
	if (name == nullptr) {
		return;
	}
	updating = true;
	for (int i = 0; i < graph->get_child_count(); i++) {
		GraphNode *node = Object::cast_to<GraphNode>(graph->get_child(i));
		if (node != nullptr) {
			node->set_selected(node->get_name() == *name);
		}
	}
	updating = false;

	const DrawnState *entry = drawn.getptr(*name);
	if (entry != nullptr) {
		_inspect(entry->spec);
		_preview_tree(entry->spec);
	}
}

// The graph draws structure; everything else about a state -- its name, its script, its
// tree, its transitions and their events -- is shown by the Inspector. Splitting it that
// way is what the task tree already does for tasks.
//
// An inherited state goes nowhere: the Inspector would be editing a state that is thrown
// away on the next refresh, and every keystroke would be silently lost.
void HSMEditorView::_inspect(const Ref<HSMStateSpec> &p_drawn) {
	if (p_drawn.is_null() || read_only) {
		// Read-only means these states stand for nodes in a running game rather than for
		// anything on disk. Handing one to the Inspector would offer fields that write to a
		// copy thrown away on the next refresh.
		return;
	}
	if (p_drawn->is_inherited()) {
		EditorInterface::get_singleton()->inspect_object(nullptr);
		return;
	}
	Ref<HSMStateSpec> authored = _authored_of(p_drawn, false);
	if (authored.is_valid()) {
		EditorInterface::get_singleton()->edit_resource(authored);
	}
}

void HSMEditorView::_on_graph_node_selected(Node *p_node) {
	if (updating || p_node == nullptr) {
		return;
	}
	const DrawnState *entry = drawn.getptr(p_node->get_name());
	if (entry == nullptr) {
		return;
	}
	updating = true;
	TreeItem *item = state_list->get_root();
	while (item != nullptr) {
		if (uint64_t(int64_t(item->get_metadata(0))) == entry->spec->get_instance_id()) {
			item->select(0);
			state_list->scroll_to_item(item);
			break;
		}
		item = item->get_next_in_tree();
	}
	updating = false;

	_inspect(entry->spec);
	_preview_tree(entry->spec);
}

void HSMEditorView::_drill_into(const Ref<HSMStateSpec> &p_state) {
	if (p_state.is_null()) {
		return;
	}
	Ref<BehaviorTree> tree = p_state->get_behavior_tree();
	if (tree.is_null()) {
		// Only a state that holds a tree has somewhere to go. The color already says
		// which ones those are, so a message here would repeat what the graph shows.
		return;
	}
	// The same one line BTSubtree already uses to drill into its subtree: the plugin
	// handles both resource types, so the tree opens as a tab on the same tab bar.
	EditorInterface::get_singleton()->edit_resource(tree);
}

void HSMEditorView::_preview_tree(const Ref<HSMStateSpec> &p_state) {
	// Said out loud whichever half is listening. On the main screen nobody is; in the
	// debugger the pane that answers is the live one standing outside this view, because a
	// tree that is running is not a thing a file can be read for.
	emit_signal("state_picked", p_state);
	if (read_only) {
		return;
	}

	Ref<BehaviorTree> tree = p_state.is_valid() ? p_state->get_behavior_tree() : Ref<BehaviorTree>();

	if (tree.is_null()) {
		// Hidden rather than emptied. An empty pane with a title over it reads as something
		// that failed to load, and the graph has a better use for the room.
		previewed_tree.unref();
		tree_view->unload();
		tree_box->hide();
		return;
	}

	previewed_tree = tree;
	if (tree_view->get_bt() != tree) {
		tree_view->load_bt(tree);
	}
	// The file name, which is what the node on the graph writes under itself too. A built-in
	// tree has no path and there is nothing else to call it by; the full path is the tooltip
	// because the label is clipped to whatever width the pane has been dragged to.
	String path = tree->get_path();
	tree_title->set_text(path.is_empty() ? TTR("Behavior Tree") : path.get_file());
	tree_title->set_tooltip_text(path);
	tree_box->show();
}

// The tree the pane is showing, not the tree the picked state holds. They are the same tree
// while the pane is up -- but the button is under the title, and the title names what is
// drawn below it.
void HSMEditorView::_on_open_tree_pressed() {
	if (previewed_tree.is_null()) {
		return;
	}
	EditorInterface::get_singleton()->edit_resource(previewed_tree);
}

namespace {

enum FileMenu {
	FILE_MENU_ADD_STATE,
	FILE_MENU_ADD_MACHINE,
	FILE_MENU_SHOW_IN_FILESYSTEM,
	FILE_MENU_COPY_PATH,
};

} // namespace

void HSMEditorView::_on_list_item_mouse_selected(const Vector2 &p_pos, int p_button) {
	if (p_button != int(LW_MBTN(RIGHT))) {
		return;
	}
	TreeItem *item = state_list->get_selected();
	if (item == nullptr) {
		return;
	}
	uint64_t id = uint64_t(int64_t(item->get_metadata(0)));
	const StringName *name = node_names.getptr(id);
	if (name != nullptr) {
		_open_node_menu(*name);
		return;
	}
	if (item != state_list->get_root() || hsm.is_null()) {
		return;
	}

	String path = hsm->get_path();
	file_menu->clear();
	file_menu->add_icon_item(LimboUtility::get_singleton()->get_task_icon("HSMStateSpec"), TTR("Add State"), FILE_MENU_ADD_STATE);
	file_menu->set_item_disabled(-1 + file_menu->get_item_count(), read_only);
	file_menu->add_icon_item(LimboUtility::get_singleton()->get_task_icon("HierarchicalStateMachine"), TTR("Add Machine"), FILE_MENU_ADD_MACHINE);
	file_menu->set_item_disabled(-1 + file_menu->get_item_count(), read_only);
	file_menu->add_separator();
	file_menu->add_icon_item(get_theme_icon("Filesystem", "EditorIcons"), TTR("Show in FileSystem"), FILE_MENU_SHOW_IN_FILESYSTEM);
	file_menu->set_item_disabled(-1 + file_menu->get_item_count(), path.is_empty());
	file_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), TTR("Copy Path"), FILE_MENU_COPY_PATH);
	file_menu->set_item_disabled(-1 + file_menu->get_item_count(), path.is_empty());
	file_menu->set_position(get_screen_position() + get_local_mouse_position());
	file_menu->reset_size();
	file_menu->popup();
}

void HSMEditorView::_on_file_menu_pressed(int p_id) {
	if (hsm.is_null()) {
		return;
	}
	String path = hsm->get_path();
	switch (p_id) {
		case FILE_MENU_ADD_STATE: {
			_add_child_state(merged_root, false);
		} break;
		case FILE_MENU_ADD_MACHINE: {
			_add_child_state(merged_root, true);
		} break;
		case FILE_MENU_SHOW_IN_FILESYSTEM: {
			if (!path.is_empty()) {
				EditorInterface::get_singleton()->get_file_system_dock()->navigate_to_path(path.get_slice("::", 0));
			}
		} break;
		case FILE_MENU_COPY_PATH: {
			DisplayServer::get_singleton()->clipboard_set(path);
		} break;
	}
}

void HSMEditorView::_on_list_item_activated() {
	_preview_tree(_selected_spec());
}

void HSMEditorView::_on_graph_node_gui_input(const Ref<InputEvent> &p_event, const StringName &p_name) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed()) {
		return;
	}
	// Swallowed either way, or the node would still be left mid-drag under a menu -- or
	// under the tree's tab, which is about to cover this graph.
	GraphNode *node = Object::cast_to<GraphNode>(graph->get_node_or_null(NodePath(String(p_name))));

	if (mb->get_button_index() == LW_MBTN(RIGHT)) {
		if (node != nullptr) {
			node->accept_event();
		}
		_open_node_menu(p_name);
		return;
	}

	if (!mb->is_double_click() || mb->get_button_index() != LW_MBTN(LEFT)) {
		return;
	}
	const DrawnState *entry = drawn.getptr(p_name);
	if (entry == nullptr || entry->spec->get_behavior_tree().is_null()) {
		return;
	}
	if (node != nullptr) {
		node->accept_event();
	}
	_preview_tree(entry->spec);
}

// ── The node's own menu ───────────────────────────────────────────────────────

Ref<HSMStateSpec> HSMEditorView::_machine_for_node(const StringName &p_name, Ref<HSMStateSpec> &r_state) const {
	const DrawnState *entry = drawn.getptr(p_name);
	if (entry != nullptr) {
		r_state = entry->spec;
		return entry->machine;
	}
	const Ref<HSMStateSpec> *machine = anystate_machines.getptr(p_name);
	r_state = Ref<HSMStateSpec>();
	return machine != nullptr ? *machine : Ref<HSMStateSpec>();
}

void HSMEditorView::_open_node_menu(const StringName &p_name) {
	if (read_only) {
		return;
	}
	Ref<HSMStateSpec> state;
	Ref<HSMStateSpec> machine = _machine_for_node(p_name, state);
	if (machine.is_null()) {
		return;
	}
	menu_node = p_name;

	node_menu->clear();
	node_menu->add_item(TTR("Connect To..."), MENU_CONNECT);

	// One item per edge leaving this node, and the id is where that edge sits in the
	// machine's list. Cutting an edge used to mean dragging it off a port; with the ports
	// gone this is the only way, and unlike a port it can name the event -- so a pair that
	// carries two of them is no longer a line that refuses to be cut.
	transition_menu->clear();
	TypedArray<HSMTransition> rows = machine->get_transitions();
	int outgoing = 0;
	for (int i = 0; i < rows.size(); i++) {
		Ref<HSMTransition> row = rows[i];
		if (row.is_null() || row->get_to().is_null() || row->get_from() != state) {
			continue;
		}
		String text = String::utf8("→ ") + String(row->get_to()->get_state_name());
		if (!String(row->get_event()).is_empty()) {
			text += vformat(" (%s)", row->get_event());
		} else if (row->get_condition().is_valid()) {
			// A polled row has no event to tell it apart, so its condition does.
			text += vformat(" [%s]", row->get_condition()->get_display_name());
		}
		transition_menu->add_item(text, i);
		outgoing += 1;
	}
	int trans_idx = node_menu->get_item_count();
	node_menu->add_submenu_node_item(TTR("Remove Transition"), transition_menu);
	node_menu->set_item_disabled(trans_idx, outgoing == 0);

	node_menu->add_separator();
	// ANYSTATE is not a state: it has no children to add to, nothing to delete, and no
	// tree. The items stay so the menu keeps one shape, greyed rather than absent.
	int add_idx = node_menu->get_item_count();
	node_menu->add_item(TTR("Add Child State"), MENU_ADD_CHILD);
	node_menu->set_item_disabled(add_idx, state.is_null());
	int del_idx = node_menu->get_item_count();
	node_menu->add_item(TTR("Remove State"), MENU_REMOVE);
	node_menu->set_item_disabled(del_idx, state.is_null() || state->is_inherited());
	int tree_idx = node_menu->get_item_count();
	node_menu->add_item(TTR("Open Behavior Tree"), MENU_OPEN_TREE);
	node_menu->set_item_disabled(tree_idx, state.is_null() || state->get_behavior_tree().is_null());

	node_menu->set_position(get_screen_position() + get_local_mouse_position());
	node_menu->reset_size();
	node_menu->popup();
}

void HSMEditorView::_on_node_menu_pressed(int p_id) {
	Ref<HSMStateSpec> state;
	Ref<HSMStateSpec> machine = _machine_for_node(menu_node, state);
	if (machine.is_null()) {
		return;
	}
	switch (p_id) {
		case MENU_CONNECT: {
			_begin_connect_from(menu_node);
		} break;
		case MENU_ADD_CHILD: {
			_add_child_state(state);
		} break;
		case MENU_REMOVE: {
			Ref<HSMStateSpec> before = _snapshot();
			if (_remove_drawn(state)) {
				_commit_edit(TTR("Remove State"), before);
				refresh();
				emit_signal("hsm_edited");
			}
		} break;
		case MENU_OPEN_TREE: {
			_drill_into(state);
		} break;
	}
}

void HSMEditorView::_on_transition_menu_pressed(int p_id) {
	Ref<HSMStateSpec> state;
	Ref<HSMStateSpec> machine = _machine_for_node(menu_node, state);
	if (machine.is_null()) {
		return;
	}
	TypedArray<HSMTransition> rows = machine->get_transitions();
	if (p_id < 0 || p_id >= rows.size()) {
		return;
	}
	Ref<HSMTransition> row = rows[p_id];
	Ref<HSMStateSpec> before = _snapshot();
	if (_remove_transition(machine, row)) {
		_commit_edit(TTR("Remove Transition"), before);
		refresh();
		emit_signal("hsm_edited");
	}
}

// ── The edge's own menu ───────────────────────────────────────────────────────

Vector<int> HSMEditorView::_rows_between(const Ref<HSMStateSpec> &p_machine, const Ref<HSMStateSpec> &p_a, const Ref<HSMStateSpec> &p_b) const {
	Vector<int> found;
	if (p_machine.is_null()) {
		return found;
	}
	TypedArray<HSMTransition> rows = p_machine->get_transitions();
	for (int i = 0; i < rows.size(); i++) {
		Ref<HSMTransition> row = rows[i];
		if (row.is_null() || row->get_to().is_null()) {
			continue;
		}
		// Either way round: a pair that goes both ways is drawn as one line, so both of its
		// rows are what was clicked. A null end is ANYSTATE, and comparing against a null
		// `p_a` is how that is asked for -- there is no node to compare with.
		bool forward = row->get_from() == p_a && row->get_to() == p_b;
		bool backward = row->get_from() == p_b && row->get_to() == p_a;
		if (forward || backward) {
			found.push_back(i);
		}
	}
	return found;
}

// One line, one Inspector -- but only when the line stands for exactly one row. With two of
// them there is no answer to "which one", and showing either would be showing the wrong one
// half the time.
void HSMEditorView::_on_connection_picked(const StringName &p_from, const StringName &p_to) {
	Ref<HSMStateSpec> from_state;
	Ref<HSMStateSpec> to_state;
	Ref<HSMStateSpec> machine = _machine_for_node(p_from, from_state);
	if (machine.is_null() || machine != _machine_for_node(p_to, to_state)) {
		return;
	}
	Vector<int> found = _rows_between(machine, from_state, to_state);
	if (found.size() != 1) {
		return;
	}
	TypedArray<HSMTransition> rows = machine->get_transitions();
	Ref<HSMTransition> authored = _authored_transition(machine, rows[found[0]]);
	if (authored.is_valid()) {
		EditorInterface::get_singleton()->edit_resource(authored);
	} else {
		// Inherited: the row on screen is a copy the merge made, and editing that would be
		// editing something the next refresh throws away.
		EditorInterface::get_singleton()->inspect_object(nullptr);
	}
}

void HSMEditorView::_on_connection_menu_requested(const StringName &p_from, const StringName &p_to) {
	if (read_only) {
		return;
	}
	Ref<HSMStateSpec> from_state;
	Ref<HSMStateSpec> to_state;
	Ref<HSMStateSpec> machine = _machine_for_node(p_from, from_state);
	if (machine.is_null() || machine != _machine_for_node(p_to, to_state)) {
		return;
	}
	Vector<int> found = _rows_between(machine, from_state, to_state);
	if (found.is_empty()) {
		return;
	}
	line_machine = machine;

	line_menu->clear();
	TypedArray<HSMTransition> rows = machine->get_transitions();
	for (int i = 0; i < found.size(); i++) {
		Ref<HSMTransition> row = rows[found[i]];
		String leaves = row->get_from().is_valid() ? String(row->get_from()->get_state_name()) : String::utf8("✱");
		String text = vformat(String::utf8("%s  %s → %s"), TTR("Remove"), leaves, String(row->get_to()->get_state_name()));
		if (!String(row->get_event()).is_empty()) {
			// Which of the rows this one is. Between the same two states that is the only
			// thing telling them apart, which is why the pair alone could not be the menu.
			text += vformat(" (%s)", row->get_event());
		} else if (row->get_condition().is_valid()) {
			// A polled row: its condition is what tells it from another polled row.
			text += vformat(" [%s]", row->get_condition()->get_display_name());
		}
		line_menu->add_item(text, found[i]);
	}

	line_menu->set_position(get_screen_position() + get_local_mouse_position());
	line_menu->reset_size();
	line_menu->popup();
}

void HSMEditorView::_on_line_menu_pressed(int p_id) {
	if (line_machine.is_null()) {
		return;
	}
	TypedArray<HSMTransition> rows = line_machine->get_transitions();
	if (p_id < 0 || p_id >= rows.size()) {
		return;
	}
	Ref<HSMStateSpec> before = _snapshot();
	if (_remove_transition(line_machine, rows[p_id])) {
		_commit_edit(TTR("Remove Transition"), before);
		refresh();
		emit_signal("hsm_edited");
	}
}

// Looked for in this file only. A row that is drawn but not here came from the base, and
// nothing done to it here could stay in this file.
Ref<HSMTransition> HSMEditorView::_authored_transition(const Ref<HSMStateSpec> &p_machine, const Ref<HSMTransition> &p_row) {
	if (p_row.is_null() || p_row->get_to().is_null()) {
		return nullptr;
	}
	Ref<HSMStateSpec> owner = _authored_of(p_machine, false);
	Ref<HSMStateSpec> to_authored = _authored_of(p_row->get_to(), false);
	Ref<HSMStateSpec> from_authored;
	if (p_row->get_from().is_valid()) {
		from_authored = _authored_of(p_row->get_from(), false);
		if (from_authored.is_null()) {
			return nullptr;
		}
	}
	if (owner.is_null() || to_authored.is_null()) {
		return nullptr;
	}

	TypedArray<HSMTransition> rows = owner->get_transitions();
	// Without a base the drawn row is the authored row itself, and that is the only answer
	// that cannot be fooled by two rows that look alike.
	for (int i = 0; i < rows.size(); i++) {
		if (Ref<HSMTransition>(rows[i]) == p_row) {
			return p_row;
		}
	}
	// Matched on the event, the condition and the priority too, not just the pair: which of
	// two edges between the same states this is, is the one thing a pair of endpoints never
	// says. Polled rows all have the same (empty) event, so the condition is what tells them
	// apart -- the merge hands the authored condition on by reference, so it compares as is.
	for (int i = 0; i < rows.size(); i++) {
		Ref<HSMTransition> row = rows[i];
		if (row.is_null()) {
			continue;
		}
		if (row->get_to() == to_authored && row->get_from() == from_authored && row->get_event() == p_row->get_event() &&
				row->get_condition() == p_row->get_condition() && row->get_priority() == p_row->get_priority()) {
			return row;
		}
	}
	return nullptr;
}

Ref<HSMStateSpec> HSMEditorView::_drawn_machine_by_id(uint64_t p_id) const {
	if (merged_root.is_valid() && merged_root->get_instance_id() == p_id) {
		return merged_root;
	}
	const StringName *name = node_names.getptr(p_id);
	if (name == nullptr) {
		return nullptr;
	}
	const DrawnState *entry = drawn.getptr(*name);
	return entry != nullptr ? entry->spec : Ref<HSMStateSpec>();
}

namespace {

// One line of the state inspector. Parts that say nothing are left out rather than shown
// blank, so a plain event row stays as short as it always was.
String _transition_line(const String &p_head, const Ref<HSMTransition> &p_row, bool p_with_note) {
	PackedStringArray parts;
	parts.push_back(p_head);
	if (p_row->get_condition().is_valid()) {
		parts.push_back(p_row->get_condition()->get_display_name());
	}
	if (!String(p_row->get_event()).is_empty()) {
		parts.push_back(String(p_row->get_event()));
	}
	if (p_with_note && !p_row->get_note().strip_edges().is_empty()) {
		// The first line only: a note is free text, and a paragraph would make one row of the
		// list taller than the rest of it.
		parts.push_back(p_row->get_note().strip_edges().get_slice("\n", 0));
	}
	return String("  |  ").join(parts);
}

String _state_label(const Ref<HSMStateSpec> &p_state) {
	return p_state.is_valid() ? String(p_state->get_state_name()) : String("Any");
}

} // namespace

Dictionary HSMEditorView::describe_state_transitions(const Ref<HSMStateSpec> &p_authored) const {
	Dictionary out;
	if (p_authored.is_null() || hsm.is_null()) {
		return out;
	}
	// The drawn state standing for it: the state itself without a base, or the merged copy
	// whose origin it is.
	const DrawnState *entry = nullptr;
	for (const KeyValue<StringName, DrawnState> &E : drawn) {
		if (E.value.spec == p_authored || E.value.spec->get_origin() == p_authored) {
			entry = &E.value;
			break;
		}
	}
	if (entry == nullptr || entry->machine.is_null()) {
		return out;
	}
	const Ref<HSMStateSpec> &state = entry->spec;
	const Ref<HSMStateSpec> &machine = entry->machine;
	int64_t machine_id = int64_t(machine->get_instance_id());
	TypedArray<HSMTransition> rows = machine->get_transitions();

	// Outgoing, in evaluation order: the same stable sort LimboHSM does when it orders the
	// polled rows of one state, so the list reads top-down the way the machine asks.
	Vector<int> order;
	for (int i = 0; i < rows.size(); i++) {
		Ref<HSMTransition> row = rows[i];
		if (row.is_null() || row->get_to().is_null()) {
			continue;
		}
		if (row->get_from().is_valid() && row->get_from() != state) {
			continue;
		}
		bool any = row->get_from().is_null();
		int pos = order.size();
		while (pos > 0) {
			Ref<HSMTransition> prev = rows[order[pos - 1]];
			bool prev_any = prev->get_from().is_null();
			bool goes_before = row->get_priority() > prev->get_priority() || (row->get_priority() == prev->get_priority() && !any && prev_any);
			if (!goes_before) {
				break;
			}
			pos -= 1;
		}
		order.insert(pos, i);
	}
	Array outgoing;
	for (int i = 0; i < order.size(); i++) {
		Ref<HSMTransition> row = rows[order[i]];
		String head = vformat("[%d] -> %s", row->get_priority(), _state_label(row->get_to()));
		if (row->get_from().is_null()) {
			// Said, because the row is not one of this state's own: it is the machine's, and
			// it applies here only because it applies everywhere.
			head += " (Any)";
		}
		Dictionary line;
		line["text"] = _transition_line(head, row, true);
		line["machine"] = machine_id;
		line["row"] = order[i];
		line["inherited"] = const_cast<HSMEditorView *>(this)->_authored_transition(machine, row).is_null();
		outgoing.push_back(line);
	}

	Array incoming;
	for (int i = 0; i < rows.size(); i++) {
		Ref<HSMTransition> row = rows[i];
		if (row.is_null() || row->get_to() != state) {
			continue;
		}
		String head = vformat("[%d] %s ->", row->get_priority(), _state_label(row->get_from()));
		Dictionary line;
		line["text"] = _transition_line(head, row, false);
		line["machine"] = machine_id;
		line["row"] = i;
		line["inherited"] = const_cast<HSMEditorView *>(this)->_authored_transition(machine, row).is_null();
		incoming.push_back(line);
	}

	out["outgoing"] = outgoing;
	out["incoming"] = incoming;
	return out;
}

void HSMEditorView::select_transition(int64_t p_machine_id, int p_row) {
	Ref<HSMStateSpec> machine = _drawn_machine_by_id(uint64_t(p_machine_id));
	if (machine.is_null()) {
		return;
	}
	TypedArray<HSMTransition> rows = machine->get_transitions();
	if (p_row < 0 || p_row >= rows.size()) {
		return;
	}
	Ref<HSMTransition> row = rows[p_row];
	if (row.is_null() || row->get_to().is_null()) {
		return;
	}
	const StringName *to = node_names.getptr(row->get_to()->get_instance_id());
	if (to == nullptr) {
		return;
	}
	StringName from;
	if (row->get_from().is_valid()) {
		const StringName *found = node_names.getptr(row->get_from()->get_instance_id());
		if (found == nullptr) {
			return;
		}
		from = *found;
	} else {
		from = _anystate_node_name_for(machine);
	}

	// The node selection goes, because the Inspector is about to stop showing that node: a
	// box still marked as picked would claim the Inspector is about it.
	updating = true;
	for (int i = 0; i < graph->get_child_count(); i++) {
		GraphNode *node = Object::cast_to<GraphNode>(graph->get_child(i));
		if (node != nullptr) {
			node->set_selected(false);
		}
	}
	updating = false;
	graph->pick_connection(from, *to);

	if (read_only) {
		return;
	}
	Ref<HSMTransition> authored = _authored_transition(machine, row);
	if (authored.is_valid()) {
		EditorInterface::get_singleton()->edit_resource(authored);
	} else {
		// Inherited, the same as a click on its line: there is nothing in this file to edit.
		EditorInterface::get_singleton()->inspect_object(nullptr);
	}
}

// Cutting a row that came from the base would cut it for every machine that inherits it.
bool HSMEditorView::_remove_transition(const Ref<HSMStateSpec> &p_machine, const Ref<HSMTransition> &p_row) {
	Ref<HSMTransition> authored = _authored_transition(p_machine, p_row);
	Ref<HSMStateSpec> owner = _authored_of(p_machine, false);
	if (authored.is_null() || owner.is_null()) {
		WARN_PRINT("LimboAI: This transition is inherited; remove it in the base machine.");
		return false;
	}
	TypedArray<HSMTransition> rows = owner->get_transitions();
	for (int i = 0; i < rows.size(); i++) {
		if (Ref<HSMTransition>(rows[i]) == authored) {
			rows.remove_at(i);
			owner->set_transitions(rows);
			return true;
		}
	}
	return false;
}

// The gesture the ports used to carry, moved onto the nodes: the line follows the cursor
// and lands on a whole node rather than on a dot. Which nodes it may land on is worked out
// here and handed over -- the graph draws the answer, it does not know it. Siblings, and
// only siblings: a machine owns the transitions between its own children, and LimboHSM
// refuses anything else at wiring time.
void HSMEditorView::_begin_connect_from(const StringName &p_name) {
	if (read_only) {
		return;
	}
	Ref<HSMStateSpec> state;
	Ref<HSMStateSpec> machine = _machine_for_node(p_name, state);
	if (machine.is_null()) {
		return;
	}
	Vector<StringName> targets;
	for (const KeyValue<StringName, DrawnState> &entry : drawn) {
		if (entry.value.machine != machine || entry.key == p_name) {
			continue;
		}
		targets.push_back(entry.key);
	}
	graph->begin_connect_from(p_name, targets);
}

void HSMEditorView::_on_graph_end_node_move() {
	if (updating || hsm.is_null()) {
		return;
	}
	if (read_only) {
		// Nothing here moves, so nothing here is saved: the nodes are not draggable at all
		// when the machine belongs to a running game. The signal still arrives from a press
		// that merely picked one, and a nest is refitted for the same reason it is anywhere
		// else -- it is measured against what it holds, and that is cheap to be sure of.
		_fit_nests();
		return;
	}
	// Taken before a single position is written, and thrown away unread when nothing moved --
	// `end_node_move` also arrives from a press that merely selected a node.
	Ref<HSMStateSpec> before = _snapshot();
	bool moved = false;
	for (int i = 0; i < graph->get_child_count(); i++) {
		GraphNode *node = Object::cast_to<GraphNode>(graph->get_child(i));
		if (node == nullptr) {
			continue;
		}
		DrawnState *entry = drawn.getptr(node->get_name());
		if (entry == nullptr) {
			// Not a state. The one other thing that is drawn and can be dragged is a ✱, and
			// where it was left is the machine's to keep -- a drag that was quietly forgotten
			// is worse here than a ✱ standing in the corner, because it looks like it took.
			Ref<HSMStateSpec> *machine = anystate_machines.getptr(node->get_name());
			if (machine == nullptr || machine->is_null()) {
				continue;
			}
			Vector2 any_pos = node->get_position_offset().round();
			if ((*machine)->get_anystate_graph_position() == any_pos) {
				continue;
			}
			Ref<HSMStateSpec> any_authored = _authored_of(*machine, true);
			if (any_authored.is_null()) {
				continue;
			}
			(*machine)->set_anystate_graph_position(any_pos);
			any_authored->set_anystate_graph_position(any_pos);
			moved = true;
			continue;
		}
		if (nest_of.has(node->get_name())) {
			// Where a nest stands is worked out from what it holds, so it has no position
			// of its own to save. Dragging one moved those, and they are saved right here.
			continue;
		}
		// Snapped to whole pixels: writing floats straight through puts a diff in the
		// file every time a node is merely brushed.
		Vector2 pos = node->get_position_offset().round();
		if (entry->spec->get_graph_position() == pos) {
			continue;
		}
		// Where an inherited node sits is this file's own business, so laying one out is
		// an edit like any other and gives it a stub. Compared against the drawn value
		// first, or merely opening the tab would fill the file with stubs.
		Ref<HSMStateSpec> authored = _authored_of(entry->spec, true);
		if (authored.is_null()) {
			continue;
		}
		entry->spec->set_graph_position(pos);
		authored->set_graph_position(pos);
		moved = true;
	}
	// Whatever moved, a nest around it is now the wrong size or in the wrong place -- and
	// that holds for a state dragged out of one as much as for one dragged in.
	_fit_nests();
	if (moved) {
		_commit_edit(TTR("Move States"), before);
		emit_signal("hsm_edited");
	}
}

Ref<HSMStateSpec> HSMEditorView::_selected_spec() const {
	TreeItem *item = state_list->get_selected();
	if (item == nullptr) {
		return nullptr;
	}
	uint64_t id = uint64_t(int64_t(item->get_metadata(0)));
	if (merged_root.is_valid() && merged_root->get_instance_id() == id) {
		return merged_root;
	}
	const StringName *name = node_names.getptr(id);
	if (name == nullptr) {
		return nullptr;
	}
	const DrawnState *entry = drawn.getptr(*name);
	return entry != nullptr ? entry->spec : Ref<HSMStateSpec>();
}

Ref<HSMStateSpec> HSMEditorView::_machine_of(const Ref<HSMStateSpec> &p_state) const {
	if (p_state.is_null()) {
		return nullptr;
	}
	const StringName *name = node_names.getptr(p_state->get_instance_id());
	if (name == nullptr) {
		return nullptr;
	}
	const DrawnState *entry = drawn.getptr(*name);
	return entry != nullptr ? entry->machine : Ref<HSMStateSpec>();
}

Ref<HSMStateSpec> HSMEditorView::_authored_of(const Ref<HSMStateSpec> &p_drawn, bool p_create) {
	if (p_drawn.is_null() || hsm.is_null()) {
		return nullptr;
	}
	if (p_drawn->get_origin().is_valid()) {
		return p_drawn->get_origin();
	}
	if (!p_drawn->is_inherited()) {
		// Nothing was merged, so the drawn state is the authored one.
		return p_drawn;
	}
	if (!p_create) {
		return nullptr;
	}

	// An edit that reaches an inherited state gives this file a state of its own that
	// names it. That empty state is how a child file points at something it does not own:
	// merging matches by name, so it lands back on the very state that is drawn here.
	Ref<HSMStateSpec> parent_drawn = _machine_of(p_drawn);
	if (parent_drawn.is_null()) {
		// The root always has an authored counterpart -- edit_hsm() sees to that.
		return hsm->get_root();
	}
	Ref<HSMStateSpec> parent = _authored_of(parent_drawn, true);
	ERR_FAIL_COND_V(parent.is_null(), nullptr);

	Ref<HSMStateSpec> stub;
	stub.instantiate();
	stub->set_state_name(p_drawn->get_state_name());
	TypedArray<HSMStateSpec> children = parent->get_children();
	children.append(stub);
	parent->set_children(children);

	// So that the rest of this edit -- and anything else before the next refresh -- finds
	// it instead of making a second one.
	p_drawn->set_origin(stub);
	return stub;
}

StringName HSMEditorView::_unique_child_name(const Ref<HSMStateSpec> &p_machine) const {
	ERR_FAIL_COND_V(p_machine.is_null(), StringName("State"));
	TypedArray<HSMStateSpec> children = p_machine->get_children();
	for (int n = 1; n < 1000; n++) {
		String candidate = n == 1 ? String("State") : vformat("State%d", n);
		bool taken = false;
		for (int i = 0; i < children.size(); i++) {
			Ref<HSMStateSpec> spec = children[i];
			if (spec.is_valid() && String(spec->get_state_name()) == candidate) {
				taken = true;
				break;
			}
		}
		if (!taken) {
			return StringName(candidate);
		}
	}
	return StringName("State");
}

void HSMEditorView::_detach_state(const Ref<HSMStateSpec> &p_machine, const Ref<HSMStateSpec> &p_state) {
	ERR_FAIL_COND(p_machine.is_null() || p_state.is_null());

	TypedArray<HSMStateSpec> children = p_machine->get_children();
	for (int i = children.size() - 1; i >= 0; i--) {
		Ref<HSMStateSpec> spec = children[i];
		if (spec == p_state) {
			children.remove_at(i);
		}
	}
	p_machine->set_children(children);

	if (p_machine->get_initial_child() == p_state) {
		// Cleared rather than moved to a sibling: get_initial_child() already falls back
		// to the first child, so guessing here would put the same answer in two places.
		p_machine->set_initial_child(nullptr);
	}

	// Every edge that mentions it goes too -- a row pointing at nothing is not an error
	// the file can express, and LimboHSM would refuse to wire it.
	TypedArray<HSMTransition> rows = p_machine->get_transitions();
	for (int i = rows.size() - 1; i >= 0; i--) {
		Ref<HSMTransition> row = rows[i];
		if (row.is_null() || row->get_to() == p_state || row->get_from() == p_state) {
			rows.remove_at(i);
		}
	}
	p_machine->set_transitions(rows);
}

void HSMEditorView::add_state() {
	if (read_only) {
		return;
	}
	Ref<HSMStateSpec> drawn_parent = _selected_spec();
	if (drawn_parent.is_null()) {
		drawn_parent = merged_root;
	}
	_add_child_state(drawn_parent, false);
}

void HSMEditorView::add_machine() {
	if (read_only) {
		return;
	}
	Ref<HSMStateSpec> drawn_parent = _selected_spec();
	if (drawn_parent.is_null()) {
		drawn_parent = merged_root;
	}
	_add_child_state(drawn_parent, true);
}

void HSMEditorView::_add_child_state(const Ref<HSMStateSpec> &p_drawn_parent, bool p_as_machine) {
	if (hsm.is_null()) {
		return;
	}
	Ref<HSMStateSpec> drawn_parent = p_drawn_parent;
	ERR_FAIL_COND(drawn_parent.is_null());
	Ref<HSMStateSpec> before = _snapshot();
	// Adding *to* an inherited machine is allowed -- adding is the only thing a child file
	// can do, and it is where the stub for that machine comes from.
	Ref<HSMStateSpec> parent = _authored_of(drawn_parent, true);
	ERR_FAIL_COND(parent.is_null());

	Ref<HSMStateSpec> spec;
	spec.instantiate();
	// Named against what is drawn, not against what this file holds: a stub's children
	// are only half of the machine, and the other half is where a clash would come from.
	spec->set_state_name(_unique_child_name(drawn_parent));

	if (p_as_machine) {
		// A state becomes a machine by gaining a child, so "Add Machine" is "Add State" plus
		// the first one. It is a separate button rather than a separate thing: an empty
		// machine draws as an ordinary state, and a box that only becomes a box on the
		// second click is a box nobody finds.
		Ref<HSMStateSpec> child;
		child.instantiate();
		child->set_state_name("State");
		TypedArray<HSMStateSpec> inner;
		inner.append(child);
		spec->set_children(inner);
		// `initial_child` is left alone on purpose: get_initial_child() already falls back to
		// the first child, and writing the fallback into the file makes a declaration out of
		// something nobody declared -- which is exactly what a machine inheriting this one
		// would then be unable to say nothing about.
	}

	TypedArray<HSMStateSpec> children = parent->get_children();
	children.append(spec);
	parent->set_children(children);

	_commit_edit(p_as_machine ? TTR("Add Machine") : TTR("Add State"), before);
	refresh();
	emit_signal("hsm_edited");
}

void HSMEditorView::remove_selected() {
	if (hsm.is_null() || read_only) {
		return;
	}
	Ref<HSMStateSpec> spec = _selected_spec();
	if (spec.is_null() || spec == merged_root) {
		// The root is the machine itself; deleting it would leave the file with nothing.
		return;
	}
	Ref<HSMStateSpec> before = _snapshot();
	if (!_remove_drawn(spec)) {
		return;
	}

	_commit_edit(TTR("Remove State"), before);
	refresh();
	emit_signal("hsm_edited");
}

// Takes a drawn state out of the file that owns it, or refuses. Inherited states are not
// removable here: what would change is the base file, and every machine that inherits it
// would change with it -- on screen that reads as "I edited the boss".
bool HSMEditorView::_remove_drawn(const Ref<HSMStateSpec> &p_drawn) {
	if (p_drawn.is_null()) {
		return false;
	}
	if (p_drawn->is_inherited()) {
		WARN_PRINT(vformat("LimboAI: \"%s\" is inherited; remove it in the base machine.", p_drawn->get_state_name()));
		return false;
	}
	Ref<HSMStateSpec> machine = _authored_of(_machine_of(p_drawn), true);
	Ref<HSMStateSpec> spec = _authored_of(p_drawn, false);
	if (machine.is_null() || spec.is_null()) {
		return false;
	}
	_detach_state(machine, spec);
	return true;
}

// One row in one machine, whatever gesture asked for it. GraphEdit's own
// `connection_request` cannot reach here any more -- it only fires when a port drag lands,
// and there are no ports to drag -- so the ports it used to carry are gone from the
// signature too.
void HSMEditorView::_on_connect_to_requested(const StringName &p_from, const StringName &p_to) {
	if (hsm.is_null() || read_only) {
		return;
	}
	const DrawnState *to_entry = drawn.getptr(p_to);
	if (to_entry == nullptr) {
		// ANYSTATE is never a destination: it says "no state is being left".
		return;
	}

	Ref<HSMStateSpec> machine;
	Ref<HSMStateSpec> from_spec;
	const DrawnState *from_entry = drawn.getptr(p_from);
	if (from_entry != nullptr) {
		from_spec = from_entry->spec;
		machine = from_entry->machine;
	} else if (p_from == _anystate_node_name_for(to_entry->machine)) {
		machine = to_entry->machine;
	} else {
		return;
	}

	// Both ends must be siblings: a machine only owns transitions between its own
	// children, and LimboHSM refuses anything else at wiring time.
	if (machine != to_entry->machine) {
		return;
	}

	// Before the stubs, not after: reaching for an inherited end gives this file a state of
	// its own, and that is part of the edit an undo has to take back.
	Ref<HSMStateSpec> before = _snapshot();

	// The row goes in this file even when both ends are inherited -- that is what drawing
	// an edge on a child machine means. Each end it reaches for gets a stub named after it.
	Ref<HSMStateSpec> owner = _authored_of(machine, true);
	Ref<HSMStateSpec> to_authored = _authored_of(to_entry->spec, true);
	if (owner.is_null() || to_authored.is_null()) {
		return;
	}
	Ref<HSMStateSpec> from_authored;
	if (from_spec.is_valid()) {
		from_authored = _authored_of(from_spec, true);
		if (from_authored.is_null()) {
			return;
		}
	}

	Ref<HSMTransition> row;
	row.instantiate();
	row->set_from(from_authored);
	row->set_to(to_authored);
	// The event is left empty on purpose -- it is the one thing the graph cannot guess,
	// and the inspector shows it as the blank it is. Note that a row with no event and no
	// condition is a polled fallback that always passes, so a fresh edge is live until it
	// is given an event or a condition.

	TypedArray<HSMTransition> rows = owner->get_transitions();
	rows.append(row);
	owner->set_transitions(rows);

	_commit_edit(TTR("Add Transition"), before);
	refresh();
	emit_signal("hsm_edited");
}

void HSMEditorView::_on_delete_nodes_request(const TypedArray<StringName> &p_nodes) {
	if (hsm.is_null() || read_only) {
		return;
	}
	Ref<HSMStateSpec> before = _snapshot();
	bool removed = false;
	for (int i = 0; i < p_nodes.size(); i++) {
		StringName name = p_nodes[i];
		const DrawnState *entry = drawn.getptr(name);
		if (entry == nullptr) {
			continue;
		}
		removed = _remove_drawn(entry->spec) || removed;
	}
	if (removed) {
		// One action for the lot. Undoing a delete of four states as four presses would be
		// undoing something nobody did.
		_commit_edit(TTR("Remove States"), before);
		refresh();
		emit_signal("hsm_edited");
	}
}

// ── Watching one that runs ─────────────────────────────────────────

void HSMEditorView::set_read_only(bool p_read_only) {
	read_only = p_read_only;
	if (read_only) {
		// The tree a state holds is shown by the live view standing outside this one, so the
		// pane here would be the same tree twice -- once as it is on disk and once as it is
		// running, side by side, differing whenever the file has been touched since launch.
		tree_box->hide();
	}
}

void HSMEditorView::set_active_chain(const Vector<Ref<HSMStateSpec>> &p_chain) {
	Vector<StringName> lit;
	Vector<uint64_t> lit_ids;
	for (int i = 0; i < p_chain.size(); i++) {
		if (p_chain[i].is_null()) {
			continue;
		}
		const StringName *name = node_names.getptr(p_chain[i]->get_instance_id());
		if (name == nullptr) {
			continue;
		}
		lit.push_back(*name);
		lit_ids.push_back(p_chain[i]->get_instance_id());
	}

	for (int i = 0; i < graph->get_child_count(); i++) {
		GraphNode *node = Object::cast_to<GraphNode>(graph->get_child(i));
		if (node == nullptr) {
			continue;
		}
		StringName name = node->get_name();
		// With nothing lit, nothing is dimmed either. An empty chain is a machine that is not
		// running, and dimming the whole picture for that would read as a fault.
		bool on = lit.is_empty() || lit.has(name);
		node->set_self_modulate(Color(1, 1, 1, on ? 1.0 : INACTIVE_ALPHA));
	}

	// Every one of them ringed, the machines included. The mark used to be a `●` after the
	// innermost one's name, on the grounds that the chain is a line of boxes inside boxes and
	// they cannot all be *the* state -- but a dot beside a word is read at the word's scale,
	// and the thing it was marking is a box the size of a quarter of the graph. What answers
	// "which one is it standing in" is the innermost ring, which is the only one with no ring
	// inside it; what the outer ones answer is which rooms that is inside of, and both
	// questions are asked by a reader looking at the picture rather than at a caption.
	graph->set_lit_nodes(lit);
	_mark_list(lit_ids);
}

void HSMEditorView::_mark_list(const Vector<uint64_t> &p_ids) {
	// The theme's own selection color, so a marked row reads as a picked one -- which is what
	// it is being asked to look like. It is not taken from the graph's ring: over there the
	// ring had to be told apart from the node's selected border, and here there is no second
	// mark to be told apart from.
	Color color(1, 1, 1, 0.12);
	if (state_list->has_theme_stylebox(StringName("selected"), "Tree")) {
		Ref<StyleBoxFlat> box = state_list->get_theme_stylebox(StringName("selected"), "Tree");
		if (box.is_valid()) {
			color = box->get_bg_color();
		}
	}

	TreeItem *item = state_list->get_root();
	while (item != nullptr) {
		if (p_ids.has(uint64_t(int64_t(item->get_metadata(0))))) {
			item->set_custom_bg_color(0, color);
		} else {
			// Cleared and not skipped: the machine leaves states as often as it enters them,
			// and a mark that is only ever put on would leave the whole path it has walked lit.
			item->clear_custom_bg_color(0);
		}
		item = item->get_next_in_tree();
	}
}

void HSMEditorView::pulse_transition(const Ref<HSMStateSpec> &p_from, const Ref<HSMStateSpec> &p_to) {
	if (p_from.is_null() || p_to.is_null()) {
		// An entry into a machine comes from no state at all. There is no line for it, which
		// is the same reason the graph does not draw one.
		return;
	}
	const StringName *from = node_names.getptr(p_from->get_instance_id());
	const StringName *to = node_names.getptr(p_to->get_instance_id());
	if (from == nullptr || to == nullptr || *from == *to) {
		return;
	}

	for (int i = 0; i < pulses.size(); i++) {
		if (pulses[i].from == *from && pulses[i].to == *to) {
			pulses.write[i].amount = 1.0;
			set_process(true);
			return;
		}
	}
	Pulse pulse;
	pulse.from = *from;
	pulse.to = *to;
	pulse.amount = 1.0;
	pulses.push_back(pulse);
	set_process(true);
}

// The fade. GraphEdit is told an amount per connection and paints the line with it, so the
// only thing kept here is how much is left -- and the process is switched off again the
// moment nothing is glowing, because a debugger tab left open is a tab left open for hours.
void HSMEditorView::_fade_pulses(double p_delta) {
	for (int i = pulses.size() - 1; i >= 0; i--) {
		float amount = pulses[i].amount - float(p_delta) / PULSE_SECONDS;
		if (amount <= 0.0) {
			graph->set_connection_activity(pulses[i].from, 0, pulses[i].to, 0, 0.0);
			pulses.remove_at(i);
			continue;
		}
		pulses.write[i].amount = amount;
		graph->set_connection_activity(pulses[i].from, 0, pulses[i].to, 0, amount);
	}
	if (pulses.is_empty()) {
		set_process(false);
	}
}

void HSMEditorView::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PROCESS: {
			_fade_pulses(get_process_delta_time());
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			// The fit that was set aside while nobody was looking. Deferred rather than done
			// here: being shown is what starts the layout, not what finishes it -- the nodes
			// have their minimum sizes applied a frame later, and this notification reaches
			// the view before it reaches them.
			if (fit_pending && is_visible_in_tree()) {
				callable_mp(this, &HSMEditorView::_fit_nests).bind(0).call_deferred();
			}
		} break;
		case NOTIFICATION_READY: {
			state_list->connect("item_selected", callable_mp(this, &HSMEditorView::_on_list_item_selected));
			state_list->connect("item_activated", callable_mp(this, &HSMEditorView::_on_list_item_activated));
			graph->connect("node_selected", callable_mp(this, &HSMEditorView::_on_graph_node_selected));
			graph->connect("end_node_move", callable_mp(this, &HSMEditorView::_on_graph_end_node_move));
			graph->connect("connect_to_requested", callable_mp(this, &HSMEditorView::_on_connect_to_requested));
			graph->connect("delete_nodes_request", callable_mp(this, &HSMEditorView::_on_delete_nodes_request));
			graph->connect("connection_picked", callable_mp(this, &HSMEditorView::_on_connection_picked));
			graph->connect("connection_menu_requested", callable_mp(this, &HSMEditorView::_on_connection_menu_requested));
			node_menu->connect("id_pressed", callable_mp(this, &HSMEditorView::_on_node_menu_pressed));
			transition_menu->connect("id_pressed", callable_mp(this, &HSMEditorView::_on_transition_menu_pressed));
			line_menu->connect("id_pressed", callable_mp(this, &HSMEditorView::_on_line_menu_pressed));
			tree_open_btn->connect("pressed", callable_mp(this, &HSMEditorView::_on_open_tree_pressed));
			state_list->connect("item_mouse_selected", callable_mp(this, &HSMEditorView::_on_list_item_mouse_selected));
			file_menu->connect("id_pressed", callable_mp(this, &HSMEditorView::_on_file_menu_pressed));
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			tree_open_btn->set_button_icon(get_theme_icon(LW_NAME(ExternalLink), LW_NAME(EditorIcons)));
			// The darker body the task tree has, so a machine's list and a tree read alike.
			ADD_STYLEBOX_OVERRIDE(state_list, LW_NAME(panel), TaskTree::make_dark_tree_panel(this));
		} break;
	}
}

void HSMEditorView::_bind_methods() {
	// Bound because the undo history calls it by name -- it is the one method here that is
	// invoked from outside this class.
	ClassDB::bind_method(D_METHOD("_restore_root", "target", "root"), &HSMEditorView::_restore_root);
	// Reached by the HSMStateSpec inspector plugin through a deferred call.
	ClassDB::bind_method(D_METHOD("select_transition", "machine_id", "row"), &HSMEditorView::select_transition);
	ClassDB::bind_method(D_METHOD("describe_state_transitions", "state"), &HSMEditorView::describe_state_transitions);

	ADD_SIGNAL(MethodInfo("hsm_edited"));
	// Which state the picture is on. The main screen answers it with the tree pane below;
	// the debugger answers it with a running tree, and neither answer belongs in here.
	ADD_SIGNAL(MethodInfo("state_picked", PropertyInfo(Variant::OBJECT, "state", PROPERTY_HINT_RESOURCE_TYPE, "HSMStateSpec")));
}

HSMEditorView::HSMEditorView() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);

	list_box = memnew(VBoxContainer);
	list_box->set_custom_minimum_size(Size2(220, 0) * EDSCALE);
	add_child(list_box);

	// No title above the list: it said the same thing the row under it says, and two
	// labels for one machine are two places to read the same name from.
	state_list = memnew(Tree);
	state_list->set_h_size_flags(SIZE_EXPAND_FILL);
	state_list->set_v_size_flags(SIZE_EXPAND_FILL);
	state_list->set_hide_root(false);
	state_list->set_columns(1);
	// A right-click picks the row, then opens the same menu its node on the graph has.
	state_list->set_allow_rmb_select(true);
	list_box->add_child(state_list);

	right_split = memnew(HSplitContainer);
	right_split->set_h_size_flags(SIZE_EXPAND_FILL);
	right_split->set_v_size_flags(SIZE_EXPAND_FILL);
	right_split->set_focus_mode(FOCUS_NONE);
	add_child(right_split);

	graph = memnew(HSMGraphEdit);
	graph->set_h_size_flags(SIZE_EXPAND_FILL);
	graph->set_v_size_flags(SIZE_EXPAND_FILL);
	graph->set_show_arrange_button(false);
	right_split->add_child(graph);

	// Down until a state that holds a tree is picked. A hidden child leaves the split with
	// one, which is the whole of "the graph gets the room back".
	tree_box = memnew(VBoxContainer);
	tree_box->set_custom_minimum_size(Size2(280, 0) * EDSCALE);
	tree_box->hide();
	right_split->add_child(tree_box);

	HBoxContainer *tree_header = memnew(HBoxContainer);
	tree_box->add_child(tree_header);

	tree_title = memnew(Label);
	tree_title->set_h_size_flags(SIZE_EXPAND_FILL);
	// Clipped rather than allowed to set the width: a Label asks for room enough for all of
	// its text, and a long path would push the pane wider than it was dragged to.
	tree_title->set_clip_text(true);
	tree_title->set_theme_type_variation("HeaderSmall");
	tree_header->add_child(tree_title);

	tree_open_btn = memnew(Button);
	tree_open_btn->set_flat(true);
	tree_open_btn->set_tooltip_text(TTR("Open this behavior tree in its own tab."));
	tree_header->add_child(tree_open_btn);

	tree_view = memnew(TaskTree);
	tree_view->set_h_size_flags(SIZE_EXPAND_FILL);
	tree_view->set_v_size_flags(SIZE_EXPAND_FILL);
	// Shown, not edited. Nothing here is wired to the task editor's actions either, so the
	// only gesture this tree answers is picking a row open or shut.
	tree_view->set_editable(false);
	tree_box->add_child(tree_view);

	// One texture for the whole view rather than one per node: it is the same blank, and a
	// refresh makes a dozen nodes.
	Ref<Image> blank = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
	blank->fill(Color(0, 0, 0, 0));
	blank_port = ImageTexture::create_from_image(blank);

	node_menu = memnew(PopupMenu);
	add_child(node_menu);
	transition_menu = memnew(PopupMenu);
	transition_menu->set_name("TransitionMenu");
	node_menu->add_child(transition_menu);

	line_menu = memnew(PopupMenu);
	line_menu->set_name("LineMenu");
	add_child(line_menu);

	file_menu = memnew(PopupMenu);
	add_child(file_menu);

	// ! HACK: the same one the task editor carries. An action's history is worked out from
	// the objects in it, and the objects here are sub-resources with no path -- a context of
	// our own plus force_fixed_history() puts every one of them in the global history, which
	// is the one Ctrl+Z reaches from a main screen.
	history_context = memnew(Object);
}

HSMEditorView::~HSMEditorView() {
	memdelete(history_context);
}

#endif // ! TOOLS_ENABLED
