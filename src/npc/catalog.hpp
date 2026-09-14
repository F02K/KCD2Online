#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kcd2o::npc
{
	inline constexpr std::string_view default_soul_id =
	    "763db0bb-4469-497d-bdc9-712b3df91b5a";
	inline constexpr std::uint64_t supported_catalog_fingerprint =
	    0x22F4D6DC5438ECABULL;
	inline constexpr std::size_t supported_catalog_size = 7266;

	struct archetype
	{
		std::string soul_id;
		std::string soul_name;
		std::string character_id;
		std::string archetype_name;
		std::string gender;
		std::string source;

		friend bool operator==(const archetype &, const archetype &) = default;
	};

	struct catalog_document
	{
		std::string source;
		std::string xml;
	};

	class catalog
	{
	public:
		[[nodiscard]] bool load_tables_pak(
		    const std::filesystem::path &path,
		    std::string &error);
		[[nodiscard]] bool load_documents(
		    std::span<const catalog_document> documents,
		    std::string &error);
		[[nodiscard]] const archetype *find(std::string_view soul_id) const;
		[[nodiscard]] bool contains(std::string_view soul_id) const;
		[[nodiscard]] std::string normalize(std::string_view soul_id) const;
		[[nodiscard]] const std::vector<archetype> &entries() const;
		[[nodiscard]] std::size_t size() const;
		[[nodiscard]] std::uint64_t fingerprint() const;

	private:
		void rebuild_index();

		std::vector<archetype> m_entries;
		std::unordered_map<std::string, std::size_t> m_index;
	};

	[[nodiscard]] catalog &runtime_catalog();
	[[nodiscard]] bool initialize_runtime_catalog(std::string &error);
}
