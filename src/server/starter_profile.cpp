#include "server/starter_profile.hpp"

#include "server/world_store.hpp"

#include <toml++/toml.hpp>

#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

namespace kcd2o::server
{
	namespace
	{
		protocol::RpgValue rpg_value(
		    std::string_view id,
		    std::int32_t level,
		    float progress)
		{
			protocol::RpgValue value;
			value.set_id(id);
			value.set_level(level);
			value.set_progress(progress);
			return value;
		}

		std::string generated_uuid()
		{
			auto hex = random_hex(16);
			hex[12] = '4';
			const auto variant =
			    static_cast<unsigned>((hex[16] <= '9' ? hex[16] - '0'
			                                         : hex[16] - 'a' + 10)
			        & 0x3U)
			    | 0x8U;
			hex[16] = "0123456789abcdef"[variant];
			return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-"
			    + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-"
			    + hex.substr(20, 12);
		}

		template<std::size_t Size>
		std::vector<protocol::RpgValue> parse_rpg_table(
		    const toml::table &document,
		    std::string_view table_name,
		    const std::array<std::string_view, Size> &canonical_ids)
		{
			const auto *table = document[table_name].as_table();
			if (!table || table->size() != Size)
			{
				throw std::runtime_error(
				    std::string(table_name) + " must define exactly "
				    + std::to_string(Size) + " canonical entries");
			}
			std::vector<protocol::RpgValue> result;
			result.reserve(Size);
			for (const auto id : canonical_ids)
			{
				const auto *entry = (*table)[id].as_table();
				const auto level = entry
				    ? (*entry)["level"].value<std::int64_t>()
				    : std::nullopt;
				const auto progress = entry
				    ? (*entry)["progress"].value<double>()
				    : std::nullopt;
				if (!level || !progress
				    || *level < 0 || *level > 100
				    || !std::isfinite(*progress)
				    || *progress < 0.0 || *progress > 1.0)
				{
					throw std::runtime_error(
					    std::string(table_name) + "." + std::string(id)
					    + " requires level 0..100 and progress 0..1");
				}
				result.push_back(rpg_value(
				    id,
				    static_cast<std::int32_t>(*level),
				    static_cast<float>(*progress)));
			}
			return result;
		}
	}

	starter_profile_template default_starter_profile_template()
	{
		starter_profile_template result;
		result.money = 100;
		for (const auto id : canonical_stat_ids)
			result.stats.push_back(rpg_value(id, 1, 0.0F));
		for (const auto id : canonical_skill_ids)
			result.skills.push_back(rpg_value(id, 1, 0.0F));
		result.inventory.push_back({
		    .definition_id = "da94ed8b-5b3b-4e2f-8c85-34ea3d0090ea",
		    .count = 2,
		    .quality = 1.0F,
		    .condition = 1.0F});
		return result;
	}

	starter_profile_template load_starter_profile_template(
	    const std::filesystem::path &path)
	{
		const auto document = toml::parse_file(path.string());
		const auto *profile_table = document["profile"].as_table();
		const auto money = profile_table
		    ? (*profile_table)["money"].value<std::int64_t>()
		    : std::nullopt;
		if (!money)
		{
			throw std::runtime_error(
			    "starter profile is missing [profile].money");
		}

		starter_profile_template result;
		result.money = *money;
		result.stats =
		    parse_rpg_table(document, "stats", canonical_stat_ids);
		result.skills =
		    parse_rpg_table(document, "skills", canonical_skill_ids);
		if (const auto *items = document["items"].as_array())
		{
			for (const auto &node : *items)
			{
				const auto *item = node.as_table();
				if (!item)
					throw std::runtime_error("items must contain only tables");
				starter_inventory_item parsed;
				parsed.definition_id =
				    (*item)["definition_id"].value_or(std::string{});
				const auto count = (*item)["count"].value<std::int64_t>();
				const auto quality = (*item)["quality"].value<double>();
				const auto condition = (*item)["condition"].value<double>();
				if (!count || !quality || !condition || *count < 0
				    || static_cast<std::uint64_t>(*count)
				        > std::numeric_limits<std::uint32_t>::max())
				{
					throw std::runtime_error(
					    "starter item requires valid count, quality, and condition");
				}
				parsed.count = static_cast<std::uint32_t>(*count);
				parsed.quality = static_cast<float>(*quality);
				parsed.condition = static_cast<float>(*condition);
				if (const auto slot =
				        (*item)["equipped_slot"].value<std::string>())
				{
					parsed.equipped_slot = *slot;
				}
				result.inventory.push_back(std::move(parsed));
			}
		}
		validate_starter_profile_template(result);
		return result;
	}

	void validate_starter_profile_template(
	    const starter_profile_template &profile)
	{
		if (profile.money < 0 || profile.money > max_profile_money
		    || profile.stats.size() != profile_stat_count
		    || profile.skills.size() != profile_skill_count
		    || profile.inventory.size() > max_profile_inventory_items)
		{
			throw std::runtime_error(
			    "starter profile has invalid money or collection sizes");
		}
		const auto exact_values = [](const auto &values, const auto &ids)
		{
			std::unordered_set<std::string_view> found;
			for (const auto &value : values)
			{
				if (std::ranges::find(ids, value.id()) == ids.end()
				    || !found.insert(value.id()).second
				    || value.level() < 0 || value.level() > 100
				    || !std::isfinite(value.progress())
				    || value.progress() < 0.0F || value.progress() > 1.0F)
				{
					return false;
				}
			}
			return found.size() == ids.size();
		};
		if (!exact_values(profile.stats, canonical_stat_ids)
		    || !exact_values(profile.skills, canonical_skill_ids))
		{
			throw std::runtime_error(
			    "starter profile stats or skills are incomplete");
		}
		std::unordered_set<std::string> slots;
		for (const auto &item : profile.inventory)
		{
			if (!is_uuid(item.definition_id)
			    || item.count == 0 || item.count > max_profile_item_count
			    || !std::isfinite(item.quality)
			    || item.quality < 0.0F || item.quality > 4.0F
			    || !std::isfinite(item.condition)
			    || item.condition < 0.0F || item.condition > 1.0F
			    || (item.equipped_slot
			        && (!is_valid_avatar_equipment_slot(*item.equipped_slot)
			            || !slots.insert(*item.equipped_slot).second)))
			{
				throw std::runtime_error(
				    "starter profile contains an invalid item or equipment slot");
			}
		}
	}

	protocol::PlayerProfile instantiate_starter_profile(
	    const starter_profile_template &profile,
	    player_id id,
	    std::string display_name,
	    std::string level_id)
	{
		validate_starter_profile_template(profile);
		protocol::PlayerProfile result;
		result.set_player_id(id);
		result.set_revision(1);
		result.set_display_name(std::move(display_name));
		result.set_level_id(std::move(level_id));
		result.set_money(profile.money);
		for (const auto &value : profile.stats)
			*result.add_stats() = value;
		for (const auto &value : profile.skills)
			*result.add_skills() = value;
		for (const auto &source : profile.inventory)
		{
			auto *item = result.add_inventory();
			item->set_instance_id(generated_uuid());
			item->set_definition_id(source.definition_id);
			item->set_count(source.count);
			item->set_quality(source.quality);
			item->set_condition(source.condition);
			if (source.equipped_slot)
				item->set_equipped_slot(*source.equipped_slot);
		}
		return result;
	}
}
