/**
 * bb_var_bind_buttons.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifndef BB_VAR_BIND_BUTTONS_H
#define BB_VAR_BIND_BUTTONS_H

#ifdef TOOLS_ENABLED

#include "../blackboard/blackboard_plan.h"

#ifdef LIMBOAI_MODULE
#include "editor/inspector/editor_inspector.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/menu_button.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/editor_property.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/menu_button.hpp>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

// Inline property binding controls at the right end of a plan variable's inspector row
// (they replace the separate "Binding" group of "Bind..." rows): a chain button that picks
// a node and one of its properties, and a vertical dots menu (Clear, Copy as Text, Edit,
// Show Node in Tree). Changes go through the row's EditorProperty as "binding/<var>", so
// the inspector records them with undo. Binding needs a scene node that owns the plan
// (BTPlayer, BTState); without one, and for mapped variables, the controls are disabled.
class BBVarBindButtons : public HBoxContainer {
	GDCLASS(BBVarBindButtons, HBoxContainer);

private:
	enum Action {
		ACTION_CLEAR,
		ACTION_COPY,
		ACTION_EDIT,
		ACTION_SELECT,
	};

	Ref<BlackboardPlan> plan;
	StringName var_name;
	EditorProperty *property = nullptr;

	Button *bind_button = nullptr;
	MenuButton *action_menu = nullptr;
	LineEdit *path_edit = nullptr;

	String _binding_property() const;
	NodePath _binding() const;
	Node *_base_node() const;
	void _set_binding(const NodePath &p_path);
	void _update();

	void _choose_property();
	void _node_selected(const NodePath &p_path);
	void _property_selected(const NodePath &p_property_path, const NodePath &p_node_path);
	void _action_selected(int p_id);
	void _accept_text();

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	void setup(const Ref<BlackboardPlan> &p_plan, const StringName &p_var_name, EditorProperty *p_property);

	BBVarBindButtons();
};

#endif // TOOLS_ENABLED

#endif // BB_VAR_BIND_BUTTONS_H
