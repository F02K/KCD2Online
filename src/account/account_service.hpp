#pragma once

#include "account/account_store.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace kcd2o::account
{
	enum class account_state
	{
		undecided,
		declined,
		registering,
		ready,
		error,
	};

	struct account_status
	{
		account_state state{account_state::undecided};
		std::string account_id;
		std::string error_key;
		std::string error_detail;
		std::string username;
		std::string display_name;
		std::string locale{"en"};
		bool busy{};
		std::uint64_t revision{};

		[[nodiscard]] bool multiplayer_enabled() const noexcept
		{
			return state == account_state::ready;
		}
	};

	class account_service
	{
	public:
		account_service();
		~account_service();
		account_service(const account_service &) = delete;
		account_service &operator=(const account_service &) = delete;

		[[nodiscard]] account_status status() const;
		void accept(std::string service_url);
		void decline();
		void ensure_profile(std::string service_url);
		void refresh_profile(std::string service_url);
		void update_profile(
		    std::string service_url,
		    std::string username,
		    std::string display_name,
		    std::string locale);

	private:
		void set_error(std::string key, std::string detail);
		void bump_revision() noexcept;

		mutable std::mutex m_mutex;
		std::unique_ptr<account_store> m_store;
		account_state m_state{account_state::undecided};
		std::string m_account_id;
		std::string m_error_key;
		std::string m_error_detail;
		std::string m_username;
		std::string m_display_name;
		std::string m_locale{"en"};
		bool m_busy{};
		bool m_profile_refresh_attempted{};
		std::uint64_t m_revision{1};
		std::jthread m_worker;
	};

	[[nodiscard]] account_service &service();
}
