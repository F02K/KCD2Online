#include "account/account_store.hpp"

#include "account/account_crypto.hpp"

#include <Windows.h>
#include <dpapi.h>

#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace kcd2o::account
{
	namespace
	{
		constexpr std::string_view entropy_text =
		    "KCD2Online autonomous account store v1";

		struct secure_text_guard
		{
			std::string &text;
			~secure_text_guard()
			{
				SecureZeroMemory(text.data(), text.size());
			}
		};

		DATA_BLOB blob(std::span<const std::byte> bytes)
		{
			return {
			    static_cast<DWORD>(bytes.size()),
			    reinterpret_cast<BYTE *>(const_cast<std::byte *>(bytes.data()))};
		}

		std::vector<std::byte> protect(std::string_view plaintext)
		{
			auto input = blob({
			    reinterpret_cast<const std::byte *>(plaintext.data()), plaintext.size()});
			auto entropy = blob({
			    reinterpret_cast<const std::byte *>(entropy_text.data()),
			    entropy_text.size()});
			DATA_BLOB output{};
			if (!CryptProtectData(
			        &input,
			        L"KCD2Online account credentials",
			        &entropy,
			        nullptr,
			        nullptr,
			        CRYPTPROTECT_UI_FORBIDDEN,
			        &output))
				throw std::runtime_error("Windows DPAPI could not protect the account");
			std::vector<std::byte> result(
			    reinterpret_cast<std::byte *>(output.pbData),
			    reinterpret_cast<std::byte *>(output.pbData + output.cbData));
			LocalFree(output.pbData);
			return result;
		}

		std::string unprotect(std::span<const std::byte> ciphertext)
		{
			auto input = blob(ciphertext);
			auto entropy = blob({
			    reinterpret_cast<const std::byte *>(entropy_text.data()),
			    entropy_text.size()});
			DATA_BLOB output{};
			if (!CryptUnprotectData(
			        &input,
			        nullptr,
			        &entropy,
			        nullptr,
			        nullptr,
			        CRYPTPROTECT_UI_FORBIDDEN,
			        &output))
				throw std::runtime_error("Windows DPAPI could not decrypt the account");
			std::string result(
			    reinterpret_cast<const char *>(output.pbData), output.cbData);
			SecureZeroMemory(output.pbData, output.cbData);
			LocalFree(output.pbData);
			return result;
		}

		void atomic_write(
		    const std::filesystem::path &target,
		    std::span<const std::byte> bytes)
		{
			std::filesystem::create_directories(target.parent_path());
			const auto temporary = target.wstring() + L".tmp";
			{
				std::ofstream output(
				    std::filesystem::path(temporary),
				    std::ios::binary | std::ios::trunc);
				output.write(
				    reinterpret_cast<const char *>(bytes.data()),
				    static_cast<std::streamsize>(bytes.size()));
				output.flush();
				if (!output)
					throw std::runtime_error("Could not write the account store");
			}
			if (!MoveFileExW(
			        temporary.c_str(),
			        target.c_str(),
			        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				DeleteFileW(temporary.c_str());
				throw std::runtime_error("Could not atomically replace the account store");
			}
		}

		std::string consent_name(consent_choice choice)
		{
			switch (choice)
			{
			case consent_choice::undecided: return "undecided";
			case consent_choice::accepted: return "accepted";
			case consent_choice::declined: return "declined";
			}
			throw std::runtime_error("Invalid account consent value");
		}

		consent_choice parse_consent(std::string_view value)
		{
			if (value == "undecided")
				return consent_choice::undecided;
			if (value == "accepted")
				return consent_choice::accepted;
			if (value == "declined")
				return consent_choice::declined;
			throw std::runtime_error("Account store contains invalid consent");
		}
	}

	bool account_record::has_identity() const noexcept
	{
		return !account_id.empty() && !credential_id.empty()
		    && !private_key_blob.empty() && !recovery_code.empty();
	}

	account_store::account_store(std::filesystem::path path) : m_path(std::move(path))
	{
		load();
	}

	const account_record &account_store::value() const noexcept
	{
		return m_value;
	}

	void account_store::replace(account_record value)
	{
		if ((!value.account_id.empty() || !value.credential_id.empty()
		        || !value.private_key_blob.empty() || !value.recovery_code.empty())
		    && !value.has_identity())
			throw std::invalid_argument("Account identity is incomplete");
		save(value);
		m_value = std::move(value);
	}

	const std::filesystem::path &account_store::path() const noexcept
	{
		return m_path;
	}

	std::filesystem::path account_store::default_path()
	{
		std::wstring buffer(32768, L'\0');
		const auto length = GetEnvironmentVariableW(
		    L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0 || length >= buffer.size())
			throw std::runtime_error("LOCALAPPDATA is unavailable");
		buffer.resize(length);
		return std::filesystem::path(buffer) / "KCD2Online" / "account.bin";
	}

	void account_store::load()
	{
		if (!std::filesystem::exists(m_path))
			return;
		std::ifstream input(m_path, std::ios::binary);
		const std::string encrypted_text{
		    std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		if ((!input && !input.eof()) || encrypted_text.empty())
			throw std::runtime_error("Account store is empty or unreadable");
		input.close();
		auto plaintext = unprotect({
		    reinterpret_cast<const std::byte *>(encrypted_text.data()),
		    encrypted_text.size()});
		secure_text_guard erase_plaintext{plaintext};
		bool migrate{};
		try
		{
			const auto json = nlohmann::json::parse(plaintext);
			const auto version = json.at("version").get<int>();
			if (version != 1 && version != 2)
				throw std::runtime_error("Unsupported account store version");
			migrate = version == 1;
			account_record loaded;
			loaded.consent = parse_consent(json.at("consent").get<std::string>());
			loaded.account_id = json.value("accountId", "");
			loaded.credential_id = json.value("credentialId", "");
			loaded.recovery_code = json.value("recoveryCode", "");
			if (version >= 2)
			{
				loaded.username = json.value("username", "");
				loaded.display_name = json.value("displayName", "");
				loaded.locale = json.value("locale", "en");
				if (loaded.locale.empty())
					loaded.locale = "en";
			}
			const auto private_key = json.value("privateKey", "");
			if (!private_key.empty())
				loaded.private_key_blob = base64url_decode(private_key);
			if ((!loaded.account_id.empty() || !loaded.credential_id.empty()
			        || !loaded.private_key_blob.empty() || !loaded.recovery_code.empty())
			    && !loaded.has_identity())
				throw std::runtime_error("Account store identity is incomplete");
			m_value = std::move(loaded);
		}
		catch (const nlohmann::json::exception &exception)
		{
			throw std::runtime_error(
			    std::string("Account store is malformed: ") + exception.what());
		}
		if (migrate)
			save(m_value);
	}

	void account_store::save(const account_record &value) const
	{
		nlohmann::json json{
		    {"version", 2},
		    {"consent", consent_name(value.consent)},
		    {"accountId", value.account_id},
		    {"credentialId", value.credential_id},
		    {"privateKey", base64url_encode(value.private_key_blob)},
		    {"recoveryCode", value.recovery_code},
		    {"username", value.username},
		    {"displayName", value.display_name},
		    {"locale", value.locale.empty() ? "en" : value.locale}};
		auto plaintext = json.dump();
		secure_text_guard erase_plaintext{plaintext};
		const auto encrypted = protect(plaintext);
		atomic_write(m_path, encrypted);
	}
}
