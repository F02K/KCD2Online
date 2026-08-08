#include "kcse/native_combat_observer.hpp"

#include <combatmodule/C_CombatActor.h>

namespace kcd2o::kcse
{
	native_combat_state read_combat_state(
	    const wh::combatmodule::C_CombatActor *actor) noexcept
	{
		if (!actor)
			return {};
		auto **vtable = *reinterpret_cast<void ***>(
		    const_cast<wh::combatmodule::C_CombatActor *>(actor));
		using query = bool (__fastcall *)(
		    const wh::combatmodule::C_CombatActor *);
		return {
		    reinterpret_cast<query>(vtable[1])(actor),
		    reinterpret_cast<query>(vtable[2])(actor)};
	}
}
