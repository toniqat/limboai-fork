/**
 * hsm_state_inspector.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#ifndef HSM_STATE_INSPECTOR_H
#define HSM_STATE_INSPECTOR_H

#include "../hsm/hsm_resource.h"

#ifdef LIMBOAI_MODULE
#include "editor/inspector/editor_inspector.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/editor_inspector_plugin.hpp>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

class HSMEditorView;

// Adds a read-only "Transitions" section to the Inspector of a state picked in the HSM editor.
//
// The rows that decide where a state goes are not stored on the state: they belong to the
// machine around it, and some of them (ANYSTATE rows, rows from a base file) do not even name
// it. Conditions are not drawn on the graph either, so without this list the only way to read
// "what takes this state where, and in which order" would be to click every edge in turn.
//
// The list comes from the HSM editor view, which holds the merged tree the graph is drawn
// from. A state that is not on that graph (opened some other way) gets no section.
class EditorInspectorPluginHSMState : public EditorInspectorPlugin {
	GDCLASS(EditorInspectorPluginHSMState, EditorInspectorPlugin);

private:
	HSMEditorView *view = nullptr;

	void _on_line_pressed(int64_t p_machine_id, int p_row);

protected:
	static void _bind_methods() {}

public:
	void set_view(HSMEditorView *p_view) { view = p_view; }

#ifdef LIMBOAI_MODULE
	virtual bool can_handle(Object *p_object) override;
	virtual void parse_begin(Object *p_object) override;
#elif LIMBOAI_GDEXTENSION
	virtual bool _can_handle(Object *p_object) const override;
	virtual void _parse_begin(Object *p_object) override;
#endif
};

#endif // HSM_STATE_INSPECTOR_H

#endif // TOOLS_ENABLED
