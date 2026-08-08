#pragma once

#include "multiplayer/protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace kcd2o::property
{
	inline constexpr std::uint32_t ledger_schema = 1;

	enum class capability
	{
		enter,
		use_container,
		use_bed,
		use_workstation,
		manage_roles,
		transfer_title
	};

	class service
	{
	public:
		service() = default;
		service(
		    protocol::PropertyCatalog catalog,
		    protocol::PropertyLedger ledger);

		void reset(
		    protocol::PropertyCatalog catalog,
		    protocol::PropertyLedger ledger);

		[[nodiscard]] const protocol::PropertyCatalog &catalog() const;
		[[nodiscard]] const protocol::PropertyLedger &ledger() const;
		[[nodiscard]] const protocol::PropertyDefinition *find_property(
		    std::string_view property_id) const;
		[[nodiscard]] const protocol::PropertyDefinition *property_for(
		    std::uint64_t entity_guid) const;
		[[nodiscard]] std::optional<protocol::PropertyHomeMarker> home_marker_for(
		    std::string_view player_persistent_id,
		    std::uint64_t now_ms = 0) const;
		[[nodiscard]] bool authorize(
		    std::string_view player_persistent_id,
		    std::uint64_t entity_guid,
		    capability requested,
		    std::uint64_t now_ms = 0) const;

		[[nodiscard]] bool system_assign_owner(
		    std::string_view property_id,
		    std::string_view target_player_id,
		    std::string assignment_id,
		    std::uint64_t now_ms,
		    std::string &error);
		[[nodiscard]] bool grant_role(
		    std::string_view actor_player_id,
		    std::string_view property_id,
		    std::string_view target_player_id,
		    protocol::PropertyRole role,
		    std::string assignment_id,
		    std::uint64_t now_ms,
		    std::uint64_t expires_at_ms,
		    std::string &error);
		[[nodiscard]] bool revoke_role(
		    std::string_view actor_player_id,
		    std::string_view assignment_id,
		    std::uint64_t now_ms,
		    std::string &error);
		[[nodiscard]] bool system_revoke_role(
		    std::string_view assignment_id,
		    std::string &error);

	private:
		[[nodiscard]] bool has_capability(
		    std::string_view player_persistent_id,
		    std::string_view property_id,
		    capability requested,
		    std::uint64_t now_ms) const;
		[[nodiscard]] const protocol::PropertyRoleAssignment *highest_assignment(
		    std::string_view player_persistent_id,
		    std::string_view property_id,
		    std::uint64_t now_ms) const;
		[[nodiscard]] bool assignment_active(
		    const protocol::PropertyRoleAssignment &assignment,
		    std::uint64_t now_ms) const;
		[[nodiscard]] bool assignment_active(
		    const protocol::PropertyRoleAssignment &assignment,
		    std::uint64_t now_ms,
		    std::unordered_set<std::string> &visited) const;
		[[nodiscard]] static bool role_has(
		    protocol::PropertyRole role,
		    capability requested);
		[[nodiscard]] static bool may_grant(
		    protocol::PropertyRole actor,
		    protocol::PropertyRole target);
		void rebuild_indexes();

		protocol::PropertyCatalog m_catalog;
		protocol::PropertyLedger m_ledger;
		std::unordered_map<std::string, const protocol::PropertyDefinition *>
		    m_properties;
		std::unordered_map<std::uint64_t, const protocol::PropertyDefinition *>
		    m_resources;
	};
}
