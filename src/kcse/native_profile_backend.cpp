#include "kcse/native_profile_backend.hpp"

#include "kcse/native_avatar_combat.hpp"
#include "kcse/native_equipment.hpp"
#include "kcse/native_inventory.hpp"
#include "kcse/join_trace.hpp"
#include "multiplayer/avatar_visual.hpp"
#include "npc/equipment_catalog.hpp"

#include <entitymodule/C_Actor.h>
#include <entitymodule/C_EquipmentManager.h>
#include <entitymodule/C_Human.h>
#include <entitymodule/C_Inventory.h>
#include <entitymodule/C_Item.h>
#include <entitymodule/C_ItemDatabase.h>
#include <entitymodule/E_ItemType.h>
#include <entitymodule/S_ItemClass.h>
#include <framework/GuidUtils.h>
#include <playermodule/C_OutfitManager.h>
#include <playermodule/C_QAMManager.h>
#include <playermodule/E_OutfitId.h>
#include <playermodule/E_QAM_FoodSlot.h>
#include <playermodule/E_QAM_WeaponSlot.h>
#include <rpgmodule/C_Soul.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <ranges>
#include <vector>

namespace kcd2o::kcse
{
	native_profile_backend::native_profile_backend(
	    native_entity_backend &entities) :
	    m_entities(entities)
	{
	}

	void native_profile_backend::set_wire_identity(
	    const protocol::PlayerProfile &profile)
	{
		m_wire_identity = profile;
		m_avatar_state = profile.avatar();
	}

	void native_profile_backend::reset()
	{
		m_wire_identity.reset();
		m_avatar_state.reset();
	}

	std::optional<native_profile_backend::native_state>
	native_profile_backend::state(std::string &error) const
	{
		const auto local = m_entities.player();
		auto *soul = local.actor ? local.actor->m_pSoul : nullptr;
		auto *inventory =
		    soul ? soul->m_inventorySoul.GetInventory() : nullptr;
		auto *equipment =
		    soul ? soul->m_inventorySoul.GetEquipmentManager() : nullptr;
		auto *outfits =
		    soul ? soul->m_inventorySoul.GetOutfitManager() : nullptr;
		if (!local.entity || !local.actor || !soul || !inventory || !equipment
		    || !outfits)
		{
			error =
			    "native readiness chain Entity -> Actor -> Soul -> Inventory "
			    "-> Equipment -> Outfit/QAM is incomplete";
			return std::nullopt;
		}
		return native_state{soul, inventory, equipment, outfits};
	}

	bool native_profile_backend::ready(std::string &error) const
	{
		const auto native = state(error);
		if (!native)
			return false;
		if (npc::runtime_equipment_catalog().size() == 0
		    && !npc::initialize_runtime_equipment_catalog(error))
			return false;
		for (const auto *item : native->inventory->m_items)
		{
			if (!item || !item->m_pClassData || item->m_amount <= 0
			    || (item->m_flags & native_item_equipped) == 0)
			{
				continue;
			}
			if (!resolved_equipped_slot(*native->equipment, *item))
			{
				error =
				    "equipped native item has no resolvable native slot: "
				    + wh::FormatGuid(item->m_pClassData->m_guid);
				return false;
			}
		}
		error.clear();
		return true;
	}

	std::optional<protocol::AvatarDescriptor>
	native_profile_backend::capture_avatar_visual(std::string &error) const
	{
		if (!m_avatar_state)
		{
			error = "native avatar state has not been initialized";
			return std::nullopt;
		}
		const auto local = m_entities.player();
		if (!local.actor)
		{
			error = "native local Human is unavailable";
			return std::nullopt;
		}

		auto avatar = *m_avatar_state;
		const auto *human =
		    reinterpret_cast<const wh::entitymodule::C_Human *>(local.actor);
		capture_native_avatar_combat(avatar, *human);
		error.clear();
		return avatar;
	}

	std::optional<protocol::PlayerProfile>
	native_profile_backend::capture(std::string &error)
	{
		if (!m_wire_identity)
		{
			error = "server profile identity has not been bound";
			return std::nullopt;
		}
		const auto native = state(error);
		if (!native)
			return std::nullopt;

		auto result = *m_wire_identity;
		result.clear_stats();
		result.clear_skills();
		result.clear_inventory();
		result.clear_quick_access_slots();

		for (std::size_t index = 0; index < canonical_stat_ids.size(); ++index)
		{
			auto *value = result.add_stats();
			value->set_id(canonical_stat_ids[index]);
			value->set_level(static_cast<std::int32_t>(
			    native->soul->GetStatLevel(static_cast<std::uint32_t>(index))));
			value->set_progress(native->soul->GetStatProgress(
			    static_cast<std::uint32_t>(index)));
		}
		for (std::uint32_t outfit = 0; outfit < 3; ++outfit)
		{
			const auto outfit_id = static_cast<
			    wh::playermodule::E_OutfitId::Type>(outfit);
			auto *weapons = native->outfits->GetWeaponQAMManager(outfit_id);
			auto *consumables =
			    native->outfits->GetConsumableQAMManager(outfit_id);
			if (!weapons || !consumables)
			{
				error = "native OutfitManager returned an incomplete QAM set";
				return std::nullopt;
			}
			for (std::uint32_t slot = 0; slot < 8; ++slot)
			{
				auto *item = weapons->GetWeaponItem(static_cast<
				    wh::playermodule::E_QAM_WeaponSlot::Type>(slot));
				if (!item)
					continue;
				if (item->m_pInventory != native->inventory)
				{
					error = "weapon QAM references an item outside player inventory";
					return std::nullopt;
				}
				auto *wire = result.add_quick_access_slots();
				wire->set_outfit(outfit);
				wire->set_type(protocol::QUICK_ACCESS_SLOT_TYPE_WEAPON);
				wire->set_slot(slot);
				wire->set_instance_id(wh::FormatGuid(item->m_instanceGuid));
			}
			for (std::uint32_t slot = 0; slot < 4; ++slot)
			{
				auto *item = consumables->GetConsumableItem(static_cast<
				    wh::playermodule::E_QAM_FoodSlot::Type>(slot));
				if (!item)
					continue;
				if (item->m_pInventory != native->inventory)
				{
					error =
					    "consumable QAM references an item outside player inventory";
					return std::nullopt;
				}
				auto *wire = result.add_quick_access_slots();
				wire->set_outfit(outfit);
				wire->set_type(protocol::QUICK_ACCESS_SLOT_TYPE_CONSUMABLE);
				wire->set_slot(slot);
				wire->set_instance_id(wh::FormatGuid(item->m_instanceGuid));
			}
		}
		for (std::size_t index = 0; index < canonical_skill_ids.size(); ++index)
		{
			auto *value = result.add_skills();
			value->set_id(canonical_skill_ids[index]);
			value->set_level(static_cast<std::int32_t>(
			    native->soul->GetSkillLevel(static_cast<std::uint32_t>(index))));
			value->set_progress(native->soul->GetSkillProgress(
			    static_cast<std::uint32_t>(index)));
		}

		std::int64_t native_money{};
		std::size_t native_money_stacks{};
		for (const auto *item : native->inventory->m_items)
		{
			if (!item || !item->m_pClassData || item->m_amount <= 0)
				continue;
			if (item->IsOfType(wh::entitymodule::E_ItemType::Money))
			{
				native_money += item->m_amount;
				++native_money_stacks;
				continue;
			}
			auto *wire = result.add_inventory();
			wire->set_instance_id(wh::FormatGuid(item->m_instanceGuid));
			wire->set_definition_id(
			    wh::FormatGuid(item->m_pClassData->m_guid));
			wire->set_count(static_cast<std::uint32_t>(item->m_amount));
			wire->set_quality(static_cast<float>(item->GetQuality()));
			wire->set_condition(item->GetCondition());
			if ((item->m_flags & native_item_equipped) != 0)
			{
				const auto equipped_slot =
				    resolved_equipped_slot(*native->equipment, *item);
				if (!equipped_slot)
				{
					error =
					    "equipped native item has no resolvable native slot: "
					    + wire->definition_id();
					return std::nullopt;
				}
				wire->set_equipped_slot(*equipped_slot);
			}
		}
		result.set_money(native_money / money_subunits_per_groschen);
		result.set_money_subunits(static_cast<std::uint32_t>(
		    native_money % money_subunits_per_groschen));
		KCD2Online_JOIN_TRACE(
		    "join.profile.capture.money",
		    std::format(
		        "native_units={} groschen={} subunits={} stacks={}",
		        native_money,
		        result.money(),
		        result.money_subunits(),
		        native_money_stacks));

		auto avatar = m_avatar_state.value_or(result.avatar());
		replace_avatar_equipment_from_profile(avatar, result);
		if (const auto local = m_entities.player(); local.actor)
		{
			const auto *human =
			    reinterpret_cast<const wh::entitymodule::C_Human *>(
			        local.actor);
			capture_native_avatar_combat(avatar, *human);
		}
		else
			canonicalize_avatar_visual(avatar);
		m_avatar_state = avatar;
		*result.mutable_avatar() = std::move(avatar);

		if (const auto transform =
		        m_entities.read_transform(m_entities.player().entity))
		{
			result.set_transform_valid(true);
			*result.mutable_last_transform() = *transform;
		}
		else
		{
			result.set_transform_valid(false);
			result.clear_last_transform();
		}
		error.clear();
		return result;
	}

	bool native_profile_backend::validate_item(
	    const protocol::InventoryItem &item,
	    std::string &error)
	{
		CryGUID guid{};
		auto *database = wh::entitymodule::C_ItemDatabase::GetInstance();
		auto *class_data = database
		    && wh::ParseGuid(item.definition_id().c_str(), guid)
		    ? database->FindClassByGuid(guid)
		    : nullptr;
		if (!class_data)
		{
			error = "unknown native item definition: "
			    + item.definition_id();
			return false;
		}
		if (!item.has_equipped_slot())
		{
			error.clear();
			return true;
		}

		const auto *definition =
		    npc::runtime_equipment_catalog().find(item.definition_id());
		if (!class_data->IsType(wh::entitymodule::E_ItemType::Equippable)
		    || !definition)
		{
			error = std::format(
			    "native item {} is not equippable; omit equipped_slot to keep it "
			    "in inventory",
			    item.definition_id());
			return false;
		}
		if (definition->equipped_slot != item.equipped_slot())
		{
			error = std::format(
			    "native item {} belongs in slot {}, not {}",
			    item.definition_id(),
			    definition->equipped_slot,
			    item.equipped_slot());
			return false;
		}
		error.clear();
		return true;
	}

	int native_profile_backend::slot_layer(std::string_view slot) const
	{
		return npc::runtime_equipment_catalog().layer_for_slot(slot);
	}

	bool native_profile_backend::unequip(
	    std::string_view instance_id,
	    std::string &error)
	{
		const auto native = state(error);
		if (!native)
			return false;
		auto *item = find_inventory_item(*native->inventory, instance_id);
		if (!item)
		{
			error = "native item to unequip does not exist";
			return false;
		}
		if ((item->m_flags & native_item_equipped) != 0)
			native->soul->m_inventorySoul.UnequipItem(item, true);
		if ((item->m_flags & native_item_equipped) != 0)
		{
			error = "native UnequipItem did not clear equipped state";
			return false;
		}
		return true;
	}

	bool native_profile_backend::remove_item(
	    std::string_view instance_id,
	    std::string &error)
	{
		const auto native = state(error);
		if (!native)
			return false;
		auto *item = find_inventory_item(*native->inventory, instance_id);
		if (!item)
		{
			error = "native item to remove does not exist";
			return false;
		}
		if ((item->m_flags & native_item_equipped) != 0
		    && !unequip(instance_id, error))
			return false;
		native->inventory->RemoveItem(
		    item,
		    2,
		    static_cast<std::uint32_t>(item->m_amount));
		if (find_inventory_item(*native->inventory, instance_id))
		{
			error = "native RemoveItem left the instance in inventory";
			return false;
		}
		return true;
	}

	bool native_profile_backend::create_item(
	    const protocol::InventoryItem &item,
	    std::string &error)
	{
		const auto native = state(error);
		if (!native || !validate_item(item, error))
			return false;
		if (find_inventory_item(*native->inventory, item.instance_id()))
		{
			error = "native inventory already contains item instance";
			return false;
		}
		CryGUID definition{};
		CryGUID instance{};
		if (!wh::ParseGuid(item.definition_id().c_str(), definition)
		    || !wh::ParseGuid(item.instance_id().c_str(), instance))
		{
			error = "item definition or instance UUID could not be parsed";
			return false;
		}
		auto *created = native->inventory->CreateItem(
		    definition,
		    item.condition(),
		    item.count());
		if (!created)
		{
			error = "native inventory item creation failed";
			return false;
		}
		created->SetInstanceGuid(instance);
		if (!find_inventory_item(*native->inventory, item.instance_id()))
		{
			error = "native item instance GUID registration failed";
			return false;
		}
		return true;
	}

	bool native_profile_backend::update_item(
	    const protocol::InventoryItem &item,
	    std::string &error)
	{
		const auto native = state(error);
		if (!native)
			return false;
		auto *existing = find_inventory_item(
		    *native->inventory, item.instance_id());
		if (!existing || !existing->m_pClassData
		    || wh::FormatGuid(existing->m_pClassData->m_guid)
		        != item.definition_id())
		{
			error = "native item instance/definition mismatch";
			return false;
		}
		const auto desired_count = static_cast<std::int32_t>(item.count());
		if (existing->m_amount != desired_count
		    && !native->inventory->ChangeItemAmount(
		        existing,
		        desired_count - existing->m_amount))
		{
			error = "native stack amount mutation was rejected";
			return false;
		}
		if (existing->IsOfType(wh::entitymodule::E_ItemType::Equippable))
		{
			const auto quality =
			    static_cast<std::int32_t>(std::lround(item.quality()));
			if (quality > existing->GetMaxQuality())
			{
				error = std::format(
				    "native item quality {} exceeds class maximum {}",
				    quality,
				    existing->GetMaxQuality());
				return false;
			}
			if (!existing->SetQuality(quality))
			{
				error = "native semantic item quality mutation failed";
				return false;
			}
		}
		if (!existing->SetCondition(item.condition()))
		{
			error = "native semantic item condition mutation failed";
			return false;
		}
		const auto actual_count = existing->m_amount;
		const auto actual_condition = existing->GetCondition();
		const auto actual_quality = static_cast<float>(existing->GetQuality());
		if (actual_count != desired_count
		    || std::abs(actual_condition - item.condition()) > 0.001F
		    || std::abs(actual_quality - item.quality()) > 0.01F)
		{
			error = std::format(
			    "native item update verification failed for {}: "
			    "count={}/{} condition={}/{} quality={}/{}",
			    item.instance_id(),
			    actual_count,
			    desired_count,
			    actual_condition,
			    item.condition(),
			    actual_quality,
			    item.quality());
			KCD2Online_JOIN_TRACE("join.profile.update-item.failed", error);
			return false;
		}
		error.clear();
		return true;
	}

	bool native_profile_backend::set_money(
	    std::int64_t money,
	    std::uint32_t subunits,
	    std::string &error)
	{
		const auto native = state(error);
		if (!native)
			return false;
		if (money < 0 || money > max_profile_money
		    || subunits >= money_subunits_per_groschen)
		{
			error = std::format(
			    "invalid native money target: groschen={} subunits={}",
			    money,
			    subunits);
			KCD2Online_JOIN_TRACE(
			    "join.profile.apply-money.rejected",
			    error);
			return false;
		}
		std::vector<wh::entitymodule::C_Item *> stacks;
		for (auto *item : native->inventory->m_items)
			if (item && item->IsOfType(wh::entitymodule::E_ItemType::Money))
				stacks.push_back(item);

		CryGUID money_guid{};
		if (stacks.empty())
		{
			auto *database = wh::entitymodule::C_ItemDatabase::GetInstance();
			const auto found = database
			    ? std::ranges::find_if(
			        database->m_guidIndex,
			        [](const auto &entry)
			        {
				        return entry.second
				            && entry.second->IsType(
				                wh::entitymodule::E_ItemType::Money);
			        })
			    : decltype(database->m_guidIndex.begin()){};
			if (!database || found == database->m_guidIndex.end())
			{
				error = "native money item definition is unavailable";
				return false;
			}
			money_guid = found->first;
		}

		std::int64_t remaining =
		    money * money_subunits_per_groschen + subunits;
		KCD2Online_JOIN_TRACE(
		    "join.profile.apply-money.begin",
		    std::format(
		        "groschen={} subunits={} native_units={} existing_stacks={}",
		        money,
		        subunits,
		        remaining,
		        stacks.size()));
		for (auto *stack : stacks)
		{
			const auto amount = static_cast<std::int32_t>(std::min<std::int64_t>(
			    remaining,
			    std::numeric_limits<std::int32_t>::max()));
			if (amount == 0)
			{
				native->inventory->RemoveItem(
				    stack,
				    2,
				    static_cast<std::uint32_t>(stack->m_amount));
			}
			else if (const auto delta = amount - stack->m_amount;
			         delta != 0
			         && !native->inventory->ChangeItemAmount(stack, delta))
			{
				error = "native money stack mutation was rejected";
				return false;
			}
			remaining -= amount;
		}
		while (remaining > 0)
		{
			const auto amount = static_cast<std::uint32_t>(
			    std::min<std::int64_t>(
			        remaining,
			        std::numeric_limits<std::int32_t>::max()));
			auto *created =
			    native->inventory->CreateItem(money_guid, 1.0F, amount);
			if (!created)
			{
				error = "native money item creation failed";
				return false;
			}
			remaining -= amount;
		}
		KCD2Online_JOIN_TRACE(
		    "join.profile.apply-money.complete",
		    std::format(
		        "groschen={} subunits={} native_units={}",
		        money,
		        subunits,
		        money * money_subunits_per_groschen + subunits));
		return true;
	}

	bool native_profile_backend::set_rpg_value(
	    bool skill,
	    const protocol::RpgValue &value,
	    std::string &error)
	{
		const auto native = state(error);
		std::uint32_t id{};
		if (skill)
		{
			const auto found =
			    std::ranges::find(canonical_skill_ids, value.id());
			if (found == canonical_skill_ids.end())
			{
				error = "unknown canonical RPG value: " + value.id();
				return false;
			}
			id = static_cast<std::uint32_t>(
			    std::distance(canonical_skill_ids.begin(), found));
		}
		else
		{
			const auto found =
			    std::ranges::find(canonical_stat_ids, value.id());
			if (found == canonical_stat_ids.end())
			{
				error = "unknown canonical RPG value: " + value.id();
				return false;
			}
			id = static_cast<std::uint32_t>(
			    std::distance(canonical_stat_ids.begin(), found));
		}
		if (!native)
		{
			return false;
		}
		KCD2Online_JOIN_TRACE(
		    "join.profile.apply-rpg.begin",
		    std::format(
		        "kind={} id={} native_id={} level={} progress={}",
		        skill ? "skill" : "stat",
		        value.id(),
		        id,
		        value.level(),
		        value.progress()));
		const auto applied = skill
		    ? native->soul->SetSkillAbsolute(
		        id,
		        static_cast<std::uint32_t>(value.level()),
		        value.progress())
		    : native->soul->SetStatAbsolute(
		        id,
		        static_cast<std::uint32_t>(value.level()),
		        value.progress());
		if (!applied)
		{
			const auto actual_level = skill
			    ? native->soul->GetSkillLevel(id)
			    : native->soul->GetStatLevel(id);
			const auto actual_progress = skill
			    ? native->soul->GetSkillProgress(id)
			    : native->soul->GetStatProgress(id);
			error = std::format(
			    "native absolute RPG setter rejected {}; requested level={} "
			    "progress={}, actual level={} progress={}",
			    value.id(),
			    value.level(),
			    value.progress(),
			    actual_level,
			    actual_progress);
			KCD2Online_JOIN_TRACE("join.profile.apply-rpg.failed", error);
		}
		else
		{
			KCD2Online_JOIN_TRACE(
			    "join.profile.apply-rpg.complete",
			    std::format(
			        "kind={} id={} native_id={} level={} progress={}",
			        skill ? "skill" : "stat",
			        value.id(),
			        id,
			        value.level(),
			        value.progress()));
		}
		return applied;
	}

	bool native_profile_backend::equip(
	    std::string_view instance_id,
	    std::string_view slot,
	    std::string &error)
	{
		const auto native = state(error);
		if (!native)
			return false;
		auto *item = find_inventory_item(*native->inventory, instance_id);
		if (!item)
		{
			error = "native item to equip does not exist";
			return false;
		}
		native->soul->m_inventorySoul.EquipItem(item, true);
		if ((item->m_flags & native_item_equipped) == 0)
		{
			error = "native EquipItem did not set equipped state";
			return false;
		}
		const auto actual_slot = native_equipped_slot(*native->equipment, *item);
		if (!actual_slot || *actual_slot != slot)
		{
			native->soul->m_inventorySoul.UnequipItem(item, true);
			error = actual_slot
			    ? "native item equipped into a different slot: " + *actual_slot
			    : "native EquipmentManager did not expose the equipped item slot";
			return false;
		}
		return true;
	}

	bool native_profile_backend::set_quick_access_slots(
	    const protocol::PlayerProfile &profile,
	    std::string &error)
	{
		const auto native = state(error);
		if (!native)
			return false;

		const auto desired = [&](std::uint32_t outfit,
		                         protocol::QuickAccessSlotType type,
		                         std::uint32_t slot)
		    -> const protocol::QuickAccessSlot *
		{
			const auto found = std::ranges::find_if(
			    profile.quick_access_slots(),
			    [&](const protocol::QuickAccessSlot &candidate)
			    {
				    return candidate.outfit() == outfit
				        && candidate.type() == type
				        && candidate.slot() == slot;
			    });
			return found == profile.quick_access_slots().end()
			    ? nullptr
			    : &*found;
		};

		for (std::uint32_t outfit = 0; outfit < 3; ++outfit)
		{
			const auto outfit_id = static_cast<
			    wh::playermodule::E_OutfitId::Type>(outfit);
			auto *weapons = native->outfits->GetWeaponQAMManager(outfit_id);
			auto *consumables =
			    native->outfits->GetConsumableQAMManager(outfit_id);
			if (!weapons || !consumables)
			{
				error = "native OutfitManager returned an incomplete QAM set";
				return false;
			}

			for (std::uint32_t slot = 0; slot < 8; ++slot)
			{
				const auto native_slot = static_cast<
				    wh::playermodule::E_QAM_WeaponSlot::Type>(slot);
				auto *current = weapons->GetWeaponItem(native_slot);
				const auto *target = desired(
				    outfit, protocol::QUICK_ACCESS_SLOT_TYPE_WEAPON, slot);
				if (current && (!target
				    || wh::FormatGuid(current->m_instanceGuid)
				        != target->instance_id()))
				{
					if (!weapons->ClearWeaponItem(current, native_slot))
					{
						error = "native weapon QAM clear was rejected";
						return false;
					}
				}
			}
			for (std::uint32_t slot = 0; slot < 4; ++slot)
			{
				const auto native_slot = static_cast<
				    wh::playermodule::E_QAM_FoodSlot::Type>(slot);
				auto *current = consumables->GetConsumableItem(native_slot);
				const auto *target = desired(
				    outfit, protocol::QUICK_ACCESS_SLOT_TYPE_CONSUMABLE, slot);
				if (current && (!target
				    || wh::FormatGuid(current->m_instanceGuid)
				        != target->instance_id()))
				{
					if (!consumables->ClearItem(current, slot))
					{
						error = "native consumable QAM clear was rejected";
						return false;
					}
				}
			}

			for (std::uint32_t target_slot = 0; target_slot < 8; ++target_slot)
			{
				const auto *target = desired(
				    outfit,
				    protocol::QUICK_ACCESS_SLOT_TYPE_WEAPON,
				    target_slot);
				if (!target)
					continue;
				auto *item = find_inventory_item(
				    *native->inventory, target->instance_id());
				if (!item)
				{
					error = "QAM target item is missing from player inventory";
					return false;
				}
				const auto slot = static_cast<
				    wh::playermodule::E_QAM_WeaponSlot::Type>(target_slot);
				if (weapons->GetWeaponItem(slot) != item)
					weapons->SetItem(item, target_slot / 2U);
				if (weapons->GetWeaponItem(slot) != item)
				{
					error = std::format(
					    "native weapon QAM assignment failed for outfit {} slot {}",
					    outfit,
					    target_slot);
					return false;
				}
			}
			for (std::uint32_t target_slot = 0; target_slot < 4; ++target_slot)
			{
				const auto *target = desired(
				    outfit,
				    protocol::QUICK_ACCESS_SLOT_TYPE_CONSUMABLE,
				    target_slot);
				if (!target)
					continue;
				auto *item = find_inventory_item(
				    *native->inventory, target->instance_id());
				if (!item)
				{
					error = "QAM target item is missing from player inventory";
					return false;
				}
				const auto slot = static_cast<
				    wh::playermodule::E_QAM_FoodSlot::Type>(target_slot);
				if (consumables->GetConsumableItem(slot) != item)
					consumables->SetItem(item, target_slot);
				if (consumables->GetConsumableItem(slot) != item)
				{
					error = std::format(
					    "native consumable QAM assignment failed for outfit {} slot {}",
					    outfit,
					    target_slot);
					return false;
				}
			}
		}
		error.clear();
		return true;
	}

	bool native_profile_backend::set_avatar_state(
	    const protocol::AvatarDescriptor &avatar,
	    std::string &error)
	{
		if (!is_valid_avatar_descriptor(avatar))
		{
			error = "target avatar descriptor is invalid";
			return false;
		}
		const auto local = m_entities.player();
		if (!local.actor)
		{
			error = "native local Human is unavailable";
			return false;
		}
		m_avatar_state = avatar;
		canonicalize_avatar_visual(*m_avatar_state);
		// The owner is authoritative for its native weapon/combat lifecycle.
		// Applying a server echo here used to interrupt unarmed and end-combat
		// transitions. capture_avatar_visual overlays the live native state before
		// the next update is sent.
		error.clear();
		return true;
	}

	bool native_profile_backend::set_transform(
	    const protocol::TransformState &transform,
	    std::string &error)
	{
		return m_entities.write_transform(
		    m_entities.player().entity,
		    transform,
		    error);
	}
}
