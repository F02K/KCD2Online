#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace kcd2o::server
{
	struct dashboard_config
	{
		bool enabled{};
		std::string bind_address{"127.0.0.1"};
		std::uint16_t port{8080};
		bool allow_remote{};
		bool require_token{true};
		std::filesystem::path token_file{"dashboard-token.txt"};
		std::uint32_t refresh_interval_ms{1000};
		std::uint32_t max_requests_per_minute{240};
	};

	[[nodiscard]] dashboard_config load_dashboard_config(
	    const std::filesystem::path &path);
	void validate_dashboard_config(const dashboard_config &config);
	[[nodiscard]] bool is_loopback_address(std::string_view address);
}
