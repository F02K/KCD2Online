#include "npc/equipment_catalog.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <pugixml.hpp>
#include <zip.h>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace kcd2o::npc
{
	namespace
	{
		struct slot_definition
		{
			std::string name;
			int layer{};
		};

		struct weapon_definition
		{
			std::string slot;
			weapon_class kind{weapon_class::none};
		};

		std::string lower(std::string_view value)
		{
			std::string result(value);
			std::ranges::transform(
			    result,
			    result.begin(),
			    [](unsigned char character)
			    {
				    return static_cast<char>(std::tolower(character));
			    });
			return result;
		}

		bool is_equipment_document(std::string_view source)
		{
			const auto name = lower(source);
			if (name == "libs/tables/item/equipment_slot.xml"
			    || name == "libs/tables/item/armor_type.xml"
			    || name == "libs/tables/item/weapon_class.xml")
			{
				return true;
			}
			constexpr std::string_view prefix = "libs/tables/item/item";
			if (!name.starts_with(prefix) || !name.ends_with(".xml"))
				return false;
			const auto suffix = std::string_view(name).substr(prefix.size());
			return suffix == ".xml" || suffix.starts_with("__");
		}

		bool is_player_slot(std::string_view value)
		{
			return !value.starts_with("horse_")
			    && !value.starts_with("cattle_");
		}

		weapon_class classify_weapon(
		    std::string_view name,
		    bool two_handed)
		{
			if (name == "unarmed")
				return weapon_class::unarmed;
			if (name == "bow")
				return weapon_class::bow;
			if (name.starts_with("crossbow") || name == "rifle")
				return weapon_class::crossbow;
			if (name == "halberd")
				return weapon_class::polearm;
			return two_handed
			    ? weapon_class::two_handed
			    : weapon_class::one_handed;
		}

		std::vector<std::string> split_words(std::string_view value)
		{
			std::istringstream input{std::string(value)};
			std::vector<std::string> result;
			for (std::string word; input >> word;)
				result.push_back(std::move(word));
			return result;
		}

		bool parse_documents(
		    std::span<const catalog_document> documents,
		    std::vector<equipment_definition> &output,
		    std::unordered_map<std::uint32_t, equipment_slot_definition> &
		        output_slots,
		    std::string &error)
		{
			std::unordered_map<std::string, slot_definition> armor_slots;
			std::unordered_map<std::string, slot_definition> slots_by_name;
			std::unordered_map<int, std::string> armor_types;
			std::unordered_map<int, weapon_definition> weapon_types;
			std::unordered_map<std::uint32_t, equipment_slot_definition>
			    native_slots;

			for (const auto &document : documents)
			{
				const auto name = lower(document.source);
				pugi::xml_document xml;
				if (!xml.load_buffer(document.xml.data(), document.xml.size()))
				{
					error = "could not parse " + document.source;
					return false;
				}
				if (name.ends_with("/equipment_slot.xml"))
				{
					for (const auto node :
					     xml.child("database")
					         .child("EquipmentSlots")
					         .children("EquipmentSlot"))
					{
						const std::string slot =
						    node.attribute("Name").value();
						if (slot.empty() || !is_player_slot(slot))
							continue;
						const int layer =
						    node.attribute("BodyLayerTypeId").as_int();
						slots_by_name[slot] = {slot, layer};
						auto id = node.attribute("Id");
						if (!id)
							id = node.attribute("id");
						if (id)
						{
							const auto native_id = id.as_uint(
							    std::numeric_limits<std::uint32_t>::max());
							if (native_id
							    != std::numeric_limits<std::uint32_t>::max())
							{
								native_slots[native_id] = {
								    native_id,
								    slot,
								    layer};
							}
						}
						for (auto armor :
						     split_words(
						         node.attribute("ArmorTypes").value()))
						{
							armor_slots[lower(armor)] = {slot, layer};
						}
					}
				}
				else if (name.ends_with("/armor_type.xml"))
				{
					for (const auto node :
					     xml.child("database")
					         .child("armor_types")
					         .children("armor_type"))
					{
						armor_types[node.attribute("Id").as_int()] =
						    lower(node.attribute("Name").value());
					}
				}
				else if (name.ends_with("/weapon_class.xml"))
				{
					const auto root =
					    xml.child("database").child("WeaponClasss");
					for (const auto node : root.children())
					{
						const std::string slot =
						    node.attribute("equip_slot").value();
						const std::string kind =
						    lower(node.attribute("name").value());
						if (slot.empty() || kind == "undefined")
							continue;
						weapon_types[node.attribute("id").as_int()] = {
						    slot,
						    classify_weapon(
						        kind,
						        node.attribute("is_twohanded").as_bool())};
					}
				}
			}

			const auto map_armor = [&](std::string_view armor,
			                           std::string_view slot)
			{
				const auto found = slots_by_name.find(std::string(slot));
				if (found != slots_by_name.end())
					armor_slots[lower(armor)] = found->second;
			};
			for (const auto armor :
			     {"Gloves", "Gauntlets"})
				map_armor(armor, "gloves");
			for (const auto armor :
			     {"Shoes", "BootsAnkle", "BootsKnee", "F_Shoes"})
				map_armor(armor, "boot");
			for (const auto armor :
			     {"CollarPadded", "CollarMail"})
				map_armor(armor, "collar");
			for (const auto armor :
			     {"Hood",
			      "F_Hood",
			      "F_Bonnet",
			      "F_CapAndWimple",
			      "F_Hat",
			      "F_HoodOpen",
			      "F_Veil",
			      "F_VeilAndWimple"})
				map_armor(armor, "head_hood");
			for (const auto armor :
			     {"Coat", "Waffenrock", "Habit"})
				map_armor(armor, "body_coat");
			map_armor("Spurs", "spur");
			map_armor("Ring", "ring");
			map_armor("Necklace", "necklace");
			map_armor("Belt", "belt");
			map_armor("Pouch", "pouch");

			if (armor_slots.empty() || armor_types.empty()
			    || weapon_types.empty())
			{
				error =
				    "equipment metadata tables are incomplete";
				return false;
			}

			std::map<std::string, equipment_definition> parsed;
			std::map<std::string, std::string> aliases;
			for (const auto &document : documents)
			{
				const auto name = lower(document.source);
				if (!name.contains("/item/item"))
					continue;
				pugi::xml_document xml;
				if (!xml.load_buffer(document.xml.data(), document.xml.size()))
				{
					error = "could not parse " + document.source;
					return false;
				}
				for (const auto node :
				     xml.child("database").child("ItemClasses").children())
				{
					const std::string id =
					    lower(node.attribute("Id").value());
					if (id.empty())
						continue;
					const std::string source_id =
					    lower(node.attribute("SourceItemId").value());
					if (!source_id.empty() && source_id != id)
						aliases[id] = source_id;

					equipment_definition definition;
					definition.definition_id = id;
					const std::string type = node.name();
					if (type == "MeleeWeapon"
					    || type == "MissileWeapon")
					{
						const auto found = weapon_types.find(
						    node.attribute("Class").as_int(-1));
						if (found == weapon_types.end())
							continue;
						definition.equipped_slot = found->second.slot;
						definition.layer = 100;
						definition.weapon = found->second.kind;
					}
					else if (type == "Armor" || type == "Hood"
					    || type == "Helmet")
					{
						const auto clothing =
						    lower(node.attribute("Clothing").value());
						std::optional<slot_definition> best;
						std::size_t best_length{};
						for (const auto &[armor_id, armor_name] :
						     armor_types)
						{
							(void)armor_id;
							if (!clothing.starts_with(armor_name)
							    || armor_name.size() <= best_length)
								continue;
							const auto slot =
							    armor_slots.find(armor_name);
							if (slot == armor_slots.end())
								continue;
							best = slot->second;
							best_length = armor_name.size();
						}
						if (!best && type == "Hood")
							best = slot_definition{"head_hood", 4};
						if (!best && type == "Helmet")
							best = slot_definition{"head_helmet", 3};
						if (!best)
							continue;
						definition.equipped_slot = best->name;
						definition.layer = best->layer;
					}
					else
					{
						continue;
					}
					if (is_player_slot(definition.equipped_slot))
						parsed[id] = std::move(definition);
				}
			}

			for (std::size_t pass = 0; pass < aliases.size(); ++pass)
			{
				bool progressed = false;
				for (const auto &[alias_id, source_id] : aliases)
				{
					if (parsed.contains(alias_id))
						continue;
					const auto source = parsed.find(source_id);
					if (source == parsed.end())
						continue;
					auto inherited = source->second;
					inherited.definition_id = alias_id;
					parsed.emplace(alias_id, std::move(inherited));
					progressed = true;
				}
				if (!progressed)
					break;
			}

			output.clear();
			output.reserve(parsed.size());
			for (auto &[id, definition] : parsed)
			{
				(void)id;
				output.push_back(std::move(definition));
			}
			if (output.empty())
			{
				error = "Tables.pak contains no visible player equipment";
				return false;
			}
			output_slots = std::move(native_slots);
			return true;
		}

		bool append_archive_documents(
		    const std::filesystem::path &path,
		    std::vector<catalog_document> &documents,
		    std::string &error)
		{
			const auto encoded = path.string();
			auto *archive = zip_open(encoded.c_str(), 0, 'r');
			if (!archive)
			{
				error = "could not open equipment catalog source: "
				    + encoded;
				return false;
			}
			std::vector<catalog_document> additions;
			const auto count = zip_entries_total(archive);
			for (int index = 0; index < count; ++index)
			{
				if (zip_entry_openbyindex(archive, index) != 0)
					continue;
				const std::string name =
				    zip_entry_name(archive) ? zip_entry_name(archive) : "";
				if (!is_equipment_document(name) || zip_entry_isdir(archive))
				{
					zip_entry_close(archive);
					continue;
				}
				const auto size = zip_entry_size(archive);
				std::string xml(static_cast<std::size_t>(size), '\0');
				const auto read = zip_entry_noallocread(
				    archive,
				    xml.data(),
				    xml.size());
				zip_entry_close(archive);
				if (read < 0
				    || static_cast<std::size_t>(read) != xml.size())
				{
					zip_close(archive);
					error = "could not read " + name + " from "
					    + encoded;
					return false;
				}
				additions.push_back({name, std::move(xml)});
			}
			zip_close(archive);
			std::ranges::sort(additions, {}, &catalog_document::source);
			for (auto &addition : additions)
			{
				const auto normalized = lower(addition.source);
				const auto replaced = std::ranges::find_if(
				    documents,
				    [&](const catalog_document &document)
				    {
					    return lower(document.source) == normalized;
				    });
				if (replaced == documents.end())
					documents.push_back(std::move(addition));
				else
					*replaced = std::move(addition);
			}
			return true;
		}

		std::optional<std::string> manifest_mod_id(
		    const std::filesystem::path &directory)
		{
			pugi::xml_document manifest;
			if (!manifest.load_file(
			        (directory / "mod.manifest").string().c_str()))
				return std::nullopt;
			const auto id = manifest.child("kcd_mod")
			                    .child("info")
			                    .child("modid")
			                    .text()
			                    .as_string();
			return *id ? std::optional(lower(id)) : std::nullopt;
		}

		std::vector<std::filesystem::path> active_mod_directories(
		    const std::filesystem::path &game_root)
		{
			std::vector<std::filesystem::path> result;
			std::unordered_set<std::string> seen;
			const auto append = [&](std::filesystem::path directory)
			{
				if (directory.is_relative())
					directory = game_root / directory;
				directory = directory.lexically_normal();
				std::error_code filesystem_error;
				if (!std::filesystem::is_directory(
				        directory / "data",
				        filesystem_error))
					return;
				const auto key = lower(directory.generic_string());
				if (seen.insert(key).second)
					result.push_back(std::move(directory));
			};

			std::ifstream log(game_root / "kcd.log");
			for (std::string line; std::getline(log, line);)
			{
				const auto lowered = lower(line);
				constexpr std::string_view marker = "opening paks in ";
				const auto marker_at = lowered.find(marker);
				if (marker_at == std::string::npos)
					continue;
				const auto path_at = marker_at + marker.size();
				auto data_at = lowered.find("/data/*.pak", path_at);
				if (data_at == std::string::npos)
					data_at = lowered.find("\\data\\*.pak", path_at);
				if (data_at == std::string::npos || data_at <= path_at)
					continue;
				auto value = line.substr(path_at, data_at - path_at);
				while (!value.empty()
				    && (value.front() == '\'' || value.front() == '"'))
					value.erase(value.begin());
				while (!value.empty()
				    && (value.back() == '\'' || value.back() == '"'
				        || std::isspace(
				            static_cast<unsigned char>(value.back()))))
					value.pop_back();
				append(std::filesystem::path(value));
			}
			if (!result.empty())
				return result;

			const auto mods = game_root / "mods";
			std::error_code filesystem_error;
			if (!std::filesystem::is_directory(mods, filesystem_error))
				return result;
			std::map<std::string, std::filesystem::path> installed;
			for (const auto &entry :
			     std::filesystem::directory_iterator(
			         mods,
			         filesystem_error))
			{
				if (!entry.is_directory(filesystem_error))
					continue;
				if (const auto id = manifest_mod_id(entry.path()))
					installed.emplace(*id, entry.path());
			}
			std::ifstream order(mods / "mod_order.txt");
			if (order)
			{
				for (std::string id; std::getline(order, id);)
				{
					id = lower(id);
					id.erase(
					    std::ranges::remove_if(
					        id,
					        [](unsigned char character)
					        {
						        return std::isspace(character);
					        })
					        .begin(),
					    id.end());
					if (const auto found = installed.find(id);
					    found != installed.end())
						append(found->second);
				}
			}
			else
			{
				for (const auto &[id, directory] : installed)
				{
					(void)id;
					append(directory);
				}
			}
			return result;
		}

#ifdef _WIN32
		std::optional<std::filesystem::path> installed_tables_pak()
		{
			std::array<char, MAX_PATH> module_path{};
			const auto length = GetModuleFileNameA(
			    nullptr,
			    module_path.data(),
			    static_cast<DWORD>(module_path.size()));
			if (length == 0 || length == module_path.size())
				return std::nullopt;
			auto current = std::filesystem::path(
			    std::string(module_path.data(), length));
			for (auto parent = current.parent_path(); !parent.empty();
			     parent = parent.parent_path())
			{
				const auto candidate = parent / "Data" / "Tables.pak";
				if (std::filesystem::is_regular_file(candidate))
					return candidate;
				if (parent == parent.root_path())
					break;
			}
			return std::nullopt;
		}
#endif
	}

	bool equipment_catalog::load_tables_pak(
	    const std::filesystem::path &path,
	    std::string &error)
	{
		std::vector<catalog_document> documents;
		if (!append_archive_documents(path, documents, error))
			return false;
		return load_documents(documents, error);
	}

	bool equipment_catalog::load_game_install(
	    const std::filesystem::path &game_root,
	    std::string &error)
	{
		std::vector<catalog_document> documents;
		if (!append_archive_documents(
		        game_root / "Data" / "Tables.pak",
		        documents,
		        error))
			return false;
		for (const auto &mod : active_mod_directories(game_root))
		{
			std::vector<std::filesystem::path> archives;
			std::error_code filesystem_error;
			for (const auto &entry :
			     std::filesystem::directory_iterator(
			         mod / "data",
			         filesystem_error))
			{
				if (entry.is_regular_file(filesystem_error)
				    && lower(entry.path().extension().string()) == ".pak")
					archives.push_back(entry.path());
			}
			std::ranges::sort(archives);
			for (const auto &archive : archives)
			{
				if (!append_archive_documents(
				        archive,
				        documents,
				        error))
					return false;
			}
		}
		return load_documents(documents, error);
	}

	bool equipment_catalog::load_documents(
	    std::span<const catalog_document> documents,
	    std::string &error)
	{
		std::vector<equipment_definition> parsed;
		std::unordered_map<std::uint32_t, equipment_slot_definition>
		    parsed_slots;
		if (!parse_documents(documents, parsed, parsed_slots, error))
			return false;
		m_entries = std::move(parsed);
		m_slots = std::move(parsed_slots);
		rebuild_index();
		error.clear();
		return true;
	}

	const equipment_definition *equipment_catalog::find(
	    std::string_view definition_id) const
	{
		const auto found = m_index.find(lower(definition_id));
		return found == m_index.end() ? nullptr : &m_entries[found->second];
	}

	const equipment_slot_definition *equipment_catalog::find_slot(
	    std::uint32_t native_id) const
	{
		const auto found = m_slots.find(native_id);
		return found == m_slots.end() ? nullptr : &found->second;
	}

	int equipment_catalog::layer_for_slot(std::string_view slot) const
	{
		int result{};
		for (const auto &[native_id, definition] : m_slots)
		{
			(void)native_id;
			if (definition.name == slot)
				result = std::max(result, definition.layer);
		}
		for (const auto &definition : m_entries)
			if (definition.equipped_slot == slot)
				result = std::max(result, definition.layer);
		return result;
	}

	const std::vector<equipment_definition> &
	equipment_catalog::entries() const
	{
		return m_entries;
	}

	std::size_t equipment_catalog::size() const
	{
		return m_entries.size();
	}

	void equipment_catalog::rebuild_index()
	{
		m_index.clear();
		for (std::size_t index = 0; index < m_entries.size(); ++index)
			m_index.emplace(m_entries[index].definition_id, index);
	}

	equipment_catalog &runtime_equipment_catalog()
	{
		static equipment_catalog result;
		return result;
	}

	bool initialize_runtime_equipment_catalog(std::string &error)
	{
#ifdef _WIN32
		const auto path = installed_tables_pak();
		if (!path)
		{
			error = "could not locate Data/Tables.pak from the game executable";
			return false;
		}
		return runtime_equipment_catalog().load_game_install(
		    path->parent_path().parent_path(),
		    error);
#else
		error =
		    "runtime equipment catalog loading is only supported by the Windows client";
		return false;
#endif
	}
}
