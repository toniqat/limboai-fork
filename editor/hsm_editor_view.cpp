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
#include "../compat/translation.h"
#include "../util/limbo_utility.h"

#ifdef LIMBOAI_MODULE
#include "core/string/string_name.h"
#include "scene/gui/control.h"
#include "scene/resources/style_box_flat.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/tree_item.hpp>
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

} // namespace

StringName HSMEditorView::_node_name_for(const Ref<HSMState> &p_state) {
	// The instance id, not the state name: names are only unique among siblings, and
	// GraphEdit needs one name per node across the whole graph.
	return StringName(itos(p_state->get_instance_id()));
}

StringName HSMEditorView::_anystate_node_name_for(const Ref<HSMState> &p_machine) {
	return StringName("any_" + itos(p_machine->get_instance_id()));
}

GraphNode *HSMEditorView::_make_node(const Ref<HSMState> &p_state, const Ref<HSMState> &p_machine, int p_depth, int p_row) {
	GraphNode *node = memnew(GraphNode);
	node->set_name(_node_name_for(p_state));
	node->set_title(String(p_state->get_state_name()));

	Color color = COLOR_PLAIN;
	String kind = TTR("state");
	if (p_state->is_machine()) {
		color = COLOR_MACHINE;
		kind = vformat(TTR("machine (%d)"), p_state->get_children().size());
	} else if (p_state->get_behavior_tree().is_valid()) {
		color = COLOR_TREE;
		kind = TTR("behavior tree");
	}

	// A GraphNode needs at least one child control before slot 0 exists.
	Label *label = memnew(Label);
	label->set_text(kind);
	node->add_child(label);
	node->set_slot(0, true, 0, color, true, 0, color);

	Vector2 pos = p_state->get_graph_position();
	if (pos == Vector2()) {
		pos = Vector2(p_depth * COLUMN_WIDTH, p_row * ROW_HEIGHT);
	}
	node->set_position_offset(pos);
	return node;
}

GraphNode *HSMEditorView::_make_anystate_node(const Ref<HSMState> &p_machine, int p_depth, int p_row) {
	GraphNode *node = memnew(GraphNode);
	node->set_name(_anystate_node_name_for(p_machine));
	node->set_title(String::utf8("✱ ANYSTATE"));

	Label *label = memnew(Label);
	label->set_text(TTR("no state is being left"));
	node->add_child(label);
	// Only an outgoing port: ANYSTATE is never a destination.
	node->set_slot(0, false, 0, COLOR_ANYSTATE, true, 0, COLOR_ANYSTATE);

	node->set_position_offset(Vector2(p_depth * COLUMN_WIDTH, p_row * ROW_HEIGHT));
	return node;
}

void HSMEditorView::_populate_graph(const Ref<HSMState> &p_machine, int p_depth, int &r_row) {
	ERR_FAIL_COND(p_machine.is_null());

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
		r_row += 1;
	}

	TypedArray<HSMState> children = p_machine->get_children();
	for (int i = 0; i < children.size(); i++) {
		Ref<HSMState> spec = children[i];
		if (spec.is_null()) {
			continue;
		}
		StringName node_name = _node_name_for(spec);
		if (node_names.has(spec->get_instance_id())) {
			// The same resource twice would be two nodes with one name.
			continue;
		}

		GraphNode *node = _make_node(spec, p_machine, p_depth, r_row);
		graph->add_child(node);

		DrawnState entry;
		entry.spec = spec;
		entry.machine = p_machine;
		entry.depth = p_depth;
		drawn[node_name] = entry;
		node_names[spec->get_instance_id()] = node_name;
		r_row += 1;

		if (spec->is_machine()) {
			_populate_graph(spec, p_depth + 1, r_row);
		}
	}
}

void HSMEditorView::_wire_graph(const Ref<HSMState> &p_machine) {
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

	TypedArray<HSMState> children = p_machine->get_children();
	for (int i = 0; i < children.size(); i++) {
		Ref<HSMState> spec = children[i];
		if (spec.is_valid() && spec->is_machine()) {
			_wire_graph(spec);
		}
	}
}

void HSMEditorView::_populate_list(const Ref<HSMState> &p_machine, TreeItem *p_parent) {
	ERR_FAIL_COND(p_machine.is_null());

	TypedArray<HSMState> children = p_machine->get_children();
	for (int i = 0; i < children.size(); i++) {
		Ref<HSMState> spec = children[i];
		if (spec.is_null()) {
			continue;
		}
		TreeItem *item = state_list->create_item(p_parent);
		item->set_text(0, String(spec->get_state_name()));
		item->set_metadata(0, Variant(int64_t(spec->get_instance_id())));

		String suffix;
		if (spec->is_machine()) {
			item->set_custom_color(0, COLOR_MACHINE);
		} else if (spec->get_behavior_tree().is_valid()) {
			item->set_custom_color(0, COLOR_TREE);
			suffix = " · BT";
		}
		if (p_machine->get_initial_child() == spec) {
			// The one place the file answers "where does this machine start".
			suffix = String::utf8(" ▶") + suffix;
		}
		if (!suffix.is_empty()) {
			item->set_text(0, String(spec->get_state_name()) + suffix);
		}

		if (spec->is_machine()) {
			_populate_list(spec, item);
			item->set_collapsed(false);
		}
	}
}

void HSMEditorView::refresh() {
	updating = true;

	graph->clear_connections();
	// Only the nodes we put there. GraphEdit keeps a non-internal `connections_layer`
	// child of its own, and removing it takes the lines with it -- after which every
	// node move errors out.
	Vector<Node *> ours;
	for (int i = 0; i < graph->get_child_count(); i++) {
		GraphNode *node = Object::cast_to<GraphNode>(graph->get_child(i));
		if (node != nullptr) {
			ours.push_back(node);
		}
	}
	for (int i = 0; i < ours.size(); i++) {
		graph->remove_child(ours[i]);
		memdelete(ours[i]);
	}
	state_list->clear();
	drawn.clear();
	node_names.clear();

	if (hsm.is_null() || hsm->get_root().is_null()) {
		list_title->set_text(TTR("No state machine"));
		updating = false;
		return;
	}

	Ref<HSMState> root = hsm->get_root();
	list_title->set_text(String(root->get_state_name()));

	int row = 0;
	_populate_graph(root, 0, row);
	_wire_graph(root);

	TreeItem *tree_root = state_list->create_item();
	tree_root->set_text(0, String(root->get_state_name()));
	tree_root->set_metadata(0, Variant(int64_t(root->get_instance_id())));
	_populate_list(root, tree_root);

	updating = false;
}

void HSMEditorView::edit_hsm(const Ref<HierarchicalStateMachine> &p_hsm) {
	hsm = p_hsm;
	if (hsm.is_valid() && hsm->get_root().is_null()) {
		// A file created from the FileSystem dock arrives empty. Giving it a root here
		// rather than in the constructor keeps "an empty file" loadable and still leaves
		// the editor with something to draw.
		Ref<HSMState> root;
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

	// The graph draws structure; everything else about a state -- its name, its script,
	// its tree, its transitions and their events -- is shown by the Inspector. Splitting
	// it that way is what the task tree already does for tasks.
	const DrawnState *entry = drawn.getptr(*name);
	if (entry != nullptr) {
		EditorInterface::get_singleton()->edit_resource(entry->spec);
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

	EditorInterface::get_singleton()->edit_resource(entry->spec);
}

void HSMEditorView::_on_graph_end_node_move() {
	if (updating || hsm.is_null()) {
		return;
	}
	bool moved = false;
	for (int i = 0; i < graph->get_child_count(); i++) {
		GraphNode *node = Object::cast_to<GraphNode>(graph->get_child(i));
		if (node == nullptr) {
			continue;
		}
		DrawnState *entry = drawn.getptr(node->get_name());
		if (entry == nullptr) {
			continue;
		}
		// Snapped to whole pixels: writing floats straight through puts a diff in the
		// file every time a node is merely brushed.
		Vector2 pos = node->get_position_offset().round();
		if (entry->spec->get_graph_position() != pos) {
			entry->spec->set_graph_position(pos);
			moved = true;
		}
	}
	if (moved) {
		emit_signal("hsm_edited");
	}
}

Ref<HSMState> HSMEditorView::_selected_spec() const {
	TreeItem *item = state_list->get_selected();
	if (item == nullptr) {
		return nullptr;
	}
	uint64_t id = uint64_t(int64_t(item->get_metadata(0)));
	if (hsm.is_valid() && hsm->get_root().is_valid() && hsm->get_root()->get_instance_id() == id) {
		return hsm->get_root();
	}
	const StringName *name = node_names.getptr(id);
	if (name == nullptr) {
		return nullptr;
	}
	const DrawnState *entry = drawn.getptr(*name);
	return entry != nullptr ? entry->spec : Ref<HSMState>();
}

Ref<HSMState> HSMEditorView::_machine_of(const Ref<HSMState> &p_state) const {
	if (p_state.is_null()) {
		return nullptr;
	}
	const StringName *name = node_names.getptr(p_state->get_instance_id());
	if (name == nullptr) {
		return nullptr;
	}
	const DrawnState *entry = drawn.getptr(*name);
	return entry != nullptr ? entry->machine : Ref<HSMState>();
}

StringName HSMEditorView::_unique_child_name(const Ref<HSMState> &p_machine) const {
	ERR_FAIL_COND_V(p_machine.is_null(), StringName("State"));
	TypedArray<HSMState> children = p_machine->get_children();
	for (int n = 1; n < 1000; n++) {
		String candidate = n == 1 ? String("State") : vformat("State%d", n);
		bool taken = false;
		for (int i = 0; i < children.size(); i++) {
			Ref<HSMState> spec = children[i];
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

void HSMEditorView::_detach_state(const Ref<HSMState> &p_machine, const Ref<HSMState> &p_state) {
	ERR_FAIL_COND(p_machine.is_null() || p_state.is_null());

	TypedArray<HSMState> children = p_machine->get_children();
	for (int i = children.size() - 1; i >= 0; i--) {
		Ref<HSMState> spec = children[i];
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

void HSMEditorView::_on_add_pressed() {
	if (hsm.is_null()) {
		return;
	}
	Ref<HSMState> parent = _selected_spec();
	if (parent.is_null()) {
		parent = hsm->get_root();
	}
	ERR_FAIL_COND(parent.is_null());

	// A state becomes a machine by gaining a child. There is no separate button for it
	// because there is no separate thing.
	Ref<HSMState> spec;
	spec.instantiate();
	spec->set_state_name(_unique_child_name(parent));

	TypedArray<HSMState> children = parent->get_children();
	children.append(spec);
	parent->set_children(children);

	refresh();
	emit_signal("hsm_edited");
}

void HSMEditorView::_on_delete_pressed() {
	if (hsm.is_null()) {
		return;
	}
	Ref<HSMState> spec = _selected_spec();
	if (spec.is_null() || spec == hsm->get_root()) {
		// The root is the machine itself; deleting it would leave the file with nothing.
		return;
	}
	Ref<HSMState> machine = _machine_of(spec);
	if (machine.is_null()) {
		return;
	}
	_detach_state(machine, spec);

	refresh();
	emit_signal("hsm_edited");
}

void HSMEditorView::_on_connection_request(const StringName &p_from, int p_from_port, const StringName &p_to, int p_to_port) {
	if (hsm.is_null()) {
		return;
	}
	const DrawnState *to_entry = drawn.getptr(p_to);
	if (to_entry == nullptr) {
		// ANYSTATE is never a destination: it says "no state is being left".
		return;
	}

	Ref<HSMState> machine;
	Ref<HSMState> from_spec;
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

	Ref<HSMTransition> row;
	row.instantiate();
	row->set_from(from_spec);
	row->set_to(to_entry->spec);
	// The event is left empty on purpose -- it is the one thing the graph cannot guess,
	// and the inspector shows it as the blank it is.

	TypedArray<HSMTransition> rows = machine->get_transitions();
	rows.append(row);
	machine->set_transitions(rows);

	refresh();
	emit_signal("hsm_edited");
}

void HSMEditorView::_on_disconnection_request(const StringName &p_from, int p_from_port, const StringName &p_to, int p_to_port) {
	if (hsm.is_null()) {
		return;
	}
	const DrawnState *to_entry = drawn.getptr(p_to);
	if (to_entry == nullptr) {
		return;
	}
	const DrawnState *from_entry = drawn.getptr(p_from);
	Ref<HSMState> from_spec = from_entry != nullptr ? from_entry->spec : Ref<HSMState>();
	Ref<HSMState> machine = to_entry->machine;

	TypedArray<HSMTransition> rows = machine->get_transitions();
	int found = -1;
	int matches = 0;
	for (int i = 0; i < rows.size(); i++) {
		Ref<HSMTransition> row = rows[i];
		if (row.is_valid() && row->get_to() == to_entry->spec && row->get_from() == from_spec) {
			matches += 1;
			found = i;
		}
	}
	if (matches > 1) {
		// One line, two events: removing "the" edge would silently drop an event that
		// was never on screen. The inspector's transition list removes them one by one.
		WARN_PRINT("LimboAI: This connection stands for more than one transition; remove them in the Inspector.");
		return;
	}
	if (found < 0) {
		return;
	}
	rows.remove_at(found);
	machine->set_transitions(rows);

	refresh();
	emit_signal("hsm_edited");
}

void HSMEditorView::_on_delete_nodes_request(const TypedArray<StringName> &p_nodes) {
	if (hsm.is_null()) {
		return;
	}
	bool removed = false;
	for (int i = 0; i < p_nodes.size(); i++) {
		StringName name = p_nodes[i];
		const DrawnState *entry = drawn.getptr(name);
		if (entry == nullptr) {
			continue;
		}
		_detach_state(entry->machine, entry->spec);
		removed = true;
	}
	if (removed) {
		refresh();
		emit_signal("hsm_edited");
	}
}

void HSMEditorView::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			state_list->connect("item_selected", callable_mp(this, &HSMEditorView::_on_list_item_selected));
			graph->connect("node_selected", callable_mp(this, &HSMEditorView::_on_graph_node_selected));
			graph->connect("end_node_move", callable_mp(this, &HSMEditorView::_on_graph_end_node_move));
			graph->connect("connection_request", callable_mp(this, &HSMEditorView::_on_connection_request));
			graph->connect("disconnection_request", callable_mp(this, &HSMEditorView::_on_disconnection_request));
			graph->connect("delete_nodes_request", callable_mp(this, &HSMEditorView::_on_delete_nodes_request));
			add_btn->connect("pressed", callable_mp(this, &HSMEditorView::_on_add_pressed));
			del_btn->connect("pressed", callable_mp(this, &HSMEditorView::_on_delete_pressed));
		} break;
	}
}

void HSMEditorView::_bind_methods() {
	ADD_SIGNAL(MethodInfo("hsm_edited"));
}

HSMEditorView::HSMEditorView() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);

	list_box = memnew(VBoxContainer);
	list_box->set_custom_minimum_size(Size2(220, 0) * EDSCALE);
	add_child(list_box);

	list_title = memnew(Label);
	list_title->set_text(TTR("States"));
	list_box->add_child(list_title);

	toolbar = memnew(HBoxContainer);
	list_box->add_child(toolbar);

	add_btn = memnew(Button);
	add_btn->set_text(TTR("Add State"));
	add_btn->set_tooltip_text(TTR("Adds a child to the selected state, which is what makes it a machine."));
	toolbar->add_child(add_btn);

	del_btn = memnew(Button);
	del_btn->set_text(TTR("Remove"));
	toolbar->add_child(del_btn);

	state_list = memnew(Tree);
	state_list->set_h_size_flags(SIZE_EXPAND_FILL);
	state_list->set_v_size_flags(SIZE_EXPAND_FILL);
	state_list->set_hide_root(false);
	state_list->set_columns(1);
	list_box->add_child(state_list);

	graph = memnew(GraphEdit);
	graph->set_h_size_flags(SIZE_EXPAND_FILL);
	graph->set_v_size_flags(SIZE_EXPAND_FILL);
	graph->set_right_disconnects(true);
	graph->set_show_arrange_button(false);
	add_child(graph);
}

#endif // ! TOOLS_ENABLED
