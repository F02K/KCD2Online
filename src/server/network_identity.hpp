#pragma once

#include <string>

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
}
