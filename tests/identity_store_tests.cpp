#include "multiplayer/identity_store.hpp"

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <exception>
#include <string>

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
			    / ("kcd2o-identity-tests-"
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
				    std::cerr << "unhandled exception: " << error.what()
				              << '\n';
			    }
		    }
		    std::_Exit(1);
	    });
	using kcd2o::identity_store;

	temporary_directory directory;
	const auto path = directory.path / "identities.bin";
	{
		identity_store store(path);
		CHECK(!store.token_for("server-a"));
		store.store("server-a", "secret-token-a");
		store.store("server-b", "secret-token-b");
		CHECK(store.token_for("server-a") == "secret-token-a");
	}

	{
		std::ifstream encrypted_input(path, std::ios::binary);
		const std::string encrypted{
		    std::istreambuf_iterator<char>(encrypted_input),
		    std::istreambuf_iterator<char>()};
		CHECK(!encrypted.empty());
		CHECK(encrypted.find("secret-token-a") == std::string::npos);
		CHECK(encrypted.find("server-a") == std::string::npos);
	}

	{
		identity_store restored(path);
		CHECK(restored.token_for("server-a") == "secret-token-a");
		CHECK(restored.token_for("server-b") == "secret-token-b");
		restored.erase("server-a");
	}
	{
		identity_store restored(path);
		CHECK(!restored.token_for("server-a"));
		CHECK(restored.token_for("server-b") == "secret-token-b");
		restored.erase("server-b");
	}
	{
		identity_store restored(path);
		CHECK(!restored.token_for("server-a"));
		CHECK(!restored.token_for("server-b"));
	}

	{
		std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
		corrupt << "not a DPAPI blob";
	}
	bool rejected_corrupt_store = false;
	try
	{
		identity_store corrupt(path);
		(void)corrupt;
	}
	catch (const std::exception &)
	{
		rejected_corrupt_store = true;
	}
	CHECK(rejected_corrupt_store);

	return 0;
}
