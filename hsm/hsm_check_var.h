/**
 * hsm_check_var.h
 * =============================================================================
 * Copyright (c) 2023-present Serhii Snitsaruk and the LimboAI contributors.
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 * =============================================================================
 */

#ifndef HSM_CHECK_VAR_H
#define HSM_CHECK_VAR_H

#include "hsm_condition.h"

#include "../blackboard/bb_param/bb_variant.h"
#include "../util/limbo_utility.h"

// The condition most rows need, without a script: compare one blackboard variable with a value.
//
// It asks the same question BTCheckVar asks in a tree, so a value a tree already checks can
// guard a transition too, written the same way. The value is a BBVariant, so the right side
// can be a constant saved in the file or another blackboard variable.
//
// It reads the blackboard of the machine it is bound to. A variable missing from it is a setup
// error, not a "no": the row then never passes and says so, instead of quietly comparing
// against null.
class HSMCheckVar : public HSMCondition {
	GDCLASS(HSMCheckVar, HSMCondition);

private:
	StringName variable;
	LimboUtility::CheckType check_type = LimboUtility::CheckType::CHECK_EQUAL;
	Ref<BBVariant> value;

protected:
	static void _bind_methods();

	virtual String _generate_name() override;
	virtual bool _check() override;

public:
	void set_variable(const StringName &p_variable);
	StringName get_variable() const { return variable; }

	void set_check_type(LimboUtility::CheckType p_check_type);
	LimboUtility::CheckType get_check_type() const { return check_type; }

	void set_value(const Ref<BBVariant> &p_value);
	Ref<BBVariant> get_value() const { return value; }
};

#endif // HSM_CHECK_VAR_H
