#include "account/account_api.hpp"
#include "account/account_crypto.hpp"
#include "account/account_store.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <dpapi.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
	[[noreturn]] void fail(const char *expression, int line)
	{
		std::cerr << "check failed at line " << line << ": " << expression
		          << '\n';
		std::_Exit(1);
	}

#define CHECK(expression)                  \
	do                                     \
	{                                      \
		if (!(expression))                 \
		{                                  \
			fail(#expression, __LINE__);   \
		}                                  \
	} while (false)

	struct temporary_directory
	{
		temporary_directory()
		{
			path = std::filesystem::temp_directory_path()
			    / ("kcd2o-account-tests-"
			        + std::to_string(GetCurrentProcessId()) + "-"
			        + std::to_string(GetTickCount64()));
			std::filesystem::create_directories(path);
		}

		~temporary_directory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}

		std::filesystem::path path;
	};

	void write_legacy_store(
	    const std::filesystem::path &path,
	    const kcd2o::account::account_record &record)
	{
		using namespace kcd2o::account;
		const auto plaintext = nlohmann::json{
		    {"version", 1},
		    {"consent", "accepted"},
		    {"accountId", record.account_id},
		    {"credentialId", record.credential_id},
		    {"privateKey", base64url_encode(record.private_key_blob)},
		    {"recoveryCode", record.recovery_code}}.dump();
		constexpr std::string_view entropy_text =
		    "KCD2Online autonomous account store v1";
		DATA_BLOB input{
		    static_cast<DWORD>(plaintext.size()),
		    reinterpret_cast<BYTE *>(const_cast<char *>(plaintext.data()))};
		DATA_BLOB entropy{
		    static_cast<DWORD>(entropy_text.size()),
		    reinterpret_cast<BYTE *>(const_cast<char *>(entropy_text.data()))};
		DATA_BLOB encrypted{};
		CHECK(CryptProtectData(
		    &input, L"KCD2Online account credentials", &entropy, nullptr, nullptr,
		    CRYPTPROTECT_UI_FORBIDDEN, &encrypted));
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.write(
		    reinterpret_cast<const char *>(encrypted.pbData), encrypted.cbData);
		LocalFree(encrypted.pbData);
		CHECK(static_cast<bool>(output));
		output.close();
	}
}

int main()
{
	std::set_terminate(
	    []
	    {
		    if (const auto exception = std::current_exception())
		    {
			    try
			    {
				    std::rethrow_exception(exception);
			    }
			    catch (const std::exception &error)
			    {
				    std::cerr << "unhandled exception: " << error.what() << '\n';
			    }
		    }
		    std::_Exit(1);
	    });

	using namespace kcd2o::account;
	const std::array sample{
	    std::byte{0x00}, std::byte{0x01}, std::byte{0x7F}, std::byte{0x80},
	    std::byte{0xFF}};
	const auto encoded = base64url_encode(sample);
	CHECK(base64url_decode(encoded) == std::vector<std::byte>(sample.begin(), sample.end()));

	const auto credential = generate_credential();
	CHECK(!credential.private_key_blob.empty());
	const auto spki = base64url_decode(credential.public_key_spki);
	CHECK(spki.size() == 91);
	const auto payload = base64url_encode(sample);
	CHECK(base64url_decode(sign_payload(credential.private_key_blob, payload)).size() == 64);
	CHECK(base64url_decode(derive_device_evidence()).size() == 32);
	CHECK(derive_device_evidence() == derive_device_evidence());

	temporary_directory directory;
	const auto path = directory.path / "account.bin";
	account_record record;
	record.consent = consent_choice::accepted;
	record.account_id = "account-id";
	record.credential_id = "credential-id";
	record.private_key_blob = credential.private_key_blob;
	record.recovery_code = "k2r_recovery-secret";
	record.username = "henry";
	record.display_name = "Henry";
	record.locale = "de";
	{
		account_store store(path);
		CHECK(store.value().consent == consent_choice::undecided);
		store.replace(record);
	}
	{
		std::ifstream input(path, std::ios::binary);
		const std::string encrypted{
		    std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		CHECK(!encrypted.empty());
		CHECK(encrypted.find("account-id") == std::string::npos);
		CHECK(encrypted.find("recovery-secret") == std::string::npos);
	}
	{
		account_store restored(path);
		CHECK(restored.value().has_identity());
		CHECK(restored.value().account_id == record.account_id);
		CHECK(restored.value().credential_id == record.credential_id);
		CHECK(restored.value().private_key_blob == record.private_key_blob);
		CHECK(restored.value().recovery_code == record.recovery_code);
		CHECK(restored.value().username == record.username);
		CHECK(restored.value().display_name == record.display_name);
		CHECK(restored.value().locale == record.locale);
		auto declined = restored.value();
		declined.consent = consent_choice::declined;
		restored.replace(std::move(declined));
	}
	{
		account_store restored(path);
		CHECK(restored.value().consent == consent_choice::declined);
		CHECK(restored.value().has_identity());
	}
	{
		const auto legacy_path = directory.path / "legacy-account.bin";
		auto legacy = record;
		legacy.username.clear();
		legacy.display_name.clear();
		legacy.locale = "en";
		write_legacy_store(legacy_path, legacy);
		const auto before_size = std::filesystem::file_size(legacy_path);
		account_store migrated(legacy_path);
		CHECK(migrated.value().has_identity());
		CHECK(migrated.value().username.empty());
		CHECK(migrated.value().display_name.empty());
		CHECK(migrated.value().locale == "en");
		CHECK(std::filesystem::file_size(legacy_path) > before_size);
		account_store reopened(legacy_path);
		CHECK(reopened.value().account_id == legacy.account_id);
	}

	if (const auto *service_url = std::getenv("KCD2ONLINE_TEST_SERVICE_URL"))
	{
		auto live_credential = generate_credential();
		std::array<std::byte, 32> live_device_evidence{};
		CHECK(BCryptGenRandom(
		    nullptr,
		    reinterpret_cast<PUCHAR>(live_device_evidence.data()),
		    static_cast<ULONG>(live_device_evidence.size()),
		    BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0);
		const auto registered = account_api(service_url).register_account(
		    live_credential.public_key_spki,
		    base64url_encode(live_device_evidence),
		    live_credential.private_key_blob);
		CHECK(!registered.account_id.empty());
		CHECK(!registered.credential_id.empty());
		CHECK(!registered.recovery_code.empty());
		account_record live_account;
		live_account.consent = consent_choice::accepted;
		live_account.account_id = registered.account_id;
		live_account.credential_id = registered.credential_id;
		live_account.private_key_blob = std::move(live_credential.private_key_blob);
		live_account.recovery_code = registered.recovery_code;
		const auto login = account_api(service_url).login(live_account, "development");
		CHECK(!login.access_token.empty());
		CHECK(login.expires_at_unix_seconds > 0);
		const auto initial_profile = account_api(service_url).get_profile(live_account);
		CHECK(initial_profile.account_id == live_account.account_id);
		const auto unique_username = "account-" + live_account.account_id.substr(0, 8);
		const auto updated_profile = account_api(service_url).update_profile(
		    live_account, unique_username, "Account Test", "en");
		CHECK(updated_profile.username == unique_username);
		CHECK(updated_profile.display_name == "Account Test");
	}

	return 0;
}
