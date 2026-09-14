#include "server/dashboard_config.hpp"

#include <toml++/toml.hpp>

#include <fstream>
#include <limits>
#include <stdexcept>

namespace kcd2o::server
{
	namespace
	{
		template<typename Target>
		Target checked_integer(
		    const toml::table &table,
		    std::string_view key,
		    Target fallback)
		{
			const auto raw = table[key].value<std::int64_t>();
			if (!raw)
				return fallback;
			if (*raw < 0
			    || static_cast<std::uint64_t>(*raw)
			        > static_cast<std::uint64_t>(
			            std::numeric_limits<Target>::max()))
			{
				throw std::runtime_error(
				    "dashboard.toml value is out of range: "
				    + std::string(key));
			}
			return static_cast<Target>(*raw);
		}
	}

	bool is_loopback_address(std::string_view address)
	{
		return address == "127.0.0.1" || address == "::1"
		    || address == "localhost";
	}

	dashboard_config load_dashboard_config(const std::filesystem::path &path)
	{
		const auto document = toml::parse_file(path.string());
		const auto *dashboard = document["dashboard"].as_table();
		if (!dashboard)
		{
			throw std::runtime_error(
			    "dashboard.toml is missing the [dashboard] table");
		}

		dashboard_config config;
		config.enabled = (*dashboard)["enabled"].value_or(false);
		config.bind_address =
		    (*dashboard)["bind_address"].value_or(config.bind_address);
		config.port = checked_integer(*dashboard, "port", config.port);
		config.allow_remote = (*dashboard)["allow_remote"].value_or(false);
		config.require_token = (*dashboard)["require_token"].value_or(true);
		config.token_file = (*dashboard)["token_file"].value_or(
		    config.token_file.string());
		config.refresh_interval_ms = checked_integer(
		    *dashboard,
		    "refresh_interval_ms",
		    config.refresh_interval_ms);
		config.max_requests_per_minute = checked_integer(
		    *dashboard,
		    "max_requests_per_minute",
		    config.max_requests_per_minute);
		if (config.token_file.is_relative())
		{
			config.token_file = std::filesystem::absolute(path).parent_path()
			    / config.token_file;
		}
		validate_dashboard_config(config);
		return config;
	}

	void validate_dashboard_config(const dashboard_config &config)
	{
		if (config.bind_address.empty())
			throw std::runtime_error("dashboard bind_address must not be empty");
		if (config.port == 0)
			throw std::runtime_error("dashboard port must be between 1 and 65535");
		if (config.refresh_interval_ms < 250
		    || config.refresh_interval_ms > 60'000)
		{
			throw std::runtime_error(
			    "dashboard refresh_interval_ms must be between 250 and 60000");
		}
		if (config.max_requests_per_minute < 30
		    || config.max_requests_per_minute > 10'000)
		{
			throw std::runtime_error(
			    "dashboard max_requests_per_minute must be between 30 and 10000");
		}
		if (!is_loopback_address(config.bind_address) && !config.allow_remote)
		{
			throw std::runtime_error(
			    "dashboard remote binding requires allow_remote = true");
		}
		if (!is_loopback_address(config.bind_address) && !config.require_token)
		{
			throw std::runtime_error(
			    "dashboard remote binding requires token authentication");
		}
		if (config.require_token && config.token_file.empty())
			throw std::runtime_error("dashboard token_file must not be empty");
	}
}
