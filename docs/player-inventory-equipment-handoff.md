# Player Inventory & Equipment — KCD2Online

## How we built it

We use our **libKCD2 fork** to read and modify the game's real inventory from a native KCSE plugin. libKCD2 exposes engine classes and function wrappers; our code adds item identity, slot lookup, validation, synchronization and rollback.

The core access path is `Actor → Soul → m_inventorySoul → Inventory / EquipmentManager`. Wait until these objects are ready. Remote NPCs also need their Human/Soul initialization to settle before equipping anything.

An item record contains:

```text
instance_id:   UUID of this specific item or stack
definition_id: UUID of the item type, e.g. a hunting sword
count:         1
quality:       1       // native quality grade, not a percentage
condition:     0.85    // 85% condition
equipped_slot: "PrimaryMainHand"  // omit to keep it in inventory only
```

Two identical swords share a definition UUID but have different instance UUIDs. Quick-access assignments reference the instance UUID.

## The same idea as one “script”

This could live in one C++ plugin file, or behind bindings exposed to a scripting language. **The example is pseudocode:** lowercase helpers below are application code to implement; `CreateItem`, `SetInstanceGuid`, `EquipItem`, etc. are native calls exposed by our libKCD2 fork. It is not a standalone Lua script.

```cpp
// Called in the game's update context after native readiness checks.
apply_loadout(actor, desired) {
    auto& soul = actor->m_pSoul->m_inventorySoul;
    auto* inv = soul.GetInventory();
    auto* equipment = soul.GetEquipmentManager();

    validate_definitions_slots_and_values(desired); // before mutations
    auto before = capture_inventory_and_quick_access(actor);
    try {
        clear_quick_access(actor);
        unequip_outer_to_inner(soul, before);
        remove_obsolete_instances(inv, before, desired);

        for (auto& entry : desired) {
            auto* item = find_by_instance_uuid(inv, entry.instance_id);
            if (!item) {
                item = inv->CreateItem(entry.definition_guid,
                                       entry.condition, entry.count);
                require(item != nullptr);
                item->SetInstanceGuid(entry.instance_guid);
            }
            if (item->m_amount != entry.count)
                require(inv->ChangeItemAmount(item, entry.count - item->m_amount));
            if (is_equippable(item)) require(item->SetQuality(entry.quality));
            require(item->SetCondition(entry.condition));
        }
        for (auto& entry : equipped_inner_to_outer(desired)) {
            auto* item = find_by_instance_uuid(inv, entry.instance_id);
            soul.EquipItem(item, true); // engine chooses the slot
            require(is_equipped(item));
            require(actual_slot(equipment, item) == entry.equipped_slot);
        }
        restore_target_quick_access(actor, desired);
        require(captured_state_matches(actor, desired));
    } catch (operation_failed) {
        restore_and_verify(actor, before); // explicit rollback logic
    }
}
```

The `try/catch` only illustrates checked-operation failures; it does not make native access violations recoverable. A failed rollback must abort the operation/session.

For removal, call `soul.UnequipItem(item, true)`, verify it is unequipped, then `inv->RemoveItem(item, 2, item->m_amount)` and verify the instance disappeared. For example, changing a stack from 3 to 5 uses `ChangeItemAmount(item, +2)`.

## Details that made it work

- **Slots and layers:** Our catalog reads `Tables.pak` for item definitions, equipment slots and clothing layers. Resolve `ItemAlias` / `SourceItemId` chains too. Unequip in descending layer order; equip in ascending order.
- **Verify native results:** `EquipItem()` does not accept a target slot. Check the equipped flag and actual `EquipmentManager` slot afterward. Read count, quality and condition back as well.
- **Quality:** Starter grades are `0..4`, subject to the item's native maximum. Non-equippable items may have definition-owned quality.
- **libKCD2 ABI:** Our `CreateItem` wrapper performs the engine's Build → Insert → Destroy sequence and supplies a 16-byte-aligned GUID copy required by `movaps`. Use the matching fork/address library for the game version; copying function names alone is insufficient.

## What multiplayer adds

The server validates profile revisions and tracks each real instance's owner/location. New items and stack increases require a valid source. If a server reply already matches native state, only metadata changes; do not re-equip everything.

For remote players, send only **definition UUID + equipped slot** and create local presentation items on their NPC, using separate instance UUIDs. Keep unchanged equipment, replace only differences, and treat an empty list as “remove equipment.” Holster remote weapons through the native controller before equipment changes. Local draw/combat state remains owned by the game.

## Code to look at

- `libKCD2/src/entitymodule/C_InventoryBase.cpp` — engine wrappers.
- `src/kcse/native_profile_backend.cpp` — capture, create, update and equip.
- `src/multiplayer/profile_reconciler.hpp` — ordering, verification and rollback.
- `src/npc/equipment_catalog.cpp` — slots, layers and aliases.
- `src/kcse/native_remote_avatar_equipment.cpp` — remote presentation.
