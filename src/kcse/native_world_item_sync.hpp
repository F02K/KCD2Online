#pragma once

#include "kcd2o.pb.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wh::entitymodule
{
	class C_Item;
}

namespace kcd2o::kcse
{
	class native_world_item_sync
	{
	public:
		[[nodiscard]] bool begin(std::string &error);
		void process();
		[[nodiscard]] std::vector<protocol::WorldItemState> poll_updates();
		[[nodiscard]] bool apply(
		    const protocol::WorldItemState &state,
		    std::string &error);
		void reset();

	private:
		[[nodiscard]] std::optional<protocol::WorldItemState> capture(
		    const wh::entitymodule::C_Item *item) const;

		bool m_active{};
		bool m_applying{};
		std::uint32_t m_poll_frame{};
		std::unordered_map<std::string, protocol::WorldItemState>
		    m_initial_world_items;
		std::unordered_map<std::string, protocol::WorldItemState> m_managed;
		std::unordered_map<std::string, protocol::WorldItemState> m_deferred;
		std::deque<protocol::WorldItemState> m_updates;
	};
}
