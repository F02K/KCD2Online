#include "server/backend_client.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <memory>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace kcd2o::server
{
	namespace
	{
		struct closer { void operator()(void *value) const noexcept { if (value) WinHttpCloseHandle(value); } };
		using handle = std::unique_ptr<void, closer>;
		std::wstring wide(std::string_view value)
		{
			const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
			if (size <= 0) throw std::invalid_argument("backend URL is invalid UTF-8");
			std::wstring result(static_cast<std::size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
			return result;
		}
		struct endpoint { std::wstring host; std::wstring path; INTERNET_PORT port{}; bool secure{}; };
		endpoint parse(std::string_view url)
		{
			auto text = wide(url);
			URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
			parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
			if (!WinHttpCrackUrl(text.c_str(), 0, 0, &parts) || parts.dwExtraInfoLength != 0)
				throw std::invalid_argument("backend URL is invalid");
			endpoint result{{parts.lpszHostName, parts.dwHostNameLength}, {parts.lpszUrlPath, parts.dwUrlPathLength}, parts.nPort, parts.nScheme == INTERNET_SCHEME_HTTPS};
			if (result.host.empty() || (!result.secure && result.host != L"127.0.0.1" && result.host != L"localhost"))
				throw std::invalid_argument("backend URL must use HTTPS");
			while (result.path.size() > 1 && result.path.ends_with(L'/')) result.path.pop_back();
			if (result.path == L"/") result.path.clear();
			return result;
		}
	}

	backend_client::backend_client(std::string base_url, std::string server_id, std::string api_key) :
	    m_base_url(std::move(base_url)), m_server_id(std::move(server_id)), m_api_key(std::move(api_key))
	{
		(void)parse(m_base_url);
	}

	server_credentials backend_client::register_server(
	    const heartbeat_data &data) const
	{
		const auto response = nlohmann::json::parse(post(
		    "/v1/server/register",
		    nlohmann::json{
		        {"name", data.name},
		        {"address", data.address},
		        {"version", data.version},
		        {"maxPlayers", data.max_players},
		        {"passwordProtected", data.password_protected},
		        {"levelId", data.level_id}}.dump(),
		    false));
		server_credentials result{
		    response.at("id").get<std::string>(),
		    response.at("apiKey").get<std::string>()};
		if (result.id.empty() || result.id.size() > 64
		    || result.api_key.empty() || result.api_key.size() > 128)
			throw std::runtime_error("backend returned invalid server credentials");
		return result;
	}

	std::optional<std::string> backend_client::introspect(std::string_view token, std::string &error) const
	{
		try
		{
			if (token.empty()) { error = "KCD2Online access token is missing"; return std::nullopt; }
			const auto response = nlohmann::json::parse(post("/v1/server/tokens/introspect", nlohmann::json{{"accessToken", token}}.dump()));
			if (!response.value("active", false)) { error = "KCD2Online access token is invalid or expired"; return std::nullopt; }
			const auto account = response.value("accountId", std::string{});
			if (account.empty()) { error = "KCD2Online response has no account ID"; return std::nullopt; }
			return account;
		}
		catch (const std::exception &exception) { error = std::string("KCD2Online authentication unavailable: ") + exception.what(); return std::nullopt; }
	}

	bool backend_client::heartbeat(const heartbeat_data &data, std::string &error) const
	{
		try
		{
			(void)post("/v1/server/heartbeat", nlohmann::json{
			    {"name", data.name}, {"address", data.address}, {"version", data.version},
			    {"playerCount", data.player_count}, {"maxPlayers", data.max_players},
			    {"passwordProtected", data.password_protected}, {"levelId", data.level_id}}.dump());
			return true;
		}
		catch (const std::exception &exception) { error = exception.what(); return false; }
	}

	std::string backend_client::post(
	    std::string_view path,
	    std::string_view body,
	    bool authenticated) const
	{
		const auto target = parse(m_base_url);
		handle session(WinHttpOpen(L"KCD2OnlineServer/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
		if (!session) throw std::runtime_error("WinHTTP initialization failed");
		WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 10000);
		handle connection(WinHttpConnect(session.get(), target.host.c_str(), target.port, 0));
		const auto request_path = target.path + wide(path);
		handle request(WinHttpOpenRequest(connection.get(), L"POST", request_path.c_str(), nullptr, nullptr, WINHTTP_DEFAULT_ACCEPT_TYPES, target.secure ? WINHTTP_FLAG_SECURE : 0));
		if (!request) throw std::runtime_error("WinHTTP request creation failed");
		if (authenticated && (m_server_id.empty() || m_api_key.empty()))
			throw std::runtime_error("server credentials are unavailable");
		auto header_text = std::string("Content-Type: application/json\r\nAccept: application/json\r\n");
		if (authenticated)
			header_text += "X-KCD2O-Server-Id: " + m_server_id
			    + "\r\nX-KCD2O-Server-Key: " + m_api_key + "\r\n";
		const auto headers = wide(header_text);
		if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()), const_cast<char *>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)
		    || !WinHttpReceiveResponse(request.get(), nullptr)) throw std::runtime_error("backend is unavailable");
		DWORD status{}, status_size = sizeof(status);
		WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &status_size, nullptr);
		std::string response;
		for (;;) { DWORD available{}; if (!WinHttpQueryDataAvailable(request.get(), &available)) throw std::runtime_error("backend response failed"); if (!available) break; const auto offset = response.size(); response.resize(offset + available); DWORD read{}; if (!WinHttpReadData(request.get(), response.data() + offset, available, &read)) throw std::runtime_error("backend response failed"); response.resize(offset + read); }
		if (status < 200 || status >= 300) throw std::runtime_error("backend rejected request (HTTP " + std::to_string(status) + ")");
		return response;
	}

	std::optional<server_credentials> load_server_credentials(
	    const std::filesystem::path &path)
	{
		if (!std::filesystem::exists(path))
			return std::nullopt;
		std::ifstream input(path, std::ios::binary);
		const std::string text{std::istreambuf_iterator<char>(input), {}};
		if (text.empty())
			throw std::runtime_error("server identity file is empty");
		const auto json = nlohmann::json::parse(text);
		if (json.value("version", 0) != 1)
			throw std::runtime_error("server identity file version is unsupported");
		server_credentials result{
		    json.at("serverId").get<std::string>(),
		    json.at("apiKey").get<std::string>()};
		if (result.id.empty() || result.id.size() > 64
		    || result.api_key.empty() || result.api_key.size() > 128)
			throw std::runtime_error("server identity file is invalid");
		return result;
	}

	void save_server_credentials(
	    const std::filesystem::path &path,
	    const server_credentials &credentials)
	{
		std::filesystem::create_directories(path.parent_path());
		const auto temporary = path.wstring() + L".tmp";
		{
			std::ofstream output(
			    std::filesystem::path(temporary),
			    std::ios::binary | std::ios::trunc);
			output << nlohmann::json{
			    {"version", 1},
			    {"serverId", credentials.id},
			    {"apiKey", credentials.api_key}}.dump(2) << '\n';
			if (!output)
				throw std::runtime_error("could not write server identity file");
		}
		if (!MoveFileExW(
		        temporary.c_str(),
		        path.c_str(),
		        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			DeleteFileW(temporary.c_str());
			throw std::runtime_error("could not persist server identity file");
		}
	}
}
