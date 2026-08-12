#pragma once

#include "account/account_store.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kcd2o::account
{
	struct registration_result
	{
		std::string account_id;
		std::string credential_id;
		std::string recovery_code;
	};

	struct login_result
	{
		std::string access_token;
		long long expires_at_unix_seconds{};
	};

	struct account_profile
	{
		std::string account_id;
		std::string username;
		std::string display_name;
		std::string locale{"en"};
		std::string network_role{"user"};
	};

	struct public_server
	{
		std::string id;
		std::string name;
		std::string address;
		std::string version;
		std::uint64_t player_count{};
		std::uint64_t max_players{};
		bool password_protected{};
		std::string level_id;
		long long last_seen_at_unix_ms{};
	};

	class account_api
	{
	public:
		explicit account_api(std::string base_url);

		[[nodiscard]] registration_result register_account(
		    std::string_view public_key_spki,
		    std::string_view device_evidence,
		    std::span<const std::byte> private_key_blob) const;
		[[nodiscard]] registration_result recover_account(
		    std::string_view recovery_code,
		    std::string_view public_key_spki,
		    std::string_view device_evidence,
		    std::span<const std::byte> private_key_blob) const;
		[[nodiscard]] login_result login(
		    const account_record &account,
		    std::string_view audience) const;
		[[nodiscard]] account_profile get_profile(
		    const account_record &account) const;
		[[nodiscard]] account_profile update_profile(
		    const account_record &account,
		    std::string_view username,
		    std::string_view display_name,
		    std::string_view locale) const;
		[[nodiscard]] std::string export_data(const account_record &account) const;
		void delete_account(
		    const account_record &account,
		    std::string_view confirmation_account_id) const;
		[[nodiscard]] std::vector<public_server> list_servers() const;

	private:
		[[nodiscard]] std::string post_json(
		    std::string_view path,
		    std::string_view body) const;
		[[nodiscard]] std::string get_json(std::string_view path) const;
		[[nodiscard]] std::string request_json(
		    std::wstring_view method,
		    std::string_view path,
		    std::string_view body,
		    std::string_view access_token = {}) const;

		std::string m_base_url;
	};
}
