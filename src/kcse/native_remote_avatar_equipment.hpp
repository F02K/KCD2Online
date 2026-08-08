#pragma once

#include "multiplayer/protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wh::entitymodule
{
	class C_Human;
	class C_Inventory;
	class C_ItemDatabase;
}

namespace wh::rpgmodule
{
	class C_Soul;
}

namespace kcd2o::kcse
{
	struct native_remote_equipment_instance
	{
		std::string definition_id;
		std::string equipped_slot;
		std::string instance_id;
	};

	// Inputs required by one remote equipment transaction. Actor lifecycle and
	// avatar bookkeeping remain owned by native_remote_avatar_backend.
	struct native_remote_equipment_context
	{
		std::uint64_t player{};
		std::uint32_t entity_id{};
		wh::entitymodule::C_Human &human;
		wh::rpgmodule::C_Soul &soul;
		wh::entitymodule::C_Inventory &inventory;
		wh::entitymodule::C_ItemDatabase &database;
	};

	[[nodiscard]] bool clear_native_remote_equipment(
	    wh::rpgmodule::C_Soul &soul,
	    wh::entitymodule::C_Inventory &inventory,
	    std::vector<native_remote_equipment_instance> &item_instances,
	    std::string &error);
	[[nodiscard]] bool apply_native_remote_equipment(
	    const native_remote_equipment_context &native,
	    const protocol::AvatarDescriptor &appearance,
	    std::vector<native_remote_equipment_instance> &item_instances,
	    std::string &error);
}
