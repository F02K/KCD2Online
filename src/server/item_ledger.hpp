#pragma once

#include "multiplayer/protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kcd2o::server
{
	enum class item_location_kind
	{
		player,
		container,
		world
	};

	struct item_location
	{
		item_location_kind kind{item_location_kind::world};
		std::uint64_t id{};

		[[nodiscard]] static item_location player(player_id id);
		[[nodiscard]] static item_location container(std::uint64_t guid);
		[[nodiscard]] static item_location world();

		bool operator==(const item_location &) const = default;
	};

	struct item_ledger_entry
	{
		protocol::InventoryItem item;
		item_location location;
	};

	class item_ledger
	{
	public:
		[[nodiscard]] const item_ledger_entry *find(
		    std::string_view instance_id) const;
		[[nodiscard]] std::size_t size() const;

		[[nodiscard]] bool register_item(
		    const protocol::InventoryItem &item,
		    item_location location,
		    std::string &error);
		[[nodiscard]] bool replace_item(
		    const protocol::InventoryItem &item,
		    item_location expected,
		    std::string &error);
		[[nodiscard]] bool move(
		    std::string_view instance_id,
		    item_location source,
		    item_location destination,
		    std::string &error);
		[[nodiscard]] bool split(
		    std::string_view source_instance_id,
		    item_location source,
		    std::string new_instance_id,
		    std::uint32_t count,
		    item_location destination,
		    std::string &error);
		[[nodiscard]] bool merge(
		    std::string_view source_instance_id,
		    item_location source,
		    std::string_view target_instance_id,
		    item_location destination,
		    std::uint32_t count,
		    std::string &error);
		[[nodiscard]] bool erase(
		    std::string_view instance_id,
		    item_location expected,
		    std::string &error);
		void erase_location(item_location location);

		[[nodiscard]] static bool same_stack(
		    const protocol::InventoryItem &left,
		    const protocol::InventoryItem &right,
		    bool compare_count = true);

	private:
		std::unordered_map<std::string, item_ledger_entry> m_entries;
	};
}
