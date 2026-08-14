#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kcd2o::server
{
	struct network_identity
	{
		std::string account_id;
		std::string network_role{"user"};
		std::string display_name;
		bool join_bypass{};
		bool full_permissions{};
		bool whitelisted{};
		bool chat_muted{};
		bool voice_muted{};
	};

	struct authentication_failure
	{
		std::string message;
		std::string error_code;
		std::string restriction_scope;
		std::string restriction_kind;
		std::string restriction_reason;
		std::uint64_t expires_at_unix_ms{};
		std::string reference_id;
		std::string support_url{"https://support.kingdom-online.cc"};
	};

	struct moderation_action
	{
		std::string account_id;
		std::string kind;
		std::string reason;
		std::string actor_account_id;
		std::uint64_t expires_at_unix_ms{};
	};

	struct account_restriction
	{
		std::string account_id;
		bool network_blocked{};
		std::string network_kind;
		std::string network_reason;
		std::uint64_t network_until_unix_ms{};
		bool server_banned{};
		std::string ban_reason;
		std::uint64_t banned_until_unix_ms{};
		bool chat_muted{};
		std::string chat_mute_reason;
		std::uint64_t chat_muted_until_unix_ms{};
		bool voice_muted{};
		std::string voice_mute_reason;
		std::uint64_t voice_muted_until_unix_ms{};
	};
}
