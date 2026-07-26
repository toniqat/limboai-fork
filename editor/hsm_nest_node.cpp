/**
 * hsm_nest_node.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#include "hsm_nest_node.h"

#ifdef LIMBOAI_GDEXTENSION

bool HSMNestNode::_has_point(const Vector2 &p_point) const {
	// The rect test is done here rather than left to the default: overriding the virtual
	// replaces it outright, so a node that only answered "not in the hole" would answer for
	// the entire graph.
	if (!Rect2(Vector2(), get_size()).has_point(p_point)) {
		return false;
	}
	return !hole.has_point(p_point);
}

#endif // LIMBOAI_GDEXTENSION

#endif // ! TOOLS_ENABLED
