#include "property/service.hpp"

#include "multiplayer/protocol.hpp"

#include <algorithm>
#include <ranges>
#include <unordered_set>

namespace kcd2o::property
{
	namespace
	{
		bool time_active(
		    const protocol::PropertyRoleAssignment &assignment,
		    std::uint64_t now_ms)
		{
			return assignment.expires_at_ms() == 0 || now_ms == 0
			    || now_ms < assignment.expires_at_ms();
		}

		int authority(protocol::PropertyRole role)
		{
			switch (role)
			{
			case protocol::PROPERTY_ROLE_OWNER:
				return 6;
			case protocol::PROPERTY_ROLE_STEWARD:
				return 5;
			case protocol::PROPERTY_ROLE_GUARD:
				return 4;
			case protocol::PROPERTY_ROLE_RESIDENT:
				return 3;
			case protocol::PROPERTY_ROLE_EMPLOYEE:
				return 2;
			case protocol::PROPERTY_ROLE_GUEST:
				return 1;
			default:
				return 0;
			}
		}
	}

	service::service(
	    protocol::PropertyCatalog catalog,
	    protocol::PropertyLedger ledger)
	{
		reset(std::move(catalog), std::move(ledger));
	}

	void service::reset(
	    protocol::PropertyCatalog catalog,
	    protocol::PropertyLedger ledger)
	{
		m_catalog = std::move(catalog);
		m_ledger = std::move(ledger);
		if (m_ledger.schema() == 0)
			m_ledger.set_schema(ledger_schema);
		if (m_ledger.revision() == 0)
			m_ledger.set_revision(1);
		rebuild_indexes();
	}

	const protocol::PropertyCatalog &service::catalog() const
	{
		return m_catalog;
	}

	const protocol::PropertyLedger &service::ledger() const
	{
		return m_ledger;
	}

	const protocol::PropertyDefinition *service::find_property(
	    std::string_view property_id) const
	{
		const auto found = m_properties.find(std::string(property_id));
		return found == m_properties.end() ? nullptr : found->second;
	}

	const protocol::PropertyDefinition *service::property_for(
	    std::uint64_t entity_guid) const
	{
		const auto found = m_resources.find(entity_guid);
		return found == m_resources.end() ? nullptr : found->second;
	}

	std::optional<protocol::PropertyHomeMarker> service::home_marker_for(
	    std::string_view player_persistent_id,
	    std::uint64_t now_ms) const
	{
		const protocol::PropertyRoleAssignment *best{};
		const protocol::PropertyDefinition *best_property{};
		const auto home_rank = [](protocol::PropertyRole role)
		{
			if (role == protocol::PROPERTY_ROLE_OWNER)
				return 2;
			if (role == protocol::PROPERTY_ROLE_RESIDENT)
				return 1;
			return 0;
		};
		for (const auto &assignment : m_ledger.assignments())
		{
			const auto rank = home_rank(assignment.role());
			if (rank == 0
			    || assignment.subject_player_id() != player_persistent_id
			    || !assignment_active(assignment, now_ms))
				continue;
			const auto *definition = find_property(assignment.property_id());
			if (!definition || !definition->has_marker_position()
			    || definition->marker_entity_guid() == 0)
				continue;
			if (!best || rank > home_rank(best->role())
			    || (rank == home_rank(best->role())
			        && definition->property_id() < best_property->property_id()))
			{
				best = &assignment;
				best_property = definition;
			}
		}
		if (!best || !best_property)
			return std::nullopt;

		protocol::PropertyHomeMarker marker;
		marker.set_property_id(best_property->property_id());
		marker.set_level_id(best_property->level_id());
		marker.set_display_name(best_property->inferred_name());
		*marker.mutable_position() = best_property->marker_position();
		marker.set_entity_guid(best_property->marker_entity_guid());
		marker.set_role(best->role());
		return marker;
	}

	bool service::authorize(
	    std::string_view player_persistent_id,
	    std::uint64_t entity_guid,
	    capability requested,
	    std::uint64_t now_ms) const
	{
		const auto *property = property_for(entity_guid);
		if (!property)
			return true;
		const bool has_owner = std::ranges::any_of(
		    m_ledger.assignments(),
		    [&](const protocol::PropertyRoleAssignment &assignment)
		    {
			    return assignment.property_id() == property->property_id()
			        && assignment.role() == protocol::PROPERTY_ROLE_OWNER
		        && assignment_active(assignment, now_ms);
		    });
		// Discovered but unclaimed properties keep vanilla behavior. This avoids
		// locking an entire level before RP administrators distribute titles.
		if (!has_owner)
			return true;
		return has_capability(
		    player_persistent_id, property->property_id(), requested, now_ms);
	}

	bool service::system_assign_owner(
	    std::string_view property_id,
	    std::string_view target_player_id,
	    std::string assignment_id,
	    std::uint64_t now_ms,
	    std::string &error)
	{
		if (!find_property(property_id) || !is_uuid(target_player_id)
		    || !is_uuid(assignment_id))
		{
			error = "property, player identity, or assignment identity is invalid";
			return false;
		}
		if (std::ranges::any_of(
		        m_ledger.assignments(),
		        [&](const protocol::PropertyRoleAssignment &assignment)
		        {
			        return assignment.property_id() == property_id
			            && assignment.role() == protocol::PROPERTY_ROLE_OWNER
			            && assignment_active(assignment, now_ms);
		        }))
		{
			error = "property already has an owner";
			return false;
		}
		auto *assignment = m_ledger.add_assignments();
		assignment->set_assignment_id(std::move(assignment_id));
		assignment->set_property_id(property_id);
		assignment->set_subject_player_id(target_player_id);
		assignment->set_role(protocol::PROPERTY_ROLE_OWNER);
		assignment->set_created_at_ms(now_ms);
		m_ledger.set_revision(m_ledger.revision() + 1);
		error.clear();
		return true;
	}

	bool service::grant_role(
	    std::string_view actor_player_id,
	    std::string_view property_id,
	    std::string_view target_player_id,
	    protocol::PropertyRole role,
	    std::string assignment_id,
	    std::uint64_t now_ms,
	    std::uint64_t expires_at_ms,
	    std::string &error)
	{
		if (!find_property(property_id) || !is_uuid(actor_player_id)
		    || !is_uuid(target_player_id) || !is_uuid(assignment_id)
		    || role == protocol::PROPERTY_ROLE_UNSPECIFIED
		    || role == protocol::PROPERTY_ROLE_OWNER
		    || (expires_at_ms != 0 && expires_at_ms <= now_ms))
		{
			error = "property role grant is invalid";
			return false;
		}
		const auto *actor = highest_assignment(
		    actor_player_id, property_id, now_ms);
		if (!actor || !role_has(actor->role(), capability::manage_roles)
		    || !may_grant(actor->role(), role))
		{
			error = "granting player is not allowed to assign this role";
			return false;
		}
		if (std::ranges::any_of(
		        m_ledger.assignments(),
		        [&](const protocol::PropertyRoleAssignment &assignment)
		        {
			        return assignment.property_id() == property_id
			            && assignment.subject_player_id() == target_player_id
			            && assignment.role() == role
			            && assignment_active(assignment, now_ms);
		        }))
		{
			error = "player already has this property role";
			return false;
		}
		auto *assignment = m_ledger.add_assignments();
		assignment->set_assignment_id(std::move(assignment_id));
		assignment->set_property_id(property_id);
		assignment->set_subject_player_id(target_player_id);
		assignment->set_role(role);
		assignment->set_granted_by_player_id(actor_player_id);
		assignment->set_granted_by_assignment_id(actor->assignment_id());
		assignment->set_created_at_ms(now_ms);
		assignment->set_expires_at_ms(expires_at_ms);
		m_ledger.set_revision(m_ledger.revision() + 1);
		error.clear();
		return true;
	}

	bool service::revoke_role(
	    std::string_view actor_player_id,
	    std::string_view assignment_id,
	    std::uint64_t now_ms,
	    std::string &error)
	{
		const auto found = std::ranges::find_if(
		    *m_ledger.mutable_assignments(),
		    [&](const protocol::PropertyRoleAssignment &assignment)
		    { return assignment.assignment_id() == assignment_id; });
		if (found == m_ledger.mutable_assignments()->end())
		{
			error = "property role assignment does not exist";
			return false;
		}
		const auto *actor = highest_assignment(
		    actor_player_id, found->property_id(), now_ms);
		if (!actor || !role_has(actor->role(), capability::manage_roles)
		    || authority(actor->role()) <= authority(found->role()))
		{
			error = "revoking player is not above the target role";
			return false;
		}
		std::unordered_set<std::string> removed{found->assignment_id()};
		bool changed = true;
		while (changed)
		{
			changed = false;
			for (const auto &assignment : m_ledger.assignments())
			{
				if (!assignment.granted_by_assignment_id().empty()
				    && removed.contains(assignment.granted_by_assignment_id())
				    && removed.insert(assignment.assignment_id()).second)
					changed = true;
			}
		}
		for (auto index = m_ledger.assignments_size(); index-- > 0;)
		{
			if (removed.contains(m_ledger.assignments(index).assignment_id()))
				m_ledger.mutable_assignments()->DeleteSubrange(index, 1);
		}
		m_ledger.set_revision(m_ledger.revision() + 1);
		error.clear();
		return true;
	}

	bool service::system_revoke_role(
	    std::string_view assignment_id,
	    std::string &error)
	{
		const auto found = std::ranges::find_if(
		    *m_ledger.mutable_assignments(),
		    [&](const protocol::PropertyRoleAssignment &assignment)
		    { return assignment.assignment_id() == assignment_id; });
		if (found == m_ledger.mutable_assignments()->end())
		{
			error = "property role assignment does not exist";
			return false;
		}
		std::unordered_set<std::string> removed{found->assignment_id()};
		bool changed = true;
		while (changed)
		{
			changed = false;
			for (const auto &assignment : m_ledger.assignments())
			{
				if (!assignment.granted_by_assignment_id().empty()
				    && removed.contains(assignment.granted_by_assignment_id())
				    && removed.insert(assignment.assignment_id()).second)
					changed = true;
			}
		}
		for (auto index = m_ledger.assignments_size(); index-- > 0;)
		{
			if (removed.contains(m_ledger.assignments(index).assignment_id()))
				m_ledger.mutable_assignments()->DeleteSubrange(index, 1);
		}
		m_ledger.set_revision(m_ledger.revision() + 1);
		error.clear();
		return true;
	}

	bool service::has_capability(
	    std::string_view player_persistent_id,
	    std::string_view property_id,
	    capability requested,
	    std::uint64_t now_ms) const
	{
		std::unordered_set<std::string> visited;
		std::string current(property_id);
		while (!current.empty() && visited.insert(current).second)
		{
			if (const auto *assignment = highest_assignment(
			        player_persistent_id, current, now_ms);
			    assignment && role_has(assignment->role(), requested))
				return true;
			const auto *property = find_property(current);
			current = property ? property->parent_property_id() : std::string{};
		}
		return false;
	}

	const protocol::PropertyRoleAssignment *service::highest_assignment(
	    std::string_view player_persistent_id,
	    std::string_view property_id,
	    std::uint64_t now_ms) const
	{
		const protocol::PropertyRoleAssignment *result{};
		for (const auto &assignment : m_ledger.assignments())
		{
			if (assignment.property_id() != property_id
			    || assignment.subject_player_id() != player_persistent_id
			    || !assignment_active(assignment, now_ms))
				continue;
			if (!result || authority(assignment.role()) > authority(result->role()))
				result = &assignment;
		}
		return result;
	}

	bool service::assignment_active(
	    const protocol::PropertyRoleAssignment &assignment,
	    std::uint64_t now_ms) const
	{
		std::unordered_set<std::string> visited;
		return assignment_active(assignment, now_ms, visited);
	}

	bool service::assignment_active(
	    const protocol::PropertyRoleAssignment &assignment,
	    std::uint64_t now_ms,
	    std::unordered_set<std::string> &visited) const
	{
		if (!time_active(assignment, now_ms)
		    || !visited.insert(assignment.assignment_id()).second)
			return false;
		if (assignment.granted_by_assignment_id().empty())
			return assignment.role() == protocol::PROPERTY_ROLE_OWNER;
		const auto parent = std::ranges::find_if(
		    m_ledger.assignments(),
		    [&](const protocol::PropertyRoleAssignment &candidate)
		    {
			    return candidate.assignment_id()
			        == assignment.granted_by_assignment_id();
		    });
		return parent != m_ledger.assignments().end()
		    && parent->property_id() == assignment.property_id()
		    && parent->subject_player_id()
		        == assignment.granted_by_player_id()
		    && may_grant(parent->role(), assignment.role())
		    && assignment_active(*parent, now_ms, visited);
	}

	bool service::role_has(
	    protocol::PropertyRole role,
	    capability requested)
	{
		if (role == protocol::PROPERTY_ROLE_OWNER)
			return true;
		if (role == protocol::PROPERTY_ROLE_STEWARD)
			return requested != capability::transfer_title;
		if (role == protocol::PROPERTY_ROLE_RESIDENT)
			return requested == capability::enter
			    || requested == capability::use_container
			    || requested == capability::use_bed
			    || requested == capability::use_workstation;
		if (role == protocol::PROPERTY_ROLE_EMPLOYEE)
			return requested == capability::enter
			    || requested == capability::use_container
			    || requested == capability::use_workstation;
		if (role == protocol::PROPERTY_ROLE_GUARD)
			return requested == capability::enter;
		if (role == protocol::PROPERTY_ROLE_GUEST)
			return requested == capability::enter;
		return false;
	}

	bool service::may_grant(
	    protocol::PropertyRole actor,
	    protocol::PropertyRole target)
	{
		if (actor == protocol::PROPERTY_ROLE_OWNER)
			return target != protocol::PROPERTY_ROLE_OWNER
			    && target != protocol::PROPERTY_ROLE_UNSPECIFIED;
		if (actor == protocol::PROPERTY_ROLE_STEWARD)
			return target == protocol::PROPERTY_ROLE_RESIDENT
			    || target == protocol::PROPERTY_ROLE_EMPLOYEE
			    || target == protocol::PROPERTY_ROLE_GUARD
			    || target == protocol::PROPERTY_ROLE_GUEST;
		return false;
	}

	void service::rebuild_indexes()
	{
		m_properties.clear();
		m_resources.clear();
		for (const auto &property : m_catalog.properties())
		{
			m_properties.emplace(property.property_id(), &property);
			for (const auto &resource : property.resources())
				m_resources.emplace(resource.entity_guid(), &property);
		}
	}
}
