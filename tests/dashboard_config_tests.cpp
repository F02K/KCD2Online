#include "server/dashboard_config.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace
{
	class temporary_directory
	{
	public:
		temporary_directory()
		{
			path = std::filesystem::temp_directory_path()
			    / ("kcd2o-dashboard-tests-"
			       + std::to_string(
			           std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(path);
		}

		~temporary_directory()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}

		std::filesystem::path path;
	};

	void write(const std::filesystem::path &path, std::string_view content)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << content;
		assert(output.good());
	}

	template<typename Function>
	bool throws(Function function)
	{
		try
		{
			function();
		}
		catch (const std::runtime_error &)
		{
			return true;
		}
		return false;
	}
}

int main()
{
	temporary_directory temporary;
	const auto path = temporary.path / "dashboard.toml";
	write(
	    path,
	    "[dashboard]\n"
	    "enabled = true\n"
	    "bind_address = \"127.0.0.1\"\n"
	    "port = 8181\n"
	    "require_token = true\n"
	    "token_file = \"private/token.txt\"\n"
	    "refresh_interval_ms = 750\n"
	    "max_requests_per_minute = 120\n");
	const auto config = kcd2o::server::load_dashboard_config(path);
	assert(config.enabled);
	assert(config.port == 8181);
	assert(config.require_token);
	assert(config.refresh_interval_ms == 750);
	assert(config.max_requests_per_minute == 120);
	assert(config.token_file == temporary.path / "private/token.txt");
	assert(kcd2o::server::is_loopback_address("127.0.0.1"));
	assert(kcd2o::server::is_loopback_address("::1"));
	assert(!kcd2o::server::is_loopback_address("0.0.0.0"));

	write(
	    path,
	    "[dashboard]\n"
	    "enabled = true\n"
	    "bind_address = \"0.0.0.0\"\n"
	    "require_token = true\n");
	assert(throws([&] { (void)kcd2o::server::load_dashboard_config(path); }));

	write(
	    path,
	    "[dashboard]\n"
	    "enabled = true\n"
	    "bind_address = \"0.0.0.0\"\n"
	    "allow_remote = true\n"
	    "require_token = false\n");
	assert(throws([&] { (void)kcd2o::server::load_dashboard_config(path); }));

	write(
	    path,
	    "[dashboard]\n"
	    "refresh_interval_ms = 100\n");
	assert(throws([&] { (void)kcd2o::server::load_dashboard_config(path); }));
}
