#pragma once

#include "account/account_api.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kcd2o::account
{
	struct directory_snapshot
	{
		std::vector<public_server> servers;
		bool loading{};
		std::string error;
		std::uint64_t revision{};
	};

	class server_directory
	{
	public:
		server_directory();
		~server_directory();
		[[nodiscard]] directory_snapshot snapshot() const;
		void refresh(std::string service_url);
		[[nodiscard]] bool favorite(std::string_view server_id) const;
		void set_favorite(std::string server_id, bool value);
		[[nodiscard]] std::optional<std::string> password(std::string_view server_id) const;
		void set_password(std::string server_id, std::string password);

	private:
		void load();
		void save() const;
		mutable std::mutex m_mutex;
		std::filesystem::path m_path;
		std::jthread m_worker;
		std::vector<public_server> m_servers;
		std::unordered_set<std::string> m_favorites;
		std::unordered_map<std::string, std::string> m_passwords;
		bool m_loading{};
		std::string m_error;
		std::uint64_t m_revision{1};
	};

	[[nodiscard]] server_directory &directory();
}
