#include "server/game_install.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kcd2o::server
{
	namespace
	{
		std::filesystem::path canonical_existing(
		    const std::filesystem::path &path)
		{
			std::error_code error;
			auto result = std::filesystem::weakly_canonical(path, error);
			return error ? std::filesystem::absolute(path) : result;
		}

		std::filesystem::path normalized_root_candidate(
		    std::filesystem::path candidate)
		{
			if (candidate.filename() == "KingdomCome.exe")
				candidate = candidate.parent_path();
			if (candidate.filename() == "Win64MasterMasterSteamPGO")
				candidate = candidate.parent_path().parent_path();
			return candidate;
		}

		std::optional<std::string> read_text(
		    const std::filesystem::path &path)
		{
			std::ifstream input(path, std::ios::binary);
			if (!input)
				return std::nullopt;
			return std::string(
			    std::istreambuf_iterator<char>(input),
			    std::istreambuf_iterator<char>());
		}

		std::string unescape_vdf_path(std::string value)
		{
			for (std::size_t index{}; index + 1 < value.size();)
			{
				if (value[index] == '\\' && value[index + 1] == '\\')
					value.erase(index, 1);
				else
					++index;
			}
			return value;
		}

		std::vector<std::filesystem::path> steam_libraries(
		    const std::filesystem::path &steam_root)
		{
			std::vector<std::filesystem::path> result{steam_root};
			const auto contents = read_text(
			    steam_root / "steamapps" / "libraryfolders.vdf");
			if (!contents)
				return result;
			const std::regex path_entry(
			    R"vdf("path"\s*"([^"]+)")vdf",
			    std::regex::icase);
			for (std::sregex_iterator iterator(
			         contents->begin(), contents->end(), path_entry), end;
			     iterator != end;
			     ++iterator)
			{
				result.emplace_back(unescape_vdf_path((*iterator)[1].str()));
			}
			return result;
		}

		std::optional<std::string> manifest_install_directory(
		    const std::filesystem::path &library)
		{
			const auto contents = read_text(
			    library / "steamapps"
			    / (std::string("appmanifest_") + kcd2_steam_app_id + ".acf"));
			if (!contents)
				return std::nullopt;
			const std::regex install_entry(
			    R"vdf("installdir"\s*"([^"]+)")vdf",
			    std::regex::icase);
			std::smatch match;
			return std::regex_search(*contents, match, install_entry)
			    ? std::optional<std::string>{match[1].str()}
			    : std::nullopt;
		}

#ifdef _WIN32
		std::optional<std::filesystem::path> registry_string(
		    HKEY hive,
		    const wchar_t *subkey,
		    const wchar_t *name)
		{
			DWORD bytes{};
			if (RegGetValueW(
			        hive, subkey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes)
			    != ERROR_SUCCESS
			    || bytes < sizeof(wchar_t))
				return std::nullopt;
			std::wstring value(bytes / sizeof(wchar_t), L'\0');
			if (RegGetValueW(
			        hive, subkey, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes)
			    != ERROR_SUCCESS)
				return std::nullopt;
			value.resize(wcsnlen_s(value.c_str(), value.size()));
			return value.empty()
			    ? std::nullopt
			    : std::optional<std::filesystem::path>{value};
		}
#endif
	}

	std::optional<game_installation> inspect_game_root(
	    const std::filesystem::path &candidate,
	    std::string source)
	{
		if (candidate.empty())
			return std::nullopt;
		const auto root = canonical_existing(
		    normalized_root_candidate(candidate));
		const auto binary = root / kcd2_binary_directory;
		const auto executable = binary / "KingdomCome.exe";
		const auto whgame = binary / "WHGame.dll";
		if (!std::filesystem::is_regular_file(executable)
		    || !std::filesystem::is_regular_file(whgame))
			return std::nullopt;
		return game_installation{
		    root,
		    canonical_existing(executable),
		    canonical_existing(whgame),
		    std::move(source)};
	}

	std::vector<std::filesystem::path> default_steam_roots()
	{
		std::vector<std::filesystem::path> result;
#ifdef _WIN32
		for (const auto &entry : {
		         registry_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
		         registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"),
		         registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath")})
		{
			if (entry)
				result.push_back(*entry);
		}
#endif
		for (const auto *variable : {"ProgramFiles(x86)", "ProgramFiles"})
		{
			if (const auto *value = std::getenv(variable); value && *value)
				result.emplace_back(std::filesystem::path(value) / "Steam");
		}
		std::set<std::filesystem::path> seen;
		std::erase_if(
		    result,
		    [&](const auto &path)
		    {
			    const auto normalized = canonical_existing(path);
			    return !seen.insert(normalized).second;
		    });
		return result;
	}

	std::optional<game_installation> discover_steam_game_installation(
	    std::span<const std::filesystem::path> steam_roots)
	{
		std::set<std::filesystem::path> visited;
		for (const auto &steam_root : steam_roots)
		{
			for (const auto &library : steam_libraries(steam_root))
			{
				const auto normalized = canonical_existing(library);
				if (!visited.insert(normalized).second)
					continue;
				const auto install_directory =
				    manifest_install_directory(normalized);
				if (!install_directory)
					continue;
				if (auto found = inspect_game_root(
				        normalized / "steamapps" / "common" / *install_directory,
				        "Steam appmanifest " + std::string(kcd2_steam_app_id)))
					return found;
			}
		}
		return std::nullopt;
	}
}
