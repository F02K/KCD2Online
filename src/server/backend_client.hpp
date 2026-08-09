#pragma once

#include <optional>
#include <filesystem>
#include <cstdint>
#include <string>
#include <string_view>

namespace kcd2o::server
{
	struct heartbeat_data
	{
		std::string name;
		std::string address;
		std::string version;
		std::uint64_t player_count{};
		std::uint64_t max_players{};
		bool password_protected{};
		std::string level_id;
	};

	struct server_credentials
	{
		std::string id;
		std::string api_key;
	};

	class backend_client
	{
	public:
		backend_client(std::string base_url, std::string server_id, std::string api_key);
		[[nodiscard]] server_credentials register_server(
		    const heartbeat_data &data) const;
		[[nodiscard]] std::optional<std::string> introspect(
		    std::string_view access_token,
		    std::string &error) const;
		[[nodiscard]] bool heartbeat(const heartbeat_data &data, std::string &error) const;

	private:
		[[nodiscard]] std::string post(
		    std::string_view path,
		    std::string_view json,
		    bool authenticated = true) const;
		std::string m_base_url;
		std::string m_server_id;
		std::string m_api_key;
	};

	[[nodiscard]] std::optional<server_credentials> load_server_credentials(
	    const std::filesystem::path &path);
	void save_server_credentials(
	    const std::filesystem::path &path,
	    const server_credentials &credentials);
}
