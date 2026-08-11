#include "multiplayer/avatar_visual.hpp"
#include "multiplayer/voice_spatial.hpp"

#include <cassert>
#include <cmath>

int main()
{
	using namespace kcd2o;
	protocol::AvatarDescriptor authoritative;
	authoritative.set_archetype_id("763db0bb-4469-497d-bdc9-712b3df91b5a");
	authoritative.set_revision(7);

	protocol::AvatarDescriptor local;
	local.set_stance(protocol::AVATAR_STANCE_READY);
	local.set_weapon_class(protocol::AVATAR_WEAPON_CLASS_ONE_HANDED);
	local.set_weapon_drawn(true);
	local.set_active_weapon_set(protocol::AVATAR_WEAPON_SET_PRIMARY);
	local.set_combat_mode(true);
	local.set_active_in_combat(true);
	auto *right = local.add_equipment();
	right->set_definition_id("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
	right->set_equipped_slot("PrimaryMainHand");
	auto *body = local.add_equipment();
	body->set_definition_id("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
	body->set_equipped_slot("body_cloth_padded");

	const auto merged = merge_avatar_visual(authoritative, local, std::string{"11111111-2222-4333-8444-555555555555"});
	assert(merged.revision() == 7);
	assert(merged.equipment_size() == 2);
	assert(merged.equipment(0).equipped_slot() == "PrimaryMainHand");
	assert(merged.equipment(1).equipped_slot() == "body_cloth_padded");
	assert(merged.weapon_drawn());
	assert(avatar_weapon_should_draw(merged));
	assert(merged.active_weapon_set() == protocol::AVATAR_WEAPON_SET_PRIMARY);
	assert(merged.combat_mode());
	assert(merged.active_in_combat());

	auto reordered = merged;
	reordered.clear_equipment();
	*reordered.add_equipment() = merged.equipment(1);
	*reordered.add_equipment() = merged.equipment(0);
	canonicalize_avatar_visual(reordered);
	assert(same_avatar_visual(merged, reordered));

	reordered.set_weapon_drawn(false);
	canonicalize_avatar_visual(reordered);
	assert(reordered.weapon_class() == protocol::AVATAR_WEAPON_CLASS_NONE);
	assert(reordered.active_weapon_set() == protocol::AVATAR_WEAPON_SET_NONE);
	assert(!avatar_weapon_should_draw(reordered));
	assert(same_avatar_equipment(merged, reordered));
	assert(!same_avatar_visual(merged, reordered));

	auto different_equipment = merged;
	different_equipment.mutable_equipment(0)->set_definition_id(
	    "cccccccc-cccc-4ccc-8ccc-cccccccccccc");
	assert(!same_avatar_equipment(merged, different_equipment));

	auto unarmed = merged;
	unarmed.set_weapon_class(protocol::AVATAR_WEAPON_CLASS_UNARMED);
	unarmed.set_active_weapon_set(protocol::AVATAR_WEAPON_SET_PRIMARY);
	unarmed.set_weapon_drawn(true);
	canonicalize_avatar_visual(unarmed);
	assert(unarmed.weapon_class() == protocol::AVATAR_WEAPON_CLASS_UNARMED);
	assert(unarmed.weapon_drawn());
	assert(avatar_weapon_should_draw(unarmed));

	protocol::PlayerProfile equipment_profile;
	auto *equipped = equipment_profile.add_inventory();
	equipped->set_definition_id("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
	equipped->set_equipped_slot("body_cloth_padded");
	auto *stored = equipment_profile.add_inventory();
	stored->set_definition_id("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
	auto profile_avatar = merged;
	replace_avatar_equipment_from_profile(profile_avatar, equipment_profile);
	assert(profile_avatar.equipment_size() == 1);
	assert(profile_avatar.equipment(0).definition_id()
	    == "dddddddd-dddd-4ddd-8ddd-dddddddddddd");
	assert(profile_avatar.weapon_class()
	    == protocol::AVATAR_WEAPON_CLASS_NONE);

	protocol::AvatarDescriptor naked;
	assert(avatar_requests_no_equipment(naked));
	const auto unequipped = merge_avatar_visual(merged, naked, std::nullopt);
	assert(unequipped.equipment().empty());
	assert(avatar_requests_no_equipment(unequipped));

	protocol::Vec3 game_position;
	game_position.set_x(10.0F);
	game_position.set_y(2000.0F);
	game_position.set_z(3.0F);
	const auto audio_position = to_voice_audio_coordinates(game_position, 1.65F);
	assert(audio_position.x == 10.0F);
	assert(std::abs(audio_position.y - 4.65F) < 0.0001F);
	assert(audio_position.z == 2000.0F);
	return 0;
}
