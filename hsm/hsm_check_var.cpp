/**
 * hsm_check_var.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#include "hsm_check_var.h"

#include "../util/limbo_string_names.h"

#ifdef LIMBOAI_MODULE
#include "core/config/engine.h"
#include "core/object/class_db.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#endif // LIMBOAI_GDEXTENSION

void HSMCheckVar::set_variable(const StringName &p_variable) {
	variable = p_variable;
	emit_changed();
}

void HSMCheckVar::set_check_type(LimboUtility::CheckType p_check_type) {
	check_type = p_check_type;
	emit_changed();
}

void HSMCheckVar::set_value(const Ref<BBVariant> &p_value) {
	value = p_value;
	emit_changed();
	// The editor redraws the row's name when the value inside changes, as it does for
	// BTCheckVar.
	if (Engine::get_singleton()->is_editor_hint() && value.is_valid() &&
			!value->is_connected(LW_NAME(changed), callable_mp((Resource *)this, &Resource::emit_changed))) {
		value->connect(LW_NAME(changed), callable_mp((Resource *)this, &Resource::emit_changed));
	}
}

String HSMCheckVar::_generate_name() {
	if (variable == StringName()) {
		return "CheckVar ???";
	}
	return vformat("%s %s %s", LimboUtility::get_singleton()->decorate_var(variable),
			LimboUtility::get_singleton()->get_check_operator_string(check_type),
			value.is_valid() ? Variant(value) : Variant("???"));
}

bool HSMCheckVar::_check() {
	ERR_FAIL_COND_V_MSG(variable == StringName(), false, "HSMCheckVar: `variable` is not set.");
	ERR_FAIL_COND_V_MSG(!value.is_valid(), false, "HSMCheckVar: `value` is not set.");
	Ref<Blackboard> bb = get_blackboard();
	ERR_FAIL_COND_V_MSG(bb.is_null(), false, "HSMCheckVar: Not bound to a machine.");
	ERR_FAIL_COND_V_MSG(!bb->has_var(variable), false, vformat("HSMCheckVar: Blackboard variable doesn't exist: \"%s\". The row does not pass.", variable));

	// A BBVariant that reads another variable resolves it against the scene root. The agent
	// is the nearest node the machine has; a saved constant never looks at it.
	Variant left_value = bb->get_var(variable, Variant());
	Variant right_value = value->get_value(get_agent(), bb);
	return LimboUtility::get_singleton()->perform_check(check_type, left_value, right_value);
}

void HSMCheckVar::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_variable", "variable"), &HSMCheckVar::set_variable);
	ClassDB::bind_method(D_METHOD("get_variable"), &HSMCheckVar::get_variable);
	ClassDB::bind_method(D_METHOD("set_check_type", "check_type"), &HSMCheckVar::set_check_type);
	ClassDB::bind_method(D_METHOD("get_check_type"), &HSMCheckVar::get_check_type);
	ClassDB::bind_method(D_METHOD("set_value", "value"), &HSMCheckVar::set_value);
	ClassDB::bind_method(D_METHOD("get_value"), &HSMCheckVar::get_value);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "variable"), "set_variable", "get_variable");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "check_type", PROPERTY_HINT_ENUM, "Equal,Less Than,Less Than Or Equal,Greater Than,Greater Than Or Equal,Not Equal"), "set_check_type", "get_check_type");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "value", PROPERTY_HINT_RESOURCE_TYPE, "BBVariant"), "set_value", "get_value");
}
