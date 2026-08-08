#include "kcse/player_respawn_guard.hpp"

#include <cassert>

int main()
{
	using namespace kcd2o::kcse;

	constexpr player_respawn_identity identity{
	    .entity = 0x1000,
	    .actor = 0x2000,
	    .soul = 0x3000,
	    .entity_id = 42,
	    .soul_guid_high = 1,
	    .soul_guid_low = 2,
	    .shared_soul_guid_high = 3,
	    .shared_soul_guid_low = 4};
	static_assert(identity.complete());
	static_assert(
	    validate_player_respawn_identity(identity, identity)
	    == player_respawn_identity_result::stable);

	auto changed = identity;
	changed.entity = 0;
	assert(
	    validate_player_respawn_identity(identity, changed)
	    == player_respawn_identity_result::incomplete_after);

	changed = identity;
	changed.entity_id = 43;
	assert(
	    validate_player_respawn_identity(identity, changed)
	    == player_respawn_identity_result::entity_changed);

	changed = identity;
	changed.actor = 0x2001;
	assert(
	    validate_player_respawn_identity(identity, changed)
	    == player_respawn_identity_result::actor_changed);

	changed = identity;
	changed.soul = 0x3001;
	assert(
	    validate_player_respawn_identity(identity, changed)
	    == player_respawn_identity_result::soul_changed);

	changed = identity;
	changed.soul_guid_low = 5;
	assert(
	    validate_player_respawn_identity(identity, changed)
	    == player_respawn_identity_result::soul_guid_changed);

	changed = identity;
	changed.shared_soul_guid_high = 6;
	assert(
	    validate_player_respawn_identity(identity, changed)
	    == player_respawn_identity_result::shared_soul_guid_changed);

	return 0;
}
