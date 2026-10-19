/**
 * editor_tooltip.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifdef TOOLS_ENABLED

#include "editor_tooltip.h"

#include "../compat/editor_scale.h"

#ifdef LIMBOAI_MODULE
#include "editor/editor_interface.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/font.h"
#include "scene/resources/theme.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/margin_container.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

namespace {

// Body text wraps beyond this width (unscaled pixels).
constexpr float BODY_MAX_WIDTH = 360.0;

} // namespace

Control *limbo_make_tooltip(const Ref<Texture2D> &p_icon, const String &p_title, const String &p_title_variation, const String &p_body) {
	// The tooltip panel's own content margin is small; this inset keeps the icon off the
	// top left corner and balances the other edges.
	MarginContainer *margin = memnew(MarginContainer);
	margin->add_theme_constant_override("margin_left", int(6 * EDSCALE));
	margin->add_theme_constant_override("margin_top", int(5 * EDSCALE));
	margin->add_theme_constant_override("margin_right", int(6 * EDSCALE));
	margin->add_theme_constant_override("margin_bottom", int(4 * EDSCALE));

	VBoxContainer *box = memnew(VBoxContainer);
	margin->add_child(box);

	HBoxContainer *head = memnew(HBoxContainer);
	box->add_child(head);
	if (p_icon.is_valid()) {
		TextureRect *icon = memnew(TextureRect);
		icon->set_texture(p_icon);
		icon->set_custom_minimum_size(Size2(16, 16) * EDSCALE);
		icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		icon->set_v_size_flags(Control::SIZE_SHRINK_CENTER);
		head->add_child(icon);
	}
	Label *title = memnew(Label);
	title->set_text(p_title);
	if (!p_title_variation.is_empty()) {
		title->set_theme_type_variation(p_title_variation);
	}
	head->add_child(title);

	if (!p_body.is_empty()) {
		Label *body = memnew(Label);
		body->set_text(p_body);
		// A wrapping label needs a width; take the text's own width up to the maximum.
		float width = BODY_MAX_WIDTH * EDSCALE;
		Ref<Theme> theme = EditorInterface::get_singleton()->get_editor_theme();
		if (theme.is_valid()) {
			Ref<Font> font = theme->get_font("font", "Label");
			int font_size = theme->get_font_size("font_size", "Label");
			if (font.is_valid()) {
				width = MIN(width, font->get_multiline_string_size(p_body, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x + 2 * EDSCALE);
			}
		}
		body->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		body->set_custom_minimum_size(Size2(width, 0));
		box->add_child(body);
	}
	return margin;
}

#endif // TOOLS_ENABLED
