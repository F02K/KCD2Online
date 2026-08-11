#pragma once

#include "kcd2o.pb.h"
#include "generated/kcd2o_version.hpp"
#include "multiplayer/environment.hpp"
#include "multiplayer/runtime_capabilities.hpp"
#include "multiplayer/address_library_manifest.generated.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "npc/catalog.hpp"

namespace kcd2o
{
	constexpr std::uint32_t supported_whgame_timestamp = 0x6A350E20;
	constexpr std::uint64_t supported_whgame_image_size = 0x5B2D000;
	constexpr std::uint32_t supported_kcse_game_version = 0x01050600;
	constexpr std::uint32_t supported_kcse_release_index = 1;
	constexpr std::size_t max_application_message_size = 64 * 1024;
	constexpr std::size_t max_display_name_codepoints = 32;
	constexpr std::size_t max_access_token_size = 8192;
	constexpr std::size_t min_display_name_codepoints = 3;
	constexpr std::size_t max_chat_codepoints = 256;
	constexpr std::size_t max_avatar_equipment_items = 32;
	constexpr std::size_t max_animation_fragment_bytes = 96;
	constexpr std::size_t max_avatar_archetypes = 32;
	constexpr std::size_t profile_stat_count = 10;
	constexpr std::size_t profile_skill_count = 35;
	constexpr std::size_t max_profile_inventory_items = 512;
	constexpr std::size_t max_world_objects = 4096;
	constexpr std::size_t max_world_object_inventory_items = 512;
	constexpr std::size_t max_world_items = 4096;
	constexpr std::size_t max_npcs_per_message = 256;
	constexpr std::size_t voice_viseme_count = 15;
	constexpr std::size_t max_voice_opus_bytes = 512;
	constexpr std::size_t max_profile_quick_access_slots = 36;
	constexpr std::int64_t max_profile_money = 2'000'000'000;
	constexpr std::uint32_t money_subunits_per_groschen = 10;
	constexpr std::uint32_t max_profile_item_count = 1'000'000;
	inline constexpr std::array<std::string_view, profile_stat_count>
	    canonical_stat_ids{
	        "strength",
	        "agility",
	        "vitality",
	        "speech",
	        "vision",
	        "hearing",
	        "barter",
	        "courage",
	        "storyProgress",
	        "prestige"};
	inline constexpr std::array<std::string_view, profile_skill_count>
	    canonical_skill_ids{
	        "stealth",
	        "horse_riding",
	        "fencing",
	        "bard",
	        "thievery",
	        "pickpocketing_obsolete",
	        "alchemy",
	        "cooking",
	        "craftsmanship",
	        "smithing_obsolete",
	        "fishing",
	        "mining",
	        "first_aid",
	        "drinking",
	        "survival",
	        "defense",
	        "weapon_sword",
	        "heavy_weapons",
	        "weapon_bow_obsolete",
	        "marksmanship",
	        "weapon_shield",
	        "weapon_mace_obsolete",
	        "weapon_dagger",
	        "weapon_large",
	        "weapon_unarmed",
	        "herbalism_obsolete",
	        "scholarship",
	        "tailoring",
	        "armourer",
	        "weaponsmithing",
	        "shoemaking",
	        "gunsmithing",
	        "bowyery",
	        "gambling",
	        "houndmaster"};
	constexpr std::string_view default_avatar_archetype_id =
	    npc::default_soul_id;

	using player_id = std::uint64_t;
	using connection_id = std::uint64_t;

	enum class reliability
	{
		unreliable,
		reliable
	};

	// Transport lanes are independent outbound ordering domains. Keep ordered
	// state changes together; only self-contained, revisioned realtime data and
	// chat are allowed to bypass that stream.
	enum class traffic_lane : std::uint16_t
	{
		player_realtime = 0,
		ordered_state = 1,
		npc_realtime = 2,
		interactive = 3,
		voice_realtime = 4
	};

	constexpr std::size_t traffic_lane_count = 5;

	struct encoded_message
	{
		std::vector<std::byte> bytes;
		reliability delivery{reliability::reliable};
	};

	[[nodiscard]] std::optional<encoded_message> encode(
	    const protocol::Envelope &envelope,
	    reliability delivery,
	    std::string *error = nullptr);
	[[nodiscard]] std::optional<protocol::Envelope> decode(
	    std::span<const std::byte> bytes,
	    std::string *error = nullptr);
	[[nodiscard]] traffic_lane lane_for(
	    const protocol::Envelope &envelope) noexcept;
	[[nodiscard]] bool valid_utf8_with_codepoint_count(
	    std::string_view value,
	    std::size_t minimum,
	    std::size_t maximum);
	[[nodiscard]] bool is_valid_display_name(std::string_view value);
	[[nodiscard]] bool is_valid_chat(std::string_view value);
	[[nodiscard]] bool is_uuid(std::string_view value);
	[[nodiscard]] bool is_valid_avatar_equipment_slot(
	    std::string_view value);
	[[nodiscard]] bool is_valid_profile(const protocol::PlayerProfile &profile);
	[[nodiscard]] bool is_valid_world_object_state(
	    const protocol::WorldObjectState &state,
	    bool require_revision = true);
	[[nodiscard]] bool is_valid_world_item_state(
	    const protocol::WorldItemState &state,
	    bool require_revision = true);
	[[nodiscard]] bool is_valid_npc_kind(protocol::NpcKind kind);
	[[nodiscard]] bool is_valid_npc_observation(
	    const protocol::NpcObservation &observation);
	[[nodiscard]] bool is_valid_npc_state(
	    const protocol::NpcState &state);
	[[nodiscard]] bool is_valid_avatar_descriptor(
	    const protocol::AvatarDescriptor &avatar);
	[[nodiscard]] bool is_valid_avatar_policy(
	    const protocol::AvatarPolicy &policy);
	[[nodiscard]] bool is_valid_address_library_identity(
	    const protocol::ClientRuntimeCapabilities &runtime);
	[[nodiscard]] bool is_supported_address_library_identity(
	    const protocol::ClientRuntimeCapabilities &runtime);
	[[nodiscard]] bool is_finite_transform(const protocol::TransformState &transform);
	[[nodiscard]] bool normalize_rotation(protocol::Quaternion *rotation);
	[[nodiscard]] protocol::MovementMode movement_mode_for(
	    const protocol::TransformState &transform);
}
