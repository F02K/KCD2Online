#include "account/account_api.hpp"

#include "account/account_crypto.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kcd2o::account
{
	namespace
	{
		struct internet_deleter
		{
			void operator()(void *handle) const noexcept
			{
				if (handle)
					WinHttpCloseHandle(handle);
			}
		};

		using internet_handle = std::unique_ptr<void, internet_deleter>;

		std::wstring wide(std::string_view value)
		{
			if (value.empty())
				return {};
			const auto length = MultiByteToWideChar(
			    CP_UTF8,
			    MB_ERR_INVALID_CHARS,
			    value.data(),
			    static_cast<int>(value.size()),
			    nullptr,
			    0);
			if (length <= 0)
				throw std::invalid_argument("Service URL is not valid UTF-8");
			std::wstring result(static_cast<std::size_t>(length), L'\0');
			MultiByteToWideChar(
			    CP_UTF8,
			    MB_ERR_INVALID_CHARS,
			    value.data(),
			    static_cast<int>(value.size()),
			    result.data(),
			    length);
			return result;
		}

		struct parsed_url
		{
			std::wstring host;
			std::wstring base_path;
			INTERNET_PORT port{};
			bool secure{};
		};

		parsed_url parse_url(std::string_view text)
		{
			auto encoded = wide(text);
			URL_COMPONENTS components{};
			components.dwStructSize = sizeof(components);
			components.dwSchemeLength = static_cast<DWORD>(-1);
			components.dwHostNameLength = static_cast<DWORD>(-1);
			components.dwUrlPathLength = static_cast<DWORD>(-1);
			components.dwExtraInfoLength = static_cast<DWORD>(-1);
			if (!WinHttpCrackUrl(encoded.c_str(), 0, 0, &components))
				throw std::invalid_argument("KCD2Online service URL is invalid");
			parsed_url result;
			result.host.assign(components.lpszHostName, components.dwHostNameLength);
			result.base_path.assign(components.lpszUrlPath, components.dwUrlPathLength);
			result.port = components.nPort;
			result.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
			if (components.dwExtraInfoLength != 0 || result.host.empty()
			    || (components.nScheme != INTERNET_SCHEME_HTTP && !result.secure))
				throw std::invalid_argument("KCD2Online service URL is invalid");
			std::wstring lower_host = result.host;
			std::ranges::transform(
			    lower_host, lower_host.begin(), [](wchar_t character)
			    { return static_cast<wchar_t>(::towlower(character)); });
			if (!result.secure && lower_host != L"127.0.0.1"
			    && lower_host != L"localhost" && lower_host != L"::1"
			    && lower_host != L"[::1]")
				throw std::invalid_argument(
				    "Plain HTTP is allowed only for a local KCD2Online service");
			while (result.base_path.size() > 1 && result.base_path.ends_with(L'/'))
				result.base_path.pop_back();
			if (result.base_path == L"/")
				result.base_path.clear();
			return result;
		}

		std::string required_string(
		    const nlohmann::json &json,
		    const char *name,
		    std::size_t maximum = 8192)
		{
			const auto result = json.at(name).get<std::string>();
			if (result.empty() || result.size() > maximum)
				throw std::runtime_error("KCD2Online service returned invalid data");
			return result;
		}
	}

	account_api::account_api(std::string base_url) : m_base_url(std::move(base_url))
	{
		(void)parse_url(m_base_url);
	}

	registration_result account_api::register_account(
	    std::string_view public_key_spki,
	    std::string_view device_evidence,
	    std::span<const std::byte> private_key_blob) const
	{
		const nlohmann::json challenge_request{
		    {"credentialPublicKey", public_key_spki},
		    {"deviceEvidence", device_evidence},
		    {"credentialLabel", "windows-client"}};
		const auto challenge_response = nlohmann::json::parse(post_json(
		    "/v1/auth/registration/challenge", challenge_request.dump()));
		const auto request_id = required_string(challenge_response, "requestId", 64);
		const auto signing_payload =
		    required_string(challenge_response, "signingPayload");
		const nlohmann::json completion_request{
		    {"requestId", request_id},
		    {"signature", sign_payload(private_key_blob, signing_payload)}};
		const auto completion = nlohmann::json::parse(post_json(
		    "/v1/auth/registration/complete", completion_request.dump()));
		return {
		    required_string(completion, "accountId", 64),
		    required_string(completion, "credentialId", 64),
		    required_string(completion, "recoveryCode", 128)};
	}

	login_result account_api::login(
	    const account_record &account,
	    std::string_view audience) const
	{
		if (!account.has_identity() || audience.empty() || audience.size() > 64)
			throw std::invalid_argument("Account login input is invalid");
		const nlohmann::json challenge_request{
		    {"credentialId", account.credential_id}, {"audience", audience}};
		const auto challenge_response = nlohmann::json::parse(post_json(
		    "/v1/auth/login/challenge", challenge_request.dump()));
		const auto request_id = required_string(challenge_response, "requestId", 64);
		const auto signing_payload =
		    required_string(challenge_response, "signingPayload");
		const nlohmann::json completion_request{
		    {"requestId", request_id},
		    {"signature", sign_payload(account.private_key_blob, signing_payload)}};
		const auto completion = nlohmann::json::parse(post_json(
		    "/v1/auth/login/complete", completion_request.dump()));
		return {
		    required_string(completion, "accessToken"),
		    completion.at("expiresAtUnixSeconds").get<long long>()};
	}

	std::vector<public_server> account_api::list_servers() const
	{
		const auto json = nlohmann::json::parse(get_json("/v1/servers"));
		if (!json.is_array() || json.size() > 2048)
			throw std::runtime_error("KCD2Online server list is invalid");
		std::vector<public_server> result;
		for (const auto &entry : json)
		{
			public_server server{
			    required_string(entry, "id", 64),
			    required_string(entry, "name", 128),
			    required_string(entry, "address", 256),
			    required_string(entry, "version", 64),
			    entry.at("playerCount").get<std::uint64_t>(),
			    entry.at("maxPlayers").get<std::uint64_t>(),
			    entry.value("passwordProtected", false),
			    required_string(entry, "levelId", 128),
			    entry.at("lastSeenAtUnixMs").get<long long>()};
			if (server.max_players < 1
			    || server.player_count > server.max_players
			    || std::ranges::any_of(result, [&](const public_server &existing)
			       { return existing.id == server.id; }))
				throw std::runtime_error("KCD2Online server list contains invalid data");
			result.push_back(std::move(server));
		}
		return result;
	}

	std::string account_api::post_json(
	    std::string_view path,
	    std::string_view body) const
	{
		if (!path.starts_with('/') || body.size() > 64 * 1024)
			throw std::invalid_argument("KCD2Online request is invalid");
		const auto endpoint = parse_url(m_base_url);
		auto session = internet_handle(WinHttpOpen(
		    L"KCD2Online/0.1",
		    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		    WINHTTP_NO_PROXY_NAME,
		    WINHTTP_NO_PROXY_BYPASS,
		    0));
		if (!session)
			throw std::runtime_error("Could not initialize KCD2Online networking");
		WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 10000);
		auto connection = internet_handle(
		    WinHttpConnect(session.get(), endpoint.host.c_str(), endpoint.port, 0));
		if (!connection)
			throw std::runtime_error("Could not connect to the KCD2Online service");
		const auto request_path = endpoint.base_path + wide(path);
		auto request = internet_handle(WinHttpOpenRequest(
		    connection.get(),
		    L"POST",
		    request_path.c_str(),
		    nullptr,
		    WINHTTP_NO_REFERER,
		    WINHTTP_DEFAULT_ACCEPT_TYPES,
		    endpoint.secure ? WINHTTP_FLAG_SECURE : 0));
		if (!request)
			throw std::runtime_error("Could not create a KCD2Online request");
		constexpr wchar_t headers[] = L"Content-Type: application/json\r\nAccept: application/json\r\n";
		if (!WinHttpSendRequest(
		        request.get(),
		        headers,
		        static_cast<DWORD>(-1),
		        const_cast<char *>(body.data()),
		        static_cast<DWORD>(body.size()),
		        static_cast<DWORD>(body.size()),
		        0)
		    || !WinHttpReceiveResponse(request.get(), nullptr))
			throw std::runtime_error("KCD2Online service is unavailable");

		DWORD status{};
		DWORD status_size = sizeof(status);
		if (!WinHttpQueryHeaders(
		        request.get(),
		        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		        WINHTTP_HEADER_NAME_BY_INDEX,
		        &status,
		        &status_size,
		        WINHTTP_NO_HEADER_INDEX))
			throw std::runtime_error("KCD2Online service returned no status");

		std::string response;
		for (;;)
		{
			DWORD available{};
			if (!WinHttpQueryDataAvailable(request.get(), &available))
				throw std::runtime_error("Could not read KCD2Online response");
			if (available == 0)
				break;
			if (response.size() + available > 128 * 1024)
				throw std::runtime_error("KCD2Online response is too large");
			const auto offset = response.size();
			response.resize(offset + available);
			DWORD read{};
			if (!WinHttpReadData(
			        request.get(), response.data() + offset, available, &read))
				throw std::runtime_error("Could not read KCD2Online response");
			response.resize(offset + read);
		}
		if (status < 200 || status >= 300)
		{
			if (status == 409)
				throw std::runtime_error("KCD2Online account already exists for this device");
			throw std::runtime_error("KCD2Online service rejected the account request");
		}
		if (response.empty())
			throw std::runtime_error("KCD2Online service returned an empty response");
		return response;
	}

	std::string account_api::get_json(std::string_view path) const
	{
		if (!path.starts_with('/'))
			throw std::invalid_argument("KCD2Online request is invalid");
		const auto endpoint = parse_url(m_base_url);
		auto session = internet_handle(WinHttpOpen(L"KCD2Online/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
		if (!session) throw std::runtime_error("Could not initialize KCD2Online networking");
		WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 10000);
		auto connection = internet_handle(WinHttpConnect(session.get(), endpoint.host.c_str(), endpoint.port, 0));
		const auto request_path = endpoint.base_path + wide(path);
		auto request = internet_handle(WinHttpOpenRequest(connection.get(), L"GET", request_path.c_str(), nullptr, nullptr, WINHTTP_DEFAULT_ACCEPT_TYPES, endpoint.secure ? WINHTTP_FLAG_SECURE : 0));
		if (!request || !WinHttpSendRequest(request.get(), L"Accept: application/json\r\n", static_cast<DWORD>(-1), nullptr, 0, 0, 0)
		    || !WinHttpReceiveResponse(request.get(), nullptr))
			throw std::runtime_error("KCD2Online service is unavailable");
		DWORD status{}, status_size = sizeof(status);
		WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &status_size, nullptr);
		std::string response;
		for (;;) { DWORD available{}; if (!WinHttpQueryDataAvailable(request.get(), &available)) throw std::runtime_error("Could not read KCD2Online response"); if (!available) break; if (response.size() + available > 512 * 1024) throw std::runtime_error("KCD2Online response is too large"); const auto offset = response.size(); response.resize(offset + available); DWORD read{}; if (!WinHttpReadData(request.get(), response.data() + offset, available, &read)) throw std::runtime_error("Could not read KCD2Online response"); response.resize(offset + read); }
		if (status < 200 || status >= 300) throw std::runtime_error("KCD2Online service rejected the server-list request");
		return response;
	}
}
