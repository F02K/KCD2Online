#include "server/item_ledger.hpp"

#include <iterator>

namespace kcd2o::server
{
	namespace
	{
		bool at_location(
		    const item_ledger_entry *entry,
		    item_location expected,
		    std::string &error)
		{
			if (!entry)
			{
				error = "item instance is not registered";
				return false;
			}
			if (entry->location != expected)
			{
				error = "item instance is not at the declared source";
				return false;
			}
			return true;
		}
	}

	item_location item_location::player(player_id id)
	{
		return {item_location_kind::player, id};
	}

	item_location item_location::container(std::uint64_t guid)
	{
		return {item_location_kind::container, guid};
	}

	item_location item_location::world()
	{
		return {item_location_kind::world, 0};
	}

	const item_ledger_entry *item_ledger::find(
	    std::string_view instance_id) const
	{
		const auto found = m_entries.find(std::string(instance_id));
		return found == m_entries.end() ? nullptr : &found->second;
	}

	std::size_t item_ledger::size() const
	{
		return m_entries.size();
	}

	bool item_ledger::register_item(
	    const protocol::InventoryItem &item,
	    item_location location,
	    std::string &error)
	{
		if (item.instance_id().empty())
		{
			error = "item instance id is empty";
			return false;
		}
		if (!m_entries.emplace(
		        item.instance_id(),
		        item_ledger_entry{item, location})
		         .second)
		{
			error = "item instance is already registered";
			return false;
		}
		return true;
	}

	bool item_ledger::replace_item(
	    const protocol::InventoryItem &item,
	    item_location expected,
	    std::string &error)
	{
		auto found = m_entries.find(item.instance_id());
		if (!at_location(
		        found == m_entries.end() ? nullptr : &found->second,
		        expected,
		        error))
		{
			return false;
		}
		if (found->second.item.definition_id() != item.definition_id())
		{
			error = "item definition cannot change for an existing instance";
			return false;
		}
		found->second.item = item;
		return true;
	}

	bool item_ledger::move(
	    std::string_view instance_id,
	    item_location source,
	    item_location destination,
	    std::string &error)
	{
		auto found = m_entries.find(std::string(instance_id));
		if (!at_location(
		        found == m_entries.end() ? nullptr : &found->second,
		        source,
		        error))
		{
			return false;
		}
		found->second.location = destination;
		return true;
	}

	bool item_ledger::split(
	    std::string_view source_instance_id,
	    item_location source,
	    std::string new_instance_id,
	    std::uint32_t count,
	    item_location destination,
	    std::string &error)
	{
		auto found = m_entries.find(std::string(source_instance_id));
		if (!at_location(
		        found == m_entries.end() ? nullptr : &found->second,
		        source,
		        error))
		{
			return false;
		}
		if (count == 0 || count >= found->second.item.count())
		{
			error = "split count must leave a non-empty source stack";
			return false;
		}
		if (new_instance_id.empty() || m_entries.contains(new_instance_id))
		{
			error = "split target instance must be new";
			return false;
		}

		auto split_item = found->second.item;
		split_item.set_instance_id(new_instance_id);
		split_item.set_count(count);
		split_item.clear_equipped_slot();
		found->second.item.set_count(found->second.item.count() - count);
		m_entries.emplace(
		    std::move(new_instance_id),
		    item_ledger_entry{std::move(split_item), destination});
		return true;
	}

	bool item_ledger::merge(
	    std::string_view source_instance_id,
	    item_location source,
	    std::string_view target_instance_id,
	    item_location destination,
	    std::uint32_t count,
	    std::string &error)
	{
		auto source_found = m_entries.find(std::string(source_instance_id));
		auto target_found = m_entries.find(std::string(target_instance_id));
		if (!at_location(
		        source_found == m_entries.end() ? nullptr : &source_found->second,
		        source,
		        error)
		    || !at_location(
		        target_found == m_entries.end() ? nullptr : &target_found->second,
		        destination,
		        error))
		{
			return false;
		}
		if (source_found == target_found || count == 0
		    || count > source_found->second.item.count())
		{
			error = "merge count or instances are invalid";
			return false;
		}
		if (!same_stack(
		        source_found->second.item,
		        target_found->second.item,
		        false))
		{
			error = "only equivalent item stacks can be merged";
			return false;
		}
		if (count > max_profile_item_count
		    || target_found->second.item.count()
		        > max_profile_item_count - count)
		{
			error = "merged stack count overflows";
			return false;
		}

		target_found->second.item.set_count(
		    target_found->second.item.count() + count);
		if (count == source_found->second.item.count())
			m_entries.erase(source_found);
		else
			source_found->second.item.set_count(
			    source_found->second.item.count() - count);
		return true;
	}

	bool item_ledger::erase(
	    std::string_view instance_id,
	    item_location expected,
	    std::string &error)
	{
		auto found = m_entries.find(std::string(instance_id));
		if (!at_location(
		        found == m_entries.end() ? nullptr : &found->second,
		        expected,
		        error))
		{
			return false;
		}
		m_entries.erase(found);
		return true;
	}

	void item_ledger::erase_location(item_location location)
	{
		for (auto iterator = m_entries.begin(); iterator != m_entries.end();)
		{
			iterator = iterator->second.location == location
			    ? m_entries.erase(iterator)
			    : std::next(iterator);
		}
	}

	bool item_ledger::same_stack(
	    const protocol::InventoryItem &left,
	    const protocol::InventoryItem &right,
	    bool compare_count)
	{
		return left.definition_id() == right.definition_id()
		    && (!compare_count || left.count() == right.count())
		    && left.quality() == right.quality()
		    && left.condition() == right.condition();
	}
}
