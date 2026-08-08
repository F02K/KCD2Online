#pragma once

namespace wh::combatmodule
{
	class C_CombatActor;
}

namespace kcd2o::kcse
{
	// Read-only view of the game's CombatActor state. Observing combat must
	// never start, stop, or otherwise repair native combat actions.
	struct native_combat_state
	{
		bool combat_mode{};
		bool active_in_combat{};
	};

	[[nodiscard]] native_combat_state read_combat_state(
	    const wh::combatmodule::C_CombatActor *actor) noexcept;
}
