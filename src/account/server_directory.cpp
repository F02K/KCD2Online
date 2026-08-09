#include "account/server_directory.hpp"

#include <Windows.h>
#include <dpapi.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <utility>

namespace kcd2o::account
{
	namespace
	{
		constexpr std::string_view entropy_text = "KCD2Online server preferences v1";
		DATA_BLOB blob(std::span<const std::byte> value) { return {static_cast<DWORD>(value.size()), reinterpret_cast<BYTE *>(const_cast<std::byte *>(value.data()))}; }
		std::vector<std::byte> protect(std::string_view text)
		{
			auto input = blob({reinterpret_cast<const std::byte *>(text.data()), text.size()});
			auto entropy = blob({reinterpret_cast<const std::byte *>(entropy_text.data()), entropy_text.size()});
			DATA_BLOB output{};
			if (!CryptProtectData(&input, L"KCD2Online server preferences", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) throw std::runtime_error("DPAPI protection failed");
			std::vector<std::byte> result(reinterpret_cast<std::byte *>(output.pbData), reinterpret_cast<std::byte *>(output.pbData + output.cbData)); LocalFree(output.pbData); return result;
		}
		std::string unprotect(std::span<const std::byte> value)
		{
			auto input = blob(value); auto entropy = blob({reinterpret_cast<const std::byte *>(entropy_text.data()), entropy_text.size()}); DATA_BLOB output{};
			if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) throw std::runtime_error("DPAPI decryption failed");
			std::string result(reinterpret_cast<const char *>(output.pbData), output.cbData); SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); return result;
		}
		bool valid_id(std::string_view id) { return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_' || c == '.'; }); }
		std::filesystem::path default_path()
		{
			std::wstring value(32768, L'\0'); const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
			if (!size || size >= value.size()) throw std::runtime_error("LOCALAPPDATA is unavailable"); value.resize(size); return std::filesystem::path(value) / "KCD2Online" / "server-preferences.bin";
		}
	}

	server_directory::server_directory() : m_path(default_path()) { load(); }
	server_directory::~server_directory()
	{
		if (m_worker.joinable())
			m_worker.join();
	}
	directory_snapshot server_directory::snapshot() const { std::scoped_lock lock(m_mutex); return {m_servers, m_loading, m_error, m_revision}; }
	void server_directory::refresh(std::string service_url)
	{
		std::scoped_lock lock(m_mutex); if (m_loading) return; m_loading = true; m_error.clear(); ++m_revision;
		m_worker = std::jthread([this, service_url = std::move(service_url)]
		{
			try
			{
				auto servers = account_api(service_url).list_servers();
				std::scoped_lock lock(m_mutex);
				std::ranges::sort(servers, [&](const public_server &a, const public_server &b) { const auto af = m_favorites.contains(a.id), bf = m_favorites.contains(b.id); return af != bf ? af > bf : a.name < b.name; });
				m_servers = std::move(servers); m_error.clear(); m_loading = false; ++m_revision;
			}
			catch (const std::exception &exception) { std::scoped_lock lock(m_mutex); m_error = exception.what(); m_loading = false; ++m_revision; }
		});
	}
	bool server_directory::favorite(std::string_view id) const { std::scoped_lock lock(m_mutex); return m_favorites.contains(std::string(id)); }
	void server_directory::set_favorite(std::string id, bool value)
	{
		if (!valid_id(id)) return; std::scoped_lock lock(m_mutex); if (value) m_favorites.insert(std::move(id)); else m_favorites.erase(id); save(); ++m_revision;
	}
	std::optional<std::string> server_directory::password(std::string_view id) const { std::scoped_lock lock(m_mutex); const auto found = m_passwords.find(std::string(id)); return found == m_passwords.end() ? std::nullopt : std::optional(found->second); }
	void server_directory::set_password(std::string id, std::string password)
	{
		if (!valid_id(id) || password.size() > 255) return; std::scoped_lock lock(m_mutex); if (password.empty()) m_passwords.erase(id); else m_passwords[std::move(id)] = std::move(password); save(); ++m_revision;
	}
	void server_directory::load()
	{
		if (!std::filesystem::exists(m_path)) return;
		std::ifstream input(m_path, std::ios::binary); const std::string encrypted{std::istreambuf_iterator<char>(input), {}}; if (encrypted.empty()) return;
		const auto plain = unprotect({reinterpret_cast<const std::byte *>(encrypted.data()), encrypted.size()}); const auto json = nlohmann::json::parse(plain);
		if (json.value("version", 0) != 1) return;
		for (const auto &id : json.value("favorites", std::vector<std::string>{})) if (valid_id(id)) m_favorites.insert(id);
		for (const auto &[id, value] : json.value("passwords", nlohmann::json::object()).items()) if (valid_id(id) && value.is_string() && value.get_ref<const std::string &>().size() <= 255) m_passwords[id] = value.get<std::string>();
	}
	void server_directory::save() const
	{
		nlohmann::json passwords = nlohmann::json::object(); for (const auto &[id, value] : m_passwords) passwords[id] = value;
		const auto plain = nlohmann::json{{"version", 1}, {"favorites", m_favorites}, {"passwords", passwords}}.dump(); const auto encrypted = protect(plain);
		std::filesystem::create_directories(m_path.parent_path()); const auto temporary = m_path.wstring() + L".tmp"; { std::ofstream output(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc); output.write(reinterpret_cast<const char *>(encrypted.data()), static_cast<std::streamsize>(encrypted.size())); }
		if (!MoveFileExW(temporary.c_str(), m_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(temporary.c_str()); throw std::runtime_error("Could not save server preferences"); }
	}
	server_directory &directory() { static server_directory value; return value; }
}
