/**
 * editor_tooltip.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifndef LIMBO_EDITOR_TOOLTIP_H
#define LIMBO_EDITOR_TOOLTIP_H

#ifdef TOOLS_ENABLED

#ifdef LIMBOAI_MODULE
#include "scene/gui/control.h"
#include "scene/resources/texture.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/texture2d.hpp>
using namespace godot;
#endif // LIMBOAI_GDEXTENSION

// A compact custom tooltip: a header row (icon, then the title in p_title_variation) and an
// optional body below it, inset from the tooltip panel's edges so the icon does not sit in
// the corner. Sized to its text: the body wraps only beyond a maximum width. Unlike the
// inspector's help bit (fixed 640 px wide) it leaves no empty space on the right.
Control *limbo_make_tooltip(const Ref<Texture2D> &p_icon, const String &p_title, const String &p_title_variation, const String &p_body);

#endif // TOOLS_ENABLED

#endif // LIMBO_EDITOR_TOOLTIP_H
