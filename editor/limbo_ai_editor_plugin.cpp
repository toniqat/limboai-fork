/**
 * limbo_ai_editor_plugin.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#include "limbo_ai_editor_plugin.h"

#include "../bt/behavior_tree.h"
#include "../bt/tasks/bt_comment.h"
#include "../bt/tasks/composites/bt_probability_selector.h"
#include "../bt/tasks/composites/bt_selector.h"
#include "../bt/tasks/decorators/bt_subtree.h"
#include "../compat/editor.h"
#include "../compat/editor_scale.h"
#include "../compat/editor_settings.h"
#include "../compat/limbo_compat.h"
#include "../compat/object.h"
#include "../compat/project_settings.h"
#include "../compat/resource.h"
#include "../compat/resource_loader.h"
#include "../compat/scene_tree.h"
#include "../compat/translation.h"
#include "../compat/variant.h"
#include "../util/limbo_utility.h"
#include "../util/limboai_version.h"
#include "action_banner.h"
#include "blackboard_plan_editor.h"
#include "debugger/limbo_debugger_plugin.h"
#include "editor_property_bb_param.h"
#include "editor_property_property_path.h"
#include "editor_property_variable_name.h"
#include "hsm_state_inspector.h"

#ifdef LIMBOAI_MODULE
#include "core/input/input.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/docks/filesystem_dock.h"
#include "editor/editor_node.h"
#include "editor/gui/editor_bottom_panel.h"
#include "editor/script/script_editor_plugin.h"
#include "editor/settings/project_settings_editor.h"
#include "scene/gui/scroll_bar.h"
#include "scene/gui/separator.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/image_texture.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include "godot_cpp/variant/dictionary.hpp"
#include <godot_cpp/classes/button_group.hpp>
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_inspector.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/file_system_dock.hpp>
#include <godot_cpp/classes/h_scroll_bar.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/v_separator.hpp>

using namespace godot;
#endif // LIMBOAI_GDEXTENSION

namespace {

// If built-in resource - switch to the owner scene (open it if not already).
inline void _switch_to_owner_scene_if_builtin(const Ref<BehaviorTree> &p_behavior_tree) {
	if (p_behavior_tree.is_valid() && p_behavior_tree->get_path().contains("::")) {
		String current_scene = SCENE_TREE()->get_edited_scene_root()->get_scene_file_path();
		String scene_path = p_behavior_tree->get_path().get_slice("::", 0);
		if (current_scene != scene_path) {
			EditorInterface::get_singleton()->open_scene_from_path(scene_path);
		}
	}
}

} // unnamed namespace

//**** LimboAIEditor

_FORCE_INLINE_ String _get_script_template_path() {
	String templates_search_path = GLOBAL_GET("editor/script/templates_search_path");
	return templates_search_path.path_join("BTTask").path_join("custom_task.gd");
}

EditorUndoRedoManager *LimboAIEditor::_new_undo_redo_action(const String &p_name, UndoRedo::MergeMode p_mode) {
#ifdef LIMBOAI_MODULE
	EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
#elif LIMBOAI_GDEXTENSION
	EditorUndoRedoManager *undo_redo = plugin->get_undo_redo();
#endif
	// ! HACK: Force global history to be used for resources without a set path.
	undo_redo->create_action(p_name, p_mode, dummy_history_context);
	undo_redo->force_fixed_history();
	return undo_redo;
}

void LimboAIEditor::_commit_action_with_update(EditorUndoRedoManager *p_undo_redo) {
	ERR_FAIL_NULL(p_undo_redo);
	p_undo_redo->add_do_method(this, LW_NAME(_update_task_tree), task_tree->get_bt());
	p_undo_redo->add_undo_method(this, LW_NAME(_update_task_tree), task_tree->get_bt());
	p_undo_redo->commit_action();
	_set_as_dirty(task_tree->get_bt(), true);
}

void LimboAIEditor::_add_task(const Ref<BTTask> &p_task, bool p_as_sibling) {
	if (task_tree->get_bt().is_null()) {
		return;
	}
	ERR_FAIL_COND(p_task.is_null());

	EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Add BT Task"));

	int insert_idx = -1;
	Ref<BTTask> selected = task_tree->get_selected();
	Ref<BTTask> parent = selected;
	if (parent.is_null()) {
		// When no task is selected, use the root task.
		parent = task_tree->get_bt()->get_root_task();
		selected = parent;
	}
	if (parent.is_null()) {
		// When tree is empty.
		undo_redo->add_do_method(task_tree->get_bt().ptr(), LW_NAME(set_root_task), p_task);
		undo_redo->add_undo_method(task_tree->get_bt().ptr(), LW_NAME(set_root_task), task_tree->get_bt()->get_root_task());
	} else {
		if (p_as_sibling && selected.is_valid() && selected->get_parent().is_valid()) {
			// Insert task after the currently selected and on the same level (usually when shift is pressed).
			parent = selected->get_parent();
			insert_idx = selected->get_index() + 1;
		}
		undo_redo->add_do_method(parent.ptr(), LW_NAME(add_child_at_index), p_task, insert_idx);
		undo_redo->add_undo_method(parent.ptr(), LW_NAME(remove_child), p_task);
	}
	_commit_action_with_update(undo_redo);
}

void LimboAIEditor::_add_task_with_prototype(const Ref<BTTask> &p_prototype) {
	Ref<BTTask> selected = task_tree->get_selected();
	bool as_sibling = Input::get_singleton()->is_key_pressed(LW_KEY(SHIFT));
	_add_task(p_prototype->clone(), as_sibling);
}

Ref<BTTask> LimboAIEditor::_create_task_by_class_or_path(const String &p_class_or_path) const {
	ERR_FAIL_COND_V(p_class_or_path.is_empty(), nullptr);

	Ref<BTTask> ret;

	if (p_class_or_path.begins_with("res:")) {
		Ref<Script> s = RESOURCE_LOAD(p_class_or_path, "Script");
		ERR_FAIL_COND_V_MSG(s.is_null(), nullptr, vformat("LimboAI: Can't add task. Bad script: %s", p_class_or_path));
		StringName base_type = s->get_instance_base_type();
		if (base_type == StringName()) {
			// Try reloading script.
			s->reload(true);
			base_type = s->get_instance_base_type();
		}
		ERR_FAIL_COND_V_MSG(base_type == StringName(), nullptr, vformat("LimboAI: Can't add task. Bad script: %s", p_class_or_path));

		Variant inst = ClassDB::instantiate(base_type);
		Object *obj = inst;
		ERR_FAIL_NULL_V_MSG(obj, nullptr, vformat("LimboAI: Can't add task. Failed to create base type \"%s\".", base_type));

		if (unlikely(!IS_CLASS(obj, BTTask))) {
			ERR_PRINT_ED(vformat("LimboAI: Can't add task. Script is not a BTTask: %s", p_class_or_path));
			VARIANT_DELETE_IF_OBJECT(inst);
			return nullptr;
		}

		ret.reference_ptr(Object::cast_to<BTTask>(obj));
		ret->set_script(s);
	} else {
		ERR_FAIL_COND_V(!ClassDB::is_parent_class(p_class_or_path, "BTTask"), nullptr);
		ret = ClassDB::instantiate(p_class_or_path);
	}
	return ret;
}

void LimboAIEditor::_add_task_by_class_or_path(const String &p_class_or_path) {
	Ref<BTTask> selected = task_tree->get_selected();
	bool as_sibling = Input::get_singleton()->is_key_pressed(LW_KEY(SHIFT));
	_add_task(_create_task_by_class_or_path(p_class_or_path), as_sibling);
}

void LimboAIEditor::_on_palette_task_dropped(const String &p_task_meta, Ref<BTTask> p_to_task, int p_to_pos) {
	Ref<BehaviorTree> bt = task_tree->get_bt();
	if (bt.is_null()) {
		return;
	}
	Ref<BTTask> task = _create_task_by_class_or_path(p_task_meta);
	ERR_FAIL_COND(task.is_null());

	EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Add BT Task"));
	if (p_to_task.is_null()) {
		// Dropped on the file item of an empty tree.
		ERR_FAIL_COND(bt->get_root_task().is_valid());
		undo_redo->add_do_method(bt.ptr(), LW_NAME(set_root_task), task);
		undo_redo->add_undo_method(bt.ptr(), LW_NAME(set_root_task), bt->get_root_task());
	} else {
		undo_redo->add_do_method(p_to_task.ptr(), LW_NAME(add_child_at_index), task, p_to_pos);
		undo_redo->add_undo_method(p_to_task.ptr(), LW_NAME(remove_child), task);
	}
	_commit_action_with_update(undo_redo);
}

void LimboAIEditor::_remove_task(const Ref<BTTask> &p_task) {
	ERR_FAIL_COND(p_task.is_null());
	ERR_FAIL_COND(task_tree->get_bt().is_null());
	EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Remove BT Task"));
	if (p_task->get_parent().is_null()) {
		ERR_FAIL_COND(task_tree->get_bt()->get_root_task() != p_task);
		undo_redo->add_do_method(task_tree->get_bt().ptr(), LW_NAME(set_root_task), Variant());
		undo_redo->add_undo_method(task_tree->get_bt().ptr(), LW_NAME(set_root_task), task_tree->get_bt()->get_root_task());
	} else {
		undo_redo->add_do_method(p_task->get_parent().ptr(), LW_NAME(remove_child), p_task);
		undo_redo->add_undo_method(p_task->get_parent().ptr(), LW_NAME(add_child_at_index), p_task, p_task->get_index());
	}
	_commit_action_with_update(undo_redo);
}

void LimboAIEditor::_new_bt() {
	Ref<BehaviorTree> bt = memnew(BehaviorTree);
	bt->set_root_task(memnew(BTSelector));
	bt->set_blackboard_plan(memnew(BlackboardPlan));
	EditorInterface::get_singleton()->edit_resource(bt);
}

void LimboAIEditor::_new_hsm() {
	Ref<HierarchicalStateMachine> hsm;
	hsm.instantiate();
	// The root state is not made here. HSMEditorView::edit_hsm() gives a machine without one
	// a root, because a file dragged in from the FileSystem dock arrives without one too --
	// filling it in on both paths would be the same rule written twice.
	EditorInterface::get_singleton()->edit_resource(hsm);
}

void LimboAIEditor::_on_new_pressed() {
	if (toolbar_mode == TOOLBAR_HSM) {
		_new_hsm();
	} else {
		_new_bt();
	}
}

void LimboAIEditor::_save_resource(const Ref<Resource> &p_res, const String &p_path) {
	ERR_FAIL_COND(p_path.is_empty());
	ERR_FAIL_COND(!p_path.begins_with("res://"));
	ERR_FAIL_COND(p_res.is_null());

	if (p_res->get_path() != p_path) {
#ifdef LIMBOAI_MODULE
		p_res->set_path(p_path, true);
#elif LIMBOAI_GDEXTENSION
		p_res->take_over_path(p_path);
#endif
	}

	// This is a workaround, because EditorNode::save_resource() function is not accessible in GDExtension.
	if (RESOURCE_IS_BUILT_IN(p_res)) {
		// If built-in resource - save the containing resource instead.
		String file_path = p_path.get_slice("::", 0);
		ERR_FAIL_COND_MSG(!RESOURCE_EXISTS(file_path, "Resource"), "LimboAI: SAVE FAILED - resource file doesn't exist: " + file_path);
		if (RESOURCE_IS_SCENE_FILE(file_path)) {
			// Packed scene - save the scene instead.
			if (EditorInterface::get_singleton()->get_open_scenes().has(file_path)) {
				// If scene is open, switch to it first, and then ask to save.
				// This is needed because saving the currently edited scene can have complications.
				EditorInterface::get_singleton()->open_scene_from_path(file_path);
				EditorInterface::get_singleton()->save_scene();
			} else {
				// If scene is not currently open in the editor, load and resave it.
				Ref<Resource> scene = RESOURCE_LOAD(file_path, "PackedScene");
				RESOURCE_SAVE(scene, file_path, ResourceSaver::FLAG_NONE);
			}
		} else {
			// Not a packed scene - save the containing resource to file.
			Ref<Resource> res = RESOURCE_LOAD(file_path, "Resource");
			RESOURCE_SAVE(res, file_path, ResourceSaver::FLAG_NONE);
		}
	} else {
		// If external resource - save to file.
		RESOURCE_SAVE(p_res, p_path, ResourceSaver::FLAG_CHANGE_PATH);
	}

	_set_as_dirty(p_res, false);
	_update_tabs();
}

void LimboAIEditor::_save_current_bt(const String &p_path) {
	ERR_FAIL_COND_MSG(p_path.is_empty(), "LimboAI: SAVE FAILED - p_path is empty");
	ERR_FAIL_COND(idx_history < 0 || idx_history >= history.size());

	_save_resource(history[idx_history], p_path);
}

void LimboAIEditor::_load_resource(const String &p_path) {
	ERR_FAIL_COND_MSG(p_path.is_empty(), "Empty p_path");
	Ref<Resource> res = RESOURCE_LOAD(p_path, "Resource");
	ERR_FAIL_COND(res.is_null());

	BehaviorTree *bt = Object::cast_to<BehaviorTree>(res.ptr());
	if (bt != nullptr) {
		if (bt->get_blackboard_plan().is_null()) {
			bt->set_blackboard_plan(memnew(BlackboardPlan));
		}
	} else if (Object::cast_to<HierarchicalStateMachine>(res.ptr()) == nullptr) {
		// Refused here rather than filtered in the dialog: both kinds live in .tres, so a
		// filter cannot tell them apart, and the third thing a .tres can be has to be turned
		// away by something that has actually looked at it.
		_popup_info_dialog(vformat(TTR("Not a behavior tree or a state machine: %s"), p_path));
		return;
	}

	EditorInterface::get_singleton()->edit_resource(res);
}

void LimboAIEditor::_update_task_tree(const Ref<BehaviorTree> &p_bt, const Ref<BTTask> &p_specific_task) {
	ERR_FAIL_COND(p_bt.is_null());
	if (task_tree->get_bt() == p_bt) {
		if (p_specific_task.is_null()) {
			task_tree->update_tree();
		} else {
			task_tree->update_task(p_specific_task);
		}
	} else {
		// The given BT is not being displayed - open it.
		edit_bt(p_bt);
	}
}

// `hsc` itself is never hidden by either of these. It carries the toolbar and the tab bar in
// the widescreen layout, and those two have to stand whatever is open -- with nothing open
// most of all, since the tab bar is how something gets opened again.
void LimboAIEditor::_disable_editing() {
	task_tree->unload();
	_show_all_tab();
}

void LimboAIEditor::_show_all_tab() {
	// Kept for when the tree's tab comes back: the tree stays loaded, but edit_bt() only
	// remembers the search of the tab it is leaving, and this leaves through no tab.
	if (task_tree->get_bt().is_valid() && history.has(task_tree->get_bt())) {
		tab_search_context.insert(task_tree->get_bt(), task_tree->tree_search_get_search_info());
	}
	idx_history = -1;
	palette_holder->hide();
	task_tree->hide();
	hsm_view->hide();
	browser->show();
	_update_toolbar(TOOLBAR_NONE);
}

void LimboAIEditor::_show_editor_for(const Ref<Resource> &p_res) {
	bool is_hsm = Object::cast_to<HierarchicalStateMachine>(p_res.ptr()) != nullptr;

	browser->hide();
	hsm_view->set_visible(is_hsm);
	task_tree->set_visible(!is_hsm);
	// The palette builds trees, so it goes with the tree rather than staying as a second
	// pane the machine would then have to share the width with.
	palette_holder->set_visible(!is_hsm);

	_update_toolbar(is_hsm ? TOOLBAR_HSM : TOOLBAR_BT);
}

void LimboAIEditor::_update_toolbar(ToolbarMode p_mode) {
	toolbar_mode = p_mode;

	fav_tasks_hbox->set_visible(p_mode == TOOLBAR_BT);
	hsm_tools_hbox->set_visible(p_mode == TOOLBAR_HSM);
	new_script_btn->set_visible(p_mode == TOOLBAR_BT);
	// Save is about the open document; with none there is nothing for it to mean. New and
	// Load stay in all three states -- they are how a document comes to be open at all, and
	// the empty screen's own hint points at them.
	save_btn->set_visible(p_mode != TOOLBAR_NONE);
	blackboard_btn->set_visible(p_mode == TOOLBAR_BT);

	docs_sep->set_visible(p_mode != TOOLBAR_NONE);
	script_sep->set_visible(p_mode == TOOLBAR_BT);

	switch (p_mode) {
		case TOOLBAR_HSM: {
			new_btn->set_text(TTR("New State Machine"));
			new_btn->set_tooltip_text(TTR("Create a new state machine."));
			// Swapped rather than left alone: a Button puts its shortcut's *name* in its
			// tooltip, so keeping the tree's would read "New Behavior Tree" on a button that
			// makes a machine. The two carry the same keys, and only one is ever on the
			// button, so they never compete.
			new_btn->set_shortcut(LW_GET_SHORTCUT("limbo_ai/new_state_machine"));
			save_dialog->set_title(TTR("Save State Machine"));
		} break;
		case TOOLBAR_BT: {
			new_btn->set_text(TTR("New Behavior Tree"));
			new_btn->set_tooltip_text(TTR("Create a new behavior tree."));
			new_btn->set_shortcut(LW_GET_SHORTCUT("limbo_ai/new_behavior_tree"));
			save_dialog->set_title(TTR("Save Behavior Tree"));
		} break;
		default: {
			// Nothing open, so nothing to name it after. Whichever kind the button then makes
			// is a guess, and the tree is the one the hint in the middle of the screen asks for.
			new_btn->set_text(TTR("New"));
			new_btn->set_tooltip_text(TTR("Create a new behavior tree."));
			new_btn->set_shortcut(LW_GET_SHORTCUT("limbo_ai/new_behavior_tree"));
			save_dialog->set_title(TTR("Save Behavior Tree"));
		} break;
	}
}

void LimboAIEditor::_on_hsm_edited() {
	Ref<Resource> edited = hsm_view->get_edited_hsm();
	if (edited.is_valid()) {
		_set_as_dirty(edited, true);
		_update_tabs();
	}
}

void LimboAIEditor::edit_hsm(const Ref<HierarchicalStateMachine> &p_hsm, bool p_force_refresh) {
	ERR_FAIL_COND_MSG(p_hsm.is_null(), "p_hsm is null");

	// What the guard spares is the reload, and nothing else. The tab bar spans both kinds of
	// document, so this view can already be holding the machine while the *other* half of the
	// screen is the one on show -- and then clicking its tab has to bring this half back.
	// Returning early here left the screen on the tree and the tab bar on the machine.
	bool reload = p_force_refresh || hsm_view->get_edited_hsm() != p_hsm;

	if (reload) {
		hsm_view->edit_hsm(p_hsm);
	}

	if (!p_hsm->is_connected(LW_NAME(changed), callable_mp(this, &LimboAIEditor::_set_as_dirty))) {
		p_hsm->connect(LW_NAME(changed), callable_mp(this, &LimboAIEditor::_set_as_dirty).bind(p_hsm, true));
	}

	int idx = history.find(p_hsm);
	if (idx != -1) {
		idx_history = idx;
	} else {
		history.push_back(p_hsm);
		idx_history = history.size() - 1;
	}

	_show_editor_for(p_hsm);
	_update_tabs();
}

void LimboAIEditor::edit_bt(const Ref<BehaviorTree> &p_behavior_tree, bool p_force_refresh) {
	ERR_FAIL_COND_MSG(p_behavior_tree.is_null(), "p_behavior_tree is null");

	_switch_to_owner_scene_if_builtin(p_behavior_tree);

	// Same as edit_hsm(): a tab whose tree is still loaded has to bring its half of the screen
	// back, and only the reload below is worth skipping.
	bool reload = p_force_refresh || task_tree->get_bt() != p_behavior_tree;

	if (reload) {
#ifdef LIMBOAI_MODULE
		p_behavior_tree->editor_set_section_unfold("blackboard_plan", true);
		p_behavior_tree->notify_property_list_changed();
#endif // LIMBOAI_MODULE
		// Remember current search info. Asked of the loaded tree rather than of the active
		// tab: coming from the All tab, there is no active tab, but the tree is still loaded.
		if (task_tree->get_bt().is_valid() && history.has(task_tree->get_bt())) {
			tab_search_context.insert(task_tree->get_bt(), task_tree->tree_search_get_search_info());
		}

		task_tree->load_bt(p_behavior_tree);

		if (task_tree->get_bt().is_valid() && !task_tree->get_bt()->is_connected(LW_NAME(changed), callable_mp(this, &LimboAIEditor::_set_as_dirty))) {
			task_tree->get_bt()->connect(LW_NAME(changed), callable_mp(this, &LimboAIEditor::_set_as_dirty).bind(task_tree->get_bt(), true));
		}
	}

	int idx = history.find(p_behavior_tree);
	if (idx != -1) {
		idx_history = idx;
	} else {
		history.push_back(p_behavior_tree);
		idx_history = history.size() - 1;
	}

	_show_editor_for(p_behavior_tree);

	// Restore search info from [tab_search_context]. Only on a reload: without one the panel
	// is already showing this tab's search, and putting it back would be putting back what
	// was saved the last time the tab was left rather than what is in it now.
	if (reload && idx_history >= 0 && idx_history < history.size()) {
		if (tab_search_context.has(history[idx_history])) {
			task_tree->tree_search_set_search_info(tab_search_context[history[idx_history]]);
		} else {
			task_tree->tree_search_set_search_info(TreeSearch::SearchInfo());
		}
	}

	_update_tabs();
}

Ref<BlackboardPlan> LimboAIEditor::get_edited_blackboard_plan() {
	if (task_tree->get_bt().is_null()) {
		return nullptr;
	}
	if (task_tree->get_bt()->get_blackboard_plan().is_null()) {
		task_tree->get_bt()->set_blackboard_plan(memnew(BlackboardPlan));
	}
	return task_tree->get_bt()->get_blackboard_plan();
}

void LimboAIEditor::set_window_layout(const Ref<ConfigFile> &p_configuration) {
	Array open_bts;
	open_bts = p_configuration->get_value("LimboAI", "bteditor_open_bts", open_bts);
	for (int i = 0; i < open_bts.size(); i++) {
		String path = open_bts[i];
		if (!FILE_EXISTS(path)) {
			continue;
		}
		// A reopened tab may be either kind: the layout only remembers paths. The type is
		// looked at here rather than left to _load_resource(), which would put a dialog on
		// screen at startup over a file nobody asked for just now.
		Ref<Resource> res = RESOURCE_LOAD(path, "Resource");
		if (res.is_null()) {
			continue;
		}
		if (Object::cast_to<HierarchicalStateMachine>(res.ptr()) != nullptr || Object::cast_to<BehaviorTree>(res.ptr()) != nullptr) {
			_load_resource(path);
		}
	}

	hsc->set_split_offset(p_configuration->get_value("LimboAI", "bteditor_hsplit", hsc->get_split_offset()));
	browser->set_split_offset(p_configuration->get_value("LimboAI", "bteditor_browser_hsplit", browser->get_split_offset()));
}

void LimboAIEditor::get_window_layout(const Ref<ConfigFile> &p_configuration) {
	Array open_bts;
	for (const Ref<Resource> &res : history) {
		open_bts.push_back(res->get_path());
	}
	p_configuration->set_value("LimboAI", "bteditor_open_bts", open_bts);

	int split_offset = hsc->get_split_offset();
	if (editor_layout != (int)EDITOR_GET("limbo_ai/editor/layout")) {
		// Editor layout settings changed - flip split offset.
		split_offset *= -1;
	}
	p_configuration->set_value("LimboAI", "bteditor_hsplit", split_offset);
	p_configuration->set_value("LimboAI", "bteditor_browser_hsplit", browser->get_split_offset());
}

void LimboAIEditor::_set_as_dirty(const Ref<Resource> &p_res, bool p_dirty) {
	if (p_dirty && !dirty.has(p_res)) {
		dirty.insert(p_res);
	} else if (p_dirty == false && dirty.has(p_res)) {
		dirty.erase(p_res);
	}
}

void LimboAIEditor::_create_user_task_dir(String task_dir) {
	ERR_FAIL_COND_MSG(DirAccess::dir_exists_absolute(task_dir), "LimboAIEditor: Directory already exists: " + task_dir);

	Error err = DirAccess::make_dir_recursive_absolute(task_dir);
	ERR_FAIL_COND_MSG(err != OK, "LimboAIEditor: Failed to create directory: " + task_dir);

#ifdef LIMBOAI_MODULE
	EditorFileSystem::get_singleton()->scan_changes();
#elif LIMBOAI_GDEXTENSION
	EditorInterface::get_singleton()->get_resource_filesystem()->scan_sources();
#endif
	_update_banners();
}

void LimboAIEditor::_edit_project_settings() {
#ifdef LIMBOAI_MODULE
	ProjectSettingsEditor::get_singleton()->set_general_page("limbo_ai/behavior_tree");
	ProjectSettingsEditor::get_singleton()->popup_project_settings();
	ProjectSettingsEditor::get_singleton()->connect(LW_NAME(visibility_changed), callable_mp(this, &LimboAIEditor::_update_banners), CONNECT_ONE_SHOT);
#elif LIMBOAI_GDEXTENSION
	// TODO: Find a way to show project setting in GDExtension.
	String text = "Can't open settings in GDExtension, sorry :(\n"
				  "To edit project settings, navigate to \"Project->Project Settings\",\n"
				  "enable \"Advanced settings\", and scroll down to the \"LimboAI\" section.";
	_popup_info_dialog(text);
#endif
}

void LimboAIEditor::_remove_task_from_favorite(const String &p_task) {
	PackedStringArray favorite_tasks = GLOBAL_GET("limbo_ai/behavior_tree/favorite_tasks");
	int idx = favorite_tasks.find(p_task);
	if (idx >= 0) {
		favorite_tasks.remove_at(idx);
	}
	ProjectSettings::get_singleton()->set_setting("limbo_ai/behavior_tree/favorite_tasks", favorite_tasks);
	ProjectSettings::get_singleton()->save();
}

void LimboAIEditor::_save_and_restart() {
	ProjectSettings::get_singleton()->save();
	EditorInterface::get_singleton()->save_all_scenes();
	EditorInterface::get_singleton()->restart_editor(true);
}

void LimboAIEditor::_extract_subtree(const String &p_path) {
	Ref<BTTask> selected = task_tree->get_selected();
	ERR_FAIL_COND(selected.is_null());

	EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Extract Subtree"));

	Ref<BehaviorTree> bt = memnew(BehaviorTree);
	bt->set_root_task(selected->clone());
	bt->set_path(p_path);
	RESOURCE_SAVE(bt, p_path, ResourceSaver::FLAG_CHANGE_PATH);

	Ref<BTSubtree> subtree = memnew(BTSubtree);
	subtree->set_subtree(bt);

	if (selected->is_root()) {
		undo_redo->add_do_method(task_tree->get_bt().ptr(), LW_NAME(set_root_task), subtree);
		undo_redo->add_undo_method(task_tree->get_bt().ptr(), LW_NAME(set_root_task), selected);
	} else {
		int idx = selected->get_index();
		undo_redo->add_do_method(selected->get_parent().ptr(), LW_NAME(remove_child), selected);
		undo_redo->add_do_method(selected->get_parent().ptr(), LW_NAME(add_child_at_index), subtree, idx);
		undo_redo->add_undo_method(selected->get_parent().ptr(), LW_NAME(remove_child), subtree);
		undo_redo->add_undo_method(selected->get_parent().ptr(), LW_NAME(add_child_at_index), selected, idx);
	}
	_commit_action_with_update(undo_redo);
	EditorInterface::get_singleton()->edit_resource(task_tree->get_selected());
}

void LimboAIEditor::_process_shortcut_input(const Ref<InputEvent> &p_event) {
	if (!p_event->is_pressed() || p_event->is_echo()) {
		return;
	}

	bool handled = false;

	// * Global shortcuts.

	if (LW_IS_SHORTCUT("limbo_ai/open_debugger", p_event)) {
		_misc_option_selected(MISC_OPEN_DEBUGGER);
		handled = true;
	}

	// * When editor is on screen.

	if (!handled && is_visible_in_tree()) {
		if (LW_IS_SHORTCUT("limbo_ai/jump_to_owner", p_event)) {
			tab_menu_idx = idx_history;
			_tab_menu_option_selected(TAB_JUMP_TO_OWNER);
			handled = true;
		} else if (LW_IS_SHORTCUT("limbo_ai/close_tab", p_event)) {
			tab_menu_idx = idx_history;
			_tab_menu_option_selected(TAB_CLOSE);
			handled = true;
		} else if (LW_IS_SHORTCUT("limbo_ai/editor_save_scene", p_event)) {
			// This intercepts the editor save action, but does not set the event as handled because we don't know the user's intention.
			// We just want to save the currently edited BT as well, which may cause a loop with built-in resource if done from "_save_external_data".
			// Workaround for: https://github.com/limbonaut/limboai/issues/240#issuecomment-2453087424
			if (task_tree->get_bt().is_valid() && RESOURCE_IS_BUILT_IN(task_tree->get_bt())) {
				_on_save_pressed();
			}
			handled = false; // intentionally not set as handled
		}
	}

	// * When editor is focused.

	if (!handled && (has_focus() || (get_viewport()->gui_get_focus_owner() && is_ancestor_of(get_viewport()->gui_get_focus_owner())))) {
		handled = true;
		if (LW_IS_SHORTCUT("limbo_ai/rename_task", p_event)) {
			_action_selected(ACTION_RENAME);
		} else if (LW_IS_SHORTCUT("limbo_ai/cut_task", p_event)) {
			_action_selected(ACTION_CUT);
		} else if (LW_IS_SHORTCUT("limbo_ai/copy_task", p_event)) {
			_action_selected(ACTION_COPY);
		} else if (LW_IS_SHORTCUT("limbo_ai/paste_task", p_event)) {
			_action_selected(ACTION_PASTE);
		} else if (LW_IS_SHORTCUT("limbo_ai/paste_task_after", p_event)) {
			_action_selected(ACTION_PASTE_AFTER);
		} else if (LW_IS_SHORTCUT("limbo_ai/move_task_up", p_event)) {
			_action_selected(ACTION_MOVE_UP);
		} else if (LW_IS_SHORTCUT("limbo_ai/move_task_down", p_event)) {
			_action_selected(ACTION_MOVE_DOWN);
		} else if (LW_IS_SHORTCUT("limbo_ai/duplicate_task", p_event)) {
			_action_selected(ACTION_DUPLICATE);
		} else if (LW_IS_SHORTCUT("limbo_ai/remove_task", p_event)) {
			_action_selected(ACTION_REMOVE);
		} else if (LW_IS_SHORTCUT("limbo_ai/new_behavior_tree", p_event)) {
			_new_bt();
		} else if (LW_IS_SHORTCUT("limbo_ai/save_behavior_tree", p_event)) {
			_on_save_pressed();
		} else if (LW_IS_SHORTCUT("limbo_ai/load_behavior_tree", p_event)) {
			_popup_file_dialog(load_dialog);
		} else if (LW_IS_SHORTCUT("limbo_ai/find_task", p_event)) {
			task_tree->tree_search_show_and_focus();
		} else {
			handled = false;
		}
	}

	if (handled) {
		get_viewport()->set_input_as_handled();
	}
}

void LimboAIEditor::_on_tree_rmb(const Vector2 &p_menu_pos) {
	menu->clear();

	Vector<Ref<BTTask>> selection = task_tree->get_selected_tasks();
	if (selection.is_empty()) {
		return;
	}

	if (selection.size() == 1 && task_tree->selected_has_probability()) {
		menu->add_icon_item(theme_cache.percent_icon, TTR("Edit Probability"), ACTION_EDIT_PROBABILITY);
	}
	menu->add_icon_shortcut(theme_cache.rename_task_icon, LW_GET_SHORTCUT("limbo_ai/rename_task"), ACTION_RENAME);
	menu->add_icon_item(theme_cache.change_type_icon, TTR("Change Type"), ACTION_CHANGE_TYPE);
	menu->add_icon_item(theme_cache.edit_script_icon, TTR("Edit Script"), ACTION_EDIT_SCRIPT);
	menu->add_icon_item(theme_cache.doc_icon, TTR("Open Documentation"), ACTION_OPEN_DOC);
	menu->set_item_disabled(menu->get_item_index(ACTION_RENAME), selection.size() != 1);
	menu->set_item_disabled(menu->get_item_index(ACTION_CHANGE_TYPE), selection.size() != 1);
	menu->set_item_disabled(menu->get_item_index(ACTION_EDIT_SCRIPT), selection.size() != 1 || selection[0]->get_script() == Variant());
	menu->set_item_disabled(menu->get_item_index(ACTION_OPEN_DOC), selection.size() != 1);

	menu->add_separator();
	{
		int acc = 0;
		for (const Ref<BTTask> &task : selection) {
			acc += task->is_enabled() ? 1 : -1;
		}
		Ref<Texture2D> icon;
		if (Math::abs(acc) == selection.size() && acc > 0) {
			icon = theme_cache.checked_icon;
		} else if (Math::abs(acc) == selection.size() && acc < 0) {
			icon = theme_cache.unchecked_icon;
		} else {
			icon = theme_cache.indeterminate_icon;
		}
		menu->add_icon_item(icon, TTR("Enabled"), ACTION_ENABLED);
		menu->set_item_checked(menu->get_item_index(ACTION_ENABLED), selection[0]->is_enabled());
	}

	menu->add_separator();
	menu->add_icon_shortcut(theme_cache.cut_icon, LW_GET_SHORTCUT("limbo_ai/cut_task"), ACTION_CUT);
	menu->add_icon_shortcut(theme_cache.copy_icon, LW_GET_SHORTCUT("limbo_ai/copy_task"), ACTION_COPY);
	menu->add_icon_shortcut(theme_cache.paste_icon, LW_GET_SHORTCUT("limbo_ai/paste_task"), ACTION_PASTE);
	menu->add_icon_shortcut(theme_cache.paste_icon, LW_GET_SHORTCUT("limbo_ai/paste_task_after"), ACTION_PASTE_AFTER);
	menu->set_item_disabled(menu->get_item_index(ACTION_PASTE), clipboard.is_empty() || selection.size() != 1);
	menu->set_item_disabled(menu->get_item_index(ACTION_PASTE_AFTER), clipboard.is_empty() || selection.size() != 1 || selection[0] == task_tree->get_bt()->get_root_task());

	menu->add_separator();
	menu->add_icon_shortcut(theme_cache.move_task_up_icon, LW_GET_SHORTCUT("limbo_ai/move_task_up"), ACTION_MOVE_UP);
	menu->add_icon_shortcut(theme_cache.move_task_down_icon, LW_GET_SHORTCUT("limbo_ai/move_task_down"), ACTION_MOVE_DOWN);
	menu->add_icon_shortcut(theme_cache.duplicate_task_icon, LW_GET_SHORTCUT("limbo_ai/duplicate_task"), ACTION_DUPLICATE);
	menu->add_icon_item(theme_cache.make_root_icon, TTR("Make Root"), ACTION_MAKE_ROOT);
	menu->add_icon_item(theme_cache.extract_subtree_icon, TTR("Extract Subtree"), ACTION_EXTRACT_SUBTREE);
	menu->set_item_disabled(menu->get_item_index(ACTION_MAKE_ROOT), selection.size() != 1);
	menu->set_item_disabled(menu->get_item_index(ACTION_EXTRACT_SUBTREE), selection.size() != 1);

	menu->add_separator();
	menu->add_icon_shortcut(theme_cache.remove_task_icon, LW_GET_SHORTCUT("limbo_ai/remove_task"), ACTION_REMOVE);

	menu->reset_size();
	menu->set_position(p_menu_pos);
	menu->popup();
}

void LimboAIEditor::_action_selected(int p_id) {
	switch (p_id) {
		case ACTION_RENAME: {
			Ref<BTTask> task = task_tree->get_selected();
			if (task.is_null()) {
				return;
			}
			if (IS_CLASS(task, BTComment)) {
				rename_dialog->set_title(TTR("Edit Comment"));
				rename_dialog->get_ok_button()->set_text(TTR("OK"));
				rename_edit->set_placeholder(TTR("Comment"));
			} else {
				rename_dialog->set_title(TTR("Rename Task"));
				rename_dialog->get_ok_button()->set_text(TTR("Rename"));
				rename_edit->set_placeholder(TTR("Custom Name"));
			}
			rename_edit->set_text(task->get_custom_name());
			rename_dialog->popup_centered();
			rename_edit->select_all();
			rename_edit->grab_focus();
		} break;
		case ACTION_ENABLED: {
			Vector<Ref<BTTask>> selection = task_tree->get_selected_tasks();
			if (selection.is_empty()) {
				return;
			}
			bool tasks_enabled = false;
			for (const Ref<BTTask> &task : selection) {
				if (task->is_enabled()) {
					tasks_enabled = true;
					break;
				}
			}
			for (int i = selection.size() - 1; i >= 0; i--) {
				if (selection[i]->is_enabled() != tasks_enabled) {
					selection.remove_at(i);
				}
			}
			EditorUndoRedoManager *undo_redo = _new_undo_redo_action(
					tasks_enabled ? TTR("Disable BT tasks") : TTR("Enable BT tasks"));
			for (const Ref<BTTask> &task : selection) {
				ERR_CONTINUE(task.is_null());
				undo_redo->add_do_method(task.ptr(), LW_NAME(_set_enabled), !tasks_enabled);
				undo_redo->add_undo_method(task.ptr(), LW_NAME(_set_enabled), tasks_enabled);
			}
			undo_redo->commit_action();
			_set_as_dirty(task_tree->get_bt(), true);
		} break;
		case ACTION_CHANGE_TYPE: {
			change_type_palette->clear_filter();
			change_type_palette->refresh();
			Rect2 rect = Rect2(get_global_mouse_position(), Size2(400.0, 600.0) * EDSCALE);
			change_type_popup->popup(rect);
		} break;
		case ACTION_EDIT_PROBABILITY: {
			Rect2 rect = task_tree->get_selected_probability_rect();
			ERR_FAIL_COND(rect == Rect2());
			rect.position.y += rect.size.y;
			rect.position += task_tree->get_rect().position;
			rect = task_tree->get_screen_transform().xform(rect);
			_update_probability_edit();
			probability_popup->popup(rect);
		} break;
		case ACTION_EDIT_SCRIPT: {
			ERR_FAIL_COND(task_tree->get_selected().is_null());
			EditorInterface::get_singleton()->edit_resource(task_tree->get_selected()->get_script());
		} break;
		case ACTION_OPEN_DOC: {
			Ref<BTTask> task = task_tree->get_selected();
			ERR_FAIL_COND(task.is_null());
			String help_class;

			Ref<Script> sc = GET_SCRIPT(task);
			if (sc.is_valid() && sc->get_path().is_absolute_path()) {
				help_class = sc->get_path();
			}
			if (help_class.is_empty()) {
				// Assuming context task is core class.
				help_class = task->get_class();
			}

			LimboUtility::get_singleton()->open_doc_class(help_class);
		} break;
		case ACTION_COPY: {
			Vector<Ref<BTTask>> selection = task_tree->get_selected_tasks();
			if (selection.is_empty()) {
				return;
			}
			clipboard.clear();
			for (const Ref<BTTask> &task : selection) {
				clipboard.push_back(task->clone());
			}
		} break;
		case ACTION_PASTE:
		case ACTION_PASTE_AFTER: {
			Ref<BTTask> sel = task_tree->get_selected();
			ERR_FAIL_COND(sel.is_null());
			EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Paste BT Tasks"));
			Ref<BTTask> parent = p_id == ACTION_PASTE_AFTER ? sel->get_parent() : sel;
			int idx = p_id == ACTION_PASTE_AFTER ? sel->get_index() + 1 : sel->get_child_count();
			Vector<Ref<BTTask>> newly_added;
			for (const Ref<BTTask> &clip : clipboard) {
				Ref<BTTask> dup = clip->clone();
				undo_redo->add_do_method(parent.ptr(), LW_NAME(add_child_at_index), dup, idx);
				undo_redo->add_undo_method(parent.ptr(), LW_NAME(remove_child), dup);
				newly_added.append(dup);
				idx += 1;
			}
			_commit_action_with_update(undo_redo);
			task_tree->clear_selection();
			for (const Ref<BTTask> &task : newly_added) {
				task_tree->add_selection(task);
			}
		} break;
		case ACTION_MOVE_UP: {
			Vector<Ref<BTTask>> selection = task_tree->get_selected_tasks();
			if (selection.is_empty()) {
				return;
			}
			for (const Ref<BTTask> &task : selection) {
				if (task->get_index() <= 0) {
					// If any selected is at the top of its hierarchy, we shouldn't move.
					return;
				}
			}
			EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Move BT Tasks"));
			for (int i = 0; i < selection.size(); i++) {
				Ref<BTTask> parent = selection[i]->get_parent();
				int idx = selection[i]->get_index();
				undo_redo->add_do_method(parent.ptr(), LW_NAME(remove_child), selection[i]);
				undo_redo->add_do_method(parent.ptr(), LW_NAME(add_child_at_index), selection[i], idx - 1);
			}
			for (int i = selection.size() - 1; i >= 0; i--) {
				Ref<BTTask> parent = selection[i]->get_parent();
				int idx = selection[i]->get_index();
				undo_redo->add_undo_method(parent.ptr(), LW_NAME(remove_child), selection[i]);
				undo_redo->add_undo_method(parent.ptr(), LW_NAME(add_child_at_index), selection[i], idx);
			}
			_commit_action_with_update(undo_redo);
		} break;
		case ACTION_MOVE_DOWN: {
			Vector<Ref<BTTask>> selection = task_tree->get_selected_tasks();
			if (selection.is_empty()) {
				return;
			}
			for (const Ref<BTTask> &task : selection) {
				if (task->get_parent().is_null() || task->get_index() >= task->get_parent()->get_child_count() - 1) {
					// If any selected is at the bottom of its hierarchy, we shouldn't move.
					return;
				}
			}
			EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Move BT Tasks"));
			for (int i = 0; i < selection.size(); i++) {
				Ref<BTTask> parent = selection[i]->get_parent();
				int idx = selection[i]->get_index();
				undo_redo->add_undo_method(parent.ptr(), LW_NAME(remove_child), selection[i]);
				undo_redo->add_undo_method(parent.ptr(), LW_NAME(add_child_at_index), selection[i], idx);
			}
			for (int i = selection.size() - 1; i >= 0; i--) {
				Ref<BTTask> parent = selection[i]->get_parent();
				int idx = selection[i]->get_index();
				undo_redo->add_do_method(parent.ptr(), LW_NAME(remove_child), selection[i]);
				undo_redo->add_do_method(parent.ptr(), LW_NAME(add_child_at_index), selection[i], idx + 1);
			}
			_commit_action_with_update(undo_redo);
		} break;
		case ACTION_DUPLICATE: {
			Vector<Ref<BTTask>> selection = task_tree->get_selected_tasks();
			if (selection.is_empty()) {
				return;
			}
			EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Duplicate BT Tasks"));
			Vector<Ref<BTTask>> duplicated;
			for (int i = selection.size() - 1; i >= 0; i--) {
				Ref<BTTask> parent = selection[i]->get_parent();
				if (parent.is_null()) {
					parent = selection[i];
				}
				const Ref<BTTask> &dup = selection[i]->clone();
				undo_redo->add_do_method(parent.ptr(), LW_NAME(add_child_at_index), dup, selection[i]->get_index() + 1);
				undo_redo->add_undo_method(parent.ptr(), LW_NAME(remove_child), dup);
				duplicated.append(dup);
			}
			_commit_action_with_update(undo_redo);
			task_tree->clear_selection();
			for (const Ref<BTTask> &dup : duplicated) {
				task_tree->add_selection(dup);
			}
		} break;
		case ACTION_MAKE_ROOT: {
			Ref<BTTask> task = task_tree->get_selected();
			if (task.is_valid() && task_tree->get_bt()->get_root_task() != task) {
				Ref<BTTask> parent = task->get_parent();
				ERR_FAIL_COND(parent.is_null());
				EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Make as BT Root"));
				undo_redo->add_do_method(parent.ptr(), LW_NAME(remove_child), task);
				Ref<BTTask> old_root = task_tree->get_bt()->get_root_task();
				undo_redo->add_do_method(task_tree->get_bt().ptr(), LW_NAME(set_root_task), task);
				undo_redo->add_do_method(task.ptr(), LW_NAME(add_child), old_root);
				undo_redo->add_undo_method(task.ptr(), LW_NAME(remove_child), old_root);
				undo_redo->add_undo_method(task_tree->get_bt().ptr(), LW_NAME(set_root_task), old_root);
				undo_redo->add_undo_method(parent.ptr(), LW_NAME(add_child_at_index), task, task->get_index());
				_commit_action_with_update(undo_redo);
			}
		} break;
		case ACTION_EXTRACT_SUBTREE: {
			Ref<BTTask> task = task_tree->get_selected();
			if (task.is_valid() && !IS_CLASS(task, BTSubtree)) {
				extract_dialog->popup_centered_ratio();
			}
		} break;
		case ACTION_CUT:
		case ACTION_REMOVE: {
			Vector<Ref<BTTask>> selection = task_tree->get_selected_tasks();
			if (selection.is_empty()) {
				return;
			}
			if (p_id == ACTION_CUT) {
				clipboard.clear();
			}
			EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Remove BT Tasks"), UndoRedo::MERGE_ALL);
			for (const Ref<BTTask> &task : selection) {
				if (task->is_root()) {
					undo_redo->add_do_method(task_tree->get_bt().ptr(), LW_NAME(set_root_task), Variant());
					undo_redo->add_undo_method(task_tree->get_bt().ptr(), LW_NAME(set_root_task), task_tree->get_bt()->get_root_task());
				} else {
					undo_redo->add_do_method(task->get_parent().ptr(), LW_NAME(remove_child), task);
					undo_redo->add_undo_method(task->get_parent().ptr(), LW_NAME(add_child_at_index), task, task->get_index());
				}
				if (p_id == ACTION_CUT) {
					clipboard.append(task->clone());
				}
			}
			_commit_action_with_update(undo_redo);
			EditorInterface::get_singleton()->edit_resource(task_tree->get_selected());
		} break;
	}
}

void LimboAIEditor::_on_probability_edited(double p_value) {
	Ref<BTTask> selected = task_tree->get_selected();
	ERR_FAIL_COND(selected.is_null());
	Ref<BTProbabilitySelector> probability_selector = selected->get_parent();
	ERR_FAIL_COND(probability_selector.is_null());
	if (percent_mode->is_pressed()) {
		probability_selector->set_probability(selected->get_index(), p_value * 0.01);
	} else {
		probability_selector->set_weight(selected->get_index(), p_value);
	}
}

void LimboAIEditor::_update_probability_edit() {
	Ref<BTTask> selected = task_tree->get_selected();
	ERR_FAIL_COND(selected.is_null());
	Ref<BTProbabilitySelector> prob = selected->get_parent();
	ERR_FAIL_COND(prob.is_null());
	double others_weight = prob->get_total_weight() - prob->get_weight(selected->get_index());
	bool cannot_edit_percent = others_weight == 0.0;
	percent_mode->set_disabled(cannot_edit_percent);
	if (cannot_edit_percent && percent_mode->is_pressed()) {
		weight_mode->set_pressed(true);
	}

	if (percent_mode->is_pressed()) {
		probability_edit->set_suffix("%");
		probability_edit->set_max(99.0);
		probability_edit->set_allow_greater(false);
		probability_edit->set_step(0.01);
		probability_edit->set_value_no_signal(task_tree->get_selected_probability_percent());
	} else {
		probability_edit->set_suffix("");
		probability_edit->set_allow_greater(true);
		probability_edit->set_max(10.0);
		probability_edit->set_step(0.01);
		probability_edit->set_value_no_signal(task_tree->get_selected_probability_weight());
	}
}

void LimboAIEditor::_probability_popup_closed() {
	probability_edit->grab_focus(); // Hack: Workaround for an EditorSpinSlider bug keeping LineEdit visible and "stuck" with ghost value.
}

void LimboAIEditor::_misc_option_selected(int p_id) {
	switch (p_id) {
		case MISC_ONLINE_DOCUMENTATION: {
			LimboUtility::get_singleton()->open_doc_online();
		} break;
		case MISC_DOC_INTRODUCTION: {
			LimboUtility::get_singleton()->open_doc_introduction();
		} break;
		case MISC_DOC_CUSTOM_TASKS: {
			LimboUtility::get_singleton()->open_doc_custom_tasks();
		} break;
		case MISC_OPEN_DEBUGGER: {
			ERR_FAIL_COND(LimboDebuggerPlugin::get_singleton() == nullptr);
			if (LimboDebuggerPlugin::get_singleton()->get_first_session_window()->get_window_enabled()) {
				LimboDebuggerPlugin::get_singleton()->get_first_session_window()->set_window_enabled(true);
			} else {
#ifdef LIMBOAI_MODULE
				EditorNode::get_bottom_panel()->make_item_visible(EditorDebuggerNode::get_singleton());
				EditorDebuggerNode::get_singleton()->get_default_debugger()->switch_to_debugger(
						LimboDebuggerPlugin::get_singleton()->get_first_session_tab_index());
#elif LIMBOAI_GDEXTENSION
				// TODO: Unsure how to switch to debugger pane with GDExtension.
#endif
			}
		} break;
		case MISC_PROJECT_SETTINGS: {
			_edit_project_settings();
		} break;
		case MISC_LAYOUT_CLASSIC: {
			EDITOR_SETTINGS()->set_setting("limbo_ai/editor/layout", LAYOUT_CLASSIC);
			EDITOR_SETTINGS()->mark_setting_changed("limbo_ai/editor/layout");
			_update_banners();
		} break;
		case MISC_LAYOUT_WIDESCREEN_OPTIMIZED: {
			EDITOR_SETTINGS()->set_setting("limbo_ai/editor/layout", LAYOUT_WIDESCREEN_OPTIMIZED);
			EDITOR_SETTINGS()->mark_setting_changed("limbo_ai/editor/layout");
			_update_banners();
		} break;
		case MISC_CREATE_SCRIPT_TEMPLATE: {
			String template_path = _get_script_template_path();
			String template_dir = template_path.get_base_dir();

			if (!FILE_EXISTS(template_path)) {
				if (!DirAccess::dir_exists_absolute(template_dir)) {
					Error err = DirAccess::make_dir_recursive_absolute(template_dir);
					ERR_FAIL_COND(err != OK);
				}

				Ref<FileAccess> f = FileAccess::open(template_path, FileAccess::WRITE);
				ERR_FAIL_COND(f.is_null());

				String script_template =
						"# meta-name: Custom Task\n"
						"# meta-description: Custom task to be used in a BehaviorTree\n"
						"# meta-default: true\n"
						"@tool\n"
						"extends _BASE_\n"
						"## _CLASS_\n"
						"\n\n"
						"# Display a customized name (requires @tool).\n"
						"func _generate_name() -> String:\n"
						"_TS_return \"_CLASS_\"\n"
						"\n\n"
						"# Called once during initialization.\n"
						"func _setup() -> void:\n"
						"_TS_pass\n"
						"\n\n"
						"# Called each time this task is entered.\n"
						"func _enter() -> void:\n"
						"_TS_pass\n"
						"\n\n"
						"# Called each time this task is exited.\n"
						"func _exit() -> void:\n"
						"_TS_pass\n"
						"\n\n"
						"# Called each time this task is ticked (aka executed).\n"
						"func _tick(delta: float) -> Status:\n"
						"_TS_return SUCCESS\n"
						"\n\n"
						"# Strings returned from this method are displayed as warnings in the behavior tree editor (requires @tool).\n"
						"func _get_configuration_warnings() -> PackedStringArray:\n"
						"_TS_var warnings := PackedStringArray()\n"
						"_TS_return warnings\n";

				f->store_string(script_template);
				f->close();
			}

			EDITOR_FILE_SYSTEM()->scan();
			EDIT_SCRIPT(template_path);
		} break;
		case MISC_SEARCH_TREE: {
			task_tree->tree_search_show_and_focus();
		} break;
	}
}

void LimboAIEditor::_on_tree_task_selected(const Ref<BTTask> &p_task) {
	EditorInterface::get_singleton()->edit_resource(p_task);
}

void LimboAIEditor::_on_tree_bt_selected(const Ref<BehaviorTree> &p_behavior_tree) {
	if (p_behavior_tree.is_valid()) {
		EditorInterface::get_singleton()->edit_resource(p_behavior_tree);
	}
}

void LimboAIEditor::_on_tree_task_activated() {
	Ref<BTTask> selected = task_tree->get_selected();
	ERR_FAIL_COND(selected.is_null());
	Ref<Script> scr = selected->get_script();
	if (scr.is_valid()) {
		EDIT_SCRIPT(scr->get_path());
	} else if (IS_CLASS(selected, BTSubtree)) {
		Ref<BehaviorTree> subtree = static_cast<Ref<BTSubtree>>(selected)->get_subtree();
		if (subtree.is_valid()) {
			EditorInterface::get_singleton()->edit_resource(subtree);
		} else {
			LimboUtility::get_singleton()->open_doc_class(selected->get_class());
		}
		EditorInterface::get_singleton()->edit_resource(subtree);
	} else {
		LimboUtility::get_singleton()->open_doc_class(selected->get_class());
	}
}

void LimboAIEditor::_on_visibility_changed() {
	if (task_tree->is_visible_in_tree()) {
		_switch_to_owner_scene_if_builtin(task_tree->get_bt());
		Ref<BTTask> sel = task_tree->get_selected();
		if (sel.is_valid()) {
			EditorInterface::get_singleton()->edit_resource(sel);
		} else if (task_tree->get_bt().is_valid() && EditorInterface::get_singleton()->get_inspector()->get_edited_object() != task_tree->get_bt().ptr()) {
			EditorInterface::get_singleton()->edit_resource(task_tree->get_bt());
		}

		task_palette->refresh();
		_update_banners();
	}
	_update_favorite_tasks();

	if (request_update_tabs && history.size() > 0) {
		_update_tabs();
		request_update_tabs = false;
	}
}

void LimboAIEditor::_on_save_pressed() {
	if (idx_history < 0 || idx_history >= history.size()) {
		return;
	}
	String path = history[idx_history]->get_path();
	if (path.is_empty()) {
		save_dialog->popup_centered_ratio();
	} else {
		_save_current_bt(path);
	}
}

void LimboAIEditor::_on_history_back() {
	ERR_FAIL_COND(history.size() == 0);
	idx_history = MAX(idx_history - 1, 0);
	EditorInterface::get_singleton()->edit_resource(history[idx_history]);
}

void LimboAIEditor::_on_history_forward() {
	ERR_FAIL_COND(history.size() == 0);
	idx_history = MIN(idx_history + 1, history.size() - 1);
	EditorInterface::get_singleton()->edit_resource(history[idx_history]);
}

void LimboAIEditor::_on_tasks_dragged(const TypedArray<BTTask> &p_tasks, Ref<BTTask> p_to_task, int p_to_pos) {
	ERR_FAIL_COND(p_to_task.is_null());
	if (p_tasks.is_empty()) {
		return;
	}

	// Remove descendants of selected.
	Vector<Ref<BTTask>> tasks_list;
	for (int i = 0; i < p_tasks.size(); i++) {
		Ref<BTTask> task = p_tasks[i];
		bool remove = false;
		for (int s_idx = 0; s_idx < p_tasks.size(); s_idx++) {
			Ref<BTTask> selected = p_tasks[s_idx];
			if (task->is_descendant_of(selected)) {
				remove = true;
				break;
			}
		}
		if (!remove) {
			tasks_list.push_back(task);
		}
	}

	EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Drag BT Task"));

	// Remove all tasks first so adding ordering is stable.
	int before_pos = 0;
	for (const Ref<BTTask> &task : tasks_list) {
		if (task->get_parent() == p_to_task && p_to_pos > task->get_index()) {
			before_pos += 1;
		}
		undo_redo->add_do_method(task->get_parent().ptr(), LW_NAME(remove_child), task);
	}

	for (int i = 0; i < tasks_list.size(); i++) {
		Ref<BTTask> task = tasks_list[i];
		undo_redo->add_do_method(p_to_task.ptr(), LW_NAME(add_child_at_index), task, p_to_pos + i - before_pos);
		undo_redo->add_undo_method(p_to_task.ptr(), LW_NAME(remove_child), task);
	}

	// Re-add tasks in later undo action so indexes match the old order.
	for (const Ref<BTTask> &task : tasks_list) {
		undo_redo->add_undo_method(task->get_parent().ptr(), LW_NAME(add_child_at_index), task, task->get_index());
	}

	_commit_action_with_update(undo_redo);
}

void LimboAIEditor::_on_resources_reload(const PackedStringArray &p_resources) {
	for (const String &res_path : p_resources) {
		if (!RESOURCE_IS_CACHED(res_path)) {
			continue;
		}

		if (RESOURCE_EXISTS(res_path, "BehaviorTree")) {
			Ref<BehaviorTree> res = RESOURCE_LOAD(res_path, "BehaviorTree");
			if (res.is_valid()) {
				if (history.has(res)) {
					disk_changed_files.insert(res_path);
				} else {
					Ref<BehaviorTree> reloaded = RESOURCE_LOAD_NO_CACHE(res_path, "BehaviorTree");
					res->copy_other(reloaded);
				}
			}
		}
	}

	// TODO: Find a way to allow resaving trees when they change outside of Godot.
	// * Currently, editor reloads them without asking in GDExtension. There is no Resource::editor_can_reload_from_file().
#ifdef LIMBOAI_MODULE
	if (disk_changed_files.size() > 0) {
		disk_changed_list->clear();
		disk_changed_list->set_hide_root(true);
		disk_changed_list->create_item();
		for (const String &fn : disk_changed_files) {
			TreeItem *ti = disk_changed_list->create_item();
			ti->set_text(0, fn);
		}

		if (!is_visible()) {
			EditorInterface::get_singleton()->set_main_screen_editor("LimboAI");
		}
		disk_changed->call_deferred("popup_centered_ratio", 0.5);
	}
#elif LIMBOAI_GDEXTENSION
	task_tree->update_tree();
#endif
}

void LimboAIEditor::_on_filesystem_changed() {
	if (history.size() == 0) {
		return;
	}

	if (is_visible_in_tree()) {
		_update_tabs();
	} else {
		request_update_tabs = true;
	}
}

void LimboAIEditor::_on_new_script_pressed() {
	PackedStringArray user_task_directories = GLOBAL_GET("limbo_ai/behavior_tree/user_task_dirs");
	ERR_FAIL_INDEX_MSG(0, user_task_directories.size(), "LimboAI: No user task directory set");
	String default_task_dir = user_task_directories[0];
	EditorInterface::get_singleton()->get_script_editor()->open_script_create_dialog("BTAction", default_task_dir.path_join("new_task"));
}

void LimboAIEditor::_task_type_selected(const String &p_class_or_path) {
	change_type_popup->hide();

	Ref<BTTask> selected_task = task_tree->get_selected();
	ERR_FAIL_COND(selected_task.is_null());
	Ref<BTTask> new_task = _create_task_by_class_or_path(p_class_or_path);
	ERR_FAIL_COND_MSG(new_task.is_null(), "LimboAI: Unable to construct task.");

	EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Change BT task type"));
	undo_redo->add_do_method(this, LW_NAME(_replace_task), selected_task, new_task);
	undo_redo->add_undo_method(this, LW_NAME(_replace_task), new_task, selected_task);
	_commit_action_with_update(undo_redo);
}

void LimboAIEditor::_copy_version_info() {
	DisplayServer::get_singleton()->clipboard_set(version_btn->get_text());
}

void LimboAIEditor::_replace_task(const Ref<BTTask> &p_task, const Ref<BTTask> &p_by_task) {
	ERR_FAIL_COND(p_task.is_null());
	ERR_FAIL_COND(p_by_task.is_null());
	ERR_FAIL_COND(p_by_task->get_child_count() > 0);
	ERR_FAIL_COND(p_by_task->get_parent().is_valid());

	while (p_task->get_child_count() > 0) {
		Ref<BTTask> child = p_task->get_child(0);
		p_task->remove_child_at_index(0);
		p_by_task->add_child(child);
	}
	p_by_task->set_custom_name(p_task->get_custom_name());

	Ref<BTTask> parent = p_task->get_parent();
	if (parent.is_null()) {
		// Assuming root task is replaced.
		ERR_FAIL_COND(task_tree->get_bt().is_null());
		ERR_FAIL_COND(task_tree->get_bt()->get_root_task() != p_task);
		task_tree->get_bt()->set_root_task(p_by_task);
	} else {
		// Non-root task is replaced.
		int idx = p_task->get_index();

		double weight = 0.0;
		Ref<BTProbabilitySelector> probability_selector = parent;
		if (probability_selector.is_valid()) {
			weight = probability_selector->get_weight(idx);
		}

		parent->remove_child(p_task);
		parent->add_child_at_index(p_by_task, idx);

		if (probability_selector.is_valid()) {
			probability_selector->set_weight(idx, weight);
		}
	}
}

void LimboAIEditor::_all_tab_clicked(int p_tab) {
	if (updating_tabs) {
		return;
	}
	_show_all_tab();
	_update_tabs();
}

void LimboAIEditor::_tab_clicked(int p_tab) {
	if (updating_tabs) {
		return;
	}
	ERR_FAIL_INDEX(p_tab, history.size());
	EditorInterface::get_singleton()->edit_resource(history[p_tab]);
}

void LimboAIEditor::_tab_closed(int p_tab) {
	ERR_FAIL_INDEX(p_tab, history.size());
	Ref<Resource> history_res = history[p_tab];
	if (history_res.is_valid() && history_res->is_connected(LW_NAME(changed), callable_mp(this, &LimboAIEditor::_set_as_dirty))) {
		history_res->disconnect(LW_NAME(changed), callable_mp(this, &LimboAIEditor::_set_as_dirty));
	}
	if (tab_search_context.has(history_res)) {
		tab_search_context.erase(history_res);
	}

	history.remove_at(p_tab);

	if (history.is_empty()) {
		_disable_editing();
		_update_tabs();
		return;
	}
	if (idx_history < 0) {
		// The All tab is on, and closing a document beside it leaves it on.
		_update_tabs();
		return;
	}
	if (p_tab != idx_history) {
		// A tab closed from its hover button need not be the active one, and then the
		// active one stays: it only moves down a place if the closed one stood before it.
		if (p_tab < idx_history) {
			idx_history--;
		}
		_update_tabs();
		return;
	}

	idx_history = MIN(idx_history, history.size() - 1);
	EditorInterface::get_singleton()->edit_resource(history[idx_history]);
	// A state machine tab never has search context -- only the task tree searches.
	TreeSearch::SearchInfo search_info_opened_tab;
	if (tab_search_context.has(history[idx_history])) {
		search_info_opened_tab = tab_search_context[history[idx_history]];
	}
	task_tree->tree_search_set_search_info(search_info_opened_tab);
	_update_tabs();
}

void LimboAIEditor::_update_tabs() {
	updating_tabs = true;

	// Pinned, and never closed: it is how a file that is not open yet gets opened. It has
	// its own bar so the documents can scroll past it without taking it along.
	// Icon only; the tooltip still names it.
	if (all_tab_bar->get_tab_count() == 0) {
		all_tab_bar->add_tab(String());
		all_tab_bar->set_tab_tooltip(0, TTR("All behavior trees and state machines in the project."));
	}
	all_tab_bar->set_tab_icon(0, get_theme_icon("Folder", "EditorIcons"));
	all_tab_bar->set_current_tab(idx_history < 0 ? 0 : -1);

	tab_bar->clear_tabs();

	Vector<String> short_names;
	// Keep track of how many times each short name is used.
	HashMap<String, int> usage_counts;

	for (int i = 0; i < history.size(); i++) {
		String tab_name;
		if (history[i]->get_path().contains("::")) {
			tab_name = history[i]->get_path().get_file();
		} else {
			tab_name = history[i]->get_path().get_file().get_basename();
		}
		short_names.append(tab_name);
		if (usage_counts.has(tab_name)) {
			usage_counts[tab_name] += 1;
		} else {
			usage_counts[tab_name] = 1;
		}
	}

	for (int i = 0; i < short_names.size(); i++) {
		String tab_name = short_names[i];
		if (tab_name.is_empty()) {
			tab_name = "[new]";
		} else if (usage_counts[tab_name] > 1) {
			// Use the full name if the short name is not unique.
			tab_name = history[i]->get_path().trim_prefix("res://");
		}
		bool is_hsm = Object::cast_to<HierarchicalStateMachine>(history[i].ptr()) != nullptr;
		tab_bar->add_tab(tab_name, LimboUtility::get_singleton()->get_task_icon(is_hsm ? "LimboHSM" : "BehaviorTree"));
	}

	Ref<Resource> active;
	if (idx_history >= 0) {
		ERR_FAIL_INDEX(idx_history, history.size());
		tab_bar->set_current_tab(idx_history);
		active = history[idx_history];
	} else {
		tab_bar->set_current_tab(-1);
	}

	// Not asked before the bar is in the tree: the theme reaches it (and this) earlier, and
	// a mouse position needs a viewport. The tab under the mouse may be a different one
	// now, after a close from its button.
	hovered_tab = -1;
	if (tab_bar_mouse_inside && tab_bar->is_inside_tree()) {
		Vector2 mouse = tab_bar->get_local_mouse_position();
		if (Rect2(Vector2(), tab_bar->get_size()).has_point(mouse)) {
			hovered_tab = tab_bar->get_tab_idx_at_point(mouse);
		}
	}
	_update_tab_close_buttons();

	updating_tabs = false;

	// Brought into view only when a different document becomes the active one. Every
	// rebuild doing it pulled the strip back to the active tab right after the user had
	// scrolled away from it (a rescan, a save or a hover rebuilds the tabs too).
	bool reveal = active.is_valid() && active != tab_revealed;
	tab_revealed = active;
	callable_mp(this, &LimboAIEditor::_layout_tab_strip).bind(reveal ? idx_history : -1).call_deferred();
}

// The close button is the tab's own button slot rather than TabBar's close button: TabBar
// only makes room for a close button on the tabs that show one, so showing it on hover
// would widen the tab under the mouse and shift every tab after it. Every document tab
// carries the slot; a blank icon of the same size fills it where the button is not shown.
void LimboAIEditor::_update_tab_close_buttons() {
	Ref<Texture2D> close_icon = tab_bar->get_theme_icon("close", "TabBar");
	if (close_icon.is_valid() && (tab_blank_icon.is_null() || tab_blank_icon->get_size() != close_icon->get_size())) {
		Vector2i size = close_icon->get_size();
		Ref<Image> blank = Image::create_empty(MAX(size.x, 1), MAX(size.y, 1), false, Image::FORMAT_RGBA8);
		blank->fill(Color(0, 0, 0, 0));
		tab_blank_icon = ImageTexture::create_from_image(blank);
	}
	int current = tab_bar->get_current_tab();
	for (int i = 0; i < tab_bar->get_tab_count(); i++) {
		bool shown = i == current || i == hovered_tab;
		tab_bar->set_tab_button_icon(i, shown ? close_icon : tab_blank_icon);
	}
	// Setting an icon makes TabBar work out its hovered tab again from where the mouse is,
	// without asking whether the mouse is on the bar at all. Over the scroll bar, which lies
	// on the bar's lower edge, the answer is a tab: the highlight came back after the bar had
	// already been told the mouse left, and nothing told it again. So it is told again.
	if (!tab_bar_mouse_inside && tab_bar->is_inside_tree()) {
		tab_bar->notification(NOTIFICATION_MOUSE_EXIT);
	}
}

void LimboAIEditor::_tab_button_pressed(int p_tab) {
	_tab_closed(p_tab);
}

void LimboAIEditor::_tab_hovered(int p_tab) {
	if (!tab_bar_mouse_inside) {
		// Hovers TabBar reports for a mouse that is not on it; see above.
		p_tab = -1;
	}
	if (hovered_tab == p_tab) {
		return;
	}
	hovered_tab = p_tab;
	_update_tab_close_buttons();
}

void LimboAIEditor::_tab_bar_mouse_entered() {
	tab_bar_mouse_inside = true;
}

void LimboAIEditor::_tab_bar_mouse_exited() {
	if (!tab_bar_mouse_inside) {
		// The exit sent from _update_tab_close_buttons() comes back here as a signal.
		return;
	}
	tab_bar_mouse_inside = false;
	_tab_hovered(-1);
	_update_tab_strip_hover();
}

// The document tabs do not clip and scroll themselves: TabBar would put its own arrow
// buttons at the end of the strip and jump to the current tab whenever it is rebuilt.
// Instead the bar is laid out at its full width inside `tab_clip`, which shows a window
// of it at `tab_scroll`, driven by `tab_hscroll` and the mouse wheel.
void LimboAIEditor::_layout_tab_strip(int p_reveal_tab) {
	Size2 bar_min = tab_bar->get_combined_minimum_size();
	// `tab_clip` is no container, so it does not ask for its child's height by itself.
	float clip_min_h = MAX(bar_min.y, all_tab_bar->get_combined_minimum_size().y);
	if (tab_clip->get_custom_minimum_size().y != clip_min_h) {
		tab_clip->set_custom_minimum_size(Size2(0, clip_min_h));
	}
	float full_w = bar_min.x;
	Size2 clip_size = tab_clip->get_size();
	tab_bar->set_size(Size2(full_w, clip_size.y));
	tab_hscroll->set_offset(SIDE_TOP, -tab_hscroll->get_combined_minimum_size().y);

	if (p_reveal_tab >= 0 && p_reveal_tab < tab_bar->get_tab_count()) {
		Rect2 rect = tab_bar->get_tab_rect(p_reveal_tab);
		if (rect.position.x < tab_scroll) {
			tab_scroll = rect.position.x;
		} else if (rect.position.x + rect.size.x > tab_scroll + clip_size.x) {
			tab_scroll = rect.position.x + rect.size.x - clip_size.x;
		}
	}
	_set_tab_scroll(tab_scroll);
}

void LimboAIEditor::_set_tab_scroll(float p_scroll) {
	float full_w = tab_bar->get_size().x;
	float clip_w = tab_clip->get_size().x;
	float max_scroll = MAX(0.0f, full_w - clip_w);
	tab_scroll = CLAMP(p_scroll, 0.0f, max_scroll);
	tab_bar->set_position(Vector2(-Math::round(tab_scroll), 0));

	updating_tab_scroll = true;
	tab_hscroll->set_max(full_w);
	tab_hscroll->set_page(clip_w);
	tab_hscroll->set_value(tab_scroll);
	updating_tab_scroll = false;

	// The same shade the FileSystem dock lays over rows scrolled out of the top: tabs are
	// hidden to the left of the strip, so the All tab does not stand next to the first one.
	tab_scroll_hint->set_visible(tab_scroll > 0.5f);
	_update_tab_strip_hover();
}

void LimboAIEditor::_on_tab_hscroll_changed(double p_value) {
	if (updating_tab_scroll) {
		return;
	}
	_set_tab_scroll(p_value);
}

// Shown only while the mouse is over the document tabs (or the bar itself, which lies over
// their lower edge), and only when there is something to scroll.
void LimboAIEditor::_update_tab_strip_hover() {
	bool hovered = false;
	if (tab_clip->is_inside_tree() && tab_clip->is_visible_in_tree()) {
		Vector2 mouse = tab_clip->get_local_mouse_position();
		hovered = Rect2(Vector2(), tab_clip->get_size()).has_point(mouse);
	}
	bool scrollable = tab_bar->get_size().x > tab_clip->get_size().x + 0.5f;
	tab_hscroll->set_visible(hovered && scrollable);
}

void LimboAIEditor::_tab_strip_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		_update_tab_strip_hover();
		return;
	}
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed()) {
		return;
	}
	float step = 0.0f;
	switch (mb->get_button_index()) {
		case LW_MBTN(WHEEL_UP):
		case LW_MBTN(WHEEL_LEFT):
			step = -1.0f;
			break;
		case LW_MBTN(WHEEL_DOWN):
		case LW_MBTN(WHEEL_RIGHT):
			step = 1.0f;
			break;
		default:
			return;
	}
	// A wheel notch moves the strip by about a third of what is on view, so a few notches
	// cross it without overshooting a short one.
	float factor = mb->get_factor() > 0.0f ? mb->get_factor() : 1.0f;
	_set_tab_scroll(tab_scroll + step * factor * MAX(40.0f * EDSCALE, tab_clip->get_size().x / 3.0f));
	tab_clip->accept_event();
}

void LimboAIEditor::_move_active_tab(int p_to_index) {
	if (idx_history < 0) {
		_update_tabs();
		return;
	}
	int to_idx = CLAMP(p_to_index, 0, history.size() - 1);
	if (idx_history != to_idx) {
		Ref<Resource> res = history[idx_history];
		history.remove_at(idx_history);
		history.insert(to_idx, res);
		idx_history = to_idx;
	}
	_update_tabs();
}

void LimboAIEditor::_tab_input(const Ref<InputEvent> &p_input) {
	Ref<InputEventMouseMotion> mm = p_input;
	if (mm.is_valid()) {
		// TabBar says when the mouse comes onto a tab, not when it leaves one for the empty
		// end of the bar, so the hovered tab is also read off the motion.
		_tab_hovered(tab_bar->get_tab_idx_at_point(mm->get_position()));
		_update_tab_strip_hover();
		return;
	}
	Ref<InputEventMouseButton> mb = p_input;
	if (mb.is_null()) {
		return;
	}
	if (mb->is_pressed() && (mb->get_button_index() == LW_MBTN(WHEEL_UP) || mb->get_button_index() == LW_MBTN(WHEEL_DOWN) ||
									mb->get_button_index() == LW_MBTN(WHEEL_LEFT) || mb->get_button_index() == LW_MBTN(WHEEL_RIGHT))) {
		_tab_strip_input(p_input);
		tab_bar->accept_event();
		return;
	}
	int tab_idx = tab_bar->get_tab_idx_at_point(tab_bar->get_local_mouse_position());
	if (tab_idx < 0) {
		return;
	}
	if (mb->is_pressed() && mb->get_button_index() == LW_MBTN(MIDDLE)) {
		_tab_closed(tab_idx);
	} else if (mb->is_pressed() && mb->get_button_index() == LW_MBTN(RIGHT)) {
		tab_menu_idx = tab_idx;
		_show_tab_context_menu();
	}
}

void LimboAIEditor::_show_tab_context_menu() {
	tab_menu->clear();
	tab_menu->add_shortcut(LW_GET_SHORTCUT("limbo_ai/jump_to_owner"), TabMenu::TAB_JUMP_TO_OWNER);
	tab_menu->add_item(TTR("Show in FileSystem"), TabMenu::TAB_SHOW_IN_FILESYSTEM);
	tab_menu->add_separator();
	tab_menu->add_shortcut(LW_GET_SHORTCUT("limbo_ai/close_tab"), TabMenu::TAB_CLOSE);
	tab_menu->add_item(TTR("Close Other Tabs"), TabMenu::TAB_CLOSE_OTHER);
	tab_menu->add_item(TTR("Close Tabs to the Right"), TabMenu::TAB_CLOSE_RIGHT);
	tab_menu->add_item(TTR("Close All Tabs"), TabMenu::TAB_CLOSE_ALL);
	tab_menu->set_position(get_screen_position() + get_local_mouse_position());
	tab_menu->reset_size();
	tab_menu->popup();
}

// Acts on tab_menu_idx: the tab the menu was opened over, or the active one for a shortcut.
void LimboAIEditor::_tab_menu_option_selected(int p_id) {
	if (tab_menu_idx < 0 || tab_menu_idx >= history.size()) {
		// No document under the menu (the All tab is on, or the tab is gone).
		return;
	}
	int target = tab_menu_idx;

	switch (p_id) {
		case TAB_SHOW_IN_FILESYSTEM: {
			Ref<Resource> res = history[target];
			String path = res->get_path();
			if (!path.is_empty()) {
				FileSystemDock *dock = EditorInterface::get_singleton()->get_file_system_dock();
				dock->navigate_to_path(path.get_slice("::", 0));
			}
		} break;
		case TAB_JUMP_TO_OWNER: {
			Ref<Resource> res = history[target];
			ERR_FAIL_COND(res.is_null());
			String bt_path = res->get_path();
			if (!bt_path.is_empty()) {
				owner_picker->pick_and_open_owner_of_resource(bt_path);
			}
		} break;
		case TAB_CLOSE: {
			_tab_closed(target);
		} break;
		case TAB_CLOSE_OTHER: {
			_close_tabs_except(target, target);
		} break;
		case TAB_CLOSE_RIGHT: {
			_close_tabs_except(0, target);
		} break;
		case TAB_CLOSE_ALL: {
			_close_tabs_except(0, -1);
		} break;
	}
}

void LimboAIEditor::_close_tabs_except(int p_keep_from, int p_keep_to) {
	Ref<Resource> active = (idx_history >= 0 && idx_history < history.size()) ? history[idx_history] : Ref<Resource>();

	for (int i = history.size() - 1; i >= 0; i--) {
		if (i >= p_keep_from && i <= p_keep_to) {
			continue;
		}
		Ref<Resource> res = history[i];
		if (res.is_valid() && res->is_connected(LW_NAME(changed), callable_mp(this, &LimboAIEditor::_set_as_dirty))) {
			res->disconnect(LW_NAME(changed), callable_mp(this, &LimboAIEditor::_set_as_dirty));
		}
		tab_search_context.erase(res);
		history.remove_at(i);
	}

	if (history.is_empty()) {
		_disable_editing();
		_update_tabs();
		return;
	}
	if (active.is_null()) {
		// The All tab was on, and stays on.
		_update_tabs();
		return;
	}
	int idx = history.find(active);
	if (idx != -1) {
		idx_history = idx;
		_update_tabs();
		return;
	}
	// The active document went with the rest: the tab the menu was opened over takes its
	// place. What was kept now starts at index 0.
	idx_history = CLAMP(p_keep_to - p_keep_from, 0, history.size() - 1);
	EditorInterface::get_singleton()->edit_resource(history[idx_history]);
}

void LimboAIEditor::_on_blackboard_pressed() {
	Ref<BlackboardPlan> plan = get_edited_blackboard_plan();
	if (plan.is_valid()) {
		EditorInterface::get_singleton()->edit_resource(plan);
	}
}

void LimboAIEditor::_reload_modified() {
	for (const String &res_path : disk_changed_files) {
		Ref<BehaviorTree> res = RESOURCE_LOAD(res_path, "BehaviorTree");
		if (res.is_valid()) {
			Ref<BehaviorTree> reloaded = RESOURCE_LOAD_NO_CACHE(res_path, "BehaviorTree");
			res->copy_other(reloaded);
			if (idx_history >= 0 && history.get(idx_history) == res) {
				edit_bt(res, true);
			}
		}
	}
	disk_changed_files.clear();
	task_tree->update_tree();
}

void LimboAIEditor::_resave_modified(String _str) {
	for (const String &res_path : disk_changed_files) {
		Ref<Resource> res = RESOURCE_LOAD(res_path, "Resource");
		if (res.is_valid()) {
			if (RESOURCE_IS_EXTERNAL(res)) {
				// Only resave external - scene files are handled by the editor.
				_save_resource(res, res->get_path());
			}
		}
	}
	task_tree->update_tree();
	disk_changed->hide();
	disk_changed_files.clear();
}

void LimboAIEditor::_popup_info_dialog(const String &p_text) {
	info_dialog->set_text(p_text);
	info_dialog->popup_centered();
}

void LimboAIEditor::_rename_task_confirmed() {
	ERR_FAIL_COND(!task_tree->get_selected().is_valid());
	rename_dialog->hide();

	EditorUndoRedoManager *undo_redo = _new_undo_redo_action(TTR("Set Custom Name"));
	undo_redo->add_do_method(task_tree->get_selected().ptr(), LW_NAME(set_custom_name), rename_edit->get_text());
	undo_redo->add_undo_method(task_tree->get_selected().ptr(), LW_NAME(set_custom_name), task_tree->get_selected()->get_custom_name());
	undo_redo->add_do_method(this, LW_NAME(_update_task_tree), task_tree->get_bt(), task_tree->get_selected());
	undo_redo->add_undo_method(this, LW_NAME(_update_task_tree), task_tree->get_bt(), task_tree->get_selected());
	undo_redo->commit_action();
}

void LimboAIEditor::save_all(bool p_external_only) {
	for (int i = 0; i < history.size(); i++) {
		Ref<Resource> res = history.get(i);
		String path = res->get_path();
		if (RESOURCE_EXISTS(path, "Resource") && (!p_external_only || RESOURCE_PATH_IS_EXTERNAL(path))) {
			_save_resource(res, path);
		}
	}
}

void LimboAIEditor::_update_favorite_tasks() {
	for (int i = 0; i < fav_tasks_hbox->get_child_count(); i++) {
		fav_tasks_hbox->get_child(i)->queue_free();
	}
	Array favorite_tasks = GLOBAL_GET("limbo_ai/behavior_tree/favorite_tasks");
	for (int i = 0; i < favorite_tasks.size(); i++) {
		String task_meta = favorite_tasks[i];

		if (task_meta.is_empty() || (!FILE_EXISTS(task_meta) && !ClassDB::class_exists(task_meta))) {
			callable_mp(this, &LimboAIEditor::_update_banners).call_deferred();
			continue;
		}

		Button *btn = memnew(Button);
		String task_name;
		if (task_meta.begins_with("res:")) {
			task_name = task_meta.get_file().get_basename().trim_prefix("BT").to_pascal_case();
		} else {
			task_name = task_meta.trim_prefix("BT");
		}
		btn->set_text(task_name);
		btn->set_meta(LW_NAME(task_meta), task_meta);
		btn->set_button_icon(LimboUtility::get_singleton()->get_task_icon(task_meta));
		btn->set_tooltip_text(vformat(TTR("Add %s task."), task_name));
		btn->set_flat(true);
		btn->add_theme_constant_override(LW_NAME(icon_max_width), 16 * EDSCALE); // Force user icons to be of the proper size.
		btn->set_focus_mode(Control::FOCUS_NONE);
		btn->connect(LW_NAME(pressed), callable_mp(this, &LimboAIEditor::_add_task_by_class_or_path).bind(task_meta));
		fav_tasks_hbox->add_child(btn);
	}
}

void LimboAIEditor::_update_misc_menu() {
	PopupMenu *misc_menu = misc_btn->get_popup();

	misc_menu->clear();

	misc_menu->add_icon_item(theme_cache.doc_icon, TTR("Online Documentation"), MISC_ONLINE_DOCUMENTATION);
	misc_menu->add_icon_item(theme_cache.introduction_icon, TTR("Introduction"), MISC_DOC_INTRODUCTION);
	misc_menu->add_icon_item(theme_cache.introduction_icon, TTR("Creating custom tasks in GDScript"), MISC_DOC_CUSTOM_TASKS);

	misc_menu->add_separator();
#ifdef LIMBOAI_MODULE
	// * Disabled in GDExtension: Not sure how to switch to debugger pane.
	misc_menu->add_icon_shortcut(theme_cache.open_debugger_icon, LW_GET_SHORTCUT("limbo_ai/open_debugger"), MISC_OPEN_DEBUGGER);
#endif // LIMBOAI_MODULE
	misc_menu->add_item(TTR("Project Settings..."), MISC_PROJECT_SETTINGS);

	PopupMenu *layout_menu = Object::cast_to<PopupMenu>(misc_menu->get_node_or_null(NodePath("LayoutMenu")));
	if (layout_menu == nullptr) {
		layout_menu = memnew(PopupMenu);
		layout_menu->set_name("LayoutMenu");
		layout_menu->connect(LW_NAME(id_pressed), callable_mp(this, &LimboAIEditor::_misc_option_selected));
		misc_menu->add_child(layout_menu);
		layout_menu->add_radio_check_item(TTR("Classic"), MISC_LAYOUT_CLASSIC);
		layout_menu->add_radio_check_item(TTR("Widescreen Optimized"), MISC_LAYOUT_WIDESCREEN_OPTIMIZED);
	}
	misc_menu->add_submenu_item(TTR("Layout"), "LayoutMenu");
	EditorLayout saved_layout = (EditorLayout)(int)EDITOR_GET("limbo_ai/editor/layout");
	layout_menu->set_item_checked(0, saved_layout == LAYOUT_CLASSIC);
	layout_menu->set_item_checked(1, saved_layout == LAYOUT_WIDESCREEN_OPTIMIZED);

	misc_menu->add_separator();
	misc_menu->add_item(
			FILE_EXISTS(_get_script_template_path()) ? TTR("Edit Script Template") : TTR("Create Script Template"),
			MISC_CREATE_SCRIPT_TEMPLATE);

	misc_menu->add_separator();
	misc_menu->add_icon_shortcut(theme_cache.search_icon, LW_GET_SHORTCUT("limbo_ai/find_task"), MISC_SEARCH_TREE);
}

void LimboAIEditor::_update_banners() {
	for (int i = banners->get_child_count() - 1; i >= 0; i--) {
		if (banners->get_child(i)->has_meta(LW_NAME(managed))) {
			Node *banner = banners->get_child(i);
			banners->remove_child(banner);
			banner->queue_free();
		}
	}

	PackedStringArray user_task_directories = GLOBAL_GET("limbo_ai/behavior_tree/user_task_dirs");
	for (const String &task_dir : user_task_directories) {
		if (!task_dir.is_empty() && !DirAccess::dir_exists_absolute(task_dir)) {
			ActionBanner *banner = memnew(ActionBanner);
			banner->set_text(vformat(TTR("Task folder not found: %s"), task_dir));
			banner->add_action(TTR("Create"), callable_mp(this, &LimboAIEditor::_create_user_task_dir).bind(task_dir), true);
			banner->add_action(TTR("Edit Path..."), callable_mp(this, &LimboAIEditor::_edit_project_settings));
			banner->add_spacer();
			banner->add_action(TTR("Help..."), callable_mp(LimboUtility::get_singleton(), &LimboUtility::open_doc_custom_tasks));
			banner->set_meta(LW_NAME(managed), Variant(true));
			banners->add_child(banner);
		}
	}

	Array favorite_tasks = GLOBAL_GET("limbo_ai/behavior_tree/favorite_tasks");
	for (int i = 0; i < favorite_tasks.size(); i++) {
		String task_meta = favorite_tasks[i];

		if (task_meta.is_empty() || (!FILE_EXISTS(task_meta) && !ClassDB::class_exists(task_meta))) {
			ActionBanner *banner = memnew(ActionBanner);
			banner->set_text(vformat(TTR("Favorite task not found: %s"), task_meta));
			banner->add_action(TTR("Remove"), callable_mp(this, &LimboAIEditor::_remove_task_from_favorite).bind(task_meta), true);
			banner->add_action(TTR("Edit Favorite Tasks..."), callable_mp(this, &LimboAIEditor::_edit_project_settings));
			banner->set_meta(LW_NAME(managed), Variant(true));
			banners->add_child(banner);
		}
	}

	EditorLayout saved_layout = (EditorLayout)(int)EDITOR_GET("limbo_ai/editor/layout");
	if (saved_layout != editor_layout) {
		ActionBanner *banner = memnew(ActionBanner);
		banner->set_text(TTR("Restart required to apply changes to editor layout"));
		banner->add_action(TTR("Save & Restart"), callable_mp(this, &LimboAIEditor::_save_and_restart), true);
		banner->set_meta(LW_NAME(managed), Variant(true));
		banners->add_child(banner);
	}
}

void LimboAIEditor::_do_update_theme_item_cache() {
	theme_cache.duplicate_task_icon = get_theme_icon(LW_NAME(Duplicate), LW_NAME(EditorIcons));
	theme_cache.edit_script_icon = get_theme_icon(LW_NAME(Script), LW_NAME(EditorIcons));
	theme_cache.make_root_icon = get_theme_icon(LW_NAME(NewRoot), LW_NAME(EditorIcons));
	theme_cache.move_task_down_icon = get_theme_icon(LW_NAME(MoveDown), LW_NAME(EditorIcons));
	theme_cache.move_task_up_icon = get_theme_icon(LW_NAME(MoveUp), LW_NAME(EditorIcons));
	theme_cache.open_debugger_icon = get_theme_icon(LW_NAME(Debug), LW_NAME(EditorIcons));
	theme_cache.doc_icon = get_theme_icon(LW_NAME(Help), LW_NAME(EditorIcons));
	theme_cache.introduction_icon = get_theme_icon(LW_NAME(Info), LW_NAME(EditorIcons));
	theme_cache.remove_task_icon = get_theme_icon(LW_NAME(Remove), LW_NAME(EditorIcons));
	theme_cache.rename_task_icon = get_theme_icon(LW_NAME(Rename), LW_NAME(EditorIcons));
	theme_cache.change_type_icon = get_theme_icon(LW_NAME(Reload), LW_NAME(EditorIcons));
	theme_cache.cut_icon = get_theme_icon(LW_NAME(ActionCut), LW_NAME(EditorIcons));
	theme_cache.copy_icon = get_theme_icon(LW_NAME(ActionCopy), LW_NAME(EditorIcons));
	theme_cache.paste_icon = get_theme_icon(LW_NAME(ActionPaste), LW_NAME(EditorIcons));
	theme_cache.search_icon = get_theme_icon(LW_NAME(Search), LW_NAME(EditorIcons));
	theme_cache.checked_icon = get_theme_icon("GuiChecked", LW_NAME(EditorIcons));
	theme_cache.unchecked_icon = get_theme_icon("GuiUnchecked", LW_NAME(EditorIcons));
	theme_cache.indeterminate_icon = get_theme_icon("GuiIndeterminate", LW_NAME(EditorIcons));

	theme_cache.behavior_tree_icon = LimboUtility::get_singleton()->get_task_icon("BehaviorTree");
	theme_cache.percent_icon = LimboUtility::get_singleton()->get_task_icon("LimboPercent");
	theme_cache.extract_subtree_icon = LimboUtility::get_singleton()->get_task_icon("LimboExtractSubtree");
}

void LimboAIEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_EXIT_TREE: {
			task_tree->unload();
			for (int i = 0; i < history.size(); i++) {
				if (history[i]->is_connected(LW_NAME(changed), callable_mp(this, &LimboAIEditor::_set_as_dirty))) {
					history[i]->disconnect(LW_NAME(changed), callable_mp(this, &LimboAIEditor::_set_as_dirty));
				}
			}
		} break;
		case NOTIFICATION_READY: {
			// **** Signals
			save_dialog->connect("file_selected", callable_mp(this, &LimboAIEditor::_save_current_bt));
			load_dialog->connect("file_selected", callable_mp(this, &LimboAIEditor::_load_resource));
			extract_dialog->connect("file_selected", callable_mp(this, &LimboAIEditor::_extract_subtree));
			new_btn->connect(LW_NAME(pressed), callable_mp(this, &LimboAIEditor::_on_new_pressed));
			hsm_add_state_btn->connect(LW_NAME(pressed), callable_mp(hsm_view, &HSMEditorView::add_state));
			hsm_add_machine_btn->connect(LW_NAME(pressed), callable_mp(hsm_view, &HSMEditorView::add_machine));
			hsm_remove_btn->connect(LW_NAME(pressed), callable_mp(hsm_view, &HSMEditorView::remove_selected));
			load_btn->connect(LW_NAME(pressed), callable_mp(this, &LimboAIEditor::_popup_file_dialog).bind(load_dialog));
			task_tree->connect("rmb_pressed", callable_mp(this, &LimboAIEditor::_on_tree_rmb));
			task_tree->connect("task_selected", callable_mp(this, &LimboAIEditor::_on_tree_task_selected));
			task_tree->connect("tasks_dragged", callable_mp(this, &LimboAIEditor::_on_tasks_dragged));
			task_tree->connect("palette_task_dropped", callable_mp(this, &LimboAIEditor::_on_palette_task_dropped));
			task_tree->connect("behavior_tree_selected", callable_mp(this, &LimboAIEditor::_on_tree_bt_selected));
			task_tree->connect("task_activated", callable_mp(this, &LimboAIEditor::_on_tree_task_activated));
			task_tree->connect("probability_clicked", callable_mp(this, &LimboAIEditor::_action_selected).bind(ACTION_EDIT_PROBABILITY));
			task_tree->connect("visibility_changed", callable_mp(this, &LimboAIEditor::_on_visibility_changed));
			save_btn->connect(LW_NAME(pressed), callable_mp(this, &LimboAIEditor::_on_save_pressed));
			misc_btn->connect(LW_NAME(pressed), callable_mp(this, &LimboAIEditor::_update_misc_menu));
			misc_btn->get_popup()->connect("id_pressed", callable_mp(this, &LimboAIEditor::_misc_option_selected));
			task_palette->connect("task_selected", callable_mp(this, &LimboAIEditor::_add_task_by_class_or_path));
			task_palette->connect("favorite_tasks_changed", callable_mp(this, &LimboAIEditor::_update_favorite_tasks));
			change_type_palette->connect("task_selected", callable_mp(this, &LimboAIEditor::_task_type_selected));
			menu->connect("id_pressed", callable_mp(this, &LimboAIEditor::_action_selected));
			weight_mode->connect(LW_NAME(pressed), callable_mp(this, &LimboAIEditor::_update_probability_edit));
			percent_mode->connect(LW_NAME(pressed), callable_mp(this, &LimboAIEditor::_update_probability_edit));
			probability_edit->connect("value_changed", callable_mp(this, &LimboAIEditor::_on_probability_edited));
			probability_popup->connect("popup_hide", callable_mp(this, &LimboAIEditor::_probability_popup_closed));
			disk_changed->connect("confirmed", callable_mp(this, &LimboAIEditor::_reload_modified));
			disk_changed->connect("custom_action", callable_mp(this, &LimboAIEditor::_resave_modified));
			rename_dialog->connect("confirmed", callable_mp(this, &LimboAIEditor::_rename_task_confirmed));
			new_script_btn->connect(LW_NAME(pressed), callable_mp(this, &LimboAIEditor::_on_new_script_pressed));
			tab_bar->connect("tab_clicked", callable_mp(this, &LimboAIEditor::_tab_clicked));
			tab_bar->connect("active_tab_rearranged", callable_mp(this, &LimboAIEditor::_move_active_tab));
			tab_bar->connect("tab_close_pressed", callable_mp(this, &LimboAIEditor::_tab_closed));
			tab_bar->connect(LW_NAME(gui_input), callable_mp(this, &LimboAIEditor::_tab_input));
			tab_menu->connect(LW_NAME(id_pressed), callable_mp(this, &LimboAIEditor::_tab_menu_option_selected));
			tab_bar->connect("tab_button_pressed", callable_mp(this, &LimboAIEditor::_tab_button_pressed));
			tab_bar->connect("tab_hovered", callable_mp(this, &LimboAIEditor::_tab_hovered));
			tab_bar->connect(LW_NAME(mouse_exited), callable_mp(this, &LimboAIEditor::_tab_bar_mouse_exited));
			tab_bar->connect(LW_NAME(mouse_entered), callable_mp(this, &LimboAIEditor::_tab_bar_mouse_entered));
			browser->connect("owners_requested", callable_mp(owner_picker, &OwnerPicker::pick_and_open_owner_of_resource));
			all_tab_bar->connect("tab_clicked", callable_mp(this, &LimboAIEditor::_all_tab_clicked));
			tab_clip->connect(LW_NAME(gui_input), callable_mp(this, &LimboAIEditor::_tab_strip_input));
			tab_clip->connect(LW_NAME(mouse_exited), callable_mp(this, &LimboAIEditor::_update_tab_strip_hover));
			tab_clip->connect("resized", callable_mp(this, &LimboAIEditor::_layout_tab_strip).bind(-1));
			tab_bar->connect("minimum_size_changed", callable_mp(this, &LimboAIEditor::_layout_tab_strip).bind(-1));
			tab_hscroll->connect("value_changed", callable_mp(this, &LimboAIEditor::_on_tab_hscroll_changed));
			tab_hscroll->connect(LW_NAME(mouse_exited), callable_mp(this, &LimboAIEditor::_update_tab_strip_hover));
			blackboard_btn->connect(LW_NAME(pressed), callable_mp(this, &LimboAIEditor::_on_blackboard_pressed));
			browser->connect("resource_activated", callable_mp(this, &LimboAIEditor::_load_resource));
			version_btn->connect(LW_NAME(pressed), callable_mp(this, &LimboAIEditor::_copy_version_info));

			EDITOR_FILE_SYSTEM()->connect("resources_reload", callable_mp(this, &LimboAIEditor::_on_resources_reload));
			EDITOR_FILE_SYSTEM()->connect("filesystem_changed", callable_mp(this, &LimboAIEditor::_on_filesystem_changed));
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			_do_update_theme_item_cache();

			ADD_STYLEBOX_OVERRIDE(tab_bar_panel, "panel", get_theme_stylebox("tabbar_background", "TabContainer"));

			new_btn->set_button_icon(get_theme_icon(LW_NAME(New), LW_NAME(EditorIcons)));
			load_btn->set_button_icon(get_theme_icon(LW_NAME(Load), LW_NAME(EditorIcons)));
			save_btn->set_button_icon(get_theme_icon(LW_NAME(Save), LW_NAME(EditorIcons)));
			new_script_btn->set_button_icon(get_theme_icon(LW_NAME(ScriptCreate), LW_NAME(EditorIcons)));
			misc_btn->set_button_icon(get_theme_icon(LW_NAME(Tools), LW_NAME(EditorIcons)));
			blackboard_btn->set_button_icon(LimboUtility::get_singleton()->get_task_icon("LimboEditBlackboard"));
			// The All tab's folder icon and the close button's size come from the theme.
			_update_tabs();
			if (has_theme_icon("scroll_hint_horizontal", "ScrollContainer")) {
				Ref<Texture2D> hint = get_theme_icon("scroll_hint_horizontal", "ScrollContainer");
				tab_scroll_hint->set_texture(hint);
				tab_scroll_hint->set_custom_minimum_size(Size2(hint->get_width(), 0));
				tab_scroll_hint->set_self_modulate(get_theme_color("scroll_hint_horizontal_color", "ScrollContainer"));
			}

			// The plugin's own icons for the two resource types, rather than a generic plus:
			// what these buttons add *is* a state and a machine, and the same two pictures
			// name them in the FileSystem dock and on the inspector.
			hsm_add_state_btn->set_button_icon(LimboUtility::get_singleton()->get_task_icon("HSMStateSpec"));
			hsm_add_machine_btn->set_button_icon(LimboUtility::get_singleton()->get_task_icon("HierarchicalStateMachine"));
			hsm_remove_btn->set_button_icon(get_theme_icon(LW_NAME(Remove), LW_NAME(EditorIcons)));

			_update_favorite_tasks();
		} break;
		case EditorSettings::NOTIFICATION_EDITOR_SETTINGS_CHANGED: {
			if (is_visible_in_tree()) {
				_update_banners();
			}
		} break;
	}
}

void LimboAIEditor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_add_task", "task"), &LimboAIEditor::_add_task);
	ClassDB::bind_method(D_METHOD("_remove_task", "task"), &LimboAIEditor::_remove_task);
	ClassDB::bind_method(D_METHOD("_add_task_with_prototype", "prototype_task"), &LimboAIEditor::_add_task_with_prototype);
	ClassDB::bind_method(D_METHOD("_new_bt"), &LimboAIEditor::_new_bt);
	ClassDB::bind_method(D_METHOD("_save_bt", "path"), &LimboAIEditor::_save_current_bt);
	ClassDB::bind_method(D_METHOD("_load_resource", "path"), &LimboAIEditor::_load_resource);
	ClassDB::bind_method(D_METHOD("_update_task_tree", "bt", "specific_task"), &LimboAIEditor::_update_task_tree, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("edit_bt", "behavior_tree", "force_refresh"), &LimboAIEditor::edit_bt, Variant(false));
	ClassDB::bind_method(D_METHOD("_reload_modified"), &LimboAIEditor::_reload_modified);
	ClassDB::bind_method(D_METHOD("_resave_modified"), &LimboAIEditor::_resave_modified);
	ClassDB::bind_method(D_METHOD("_replace_task", "task", "by_task"), &LimboAIEditor::_replace_task);
	ClassDB::bind_method(D_METHOD("_popup_file_dialog"), &LimboAIEditor::_popup_file_dialog);
	ClassDB::bind_method(D_METHOD("get_edited_blackboard_plan"), &LimboAIEditor::get_edited_blackboard_plan);
}

LimboAIEditor::LimboAIEditor() {
	plugin = nullptr;
	idx_history = -1;
	dummy_history_context = memnew(Object);

	EDITOR_DEF("limbo_ai/editor/prefer_online_documentation", false);

	EDITOR_DEF("limbo_ai/editor/layout", 0);
#ifdef LIMBOAI_MODULE
	EDITOR_SETTINGS()->add_property_hint(PropertyInfo(Variant::INT, "limbo_ai/editor/layout", PROPERTY_HINT_ENUM, "Classic:0,Widescreen Optimized:1"));
	EDITOR_SETTINGS()->set_restart_if_changed("limbo_ai/editor/layout", true);
#elif LIMBOAI_GDEXTENSION
	Dictionary pinfo;
	pinfo["name"] = "limbo_ai/editor/layout";
	pinfo["type"] = Variant::INT;
	pinfo["hint"] = PROPERTY_HINT_ENUM;
	pinfo["hint_string"] = "Classic:0,Widescreen Optimized:1";
	EDITOR_SETTINGS()->add_property_info(pinfo);
#endif

	LW_SHORTCUT("limbo_ai/rename_task", TTR("Rename"), LW_KEY(F2));
	// Todo: Add override support for shortcuts.
	// LW_SHORTCUT_OVERRIDE("limbo_ai/rename_task", "macos", Key::ENTER);
	LW_SHORTCUT("limbo_ai/move_task_up", TTR("Move Up"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY(UP)));
	LW_SHORTCUT("limbo_ai/move_task_down", TTR("Move Down"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY(DOWN)));
	LW_SHORTCUT("limbo_ai/duplicate_task", TTR("Duplicate"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY(D)));
	LW_SHORTCUT("limbo_ai/remove_task", TTR("Remove"), Key::KEY_DELETE);
	LW_SHORTCUT("limbo_ai/cut_task", TTR("Cut"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY(X)));
	LW_SHORTCUT("limbo_ai/copy_task", TTR("Copy"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY(C)));
	LW_SHORTCUT("limbo_ai/paste_task", TTR("Paste"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY(V)));
	LW_SHORTCUT("limbo_ai/paste_task_after", TTR("Paste After Selected"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY_MASK(SHIFT) | LW_KEY(V)));

	LW_SHORTCUT("limbo_ai/new_behavior_tree", TTR("New Behavior Tree"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY_MASK(ALT) | LW_KEY(N)));
	// The same keys as above, and deliberately so: only one of the two is ever on the New
	// button, and which one it is says what the button is about to make.
	LW_SHORTCUT("limbo_ai/new_state_machine", TTR("New State Machine"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY_MASK(ALT) | LW_KEY(N)));
	LW_SHORTCUT("limbo_ai/save_behavior_tree", TTR("Save Behavior Tree"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY_MASK(ALT) | LW_KEY(S)));
	LW_SHORTCUT("limbo_ai/load_behavior_tree", TTR("Load Behavior Tree"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY_MASK(ALT) | LW_KEY(L)));
	LW_SHORTCUT("limbo_ai/open_debugger", TTR("Open Debugger"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY_MASK(ALT) | LW_KEY(D)));
	LW_SHORTCUT("limbo_ai/jump_to_owner", TTR("Jump to Owner"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY(G)));
	LW_SHORTCUT("limbo_ai/close_tab", TTR("Close Tab"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY(W)));
	LW_SHORTCUT("limbo_ai/find_task", TTR("Find Task"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY(F)));
	LW_SHORTCUT("limbo_ai/hide_tree_search", TTR("Close Search"), (Key)(LW_KEY(ESCAPE)));

	// Intercept editor save scene action.
	LW_SHORTCUT("limbo_ai/editor_save_scene", TTR("Save Scene"), (Key)(LW_KEY_MASK(CMD_OR_CTRL) | LW_KEY(S)));

	set_process_shortcut_input(true);

	save_dialog = memnew(FileDialog);
	save_dialog->set_file_mode(FileDialog::FILE_MODE_SAVE_FILE);
	save_dialog->set_title(TTR("Save Behavior Tree"));
	save_dialog->add_filter("*.tres");
	save_dialog->hide();
	add_child(save_dialog);

	load_dialog = memnew(FileDialog);
	load_dialog->set_file_mode(FileDialog::FILE_MODE_OPEN_FILE);
	// Not per document kind, unlike the save dialog: saving is about the one thing already
	// open, opening is about anything on disk.
	load_dialog->set_title(TTR("Load Behavior Tree or State Machine"));
	load_dialog->add_filter("*.tres");
	load_dialog->hide();
	add_child(load_dialog);

	extract_dialog = memnew(FileDialog);
	extract_dialog->set_file_mode(FileDialog::FILE_MODE_SAVE_FILE);
	extract_dialog->set_title(TTR("Save Extracted Tree"));
	extract_dialog->add_filter("*.tres");
	extract_dialog->hide();
	add_child(extract_dialog);

	vbox = memnew(VBoxContainer);
	vbox->set_anchor(SIDE_RIGHT, ANCHOR_END);
	vbox->set_anchor(SIDE_BOTTOM, ANCHOR_END);
	add_child(vbox);

	// Above the toolbar, because the toolbar answers to it: which buttons stand there is
	// decided by which tab is on. Reading downward, the tabs say what the document is and
	// the row under them says what can be done to it.
	tab_bar_panel = memnew(PanelContainer);
	vbox->add_child(tab_bar_panel);
	tab_bar_container = memnew(HBoxContainer);
	tab_bar_panel->add_child(tab_bar_container);

	// The All tab stands on its own bar, outside the scrolled strip, so it stays at the
	// left end however far the documents are scrolled.
	all_tab_bar = memnew(TabBar);
	all_tab_bar->set_deselect_enabled(true);
	all_tab_bar->set_select_with_rmb(false);
	all_tab_bar->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	all_tab_bar->set_focus_mode(FocusMode::FOCUS_NONE);
	tab_bar_container->add_child(all_tab_bar);

	// The window onto the document tabs; see _layout_tab_strip().
	tab_clip = memnew(Control);
	tab_clip->set_clip_contents(true);
	tab_clip->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	tab_clip->set_mouse_filter(MOUSE_FILTER_PASS);
	tab_bar_container->add_child(tab_clip);

	tab_bar = memnew(TabBar);
	// Laid out at full width and never clipped by itself: no arrow buttons at its end.
	tab_bar->set_clip_tabs(false);
	tab_bar->set_deselect_enabled(true);
	// A right-click opens the tab menu over the tab without making it the active one.
	tab_bar->set_select_with_rmb(false);
	tab_bar->set_drag_to_rearrange_enabled(true);
	tab_bar->set_max_tab_width(int(EDITOR_GET("interface/scene_tabs/maximum_width")) * EDSCALE);
	tab_bar->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	// Closing goes through each tab's button slot instead; see _update_tab_close_buttons().
	tab_bar->set_tab_close_display_policy(TabBar::CLOSE_BUTTON_SHOW_NEVER);
	tab_bar->set_focus_mode(FocusMode::FOCUS_NONE);
	tab_clip->add_child(tab_bar);

	tab_scroll_hint = memnew(TextureRect);
	tab_scroll_hint->set_anchor(SIDE_BOTTOM, ANCHOR_END);
	tab_scroll_hint->set_custom_minimum_size(Size2(16, 0) * EDSCALE);
	tab_scroll_hint->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	tab_scroll_hint->set_stretch_mode(TextureRect::STRETCH_SCALE);
	tab_scroll_hint->set_mouse_filter(MOUSE_FILTER_IGNORE);
	tab_scroll_hint->hide();
	tab_clip->add_child(tab_scroll_hint);

	// Over the lower edge of the tabs rather than on a row of its own, so that showing it
	// on hover does not push the toolbar down.
	tab_hscroll = memnew(HScrollBar);
	tab_hscroll->set_anchor(SIDE_TOP, ANCHOR_END);
	tab_hscroll->set_anchor(SIDE_RIGHT, ANCHOR_END);
	tab_hscroll->set_anchor(SIDE_BOTTOM, ANCHOR_END);
	tab_hscroll->set_focus_mode(FocusMode::FOCUS_NONE);
	tab_hscroll->hide();
	tab_clip->add_child(tab_hscroll);

	HBoxContainer *toolbar = memnew(HBoxContainer);
	vbox->add_child(toolbar);

	PackedStringArray favorite_tasks_default;
	favorite_tasks_default.append("BTSelector");
	favorite_tasks_default.append("BTSequence");
	favorite_tasks_default.append("BTComment");
	GLOBAL_DEF(PropertyInfo(Variant::PACKED_STRING_ARRAY, "limbo_ai/behavior_tree/favorite_tasks", PROPERTY_HINT_ARRAY_TYPE, "String"), favorite_tasks_default);

	fav_tasks_hbox = memnew(HBoxContainer);
	toolbar->add_child(fav_tasks_hbox);

	// The machine's counterpart to the favorite tasks, and in the same place: what the left
	// end of this row offers is always "the pieces the open document is made of".
	hsm_tools_hbox = memnew(HBoxContainer);
	toolbar->add_child(hsm_tools_hbox);

	hsm_add_state_btn = memnew(Button);
	hsm_add_state_btn->set_text(TTR("Add State"));
	hsm_add_state_btn->set_tooltip_text(TTR("Add a state inside the selected one, which is what makes it a machine."));
	hsm_add_state_btn->set_flat(true);
	hsm_add_state_btn->set_focus_mode(Control::FOCUS_NONE);
	hsm_tools_hbox->add_child(hsm_add_state_btn);

	hsm_add_machine_btn = memnew(Button);
	hsm_add_machine_btn->set_text(TTR("Add Machine"));
	hsm_add_machine_btn->set_tooltip_text(TTR("Add a state that already holds one, so that it is drawn as a machine."));
	hsm_add_machine_btn->set_flat(true);
	hsm_add_machine_btn->set_focus_mode(Control::FOCUS_NONE);
	hsm_tools_hbox->add_child(hsm_add_machine_btn);

	hsm_remove_btn = memnew(Button);
	hsm_remove_btn->set_text(TTR("Remove"));
	hsm_remove_btn->set_tooltip_text(TTR("Remove the selected state."));
	hsm_remove_btn->set_flat(true);
	hsm_remove_btn->set_focus_mode(Control::FOCUS_NONE);
	hsm_tools_hbox->add_child(hsm_remove_btn);

	docs_sep = memnew(VSeparator);
	toolbar->add_child(docs_sep);

	new_btn = memnew(Button);
	new_btn->set_flat(true);
	new_btn->set_focus_mode(Control::FOCUS_NONE);
	toolbar->add_child(new_btn);

	load_btn = memnew(Button);
	load_btn->set_text(TTR("Load"));
	load_btn->set_tooltip_text(TTR("Load a behavior tree or state machine from a resource file."));
	load_btn->set_shortcut(LW_GET_SHORTCUT("limbo_ai/load_behavior_tree"));
	load_btn->set_flat(true);
	load_btn->set_focus_mode(Control::FOCUS_NONE);
	toolbar->add_child(load_btn);

	save_btn = memnew(Button);
	save_btn->set_text(TTR("Save"));
	save_btn->set_tooltip_text(TTR("Save the edited document to a resource file."));
	save_btn->set_shortcut(LW_GET_SHORTCUT("limbo_ai/save_behavior_tree"));
	save_btn->set_flat(true);
	save_btn->set_focus_mode(Control::FOCUS_NONE);
	toolbar->add_child(save_btn);

	script_sep = memnew(VSeparator);
	toolbar->add_child(script_sep);

	new_script_btn = memnew(Button);
	new_script_btn->set_text(TTR("New Task"));
	new_script_btn->set_tooltip_text(TTR("Create new task script and edit it."));
	new_script_btn->set_flat(true);
	new_script_btn->set_focus_mode(Control::FOCUS_NONE);
	toolbar->add_child(new_script_btn);

	misc_btn = memnew(MenuButton);
	misc_btn->set_text(TTR("Misc"));
	misc_btn->set_flat(true);
	toolbar->add_child(misc_btn);

	blackboard_btn = memnew(Button);
	blackboard_btn->set_text(TTR("Blackboard"));
	blackboard_btn->set_tooltip_text(TTR("Edit the blackboard plan of the open behavior tree in the Inspector."));
	blackboard_btn->set_flat(true);
	blackboard_btn->set_focus_mode(Control::FOCUS_NONE);
	toolbar->add_child(blackboard_btn);

	HBoxContainer *version_hbox = memnew(HBoxContainer);
	version_hbox->set_h_size_flags(SIZE_EXPAND | SIZE_SHRINK_END);
	toolbar->add_child(version_hbox);

	TextureRect *logo = memnew(TextureRect);
	logo->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	logo->set_texture(LimboUtility::get_singleton()->get_task_icon("LimboAI"));
	version_hbox->add_child(logo);

	version_btn = memnew(LinkButton);
	version_btn->set_text(TTR("v") + String(GET_LIMBOAI_FULL_VERSION()));
	version_btn->set_tooltip_text(TTR("Click to copy."));
	version_btn->set_self_modulate(Color(1, 1, 1, 0.65));
	version_btn->set_underline_mode(LinkButton::UNDERLINE_MODE_ON_HOVER);
	version_btn->set_v_size_flags(SIZE_SHRINK_CENTER);
	version_hbox->add_child(version_btn);

	Control *version_spacer = memnew(Control);
	version_spacer->set_custom_minimum_size(Size2(2, 0) * EDSCALE);
	version_hbox->add_child(version_spacer);

	tab_menu = memnew(PopupMenu);
	add_child(tab_menu);

	owner_picker = memnew(OwnerPicker);
	add_child(owner_picker);

	hsc = memnew(HSplitContainer);
	hsc->set_h_size_flags(SIZE_EXPAND_FILL);
	hsc->set_v_size_flags(SIZE_EXPAND_FILL);
	hsc->set_focus_mode(FOCUS_NONE);
	vbox->add_child(hsc);

	task_tree = memnew(TaskTree);
	task_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	task_tree->set_h_size_flags(SIZE_EXPAND_FILL);
	task_tree->hide();
	hsc->add_child(task_tree);

	// Beside the task tree, not beside `hsc`. The two are the same kind of thing -- the body
	// of the open document -- and standing in the same place is what lets the toolbar and the
	// tab bar stay up while they take turns. As a sibling of `hsc` the only way to raise one
	// was to hide the other's whole container, and the widescreen layout keeps those two rows
	// inside it.
	//
	// `hsc` splits the first two of its *visible* children, so at most two of the four ever
	// stand at once: tree + palette, machine alone, or the All tab's browser alone.
	hsm_view = memnew(HSMEditorView);
	hsm_view->hide();
	hsc->add_child(hsm_view);
	hsm_view->connect("hsm_edited", callable_mp(this, &LimboAIEditor::_on_hsm_edited));

	// The All tab's view. It takes the place the old "create or load" hint had: with no
	// document open the All tab is the one on, and the list is how one gets opened.
	browser = memnew(LimboResourceBrowser);
	hsc->add_child(browser);

	task_palette = memnew(TaskPalette());
	task_palette->hide();
	hsc->add_child(task_palette);
	// What gets put away when the palette is not wanted. In the classic layout that is the
	// palette itself; the widescreen one wraps it in a column with a heading over it, and
	// hiding only the palette would leave the heading standing over nothing.
	palette_holder = task_palette;

	// Nothing is open yet, and set_window_layout() may not reopen anything either -- so the
	// starting state is settled here rather than left to the first document.
	_show_all_tab();

	banners = memnew(VBoxContainer);
	vbox->add_child(banners);

	editor_layout = (EditorLayout)(int)EDITOR_GET("limbo_ai/editor/layout");
	if (editor_layout == LAYOUT_WIDESCREEN_OPTIMIZED) {
		// * Alternative layout optimized for wide screen.
		VBoxContainer *sidebar_vbox = memnew(VBoxContainer);
		hsc->add_child(sidebar_vbox);
		sidebar_vbox->set_v_size_flags(SIZE_EXPAND_FILL);

		HBoxContainer *header_bar = memnew(HBoxContainer);
		sidebar_vbox->add_child(header_bar);
		Control *header_spacer = memnew(Control);
		header_bar->add_child(header_spacer);
		header_spacer->set_custom_minimum_size(Size2(6, 0) * EDSCALE);
		TextureRect *header_logo = Object::cast_to<TextureRect>(logo->duplicate());
		header_bar->add_child(header_logo);
		Label *header_title = memnew(Label);
		header_bar->add_child(header_title);
		header_title->set_text(TTR("Behavior Tree Editor"));
		header_title->set_v_size_flags(SIZE_SHRINK_CENTER);
		header_title->set_theme_type_variation("HeaderMedium");

		task_palette->reparent(sidebar_vbox);
		task_palette->set_v_size_flags(SIZE_EXPAND_FILL);
		// The palette is now inside a column with a heading, so the column is what goes away
		// with it -- see palette_holder. The "put away" state moves with the job: the palette
		// itself goes back to visible, and the column starts down, or the screen opens on a
		// heading with nothing under it.
		task_palette->show();
		sidebar_vbox->hide();
		palette_holder = sidebar_vbox;

		VBoxContainer *editor_vbox = memnew(VBoxContainer);
		hsc->add_child(editor_vbox);
		tab_bar_panel->reparent(editor_vbox);
		toolbar->reparent(editor_vbox);
		task_tree->reparent(editor_vbox);
		// The machine goes wherever the tree goes: the two take turns in one place.
		hsm_view->reparent(editor_vbox);
		browser->reparent(editor_vbox);
		banners->reparent(editor_vbox);
	}

	hsc->set_split_offset((editor_layout == LAYOUT_CLASSIC ? -320 : 320) * EDSCALE);

	change_type_popup = memnew(PopupPanel);
	add_child(change_type_popup);
	{
		VBoxContainer *change_type_vbox = memnew(VBoxContainer);
		change_type_popup->add_child(change_type_vbox);

		Label *change_type_title = memnew(Label);
		change_type_vbox->add_child(change_type_title);
		change_type_title->set_theme_type_variation("HeaderSmall");
		change_type_title->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		change_type_title->set_text(TTR("Choose New Type"));

		change_type_palette = memnew(TaskPalette);
		change_type_vbox->add_child(change_type_palette);
		change_type_palette->use_dialog_mode();
		change_type_palette->set_v_size_flags(SIZE_EXPAND_FILL);
	}

	menu = memnew(PopupMenu);
	add_child(menu);

	probability_popup = memnew(PopupPanel);
	{
		VBoxContainer *vbc = memnew(VBoxContainer);
		probability_popup->add_child(vbc);

		PanelContainer *mode_panel = memnew(PanelContainer);
		vbc->add_child(mode_panel);

		HBoxContainer *mode_hbox = memnew(HBoxContainer);
		mode_panel->add_child(mode_hbox);

		Ref<ButtonGroup> button_group;
		button_group.instantiate();

		weight_mode = memnew(Button);
		mode_hbox->add_child(weight_mode);
		weight_mode->set_toggle_mode(true);
		weight_mode->set_button_group(button_group);
		weight_mode->set_focus_mode(Control::FOCUS_NONE);
		weight_mode->set_text(TTR("Weight"));
		weight_mode->set_tooltip_text(TTR("Edit weight"));
		weight_mode->set_pressed_no_signal(true);

		percent_mode = memnew(Button);
		mode_hbox->add_child(percent_mode);
		percent_mode->set_toggle_mode(true);
		percent_mode->set_button_group(button_group);
		percent_mode->set_focus_mode(Control::FOCUS_NONE);
		percent_mode->set_text(TTR("Percent"));
		percent_mode->set_tooltip_text(TTR("Edit percent"));

		probability_edit = memnew(EditorSpinSlider);
		vbc->add_child(probability_edit);
		probability_edit->set_min(0.0);
		probability_edit->set_max(10.0);
		probability_edit->set_step(0.01);
		probability_edit->set_allow_greater(true);
		probability_edit->set_custom_minimum_size(Size2(200.0 * EDSCALE, 0.0));
	}
	add_child(probability_popup);

	rename_dialog = memnew(ConfirmationDialog);
	{
		VBoxContainer *vbc = memnew(VBoxContainer);
		rename_dialog->add_child(vbc);

		rename_edit = memnew(LineEdit);
		vbc->add_child(rename_edit);
		rename_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		rename_edit->set_custom_minimum_size(Size2(350.0, 0.0));

		rename_dialog->register_text_enter(rename_edit);
	}
	add_child(rename_dialog);

	disk_changed = memnew(ConfirmationDialog);
	{
		VBoxContainer *vbc = memnew(VBoxContainer);
		disk_changed->add_child(vbc);

		Label *dl = memnew(Label);
		dl->set_text(TTR("The following BehaviorTree resources are newer on disk.\nWhat action should be taken?"));
		vbc->add_child(dl);

		disk_changed_list = memnew(Tree);
		vbc->add_child(disk_changed_list);
		disk_changed_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);

		disk_changed->get_ok_button()->set_text(TTR("Reload"));
		disk_changed->add_button(TTR("Resave"), !DisplayServer::get_singleton()->get_swap_cancel_ok(), "resave");
	}

	info_dialog = memnew(AcceptDialog);
	add_child(info_dialog);

	EditorInterface::get_singleton()->get_base_control()->add_child(disk_changed);

	GLOBAL_DEF(PropertyInfo(Variant::STRING, "limbo_ai/behavior_tree/behavior_tree_default_dir", PROPERTY_HINT_DIR), "res://ai/trees");
	PackedStringArray user_task_dir_default;
	user_task_dir_default.append("res://ai/tasks");
	GLOBAL_DEF(PropertyInfo(Variant::PACKED_STRING_ARRAY, "limbo_ai/behavior_tree/user_task_dirs", PROPERTY_HINT_TYPE_STRING, vformat("%s/%s:", Variant::STRING, PROPERTY_HINT_DIR)), user_task_dir_default);

	String bt_default_dir = GLOBAL_GET("limbo_ai/behavior_tree/behavior_tree_default_dir");
	save_dialog->set_current_dir(bt_default_dir);
	load_dialog->set_current_dir(bt_default_dir);
	extract_dialog->set_current_dir(bt_default_dir);
}

LimboAIEditor::~LimboAIEditor() {
	memdelete(dummy_history_context);
}

//**** LimboAIEditor ^

//**** LimboAIEditorPlugin

void LimboAIEditorPlugin::_bind_methods() {
}

void LimboAIEditorPlugin::_notification(int p_notification) {
	switch (p_notification) {
		case NOTIFICATION_READY: {
			add_debugger_plugin(memnew(LimboDebuggerPlugin));
			add_inspector_plugin(memnew(EditorInspectorPluginBBPlan));

			EditorInspectorPluginVariableName *var_plugin = memnew(EditorInspectorPluginVariableName);
			var_plugin->set_editor_plan_provider(Callable(limbo_ai_editor, "get_edited_blackboard_plan"));
			add_inspector_plugin(var_plugin);

			EditorInspectorPluginPropertyPath *path_plugin = memnew(EditorInspectorPluginPropertyPath);
			add_inspector_plugin(path_plugin);

			EditorInspectorPluginBBParam *param_plugin = memnew(EditorInspectorPluginBBParam);
			param_plugin->set_plan_getter(Callable(limbo_ai_editor, "get_edited_blackboard_plan"));
			add_inspector_plugin(param_plugin);

			EditorInspectorPluginHSMState *state_plugin = memnew(EditorInspectorPluginHSMState);
			state_plugin->set_view(limbo_ai_editor->get_hsm_view());
			add_inspector_plugin(state_plugin);
		} break;
		case NOTIFICATION_ENTER_TREE: {
			// Add BehaviorTree to the list of resources that should open in a new inspector.
			PackedStringArray open_in_new_inspector = EDITOR_GET("interface/inspector/resources_to_open_in_new_inspector");
			if (!open_in_new_inspector.has("BehaviorTree") || !open_in_new_inspector.has("HierarchicalStateMachine")) {
				if (!open_in_new_inspector.has("BehaviorTree")) {
					open_in_new_inspector.push_back("BehaviorTree");
				}
				if (!open_in_new_inspector.has("HierarchicalStateMachine")) {
					open_in_new_inspector.push_back("HierarchicalStateMachine");
				}
				EDITOR_SETTINGS()->set_setting("interface/inspector/resources_to_open_in_new_inspector", open_in_new_inspector);
			}
		} break;
	}
}

#ifdef LIMBOAI_MODULE
void LimboAIEditorPlugin::make_visible(bool p_visible) {
#elif LIMBOAI_GDEXTENSION
void LimboAIEditorPlugin::_make_visible(bool p_visible) {
#endif
	limbo_ai_editor->set_visible(p_visible);
}

#ifdef LIMBOAI_MODULE
void LimboAIEditorPlugin::get_window_layout(Ref<ConfigFile> p_configuration) {
#elif LIMBOAI_GDEXTENSION
void LimboAIEditorPlugin::_get_window_layout(const Ref<ConfigFile> &p_configuration) {
#endif
	limbo_ai_editor->get_window_layout(p_configuration);
}

#ifdef LIMBOAI_MODULE
void LimboAIEditorPlugin::set_window_layout(Ref<ConfigFile> p_configuration) {
#elif LIMBOAI_GDEXTENSION
void LimboAIEditorPlugin::_set_window_layout(const Ref<ConfigFile> &p_configuration) {
#endif
	limbo_ai_editor->set_window_layout(p_configuration);
}

#ifdef LIMBOAI_MODULE
void LimboAIEditorPlugin::edit(Object *p_object) {
#elif LIMBOAI_GDEXTENSION
void LimboAIEditorPlugin::_edit(Object *p_object) {
#endif
	Ref<BehaviorTree> bt = Object::cast_to<BehaviorTree>(p_object);
	if (bt.is_valid()) {
		limbo_ai_editor->edit_bt(bt);
		return;
	}
	Ref<HierarchicalStateMachine> hsm = Object::cast_to<HierarchicalStateMachine>(p_object);
	if (hsm.is_valid()) {
		limbo_ai_editor->edit_hsm(hsm);
	}
}

#ifdef LIMBOAI_MODULE
bool LimboAIEditorPlugin::handles(Object *p_object) const {
#elif LIMBOAI_GDEXTENSION
bool LimboAIEditorPlugin::_handles(Object *p_object) const {
#endif
	if (Object::cast_to<BehaviorTree>(p_object)) {
		return true;
	}
	if (Object::cast_to<HierarchicalStateMachine>(p_object)) {
		return true;
	}
	return false;
}

#ifdef LIMBOAI_MODULE
void LimboAIEditorPlugin::save_external_data() {
#elif LIMBOAI_GDEXTENSION
void LimboAIEditorPlugin::_save_external_data() {
#endif
	limbo_ai_editor->save_all(true);
}

#ifdef LIMBOAI_GDEXTENSION
Ref<Texture2D> LimboAIEditorPlugin::_get_plugin_icon() const {
	return LimboUtility::get_singleton()->get_task_icon("LimboAI");
}
#endif // LIMBOAI_GDEXTENSION

LimboAIEditorPlugin::LimboAIEditorPlugin() {
	limbo_ai_editor = memnew(LimboAIEditor());
	limbo_ai_editor->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	EditorInterface::get_singleton()->get_editor_main_screen()->add_child(limbo_ai_editor);
	limbo_ai_editor->hide();
	limbo_ai_editor->set_plugin(this);
}

LimboAIEditorPlugin::~LimboAIEditorPlugin() {
}

#endif // ! TOOLS_ENABLED
