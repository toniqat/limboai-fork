/**
 * hsm_condition.cpp
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#include "hsm_condition.h"

#include "../compat/object.h"
#include "../util/limbo_string_names.h"

#ifdef LIMBOAI_MODULE
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#endif // LIMBOAI_MODULE

#ifdef LIMBOAI_GDEXTENSION
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/core/class_db.hpp>
#endif // LIMBOAI_GDEXTENSION

String HSMCondition::_generate_name() {
	// Same fallback BTTask uses: a script is named after its file, which is the name the
	// person who wrote it already chose. `in_combat.gd` reads as `InCombat`.
	Ref<Script> sc = GET_SCRIPT(this);
	if (sc.is_valid() && sc->get_path().is_absolute_path()) {
		return sc->get_path().get_basename().get_file().to_pascal_case();
	}
	return get_class();
}

String HSMCondition::get_display_name() {
	Ref<Script> sc = get_script();
	if (sc.is_valid() && sc->is_tool() && has_method(LW_NAME(_generate_name))) {
		// Only a tool script runs in the editor, and the editor is where this name is read
		// most. A non-tool script that defines the method still gets the file name: calling
		// it would run nothing and hand back an empty string.
		String call_result;
		if (GDVIRTUAL_CALL(_generate_name, call_result) && !call_result.is_empty() && call_result != "<null>") {
			return call_result;
		}
	}
	return _generate_name();
}

bool HSMCondition::check() {
	bool ret = false;
	if (GDVIRTUAL_CALL(_check, ret)) {
		return ret;
	}
	return _check();
}

void HSMCondition::bind_to(Node *p_machine, Node *p_agent, const Ref<Blackboard> &p_blackboard) {
	ERR_FAIL_COND_MSG(bound, "HSMCondition: Already bound to a machine. Each machine needs its own copy.");
	machine = p_machine;
	agent = p_agent;
	blackboard = p_blackboard;
	bound = true;
	_setup();
	GDVIRTUAL_CALL(_setup);
}

void HSMCondition::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_blackboard"), &HSMCondition::get_blackboard);
	ClassDB::bind_method(D_METHOD("get_agent"), &HSMCondition::get_agent);
	ClassDB::bind_method(D_METHOD("get_machine"), &HSMCondition::get_machine);
	ClassDB::bind_method(D_METHOD("is_bound"), &HSMCondition::is_bound);
	ClassDB::bind_method(D_METHOD("get_display_name"), &HSMCondition::get_display_name);
	ClassDB::bind_method(D_METHOD("check"), &HSMCondition::check);

	// Runtime only, so none of them is stored: duplicating the condition for another machine
	// must not carry the first machine's binding along, and a file must never hold one.
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "blackboard", PROPERTY_HINT_RESOURCE_TYPE, "Blackboard", PROPERTY_USAGE_NONE), "", "get_blackboard");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "agent", PROPERTY_HINT_RESOURCE_TYPE, "Node", PROPERTY_USAGE_NONE), "", "get_agent");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "machine", PROPERTY_HINT_RESOURCE_TYPE, "LimboHSM", PROPERTY_USAGE_NONE), "", "get_machine");

	GDVIRTUAL_BIND(_generate_name);
	GDVIRTUAL_BIND(_setup);
	GDVIRTUAL_BIND(_check);
}

HSMCondition::HSMCondition() {
}
