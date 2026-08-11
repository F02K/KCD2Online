#include "server/permission_store.hpp"

#include "multiplayer/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
	#include <Windows.h>
#endif

namespace kcd2o::server
{
	permission_store::permission_store(
	    const std::filesystem::path &world_directory,
	    std::vector<std::string> owners) :
	    m_path(world_directory / "permissions.json"),
	    m_audit_path(world_directory / "admin-audit.jsonl")
	{
		std::filesystem::create_directories(world_directory);
		for (auto &owner : owners)
		{
			if (!is_uuid(owner))
				throw std::runtime_error("[permissions].owners contains an invalid UUID");
			m_owners.insert(std::move(owner));
		}
		load();
	}

	bool permission_store::valid_scope(std::string_view permission)
	{
		if (permission.empty() || permission.size() > 64)
			return false;
		if (permission == "*")
			return true;
		if (permission.front() == '.' || permission.back() == '.')
			return false;
		char previous{};
		for (const auto character : permission)
		{
			const auto valid = (character >= 'a' && character <= 'z')
			    || (character >= '0' && character <= '9')
			    || character == '.' || character == '-' || character == '_'
			    || character == '*';
			if (!valid)
				return false;
			if (character == '.' && previous == '.')
				return false;
			previous = character;
		}
		const auto wildcard = permission.find('*');
		return wildcard == std::string_view::npos
		    || (wildcard == permission.size() - 1 && wildcard > 0
		        && permission[wildcard - 1] == '.');
	}

	bool permission_store::has(
	    std::string_view persistent_id,
	    std::string_view permission) const
	{
		if (m_owners.contains(std::string(persistent_id)))
			return true;
		const auto found = m_grants.find(std::string(persistent_id));
		if (found == m_grants.end())
			return false;
		if (found->second.contains("*")
		    || found->second.contains(std::string(permission)))
			return true;
		for (const auto &granted : found->second)
		{
			if (granted.size() > 2 && granted.ends_with(".*"))
			{
				const auto prefix = std::string_view(granted).substr(0, granted.size() - 1);
				if (permission.starts_with(prefix))
					return true;
			}
		}
		return false;
	}

	std::vector<std::string> permission_store::list(
	    std::string_view persistent_id) const
	{
		std::vector<std::string> result;
		if (m_owners.contains(std::string(persistent_id)))
			result.emplace_back("*");
		if (const auto found = m_grants.find(std::string(persistent_id));
		    found != m_grants.end())
		{
			result.insert(result.end(), found->second.begin(), found->second.end());
		}
		std::ranges::sort(result);
		result.erase(std::ranges::unique(result).begin(), result.end());
		return result;
	}

	bool permission_store::grant(
	    std::string_view persistent_id,
	    std::string permission,
	    std::string &error)
	{
		if (!is_uuid(persistent_id) || !valid_scope(permission))
		{
			error = "invalid persistent player ID or permission scope";
			return false;
		}
		auto &permissions = m_grants[std::string(persistent_id)];
		if (!permissions.insert(permission).second)
			return true;
		if (!save(error))
		{
			permissions.erase(permission);
			return false;
		}
		return true;
	}

	bool permission_store::revoke(
	    std::string_view persistent_id,
	    std::string_view permission,
	    std::string &error)
	{
		if (!is_uuid(persistent_id) || !valid_scope(permission))
		{
			error = "invalid persistent player ID or permission scope";
			return false;
		}
		const auto found = m_grants.find(std::string(persistent_id));
		if (found == m_grants.end() || found->second.erase(std::string(permission)) == 0)
		{
			error = "permission was not granted";
			return false;
		}
		const auto persistent_key = std::string(persistent_id);
		if (found->second.empty())
			m_grants.erase(found);
		if (!save(error))
		{
			m_grants[persistent_key].insert(std::string(permission));
			return false;
		}
		return true;
	}

	void permission_store::load()
	{
		if (!std::filesystem::exists(m_path))
			return;
		std::ifstream input(m_path);
		if (!input)
			throw std::runtime_error("could not open permissions.json");
		const auto document = nlohmann::json::parse(input);
		if (document.value("version", 0) != 1
		    || !document.contains("grants") || !document["grants"].is_object())
			throw std::runtime_error("permissions.json has an unsupported format");
		for (const auto &[persistent_id, scopes] : document["grants"].items())
		{
			if (!is_uuid(persistent_id) || !scopes.is_array())
				throw std::runtime_error("permissions.json contains an invalid player entry");
			for (const auto &scope : scopes)
			{
				if (!scope.is_string() || !valid_scope(scope.get_ref<const std::string &>()))
					throw std::runtime_error("permissions.json contains an invalid scope");
				m_grants[persistent_id].insert(scope.get<std::string>());
			}
		}
	}

	bool permission_store::save(std::string &error) const
	{
		try
		{
			nlohmann::json grants = nlohmann::json::object();
			for (const auto &[persistent_id, scopes] : m_grants)
			{
				auto sorted = std::vector<std::string>(scopes.begin(), scopes.end());
				std::ranges::sort(sorted);
				grants[persistent_id] = std::move(sorted);
			}
			const auto temporary = m_path.string() + ".tmp";
			{
				std::ofstream output(temporary, std::ios::trunc);
				if (!output)
					throw std::runtime_error("could not create permissions temporary file");
				output << nlohmann::json{{"version", 1}, {"grants", grants}}.dump(2) << '\n';
				if (!output)
					throw std::runtime_error("could not write permissions temporary file");
			}
#ifdef _WIN32
			if (!MoveFileExW(
			        std::filesystem::path(temporary).c_str(),
			        m_path.c_str(),
			        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				throw std::system_error(
				    static_cast<int>(GetLastError()),
				    std::system_category(),
				    "could not replace permissions.json");
			}
#else
			std::filesystem::rename(temporary, m_path);
#endif
			return true;
		}
		catch (const std::exception &exception)
		{
			error = exception.what();
			return false;
		}
	}

	void permission_store::audit(
	    std::string_view actor,
	    std::string_view action,
	    std::string_view target,
	    std::string_view outcome,
	    std::string_view detail) const
	{
		try
		{
			const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
			    std::chrono::system_clock::now().time_since_epoch()).count();
			std::ofstream output(m_audit_path, std::ios::app);
			if (output)
			{
				output << nlohmann::json{
				    {"time_ms", timestamp},
				    {"actor", actor},
				    {"action", action},
				    {"target", target},
				    {"outcome", outcome},
				    {"detail", detail}}.dump() << '\n';
			}
		}
		catch (...)
		{
			// Audit failures must not terminate the authoritative server loop.
		}
	}
}
