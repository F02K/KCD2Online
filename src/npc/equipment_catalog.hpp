#pragma once

#include "npc/catalog.hpp"
#include "npc/npc.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kcd2o::npc
{
	struct equipment_definition
	{
		std::string definition_id;
		std::string equipped_slot;
		int layer{};
		weapon_class weapon{weapon_class::none};

		friend bool operator==(
		    const equipment_definition &,
		    const equipment_definition &) = default;
	};

	struct equipment_slot_definition
	{
		std::uint32_t native_id{};
		std::string name;
		int layer{};

		friend bool operator==(
		    const equipment_slot_definition &,
		    const equipment_slot_definition &) = default;
	};

	class equipment_catalog
	{
	public:
		[[nodiscard]] bool load_tables_pak(
		    const std::filesystem::path &path,
		    std::string &error);
		[[nodiscard]] bool load_game_install(
		    const std::filesystem::path &game_root,
		    std::string &error);
		[[nodiscard]] bool load_documents(
		    std::span<const catalog_document> documents,
		    std::string &error);

		[[nodiscard]] const equipment_definition *find(
		    std::string_view definition_id) const;
		[[nodiscard]] const equipment_slot_definition *find_slot(
		    std::uint32_t native_id) const;
		[[nodiscard]] int layer_for_slot(std::string_view slot) const;
		[[nodiscard]] const std::vector<equipment_definition> &entries() const;
		[[nodiscard]] std::size_t size() const;

	private:
		void rebuild_index();

		std::vector<equipment_definition> m_entries;
		std::unordered_map<std::string, std::size_t> m_index;
		std::unordered_map<std::uint32_t, equipment_slot_definition> m_slots;
	};

	[[nodiscard]] equipment_catalog &runtime_equipment_catalog();
	[[nodiscard]] bool initialize_runtime_equipment_catalog(
	    std::string &error);
}
