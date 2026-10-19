/**
 * resource_browser.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#ifndef LIMBO_RESOURCE_BROWSER_H
#define LIMBO_RESOURCE_BROWSER_H

#include "../compat/editor.h"
#include "task_tree.h"

#ifdef LIMBOAI_MODULE
#include "core/templates/hash_set.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tree.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/editor_file_system_directory.hpp>
#include <godot_cpp/classes/h_split_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/templates/hash_set.hpp>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

// What the pinned "All" tab shows: every behavior tree and state machine file in the
// project, laid out by folder the way the FileSystem dock lays them out, with a read-only
// outline of the picked file beside it. A folder holding none of them is left out, so the
// tree is a map of where the AI lives and nothing else.
//
// Picking a file previews it; activating it (double-click or Enter) asks the editor to open
// it in a tab, which is the one thing this view cannot do for itself. So does "Open" on the
// right-click menu, and "Find Owners" asks the editor's owner picker.
class LimboResourceBrowser : public HSplitContainer {
	GDCLASS(LimboResourceBrowser, HSplitContainer);

private:
	Tree *file_tree;
	VBoxContainer *preview_box;
	// What a right-click on a row offers; `menu_path` is the row it was opened over.
	PopupMenu *item_menu;
	String menu_path;
	// One of the two stands under the title, by the picked file's type. The tree is the same
	// task tree a tab shows, locked; the machine is the state list a machine tab shows.
	TaskTree *bt_preview;
	Tree *hsm_preview;

	// The file being previewed, kept by path so a rebuild of the list can pick it again.
	String previewed_path;
	// Folders the user folded. Everything else starts open: the list only holds folders that
	// lead to an AI file, so an open folder is never noise.
	HashSet<String> collapsed_dirs;
	Ref<Texture2D> folder_icon;
	Color folder_color = Color(1, 1, 1);
	bool rebuilding = false;
	// Set when the file system moved while the view was hidden; the rebuild waits for it to
	// be shown rather than walking the project for nobody.
	bool list_dirty = true;

	static bool _is_ai_type(const String &p_type);
	static bool _has_ai_files(EditorFileSystemDirectory *p_dir);
	void _fill_dir(EditorFileSystemDirectory *p_dir, TreeItem *p_item);
	void _rebuild();
	void _show_preview(const String &p_path);
	void _update_preview_visibility();

	void _on_item_selected();
	void _on_item_activated();
	void _on_item_collapsed(TreeItem *p_item);
	void _on_filesystem_changed();
	void _on_item_mouse_selected(const Vector2 &p_pos, int p_button);
	void _on_item_menu_pressed(int p_id);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	LimboResourceBrowser();
};

#endif // LIMBO_RESOURCE_BROWSER_H

#endif // TOOLS_ENABLED
