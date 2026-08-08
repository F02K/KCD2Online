#include "server/npc_registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace kcd2o::server
{
	namespace
	{
		constexpr float interest_enter_radius = 120.0F;
		constexpr float interest_leave_radius = 150.0F;
		constexpr float discovery_radius = 180.0F;
		constexpr float maximum_npc_speed = 40.0F;
		// A short burst of packet loss must not cause authority to flap between
		// nearby players. Updates renew this lease continuously.
		constexpr auto lease_duration = std::chrono::seconds(5);

		std::optional<std::uint64_t> parse_authored_guid(
		    std::string_view value)
		{
			if (value.size() != 18 || value[8] != '-' || value[13] != '-')
				return std::nullopt;
			const auto parse = [](std::string_view part)
			    -> std::optional<std::uint64_t>
			{
				std::uint64_t result{};
				const auto parsed = std::from_chars(
				    part.data(), part.data() + part.size(), result, 16);
				if (parsed.ec != std::errc{}
				    || parsed.ptr != part.data() + part.size())
					return std::nullopt;
				return result;
			};
			const auto low = parse(value.substr(0, 8));
			const auto middle = parse(value.substr(9, 4));
			const auto high = parse(value.substr(14, 4));
			if (!low || !middle || !high)
				return std::nullopt;
			const auto result = *low | (*middle << 32U) | (*high << 48U);
			return result == 0 ? std::nullopt
			                   : std::optional<std::uint64_t>{result};
		}

		bool enabled(
		    protocol::NpcKind kind,
		    bool humans_enabled,
		    bool animals_enabled)
		{
			return (kind == protocol::NPC_KIND_HUMAN && humans_enabled)
			    || (kind == protocol::NPC_KIND_ANIMAL && animals_enabled);
		}

		bool reserved_managed_actor_name(std::string_view name)
		{
			return name.starts_with("KCD2Online_Remote_")
			    || name.starts_with("KCD2Online_Dynamic_");
		}

		bool same_inventory(
		    const protocol::NpcInventoryState &left,
		    const protocol::NpcInventoryState &right)
		{
			auto a = left;
			auto b = right;
			a.set_revision(0);
			b.set_revision(0);
			return a.SerializeAsString() == b.SerializeAsString();
		}

		bool same_dialog(
		    const protocol::NpcDialogState &left,
		    const protocol::NpcDialogState &right)
		{
			auto a = left;
			auto b = right;
			a.set_revision(0);
			b.set_revision(0);
			return a.SerializeAsString() == b.SerializeAsString();
		}

		bool same_gameplay(
		    const protocol::NpcGameplayState &left,
		    const protocol::NpcGameplayState &right)
		{
			auto a = left;
			auto b = right;
			a.set_revision(0);
			b.set_revision(0);
			if (a.has_inventory())
				a.mutable_inventory()->set_revision(0);
			if (b.has_inventory())
				b.mutable_inventory()->set_revision(0);
			if (a.has_dialog())
				a.mutable_dialog()->set_revision(0);
			if (b.has_dialog())
				b.mutable_dialog()->set_revision(0);
			return a.SerializeAsString() == b.SerializeAsString();
		}
	}

	npc_registry::npc_registry(
	    std::string level_id,
	    const std::filesystem::path &catalog_path)
	{
		if (catalog_path.empty() || !std::filesystem::is_regular_file(catalog_path))
			return;
		std::ifstream input(catalog_path, std::ios::binary);
		if (!input)
			throw std::runtime_error("could not open NPC world catalog: "
			    + catalog_path.string());
		const auto document = nlohmann::json::parse(input);
		for (const auto &level : document.at("levels"))
		{
			if (level.value("level_id", std::string{}) != level_id)
				continue;
			for (const auto &npc : level.at("npcs"))
			{
				const auto guid = parse_authored_guid(
				    npc.value("entity_guid", std::string{}));
				const auto kind_name = npc.value("kind", std::string{});
				const auto kind = kind_name == "human"
				    ? protocol::NPC_KIND_HUMAN
				    : kind_name == "animal" ? protocol::NPC_KIND_ANIMAL
				                              : protocol::NPC_KIND_UNSPECIFIED;
				if (guid && is_valid_npc_kind(kind))
					m_catalog.insert_or_assign(*guid, kind);
			}
			m_catalog_required = true;
			break;
		}
	}

	void npc_registry::observe(
	    player_id reporter,
	    const protocol::ClientNpcDiscovery &message,
	    const protocol::TransformState *reporter_transform,
	    bool humans_enabled,
	    bool animals_enabled,
	    npc_time_point now)
	{
		(void)reporter;
		for (const auto &observation : message.observations())
		{
			// Managed remote players and server-created dynamic NPCs must always
			// arrive with a known canonical id. Rejecting them at discovery also
			// prevents an older/misclassifying client from recursively spawning
			// player or NPC puppets as fresh dynamic actors.
			if (observation.dynamic()
			    && reserved_managed_actor_name(observation.entity_name()))
				continue;
			const auto catalog_entry = m_catalog.find(
			    observation.authored_guid());
			const bool catalog_conflict = m_catalog_required
			    && catalog_entry != m_catalog.end()
			    && catalog_entry->second != observation.kind();
			const bool catalogued = !m_catalog_required
			    || (catalog_entry != m_catalog.end()
			        && catalog_entry->second == observation.kind());
			const bool runtime_dynamic = observation.dynamic()
			    && !catalog_conflict
			    && (m_catalog_required ? catalog_entry == m_catalog.end() : true);
			if (!enabled(observation.kind(), humans_enabled, animals_enabled)
			    || catalog_conflict || (!catalogued && !runtime_dynamic)
			    // A runtime Entity GUID is local to one game process. Treating it as
			    // a global identity makes two clients at the same location create
			    // two canonical NPCs and then spawn each other's copy. Until KCD2Online
			    // has a server-authored spawn provenance/binding protocol, only
			    // catalog-backed authored NPCs are safe to replicate.
			    || runtime_dynamic
			    || (reporter_transform
			        && distance_squared(*reporter_transform, observation.transform())
			            > discovery_radius * discovery_radius))
				continue;

			std::uint64_t npc_id = observation.known_npc_id();
			if (npc_id != 0 && !m_entries.contains(npc_id))
				continue;
			if (npc_id == 0)
				npc_id = observation.authored_guid();

			auto found = m_entries.find(npc_id);
			if (found != m_entries.end())
			{
				// A conflicting classification never replaces the first validated
				// authored observation.
				if (found->second.state.kind() != observation.kind())
					continue;
				continue;
			}

			entry created;
			created.state.set_npc_id(npc_id);
			created.state.set_generation(1);
			created.state.set_authored_guid(observation.authored_guid());
			created.state.set_kind(observation.kind());
			created.state.set_dynamic(false);
			*created.state.mutable_transform() = observation.transform();
			if (observation.has_gameplay())
				*created.state.mutable_gameplay() = observation.gameplay();
			else
			{
				auto *gameplay = created.state.mutable_gameplay();
				gameplay->set_revision(1);
				gameplay->set_health(100.0F);
				gameplay->set_max_health(100.0F);
				gameplay->set_behavior(protocol::NPC_BEHAVIOR_IDLE);
			}
			(void)normalize_rotation(created.state.mutable_transform()->mutable_rotation());
			created.state.set_revision(1);
			created.last_update = now;
			m_entries.emplace(npc_id, std::move(created));
		}
	}

	bool npc_registry::update(
	    player_id reporter,
	    const protocol::ClientNpcUpdate &message,
	    npc_time_point now)
	{
		auto found = m_entries.find(message.npc_id());
		if (found == m_entries.end())
			return false;
		auto &entry = found->second;
		if (entry.state.generation() != message.generation()
		    || entry.state.authority_player_id() != reporter
		    || entry.state.lease_id() != message.lease_id()
		    || now >= entry.lease_expires)
			return false;

		auto transform = message.transform();
		if (!normalize_rotation(transform.mutable_rotation()))
			return false;
		const auto elapsed = std::chrono::duration<float>(
		    now - entry.last_update).count();
		if (elapsed > 0.0F)
		{
			const auto allowed = maximum_npc_speed * elapsed + 2.0F;
			if (distance_squared(entry.state.transform(), transform)
			    > allowed * allowed)
				return false;
		}
		transform.set_sequence(entry.state.transform().sequence() + 1);
		*entry.state.mutable_transform() = std::move(transform);
		auto gameplay = message.has_gameplay()
		    ? message.gameplay() : entry.state.gameplay();
		const auto previous_gameplay = entry.state.gameplay();
		gameplay.clear_aggro();
		for (const auto &aggro : previous_gameplay.aggro())
			*gameplay.add_aggro() = aggro;
		gameplay.set_combat_target_player_id(
		    previous_gameplay.combat_target_player_id());
		if (previous_gameplay.has_last_combat_result())
			*gameplay.mutable_last_combat_result() =
			    previous_gameplay.last_combat_result();
		if (gameplay.has_inventory())
		{
			const auto changed = !previous_gameplay.has_inventory()
			    || !same_inventory(
			        gameplay.inventory(), previous_gameplay.inventory());
			gameplay.mutable_inventory()->set_revision(changed
			        ? (previous_gameplay.has_inventory()
			              ? previous_gameplay.inventory().revision() + 1 : 1)
		        : previous_gameplay.inventory().revision());
		}
		else if (previous_gameplay.has_inventory())
			*gameplay.mutable_inventory() = previous_gameplay.inventory();
		if (gameplay.has_dialog())
		{
			const auto changed = !previous_gameplay.has_dialog()
			    || !same_dialog(gameplay.dialog(), previous_gameplay.dialog());
			gameplay.mutable_dialog()->set_revision(changed
			        ? (previous_gameplay.has_dialog()
			              ? previous_gameplay.dialog().revision() + 1 : 1)
		        : previous_gameplay.dialog().revision());
		}
		else if (previous_gameplay.has_dialog()
		    && previous_gameplay.dialog().active())
		{
			*gameplay.mutable_dialog() = previous_gameplay.dialog();
			gameplay.mutable_dialog()->set_revision(
			    previous_gameplay.dialog().revision() + 1);
			gameplay.mutable_dialog()->set_active(false);
		}
		else if (previous_gameplay.has_dialog())
			*gameplay.mutable_dialog() = previous_gameplay.dialog();
		const auto previous_health = entry.state.gameplay().health();
		if (gameplay.health() + 0.001F < previous_health)
		{
			const auto damage = previous_health - gameplay.health();
			auto *result = gameplay.mutable_last_combat_result();
			const auto previous_event =
			    entry.state.gameplay().has_last_combat_result()
			    ? entry.state.gameplay().last_combat_result().event_id()
			    : 0;
			result->set_event_id(previous_event + 1);
			result->set_attacker_player_id(reporter);
			result->set_health_damage(damage);
			result->set_stamina_damage(0.0F);
			result->set_fatal(gameplay.dead());
			gameplay.set_combat_target_player_id(reporter);
			gameplay.set_behavior(
			    gameplay.dead() ? protocol::NPC_BEHAVIOR_DEAD
			                    : protocol::NPC_BEHAVIOR_COMBAT);
			bool found_aggro{};
			for (auto &aggro : *gameplay.mutable_aggro())
			{
				if (aggro.player_id() != reporter)
					continue;
				aggro.set_value(std::min(1'000'000.0F, aggro.value() + damage));
				found_aggro = true;
				break;
			}
			if (!found_aggro)
			{
				auto *aggro = gameplay.add_aggro();
				aggro->set_player_id(reporter);
				aggro->set_value(damage);
			}
		}
		else if (gameplay.behavior() != protocol::NPC_BEHAVIOR_COMBAT)
		{
			const auto decay = std::max(0.0F, elapsed) * 5.0F;
			for (int index = gameplay.aggro_size() - 1; index >= 0; --index)
			{
				auto *aggro = gameplay.mutable_aggro(index);
				aggro->set_value(std::max(0.0F, aggro->value() - decay));
				if (aggro->value() <= 0.001F)
					gameplay.mutable_aggro()->DeleteSubrange(index, 1);
			}
			if (gameplay.aggro().empty())
				gameplay.set_combat_target_player_id(0);
		}
		const bool gameplay_changed = !same_gameplay(
		    gameplay, previous_gameplay);
		gameplay.set_revision(gameplay_changed
		        ? previous_gameplay.revision() + 1
		        : previous_gameplay.revision());
		*entry.state.mutable_gameplay() = std::move(gameplay);
		entry.state.set_revision(entry.state.revision() + 1);
		entry.last_update = now;
		entry.lease_expires = now + lease_duration;
		return true;
	}

	std::vector<npc_registry::event> npc_registry::reconcile(
	    std::span<const player_position> players,
	    npc_time_point now)
	{
		std::vector<std::pair<player_id, std::uint64_t>> entered;
		std::vector<event> result;
		std::unordered_set<player_id> live;
		for (const auto &player : players)
		{
			if (!player.connected)
				continue;
			live.insert(player.id);
			auto &interest = m_interest[player.id];
			for (const auto &[npc_id, entry] : m_entries)
			{
				const bool current = interest.contains(npc_id);
				const auto radius = current
				    ? interest_leave_radius
				    : interest_enter_radius;
				const bool desired = player.transform
				    && distance_squared(*player.transform, entry.state.transform())
				        <= radius * radius;
				if (desired && !current)
				{
					interest.insert(npc_id);
					entered.emplace_back(player.id, npc_id);
				}
				else if (!desired && current)
				{
					interest.erase(npc_id);
					result.push_back({event_kind::leave, player.id, entry.state});
				}
			}
		}
		for (auto iterator = m_interest.begin(); iterator != m_interest.end();)
			iterator = live.contains(iterator->first)
			    ? std::next(iterator)
			    : m_interest.erase(iterator);

		auto authority_events = assign_authorities(players, now);
		result.insert(
		    result.end(),
		    std::make_move_iterator(authority_events.begin()),
		    std::make_move_iterator(authority_events.end()));
		for (const auto &[recipient, npc_id] : entered)
		{
			if (const auto found = m_entries.find(npc_id);
			    found != m_entries.end())
				result.push_back({event_kind::enter, recipient, found->second.state});
		}
		return result;
	}

	std::vector<npc_registry::event> npc_registry::remove_player(
	    player_id id,
	    std::span<const player_position> players,
	    npc_time_point now)
	{
		m_interest.erase(id);
		for (auto &[npc_id, entry] : m_entries)
		{
			(void)npc_id;
			if (entry.state.authority_player_id() == id)
				entry.lease_expires = npc_time_point{};
		}
		return assign_authorities(players, now);
	}

	std::vector<npc_registry::event> npc_registry::disable_kind(
	    protocol::NpcKind kind)
	{
		std::vector<event> result;
		for (auto iterator = m_entries.begin(); iterator != m_entries.end();)
		{
			if (iterator->second.state.kind() != kind)
			{
				++iterator;
				continue;
			}
			for (auto &[recipient, interest] : m_interest)
			{
				if (interest.erase(iterator->first) != 0)
					result.push_back(
					    {event_kind::leave, recipient, iterator->second.state});
			}
			iterator = m_entries.erase(iterator);
		}
		return result;
	}

	std::vector<protocol::NpcState> npc_registry::states_for(player_id id) const
	{
		std::vector<protocol::NpcState> result;
		const auto interest = m_interest.find(id);
		if (interest == m_interest.end())
			return result;
		result.reserve(interest->second.size());
		for (const auto npc_id : interest->second)
		{
			if (const auto found = m_entries.find(npc_id);
			    found != m_entries.end())
				result.push_back(found->second.state);
		}
		std::ranges::sort(result, {}, &protocol::NpcState::npc_id);
		return result;
	}

	std::size_t npc_registry::size() const noexcept
	{
		return m_entries.size();
	}

	float npc_registry::distance_squared(
	    const protocol::TransformState &left,
	    const protocol::TransformState &right)
	{
		const auto x = left.position().x() - right.position().x();
		const auto y = left.position().y() - right.position().y();
		const auto z = left.position().z() - right.position().z();
		return x * x + y * y + z * z;
	}

	std::vector<npc_registry::event> npc_registry::assign_authorities(
	    std::span<const player_position> players,
	    npc_time_point now)
	{
		std::vector<event> result;
		for (auto &[npc_id, entry] : m_entries)
		{
			const auto current = entry.state.authority_player_id();
			const bool valid_current = current != 0
			    && now < entry.lease_expires
			    && m_interest.contains(current)
			    && m_interest.at(current).contains(npc_id);
			if (valid_current)
				continue;

			player_id selected{};
			float best = std::numeric_limits<float>::max();
			for (const auto &player : players)
			{
				if (!player.connected || !player.transform)
					continue;
				const auto interest = m_interest.find(player.id);
				if (interest == m_interest.end()
				    || !interest->second.contains(npc_id))
					continue;
				const auto distance = distance_squared(
				    *player.transform, entry.state.transform());
				if (distance < best || (distance == best && player.id < selected))
				{
					best = distance;
					selected = player.id;
				}
			}

			entry.state.set_authority_player_id(selected);
			if (selected == 0)
			{
				entry.state.set_lease_id(0);
				entry.lease_expires = {};
			}
			else
			{
				if (++m_next_lease_id == 0)
					++m_next_lease_id;
				entry.state.set_lease_id(m_next_lease_id);
				entry.lease_expires = now + lease_duration;
				entry.last_update = now;
			}
			for (const auto &[recipient, interest] : m_interest)
			{
				if (interest.contains(npc_id))
					result.push_back(
					    {event_kind::authority, recipient, entry.state});
			}
		}
		return result;
	}
}
