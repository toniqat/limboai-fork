/**
 * hsm_graph_edit.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#include "hsm_graph_edit.h"

#include "../compat/editor_scale.h"
#include "../compat/limbo_compat.h"
#include "hsm_nest_node.h"

#ifdef LIMBOAI_MODULE
#include "core/input/input_event.h"
#include "core/math/math_funcs.h"
#include "scene/main/viewport.h"
#include "scene/gui/box_container.h"
#include "scene/resources/style_box.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#endif // LIMBOAI_GDEXTENSION

namespace {

// The arrowhead, a filled triangle. Line thickness comes from the theme, so only the length
// and the half-angle are here.
const float ARROW_LENGTH = 16.0;
const float ARROW_SPREAD = 0.5;
// Where the line stops, as a share of the head's depth measured from the tip: just past the
// base, so the two touch without a gap. Any deeper and the line's antialiased edges are drawn
// across the inside of the head as dark seams.
const float ARROW_LINE_INSET = 0.92;
// Between the border and the tip. At zero the head is buried in the node.
const float EDGE_GAP = 6.0;

// How far the control point is pushed aside. What is seen is half of it: a quadratic
// passes through the chord's midpoint plus offset/2 at t=0.5.
const float BOW_STEP = 130.0;
const int BOW_TRIES = 4;
const int CURVE_SEGMENTS = 16;
// Slack around a node. A line that merely grazes one already reads as crossing it.
const float CLEARANCE = 10.0;

// While a new edge is being aimed. Green and red rather than one highlight: the question
// the cursor is asking is not "which node is under me" but "may this one be the end of it",
// and a node that may not has to say so before the click, not after.
const Color COLOR_PENDING = Color(0.55, 0.78, 1.0);
const Color COLOR_TARGET_OK = Color(0.45, 0.90, 0.55);
const Color COLOR_TARGET_NO = Color(0.95, 0.40, 0.40);

// The ring around a node a running machine is standing in. It is drawn rather than switched
// on, because what would be switched on is the node's own selected look -- and that one is
// already answering a different question: which node the reader picked. Two questions, two
// marks, and white is the one nothing else on this graph uses.
//
// Outside the node and not on its border: the border is the theme's, and a ring laid over it
// would read as the node having changed rather than as something put around it.
const Color COLOR_LIT = Color(1, 1, 1);
const float LIT_GROW = 3.0;
const float LIT_WIDTH = 2.0;

// How near a press has to land to count as landing on an edge. GraphEdit's own default is
// 4, which is about the thickness of the line -- and an edge here is a curve rather than the
// straight run between two ports, so aiming at it is harder than aiming at those.
const float PICK_DISTANCE = 8.0;

} // namespace

Rect2 HSMGraphEdit::_rect_of(GraphNode *p_node) {
	Size2 size = p_node->get_size();
	if (size.x < 1.0 || size.y < 1.0) {
		size = p_node->get_combined_minimum_size();
	}
	return Rect2(p_node->get_position_offset(), size);
}

float HSMGraphEdit::_zoom() const {
	float zoom = get_zoom();
	return zoom > 0.0 ? zoom : 1.0;
}

Rect2 HSMGraphEdit::_screen_rect_of(GraphNode *p_node) const {
	Size2 size = p_node->get_size();
	if (size.x < 1.0 || size.y < 1.0) {
		size = p_node->get_combined_minimum_size();
	}
	// get_position() already carries the scroll and the zoom; the size does not, because a
	// zoomed node is a scaled node.
	return Rect2(p_node->get_position(), size * _zoom());
}

// Where a ray from the center leaves the rect.
Vector2 HSMGraphEdit::_edge_point(const Rect2 &p_rect, const Vector2 &p_dir) {
	Vector2 center = p_rect.get_center();
	Vector2 half = p_rect.size * 0.5;
	float tx = Math::abs(p_dir.x) < 0.0001 ? INFINITY : half.x / Math::abs(p_dir.x);
	float ty = Math::abs(p_dir.y) < 0.0001 ? INFINITY : half.y / Math::abs(p_dir.y);
	float t = MIN(tx, ty);
	if (!Math::is_finite(t)) {
		return center;
	}
	return center + p_dir * t;
}

// Slab clipping. Sampled points would miss a node clipped at a corner, and nodes here are
// wide enough for that to happen on most diagonals.
bool HSMGraphEdit::_segment_hits(const Vector2 &p_a, const Vector2 &p_b, const Rect2 &p_rect) {
	if (p_rect.has_point(p_a) || p_rect.has_point(p_b)) {
		return true;
	}
	Vector2 d = p_b - p_a;
	float t0 = 0.0;
	float t1 = 1.0;
	for (int axis = 0; axis < 2; axis++) {
		float dv = d[axis];
		float lo = p_rect.position[axis];
		float hi = p_rect.position[axis] + p_rect.size[axis];
		if (Math::abs(dv) < 0.0001) {
			if (p_a[axis] < lo || p_a[axis] > hi) {
				return false;
			}
			continue;
		}
		float ta = (lo - p_a[axis]) / dv;
		float tb = (hi - p_a[axis]) / dv;
		if (ta > tb) {
			SWAP(ta, tb);
		}
		t0 = MAX(t0, ta);
		t1 = MIN(t1, tb);
		if (t0 > t1) {
			return false;
		}
	}
	return true;
}

int HSMGraphEdit::_hits(const Rect2 &p_from, const Rect2 &p_to, const Vector2 &p_ctrl, const Vector<Rect2> &p_blockers) {
	Vector2 p0 = _edge_point(p_from, (p_ctrl - p_from.get_center()).normalized());
	Vector2 p1 = _edge_point(p_to, (p_ctrl - p_to.get_center()).normalized());
	int count = 0;
	for (int i = 0; i < p_blockers.size(); i++) {
		const Rect2 &rect = p_blockers[i];
		Vector2 prev = p0;
		for (int s = 1; s <= CURVE_SEGMENTS; s++) {
			float t = float(s) / float(CURVE_SEGMENTS);
			float u = 1.0 - t;
			Vector2 cur = p0 * (u * u) + p_ctrl * (2.0 * u * t) + p1 * (t * t);
			if (_segment_hits(prev, cur, rect)) {
				count += 1;
				break;
			}
			prev = cur;
		}
	}
	return count;
}

// `p_out` points away from the node, so the wings open that way: wing, tip, wing.
PackedVector2Array HSMGraphEdit::_head(const Vector2 &p_tip, const Vector2 &p_out, float p_scale) {
	PackedVector2Array points;
	points.push_back(p_tip + p_out.rotated(ARROW_SPREAD) * ARROW_LENGTH * p_scale);
	points.push_back(p_tip);
	points.push_back(p_tip + p_out.rotated(-ARROW_SPREAD) * ARROW_LENGTH * p_scale);
	return points;
}

// Filled, with a one pixel antialiased outline: a polygon alone has stair-stepped edges
// next to the antialiased line it caps.
void HSMGraphEdit::_draw_head(Control *p_canvas, const PackedVector2Array &p_head, const Color &p_color) {
	p_canvas->draw_colored_polygon(p_head, p_color);
	PackedVector2Array outline = p_head;
	outline.push_back(p_head[0]);
	p_canvas->draw_polyline(outline, p_color, 1.0, true);
}

Vector2 HSMGraphEdit::_head_base(const Vector2 &p_tip, const Vector2 &p_out, float p_scale) {
	return p_tip + p_out * (ARROW_LENGTH * Math::cos(ARROW_SPREAD) * ARROW_LINE_INSET * p_scale);
}

// The height GraphNode gives its title bar: the row of title controls plus the bar's margins.
float HSMGraphEdit::_title_height(GraphNode *p_node) {
	float height = p_node->get_titlebar_hbox()->get_size().y;
	Ref<StyleBox> bar = p_node->get_theme_stylebox(StringName("titlebar"));
	if (bar.is_valid()) {
		height += bar->get_minimum_size().y;
	}
	return height;
}

// The node holding a port at this position. GraphEdit builds the argument as
// `port position + position offset`, so the same sum finds its way back.
GraphNode *HSMGraphEdit::_node_at(const Vector2 &p_position, bool p_input) const {
	for (int i = 0; i < get_child_count(); i++) {
		GraphNode *node = Object::cast_to<GraphNode>(get_child(i));
		if (node == nullptr) {
			continue;
		}
		Vector2 port;
		if (p_input) {
			if (node->get_input_port_count() < 1) {
				continue;
			}
			port = node->get_input_port_position(0);
		} else {
			if (node->get_output_port_count() < 1) {
				continue;
			}
			port = node->get_output_port_position(0);
		}
		if ((node->get_position_offset() + port).distance_squared_to(p_position) < 4.0) {
			return node;
		}
	}
	return nullptr;
}

HSMGraphEdit::Route HSMGraphEdit::_route(GraphNode *p_from, GraphNode *p_to) const {
	// A pair that goes both ways is one line with a head at each end. The two connections
	// stay and are made to agree on the exact same line instead, so they draw on top of
	// each other. Agreeing means neither end may decide anything of its own: the pair is
	// put in name order here, before a single measurement is taken.
	bool both = const_cast<HSMGraphEdit *>(this)->is_node_connected(p_to->get_name(), 0, p_from->get_name(), 0);
	if (both && String(p_from->get_name()) > String(p_to->get_name())) {
		SWAP(p_from, p_to);
	}

	Rect2 from_rect = _rect_of(p_from);
	Rect2 to_rect = _rect_of(p_to);
	Vector2 from_center = from_rect.get_center();
	Vector2 to_center = to_rect.get_center();

	Route route;
	if (from_center.distance_squared_to(to_center) < 1.0) {
		route.line.push_back(from_center);
		route.line.push_back(to_center);
		return route;
	}

	// Only the nodes this edge does not belong to. Its own ends are always "crossed",
	// since the line starts on their borders.
	//
	// A node that an end stands *inside* is not in the way of it either. A machine is drawn
	// as a node around the states it holds, so every edge between two of those begins and
	// ends within it: counted as a crossing there would be no way around it, and each of
	// those lines would be bowed as far aside as the graph allows for nothing.
	Vector<Rect2> blockers;
	for (int i = 0; i < get_child_count(); i++) {
		GraphNode *node = Object::cast_to<GraphNode>(get_child(i));
		if (node == nullptr || node == p_from || node == p_to) {
			continue;
		}
		Rect2 rect = _rect_of(node);
		if (rect.encloses(from_rect) || rect.encloses(to_rect)) {
			// Its title bar still is: the bar is drawn over the lines, and an edge leaving the
			// box through it would vanish there. Not grown -- the states inside stand just
			// below the bar, and their own borders would count as crossing it.
			blockers.push_back(Rect2(rect.position, Size2(rect.size.x, _title_height(node))));
			continue;
		}
		blockers.push_back(rect.grow(CLEARANCE));
	}

	Vector2 normal = (to_center - from_center).normalized().orthogonal();
	Vector2 mid = (from_center + to_center) * 0.5;

	Vector2 best = mid;
	int best_hits = 0x7FFFFFFF;
	for (int i = 0; i < BOW_TRIES * 2 + 1; i++) {
		// Straight first, then the same width to either side.
		int step = (i + 1) / 2;
		float sign = (i % 2 == 0) ? 1.0 : -1.0;
		Vector2 ctrl = mid + normal * (BOW_STEP * step * sign);
		int hits = _hits(from_rect, to_rect, ctrl, blockers);
		if (hits < best_hits) {
			best_hits = hits;
			best = ctrl;
		}
		if (hits == 0) {
			break;
		}
	}

	// Where the line leaves is decided by where it is headed: the ray from the center
	// towards the control point, taken to the border.
	Vector2 from_dir = (best - from_center).normalized();
	Vector2 to_dir = (best - to_center).normalized();
	route.tip_node = p_to;
	route.tip = _edge_point(to_rect, to_dir) + to_dir * EDGE_GAP;
	route.tip_out = to_dir;
	Vector2 p0 = _edge_point(from_rect, from_dir) + from_dir * EDGE_GAP;
	if (both) {
		route.tail_node = p_from;
		route.tail = p0;
		route.tail_out = from_dir;
		p0 = _head_base(p0, from_dir, 1.0);
	}
	// The ends sit on the rays from each center through the control point, so the curve
	// arrives along the same line the head points down.
	Vector2 p1 = _head_base(route.tip, to_dir, 1.0);
	for (int s = 0; s <= CURVE_SEGMENTS; s++) {
		float t = float(s) / float(CURVE_SEGMENTS);
		float u = 1.0 - t;
		route.line.push_back(p0 * (u * u) + best * (2.0 * u * t) + p1 * (t * t));
	}
	return route;
}

// Under GraphEdit's Line2D children, in their coordinates: unscaled graph space times the
// zoom, the layer itself being shifted by the scroll. First the machines' boxes, outermost
// first (the child order the view keeps them in), then every head.
void HSMGraphEdit::_draw_under_lines() {
	float zoom = _zoom();
	Ref<StyleBox> panel = get_theme_stylebox(StringName("panel"), StringName("GraphNode"));
	Ref<StyleBox> panel_selected = get_theme_stylebox(StringName("panel_selected"), StringName("GraphNode"));
	for (int i = 0; i < get_child_count(); i++) {
		HSMNestNode *nest = Object::cast_to<HSMNestNode>(get_child(i));
		if (nest == nullptr || !nest->is_visible()) {
			continue;
		}
		Ref<StyleBox> box = nest->is_selected() && panel_selected.is_valid() ? panel_selected : panel;
		if (box.is_null()) {
			continue;
		}
		Rect2 rect = _rect_of(nest);
		float title = _title_height(nest);
		lines_layer->draw_set_transform(rect.position * zoom, 0.0, Vector2(zoom, zoom));
		lines_layer->draw_style_box(box, Rect2(0.0, title, rect.size.x, rect.size.y - title));
	}
	lines_layer->draw_set_transform(Vector2(), 0.0, Vector2(1, 1));

	Color activity_color = get_theme_color(StringName("activity"), StringName("GraphEdit"));
	TypedArray<Dictionary> list = get_connection_list();
	for (int i = 0; i < list.size(); i++) {
		Dictionary c = list[i];
		GraphNode *from = Object::cast_to<GraphNode>(get_node_or_null(NodePath(String(StringName(c["from_node"])))));
		GraphNode *to = Object::cast_to<GraphNode>(get_node_or_null(NodePath(String(StringName(c["to_node"])))));
		if (from == nullptr || to == nullptr || from == to || !from->is_visible() || !to->is_visible()) {
			continue;
		}
		Route route = _route(from, to);
		if (route.tip_node == nullptr) {
			continue;
		}
		const float *amount = activity.getptr(String(from->get_name()) + "\n" + String(to->get_name()));
		float lit = amount != nullptr ? *amount : 0.0f;
		// The line's own colors: GraphEdit paints a gradient from this connection's source color
		// at the line's first point to its destination color at the last. A pair going both ways
		// shares one route, so for the second of the two the first point is at its destination;
		// colored by the connection rather than by the route's ends, each head matches the line
		// it caps, and the pair's later connection is drawn last here as it is there.
		Color first_color = from->get_slot_color_right(0).lerp(activity_color, lit);
		Color last_color = to->get_slot_color_left(0).lerp(activity_color, lit);
		_draw_head(lines_layer, _head(route.tip * zoom, route.tip_out, zoom), last_color);
		if (route.tail_node != nullptr) {
			_draw_head(lines_layer, _head(route.tail * zoom, route.tail_out, zoom), first_color);
		}
	}
}

void HSMGraphEdit::_on_node_selection_changed(Node *p_node) {
	// A selected box is painted with the selected panel, and nothing else redraws the layer.
	if (Object::cast_to<HSMNestNode>(p_node) != nullptr) {
		lines_layer->queue_redraw();
	}
}

void HSMGraphEdit::clear_edges() {
	activity.clear();
	clear_connections();
}

void HSMGraphEdit::set_edge_activity(const StringName &p_from, const StringName &p_to, float p_amount) {
	activity[String(p_from) + "\n" + String(p_to)] = p_amount;
	set_connection_activity(p_from, 0, p_to, 0, p_amount);
	if (lines_layer != nullptr) {
		lines_layer->queue_redraw();
	}
}

void HSMGraphEdit::settle_layers() {
	if (lines_layer != nullptr && lines_layer->get_parent() == this) {
		move_child(lines_layer, 0);
	}
}

// ── Aiming a new edge at a node ───────────────────────────────────────────────

GraphNode *HSMGraphEdit::_node_under(const Vector2 &p_local) const {
	// Backwards: the last child is the one drawn on top, and that is the one the eye
	// picked when two of them overlap.
	for (int i = get_child_count() - 1; i >= 0; i--) {
		GraphNode *node = Object::cast_to<GraphNode>(get_child(i));
		if (node == nullptr || !node->is_visible()) {
			continue;
		}
		if (_screen_rect_of(node).has_point(p_local)) {
			return node;
		}
	}
	return nullptr;
}

bool HSMGraphEdit::_is_target(const StringName &p_name) const {
	for (int i = 0; i < connect_targets.size(); i++) {
		if (connect_targets[i] == p_name) {
			return true;
		}
	}
	return false;
}

void HSMGraphEdit::_update_hover() {
	GraphNode *node = _node_under(get_local_mouse_position());
	hovered = node != nullptr ? node->get_name() : StringName();
	// Always, not only when the node changed: the line itself follows the cursor.
	overlay->queue_redraw();
}

void HSMGraphEdit::_draw_overlay() {
	_draw_lit();
	_draw_pending();
}

// The chain, drawn from the outside in. Order does not matter to the picture -- the rings do
// not overlap, one being a box the other stands inside -- but it is the order the chain
// arrives in and nothing here has a reason to change it.
void HSMGraphEdit::_draw_lit() {
	for (int i = 0; i < lit.size(); i++) {
		GraphNode *node = Object::cast_to<GraphNode>(get_node_or_null(NodePath(String(lit[i]))));
		if (node == nullptr || !node->is_visible()) {
			continue;
		}
		overlay->draw_rect(_screen_rect_of(node).grow(LIT_GROW * EDSCALE), COLOR_LIT, false, LIT_WIDTH * EDSCALE);
	}
}

void HSMGraphEdit::set_lit_nodes(const Vector<StringName> &p_names) {
	lit = p_names;
	// Above every node, and it has to be said again each time for the same reason
	// begin_connect_from() says it: the view rebuilds its GraphNodes on every report from the
	// game, and they land after whatever was already there.
	move_child(overlay, get_child_count() - 1);
	overlay->queue_redraw();
}

void HSMGraphEdit::redraw_overlay() {
	overlay->queue_redraw();
}

void HSMGraphEdit::_on_scroll_offset_changed(const Vector2 &p_offset) {
	if (lit.is_empty()) {
		// Nothing is drawn on it while a machine is not being watched, and this fires on every
		// pixel of every pan.
		return;
	}
	overlay->queue_redraw();
}

void HSMGraphEdit::_draw_pending() {
	if (!connecting) {
		return;
	}
	GraphNode *from = Object::cast_to<GraphNode>(get_node_or_null(NodePath(String(connect_from))));
	if (from == nullptr) {
		return;
	}
	float zoom = _zoom();
	float width = 2.0 * EDSCALE;

	Rect2 from_rect = _screen_rect_of(from);
	overlay->draw_rect(from_rect.grow(3.0 * EDSCALE), Color(COLOR_PENDING, 0.5), false, width);

	Vector2 mouse = get_local_mouse_position();
	Vector2 dir = (mouse - from_rect.get_center()).normalized();
	if (dir == Vector2()) {
		return;
	}

	bool ok = !hovered.is_empty() && _is_target(hovered);
	Color color = hovered.is_empty() ? COLOR_PENDING : (ok ? COLOR_TARGET_OK : COLOR_TARGET_NO);

	Vector2 start = _edge_point(from_rect, dir) + dir * EDGE_GAP * zoom;
	// The tip is at the cursor and the wings open back down the line, which is the same
	// shape a finished edge ends in -- the gesture looks like what it is about to make.
	_draw_head(overlay, _head(mouse, -dir, zoom), color);
	overlay->draw_line(start, _head_base(mouse, -dir, zoom), color, width, true);

	if (!hovered.is_empty()) {
		GraphNode *node = Object::cast_to<GraphNode>(get_node_or_null(NodePath(String(hovered))));
		if (node != nullptr) {
			overlay->draw_rect(_screen_rect_of(node).grow(3.0 * EDSCALE), color, false, width);
		}
	}
}

void HSMGraphEdit::begin_connect_from(const StringName &p_from, const Vector<StringName> &p_targets) {
	connect_from = p_from;
	connect_targets = p_targets;
	connecting = true;
	hovered = StringName();
	// Above every node, and it has to be moved again each time: the view rebuilds its
	// GraphNodes on any edit, and they land after whatever was already there.
	move_child(overlay, get_child_count() - 1);
	set_process_input(true);
	overlay->queue_redraw();
}

void HSMGraphEdit::cancel_connect() {
	if (!connecting) {
		return;
	}
	_stop_connecting();
}

// ── Picking an edge ───────────────────────────────────────────────────────────

// Marked by turning the connection's "activity" up rather than by drawing over it. Activity
// is the one thing about a drawn line GraphEdit will take from outside, and a highlight that
// re-routed the line itself would have to agree with _route() forever.
void HSMGraphEdit::_mark_pick(float p_activity) {
	if (String(picked_from).is_empty() || String(picked_to).is_empty()) {
		return;
	}
	if (is_node_connected(picked_from, 0, picked_to, 0)) {
		set_edge_activity(picked_from, picked_to, p_activity);
	}
	// Both ways when the pair goes both ways. The two are drawn as one line, one on top of
	// the other, so lighting up only one of them leaves the plain one covering it.
	if (is_node_connected(picked_to, 0, picked_from, 0)) {
		set_edge_activity(picked_to, picked_from, p_activity);
	}
}

void HSMGraphEdit::_clear_pick() {
	if (String(picked_from).is_empty()) {
		return;
	}
	_mark_pick(0.0);
	picked_from = StringName();
	picked_to = StringName();
}

bool HSMGraphEdit::_pick_connection_at(const Vector2 &p_position) {
	Dictionary found = get_closest_connection_at_point(p_position, PICK_DISTANCE * EDSCALE);
	_clear_pick();
	if (found.is_empty()) {
		return false;
	}
	picked_from = found["from_node"];
	picked_to = found["to_node"];
	_mark_pick(1.0);
	return true;
}

void HSMGraphEdit::pick_connection(const StringName &p_from, const StringName &p_to) {
	_clear_pick();
	if (!is_node_connected(p_from, 0, p_to, 0) && !is_node_connected(p_to, 0, p_from, 0)) {
		return;
	}
	picked_from = p_from;
	picked_to = p_to;
	_mark_pick(1.0);
}

void HSMGraphEdit::_stop_connecting() {
	connecting = false;
	connect_from = StringName();
	connect_targets.clear();
	hovered = StringName();
	set_process_input(false);
	overlay->queue_redraw();
}

#ifdef LIMBOAI_GDEXTENSION

void HSMGraphEdit::_input(const Ref<InputEvent> &p_event) {
	if (!connecting) {
		return;
	}

	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		_update_hover();
		return;
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->is_pressed()) {
		if (mb->get_button_index() == LW_MBTN(LEFT)) {
			// Wherever it landed ends the gesture, node or not. A click that misses is an
			// answer too, and leaving the mode running after one would mean the graph stays
			// armed while the cursor is somewhere else entirely.
			_update_hover();
			StringName from = connect_from;
			StringName to = hovered;
			bool ok = !to.is_empty() && _is_target(to);
			_stop_connecting();
			get_viewport()->set_input_as_handled();
			if (ok) {
				emit_signal("connect_to_requested", from, to);
			}
			return;
		}
		if (mb->get_button_index() == LW_MBTN(RIGHT)) {
			_stop_connecting();
			get_viewport()->set_input_as_handled();
			return;
		}
	}

	Ref<InputEventKey> k = p_event;
	if (k.is_valid() && k->is_pressed() && k->get_keycode() == LW_KEY(ESCAPE)) {
		_stop_connecting();
		get_viewport()->set_input_as_handled();
	}
}

// A press that reached the graph itself is a press that missed every node -- either it
// landed on empty ground or it landed inside a machine's box, which lets what is not its own
// chrome through. Both are places an edge can be, so both ask.
void HSMGraphEdit::_gui_input(const Ref<InputEvent> &p_event) {
	if (connecting) {
		// Aiming owns the mouse, and it is read in _input() so that it keeps working while
		// the cursor is over a node.
		return;
	}
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed()) {
		return;
	}
	bool right = mb->get_button_index() == LW_MBTN(RIGHT);
	if (!right && mb->get_button_index() != LW_MBTN(LEFT)) {
		return;
	}
	if (!_pick_connection_at(mb->get_position())) {
		// A miss un-picks and is then let through: on empty ground that same press is a box
		// selection or a pan, and swallowing it would cost the graph both.
		return;
	}
	accept_event();
	emit_signal(right ? StringName("connection_menu_requested") : StringName("connection_picked"), picked_from, picked_to);
}

PackedVector2Array HSMGraphEdit::_get_connection_line(const Vector2 &p_from_position, const Vector2 &p_to_position) const {
	// The points arrive already multiplied by the zoom -- the layer the lines are drawn on
	// is not scaled, so they have to be. Ports are looked up in the unscaled space the
	// nodes stand in, and the route is scaled on the way back out. Comparing the raw
	// numbers instead works at 100% and quietly stops working at every other zoom, which
	// on screen is edges snapping back to plain port-to-port lines as the graph is zoomed.
	const float scales[2] = { _zoom(), 1.0 };
	for (int i = 0; i < 2; i++) {
		float scale = scales[i];
		GraphNode *from = _node_at(p_from_position / scale, false);
		GraphNode *to = _node_at(p_to_position / scale, true);
		if (from == nullptr || to == nullptr || from == to) {
			continue;
		}
		PackedVector2Array points = _route(from, to).line;
		if (scale != 1.0) {
			for (int p = 0; p < points.size(); p++) {
				points.set(p, points[p] * scale);
			}
		}
		return points;
	}

	// Neither end is a port -- the minimap asks in its own coordinates, and nothing else
	// drags a connection now that the hotzones are refused. So this branch *is* the minimap,
	// and what it is handed back is a line of no length: the map keeps the nodes and loses
	// the edges. It could never draw the ones the graph draws -- it asks about points that
	// find no node here, so every route it got back was the plain port-to-port line this
	// editor exists to have taken apart, and a map drawn to different rules than the thing
	// it is a map of is read as a second picture rather than a smaller one.
	//
	// Two points and not none: an empty polyline is a thing the drawing side has to have
	// been written to expect, and this side cannot see whether it was.
	PackedVector2Array none;
	none.push_back(p_from_position);
	none.push_back(p_from_position);
	return none;
}

#endif // LIMBOAI_GDEXTENSION

void HSMGraphEdit::_bind_methods() {
	ADD_SIGNAL(MethodInfo("connect_to_requested",
			PropertyInfo(Variant::STRING_NAME, "from"),
			PropertyInfo(Variant::STRING_NAME, "to")));
	// The pair of nodes, not a row. Which of the rows between them was meant is the view's
	// to work out -- it is the only side that knows there is more than a line there.
	ADD_SIGNAL(MethodInfo("connection_picked",
			PropertyInfo(Variant::STRING_NAME, "from"),
			PropertyInfo(Variant::STRING_NAME, "to")));
	ADD_SIGNAL(MethodInfo("connection_menu_requested",
			PropertyInfo(Variant::STRING_NAME, "from"),
			PropertyInfo(Variant::STRING_NAME, "to")));
}

HSMGraphEdit::HSMGraphEdit() {
	overlay = memnew(Control);
	overlay->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	overlay->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	overlay->connect("draw", callable_mp(this, &HSMGraphEdit::_draw_overlay));
	add_child(overlay);

	connect("scroll_offset_changed", callable_mp(this, &HSMGraphEdit::_on_scroll_offset_changed));

	// Made by GraphEdit's constructor, which has already run. Without it there are no boxes
	// or heads, and the plain lines still work.
	lines_layer = Object::cast_to<Control>(get_node_or_null(NodePath("_connection_layer")));
	if (lines_layer != nullptr) {
		lines_layer->connect("draw", callable_mp(this, &HSMGraphEdit::_draw_under_lines));
		connect("node_selected", callable_mp(this, &HSMGraphEdit::_on_node_selection_changed));
		connect("node_deselected", callable_mp(this, &HSMGraphEdit::_on_node_selection_changed));
	}
}

#endif // ! TOOLS_ENABLED
