/**
 * task_palette.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#include "task_palette.h"

#include "../compat/editor.h"
#include "../compat/editor_paths.h"
#include "../compat/editor_scale.h"
#include "../compat/project_settings.h"
#include "../compat/translation.h"
#include "../compat/variant.h"
#include "../util/limbo_string_names.h"
#include "../util/limbo_task_db.h"
#include "../util/limbo_utility.h"

#ifdef LIMBOAI_MODULE
#include "core/io/config_file.h"
#include "core/io/file_access.h"
#include "editor/doc/editor_help.h"
#include "editor/docks/filesystem_dock.h"
#include "editor/editor_node.h"
#include "scene/gui/check_box.h"
#include "scene/gui/label.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/scroll_bar.h"
#include "scene/gui/texture_rect.h"
#endif // LIMBO_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/button_group.hpp>
#include <godot_cpp/classes/check_box.hpp>
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/file_system_dock.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/style_box.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
using namespace godot;

#include "../gen/task_briefs.gen.h"
#endif // LIMBOAI_GDEXTENSION

namespace {

// Metadata prefix of a folder item. A task item's metadata is its task meta, a class name or a
// res:// path, which never begins with it.
const char *FOLDER_PREFIX = "#folder:";

bool _is_folder(TreeItem *p_item) {
	return String(p_item->get_metadata(0)).begins_with(FOLDER_PREFIX);
}

String _folder_key(TreeItem *p_item) {
	return String(p_item->get_metadata(0)).trim_prefix(FOLDER_PREFIX);
}

#ifdef LIMBOAI_GDEXTENSION
// Core task descriptions, generated from doc_classes/*.xml at build time (gen_task_briefs.py):
// an extension cannot read EditorHelp back. Maps a class name to its LIMBO_TASK_BRIEFS row.
const HashMap<String, int> &_core_task_rows() {
	static HashMap<String, int> rows;
	if (rows.is_empty()) {
		for (int i = 0; i < int(sizeof(LIMBO_TASK_BRIEFS) / sizeof(LIMBO_TASK_BRIEFS[0])); i++) {
			rows.insert(String::utf8(LIMBO_TASK_BRIEFS[i][0]), i);
		}
	}
	return rows;
}
#endif // LIMBOAI_GDEXTENSION

// The class documentation of a task script: the `##` lines after its header lines (@tool,
// extends, class_name, plain comments), kept line for line.
String _script_doc_comment(const String &p_path) {
	PackedStringArray lines = FileAccess::get_file_as_string(p_path).split("\n");
	PackedStringArray doc;
	for (int i = 0; i < lines.size(); i++) {
		String line = lines[i].strip_edges();
		if (line.begins_with("##")) {
			line = line.substr(2);
			if (line.begins_with(" ")) {
				line = line.substr(1);
			}
			if (!line.begins_with("@tutorial")) {
				doc.push_back(line);
			}
		} else if (!doc.is_empty()) {
			break;
		} else if (!(line.is_empty() || line.begins_with("@") || line.begins_with("extends") || line.begins_with("class_name") || line.begins_with("#"))) {
			break;
		}
	}
	while (!doc.is_empty() && doc[doc.size() - 1].is_empty()) {
		doc.remove_at(doc.size() - 1);
	}
	return String("\n").join(doc);
}

} // namespace

//**** TaskPalette

void TaskPalette::_menu_action_selected(int p_id) {
	ERR_FAIL_COND(context_task.is_empty());
	switch (p_id) {
		case MENU_ADD_TASK: {
			emit_signal(LW_NAME(task_selected), context_task);
		} break;
		case MENU_OPEN_DOC: {
			LimboUtility::get_singleton()->open_doc_class(context_task);
		} break;
		case MENU_EDIT_SCRIPT: {
			ERR_FAIL_COND(!context_task.begins_with("res://"));
			EDIT_SCRIPT(context_task);
		} break;
		case MENU_SHOW_IN_FILESYSTEM: {
			ERR_FAIL_COND(!context_task.begins_with("res://"));
			EditorInterface::get_singleton()->get_file_system_dock()->navigate_to_path(context_task);
		} break;
		case MENU_FAVORITE: {
			PackedStringArray favorite_tasks = GLOBAL_GET("limbo_ai/behavior_tree/favorite_tasks");
			if (favorite_tasks.has(context_task)) {
				int idx = favorite_tasks.find(context_task);
				if (idx >= 0) {
					favorite_tasks.remove_at(idx);
				}
			} else {
				favorite_tasks.append(context_task);
			}
			ProjectSettings::get_singleton()->set_setting("limbo_ai/behavior_tree/favorite_tasks", favorite_tasks);
			ProjectSettings::get_singleton()->save();
			emit_signal(LW_NAME(favorite_tasks_changed));
		} break;
	}
}

void TaskPalette::_on_item_mouse_selected(const Vector2 &p_pos, MouseButton p_button_index) {
	TreeItem *item = tree->get_selected();
	if (item == nullptr) {
		return;
	}
	if (_is_folder(item)) {
		return; // Folders open and close on their arrow only.
	}
	// In the palette a task is added from its context menu or by dragging it onto the task
	// tree, so a stray click never edits the tree. The change-type dialog is a picker: there a
	// click is the choice.
	String task_meta = item->get_metadata(0);
	if (p_button_index == LW_MBTN(LEFT) && dialog_mode) {
		emit_signal(LW_NAME(task_selected), task_meta);
	} else if (p_button_index == LW_MBTN(RIGHT)) {
		_on_task_button_rmb(task_meta);
	}
}

Variant TaskPalette::_get_drag_data_fw(const Point2 &p_point) {
	if (dialog_mode) {
		return Variant();
	}
	TreeItem *item = tree->get_item_at_position(p_point);
	if (item == nullptr || _is_folder(item)) {
		return Variant();
	}

	HBoxContainer *preview = memnew(HBoxContainer);
	TextureRect *icon = memnew(TextureRect);
	icon->set_custom_minimum_size(Size2(16, 16) * EDSCALE);
	icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
	icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
	icon->set_texture(item->get_icon(0));
	preview->add_child(icon);
	Label *label = memnew(Label);
	label->set_text(item->get_text(0));
	preview->add_child(label);
	set_drag_preview(preview);

	// TaskTree takes this kind on drop and creates the task where it lands.
	Dictionary drag_data;
	drag_data["type"] = "palette_task";
	drag_data["task_meta"] = item->get_metadata(0);
	return drag_data;
}

void TaskPalette::_update_sticky_headers() {
	sticky_items.clear();
	TreeItem *root = tree->get_root();
	Ref<StyleBox> panel = tree->get_theme_stylebox(LW_NAME(panel));
	sticky_content_ofs = panel.is_valid() ? Vector2(panel->get_margin(SIDE_LEFT), panel->get_margin(SIDE_TOP)) : Vector2();

	TreeItem *first = root != nullptr ? root->get_first_child() : nullptr;
	sticky_row_height = first != nullptr ? tree->get_item_area_rect(first).size.y : 0.0;
	float scroll = tree->get_scroll().y;
	// Where a row stands now, in tree coordinates (see _row_content_y()).
	auto row_top = [&](TreeItem *p_item) { return sticky_content_ofs.y + _row_content_y(p_item) - scroll; };

	// One slot per level (the roots, then their categories). A folder takes its level's slot
	// once its own row has gone up under that slot, and keeps it while some of its contents
	// are still showing below the slot. So a pinned folder never stands over its own row, and
	// a folder whose row is still in view is left to that row.
	TreeItem *parent = root;
	for (int level = 0; parent != nullptr && sticky_row_height > 0.0 && level < 2; level++) {
		float slot_top = sticky_content_ofs.y + level * sticky_row_height;
		TreeItem *pinned = nullptr;
		for (TreeItem *folder = parent->get_first_child(); folder != nullptr; folder = folder->get_next()) {
			if (!folder->is_visible() || !_is_folder(folder)) {
				continue;
			}
			if (row_top(folder) >= slot_top) {
				break; // This and every later sibling is still in view.
			}
			if (folder->is_collapsed()) {
				continue;
			}
			// The last row the folder shows: walk down its last open, visible children.
			TreeItem *last = folder;
			while (!last->is_collapsed()) {
				TreeItem *child = nullptr;
				for (int i = last->get_child_count() - 1; i >= 0; i--) {
					if (last->get_child(i)->is_visible()) {
						child = last->get_child(i);
						break;
					}
				}
				if (child == nullptr) {
					break;
				}
				last = child;
			}
			if (row_top(last) + sticky_row_height > slot_top + sticky_row_height) {
				pinned = folder;
			}
		}
		if (pinned == nullptr) {
			break;
		}
		sticky_items.push_back(pinned);
		parent = pinned;
	}

	if (sticky_items.is_empty()) {
		sticky_hovered = -1;
		sticky_overlay->hide();
		return;
	}
	if (sticky_hovered >= sticky_items.size()) {
		sticky_hovered = -1;
	}

	// Edge to edge, scrollbar included, and from the tree's very top: Tree draws rows into its
	// panel margin as well (it only clips there for column titles), so a stack starting at the
	// margin leaves a strip of scrolled rows showing above it.
	sticky_overlay->set_position(Vector2());
	sticky_overlay->set_size(Size2(tree->get_size().x, sticky_content_ofs.y + sticky_row_height * sticky_items.size()));
	sticky_overlay->show();
	sticky_overlay->queue_redraw();
}

// A row's top in content space: 0 for the first row, growing down, whatever the scroll.
// Measured against the first row rather than read off get_item_area_rect() alone, because
// Tree folds the scroll into that rect only when it next draws: called from the scrollbar's
// value_changed, the rect still stands where the previous scroll put it, which a wheel step
// of several rows (or a jump back to the top) makes plainly wrong.
float TaskPalette::_row_content_y(TreeItem *p_item) const {
	TreeItem *first = tree->get_root() != nullptr ? tree->get_root()->get_first_child() : nullptr;
	if (first == nullptr) {
		return 0.0;
	}
	return tree->get_item_area_rect(p_item).position.y - tree->get_item_area_rect(first).position.y;
}

int TaskPalette::_sticky_row_at(float p_y) const {
	if (sticky_row_height <= 0.0) {
		return -1;
	}
	int idx = int(MAX(p_y - sticky_content_ofs.y, 0.0f) / sticky_row_height);
	return idx < sticky_items.size() ? idx : -1;
}

void TaskPalette::_draw_sticky_headers() {
	if (sticky_items.is_empty()) {
		return;
	}
	TreeItem *root = tree->get_root();
	Ref<Font> font = tree->get_theme_font(LW_NAME(font));
	int font_size = tree->get_theme_font_size(LW_NAME(font_size));
	Color font_color = tree->get_theme_color(LW_NAME(font_color));
	int item_margin = tree->get_theme_constant("item_margin");
	int inner_margin = tree->get_theme_constant("inner_item_margin_left");
	int h_separation = tree->get_theme_constant(LW_NAME(h_separation));
	Ref<Texture2D> arrow = tree->get_theme_icon("arrow");

	// The palette's own background, the gray behind the filter row above the tree: lighter than
	// the dark tree, so the pinned rows read as a header and not as rows that stopped
	// scrolling. Opaque, so the scrolled rows never show through.
	Color bg_color = get_theme_color("base_color", LW_NAME(Editor));
	Ref<StyleBoxFlat> own_panel = get_theme_stylebox(LW_NAME(panel));
	if (own_panel.is_valid() && own_panel->is_draw_center_enabled() && own_panel->get_bg_color().a > 0.0) {
		bg_color = bg_color.blend(own_panel->get_bg_color());
	}
	bg_color.a = 1.0;

	float width = sticky_overlay->get_size().x;
	float rh = sticky_row_height;
	float bottom = sticky_content_ofs.y + rh * sticky_items.size();
	sticky_overlay->draw_rect(Rect2(0, 0, width, bottom), bg_color);
	for (int i = 0; i < sticky_items.size(); i++) {
		TreeItem *item = sticky_items[i];
		int depth = 0;
		for (TreeItem *p = item->get_parent(); p != nullptr && p != root; p = p->get_parent()) {
			depth++;
		}
		// Laid out the way Tree lays out a row: the arrow at the right of the item margin
		// for its depth, then the cell with its icon and text.
		float y = sticky_content_ofs.y + i * rh;
		float x = sticky_content_ofs.x + depth * item_margin;
		if (i == sticky_hovered) {
			// The whole line, edge to edge, as it is a button for the whole line.
			Ref<StyleBox> hovered = tree->get_theme_stylebox("hovered");
			if (hovered.is_valid()) {
				hovered->draw(sticky_overlay->get_canvas_item(), Rect2(0, y, width, rh));
			}
		}
		if (arrow.is_valid()) {
			sticky_overlay->draw_texture(arrow, Vector2(x + item_margin - arrow->get_width(), y + Math::floor((rh - arrow->get_height()) * 0.5)));
		}
		float text_x = x + item_margin + inner_margin;
		Ref<Texture2D> icon = item->get_icon(0);
		if (icon.is_valid()) {
			Size2 icon_size = icon->get_size();
			int max_w = item->get_icon_max_width(0);
			if (max_w > 0 && icon_size.x > max_w) {
				icon_size = Size2(max_w, icon_size.y * max_w / icon_size.x);
			}
			sticky_overlay->draw_texture_rect(icon, Rect2(Vector2(text_x, y + Math::floor((rh - icon_size.y) * 0.5)), icon_size), false, item->get_icon_modulate(0));
			text_x += icon_size.x + h_separation;
		}
		if (font.is_valid()) {
			float ascent = font->get_ascent(font_size);
			float baseline = y + Math::floor((rh - (ascent + font->get_descent(font_size))) * 0.5 + ascent);
			sticky_overlay->draw_string(font, Vector2(text_x, baseline), item->get_text(0), HORIZONTAL_ALIGNMENT_LEFT, MAX(width - text_x, 0.0f), font_size, font_color);
		}
	}

	// The shade under the pinned rows that FileSystem draws at the top of a scrolled tree.
	Ref<Texture2D> hint = tree->get_theme_icon("scroll_hint");
	if (hint.is_valid()) {
		sticky_overlay->draw_texture_rect(hint, Rect2(0, bottom, width, hint->get_height()), false, tree->get_theme_color("scroll_hint_color"));
	} else {
		float h = 8 * EDSCALE;
		PackedVector2Array points;
		points.push_back(Vector2(0, bottom));
		points.push_back(Vector2(width, bottom));
		points.push_back(Vector2(width, bottom + h));
		points.push_back(Vector2(0, bottom + h));
		PackedColorArray colors;
		Color shade(0, 0, 0, 0.35);
		colors.push_back(shade);
		colors.push_back(shade);
		colors.push_back(Color(0, 0, 0, 0));
		colors.push_back(Color(0, 0, 0, 0));
		sticky_overlay->draw_polygon(points, colors);
	}
}

void TaskPalette::_on_sticky_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		int idx = _sticky_row_at(mm->get_position().y);
		if (idx != sticky_hovered) {
			sticky_hovered = idx;
			sticky_overlay->queue_redraw();
		}
		return;
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed() || mb->get_button_index() != LW_MBTN(LEFT)) {
		return;
	}
	sticky_overlay->accept_event();
	int idx = _sticky_row_at(mb->get_position().y);
	if (idx < 0) {
		return;
	}
	// Scroll the folder's own row to just under the slots its ancestors will hold once it is
	// up there, so it is shown as a row again rather than pinned.
	VScrollBar *vsb = nullptr;
	for (int i = 0; i < tree->get_child_count(true) && vsb == nullptr; i++) {
		vsb = Object::cast_to<VScrollBar>(tree->get_child(i, true));
	}
	if (vsb == nullptr) {
		return;
	}
	vsb->set_value(MAX(_row_content_y(sticky_items[idx]) - idx * sticky_row_height, 0.0f));
}

void TaskPalette::_on_sticky_mouse_exited() {
	if (sticky_hovered != -1) {
		sticky_hovered = -1;
		sticky_overlay->queue_redraw();
	}
}

void TaskPalette::_on_item_collapsed(Object *p_item) {
	callable_mp(this, &TaskPalette::_update_sticky_headers).call_deferred();
	if (updating_tree || filtering || dialog_mode) {
		return;
	}
	TreeItem *item = Object::cast_to<TreeItem>(p_item);
	if (item == nullptr || !_is_folder(item)) {
		return;
	}
	if (item->is_collapsed()) {
		collapsed_folders.insert(_folder_key(item));
	} else {
		collapsed_folders.erase(_folder_key(item));
	}
}

TreeItem *TaskPalette::_create_folder(TreeItem *p_parent, const String &p_text, const String &p_key, const Ref<Texture2D> &p_icon) {
	TreeItem *item = tree->create_item(p_parent);
	item->set_text(0, p_text);
	if (p_icon.is_valid()) {
		item->set_icon(0, p_icon);
	} else {
		item->set_icon(0, theme_cache.folder_icon);
		item->set_icon_modulate(0, theme_cache.folder_icon_color);
	}
	item->set_icon_max_width(0, 16 * EDSCALE);
	item->set_metadata(0, String(FOLDER_PREFIX) + p_key);
	item->set_collapsed(!dialog_mode && collapsed_folders.has(p_key));
	return item;
}

String TaskPalette::_get_task_description(const String &p_task_meta, bool p_full) const {
	String desc;
	if (p_task_meta.begins_with("res://")) {
		desc = _script_doc_comment(p_task_meta);
	} else {
#ifdef LIMBOAI_GDEXTENSION
		const HashMap<String, int> &rows = _core_task_rows();
		if (rows.has(p_task_meta)) {
			desc = String::utf8(LIMBO_TASK_BRIEFS[rows.get(p_task_meta)][p_full ? 2 : 1]);
		}
#endif // LIMBOAI_GDEXTENSION
#ifdef LIMBOAI_MODULE
		HashMap<String, DocData::ClassDoc>::ConstIterator E = EditorHelp::get_doc_data()->class_list.find(p_task_meta);
		if (E) {
			desc = DTR(p_full && !E->value.description.is_empty() ? E->value.description : E->value.brief_description);
		}
#endif // LIMBOAI_MODULE
	}
	if (desc.is_empty()) {
		desc = TTR("No description.");
	}
	return desc;
}

void TaskPalette::_update_description() {
	description_text->clear();
	TreeItem *item = tree->get_selected();
	Color dim_color = get_theme_color(LW_NAME(disabled_font_color), LW_NAME(Editor));
	if (item == nullptr || _is_folder(item)) {
		description_text->push_color(dim_color);
		description_text->add_text(TTR("Select a task to see its description."));
		description_text->pop();
		return;
	}
	String task_meta = item->get_metadata(0);
	int icon_size = 16 * EDSCALE;
	description_text->add_image(item->get_icon(0), icon_size, icon_size);
	description_text->add_text(" ");
	description_text->push_font(get_theme_font(LW_NAME(bold), LW_NAME(EditorFonts)));
	description_text->add_text(item->get_text(0));
	description_text->pop();
	description_text->add_text("\n\n" + _get_task_description(task_meta, true));
	if (task_meta.begins_with("res://")) {
		description_text->add_text("\n\n");
		description_text->push_color(dim_color);
		description_text->add_text(task_meta);
		description_text->pop();
	}
}

void TaskPalette::_update_description_visibility() {
	// Below this the tree would be squeezed to make room; the tooltip still carries the brief.
	const float DESCRIPTION_MIN_WIDTH = 460.0;
	description_pane->set_visible(!dialog_mode && get_size().x >= DESCRIPTION_MIN_WIDTH * EDSCALE);
}

void TaskPalette::_load_collapsed_folders() {
	if (collapsed_folders_loaded || dialog_mode) {
		return;
	}
	collapsed_folders_loaded = true;
	Ref<ConfigFile> cf;
	cf.instantiate();
	if (cf->load(LAYOUT_CONFIG_FILE()) != OK) {
		return;
	}
	Variant value = cf->get_value("LimboAI", "task_palette_collapsed_folders", Array());
	if (VARIANT_IS_ARRAY(value)) {
		Array arr = value;
		for (int i = 0; i < arr.size(); i++) {
			if (arr[i].get_type() == Variant::STRING) {
				collapsed_folders.insert(arr[i]);
			}
		}
	}
}

void TaskPalette::_on_task_button_rmb(const String &p_task) {
	if (dialog_mode) {
		return;
	}

	ERR_FAIL_COND(p_task.is_empty());

	context_task = p_task;
	menu->clear();

	menu->add_icon_item(get_theme_icon("Add", LW_NAME(EditorIcons)), TTR("Add Task"), MENU_ADD_TASK);
	menu->add_separator();
	// Only a script task has a file; the built-in ones keep the items, greyed out.
	bool is_script = context_task.begins_with("res://");
	menu->add_icon_item(theme_cache.edit_script_icon, TTR("Edit Script"), MENU_EDIT_SCRIPT);
	menu->set_item_disabled(menu->get_item_index(MENU_EDIT_SCRIPT), !is_script);
	menu->add_icon_item(get_theme_icon("ShowInFileSystem", LW_NAME(EditorIcons)), TTR("Show in FileSystem"), MENU_SHOW_IN_FILESYSTEM);
	menu->set_item_disabled(menu->get_item_index(MENU_SHOW_IN_FILESYSTEM), !is_script);
	menu->add_icon_item(theme_cache.open_doc_icon, TTR("Open Documentation"), MENU_OPEN_DOC);

	menu->add_separator();
	Array favorite_tasks = GLOBAL_GET("limbo_ai/behavior_tree/favorite_tasks");
	if (favorite_tasks.has(context_task)) {
		menu->add_icon_item(theme_cache.remove_from_favorites_icon, TTR("Remove from Favorites"), MENU_FAVORITE);
	} else {
		menu->add_icon_item(theme_cache.add_to_favorites_icon, TTR("Add to Favorites"), MENU_FAVORITE);
	}

	menu->reset_size();
	menu->set_position(get_screen_position() + get_local_mouse_position());
	menu->popup();
}

void TaskPalette::_apply_filter(const String &p_text) {
	TreeItem *root = tree->get_root();
	if (root == nullptr) {
		return;
	}
	// Folders with a match are opened while filtering; clearing the filter puts back what the
	// user had collapsed.
	filtering = !p_text.is_empty();
	updating_tree = true;
	for (TreeItem *group = root->get_first_child(); group != nullptr; group = group->get_next()) {
		int group_matches = 0;
		for (TreeItem *folder = group->get_first_child(); folder != nullptr; folder = folder->get_next()) {
			int matches = 0;
			for (TreeItem *task = folder->get_first_child(); task != nullptr; task = task->get_next()) {
				bool match = !filtering || task->get_text(0).findn(p_text) != -1;
				task->set_visible(match);
				matches += match;
			}
			folder->set_visible(matches > 0);
			folder->set_collapsed(!filtering && !dialog_mode && collapsed_folders.has(_folder_key(folder)));
			group_matches += matches;
		}
		group->set_visible(group_matches > 0);
		group->set_collapsed(!filtering && !dialog_mode && collapsed_folders.has(_folder_key(group)));
	}
	updating_tree = false;
	callable_mp(this, &TaskPalette::_update_sticky_headers).call_deferred();
}

void TaskPalette::_update_filter_popup() {
	switch (filter_settings.type_filter) {
		case FilterSettings::TypeFilter::TYPE_ALL: {
			type_all->set_pressed(true);
		} break;
		case FilterSettings::TypeFilter::TYPE_CORE: {
			type_core->set_pressed(true);
		} break;
		case FilterSettings::TypeFilter::TYPE_USER: {
			type_user->set_pressed(true);
		} break;
	}

	switch (filter_settings.category_filter) {
		case FilterSettings::CategoryFilter::CATEGORY_ALL: {
			category_all->set_pressed(true);
		} break;
		case FilterSettings::CategoryFilter::CATEGORY_INCLUDE: {
			category_include->set_pressed(true);
		} break;
		case FilterSettings::CategoryFilter::CATEGORY_EXCLUDE: {
			category_exclude->set_pressed(true);
		} break;
	}

	while (category_list->get_child_count() > 0) {
		Node *item = category_list->get_child(0);
		category_list->remove_child(item);
		item->queue_free();
	}
	for (String &cat : LimboTaskDB::get_categories()) {
		CheckBox *category_item = memnew(CheckBox);
		category_item->set_text(cat);
		category_item->set_focus_mode(FocusMode::FOCUS_NONE);
		category_item->set_pressed_no_signal(LOGICAL_XOR(
				filter_settings.excluded_categories.has(cat),
				filter_settings.category_filter == FilterSettings::CategoryFilter::CATEGORY_INCLUDE));
		category_item->connect(LW_NAME(toggled), callable_mp(this, &TaskPalette::_category_item_toggled).bind(cat));
		category_list->add_child(category_item);
	}

	category_list->reset_size();
	Size2 size = category_list->get_size() + Size2(8, 8);
	size.width = MIN(size.width, 400 * EDSCALE);
	size.height = MIN(size.height, 600 * EDSCALE);
	category_scroll->set_custom_minimum_size(size);

	category_choice->set_visible(filter_settings.category_filter != FilterSettings::CATEGORY_ALL);
}

void TaskPalette::_show_filter_popup() {
	_update_filter_popup();

	tool_filters->set_pressed_no_signal(true);

	Transform2D xform = tool_filters->get_screen_transform();
	Rect2i rect = Rect2(xform.get_origin(), xform.get_scale() * tool_filters->get_size());

	rect.position.y += rect.size.height;
	rect.size.height = 0;
	filter_popup->reset_size();
	filter_popup->popup(rect);
}

void TaskPalette::_category_filter_changed() {
	if (category_all->is_pressed()) {
		filter_settings.category_filter = FilterSettings::CategoryFilter::CATEGORY_ALL;
	} else if (category_include->is_pressed()) {
		filter_settings.category_filter = FilterSettings::CategoryFilter::CATEGORY_INCLUDE;
	} else if (category_exclude->is_pressed()) {
		filter_settings.category_filter = FilterSettings::CategoryFilter::CATEGORY_EXCLUDE;
	}

	for (int i = 0; i < category_list->get_child_count(); i++) {
		CheckBox *item = Object::cast_to<CheckBox>(category_list->get_child(i));
		item->set_pressed_no_signal(LOGICAL_XOR(
				filter_settings.excluded_categories.has(item->get_text()),
				filter_settings.category_filter == FilterSettings::CATEGORY_INCLUDE));
	}

	category_choice->set_visible(filter_settings.category_filter != FilterSettings::CATEGORY_ALL);
	filter_popup->reset_size();
	_filter_data_changed();
}

void TaskPalette::_set_all_filter_categories(bool p_selected) {
	for (int i = 0; i < category_list->get_child_count(); i++) {
		CheckBox *item = Object::cast_to<CheckBox>(category_list->get_child(i));
		item->set_pressed_no_signal(p_selected);
		bool excluded = LOGICAL_XOR(p_selected, filter_settings.category_filter == FilterSettings::CATEGORY_INCLUDE);
		_set_category_excluded(item->get_text(), excluded);
	}
	_filter_data_changed();
}

void TaskPalette::_type_filter_changed() {
	if (type_all->is_pressed()) {
		filter_settings.type_filter = FilterSettings::TypeFilter::TYPE_ALL;
	} else if (type_core->is_pressed()) {
		filter_settings.type_filter = FilterSettings::TypeFilter::TYPE_CORE;
	} else if (type_user->is_pressed()) {
		filter_settings.type_filter = FilterSettings::TypeFilter::TYPE_USER;
	}
	_filter_data_changed();
}

void TaskPalette::_category_item_toggled(bool p_pressed, const String &p_category) {
	bool excluded = LOGICAL_XOR(p_pressed, filter_settings.category_filter == FilterSettings::CATEGORY_INCLUDE);
	_set_category_excluded(p_category, excluded);
	_filter_data_changed();
}

void TaskPalette::_filter_data_changed() {
	callable_mp(this, &TaskPalette::refresh).call_deferred();
	_update_filter_button();
}

void TaskPalette::_draw_filter_popup_background() {
	theme_cache.category_choice_background->draw(category_choice->get_canvas_item(), Rect2(Point2(), category_choice->get_size()));
}

void TaskPalette::_update_filter_button() {
	tool_filters->set_pressed_no_signal(filter_popup->is_visible() ||
			filter_settings.type_filter != FilterSettings::TYPE_ALL ||
			(filter_settings.category_filter != FilterSettings::CATEGORY_ALL && !filter_settings.excluded_categories.is_empty()));
}

void TaskPalette::refresh() {
	_load_collapsed_folders();

	LimboTaskDB::scan_user_tasks();
	List<String> categories = LimboTaskDB::get_categories();
	categories.sort();

	updating_tree = true;
	sticky_items.clear();
	tree->clear();
	TreeItem *root = tree->create_item();

	// Two passes over the same categories: built-in tasks under "LimboAI", then the scripts of
	// user_task_dirs under "Project". A root or folder is created only once a task lands in it.
	for (int pass = 0; pass < 2; pass++) {
		bool user_pass = pass == 1;
		if ((user_pass && filter_settings.type_filter == FilterSettings::TYPE_CORE) ||
				(!user_pass && filter_settings.type_filter == FilterSettings::TYPE_USER)) {
			continue;
		}
		TreeItem *group = nullptr;
		for (const String &cat : categories) {
			if (filter_settings.category_filter != FilterSettings::CATEGORY_ALL && filter_settings.excluded_categories.has(cat)) {
				continue;
			}
			TreeItem *folder = nullptr;
			for (const String &task_meta : LimboTaskDB::get_tasks_in_category(cat)) {
				if (task_meta.begins_with("res:") != user_pass) {
					continue;
				}
				if (group == nullptr) {
					group = user_pass
							? _create_folder(root, TTR("Project"), "user", Ref<Texture2D>())
							: _create_folder(root, "LimboAI", "core", LimboUtility::get_singleton()->get_task_icon("LimboAI"));
				}
				if (folder == nullptr) {
					folder = _create_folder(group, cat, (user_pass ? "user/" : "core/") + cat, Ref<Texture2D>());
				}

				String tname = user_pass
						? task_meta.get_file().get_basename().trim_prefix("BT").to_pascal_case()
						: task_meta.trim_prefix("BT");
				TreeItem *item = tree->create_item(folder);
				item->set_text(0, tname);
				item->set_icon(0, LimboUtility::get_singleton()->get_task_icon(task_meta));
				item->set_icon_max_width(0, 16 * EDSCALE); // Force user icons to be of the proper size.
				item->set_metadata(0, task_meta);
				item->set_tooltip_text(0, user_pass ? _get_task_description(task_meta, false) + "\n\n" + task_meta : _get_task_description(task_meta, false));
			}
		}
	}
	updating_tree = false;
	filtering = false;

	if (!filter_edit->get_text().is_empty()) {
		_apply_filter(filter_edit->get_text());
	}
	callable_mp(this, &TaskPalette::_update_sticky_headers).call_deferred();
	_update_description();
}

void TaskPalette::use_dialog_mode() {
	tool_filters->hide();
	tool_refresh->hide();
	dialog_mode = true;
}

void TaskPalette::_do_update_theme_item_cache() {
	theme_cache.add_to_favorites_icon = get_theme_icon(LW_NAME(Favorites), LW_NAME(EditorIcons));
	theme_cache.edit_script_icon = get_theme_icon(LW_NAME(Script), LW_NAME(EditorIcons));
	theme_cache.open_doc_icon = get_theme_icon(LW_NAME(Help), LW_NAME(EditorIcons));
	theme_cache.remove_from_favorites_icon = get_theme_icon(LW_NAME(NonFavorite), LW_NAME(EditorIcons));

	theme_cache.category_choice_background = get_theme_stylebox(LW_NAME(normal), LW_NAME(LineEdit));

	theme_cache.folder_icon = get_theme_icon("Folder", LW_NAME(EditorIcons));
	theme_cache.folder_icon_color = get_theme_color("folder_icon_color", "FileDialog");

	// The same darker body TaskTree gives the behavior tree, so both trees read alike. Built
	// from the theme's own Tree panel so margins and borders stay the theme's.
	Ref<StyleBox> tree_panel = get_theme_stylebox(LW_NAME(panel), LW_NAME(Tree));
	Ref<StyleBoxFlat> dark_panel;
	if (Object::cast_to<StyleBoxFlat>(tree_panel.ptr())) {
		dark_panel = tree_panel->duplicate();
	} else {
		dark_panel.instantiate();
		if (tree_panel.is_valid()) {
			for (int side = 0; side < 4; side++) {
				dark_panel->set_content_margin(Side(side), tree_panel->get_content_margin(Side(side)));
			}
		}
	}
	dark_panel->set_draw_center(true);
	dark_panel->set_bg_color(get_theme_color(LW_NAME(dark_color_2), LW_NAME(Editor)));
	ADD_STYLEBOX_OVERRIDE(tree, LW_NAME(panel), dark_panel);
	ADD_STYLEBOX_OVERRIDE(description_pane, LW_NAME(panel), dark_panel);
}

void TaskPalette::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			// **** Signals
			tool_filters->connect(LW_NAME(pressed), callable_mp(this, &TaskPalette::_show_filter_popup));
			filter_edit->connect(LW_NAME(text_changed), callable_mp(this, &TaskPalette::_apply_filter));
			tool_refresh->connect(LW_NAME(pressed), callable_mp(this, &TaskPalette::refresh));
			menu->connect(LW_NAME(id_pressed), callable_mp(this, &TaskPalette::_menu_action_selected));
			tree->connect("item_mouse_selected", callable_mp(this, &TaskPalette::_on_item_mouse_selected));
			tree->connect(LW_NAME(item_collapsed), callable_mp(this, &TaskPalette::_on_item_collapsed));
			tree->connect("resized", callable_mp(this, &TaskPalette::_update_sticky_headers));
			tree->connect("item_selected", callable_mp(this, &TaskPalette::_update_description));
			connect("resized", callable_mp(this, &TaskPalette::_update_description_visibility));
			for (int i = 0; i < tree->get_child_count(true); i++) {
				VScrollBar *vsb = Object::cast_to<VScrollBar>(tree->get_child(i, true));
				if (vsb != nullptr) {
					vsb->connect("value_changed", callable_mp(this, &TaskPalette::_update_sticky_headers).unbind(1));
				}
			}
			sticky_overlay->connect(LW_NAME(draw), callable_mp(this, &TaskPalette::_draw_sticky_headers));
			sticky_overlay->connect(LW_NAME(gui_input), callable_mp(this, &TaskPalette::_on_sticky_gui_input));
			sticky_overlay->connect("mouse_exited", callable_mp(this, &TaskPalette::_on_sticky_mouse_exited));
			type_all->connect(LW_NAME(pressed), callable_mp(this, &TaskPalette::_type_filter_changed));
			type_core->connect(LW_NAME(pressed), callable_mp(this, &TaskPalette::_type_filter_changed));
			type_user->connect(LW_NAME(pressed), callable_mp(this, &TaskPalette::_type_filter_changed));
			category_all->connect(LW_NAME(pressed), callable_mp(this, &TaskPalette::_category_filter_changed));
			category_include->connect(LW_NAME(pressed), callable_mp(this, &TaskPalette::_category_filter_changed));
			category_exclude->connect(LW_NAME(pressed), callable_mp(this, &TaskPalette::_category_filter_changed));
			category_choice->connect(LW_NAME(draw), callable_mp(this, &TaskPalette::_draw_filter_popup_background));
			select_all->connect(LW_NAME(pressed), callable_mp(this, &TaskPalette::_set_all_filter_categories).bind(true));
			deselect_all->connect(LW_NAME(pressed), callable_mp(this, &TaskPalette::_set_all_filter_categories).bind(false));
			filter_popup->connect(LW_NAME(popup_hide), callable_mp(this, &TaskPalette::_update_filter_button));
		} break;
		case NOTIFICATION_ENTER_TREE: {
			Ref<ConfigFile> cf;
			cf.instantiate();
			String conf_path = LAYOUT_CONFIG_FILE();
			if (cf->load(conf_path) == OK) {
				Variant value = cf->get_value("LimboAI", "task_palette_type_filter", FilterSettings::TypeFilter(0));
				if (VARIANT_IS_NUM(value)) {
					filter_settings.type_filter = (FilterSettings::TypeFilter)(int)value;
				}

				value = cf->get_value("LimboAI", "task_palette_category_filter", FilterSettings::CategoryFilter(0));
				if (VARIANT_IS_NUM(value)) {
					filter_settings.category_filter = (FilterSettings::CategoryFilter)(int)value;
				}

				value = cf->get_value("LimboAI", "task_palette_excluded_categories", Array());
				if (VARIANT_IS_ARRAY(value)) {
					Array arr = value;
					for (int i = 0; i < arr.size(); i++) {
						if (arr[i].get_type() == Variant::STRING) {
							filter_settings.excluded_categories.insert(arr[i]);
						}
					}
				}
			}
			_update_filter_button();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			Ref<ConfigFile> cf;
			cf.instantiate();
			String conf_path = LAYOUT_CONFIG_FILE();
			cf->load(conf_path);

			// The change-type dialog keeps every folder open and must not overwrite the palette's.
			if (!dialog_mode) {
				Array collapsed;
				for (const String &key : collapsed_folders) {
					collapsed.push_back(key);
				}
				cf->set_value("LimboAI", "task_palette_collapsed_folders", collapsed);
			}

			cf->set_value("LimboAI", "task_palette_type_filter", filter_settings.type_filter);
			cf->set_value("LimboAI", "task_palette_category_filter", filter_settings.category_filter);

			Array excluded_categories;
			for (const String &cat : filter_settings.excluded_categories) {
				excluded_categories.append(cat);
			}
			cf->set_value("LimboAI", "task_palette_excluded_categories", excluded_categories);

			cf->save(conf_path);
		} break;
		case NOTIFICATION_THEME_CHANGED: {
			_do_update_theme_item_cache();

			tool_filters->set_button_icon(get_theme_icon(LW_NAME(AnimationFilter), LW_NAME(EditorIcons)));
			tool_refresh->set_button_icon(get_theme_icon(LW_NAME(Reload), LW_NAME(EditorIcons)));

			filter_edit->set_right_icon(get_theme_icon(LW_NAME(Search), LW_NAME(EditorIcons)));

			select_all->set_button_icon(LimboUtility::get_singleton()->get_task_icon("LimboSelectAll"));
			deselect_all->set_button_icon(LimboUtility::get_singleton()->get_task_icon("LimboDeselectAll"));

			category_choice->queue_redraw();

			if (is_visible_in_tree()) {
				refresh();
			}
		} break;
	}
}

void TaskPalette::_bind_methods() {
	ClassDB::bind_method(D_METHOD("refresh"), &TaskPalette::refresh);
	ClassDB::bind_method(D_METHOD("_get_drag_data_fw"), &TaskPalette::_get_drag_data_fw);
	ClassDB::bind_method(D_METHOD("_can_drop_data_fw"), &TaskPalette::_can_drop_data_fw);
	ClassDB::bind_method(D_METHOD("_drop_data_fw"), &TaskPalette::_drop_data_fw);

	ADD_SIGNAL(MethodInfo("task_selected"));
	ADD_SIGNAL(MethodInfo("favorite_tasks_changed"));
}

TaskPalette::TaskPalette() {
	VBoxContainer *vb = memnew(VBoxContainer);
	add_child(vb);

	HBoxContainer *hb = memnew(HBoxContainer);
	vb->add_child(hb);

	tool_filters = memnew(Button);
	tool_filters->set_tooltip_text(TTR("Show filters"));
	tool_filters->set_flat(true);
	tool_filters->set_toggle_mode(true);
	tool_filters->set_focus_mode(FocusMode::FOCUS_NONE);
	hb->add_child(tool_filters);

	filter_edit = memnew(LineEdit);
	filter_edit->set_clear_button_enabled(true);
	filter_edit->set_placeholder(TTR("Filter tasks"));
	filter_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	hb->add_child(filter_edit);

	tool_refresh = memnew(Button);
	tool_refresh->set_tooltip_text(TTR("Refresh tasks"));
	tool_refresh->set_flat(true);
	tool_refresh->set_focus_mode(FocusMode::FOCUS_NONE);
	hb->add_child(tool_refresh);

	tree = memnew(Tree);
	tree->set_h_size_flags(SIZE_EXPAND_FILL);
	tree->set_v_size_flags(SIZE_EXPAND_FILL);
	tree->set_hide_root(true);
	tree->set_select_mode(Tree::SELECT_SINGLE);
	tree->set_allow_rmb_select(true);
	tree->set_allow_reselect(true);
	tree->set_drag_forwarding(callable_mp(this, &TaskPalette::_get_drag_data_fw), callable_mp(this, &TaskPalette::_can_drop_data_fw), callable_mp(this, &TaskPalette::_drop_data_fw));

	HBoxContainer *body = memnew(HBoxContainer);
	body->set_v_size_flags(SIZE_EXPAND_FILL);
	vb->add_child(body);
	body->add_child(tree);

	description_pane = memnew(PanelContainer);
	description_pane->set_h_size_flags(SIZE_EXPAND_FILL);
	description_pane->hide();
	body->add_child(description_pane);

	description_text = memnew(RichTextLabel);
	description_text->set_use_bbcode(false);
	description_text->set_selection_enabled(true);
	description_text->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	description_text->set_focus_mode(FOCUS_NONE);
	description_pane->add_child(description_text);

	sticky_overlay = memnew(Control);
	sticky_overlay->set_mouse_filter(MOUSE_FILTER_STOP);
	sticky_overlay->hide();
	tree->add_child(sticky_overlay);

	menu = memnew(PopupMenu);
	add_child(menu);

	filter_popup = memnew(PopupPanel);
	{
		VBoxContainer *vbox = memnew(VBoxContainer);
		filter_popup->add_child(vbox);

		Label *type_header = memnew(Label);
		type_header->set_text(TTR("Type"));
		type_header->set_theme_type_variation("HeaderSmall");
		vbox->add_child(type_header);

		HBoxContainer *type_filter = memnew(HBoxContainer);
		vbox->add_child(type_filter);

		Ref<ButtonGroup> type_filter_group;
		type_filter_group.instantiate();

		type_all = memnew(Button);
		type_all->set_text(TTR("All"));
		type_all->set_tooltip_text(TTR("Show tasks of all types"));
		type_all->set_toggle_mode(true);
		type_all->set_focus_mode(FocusMode::FOCUS_NONE);
		type_all->set_button_group(type_filter_group);
		type_all->set_pressed(true);
		type_filter->add_child(type_all);

		type_core = memnew(Button);
		type_core->set_text(TTR("Core"));
		type_core->set_tooltip_text(TTR("Show only core tasks"));
		type_core->set_toggle_mode(true);
		type_core->set_focus_mode(FocusMode::FOCUS_NONE);
		type_core->set_button_group(type_filter_group);
		type_filter->add_child(type_core);

		type_user = memnew(Button);
		type_user->set_text(TTR("User"));
		type_user->set_tooltip_text(TTR("Show only user-implemented tasks (aka scripts)"));
		type_user->set_toggle_mode(true);
		type_user->set_focus_mode(FocusMode::FOCUS_NONE);
		type_user->set_button_group(type_filter_group);
		type_filter->add_child(type_user);

		Control *space1 = memnew(Control);
		space1->set_custom_minimum_size(Size2(0, 4));
		vbox->add_child(space1);

		Label *category_header = memnew(Label);
		category_header->set_text(TTR("Categories"));
		category_header->set_theme_type_variation("HeaderSmall");
		vbox->add_child(category_header);

		HBoxContainer *category_filter = memnew(HBoxContainer);
		vbox->add_child(category_filter);

		Ref<ButtonGroup> category_filter_group;
		category_filter_group.instantiate();

		category_all = memnew(Button);
		category_all->set_text(TTR("All"));
		category_all->set_tooltip_text(TTR("Show tasks of all categories"));
		category_all->set_toggle_mode(true);
		category_all->set_focus_mode(FocusMode::FOCUS_NONE);
		category_all->set_pressed(true);
		category_all->set_button_group(category_filter_group);
		category_filter->add_child(category_all);

		category_include = memnew(Button);
		category_include->set_text(TTR("Include"));
		category_include->set_tooltip_text(TTR("Show tasks from selected categories"));
		category_include->set_toggle_mode(true);
		category_include->set_focus_mode(FocusMode::FOCUS_NONE);
		category_include->set_button_group(category_filter_group);
		category_filter->add_child(category_include);

		category_exclude = memnew(Button);
		category_exclude->set_text(TTR("Exclude"));
		category_exclude->set_tooltip_text(TTR("Don't show tasks from selected categories"));
		category_exclude->set_toggle_mode(true);
		category_exclude->set_focus_mode(FocusMode::FOCUS_NONE);
		category_exclude->set_button_group(category_filter_group);
		category_filter->add_child(category_exclude);

		category_choice = memnew(VBoxContainer);
		vbox->add_child(category_choice);

		HBoxContainer *selection_controls = memnew(HBoxContainer);
		selection_controls->set_alignment(BoxContainer::ALIGNMENT_CENTER);
		category_choice->add_child(selection_controls);

		select_all = memnew(Button);
		select_all->set_tooltip_text(TTR("Select all categories"));
		select_all->set_focus_mode(FocusMode::FOCUS_NONE);
		selection_controls->add_child(select_all);

		deselect_all = memnew(Button);
		deselect_all->set_tooltip_text(TTR("Deselect all categories"));
		deselect_all->set_focus_mode(FocusMode::FOCUS_NONE);
		selection_controls->add_child(deselect_all);

		category_scroll = memnew(ScrollContainer);
		category_choice->add_child(category_scroll);

		category_list = memnew(VBoxContainer);
		category_scroll->add_child(category_list);
	}
	add_child(filter_popup);
}

TaskPalette::~TaskPalette() {
}

#endif // ! TOOLS_ENABLED
