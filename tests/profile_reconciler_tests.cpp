#include "multiplayer/profile_reconciler.hpp"
#include "multiplayer/runtime.hpp"
#include "server/starter_profile.hpp"

#include <cassert>
#include <string>
#include <unordered_set>

namespace
{
	using namespace kcd2o;

	class fake_backend final : public profile_backend
	{
	public:
		protocol::PlayerProfile profile;
		std::vector<std::string> operations;
		std::unordered_set<std::string> definitions;
		std::string fail_operation;
		bool failed_once{};

		std::optional<protocol::PlayerProfile> capture(
		    std::string &) override
		{
			return profile;
		}
		bool validate_item(
		    const protocol::InventoryItem &item,
		    std::string &error) override
		{
			if (!definitions.contains(item.definition_id()))
			{
				error = "unknown definition";
				return false;
			}
			return true;
		}
		int slot_layer(std::string_view slot) const override
		{
			return slot == "body_plate" ? 20 : 10;
		}
		bool perform(std::string operation, std::string &error)
		{
			operations.push_back(operation);
			if (!failed_once && operation == fail_operation)
			{
				failed_once = true;
				error = "injected failure";
				return false;
			}
			return true;
		}
		bool unequip(std::string_view id, std::string &error) override
		{
			if (!perform("unequip:" + std::string(id), error))
				return false;
			for (auto &item : *profile.mutable_inventory())
				if (item.instance_id() == id)
					item.clear_equipped_slot();
			profile.mutable_avatar()->clear_equipment();
			return true;
		}
		bool remove_item(std::string_view id, std::string &error) override
		{
			if (!perform("remove:" + std::string(id), error))
				return false;
			auto *items = profile.mutable_inventory();
			for (int index = 0; index < items->size(); ++index)
				if ((*items)[index].instance_id() == id)
				{
					items->DeleteSubrange(index, 1);
					break;
				}
			return true;
		}
		bool create_item(
		    const protocol::InventoryItem &item,
		    std::string &error) override
		{
			if (!perform("create:" + item.instance_id(), error))
				return false;
			*profile.add_inventory() = item;
			return true;
		}
		bool update_item(
		    const protocol::InventoryItem &item,
		    std::string &error) override
		{
			if (!perform("update:" + item.instance_id(), error))
				return false;
			for (auto &existing : *profile.mutable_inventory())
				if (existing.instance_id() == item.instance_id())
				{
					const auto slot = existing.has_equipped_slot()
					    ? std::optional(existing.equipped_slot())
					    : std::nullopt;
					existing = item;
					existing.clear_equipped_slot();
					if (slot)
						existing.set_equipped_slot(*slot);
				}
			return true;
		}
		bool set_money(
		    std::int64_t value,
		    std::uint32_t subunits,
		    std::string &error) override
		{
			if (!perform("money", error))
				return false;
			profile.set_money(value);
			profile.set_money_subunits(subunits);
			return true;
		}
		bool set_rpg_value(
		    bool skill,
		    const protocol::RpgValue &value,
		    std::string &error) override
		{
			if (!perform(
			        std::string(skill ? "skill:" : "stat:") + value.id(),
			        error))
				return false;
			auto *values =
			    skill ? profile.mutable_skills() : profile.mutable_stats();
			for (auto &existing : *values)
				if (existing.id() == value.id())
					existing = value;
			return true;
		}
		bool equip(
		    std::string_view id,
		    std::string_view slot,
		    std::string &error) override
		{
			if (!perform("equip:" + std::string(id), error))
				return false;
			for (auto &item : *profile.mutable_inventory())
				if (item.instance_id() == id)
					item.set_equipped_slot(slot);
			return true;
		}
		bool set_quick_access_slots(
		    const protocol::PlayerProfile &value,
		    std::string &error) override
		{
			if (!perform("qam", error))
				return false;
			*profile.mutable_quick_access_slots() = value.quick_access_slots();
			return true;
		}
		bool set_avatar_state(
		    const protocol::AvatarDescriptor &avatar,
		    std::string &error) override
		{
			if (!perform("avatar", error))
				return false;
			*profile.mutable_avatar() = avatar;
			return true;
		}
		bool set_transform(
		    const protocol::TransformState &value,
		    std::string &error) override
		{
			if (!perform("transform", error))
				return false;
			profile.set_transform_valid(true);
			*profile.mutable_last_transform() = value;
			return true;
		}
	};
}

int main()
{
	using namespace kcd2o;
	auto baseline = server::instantiate_starter_profile(
	    server::default_starter_profile_template(),
	    1,
	    "Henry",
	    "sandbox");
	assert(baseline.inventory_size() == 1);
	assert(baseline.inventory(0).quality() == 1.0F);
	assert(!baseline.inventory(0).has_equipped_slot());
	auto *avatar = baseline.mutable_avatar();
	avatar->set_archetype_id(
	    "763db0bb-4469-497d-bdc9-712b3df91b5a");
	avatar->set_revision(1);

	auto target = baseline;
	target.set_money(250);
	target.set_money_subunits(7);
	target.mutable_stats(0)->set_level(7);
	target.mutable_inventory(0)->set_count(5);

	fake_backend success;
	success.profile = baseline;
	success.definitions.insert(target.inventory(0).definition_id());
	const auto applied = reconcile_profile(success, target);
	assert(applied.success);
	assert(success.profile.money() == 250);
	assert(success.profile.money_subunits() == 7);
	assert(success.profile.stats(0).level() == 7);
	assert(success.profile.inventory(0).count() == 5);
	assert(std::ranges::none_of(
	    success.operations,
	    [](const std::string &operation)
	    {
		    return operation.starts_with("equip:");
	    }));

	auto acknowledged = target;
	acknowledged.set_revision(target.revision() + 1);
	acknowledged.mutable_last_transform()->mutable_position()->set_x(999.0F);
	acknowledged.set_transform_valid(true);
	assert(same_native_profile_state(target, acknowledged));
	acknowledged.mutable_inventory(0)->set_count(
	    acknowledged.inventory(0).count() + 1);
	assert(!same_native_profile_state(target, acknowledged));

	auto equipped_target = target;
	auto *equipped_item = equipped_target.add_inventory();
	equipped_item->set_instance_id(
	    "22222222-2222-4222-8222-222222222222");
	equipped_item->set_definition_id(
	    "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
	equipped_item->set_count(1);
	equipped_item->set_quality(1.0F);
	equipped_item->set_condition(1.0F);
	equipped_item->set_equipped_slot("PrimaryMainHand");
	auto *visible = equipped_target.mutable_avatar()->add_equipment();
	visible->set_definition_id(equipped_item->definition_id());
	visible->set_equipped_slot(equipped_item->equipped_slot());
	auto *quick_slot = equipped_target.add_quick_access_slots();
	quick_slot->set_outfit(0);
	quick_slot->set_type(protocol::QUICK_ACCESS_SLOT_TYPE_WEAPON);
	quick_slot->set_slot(0);
	quick_slot->set_instance_id(equipped_item->instance_id());
	fake_backend equipped;
	equipped.profile = baseline;
	equipped.definitions = success.definitions;
	equipped.definitions.insert(equipped_item->definition_id());
	const auto equipped_applied = reconcile_profile(equipped, equipped_target);
	assert(equipped_applied.success);
	assert(!equipped.profile.inventory(0).has_equipped_slot());
	assert(equipped.profile.inventory(1).has_equipped_slot());
	assert(
	    equipped.profile.inventory(1).equipped_slot()
	    == "PrimaryMainHand");
	assert(equipped.profile.quick_access_slots_size() == 1);
	assert(equipped.profile.quick_access_slots(0).instance_id()
	    == equipped_item->instance_id());
	assert(std::ranges::count_if(
	           equipped.operations,
	           [](const std::string &operation)
	           {
		           return operation.starts_with("equip:");
	           })
	    == 1);

	fake_backend rollback;
	rollback.profile = baseline;
	rollback.definitions = success.definitions;
	rollback.fail_operation = "skill:alchemy";
	const auto failed = reconcile_profile(rollback, target);
	assert(!failed.success);
	assert(failed.rollback_attempted);
	assert(failed.rollback_succeeded);
	assert(rollback.profile.money() == baseline.money());
	assert(rollback.profile.stats(0).level() == baseline.stats(0).level());

	fake_backend invalid;
	invalid.profile = baseline;
	auto invalid_target = target;
	invalid_target.set_transform_valid(true);
	invalid_target.mutable_last_transform()->mutable_rotation()->Clear();
	const auto rejected = reconcile_profile(invalid, invalid_target);
	assert(!rejected.success);
	assert(!rejected.rollback_attempted);
	assert(!rejected.rollback_succeeded);
	assert(!profile_failure_requires_world_unload(rejected));
	assert(rejected.error == "target profile is invalid");
	assert(invalid.operations.empty());

	profile_apply_result unrecoverable;
	unrecoverable.rollback_attempted = true;
	assert(profile_failure_requires_world_unload(unrecoverable));
	unrecoverable.rollback_succeeded = true;
	assert(!profile_failure_requires_world_unload(unrecoverable));

	protocol::ServerBootstrap initializer;
	initializer.set_mode(protocol::BOOTSTRAP_MODE_INITIALIZE);
	*initializer.mutable_profile() = baseline;
	initializer.mutable_profile()->set_transform_valid(false);
	initializer.mutable_profile()->clear_last_transform();
	protocol::TransformState local_spawn;
	local_spawn.mutable_position()->set_x(123.0F);
	local_spawn.mutable_rotation()->set_w(1.0F);
	local_spawn.mutable_velocity();
	const auto initializer_spawn =
	    select_sandbox_spawn(initializer, local_spawn);
	assert(initializer_spawn.transform);
	assert(
	    initializer_spawn.source
	    == sandbox_spawn_source::local_engine_default);
	assert(initializer_spawn.transform->position().x() == 123.0F);

	initializer.set_mode(protocol::BOOTSTRAP_MODE_LOAD);
	const auto missing_load_spawn =
	    select_sandbox_spawn(initializer, local_spawn);
	assert(!missing_load_spawn.transform);
	assert(missing_load_spawn.source == sandbox_spawn_source::none);
	return 0;
}
