#pragma once

#include "multiplayer/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kcd2o
{
	class profile_backend
	{
	public:
		virtual ~profile_backend() = default;
		[[nodiscard]] virtual std::optional<protocol::PlayerProfile> capture(
		    std::string &error) = 0;
		[[nodiscard]] virtual bool validate_item(
		    const protocol::InventoryItem &item,
		    std::string &error) = 0;
		[[nodiscard]] virtual int slot_layer(std::string_view slot) const = 0;
		[[nodiscard]] virtual bool unequip(
		    std::string_view instance_id,
		    std::string &error) = 0;
		[[nodiscard]] virtual bool remove_item(
		    std::string_view instance_id,
		    std::string &error) = 0;
		[[nodiscard]] virtual bool create_item(
		    const protocol::InventoryItem &item,
		    std::string &error) = 0;
		[[nodiscard]] virtual bool update_item(
		    const protocol::InventoryItem &item,
		    std::string &error) = 0;
		[[nodiscard]] virtual bool set_money(
		    std::int64_t money,
		    std::uint32_t subunits,
		    std::string &error) = 0;
		[[nodiscard]] virtual bool set_rpg_value(
		    bool skill,
		    const protocol::RpgValue &value,
		    std::string &error) = 0;
		[[nodiscard]] virtual bool equip(
		    std::string_view instance_id,
		    std::string_view slot,
		    std::string &error) = 0;
		[[nodiscard]] virtual bool set_quick_access_slots(
		    const protocol::PlayerProfile &profile,
		    std::string &error) = 0;
		[[nodiscard]] virtual bool set_avatar_state(
		    const protocol::AvatarDescriptor &avatar,
		    std::string &error) = 0;
		[[nodiscard]] virtual bool set_transform(
		    const protocol::TransformState &transform,
		    std::string &error) = 0;
	};

	struct profile_apply_result
	{
		bool success{};
		bool rollback_attempted{};
		bool rollback_succeeded{};
		std::string error;
	};

	[[nodiscard]] inline bool profile_failure_requires_world_unload(
	    const profile_apply_result &result) noexcept
	{
		return !result.success && result.rollback_attempted
		    && !result.rollback_succeeded;
	}

	namespace detail
	{
		inline bool close(float left, float right, float tolerance = 0.0001F)
		{
			return std::isfinite(left) && std::isfinite(right)
			    && std::abs(left - right) <= tolerance;
		}

		inline bool same_transform(
		    const protocol::TransformState &left,
		    const protocol::TransformState &right)
		{
			const auto position_error = std::hypot(
			    std::hypot(
			        left.position().x() - right.position().x(),
			        left.position().y() - right.position().y()),
			    left.position().z() - right.position().z());
			const auto rotation_dot = std::abs(
			    left.rotation().x() * right.rotation().x()
			    + left.rotation().y() * right.rotation().y()
			    + left.rotation().z() * right.rotation().z()
			    + left.rotation().w() * right.rotation().w());
			// Velocity is sampled state, not part of SetWorldTM. The runtime
			// resets its sampler after the verified position/rotation write.
			return position_error <= 0.01F && rotation_dot >= 0.9999F;
		}

		inline bool same_profile(
		    const protocol::PlayerProfile &left,
		    const protocol::PlayerProfile &right)
		{
			if (left.player_id() != right.player_id()
			    || left.persistent_id() != right.persistent_id()
			    || left.revision() != right.revision()
			    || left.display_name() != right.display_name()
			    || left.level_id() != right.level_id()
			    || left.money() != right.money()
			    || left.money_subunits() != right.money_subunits()
			    || left.transform_valid() != right.transform_valid()
			    || left.avatar().SerializeAsString()
			        != right.avatar().SerializeAsString()
			    || left.stats_size() != right.stats_size()
			    || left.skills_size() != right.skills_size()
			    || left.inventory_size() != right.inventory_size()
			    || left.quick_access_slots_size()
			        != right.quick_access_slots_size()
			    || (left.transform_valid()
			        && !same_transform(
			            left.last_transform(),
			            right.last_transform())))
				return false;

			const auto same_rpg = [](const auto &a, const auto &b)
			{
				for (const auto &value : a)
				{
					const auto match = std::ranges::find_if(
					    b,
					    [&](const protocol::RpgValue &candidate)
					    {
						    return candidate.id() == value.id();
					    });
					if (match == b.end()
					    || match->level() != value.level()
					    || !close(match->progress(), value.progress()))
						return false;
				}
				return true;
			};
			if (!same_rpg(left.stats(), right.stats())
			    || !same_rpg(left.skills(), right.skills()))
				return false;

			for (const auto &item : left.inventory())
			{
				const auto match = std::ranges::find_if(
				    right.inventory(),
				    [&](const protocol::InventoryItem &candidate)
				    {
					    return candidate.instance_id()
					        == item.instance_id();
				    });
				if (match == right.inventory().end()
				    || match->definition_id() != item.definition_id()
				    || match->count() != item.count()
				    || !close(match->quality(), item.quality(), 0.01F)
				    || !close(match->condition(), item.condition(), 0.001F)
				    || match->has_equipped_slot()
				        != item.has_equipped_slot()
				    || (item.has_equipped_slot()
				        && match->equipped_slot()
				            != item.equipped_slot()))
					return false;
			}
			for (const auto &slot : left.quick_access_slots())
			{
				const auto match = std::ranges::find_if(
				    right.quick_access_slots(),
				    [&](const protocol::QuickAccessSlot &candidate)
				    {
					    return candidate.outfit() == slot.outfit()
					        && candidate.type() == slot.type()
					        && candidate.slot() == slot.slot();
				    });
				if (match == right.quick_access_slots().end()
				    || match->instance_id() != slot.instance_id())
					return false;
			}
			return true;
		}

		inline bool apply_profile_once(
		    profile_backend &backend,
		    const protocol::PlayerProfile &current,
		    const protocol::PlayerProfile &target,
		    std::string &error)
		{
			for (const auto &item : target.inventory())
			{
				if (!backend.validate_item(item, error))
					return false;
			}
			protocol::PlayerProfile cleared_qam;
			if (!backend.set_quick_access_slots(cleared_qam, error))
				return false;

			std::vector<const protocol::InventoryItem *> equipped;
			for (const auto &item : current.inventory())
				if (item.has_equipped_slot())
					equipped.push_back(&item);
			std::ranges::sort(
			    equipped,
			    [&](const auto *left, const auto *right)
			    {
				    return backend.slot_layer(left->equipped_slot())
				        > backend.slot_layer(right->equipped_slot());
			    });
			for (const auto *item : equipped)
				if (!backend.unequip(item->instance_id(), error))
					return false;

			std::unordered_map<std::string_view, const protocol::InventoryItem *>
			    desired;
			for (const auto &item : target.inventory())
				desired.emplace(item.instance_id(), &item);
			for (const auto &item : current.inventory())
				if (!desired.contains(item.instance_id())
				    && !backend.remove_item(item.instance_id(), error))
					return false;

			std::unordered_map<std::string_view, const protocol::InventoryItem *>
			    existing;
			for (const auto &item : current.inventory())
				existing.emplace(item.instance_id(), &item);
			for (const auto &item : target.inventory())
			{
				if (!existing.contains(item.instance_id())
				    && !backend.create_item(item, error))
					return false;
				if (!backend.update_item(item, error))
					return false;
			}

			if (!backend.set_money(
			        target.money(),
			        target.money_subunits(),
			        error))
				return false;
			for (const auto &value : target.stats())
				if (!backend.set_rpg_value(false, value, error))
					return false;
			for (const auto &value : target.skills())
				if (!backend.set_rpg_value(true, value, error))
					return false;

			equipped.clear();
			for (const auto &item : target.inventory())
				if (item.has_equipped_slot())
					equipped.push_back(&item);
			std::ranges::sort(
			    equipped,
			    [&](const auto *left, const auto *right)
			    {
				    return backend.slot_layer(left->equipped_slot())
				        < backend.slot_layer(right->equipped_slot());
			    });
			for (const auto *item : equipped)
				if (!backend.equip(
				        item->instance_id(),
				        item->equipped_slot(),
				        error))
					return false;
			if (!backend.set_quick_access_slots(target, error))
				return false;
			if (!backend.set_avatar_state(target.avatar(), error))
				return false;
			if (target.transform_valid()
			    && !backend.set_transform(target.last_transform(), error))
				return false;
			return true;
		}
	}

	// Server acknowledgements advance wire metadata even when the native game
	// state already matches. Comparing that metadata as part of a live
	// correction used to force a full inventory/equipment transaction after
	// every pickup. Keep native state comparison separate from wire identity and
	// the continuously sampled transform.
	[[nodiscard]] inline bool same_native_profile_state(
	    protocol::PlayerProfile left,
	    protocol::PlayerProfile right)
	{
		const auto normalize = [](protocol::PlayerProfile &profile)
		{
			profile.set_player_id(0);
			profile.clear_persistent_id();
			profile.set_revision(0);
			profile.clear_display_name();
			profile.clear_level_id();
			profile.set_transform_valid(false);
			profile.clear_last_transform();
		};
		normalize(left);
		normalize(right);
		return detail::same_profile(left, right);
	}

	[[nodiscard]] inline profile_apply_result reconcile_profile(
	    profile_backend &backend,
	    const protocol::PlayerProfile &target)
	{
		profile_apply_result result;
		if (!is_valid_profile(target))
		{
			result.error = "target profile is invalid";
			return result;
		}
		std::string error;
		const auto baseline = backend.capture(error);
		if (!baseline || !is_valid_profile(*baseline))
		{
			result.error = error.empty()
			    ? "native baseline profile capture is invalid"
			    : std::move(error);
			return result;
		}
		if (detail::apply_profile_once(backend, *baseline, target, error))
		{
			const auto verified = backend.capture(error);
			if (verified && is_valid_profile(*verified)
			    && detail::same_profile(*verified, target))
			{
				result.success = true;
				return result;
			}
			if (error.empty())
				error =
				    "native post-apply capture does not match the target";
		}

		const auto apply_error = error.empty()
		    ? std::string{"native profile transaction failed"}
		    : error;
		result.rollback_attempted = true;
		error.clear();
		const auto current = backend.capture(error);
		if (current && is_valid_profile(*current)
		    && detail::apply_profile_once(
		        backend,
		        *current,
		        *baseline,
		        error))
		{
			const auto restored = backend.capture(error);
			result.rollback_succeeded =
			    restored && is_valid_profile(*restored)
			    && detail::same_profile(*restored, *baseline);
		}
		result.error = apply_error;
		if (!result.rollback_succeeded)
		{
			result.error += "; rollback failed";
			if (!error.empty())
				result.error += ": " + error;
		}
		return result;
	}
}
