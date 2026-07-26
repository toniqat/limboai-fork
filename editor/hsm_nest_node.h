/**
 * hsm_nest_node.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#ifndef HSM_NEST_NODE_H
#define HSM_NEST_NODE_H

#ifdef LIMBOAI_MODULE
#include "scene/gui/graph_node.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/graph_node.hpp>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

// The node a machine is drawn as: a box with the states it holds standing inside it.
//
// Being a box is the whole difficulty. A GraphNode answers to the mouse over its entire
// rect, and this one is spread over everything it contains -- so it takes every press meant
// for a state inside it and every press meant for a line running between two of them. On
// screen that is a machine whose contents cannot be dragged, cannot be right-clicked, and
// whose edges cannot be picked; the box reads as a lid rather than as a backdrop.
//
// So the middle is cut out of what the node answers to. `hole` is a rect in the node's own
// coordinates where has_point() says no, and a control that says no is skipped by the
// viewport's search -- the press carries on to whatever is behind, which is the graph, which
// is what knows about the states and the edges standing there.
//
// What is left answering is the title bar and a band inside the other three edges. That is
// deliberately the same shape a window has: the title bar is where a thing this size is
// dragged from, and the band is where its resizer sits. The room the box leaves around its
// states is not part of it -- an edge that bows out of the way of a node runs through that
// room, and it has to stay pickable there.
class HSMNestNode : public GraphNode {
	GDCLASS(HSMNestNode, GraphNode);

private:
	Rect2 hole;

protected:
	static void _bind_methods() {}

public:
	void set_hole(const Rect2 &p_hole) { hole = p_hole; }
	Rect2 get_hole() const { return hole; }

#ifdef LIMBOAI_GDEXTENSION
	// Only the extension build overrides it, which is where this editor is built. The module
	// build reaches the same virtual under another name, and hsm_graph_edit.h already leaves
	// that half out for the same reason.
	virtual bool _has_point(const Vector2 &p_point) const override;
#endif // LIMBOAI_GDEXTENSION
};

#endif // HSM_NEST_NODE_H

#endif // ! TOOLS_ENABLED
