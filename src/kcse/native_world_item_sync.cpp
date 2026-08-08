#include "kcse/native_world_item_sync.hpp"

#include "multiplayer/protocol.hpp"

#include <Windows.h>

#include <crysystem/CCryAction.h>
#include <crysystem/SSystemGlobalEnvironment.h>
#include <crysystem/ScriptAnyValue.h>
#include <entitymodule/C_Actor.h>
#include <entitymodule/C_Inventory.h>
#include <entitymodule/C_InventoryManager.h>
#include <entitymodule/C_Item.h>
#include <entitymodule/S_ItemClass.h>
#include <framework/GuidUtils.h>
#include <game/S_GameContext.h>
#include <Offsets/vtables/IEntity.h>
#include <Offsets/vtables/IScriptSystem.h>

#include <algorithm>
#include <cmath>

namespace kcd2o::kcse
{
	namespace
	{
		constexpr std::uint32_t poll_interval_frames = 6;

		wh::entitymodule::C_Inventory *player_inventory(
		    Offsets::IEntity **player_entity = nullptr)
		{
			auto *framework = CCryAction::GetInstance();
			auto *entity = framework ? framework->GetClientEntity() : nullptr;
			auto *context = wh::game::S_GameContext::GetInstance();
			auto *actor = context && entity
			    ? context->GetActorById(entity->GetId())
			    : nullptr;
			if (player_entity)
				*player_entity = entity;
			return actor ? actor->GetInventory() : nullptr;
		}

		wh::entitymodule::C_Item *find_item(
		    wh::entitymodule::C_InventoryBase &inventory,
		    const std::string &instance)
		{
			const auto found = std::ranges::find_if(
			    inventory.m_items,
			    [&](const wh::entitymodule::C_Item *item)
			    {
				    return item
				        && wh::FormatGuid(item->m_instanceGuid) == instance;
			    });
			return found == inventory.m_items.end() ? nullptr : *found;
		}

		wh::entitymodule::C_Item *find_world_item(
		    wh::entitymodule::C_WorldInventory &world,
		    const std::string &instance)
		{
			const auto found = std::ranges::find_if(
			    world.m_registeredItems,
			    [&](const wh::entitymodule::C_Item *item)
			    {
				    return item
				        && wh::FormatGuid(item->m_instanceGuid) == instance;
			    });
			return found == world.m_registeredItems.end() ? nullptr : *found;
		}

		protocol::TransformState wire_transform(const Matrix34 &matrix)
		{
			protocol::TransformState result;
			auto *position = result.mutable_position();
			position->set_x(matrix.m03);
			position->set_y(matrix.m13);
			position->set_z(matrix.m23);
			const Quat rotation(matrix);
			auto *wire_rotation = result.mutable_rotation();
			wire_rotation->set_x(rotation.v.x);
			wire_rotation->set_y(rotation.v.y);
			wire_rotation->set_z(rotation.v.z);
			wire_rotation->set_w(rotation.w);
			result.mutable_velocity();
			return result;
		}

		Matrix34 native_transform(const protocol::TransformState &transform)
		{
			return Matrix34(
			    Vec3(1.0F, 1.0F, 1.0F),
			    Quat(
			        transform.rotation().w(),
			        transform.rotation().x(),
			        transform.rotation().y(),
			        transform.rotation().z()),
			    Vec3(
			        transform.position().x(),
			        transform.position().y(),
			        transform.position().z()));
		}

		bool set_world_transform(
		    Offsets::IEntity *entity,
		    const protocol::TransformState &transform) noexcept
		{
			if (!entity)
				return false;
			const auto matrix = native_transform(transform);
#ifdef _WIN32
			__try
			{
				entity->SetWorldTM(matrix, 0);
				return true;
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
#else
			entity->SetWorldTM(matrix, 0);
			return true;
#endif
		}

		bool place_from_player(
		    wh::entitymodule::C_Item &item,
		    Offsets::IEntity &player) noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			auto *scripts = environment ? environment->pScriptSystem : nullptr;
			if (!scripts)
				return false;
			ScriptAnyValue item_handle;
			item_handle.type = ANY_THANDLE;
			item_handle.nHandle = static_cast<std::int64_t>(item.m_wuid.m_value);
			ScriptAnyValue player_handle;
			player_handle.type = ANY_THANDLE;
			player_handle.nHandle = player.GetId();
#ifdef _WIN32
			__try
			{
#endif
				scripts->SetGlobalAny("KCD2Online_world_item_handle", item_handle);
				scripts->SetGlobalAny("KCD2Online_world_item_player", player_handle);
				constexpr char script[] =
				    "local p=System.GetEntity(KCD2Online_world_item_player) "
				    "if p and p.human then "
				    "p.human:PlaceItem(KCD2Online_world_item_handle,p.id,false) end";
				const auto result = scripts->ExecuteBuffer(
				    script,
				    sizeof(script) - 1,
				    "KCD2Online dropped item sync",
				    nullptr);
				scripts->SetGlobalToNull("KCD2Online_world_item_handle");
				scripts->SetGlobalToNull("KCD2Online_world_item_player");
				return result;
#ifdef _WIN32
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
#endif
		}

		bool apply_item_values(
		    wh::entitymodule::C_Item &item,
		    const protocol::InventoryItem &wire)
		{
			if (!item.m_pClassData
			    || wh::FormatGuid(item.m_pClassData->m_guid)
			        != wire.definition_id())
			{
				return false;
			}
			if (item.m_amount != static_cast<std::int32_t>(wire.count()))
			{
				if (item.m_pInventory)
				{
					if (!item.m_pInventory->ChangeItemAmount(
					        &item,
					        static_cast<std::int32_t>(wire.count())
					            - item.m_amount))
					{
						return false;
					}
				}
				else
				{
					item.SetAmount(static_cast<std::int32_t>(wire.count()));
				}
			}
			if (!item.SetCondition(wire.condition()))
				return false;
			return !item.IsOfType(wh::entitymodule::E_ItemType::Equippable)
			    || item.SetQuality(static_cast<std::int32_t>(wire.quality()));
		}
	}

	bool native_world_item_sync::begin(std::string &error)
	{
		reset();
		auto *manager = wh::entitymodule::C_InventoryManager::GetInstance();
		Offsets::IEntity *player{};
		if (!manager || !player_inventory(&player) || !player)
		{
			error = "dropped-item sync requires world and player inventories";
			return false;
		}
		for (const auto *item : manager->m_worldInventory.m_registeredItems)
		{
			if (auto state = capture(item))
				m_initial_world_items.insert_or_assign(
				    state->instance_id(), std::move(*state));
		}
		m_active = true;
		error.clear();
		return true;
	}

	void native_world_item_sync::process()
	{
		if (!m_active || m_applying
		    || ++m_poll_frame % poll_interval_frames != 0)
		{
			return;
		}
		auto *manager = wh::entitymodule::C_InventoryManager::GetInstance();
		auto *inventory = player_inventory();
		if (!manager || !inventory)
			return;
		auto &world = manager->m_worldInventory;

		for (auto iterator = m_initial_world_items.begin();
		     iterator != m_initial_world_items.end();)
		{
			if (find_item(*inventory, iterator->first))
			{
				auto removed = iterator->second;
				removed.set_revision(0);
				removed.set_present(false);
				m_managed.insert_or_assign(iterator->first, removed);
				m_updates.push_back(std::move(removed));
				iterator = m_initial_world_items.erase(iterator);
			}
			else
				++iterator;
		}

		std::vector<std::string> completed_deferred;
		for (auto &[instance, desired] : m_deferred)
		{
			if (auto *item = find_world_item(world, instance);
			    item && item->m_pLinkedEntity
			    && set_world_transform(item->m_pLinkedEntity, desired.transform()))
			{
				m_managed.insert_or_assign(instance, desired);
				completed_deferred.push_back(instance);
			}
		}
		for (const auto &instance : completed_deferred)
			m_deferred.erase(instance);

		std::unordered_set<std::string> seen;
		for (auto *item : world.m_registeredItems)
		{
			auto state = capture(item);
			if (!state)
				continue;
			const auto instance = state->instance_id();
			seen.insert(instance);
			if (m_initial_world_items.contains(instance))
				continue;
			state->set_revision(0);
			const auto previous = m_managed.find(instance);
			if (previous != m_managed.end())
			{
				auto comparable = previous->second;
				comparable.set_revision(0);
				if (comparable.SerializeAsString() == state->SerializeAsString())
					continue;
			}
			m_managed.insert_or_assign(instance, *state);
			m_updates.push_back(std::move(*state));
		}

		for (auto &[instance, previous] : m_managed)
		{
			if (!previous.present() || seen.contains(instance)
			    || !find_item(*inventory, instance))
			{
				continue;
			}
			auto removed = previous;
			removed.set_revision(0);
			removed.set_present(false);
			previous = removed;
			m_updates.push_back(std::move(removed));
		}
	}

	std::vector<protocol::WorldItemState>
	native_world_item_sync::poll_updates()
	{
		std::vector<protocol::WorldItemState> result;
		result.reserve(m_updates.size());
		while (!m_updates.empty())
		{
			result.push_back(std::move(m_updates.front()));
			m_updates.pop_front();
		}
		return result;
	}

	std::optional<protocol::WorldItemState> native_world_item_sync::capture(
	    const wh::entitymodule::C_Item *item) const
	{
		if (!item || !item->m_pClassData || !item->m_pLinkedEntity
		    || item->m_amount <= 0)
		{
			return std::nullopt;
		}
		const auto *matrix = item->m_pLinkedEntity->GetWorldTMPtr();
		if (!matrix)
			return std::nullopt;
		protocol::WorldItemState state;
		state.set_instance_id(wh::FormatGuid(item->m_instanceGuid));
		state.set_revision(0);
		state.set_present(true);
		auto *wire = state.mutable_item();
		wire->set_instance_id(state.instance_id());
		wire->set_definition_id(wh::FormatGuid(item->m_pClassData->m_guid));
		wire->set_count(static_cast<std::uint32_t>(item->m_amount));
		wire->set_quality(static_cast<float>(item->GetQuality()));
		wire->set_condition(item->GetCondition());
		*state.mutable_transform() = wire_transform(*matrix);
		return is_valid_world_item_state(state, false)
		    ? std::optional{std::move(state)}
		    : std::nullopt;
	}

	bool native_world_item_sync::apply(
	    const protocol::WorldItemState &state,
	    std::string &error)
	{
		if (!m_active || !is_valid_world_item_state(state))
		{
			error = "authoritative world item state is invalid";
			return false;
		}
		auto *manager = wh::entitymodule::C_InventoryManager::GetInstance();
		Offsets::IEntity *player{};
		auto *inventory = player_inventory(&player);
		if (!manager || !inventory || !player)
		{
			error = "native dropped-item dependencies are unavailable";
			return false;
		}
		auto &world = manager->m_worldInventory;
		m_applying = true;
		auto *item = find_world_item(world, state.instance_id());
		bool created = false;
		const auto fail = [&](const char *message)
		{
			if (created && item && item->m_pInventory)
				item->m_pInventory->RemoveItem(item, 2, 0);
			m_applying = false;
			error = message;
			return false;
		};
		if (!state.present())
		{
			if (item && item->m_pInventory)
				item->m_pInventory->RemoveItem(item, 2, 0);
			m_initial_world_items.erase(state.instance_id());
			m_deferred.erase(state.instance_id());
			m_managed.insert_or_assign(state.instance_id(), state);
			m_applying = false;
			error.clear();
			return true;
		}

		if (!item)
			item = find_item(*inventory, state.instance_id());
		if (!item)
		{
			CryGUID definition{};
			CryGUID instance{};
			if (!wh::ParseGuid(state.item().definition_id().c_str(), definition)
			    || !wh::ParseGuid(state.instance_id().c_str(), instance))
				return fail("world item UUID could not be parsed");
			item = inventory->CreateItem(
			    definition,
			    state.item().condition(),
			    state.item().count());
			if (!item)
				return fail("native world item creation failed");
			created = true;
			item->SetInstanceGuid(instance);
		}
		if (!apply_item_values(*item, state.item()))
			return fail("native world item values could not be applied");
		if (item->m_pInventory != &world
		    && !place_from_player(*item, *player))
			return fail("native Human.PlaceItem failed");
		if (item->m_pInventory != &world)
			return fail("native Human.PlaceItem did not move the item to the world");
		if (item->m_pLinkedEntity)
		{
			if (!set_world_transform(item->m_pLinkedEntity, state.transform()))
				return fail("native world item transform failed");
			m_deferred.erase(state.instance_id());
		}
		else
		{
			m_deferred.insert_or_assign(state.instance_id(), state);
		}
		m_initial_world_items.erase(state.instance_id());
		m_managed.insert_or_assign(state.instance_id(), state);
		m_applying = false;
		error.clear();
		return true;
	}

	void native_world_item_sync::reset()
	{
		m_active = false;
		m_applying = false;
		m_poll_frame = 0;
		m_initial_world_items.clear();
		m_managed.clear();
		m_deferred.clear();
		m_updates.clear();
	}
}
