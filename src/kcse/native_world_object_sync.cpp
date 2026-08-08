#include "kcse/native_world_object_sync.hpp"

#include "multiplayer/protocol.hpp"

#include <Windows.h>

#include <crysystem/SSystemGlobalEnvironment.h>
#include <crysystem/ScriptAnyValue.h>
#include <entitymodule/C_Inventory.h>
#include <entitymodule/C_InventoryManager.h>
#include <entitymodule/C_Item.h>
#include <entitymodule/S_ItemClass.h>
#include <framework/GuidUtils.h>
#include <Offsets/vtables/IEntity.h>
#include <Offsets/vtables/IEntitySystem.h>
#include <Offsets/vtables/IScriptSystem.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace kcd2o::kcse
{
	namespace
	{
		constexpr int entity_event_init = 3;
		constexpr int entity_event_script = 18;
		constexpr std::uint32_t inventory_poll_interval_frames = 6;

		struct native_entity_event
		{
			std::int32_t event{};
			std::int32_t padding{};
			std::intptr_t parameters[4]{};
			float floats[2]{};
		};

		enum class world_script_event
		{
			none,
			open,
			close
		};

		world_script_event guarded_world_script_event(
		    const char *name) noexcept
		{
			if (!name)
				return world_script_event::none;
#ifdef _WIN32
			__try
			{
				if (std::strcmp(name, "Open") == 0)
					return world_script_event::open;
				if (std::strcmp(name, "Close") == 0)
					return world_script_event::close;
				return world_script_event::none;
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				return world_script_event::none;
			}
#else
			if (std::strcmp(name, "Open") == 0)
				return world_script_event::open;
			if (std::strcmp(name, "Close") == 0)
				return world_script_event::close;
			return world_script_event::none;
#endif
		}

		bool execute_script(std::string_view script) noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pScriptSystem)
				return false;
#ifdef _WIN32
			__try
			{
				return environment->pScriptSystem->ExecuteBuffer(
				    script.data(),
				    script.size(),
				    "KCD2Online world sync",
				    nullptr);
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
#else
			return environment->pScriptSystem->ExecuteBuffer(
			    script.data(), script.size(), "KCD2Online world sync", nullptr);
#endif
		}

		bool read_script_global(
		    const char *name,
		    ScriptAnyValue &value) noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pScriptSystem)
				return false;
#ifdef _WIN32
			__try
			{
				return environment->pScriptSystem->GetGlobalAny(name, value);
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
#else
			return environment->pScriptSystem->GetGlobalAny(name, value);
#endif
		}

		struct script_world_view
		{
			protocol::WorldObjectKind kind{
			    protocol::WORLD_OBJECT_KIND_UNSPECIFIED};
			world_inventory_source inventory_source{
			    world_inventory_source::none};
			bool opened{};
			std::uint64_t inventory_wuid{};
		};

		std::optional<std::uint64_t> script_wuid(
		    const ScriptAnyValue &value)
		{
			if (value.type == ANY_THANDLE && value.nHandle != 0)
				return static_cast<std::uint64_t>(value.nHandle);
			return std::nullopt;
		}

		std::optional<script_world_view> inspect_world_entity(
		    Offsets::IEntity *entity)
		{
			if (!entity || entity->GetId() == 0 || entity->GetGuid() == 0)
				return std::nullopt;
			const auto script = std::format(
			    "KCD2Online_world_inventory_source=0 "
			    "KCD2Online_world_is_door=false KCD2Online_world_open=false "
			    "KCD2Online_world_inventory=nil local e=System.GetEntity({}) "
			    "if e then if e.GetInventoryToOpen~=nil then "
			    "KCD2Online_world_inventory_source=1 "
			    "KCD2Online_world_inventory=e:GetInventoryToOpen() "
			    "elseif e.inventoryId~=nil and e.Open~=nil and e.Close~=nil "
			    "and e.OnInventoryClosed~=nil then "
			    "KCD2Online_world_inventory_source=2 "
			    "KCD2Online_world_inventory=e.inventoryId "
			    "elseif e.LockType=='door' then KCD2Online_world_is_door=true end "
			    "KCD2Online_world_open=(e.bOpened==1 or e.nDirection==1) end",
			    entity->GetId());
			if (!execute_script(script))
				return std::nullopt;

			ScriptAnyValue inventory_source;
			ScriptAnyValue is_door;
			ScriptAnyValue opened;
			if (!read_script_global(
			        "KCD2Online_world_inventory_source", inventory_source)
			    || inventory_source.type != ANY_TNUMBER
			    || inventory_source.number < 0.0F
			    || inventory_source.number > 2.0F
			    || !read_script_global("KCD2Online_world_is_door", is_door)
			    || is_door.type != ANY_TBOOLEAN
			    || !read_script_global("KCD2Online_world_open", opened)
			    || opened.type != ANY_TBOOLEAN)
			{
				return std::nullopt;
			}

			script_world_view result;
			result.inventory_source = static_cast<world_inventory_source>(
			    static_cast<std::uint8_t>(inventory_source.number));
			result.kind = classify_world_object(
			    result.inventory_source, is_door.b);
			result.opened = opened.b;
			if (result.kind == protocol::WORLD_OBJECT_KIND_UNSPECIFIED)
				return std::nullopt;
			if (result.kind == protocol::WORLD_OBJECT_KIND_CONTAINER)
			{
				ScriptAnyValue inventory;
				if (!read_script_global("KCD2Online_world_inventory", inventory))
					return std::nullopt;
				const auto wuid = script_wuid(inventory);
				if (!wuid)
					return std::nullopt;
				result.inventory_wuid = *wuid;
			}
			return result;
		}

		protocol::InventoryItem wire_item(
		    const wh::entitymodule::C_Item &item)
		{
			protocol::InventoryItem result;
			result.set_instance_id(wh::FormatGuid(item.m_instanceGuid));
			if (item.m_pClassData)
				result.set_definition_id(wh::FormatGuid(item.m_pClassData->m_guid));
			result.set_count(static_cast<std::uint32_t>(
			    std::max(item.m_amount, 0)));
			result.set_quality(static_cast<float>(item.GetQuality()));
			result.set_condition(item.GetCondition());
			return result;
		}
	}

	void native_world_object_sync::process()
	{
		auto *environment = SSystemGlobalEnvironment::GetInstance();
		auto *system = environment ? environment->pEntitySystem : nullptr;
		if (!system)
			return;

		if (!m_deferred_states.empty())
		{
			std::vector<protocol::WorldObjectState> ready;
			for (const auto &[guid, state] : m_deferred_states)
			{
				if (system->FindEntityByGuid(guid) != 0)
					ready.push_back(state);
			}
			for (const auto &state : ready)
			{
				std::string ignored;
				if (apply(state, ignored))
					m_deferred_states.erase(state.entity_guid());
			}
		}

		if (++m_poll_frame % inventory_poll_interval_frames != 0)
			return;
		for (const auto guid : m_open_containers)
		{
			const auto id = system->FindEntityByGuid(guid);
			if (auto *entity = id ? system->GetEntity(id) : nullptr)
			{
				if (auto state = capture(entity))
				{
					state->set_revision(0);
					const auto previous = m_last_observations.find(guid);
					if (previous == m_last_observations.end()
					    || previous->second.SerializeAsString()
					        != state->SerializeAsString())
					{
						m_last_observations.insert_or_assign(guid, *state);
						m_updates.push_back(std::move(*state));
					}
				}
			}
		}
	}

	std::vector<protocol::WorldObjectState>
	native_world_object_sync::poll_updates()
	{
		std::vector<protocol::WorldObjectState> result;
		result.reserve(m_updates.size());
		while (!m_updates.empty())
		{
			result.push_back(std::move(m_updates.front()));
			m_updates.pop_front();
		}
		return result;
	}

	std::optional<protocol::WorldObjectState>
	native_world_object_sync::capture(Offsets::IEntity *entity) const
	{
		const auto inspected = inspect_world_entity(entity);
		if (!inspected)
			return std::nullopt;
		protocol::WorldObjectState state;
		state.set_entity_guid(entity->GetGuid());
		state.set_kind(inspected->kind);
		state.set_opened(effective_world_object_opened(
		    inspected->kind,
		    inspected->opened,
		    m_open_containers.contains(entity->GetGuid())));
		state.set_revision(0);
		if (inspected->kind == protocol::WORLD_OBJECT_KIND_CONTAINER)
		{
			auto *manager = wh::entitymodule::C_InventoryManager::GetInstance();
			const wh::framework::WUID wuid{inspected->inventory_wuid};
			auto *inventory = manager ? manager->LookupByWUID(wuid) : nullptr;
			if (!inventory)
				return std::nullopt;
			state.set_has_inventory(true);
			for (const auto *item : inventory->m_items)
			{
				if (!item || !item->m_pClassData || item->m_amount <= 0)
					continue;
				*state.add_inventory() = wire_item(*item);
			}
			std::ranges::sort(
			    *state.mutable_inventory(),
			    {},
			    &protocol::InventoryItem::instance_id);
		}
		return is_valid_world_object_state(state, false)
		    ? std::optional{std::move(state)}
		    : std::nullopt;
	}

	bool native_world_object_sync::apply_inventory(
	    Offsets::IEntity *entity,
	    const protocol::WorldObjectState &state,
	    std::string &error) const
	{
		if (!state.has_inventory())
			return true;
		const auto inspected = inspect_world_entity(entity);
		auto *manager = wh::entitymodule::C_InventoryManager::GetInstance();
		const wh::framework::WUID wuid{
		    inspected ? inspected->inventory_wuid : 0};
		auto *inventory = manager && inspected
		    ? manager->LookupByWUID(wuid)
		    : nullptr;
		if (!inventory)
		{
			error = "native container inventory is unavailable";
			return false;
		}
		std::unordered_map<std::string, const protocol::InventoryItem *> desired;
		for (const auto &item : state.inventory())
			desired.emplace(item.instance_id(), &item);
		const auto existing = inventory->m_items;
		for (auto *item : existing)
		{
			if (!item)
				continue;
			if (!desired.contains(wh::FormatGuid(item->m_instanceGuid)))
			{
				inventory->RemoveItem(
				    item,
				    2,
				    static_cast<std::uint32_t>(item->m_amount));
			}
		}
		for (const auto &[instance_id, wire] : desired)
		{
			auto found = std::ranges::find_if(
			    inventory->m_items,
			    [&](const wh::entitymodule::C_Item *item)
			    {
				    return item
				        && wh::FormatGuid(item->m_instanceGuid) == instance_id;
			    });
			wh::entitymodule::C_Item *native = found == inventory->m_items.end()
			    ? nullptr
			    : *found;
			if (native
			    && (!native->m_pClassData
			        || wh::FormatGuid(native->m_pClassData->m_guid)
			            != wire->definition_id()))
			{
				error = "native container item definition does not match its instance";
				return false;
			}
			if (!native)
			{
				CryGUID definition{};
				CryGUID instance{};
				if (!wh::ParseGuid(wire->definition_id().c_str(), definition)
				    || !wh::ParseGuid(instance_id.c_str(), instance))
				{
					error = "container item UUID could not be parsed";
					return false;
				}
				native = inventory->CreateItem(
				    definition,
				    wire->condition(),
				    wire->count());
				if (!native)
				{
					error = "native container item creation failed";
					return false;
				}
				native->SetInstanceGuid(instance);
			}
			else if (native->m_amount != static_cast<std::int32_t>(wire->count())
			    && !inventory->ChangeItemAmount(
			        native,
			        static_cast<std::int32_t>(wire->count()) - native->m_amount))
			{
				error = "native container stack update failed";
				return false;
			}
			if (!native->SetCondition(wire->condition()))
			{
				error = "native container condition update failed";
				return false;
			}
			if (native->IsOfType(wh::entitymodule::E_ItemType::Equippable)
			    && !native->SetQuality(
			        static_cast<std::int32_t>(wire->quality())))
			{
				error = "native container quality update failed";
				return false;
			}
		}
		error.clear();
		return true;
	}

	bool native_world_object_sync::apply(
	    const protocol::WorldObjectState &state,
	    std::string &error)
	{
		if (!is_valid_world_object_state(state))
		{
			error = "authoritative world object state is invalid";
			return false;
		}
		auto *environment = SSystemGlobalEnvironment::GetInstance();
		auto *system = environment ? environment->pEntitySystem : nullptr;
		const auto id = system ? system->FindEntityByGuid(state.entity_guid()) : 0;
		auto *entity = id && system ? system->GetEntity(id) : nullptr;
		if (!entity)
		{
			m_deferred_states.insert_or_assign(state.entity_guid(), state);
			error.clear();
			return true;
		}
		m_deferred_states.erase(state.entity_guid());
		m_applying_world_state = true;
		const auto script = std::format(
		    "local e=System.GetEntity({}) if e then "
		    "if e.Unlock~=nil then e:Unlock() end "
		    "if {} then if e.Event_Open~=nil then e:Event_Open() end "
		    "else if e.Event_Close~=nil then e:Event_Close() end end end",
		    id,
		    state.opened() ? "true" : "false");
		const bool script_applied = execute_script(script);
		const bool inventory_applied = apply_inventory(entity, state, error);
		m_applying_world_state = false;
		if (!script_applied || !inventory_applied)
		{
			if (error.empty())
				error = "native door/container script update failed";
			return false;
		}
		auto local = state;
		local.set_revision(0);
		m_last_observations.insert_or_assign(
		    state.entity_guid(), std::move(local));
		if (state.kind() == protocol::WORLD_OBJECT_KIND_CONTAINER
		    && state.opened())
		{
			m_open_containers.insert(state.entity_guid());
		}
		else
		{
			m_open_containers.erase(state.entity_guid());
		}
		error.clear();
		return true;
	}

	void native_world_object_sync::reset()
	{
		m_applying_world_state = false;
		m_poll_frame = 0;
		m_open_containers.clear();
		m_last_observations.clear();
		m_deferred_states.clear();
		m_updates.clear();
	}

	bool native_world_object_sync::handle_entity_event(
	    Offsets::IEntity *entity,
	    void *raw_event)
	{
		if (!entity || !raw_event)
			return false;
		const auto *event = static_cast<const native_entity_event *>(raw_event);
		const auto deferred = m_deferred_states.find(entity->GetGuid());
		if (deferred != m_deferred_states.end()
		    && (event->event == entity_event_init
		        || event->event == entity_event_script))
		{
			const auto authoritative = deferred->second;
			std::string ignored;
			if (apply(authoritative, ignored))
				m_deferred_states.erase(authoritative.entity_guid());
			if (event->event == entity_event_script)
				return true;
		}
		if (event->event != entity_event_script || m_applying_world_state
		    || event->parameters[0] == 0)
		{
			return false;
		}
		const auto script_event = guarded_world_script_event(
		    reinterpret_cast<const char *>(event->parameters[0]));
		if (script_event == world_script_event::none)
			return false;
		auto state = capture(entity);
		if (!state)
			return false;
		state->set_opened(script_event == world_script_event::open);
		state->set_revision(0);
		const auto guid = state->entity_guid();
		if (state->kind() == protocol::WORLD_OBJECT_KIND_CONTAINER)
		{
			if (state->opened())
				m_open_containers.insert(guid);
			else
				m_open_containers.erase(guid);
		}
		const auto previous = m_last_observations.find(guid);
		if (previous != m_last_observations.end()
		    && previous->second.SerializeAsString() == state->SerializeAsString())
		{
			return true;
		}
		m_last_observations.insert_or_assign(guid, *state);
		m_updates.push_back(std::move(*state));
		return true;
	}

	void native_world_object_sync::entity_removed(Offsets::IEntity *entity)
	{
		if (!entity)
			return;
		m_open_containers.erase(entity->GetGuid());
	}
}
