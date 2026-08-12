#include "multiplayer/protocol.hpp"
#include "multiplayer/emote_catalog.hpp"
#include "multiplayer/world_catalog.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <string>
#include <vector>

namespace
{
	kcd2o::protocol::EnvironmentState environment()
	{
		kcd2o::protocol::EnvironmentState state;
		state.set_revision(1);
		state.set_time_of_day_hours(8.0);
		state.set_world_time_seconds(8.0 * kcd2o::seconds_per_hour);
		state.set_time_scale(15.0F);
		state.set_server_time_ms(1);
		state.set_weather_id(1);
		state.set_weather_transition_ms(30'000);
		state.set_weather_revision(1);
		return state;
	}

	kcd2o::protocol::TransformState transform(
	    float x,
	    float speed,
	    std::uint64_t sequence)
	{
		kcd2o::protocol::TransformState value;
		value.mutable_position()->set_x(x);
		value.mutable_position()->set_y(2.0F);
		value.mutable_position()->set_z(3.0F);
		value.mutable_rotation()->set_w(2.0F);
		value.mutable_velocity()->set_x(speed);
		value.set_sequence(sequence);
		value.set_client_time_ms(sequence * 10);
		return value;
	}
}

int main()
{
	using kcd2o::canonical_level_id;
	using kcd2o::find_native_world_level;
	assert(find_native_world_level("2")->name == "trosecko");
	assert(find_native_world_level("kutnohorsko")->id == "3");
	assert(find_native_world_level("4")->production);
	assert(!find_native_world_level("300")->production);
	assert(!find_native_world_level("sandbox"));
	assert(canonical_level_id("klaster") == "4");
	assert(canonical_level_id("custom_level") == "custom_level");

	using namespace kcd2o;

	assert(is_valid_display_name("Henry"));
	assert(is_valid_display_name("Jindřich"));
	assert(!is_valid_display_name("ab"));
	assert(!is_valid_display_name(" leading"));
	assert(!valid_utf8_with_codepoint_count("\xC0\xAF", 1, 10));
	assert(is_valid_chat("Hello, Kuttenberg!"));
	assert(!is_valid_chat(""));

	protocol::Envelope lane_message;
	lane_message.mutable_chat_send()->set_text("hello");
	assert(lane_for(lane_message) == traffic_lane::interactive);
	lane_message.Clear();
	lane_message.mutable_client_transform();
	assert(lane_for(lane_message) == traffic_lane::player_realtime);
	lane_message.Clear();
	lane_message.mutable_client_npc_update_batch();
	assert(lane_for(lane_message) == traffic_lane::npc_realtime);
	lane_message.Clear();
	lane_message.mutable_client_profile_update();
	assert(lane_for(lane_message) == traffic_lane::ordered_state);
	lane_message.Clear();
	auto *voice_lane = lane_message.mutable_client_voice_frame();
	voice_lane->set_sequence(1);
	voice_lane->set_capture_time_ms(1);
	voice_lane->set_opus("opus");
	voice_lane->set_visemes(std::string(voice_viseme_count, '\0'));
	assert(lane_for(lane_message) == traffic_lane::voice_realtime);
	assert(encode(lane_message, reliability::unreliable));
	voice_lane->set_visemes("short");
	assert(!encode(lane_message, reliability::unreliable));
	lane_message.Clear();
	assert(lane_for(lane_message) == traffic_lane::ordered_state);

	protocol::Envelope envelope;
	auto *hello = envelope.mutable_client_hello();
	hello->set_version(kcd2o_version);
	hello->set_whgame_timestamp(supported_whgame_timestamp);
	hello->set_whgame_image_size(supported_whgame_image_size);
	hello->set_display_name("Henry");
	hello->set_level_id("sandbox");
	auto *runtime = hello->mutable_runtime();
	runtime->set_features(required_client_runtime_capabilities);
	runtime->set_kcse_version(1);
	runtime->set_game_version(0x01050600);
	runtime->set_release_index(1);
	runtime->set_runtime_epoch(1);
	const auto &address_library = supported_address_libraries.back();
	runtime->set_address_library(address_library.build_key);
	runtime->set_address_library_distribution(address_library.distribution);
	runtime->set_address_library_format(address_library.format_version);
	runtime->set_address_library_entries(address_library.entry_count);
	runtime->set_address_library_sha256(address_library.sha256);
	std::string error;
	const auto encoded = encode(envelope, reliability::reliable, &error);
	assert(encoded);
	assert(encoded->delivery == reliability::reliable);
	const auto decoded = decode(encoded->bytes, &error);
	assert(decoded);
	assert(decoded->client_hello().display_name() == "Henry");
	assert(decoded->client_hello().version() == "0.1.5");
	auto incompatible = envelope;
	incompatible.mutable_client_hello()->set_version("0.0.8");
	assert(encode(incompatible, reliability::reliable, &error));
	incompatible.mutable_client_hello()->set_version("invalid version");
	assert(!encode(incompatible, reliability::reliable, &error));
	auto truncated = encoded->bytes;
	truncated.pop_back();
	assert(!decode(truncated, &error));

	std::vector<std::byte> empty;
	assert(!decode(empty, &error));
	std::vector<std::byte> oversized(max_application_message_size + 1);
	assert(!decode(oversized, &error));

	auto value = transform(1.0F, 0.0F, 1);
	assert(is_finite_transform(value));
	assert(normalize_rotation(value.mutable_rotation()));
	assert(std::abs(value.rotation().w() - 1.0F) < 0.0001F);
	assert(movement_mode_for(value) == protocol::MOVEMENT_MODE_IDLE);
	value.mutable_velocity()->set_x(2.0F);
	assert(movement_mode_for(value) == protocol::MOVEMENT_MODE_WALK);
	value.mutable_velocity()->set_x(5.0F);
	assert(movement_mode_for(value) == protocol::MOVEMENT_MODE_SPRINT);
	value.mutable_velocity()->set_x(3.5F);
	assert(movement_mode_for(value) == protocol::MOVEMENT_MODE_RUN);
	auto *locomotion = value.mutable_locomotion();
	locomotion->mutable_local_velocity();
	locomotion->mutable_acceleration();
	locomotion->mutable_facing_direction()->set_y(1.0F);
	locomotion->set_speed(3.5F);
	auto *animation = value.mutable_animation();
	animation->set_sequence(1);
	animation->set_fragment("Jump");
	animation->set_active(true);
	assert(is_finite_transform(value));
	animation->set_fragment("Jump'); os.execute('bad");
	assert(!is_finite_transform(value));
	animation->set_fragment("CombatAttack");
	assert(!is_finite_transform(value));
	animation->set_fragment("Jump");
	for (const auto &emote : emote_catalog)
	{
		auto emote_transform = transform(1.0F, 0.0F, 1);
		auto *emote_animation = emote_transform.mutable_animation();
		emote_animation->set_sequence(1);
		emote_animation->set_fragment(emote.fragment);
		emote_animation->set_active(true);
		assert(is_finite_transform(emote_transform));
		assert(find_emote(emote.kind) == &emote);
	}
	value.mutable_position()->set_x(std::numeric_limits<float>::infinity());
	assert(!is_finite_transform(value));

	protocol::Envelope no_payload;
	assert(!encode(no_payload, reliability::reliable, &error));

	protocol::Envelope npc_discovery;
	auto *npc_observation =
	    npc_discovery.mutable_client_npc_discovery()->add_observations();
	npc_observation->set_authored_guid(0x1234);
	npc_observation->set_kind(protocol::NPC_KIND_HUMAN);
	*npc_observation->mutable_transform() = transform(4.0F, 0.0F, 1);
	assert(encode(npc_discovery, reliability::reliable, &error));
	const auto duplicate_npc_observation = *npc_observation;
	*npc_discovery.mutable_client_npc_discovery()->add_observations() =
	    duplicate_npc_observation;
	assert(!encode(npc_discovery, reliability::reliable, &error));

	protocol::Envelope npc_update;
	auto *npc_update_message = npc_update.mutable_client_npc_update();
	npc_update_message->set_npc_id(0x1234);
	npc_update_message->set_generation(1);
	npc_update_message->set_lease_id(7);
	*npc_update_message->mutable_transform() = transform(5.0F, 1.0F, 2);
	assert(encode(npc_update, reliability::unreliable, &error));
	npc_update_message->set_lease_id(0);
	assert(!encode(npc_update, reliability::unreliable, &error));
	npc_update_message->set_lease_id(7);
	protocol::Envelope npc_update_batch;
	*npc_update_batch.mutable_client_npc_update_batch()->add_updates() =
	    *npc_update_message;
	assert(encode(npc_update_batch, reliability::unreliable, &error));
	*npc_update_batch.mutable_client_npc_update_batch()->add_updates() =
	    *npc_update_message;
	assert(!encode(npc_update_batch, reliability::unreliable, &error));

	protocol::Envelope npc_enter;
	auto *npc_state = npc_enter.mutable_server_npc_enter()->mutable_state();
	npc_state->set_npc_id(0x1234);
	npc_state->set_generation(1);
	npc_state->set_authored_guid(0x1234);
	npc_state->set_kind(protocol::NPC_KIND_HUMAN);
	*npc_state->mutable_transform() = transform(5.0F, 0.0F, 1);
	npc_state->set_revision(1);
	npc_state->set_authority_player_id(3);
	npc_state->set_lease_id(7);
	assert(encode(npc_enter, reliability::reliable, &error));
	protocol::Envelope npc_motion;
	auto *motion = npc_motion.mutable_server_npc_motion();
	motion->set_server_tick(1);
	auto *motion_state = motion->add_npcs();
	motion_state->set_npc_id(npc_state->npc_id());
	motion_state->set_generation(npc_state->generation());
	motion_state->set_revision(2);
	*motion_state->mutable_transform() = transform(5.5F, 0.0F, 1);
	assert(encode(npc_motion, reliability::unreliable, &error));

	protocol::Envelope npc_gameplay;
	auto *gameplay = npc_gameplay.mutable_server_npc_gameplay_update();
	gameplay->set_npc_id(npc_state->npc_id());
	gameplay->set_generation(npc_state->generation());
	gameplay->set_state_revision(2);
	gameplay->mutable_gameplay()->set_revision(1);
	gameplay->mutable_gameplay()->set_health(100.0F);
	gameplay->mutable_gameplay()->set_max_health(100.0F);
	gameplay->mutable_gameplay()->set_behavior(
	    protocol::NPC_BEHAVIOR_IDLE);
	assert(encode(npc_gameplay, reliability::reliable, &error));
	npc_state->set_kind(protocol::NPC_KIND_UNSPECIFIED);
	assert(!encode(npc_enter, reliability::reliable, &error));

	protocol::Envelope activity_start;
	auto *start_activity = activity_start.mutable_client_activity_start();
	start_activity->set_kind(
	    protocol::PLAYER_ACTIVITY_KIND_BLACKSMITHING);
	start_activity->set_station_guid(0xAABBCCDDULL);
	assert(encode(activity_start, reliability::reliable, &error));
	auto invalid_activity_start = activity_start;
	invalid_activity_start.mutable_client_activity_start()->set_kind(
	    protocol::PLAYER_ACTIVITY_KIND_NONE);
	assert(!encode(invalid_activity_start, reliability::reliable, &error));

	protocol::Envelope activity_update;
	auto *player_activity =
	    activity_update.mutable_player_activity_updated();
	player_activity->set_player_id(7);
	auto *activity = player_activity->mutable_activity();
	activity->set_kind(protocol::PLAYER_ACTIVITY_KIND_BLACKSMITHING);
	activity->set_station_guid(0xAABBCCDDULL);
	activity->set_session_id(11);
	activity->set_revision(1);
	activity->set_active(true);
	assert(encode(activity_update, reliability::reliable, &error));
	activity->set_session_id(0);
	assert(!encode(activity_update, reliability::reliable, &error));

	protocol::Envelope home_update;
	auto *home_message = home_update.mutable_server_home_marker_updated();
	home_message->set_active(true);
	home_message->set_ledger_revision(2);
	auto *home = home_message->mutable_marker();
	home->set_property_id("3:fixture");
	home->set_level_id("3");
	home->set_display_name("Fixture house");
	home->set_entity_guid(1);
	home->set_role(protocol::PROPERTY_ROLE_OWNER);
	home->mutable_position()->set_x(10.0F);
	home->mutable_position()->set_y(20.0F);
	home->mutable_position()->set_z(30.0F);
	assert(encode(home_update, reliability::reliable, &error));
	home->mutable_position()->set_x(
	    std::numeric_limits<float>::infinity());
	assert(!encode(home_update, reliability::reliable, &error));

	protocol::Envelope invalid_enum;
	auto *snapshot = invalid_enum.mutable_player_joined()->mutable_player();
	snapshot->set_player_id(1);
	snapshot->set_display_name("Henry");
	snapshot->set_movement_mode(
	    static_cast<protocol::MovementMode>(999));
	assert(!encode(invalid_enum, reliability::reliable, &error));

	protocol::Envelope authentication;
	auto *credentials = authentication.mutable_client_authenticate();
	credentials->set_identity_token("token");
	credentials->set_enroll(true);
	assert(!encode(authentication, reliability::reliable, &error));
	credentials->clear_identity_token();
	credentials->set_enroll(false);
	credentials->set_access_token("central-access-token");
	assert(encode(authentication, reliability::reliable, &error));
	credentials->set_identity_token("token");
	assert(!encode(authentication, reliability::reliable, &error));
	credentials->clear_identity_token();
	credentials->set_access_token(
	    std::string(max_access_token_size + 1, 'a'));
	assert(!encode(authentication, reliability::reliable, &error));

	protocol::Envelope profile_envelope;
	auto *profile_update =
	    profile_envelope.mutable_client_profile_update();
	profile_update->set_base_revision(1);
	auto *profile = profile_update->mutable_profile();
	profile->set_player_id(42);
	profile->set_revision(1);
	profile->set_display_name("Henry");
	profile->set_level_id("sandbox");
	auto *avatar = profile->mutable_avatar();
	avatar->set_archetype_id(
	    "763db0bb-4469-497d-bdc9-712b3df91b5a");
	avatar->set_revision(1);
	auto *visible_item = avatar->add_equipment();
	visible_item->set_definition_id(
	    "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
	visible_item->set_equipped_slot("PrimaryMainHand");
	profile->set_money(100);
	profile->set_money_subunits(7);
	for (const auto id : canonical_stat_ids)
	{
		auto *value = profile->add_stats();
		value->set_id(id);
		value->set_level(1);
		value->set_progress(0.0F);
	}
	for (const auto id : canonical_skill_ids)
	{
		auto *value = profile->add_skills();
		value->set_id(id);
		value->set_level(1);
		value->set_progress(0.0F);
	}
	auto *inventory_item = profile->add_inventory();
	inventory_item->set_instance_id(
	    "11111111-1111-4111-8111-111111111111");
	inventory_item->set_definition_id(visible_item->definition_id());
	inventory_item->set_count(1);
	inventory_item->set_quality(100.0F);
	inventory_item->set_condition(1.0F);
	inventory_item->set_equipped_slot(visible_item->equipped_slot());
	auto *quick_slot = profile->add_quick_access_slots();
	quick_slot->set_outfit(0);
	quick_slot->set_type(protocol::QUICK_ACCESS_SLOT_TYPE_WEAPON);
	quick_slot->set_slot(0);
	quick_slot->set_instance_id(inventory_item->instance_id());
	assert(is_valid_profile(*profile));
	auto invalid_qam_slot = *profile;
	invalid_qam_slot.mutable_quick_access_slots(0)->set_slot(8);
	assert(!is_valid_profile(invalid_qam_slot));
	auto missing_qam_item = *profile;
	missing_qam_item.mutable_quick_access_slots(0)->set_instance_id(
	    "22222222-2222-4222-8222-222222222222");
	assert(!is_valid_profile(missing_qam_item));
	auto invalid_money_subunits = *profile;
	invalid_money_subunits.set_money_subunits(
	    money_subunits_per_groschen);
	assert(!is_valid_profile(invalid_money_subunits));
	auto incomplete_profile = *profile;
	incomplete_profile.mutable_skills()->RemoveLast();
	assert(!is_valid_profile(incomplete_profile));
	auto duplicate_instance = *profile;
	*duplicate_instance.add_inventory() = duplicate_instance.inventory(0);
	assert(!is_valid_profile(duplicate_instance));
	auto invalid_condition = *profile;
	invalid_condition.mutable_inventory(0)->set_condition(1.01F);
	assert(!is_valid_profile(invalid_condition));
	const auto encoded_profile =
	    encode(profile_envelope, reliability::reliable, &error);
	assert(encoded_profile);
	const auto decoded_profile = decode(encoded_profile->bytes, &error);
	assert(decoded_profile);
	assert(decoded_profile->client_profile_update()
	    .profile()
	    .quick_access_slots_size() == 1);
	assert(
	    decoded_profile->client_profile_update()
	        .profile()
	        .avatar()
	        .archetype_id()
	    == "763db0bb-4469-497d-bdc9-712b3df91b5a");
	assert(
	    decoded_profile->client_profile_update()
	        .profile()
	        .money_subunits()
	    == 7);

	protocol::Envelope avatar_update_envelope;
	auto *avatar_update =
	    avatar_update_envelope.mutable_client_avatar_update();
	avatar_update->set_base_revision(1);
	*avatar_update->mutable_avatar() = *avatar;
	assert(encode(
	    avatar_update_envelope,
	    reliability::reliable,
	    &error));

	auto duplicate_slot = *avatar;
	*duplicate_slot.add_equipment() = duplicate_slot.equipment(0);
	duplicate_slot.mutable_equipment(1)->set_definition_id(
	    "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
	assert(!is_valid_avatar_descriptor(duplicate_slot));
	auto invalid_uuid = *avatar;
	invalid_uuid.mutable_equipment(0)->set_definition_id("runtime-item-42");
	assert(!is_valid_avatar_descriptor(invalid_uuid));
	auto invalid_slot = *avatar;
	invalid_slot.mutable_equipment(0)->set_equipped_slot("horse_body");
	assert(!is_valid_avatar_descriptor(invalid_slot));
	assert(is_valid_avatar_equipment_slot("SecondaryOffHand"));
	assert(is_valid_avatar_equipment_slot("OversizedOff"));
	assert(is_valid_avatar_equipment_slot("dlc_mantle_outer"));
	assert(!is_valid_avatar_equipment_slot("dlc mantle outer"));
	auto invalid_weapon_state = *avatar;
	invalid_weapon_state.set_weapon_class(
	    protocol::AVATAR_WEAPON_CLASS_NONE);
	invalid_weapon_state.set_weapon_drawn(true);
	assert(!is_valid_avatar_descriptor(invalid_weapon_state));
	auto unarmed_avatar = *avatar;
	unarmed_avatar.set_weapon_class(
	    protocol::AVATAR_WEAPON_CLASS_UNARMED);
	unarmed_avatar.set_weapon_drawn(true);
	unarmed_avatar.set_active_weapon_set(
	    protocol::AVATAR_WEAPON_SET_PRIMARY);
	assert(is_valid_avatar_descriptor(unarmed_avatar));
	unarmed_avatar.set_active_weapon_set(
	    protocol::AVATAR_WEAPON_SET_SECONDARY);
	assert(!is_valid_avatar_descriptor(unarmed_avatar));
	auto oversized_avatar = *avatar;
	for (std::size_t index = oversized_avatar.equipment_size();
	     index <= max_avatar_equipment_items;
	     ++index)
	{
		auto *extra = oversized_avatar.add_equipment();
		extra->set_definition_id(std::format(
		    "aaaaaaaa-aaaa-4aaa-8aaa-{:012x}",
		    index));
		extra->set_equipped_slot("body_plate");
	}
	assert(!is_valid_avatar_descriptor(oversized_avatar));

	protocol::AvatarPolicy policy;
	policy.set_default_archetype_id(
	    "763db0bb-4469-497d-bdc9-712b3df91b5a");
	policy.add_allowed_archetype_ids(
	    "763db0bb-4469-497d-bdc9-712b3df91b5a");
	assert(is_valid_avatar_policy(policy));
	policy.add_allowed_archetype_ids(
	    "763db0bb-4469-497d-bdc9-712b3df91b5a");
	assert(!is_valid_avatar_policy(policy));

	protocol::Envelope static_avatar_in_snapshot;
	auto *static_world = static_avatar_in_snapshot.mutable_world_snapshot();
	*static_world->mutable_environment() = environment();
	auto *static_player = static_world->add_players();
	static_player->set_player_id(1);
	static_player->set_display_name("Henry");
	*static_player->mutable_avatar() = *avatar;
	assert(!encode(
	    static_avatar_in_snapshot,
	    reliability::unreliable,
	    &error));

	protocol::Envelope entity_control;
	auto *entity_control_message =
	    entity_control.mutable_server_entity_control();
	entity_control_message->set_non_player_entities_disabled(false);
	entity_control_message->set_human_npcs_disabled(true);
	entity_control_message->set_animal_npcs_disabled(false);
	const auto encoded_control =
	    encode(entity_control, reliability::reliable, &error);
	assert(encoded_control);
	const auto decoded_control = decode(encoded_control->bytes, &error);
	assert(decoded_control);
	const auto &decoded_entity_control =
	    decoded_control->server_entity_control();
	assert(!decoded_entity_control.non_player_entities_disabled());
	assert(decoded_entity_control.has_human_npcs_disabled());
	assert(decoded_entity_control.human_npcs_disabled());
	assert(decoded_entity_control.has_animal_npcs_disabled());
	assert(!decoded_entity_control.animal_npcs_disabled());

	protocol::Envelope container_update_envelope;
	auto *container_update =
	    container_update_envelope.mutable_client_world_object_update();
	container_update->set_base_revision(0);
	auto *container = container_update->mutable_state();
	container->set_entity_guid(0x12345678ULL);
	container->set_kind(protocol::WORLD_OBJECT_KIND_CONTAINER);
	container->set_revision(0);
	container->set_opened(true);
	container->set_has_inventory(true);
	auto *container_item = container->add_inventory();
	container_item->set_instance_id(
	    "cccccccc-cccc-4ccc-8ccc-cccccccccccc");
	container_item->set_definition_id(
	    "dddddddd-dddd-4ddd-8ddd-dddddddddddd");
	container_item->set_count(1);
	container_item->set_quality(100.0F);
	container_item->set_condition(1.0F);
	assert(is_valid_world_object_state(*container, false));
	assert(encode(
	    container_update_envelope,
	    reliability::reliable,
	    &error));

	auto mismatched_revision = container_update_envelope;
	mismatched_revision.mutable_client_world_object_update()
	    ->mutable_state()
	    ->set_revision(1);
	assert(!encode(
	    mismatched_revision,
	    reliability::reliable,
	    &error));
	auto door_with_inventory = *container;
	door_with_inventory.set_kind(protocol::WORLD_OBJECT_KIND_DOOR);
	assert(!is_valid_world_object_state(door_with_inventory, false));
	auto duplicate_container_item = *container;
	*duplicate_container_item.add_inventory() =
	    duplicate_container_item.inventory(0);
	assert(!is_valid_world_object_state(duplicate_container_item, false));

	protocol::Envelope world_update_envelope;
	container->set_revision(1);
	*world_update_envelope.mutable_world_object_updated()->mutable_state() =
	    *container;
	assert(encode(
	    world_update_envelope,
	    reliability::reliable,
	    &error));

	protocol::Envelope dropped_item_envelope;
	auto *dropped_update =
	    dropped_item_envelope.mutable_client_world_item_update();
	dropped_update->set_base_revision(0);
	auto *dropped = dropped_update->mutable_state();
	dropped->set_instance_id(container_item->instance_id());
	dropped->set_revision(0);
	dropped->set_present(true);
	*dropped->mutable_item() = *container_item;
	*dropped->mutable_transform() = transform(4.0F, 0.0F, 0);
	assert(is_valid_world_item_state(*dropped, false));
	assert(encode(dropped_item_envelope, reliability::reliable, &error));
	dropped_update->set_source_instance_id("not-a-uuid");
	assert(!encode(dropped_item_envelope, reliability::reliable, &error));
	dropped_update->set_source_instance_id(
	    "ffffffff-ffff-4fff-8fff-ffffffffffff");
	dropped_update->set_transfer_count(max_profile_item_count + 1);
	assert(!encode(dropped_item_envelope, reliability::reliable, &error));
	dropped_update->set_transfer_count(dropped->item().count());
	assert(encode(dropped_item_envelope, reliability::reliable, &error));
	auto bad_drop = *dropped;
	bad_drop.mutable_item()->set_instance_id(
	    "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
	assert(!is_valid_world_item_state(bad_drop, false));
	bad_drop = *dropped;
	bad_drop.mutable_transform()->mutable_rotation()->set_w(0.0F);
	assert(!is_valid_world_item_state(bad_drop, false));

	protocol::Envelope incomplete_profile_rejection;
	auto *profile_rejection =
	    incomplete_profile_rejection.mutable_profile_rejected();
	profile_rejection->set_authoritative_revision(profile->revision());
	profile_rejection->set_reason("profile conflict");
	assert(!encode(
	    incomplete_profile_rejection,
	    reliability::reliable,
	    &error));
	*profile_rejection->mutable_authoritative_profile() = *profile;
	assert(encode(
	    incomplete_profile_rejection,
	    reliability::reliable,
	    &error));

	auto valid_environment = environment();
	assert(is_valid_environment_state(valid_environment));
	assert(std::abs(project_time_of_day_hours(
	    23.5,
	    600.0F,
	    std::chrono::seconds(126)) - 20.5) < 0.000001);
	assert(std::abs(project_world_time_seconds(
	    23.5 * seconds_per_hour,
	    600.0F,
	    std::chrono::seconds(126)) - 160'200.0) < 0.000001);
	assert(std::abs(next_world_time_at_hour(20.5 * seconds_per_hour, 6.25)
	    - 108'900.0) < 0.000001);
	assert(circular_time_distance_hours(23.9, 0.1) < 0.21);
	protocol::Envelope environment_update;
	*environment_update.mutable_server_environment_updated()->mutable_state() =
	    valid_environment;
	assert(encode(environment_update, reliability::reliable, &error));
	valid_environment.set_time_scale(maximum_time_scale + 1.0F);
	assert(!is_valid_environment_state(valid_environment));
	*environment_update.mutable_server_environment_updated()->mutable_state() =
	    valid_environment;
	assert(!encode(environment_update, reliability::reliable, &error));
	valid_environment = environment();
	valid_environment.set_world_time_seconds(0.0);
	assert(!is_valid_environment_state(valid_environment));
	valid_environment = environment();
	valid_environment.set_world_time_seconds(-1.0);
	assert(!is_valid_environment_state(valid_environment));

	assert(kcd2o_version == "0.1.5");
	auto unknown_address_library = *runtime;
	unknown_address_library.set_address_library_sha256(std::string(64, '0'));
	assert(is_valid_address_library_identity(unknown_address_library));
	assert(!is_supported_address_library_identity(unknown_address_library));

	return 0;
}
