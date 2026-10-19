/**
 * hsm_graph_edit.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#ifndef HSM_GRAPH_EDIT_H
#define HSM_GRAPH_EDIT_H

#ifdef LIMBOAI_MODULE
#include "core/input/input_event.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "scene/2d/node_2d.h"
#include "scene/gui/control.h"
#include "scene/gui/graph_edit.h"
#include "scene/gui/graph_node.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/graph_edit.hpp>
#include <godot_cpp/classes/graph_node.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/templates/hash_map.hpp>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

// A GraphEdit that draws a state machine rather than a signal flow.
//
// GraphEdit's default line runs right port -> left port, which reads well only when the
// graph flows one way. A state machine does not: nearly every pair of states goes both
// ways, so half the edges leave on the right and have to crawl back around the node to
// arrive on the left. With a dozen states those crawls overlap each other and the picture
// stops answering "which state becomes which".
//
// Three things change here.
//
//  * An edge leaves and arrives at the node's *border* -- the point where the line joining
//    the two centers crosses each rect. Direction is therefore position, and an edge that
//    goes back takes the same road as the one that came.
//  * It ends in an arrowhead. Without ports there is nothing else left to say which end is
//    the destination. The virtual hands back points and nothing else, so the line stops at
//    the head's base and the head itself is a filled triangle painted over the lines (on a
//    child of the lines' layer drawn after them), covering the line's end so the two read
//    as one stroke. (A polyline doubling back into wing, tip, wing broke apart at its
//    joints.)
//  * A machine's box is painted on the lines' layer itself, under the lines, and its node keeps
//    only the title bar opaque. Lines run over the box between the states inside it, and
//    under every title bar, so no edge is drawn through a header.
//  * A pair that goes both ways becomes one line with a head at each end. How many lines
//    are on screen is how readable the graph is, and half of them here are a second
//    direction. The two *connections* stay and are made to agree on the exact same line
//    instead, drawing on top of each other. Agreeing means neither end may decide anything
//    of its own, so the pair is put in name order first.
//
// And a line that would cross a node it does not belong to is bowed aside until it does
// not. Siblings stack in a column here, so a straight edge between two of them runs over
// everyone in between.
//
// The ports themselves are dead. Their dots are hidden by the view and their hotzones are
// refused here, so dragging near an edge always moves the node -- there is no invisible
// spot that answers to the mouse. Making an edge is a gesture between *nodes* instead:
// begin_connect_from() puts the graph in a mode where the line follows the cursor, whatever
// node it lands on is the destination, and hovering says up front whether that node can be
// one. That is also why the destination no longer has to be a port: a port could only be
// aimed at once it was visible, and it is not.
class HSMGraphEdit : public GraphEdit {
	GDCLASS(HSMGraphEdit, GraphEdit);

private:
	// Drawn on top of the nodes, so what is drawn over them is not buried under them. A
	// plain Control rather than a class of its own -- it holds nothing, and the view only
	// ever deletes GraphNode children, so it survives a rebuild.
	//
	// Two things ride on it: the line being aimed, and the ring around whatever a running
	// machine is standing in. One layer for both because the two are the same kind of thing --
	// something drawn about a node rather than by it -- and they are never on at once anyway,
	// the second only ever appearing on a read-only view.
	Control *overlay = nullptr;
	// GraphEdit's own `_connection_layer`, where its lines are Line2D children. Drawing on it
	// lands under them, and it is redrawn exactly when they are rebuilt.
	Control *lines_layer = nullptr;
	// The lines layer's internal child at the back, drawn after its Line2D children: the
	// heads go here, over the lines and still under every node. A Node2D, not a Control: a
	// Control is culled by its own rect, and a zero-size one at the graph origin made every
	// head vanish once that origin was scrolled out of view (easily, zoomed in).
	Node2D *heads_layer = nullptr;
	// What set_connection_activity() was last told, per from/to pair, so a head is tinted the
	// way its line is. GraphEdit keeps the amount but does not hand it back.
	HashMap<String, float> activity;
	bool connecting = false;
	StringName connect_from;
	Vector<StringName> connect_targets;
	StringName hovered;

	// The picked edge, as the pair of nodes it runs between. Not a row: one line can stand
	// for several transitions -- both directions of a pair are drawn on top of each other,
	// and two states may be joined by more than one event -- so which row was meant is a
	// question only the view can answer, and it is asked once the click has been placed.
	StringName picked_from;
	StringName picked_to;

	// The nodes a running machine is standing in, outermost first. Names and not nodes: a
	// rebuild throws every node away, and a pointer kept here would be drawn a ring around
	// after it had been deleted.
	Vector<StringName> lit;
	bool _pick_connection_at(const Vector2 &p_position);
	void _clear_pick();
	void _mark_pick(float p_activity);

	// Nothing is cached. A route is chosen against where every *other* node stands, so a
	// cache would have to be dropped whenever any node moves -- and a redraw that lands
	// mid-drag would still hand back the stale one. Re-walking a dozen rects per edge is
	// cheaper than being wrong about which of the two it is.
	GraphNode *_node_at(const Vector2 &p_position, bool p_input) const;
	static Vector2 _edge_point(const Rect2 &p_rect, const Vector2 &p_dir);
	static bool _segment_hits(const Vector2 &p_a, const Vector2 &p_b, const Rect2 &p_rect);
	static Rect2 _rect_of(GraphNode *p_node);
	static void _curve(const Rect2 &p_from, const Rect2 &p_to, const Vector2 &p_ctrl, Vector2 &r_p0, Vector2 &r_ctrl, Vector2 &r_p1);
	static int _hits(const Rect2 &p_from, const Rect2 &p_to, const Vector2 &p_ctrl, const Vector<Rect2> &p_blockers);
	// The line plus where its heads go. `tip` is at the destination, `tail` (only when the
	// pair goes both ways) at the source; each `*_out` points away from its node.
	struct Route {
		PackedVector2Array line;
		GraphNode *tip_node = nullptr;
		Vector2 tip;
		Vector2 tip_out;
		GraphNode *tail_node = nullptr;
		Vector2 tail;
		Vector2 tail_out;
	};
	static PackedVector2Array _head(const Vector2 &p_tip, const Vector2 &p_out, float p_scale);
	// Where the line stops short of a head's tip: inside the triangle, so the head covers it.
	static Vector2 _head_base(const Vector2 &p_tip, const Vector2 &p_out, float p_scale);
	static void _draw_head(CanvasItem *p_canvas, const PackedVector2Array &p_head, const Color &p_color);
	static float _title_height(GraphNode *p_node);
	Route _route(GraphNode *p_from, GraphNode *p_to) const;
	void _draw_under_lines();
	void _draw_heads();
	void _on_node_selection_changed(Node *p_node);

	float _zoom() const;
	// Where a node actually sits on screen. Its own rect is unscaled -- GraphEdit zooms a
	// node by scaling it -- so the size has to be multiplied to match the position.
	Rect2 _screen_rect_of(GraphNode *p_node) const;
	GraphNode *_node_under(const Vector2 &p_local) const;
	bool _is_target(const StringName &p_name) const;
	void _update_hover();
	void _draw_overlay();
	void _draw_lit();
	void _draw_pending();
	void _stop_connecting();
	// The ring is laid down in screen coordinates, so it has to be laid down again whenever
	// the nodes move under it. Panning and zooming both arrive here.
	void _on_scroll_offset_changed(const Vector2 &p_offset);

protected:
	static void _bind_methods();

public:
#ifdef LIMBOAI_GDEXTENSION
	virtual PackedVector2Array _get_connection_line(const Vector2 &p_from_position, const Vector2 &p_to_position) const override;
	// Refused, both of them. The dots are hidden, and a hotzone that is not drawn is a
	// place the mouse behaves differently for no reason the screen can give.
	virtual bool _is_in_input_hotzone(Object *, int32_t, const Vector2 &) override { return false; }
	virtual bool _is_in_output_hotzone(Object *, int32_t, const Vector2 &) override { return false; }
	// Not _gui_input: while the cursor is over a GraphNode the node takes the event and the
	// graph never sees the motion, which is exactly when the highlight has to move.
	virtual void _input(const Ref<InputEvent> &p_event) override;
	// This one *is* _gui_input: picking an edge is about a press that landed on the graph
	// itself. It runs before GraphEdit's own handler, so an edge that was hit can be taken
	// before the graph reads the same press as the start of a box selection.
	virtual void _gui_input(const Ref<InputEvent> &p_event) override;
#endif // LIMBOAI_GDEXTENSION

	// p_targets is the whole answer to "can this node be the destination" -- the graph is
	// told, it does not work it out. Which states may be joined is the machine's business
	// and lives with the view.
	void begin_connect_from(const StringName &p_from, const Vector<StringName> &p_targets);
	void cancel_connect();
	bool is_connecting() const { return connecting; }

	// Dropped without a signal: every caller of it is already about to redraw the graph.
	void clear_picked_connection() { _clear_pick(); }
	// clear_connections() plus the activity kept for the heads, which would otherwise light a
	// head whose fresh line is plain.
	void clear_edges();
	// set_connection_activity() that the heads follow too. Callers use this one.
	void set_edge_activity(const StringName &p_from, const StringName &p_to, float p_amount);
	// The lines' layer goes below every node, machine boxes included. The view calls it after
	// it has put the boxes at the back.
	void settle_layers();
	// Picks the line between two nodes as if it had been clicked, without the signal: the
	// caller is the one deciding what to inspect. Does nothing when no line joins the two
	// (a self-loop is never drawn, so it has nothing to pick).
	void pick_connection(const StringName &p_from, const StringName &p_to);

	// Every node on the chain and not only the innermost, because a machine is one of them:
	// a state standing inside `Combat` is two answers to "where is it", and a ring around
	// only the inner one leaves the box it is in reading like any other box on the graph.
	void set_lit_nodes(const Vector<StringName> &p_names);
	// For whoever moved the nodes without the graph hearing of it -- the view's own fit does
	// exactly that, deferred, after every rebuild.
	void redraw_overlay();

	HSMGraphEdit();
};

#endif // HSM_GRAPH_EDIT_H

#endif // ! TOOLS_ENABLED
