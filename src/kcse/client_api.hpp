#pragma once

#include "generated/kcd2o_version.hpp"

#include <cstddef>
#include <cstdint>

namespace kcd2o::kcse
{
	inline constexpr wchar_t client_module_name[] = L"KCD2OnlineKCSEClient.dll";
	inline constexpr char client_query_export[]   = "KCD2Online_QueryClient";

	inline constexpr std::size_t short_text_capacity = 64;
	inline constexpr std::size_t text_capacity       = 256;

	struct fixed_string
	{
		char value[short_text_capacity]{};
	};

	struct connect_request
	{
		std::uint32_t struct_size{sizeof(connect_request)};
		char address[short_text_capacity]{};
		char display_name[short_text_capacity]{};
		char password[text_capacity]{};
		char content_hash[short_text_capacity]{};
		char claim_code[short_text_capacity]{};
		char server_id[short_text_capacity]{};
		char account_service_url[text_capacity]{};
	};

	struct runtime_status
	{
		std::uint32_t struct_size{sizeof(runtime_status)};
		std::uint32_t available{};
		std::uint32_t joinable{};
		std::uint32_t kcse_version{};
		std::uint32_t game_version{};
		std::uint32_t release_index{};
		std::uint64_t epoch{};
		std::uint64_t capabilities{};
		char address_library[short_text_capacity]{};
		char level_id[short_text_capacity]{};
		char diagnostic[text_capacity]{};
	};

	struct client_status_view
	{
		std::uint32_t struct_size{sizeof(client_status_view)};
		std::uint32_t state{};
		std::uint64_t local_player_id{};
		std::int32_t ping_ms{-1};
		float packet_loss_percent{};
		std::uint32_t game_queue_size{};
		char server_name[short_text_capacity]{};
		char server_id[short_text_capacity]{};
		char session_id[short_text_capacity]{};
		char level_id[short_text_capacity]{};
		char error[text_capacity]{};
		char avatar_archetype_id[short_text_capacity]{};
		char default_avatar_archetype_id[short_text_capacity]{};
		std::uint32_t sleeping{};
		std::uint32_t sleeping_players{};
		std::uint32_t sleeping_players_required{1};
		std::uint32_t dead{};
		std::uint32_t respawn_pending{};
		std::uint32_t voice_recording{};
		std::uint32_t voice_speaking{};
		float voice_level{};
		std::uint32_t voice_range{};
		std::uint32_t native_keybinds{};
		std::uint32_t chat_action_generation{};
		std::uint32_t emote_action_held{};
		std::uint32_t network_role{};
		char effective_permissions[text_capacity]{};
		char error_code[short_text_capacity]{};
		char restriction_scope[short_text_capacity]{};
		char restriction_kind[short_text_capacity]{};
		char restriction_reason[text_capacity]{};
		std::uint64_t restriction_expires_at_unix_ms{};
		char restriction_reference_id[short_text_capacity]{};
		char support_url[text_capacity]{};
		std::uint32_t staff_action_generation{};
		std::uint32_t player_hub_action_generation{};
		std::uint32_t social_action_generation{};
		std::uint32_t environment_available{};
		double time_of_day_hours{};
		float time_scale{};
		std::uint32_t weather_id{};
	};

	struct remote_player_view
	{
		std::uint64_t player_id{};
		std::uint32_t connected{};
		std::uint32_t movement_mode{};
		char display_name[short_text_capacity]{};
		char persistent_id[short_text_capacity]{};
		std::uint32_t network_role{};
	};

	struct chat_entry_view
	{
		std::uint64_t player_id{};
		std::uint64_t server_time_ms{};
		std::uint32_t channel{};
		char display_name[short_text_capacity]{};
		char text[text_capacity]{};
		std::uint32_t network_role{};
	};

	struct client_api
	{
		std::uint32_t struct_size{};
		std::uint32_t version_major{};
		std::uint32_t version_minor{};
		std::uint32_t version_patch{};
		std::uint32_t(__cdecl *get_runtime_status)(runtime_status *result) noexcept {};
		std::uint32_t(__cdecl *connect)(const connect_request *request) noexcept {};
		void(__cdecl *disconnect)() noexcept {};
		std::uint32_t(__cdecl *send_chat)(const char *text) noexcept {};
		std::uint32_t(__cdecl *play_emote)(std::uint32_t kind) noexcept {};
		std::uint32_t(__cdecl *select_avatar)(const char *archetype_id) noexcept {};
		std::uint32_t(__cdecl *attempt_sleep)() noexcept {};
		std::uint32_t(__cdecl *request_respawn)() noexcept {};
		std::uint32_t(__cdecl *get_status)(client_status_view *result) noexcept {};
		std::uint32_t(__cdecl *copy_players)(remote_player_view *output, std::uint32_t capacity) noexcept {};
		std::uint32_t(__cdecl *copy_chat)(chat_entry_view *output, std::uint32_t capacity) noexcept {};
		std::uint32_t(__cdecl *copy_avatar_archetypes)(fixed_string *output, std::uint32_t capacity) noexcept {};
		void(__cdecl *set_diagnostic_logging)(std::uint32_t enabled) noexcept {};
		std::uint32_t status_view_size{sizeof(client_status_view)};
		std::uint32_t remote_player_view_size{sizeof(remote_player_view)};
		std::uint32_t(__cdecl *set_player_voice_volume)(std::uint64_t player_id, float volume) noexcept {};
	};

	using query_client = const client_api *(__cdecl *)(std::uint32_t requested_version_major, std::uint32_t requested_version_minor, std::uint32_t requested_version_patch) noexcept;

	[[nodiscard]] constexpr bool compatible(const client_api *api) noexcept
	{
		return api && api->struct_size == sizeof(client_api) && api->status_view_size == sizeof(client_status_view) && api->remote_player_view_size == sizeof(remote_player_view) && api->version_major == kcd2o_version_major && api->version_minor == kcd2o_version_minor && api->version_patch == kcd2o_version_patch && api->get_runtime_status && api->connect && api->disconnect && api->send_chat && api->play_emote && api->select_avatar && api->get_status && api->attempt_sleep && api->request_respawn && api->copy_players && api->copy_chat && api->copy_avatar_archetypes && api->set_diagnostic_logging && api->set_player_voice_volume;
	}
} // namespace kcd2o::kcse
