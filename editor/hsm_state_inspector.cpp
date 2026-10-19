/**
 * hsm_state_inspector.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#include "hsm_state_inspector.h"

#include "../compat/editor_scale.h"
#include "../compat/translation.h"
#include "../util/limbo_string_names.h"
#include "hsm_editor_view.h"

#ifdef LIMBOAI_MODULE
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#endif // LIMBOAI_GDEXTENSION

namespace {

// The same figure the graph dims inherited states to, so an inherited row reads the same
// way in both places: shown because the machine has it, dimmed because this file does not.
const float INHERITED_LINE_ALPHA = 0.55;

} // namespace

void EditorInspectorPluginHSMState::_on_line_pressed(int64_t p_machine_id, int p_row) {
	if (view == nullptr) {
		return;
	}
	// Deferred: selecting the row hands the Inspector another object, and the button that
	// was just pressed belongs to the Inspector being torn down for it.
	callable_mp(view, &HSMEditorView::select_transition).bind(p_machine_id, p_row).call_deferred();
}

#ifdef LIMBOAI_MODULE
bool EditorInspectorPluginHSMState::can_handle(Object *p_object) {
#elif LIMBOAI_GDEXTENSION
bool EditorInspectorPluginHSMState::_can_handle(Object *p_object) const {
#endif
	return view != nullptr && Object::cast_to<HSMStateSpec>(p_object) != nullptr;
}

#ifdef LIMBOAI_MODULE
void EditorInspectorPluginHSMState::parse_begin(Object *p_object) {
#elif LIMBOAI_GDEXTENSION
void EditorInspectorPluginHSMState::_parse_begin(Object *p_object) {
#endif
	Ref<HSMStateSpec> state = Object::cast_to<HSMStateSpec>(p_object);
	if (state.is_null() || view == nullptr) {
		return;
	}
	Dictionary info = view->describe_state_transitions(state);
	if (info.is_empty()) {
		// Not a state the open machine draws, or the root: the root is left by nothing and
		// entered by nothing, since there is no machine around it to own such a row.
		return;
	}

	VBoxContainer *box = memnew(VBoxContainer);

	Label *title = memnew(Label);
	title->set_text(TTR("Transitions"));
	title->set_theme_type_variation("HeaderSmall");
	box->add_child(title);

	struct Section {
		String caption;
		Array lines;
	};
	Section sections[2] = {
		{ TTR("Outgoing (evaluation order)"), info.get("outgoing", Array()) },
		{ TTR("Incoming"), info.get("incoming", Array()) },
	};

	for (int s = 0; s < 2; s++) {
		Label *caption = memnew(Label);
		caption->set_text(sections[s].caption);
		box->add_child(caption);

		if (sections[s].lines.is_empty()) {
			Label *none = memnew(Label);
			none->set_text(TTR("None"));
			none->set_modulate(Color(1, 1, 1, INHERITED_LINE_ALPHA));
			box->add_child(none);
			continue;
		}
		for (int i = 0; i < sections[s].lines.size(); i++) {
			Dictionary line = sections[s].lines[i];
			// A flat button rather than a label: it reads as a line of text, and a click on
			// it is the shortcut to the row itself. It edits nothing on its own.
			Button *btn = memnew(Button);
			btn->set_flat(true);
			btn->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
			btn->set_clip_text(true);
			btn->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			btn->set_text(line.get("text", String()));
			btn->set_tooltip_text(line.get("text", String()));
			if (bool(line.get("inherited", false))) {
				btn->set_modulate(Color(1, 1, 1, INHERITED_LINE_ALPHA));
			}
			int64_t machine_id = line.get("machine", 0);
			int row = line.get("row", -1);
			btn->connect(LW_NAME(pressed), callable_mp(this, &EditorInspectorPluginHSMState::_on_line_pressed).bind(machine_id, row));
			box->add_child(btn);
		}
	}

	add_custom_control(box);
}

#endif // TOOLS_ENABLED
