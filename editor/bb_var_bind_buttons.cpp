/**
 * bb_var_bind_buttons.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#include "bb_var_bind_buttons.h"

#include "../compat/editor_scale.h"
#include "../compat/translation.h"
#include "../util/limbo_string_names.h"

#ifdef LIMBOAI_MODULE
#include "editor/editor_interface.h"
#include "scene/gui/popup_menu.h"
#include "scene/main/scene_tree.h"
#include "servers/display/display_server.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

String BBVarBindButtons::_binding_property() const {
	return "binding/" + String(var_name);
}

NodePath BBVarBindButtons::_binding() const {
	return plan.is_valid() && plan->has_property_binding(var_name) ? plan->get_property_binding(var_name) : NodePath();
}

// The node the binding path is relative to: the node being inspected (BTPlayer or BTState
// owning the plan). A plan opened on its own has none.
Node *BBVarBindButtons::_base_node() const {
	return Object::cast_to<Node>(EditorInterface::get_singleton()->get_inspector()->get_edited_object());
}

void BBVarBindButtons::_set_binding(const NodePath &p_path) {
	ERR_FAIL_NULL(property);
	property->emit_changed(_binding_property(), p_path);
}

void BBVarBindButtons::_update() {
	if (plan.is_null()) {
		return;
	}
	const bool mapped = plan->has_mapping(var_name);
	const bool bindable = !mapped && _base_node() != nullptr;
	const NodePath binding = _binding();
	bind_button->set_disabled(!bindable);
	bind_button->set_pressed_no_signal(!binding.is_empty());
	if (mapped) {
		bind_button->set_tooltip_text(TTR("Mapped to a parent scope variable; it cannot be bound."));
	} else if (!bindable) {
		bind_button->set_tooltip_text(TTR("Binding needs a scene node that owns this plan (BTPlayer or BTState)."));
	} else if (binding.is_empty()) {
		bind_button->set_tooltip_text(TTR("Bind to a property of a node..."));
	} else {
		bind_button->set_tooltip_text(vformat(TTR("Bound to %s"), String(binding)));
	}
	action_menu->set_disabled(mapped || (binding.is_empty() && !bindable));
	PopupMenu *popup = action_menu->get_popup();
	popup->set_item_disabled(popup->get_item_index(ACTION_CLEAR), binding.is_empty());
	popup->set_item_disabled(popup->get_item_index(ACTION_COPY), binding.is_empty());
	popup->set_item_disabled(popup->get_item_index(ACTION_EDIT), !bindable);
	popup->set_item_disabled(popup->get_item_index(ACTION_SELECT), binding.is_empty() || !bindable);
}

void BBVarBindButtons::_choose_property() {
	// The toggle look only shows the state; a press always opens the picker.
	bind_button->set_pressed_no_signal(!_binding().is_empty());
	if (_base_node() == nullptr) {
		return;
	}
	EditorInterface::get_singleton()->popup_node_selector(callable_mp(this, &BBVarBindButtons::_node_selected));
}

void BBVarBindButtons::_node_selected(const NodePath &p_path) {
	if (p_path.is_empty()) {
		return;
	}
	Node *selected_node = get_tree()->get_edited_scene_root()->get_node_or_null(p_path);
	ERR_FAIL_NULL(selected_node);
	PackedInt32Array valid_types;
	valid_types.append(plan->get_var(var_name).get_type());
	EditorInterface::get_singleton()->popup_property_selector(selected_node, callable_mp(this, &BBVarBindButtons::_property_selected).bind(p_path), valid_types);
}

void BBVarBindButtons::_property_selected(const NodePath &p_property_path, const NodePath &p_node_path) {
	if (p_property_path.is_empty()) {
		return;
	}
	Node *base_node = _base_node();
	ERR_FAIL_NULL(base_node);
	Node *selected_node = get_tree()->get_edited_scene_root()->get_node_or_null(p_node_path);
	ERR_FAIL_NULL(selected_node);
	_set_binding(String(base_node->get_path_to(selected_node)) + String(p_property_path));
}

void BBVarBindButtons::_action_selected(int p_id) {
	switch (p_id) {
		case ACTION_CLEAR: {
			_set_binding(NodePath());
		} break;
		case ACTION_COPY: {
			DisplayServer::get_singleton()->clipboard_set(String(_binding()));
		} break;
		case ACTION_EDIT: {
			bind_button->hide();
			action_menu->hide();
			path_edit->show();
			path_edit->set_text(String(_binding()));
			path_edit->grab_focus();
		} break;
		case ACTION_SELECT: {
			Node *base_node = _base_node();
			Node *bound_node = base_node ? base_node->get_node_or_null(NodePath(_binding().get_concatenated_names())) : nullptr;
			if (bound_node) {
				EditorInterface::get_singleton()->get_selection()->clear();
				EditorInterface::get_singleton()->get_selection()->add_node(bound_node);
			}
		} break;
	}
}

void BBVarBindButtons::_accept_text() {
	if (!path_edit->is_visible()) {
		return;
	}
	path_edit->hide();
	bind_button->show();
	action_menu->show();
	if (NodePath(path_edit->get_text()) != _binding()) {
		_set_binding(NodePath(path_edit->get_text()));
	}
}

void BBVarBindButtons::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_THEME_CHANGED: {
			bind_button->set_button_icon(get_theme_icon("Instance", LW_NAME(EditorIcons)));
			action_menu->set_button_icon(get_theme_icon(LW_NAME(GuiTabMenuHl), LW_NAME(EditorIcons)));
			PopupMenu *popup = action_menu->get_popup();
			popup->set_item_icon(popup->get_item_index(ACTION_CLEAR), get_theme_icon(LW_NAME(Clear), LW_NAME(EditorIcons)));
			popup->set_item_icon(popup->get_item_index(ACTION_COPY), get_theme_icon(LW_NAME(ActionCopy), LW_NAME(EditorIcons)));
			popup->set_item_icon(popup->get_item_index(ACTION_EDIT), get_theme_icon(LW_NAME(Edit), LW_NAME(EditorIcons)));
			popup->set_item_icon(popup->get_item_index(ACTION_SELECT), get_theme_icon(LW_NAME(ExternalLink), LW_NAME(EditorIcons)));
			_update();
		} break;
	}
}

void BBVarBindButtons::setup(const Ref<BlackboardPlan> &p_plan, const StringName &p_var_name, EditorProperty *p_property) {
	plan = p_plan;
	var_name = p_var_name;
	property = p_property;
	if (is_inside_tree()) {
		_update();
	}
}

BBVarBindButtons::BBVarBindButtons() {
	add_theme_constant_override(LW_NAME(separation), 0);

	bind_button = memnew(Button);
	add_child(bind_button);
	bind_button->set_flat(true);
	bind_button->set_toggle_mode(true);
	bind_button->set_focus_mode(FOCUS_NONE);
	bind_button->connect(LW_NAME(pressed), callable_mp(this, &BBVarBindButtons::_choose_property));

	path_edit = memnew(LineEdit);
	add_child(path_edit);
	path_edit->set_custom_minimum_size(Size2(200, 0) * EDSCALE);
	path_edit->set_placeholder(TTR("Node:property"));
	path_edit->connect(LW_NAME(focus_exited), callable_mp(this, &BBVarBindButtons::_accept_text));
	path_edit->connect(LW_NAME(text_submitted), callable_mp(this, &BBVarBindButtons::_accept_text).unbind(1));
	path_edit->hide();

	action_menu = memnew(MenuButton);
	add_child(action_menu);
	action_menu->set_flat(true);
	action_menu->set_focus_mode(FOCUS_NONE);
	action_menu->set_tooltip_text(TTR("Binding actions"));
	PopupMenu *popup = action_menu->get_popup();
	popup->add_item(TTR("Clear"), ACTION_CLEAR);
	popup->add_item(TTR("Copy as Text"), ACTION_COPY);
	popup->add_item(TTR("Edit"), ACTION_EDIT);
	popup->add_item(TTR("Show Node in Tree"), ACTION_SELECT);
	popup->connect(LW_NAME(id_pressed), callable_mp(this, &BBVarBindButtons::_action_selected));
}

#endif // TOOLS_ENABLED
