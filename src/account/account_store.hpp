#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kcd2o::account
{
	enum class consent_choice
	{
		undecided,
		accepted,
		declined,
	};

	struct account_record
	{
		consent_choice consent{consent_choice::undecided};
		std::string account_id;
		std::string credential_id;
		std::vector<std::byte> private_key_blob;
		std::string recovery_code;
		std::string username;
		std::string display_name;
		std::string locale{"en"};

		[[nodiscard]] bool has_identity() const noexcept;
	};

	class account_store
	{
	public:
		explicit account_store(std::filesystem::path path = default_path());

		[[nodiscard]] const account_record &value() const noexcept;
		void replace(account_record value);
		[[nodiscard]] const std::filesystem::path &path() const noexcept;

		[[nodiscard]] static std::filesystem::path default_path();
		[[nodiscard]] static std::filesystem::path save_data_export(
		    std::string_view account_id,
		    std::string_view json);

	private:
		void load();
		void save(const account_record &value) const;

		std::filesystem::path m_path;
		account_record m_value;
	};
}
