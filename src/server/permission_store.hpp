#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kcd2o::server
{
	class permission_store
	{
	public:
		explicit permission_store(
		    const std::filesystem::path &world_directory,
		    std::vector<std::string> owners = {});

		[[nodiscard]] bool has(
		    std::string_view persistent_id,
		    std::string_view permission) const;
		[[nodiscard]] std::vector<std::string> list(
		    std::string_view persistent_id) const;
		[[nodiscard]] bool grant(
		    std::string_view persistent_id,
		    std::string permission,
		    std::string &error);
		[[nodiscard]] bool revoke(
		    std::string_view persistent_id,
		    std::string_view permission,
		    std::string &error);
		void audit(
		    std::string_view actor,
		    std::string_view action,
		    std::string_view target,
		    std::string_view outcome,
		    std::string_view detail = {}) const;

		[[nodiscard]] static bool valid_scope(std::string_view permission);

	private:
		void load();
		[[nodiscard]] bool save(std::string &error) const;

		std::filesystem::path m_path;
		std::filesystem::path m_audit_path;
		std::unordered_set<std::string> m_owners;
		std::unordered_map<std::string, std::unordered_set<std::string>> m_grants;
	};
}
