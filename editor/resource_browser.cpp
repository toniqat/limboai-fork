/**
 * resource_browser.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#include "resource_browser.h"

#include "../bt/behavior_tree.h"
#include "../compat/editor_scale.h"
#include "../compat/limbo_compat.h"
#include "../compat/resource_loader.h"
#include "../compat/translation.h"
#include "../hsm/hsm_resource.h"
#include "../util/limbo_utility.h"
#include "hsm_editor_view.h"

#ifdef LIMBOAI_MODULE
#include "editor/docks/filesystem_dock.h"
#include "servers/display_server.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/file_system_dock.hpp>
#include <godot_cpp/classes/tree_item.hpp>
#endif // LIMBOAI_GDEXTENSION

namespace {

enum ItemMenu {
	ITEM_OPEN,
	ITEM_SHOW_IN_FILESYSTEM,
	ITEM_COPY_PATH,
	ITEM_FIND_OWNERS,
};

// Both the file tree and the preview need this much to be worth reading; below their sum
// the preview is put away and the file tree gets the whole width.
const float FILE_TREE_MIN_WIDTH = 250.0;
const float PREVIEW_MIN_WIDTH = 250.0;

} // namespace

bool LimboResourceBrowser::_is_ai_type(const String &p_type) {
	return p_type == "BehaviorTree" || p_type == "HierarchicalStateMachine";
}

bool LimboResourceBrowser::_has_ai_files(EditorFileSystemDirectory *p_dir) {
	for (int i = 0; i < p_dir->get_file_count(); i++) {
		if (_is_ai_type(String(p_dir->get_file_type(i)))) {
			return true;
		}
	}
	for (int i = 0; i < p_dir->get_subdir_count(); i++) {
		if (_has_ai_files(p_dir->get_subdir(i))) {
			return true;
		}
	}
	return false;
}

void LimboResourceBrowser::_fill_dir(EditorFileSystemDirectory *p_dir, TreeItem *p_item) {
	String dir_path = p_dir->get_path();
	p_item->set_text(0, dir_path == "res://" ? String("res://") : p_dir->get_name());
	p_item->set_metadata(0, dir_path);
	p_item->set_tooltip_text(0, dir_path);
	p_item->set_icon(0, folder_icon);
	p_item->set_icon_modulate(0, folder_color);
	p_item->set_collapsed(collapsed_dirs.has(dir_path));

	// Folders before files, each in the file system's own (alphabetical) order, as the
	// FileSystem dock lists them.
	for (int i = 0; i < p_dir->get_subdir_count(); i++) {
		EditorFileSystemDirectory *sub = p_dir->get_subdir(i);
		if (_has_ai_files(sub)) {
			_fill_dir(sub, file_tree->create_item(p_item));
		}
	}

	for (int i = 0; i < p_dir->get_file_count(); i++) {
		String type = p_dir->get_file_type(i);
		if (!_is_ai_type(type)) {
			continue;
		}
		String path = p_dir->get_file_path(i);
		TreeItem *file_item = file_tree->create_item(p_item);
		file_item->set_text(0, p_dir->get_file(i));
		file_item->set_metadata(0, path);
		file_item->set_tooltip_text(0, path);
		// The same pictures the tabs use, so a file reads the same here and once opened.
		file_item->set_icon(0, LimboUtility::get_singleton()->get_task_icon(type == "HierarchicalStateMachine" ? "LimboHSM" : "BehaviorTree"));
		file_item->set_icon_max_width(0, 16 * EDSCALE);
		if (path == previewed_path) {
			file_item->select(0);
		}
	}
}

void LimboResourceBrowser::_rebuild() {
	list_dirty = false;
	rebuilding = true;
	file_tree->clear();

	EditorFileSystemDirectory *root_dir = EDITOR_FILE_SYSTEM()->get_filesystem();
	bool previewed_found = false;
	if (root_dir != nullptr) {
		TreeItem *root = file_tree->create_item();
		_fill_dir(root_dir, root);
		TreeItem *selected = file_tree->get_selected();
		previewed_found = selected != nullptr && String(selected->get_metadata(0)) == previewed_path;
	}
	rebuilding = false;

	// The file it was showing is gone, or moved: there is nothing left to show.
	_show_preview(previewed_found ? previewed_path : String());
}

void LimboResourceBrowser::_show_preview(const String &p_path) {
	previewed_path = p_path;

	Ref<Resource> res;
	if (!p_path.is_empty() && FILE_EXISTS(p_path)) {
		res = RESOURCE_LOAD(p_path, "Resource");
	}
	Ref<BehaviorTree> bt = res;
	Ref<HierarchicalStateMachine> hsm = res;

	if (bt.is_valid()) {
		if (bt_preview->get_bt() == bt) {
			// Shown again after an edit in its own tab, or after a rescan: the rows are
			// rebuilt from what the tree holds now.
			bt_preview->update_tree();
		} else {
			bt_preview->load_bt(bt);
		}
	} else {
		bt_preview->unload();
	}

	hsm_preview->clear();
	if (hsm.is_valid() && hsm->get_effective_root().is_valid()) {
		// The merged root, as the machine's own tab draws it: what a child file inherits is
		// part of what it is.
		Ref<HSMStateSpec> root = hsm->get_effective_root();
		TreeItem *root_item = hsm_preview->create_item();
		root_item->set_text(0, p_path.get_file());
		root_item->set_icon(0, LimboUtility::get_singleton()->get_task_icon("LimboHSM"));
		root_item->set_icon_max_width(0, 16 * EDSCALE);
		HSMEditorView::populate_state_list(hsm_preview, root, root_item);
	}

	bt_preview->set_visible(bt.is_valid());
	hsm_preview->set_visible(hsm.is_valid());
	_update_preview_visibility();
}

void LimboResourceBrowser::_update_preview_visibility() {
	bool has_preview = bt_preview->is_visible() || hsm_preview->is_visible();
	float needed = (FILE_TREE_MIN_WIDTH + PREVIEW_MIN_WIDTH) * EDSCALE + get_theme_constant("separation");
	bool fits = get_size().x >= needed;
	if (preview_box->is_visible() != (has_preview && fits)) {
		preview_box->set_visible(has_preview && fits);
	}
}

void LimboResourceBrowser::_on_item_selected() {
	if (rebuilding) {
		return;
	}
	TreeItem *item = file_tree->get_selected();
	String path = item != nullptr ? String(item->get_metadata(0)) : String();
	// A folder has nothing to outline.
	_show_preview(path.ends_with("/") ? String() : path);
}

void LimboResourceBrowser::_on_item_activated() {
	TreeItem *item = file_tree->get_selected();
	if (item == nullptr) {
		return;
	}
	String path = item->get_metadata(0);
	if (path.ends_with("/")) {
		item->set_collapsed(!item->is_collapsed());
		return;
	}
	emit_signal("resource_activated", path);
}

void LimboResourceBrowser::_on_item_collapsed(TreeItem *p_item) {
	if (rebuilding || p_item == nullptr) {
		return;
	}
	String path = p_item->get_metadata(0);
	if (!path.ends_with("/")) {
		return;
	}
	if (p_item->is_collapsed()) {
		collapsed_dirs.insert(path);
	} else {
		collapsed_dirs.erase(path);
	}
}

void LimboResourceBrowser::_on_item_mouse_selected(const Vector2 &p_pos, int p_button) {
	if (p_button != int(LW_MBTN(RIGHT))) {
		return;
	}
	TreeItem *item = file_tree->get_selected();
	if (item == nullptr) {
		return;
	}
	menu_path = item->get_metadata(0);
	bool is_dir = menu_path.ends_with("/");

	item_menu->clear();
	if (!is_dir) {
		item_menu->add_icon_item(get_theme_icon("Load", "EditorIcons"), TTR("Open"), ITEM_OPEN);
		item_menu->add_separator();
	}
	item_menu->add_icon_item(get_theme_icon("Filesystem", "EditorIcons"), TTR("Show in FileSystem"), ITEM_SHOW_IN_FILESYSTEM);
	item_menu->add_icon_item(get_theme_icon("ActionCopy", "EditorIcons"), TTR("Copy Path"), ITEM_COPY_PATH);
	if (!is_dir) {
		item_menu->add_item(TTR("Find Owners"), ITEM_FIND_OWNERS);
	}
	item_menu->set_position(get_screen_position() + get_local_mouse_position());
	item_menu->reset_size();
	item_menu->popup();
}

void LimboResourceBrowser::_on_item_menu_pressed(int p_id) {
	if (menu_path.is_empty()) {
		return;
	}
	switch (p_id) {
		case ITEM_OPEN: {
			emit_signal("resource_activated", menu_path);
		} break;
		case ITEM_SHOW_IN_FILESYSTEM: {
			EditorInterface::get_singleton()->get_file_system_dock()->navigate_to_path(menu_path);
		} break;
		case ITEM_COPY_PATH: {
			DisplayServer::get_singleton()->clipboard_set(menu_path);
		} break;
		case ITEM_FIND_OWNERS: {
			emit_signal("owners_requested", menu_path);
		} break;
	}
}

void LimboResourceBrowser::_on_filesystem_changed() {
	list_dirty = true;
	if (is_visible_in_tree()) {
		_rebuild();
	}
}

void LimboResourceBrowser::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			file_tree->connect("item_selected", callable_mp(this, &LimboResourceBrowser::_on_item_selected));
			file_tree->connect("item_activated", callable_mp(this, &LimboResourceBrowser::_on_item_activated));
			file_tree->connect("item_collapsed", callable_mp(this, &LimboResourceBrowser::_on_item_collapsed));
			EDITOR_FILE_SYSTEM()->connect("filesystem_changed", callable_mp(this, &LimboResourceBrowser::_on_filesystem_changed));
			file_tree->connect("item_mouse_selected", callable_mp(this, &LimboResourceBrowser::_on_item_mouse_selected));
			item_menu->connect("id_pressed", callable_mp(this, &LimboResourceBrowser::_on_item_menu_pressed));
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			folder_icon = get_theme_icon("Folder", "EditorIcons");
			folder_color = get_theme_color("folder_icon_color", "FileDialog");
			// The same darker body the task tree has, so both previews read alike.
			ADD_STYLEBOX_OVERRIDE(hsm_preview, "panel", TaskTree::make_dark_tree_panel(this));
			list_dirty = true;
			if (is_visible_in_tree()) {
				_rebuild();
			}
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (is_visible_in_tree()) {
				if (list_dirty) {
					_rebuild();
				} else {
					// The previewed file may have been edited in its tab since it was last
					// shown here.
					_show_preview(previewed_path);
				}
			}
		} break;
		case NOTIFICATION_RESIZED: {
			_update_preview_visibility();
		} break;
	}
}

void LimboResourceBrowser::_bind_methods() {
	ADD_SIGNAL(MethodInfo("resource_activated", PropertyInfo(Variant::STRING, "path")));
	ADD_SIGNAL(MethodInfo("owners_requested", PropertyInfo(Variant::STRING, "path")));
}

LimboResourceBrowser::LimboResourceBrowser() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	// Hidden until the All tab is the one on; LimboAIEditor shows it.
	hide();

	file_tree = memnew(Tree);
	file_tree->set_custom_minimum_size(Size2(FILE_TREE_MIN_WIDTH, 0) * EDSCALE);
	file_tree->set_h_size_flags(SIZE_EXPAND_FILL);
	file_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	file_tree->set_hide_root(true);
	file_tree->set_columns(1);
	file_tree->set_select_mode(Tree::SELECT_SINGLE);
	// A right-click picks the row it lands on, so the menu is plainly about that row.
	file_tree->set_allow_rmb_select(true);
	file_tree->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	add_child(file_tree);

	// Down until a file is picked, and whenever the two panes would not both fit. A hidden
	// child leaves the split with one, which then takes the whole width.
	preview_box = memnew(VBoxContainer);
	preview_box->set_custom_minimum_size(Size2(PREVIEW_MIN_WIDTH, 0) * EDSCALE);
	preview_box->set_h_size_flags(SIZE_EXPAND_FILL);
	preview_box->set_v_size_flags(SIZE_EXPAND_FILL);
	preview_box->hide();
	add_child(preview_box);

	bt_preview = memnew(TaskTree);
	bt_preview->set_h_size_flags(SIZE_EXPAND_FILL);
	bt_preview->set_v_size_flags(SIZE_EXPAND_FILL);
	// Shown, not edited: edits belong to the file's own tab, with its undo history.
	bt_preview->set_editable(false);
	bt_preview->hide();
	preview_box->add_child(bt_preview);

	hsm_preview = memnew(Tree);
	hsm_preview->set_h_size_flags(SIZE_EXPAND_FILL);
	hsm_preview->set_v_size_flags(SIZE_EXPAND_FILL);
	hsm_preview->set_hide_root(false);
	hsm_preview->set_columns(1);
	hsm_preview->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	hsm_preview->hide();
	preview_box->add_child(hsm_preview);

	item_menu = memnew(PopupMenu);
	add_child(item_menu);
}

#endif // TOOLS_ENABLED
