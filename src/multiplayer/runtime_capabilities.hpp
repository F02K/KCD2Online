#pragma once

#include <cstdint>

namespace kcd2o
{
	enum runtime_capability : std::uint64_t
	{
		runtime_capability_kcse = 1ULL << 0,
		runtime_capability_game_thread = 1ULL << 1,
		runtime_capability_local_player = 1ULL << 2,
		runtime_capability_transform_read = 1ULL << 3,
		runtime_capability_transform_write = 1ULL << 4,
		runtime_capability_sandbox = 1ULL << 5,
		runtime_capability_entity_isolation = 1ULL << 6,
		runtime_capability_remote_avatar = 1ULL << 7,
		runtime_capability_equipment = 1ULL << 8,
		runtime_capability_profile_capture = 1ULL << 9,
		runtime_capability_profile_apply = 1ULL << 10,
		runtime_capability_profile_qam = 1ULL << 11,
		runtime_capability_transition_gate = 1ULL << 12,
		runtime_capability_address_library_identity = 1ULL << 13,
		runtime_capability_environment = 1ULL << 14,
		runtime_capability_npc_sync = 1ULL << 15,
	};

	constexpr std::uint64_t known_client_runtime_capabilities =
	    (runtime_capability_npc_sync << 1) - 1;

	// Features required for the authoritative multiplayer simulation. Future
	// cosmetic/debug capabilities belong in `known`, not in this mask.
	constexpr std::uint64_t required_client_runtime_capabilities =
	    known_client_runtime_capabilities;

	constexpr std::uint64_t server_supported_runtime_capabilities =
	    known_client_runtime_capabilities;

	[[nodiscard]] constexpr std::uint64_t negotiate_runtime_capabilities(
	    std::uint64_t client) noexcept
	{
		return client & server_supported_runtime_capabilities;
	}
}
