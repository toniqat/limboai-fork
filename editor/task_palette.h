/**
 * task_palette.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#ifndef TASK_PALETTE_H
#define TASK_PALETTE_H

#ifdef LIMBOAI_MODULE
#include "core/templates/hash_set.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/popup.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/tree.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/panel_container.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/rich_text_label.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/templates/hash_set.hpp>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

class TaskPalette : public PanelContainer {
	GDCLASS(TaskPalette, PanelContainer)

private:
	enum MenuAction {
		MENU_ADD_TASK,
		MENU_EDIT_SCRIPT,
		MENU_SHOW_IN_FILESYSTEM,
		MENU_OPEN_DOC,
		MENU_FAVORITE,
	};

	struct FilterSettings {
		enum TypeFilter {
			TYPE_ALL,
			TYPE_CORE,
			TYPE_USER,
		} type_filter;

		enum CategoryFilter {
			CATEGORY_ALL,
			CATEGORY_INCLUDE,
			CATEGORY_EXCLUDE,
		} category_filter;

		HashSet<String> excluded_categories;
	} filter_settings;

	struct ThemeCache {
		Ref<Texture2D> add_to_favorites_icon;
		Ref<Texture2D> edit_script_icon;
		Ref<Texture2D> open_doc_icon;
		Ref<Texture2D> remove_from_favorites_icon;

		Ref<StyleBox> category_choice_background;

		Ref<Texture2D> folder_icon;
		Color folder_icon_color;
	} theme_cache;

	LineEdit *filter_edit;
	// FileSystem-style tree: a "LimboAI" root holding the core categories and a "Project" root
	// holding the categories of user_task_dirs. Folder items carry FOLDER_PREFIX + their key
	// ("core/Actions", "user/Combat") as metadata, task items carry their task meta (class name
	// or script path), which never begins with the prefix.
	Tree *tree;
	// Folder keys the user has collapsed. The tree is rebuilt on every refresh, so this is what
	// survives it, and what is saved in the editor layout.
	HashSet<String> collapsed_folders;
	bool collapsed_folders_loaded = false;
	// While a filter is typed every folder with a match is expanded; programmatic expansion and
	// the user's own toggling during a filter do not touch collapsed_folders.
	bool filtering = false;
	bool updating_tree = false;

	// Sticky headers: the folders the first visible row sits in, pinned over the top of the
	// tree while it is scrolled. Tree (as of 4.7) has no such mode, and it draws its rows on a
	// canvas item of its own, so this is a child control laid over them rather than a draw
	// callback on the tree. Clicking a pinned row scrolls to that folder.
	Control *sticky_overlay;
	// The selected task's full description, beside the tree once the palette is wide enough
	// for both (DESCRIPTION_MIN_WIDTH). Never shown in the change-type dialog.
	PanelContainer *description_pane;
	RichTextLabel *description_text;
	Vector<TreeItem *> sticky_items;
	float sticky_row_height = 0.0;
	// Where Tree starts its rows: the panel's top-left margin. The pinned rows sit there too,
	// while their background runs from the very top, since Tree draws rows into that margin.
	Vector2 sticky_content_ofs;
	int sticky_hovered = -1;
	PopupMenu *menu;
	Button *tool_filters;
	Button *tool_refresh;

	// Filter popup
	PopupPanel *filter_popup;
	Button *type_all;
	Button *type_core;
	Button *type_user;
	Button *category_all;
	Button *category_include;
	Button *category_exclude;
	VBoxContainer *category_choice;
	Button *select_all;
	Button *deselect_all;
	ScrollContainer *category_scroll;
	VBoxContainer *category_list;

	String context_task;
	bool dialog_mode = false;

	void _menu_action_selected(int p_id);
	void _on_task_button_rmb(const String &p_task);
	void _on_item_mouse_selected(const Vector2 &p_pos, MouseButton p_button_index);
	void _on_item_collapsed(Object *p_item);
	TreeItem *_create_folder(TreeItem *p_parent, const String &p_text, const String &p_key, const Ref<Texture2D> &p_icon);
	// The `##` doc comment of a task script, or a core task's brief (`p_full` false, the
	// tooltip) or full description (`p_full` true, the description pane).
	String _get_task_description(const String &p_task_meta, bool p_full) const;
	void _update_description();
	void _update_description_visibility();
	void _load_collapsed_folders();
	Variant _get_drag_data_fw(const Point2 &p_point);
	bool _can_drop_data_fw(const Point2 &p_point, const Variant &p_data) const { return false; }
	void _drop_data_fw(const Point2 &p_point, const Variant &p_data) {}
	void _update_sticky_headers();
	void _draw_sticky_headers();
	void _on_sticky_gui_input(const Ref<InputEvent> &p_event);
	void _on_sticky_mouse_exited();
	int _sticky_row_at(float p_y) const;
	float _row_content_y(TreeItem *p_item) const;
	void _apply_filter(const String &p_text);
	void _update_filter_popup();
	void _show_filter_popup();
	void _type_filter_changed();
	void _category_filter_changed();
	void _set_all_filter_categories(bool p_selected);
	void _category_item_toggled(bool p_pressed, const String &p_category);
	void _filter_data_changed();
	void _draw_filter_popup_background();
	void _update_filter_button();

	_FORCE_INLINE_ void _set_category_excluded(const String &p_category, bool p_excluded) {
		if (p_excluded) {
			filter_settings.excluded_categories.insert(p_category);
		} else {
			filter_settings.excluded_categories.erase(p_category);
		}
	}

protected:
	virtual void _do_update_theme_item_cache();

	void _notification(int p_what);
	static void _bind_methods();

public:
	void refresh();
	void use_dialog_mode();
	void clear_filter() { filter_edit->set_text(""); }

	TaskPalette();
	~TaskPalette();
};

#endif // TASK_PALETTE_H

#endif // ! TOOLS_ENABLED
