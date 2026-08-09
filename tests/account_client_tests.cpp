#include "account/account_api.hpp"
#include "account/account_crypto.hpp"
#include "account/account_store.hpp"

#include <Windows.h>

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
		auto declined = restored.value();
		declined.consent = consent_choice::declined;
		restored.replace(std::move(declined));
	}
	{
		account_store restored(path);
		CHECK(restored.value().consent == consent_choice::declined);
		CHECK(restored.value().has_identity());
	}

	if (const auto *service_url = std::getenv("KCD2ONLINE_TEST_SERVICE_URL"))
	{
		auto live_credential = generate_credential();
		const auto registered = account_api(service_url).register_account(
		    live_credential.public_key_spki,
		    derive_device_evidence(),
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
	}

	return 0;
}
