#include "server/npc_registry.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#undef assert
#define assert(expression)                                                   \
	do                                                                       \
	{                                                                        \
		if (!(expression))                                                   \
		{                                                                    \
			std::cerr << "assertion failed at " << __FILE__ << ':' << __LINE__ \
			          << ": " #expression << '\n';                          \
			std::_Exit(1);                                                   \
		}                                                                    \
	} while (false)

namespace
{
	using namespace kcd2o;
	using namespace kcd2o::server;
	using namespace std::chrono_literals;

	protocol::TransformState transform(float x)
	{
		protocol::TransformState result;
		result.mutable_position()->set_x(x);
		result.mutable_rotation()->set_w(1.0F);
		result.mutable_velocity();
		return result;
	}

	void set_gameplay(protocol::NpcObservation &observation, float health)
	{
		auto *gameplay = observation.mutable_gameplay();
		gameplay->set_revision(1);
		gameplay->set_health(health);
		gameplay->set_max_health(100.0F);
		gameplay->set_behavior(protocol::NPC_BEHAVIOR_IDLE);
	}
}

int main()
{
	using namespace kcd2o;
	using namespace kcd2o::server;
	using namespace std::chrono_literals;

	npc_registry registry;
	const auto start = std::chrono::steady_clock::now();
	auto first_position = transform(0.0F);
	auto second_position = transform(8.0F);
	std::vector<npc_registry::player_position> players{
	    {1, &first_position, true},
	    {2, &second_position, true}};

	protocol::ClientNpcDiscovery discovery;
	auto *human = discovery.add_observations();
	human->set_authored_guid(0x1234);
	human->set_kind(protocol::NPC_KIND_HUMAN);
	*human->mutable_transform() = transform(10.0F);
	set_gameplay(*human, 100.0F);
	registry.observe(1, discovery, &first_position, true, true, start);
	assert(registry.size() == 1);

	const auto events = registry.reconcile(players, start);
	assert(!events.empty());
	const auto first_states = registry.states_for(1);
	const auto second_states = registry.states_for(2);
	assert(first_states.size() == 1);
	assert(second_states.size() == 1);
	assert(first_states.front().npc_id() == 0x1234);
	assert(first_states.front().authority_player_id() == 2);
	assert(first_states.front().lease_id() != 0);

	protocol::ClientNpcUpdate update;
	update.set_npc_id(0x1234);
	update.set_generation(1);
	update.set_lease_id(first_states.front().lease_id());
	*update.mutable_transform() = transform(11.0F);
	assert(!registry.update(1, update, start + 100ms));
	assert(registry.update(2, update, start + 100ms));
	assert(registry.states_for(1).front().revision() == 2);

	protocol::ClientNpcUpdate damage = update;
	*damage.mutable_transform() = transform(12.0F);
	auto *damaged = damage.mutable_gameplay();
	damaged->set_revision(2);
	damaged->set_health(75.0F);
	damaged->set_max_health(100.0F);
	damaged->set_behavior(protocol::NPC_BEHAVIOR_COMBAT);
	auto *inventory = damaged->mutable_inventory();
	inventory->set_revision(99);
	auto *item = inventory->add_items();
	item->set_instance_id("11111111-1111-4111-8111-111111111111");
	item->set_definition_id("22222222-2222-4222-8222-222222222222");
	item->set_count(2);
	item->set_quality(1.0F);
	item->set_condition(1.0F);
	assert(registry.update(2, damage, start + 150ms));
	const auto after_damage = registry.states_for(1).front().gameplay();
	assert(after_damage.health() == 75.0F);
	assert(after_damage.combat_target_player_id() == 2);
	assert(after_damage.aggro_size() == 1);
	assert(after_damage.aggro(0).player_id() == 2);
	assert(after_damage.aggro(0).value() == 25.0F);
	assert(after_damage.last_combat_result().attacker_player_id() == 2);
	assert(after_damage.last_combat_result().health_damage() == 25.0F);
	assert(after_damage.inventory().revision() == 1);
	assert(after_damage.inventory().items_size() == 1);
	const auto gameplay_revision = after_damage.revision();

	protocol::ClientNpcUpdate transform_only;
	transform_only.set_npc_id(0x1234);
	transform_only.set_generation(1);
	transform_only.set_lease_id(first_states.front().lease_id());
	*transform_only.mutable_transform() = transform(12.5F);
	assert(registry.update(2, transform_only, start + 175ms));
	const auto after_transform_only = registry.states_for(1).front().gameplay();
	assert(after_transform_only.revision() == gameplay_revision);
	assert(after_transform_only.inventory().revision() == 1);
	assert(after_transform_only.inventory().items_size() == 1);

	players[1].connected = false;
	const auto handoff = registry.remove_player(2, players, start + 200ms);
	assert(!handoff.empty());
	assert(registry.states_for(1).front().authority_player_id() == 1);
	assert(registry.states_for(1).front().lease_id()
	    != first_states.front().lease_id());

	first_position = transform(400.0F);
	const auto leaves = registry.reconcile(players, start + 300ms);
	assert(std::ranges::any_of(
	    leaves,
	    [](const npc_registry::event &event)
	    { return event.kind == npc_registry::event_kind::leave; }));
	assert(registry.states_for(1).empty());

	protocol::ClientNpcDiscovery animal_discovery;
	auto *animal = animal_discovery.add_observations();
	animal->set_authored_guid(0x5678);
	animal->set_kind(protocol::NPC_KIND_ANIMAL);
	*animal->mutable_transform() = transform(400.0F);
	set_gameplay(*animal, 100.0F);
	registry.observe(
	    1, animal_discovery, &first_position, true, false, start + 400ms);
	assert(registry.size() == 1);

	const auto disabled = registry.disable_kind(protocol::NPC_KIND_HUMAN);
	assert(disabled.empty());
	assert(registry.size() == 0);

	const auto catalog_path = std::filesystem::temp_directory_path()
	    / "kcd2o-npc-registry-catalog.json";
	{
		std::ofstream catalog(catalog_path, std::ios::binary | std::ios::trunc);
		catalog << R"({"levels":[{"level_id":"2","npcs":[{"entity_guid":"12345678-9abc-def0","kind":"human","entity_class":"NPC","name":"catalogued","position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0]}]}]})";
	}
	npc_registry catalogued("2", catalog_path);
	assert(catalogued.size() == 0); // catalog is an identity allowlist, not live state
	first_position = transform(0.0F);
	protocol::ClientNpcDiscovery catalog_discovery;
	auto *catalog_human = catalog_discovery.add_observations();
	catalog_human->set_authored_guid(0xDEF09ABC12345678ULL);
	catalog_human->set_kind(protocol::NPC_KIND_HUMAN);
	*catalog_human->mutable_transform() = transform(0.0F);
	set_gameplay(*catalog_human, 100.0F);
	catalogued.observe(
	    1, catalog_discovery, &first_position, true, true, start + 500ms);
	assert(catalogued.size() == 1);
	catalogued.observe(
	    1, catalog_discovery, &first_position, true, true, start + 600ms);
	assert(catalogued.size() == 1);
	protocol::ClientNpcDiscovery conflicting_catalog_entry;
	auto *conflict = conflicting_catalog_entry.add_observations();
	conflict->set_authored_guid(0xDEF09ABC12345678ULL);
	conflict->set_kind(protocol::NPC_KIND_ANIMAL);
	conflict->set_dynamic(true);
	conflict->set_entity_class("Animal");
	conflict->set_entity_name("spoofed_catalog_kind");
	*conflict->mutable_transform() = transform(0.0F);
	set_gameplay(*conflict, 100.0F);
	catalogued.observe(
	    1, conflicting_catalog_entry, &first_position, true, true, start + 650ms);
	assert(catalogued.size() == 1);

	protocol::ClientNpcDiscovery dynamic_discovery;
	auto *dynamic = dynamic_discovery.add_observations();
	dynamic->set_authored_guid(0xBEEF);
	dynamic->set_kind(protocol::NPC_KIND_HUMAN);
	dynamic->set_dynamic(true);
	dynamic->set_entity_class("NPC");
	dynamic->set_entity_name("runtime_bandit");
	*dynamic->mutable_transform() = transform(5.0F);
	set_gameplay(*dynamic, 80.0F);
	catalogued.observe(
	    1, dynamic_discovery, &first_position, true, true, start + 700ms);
	assert(catalogued.size() == 1);
	// Runtime GUIDs are client-local and cannot safely identify a shared NPC.
	// A second reporter must not turn either observation into a replicated spawn.
	catalogued.observe(
	    2, dynamic_discovery, &first_position, true, true, start + 701ms);
	assert(catalogued.size() == 1);
	protocol::ClientNpcDiscovery recursive_managed_discovery;
	auto *recursive = recursive_managed_discovery.add_observations();
	recursive->set_authored_guid(0xCAFE);
	recursive->set_kind(protocol::NPC_KIND_HUMAN);
	recursive->set_dynamic(true);
	recursive->set_entity_class("NPC");
	recursive->set_entity_name("KCD2Online_Remote_1_2_3");
	*recursive->mutable_transform() = transform(5.0F);
	set_gameplay(*recursive, 100.0F);
	catalogued.observe(
	    1,
	    recursive_managed_discovery,
	    &first_position,
	    true,
	    true,
	    start + 702ms);
	assert(catalogued.size() == 1);
	std::vector<npc_registry::player_position> dynamic_players{
	    {1, &first_position, true}};
	(void)catalogued.reconcile(dynamic_players, start + 700ms);
	const auto dynamic_states = catalogued.states_for(1);
	assert(std::ranges::none_of(
	    dynamic_states,
	    [](const protocol::NpcState &state) { return state.dynamic(); }));
	std::error_code ignored;
	std::filesystem::remove(catalog_path, ignored);
	return 0;
}
