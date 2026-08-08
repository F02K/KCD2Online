#include "multiplayer/protocol.hpp"
#include "property/catalog.hpp"
#include "property/service.hpp"
#include "server/world_store.hpp"

#include <zip.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

namespace
{
	constexpr std::string_view owner_id =
	    "11111111-1111-4111-8111-111111111111";
	constexpr std::string_view steward_id =
	    "22222222-2222-4222-8222-222222222222";
	constexpr std::string_view guest_id =
	    "33333333-3333-4333-8333-333333333333";

	kcd2o::protocol::PropertyCatalog catalog()
	{
		kcd2o::protocol::PropertyCatalog result;
		result.set_schema(kcd2o::property::catalog_schema);
		result.set_level_id("3");
		result.set_content_fingerprint("fixture");
		auto *property = result.add_properties();
		property->set_property_id("3:fixture");
		property->set_level_id("3");
		property->set_anchor_guid("00000001-0000-0000");
		property->set_inferred_name("Fixture house");
		property->set_source_path("main/fixture/1_home");
		property->set_discovery_confidence(1.0F);
		property->set_marker_entity_guid(1);
		property->mutable_marker_position()->set_x(10.0F);
		property->mutable_marker_position()->set_y(20.0F);
		property->mutable_marker_position()->set_z(30.0F);
		auto *door = property->add_resources();
		door->set_entity_guid(10);
		door->set_kind(kcd2o::protocol::PROPERTY_RESOURCE_KIND_DOOR);
		auto *container = property->add_resources();
		container->set_entity_guid(11);
		container->set_kind(kcd2o::protocol::PROPERTY_RESOURCE_KIND_CONTAINER);
		return result;
	}

	void write_fixture_pak(const std::filesystem::path &path)
	{
		const std::string xml = R"xml(<Objects>
<Entity Name="fixtureHub" EntityClass="SchedulerHub" EntityId="1" EntityGuid="00000001-0000-0000" Pos="10.5,20.25,30" EditorLayer="Main/test/village/1_home/_script/scheduler"><EntityLinks><Link TargetId="2" TargetGuid="00000000-0000-0000" Name="home_area" /></EntityLinks></Entity>
<Entity Name="fixtureArea" EntityClass="TriggerArea" EntityId="2" EntityGuid="00000002-0000-0000" EditorLayer="Main/test/village/1_home/_common"><EntityLinks><Link TargetId="3" TargetGuid="00000000-0000-0000" Name="crime_door[type(entrance)]" /></EntityLinks><Properties Label="private; personal; interior" /></Entity>
<Entity Name="fixtureDoor" EntityClass="AnimDoor" EntityId="3" EntityGuid="00000003-0000-0000" EditorLayer="Main/test/village/1_home/_common" />
<Entity Name="fixtureStash" EntityClass="Stash" EntityId="4" EntityGuid="00000004-0000-0000" EditorLayer="Main/test/village/1_home/_common" />
</Objects>)xml";
		auto *archive = zip_open(path.string().c_str(), 6, 'w');
		assert(archive);
		assert(zip_entry_open(archive, "objects_mission0.xml") == 0);
		assert(zip_entry_write(archive, xml.data(), xml.size()) == 0);
		assert(zip_entry_close(archive) == 0);
		zip_close(archive);
	}
}

int main()
{
	using namespace kcd2o;
	using namespace kcd2o::property;

	assert(is_uuid(server::random_uuid_v4()));
	service permissions(catalog(), {});
	std::string error;
	assert(permissions.authorize(owner_id, 10, capability::enter, 100));
	assert(permissions.system_assign_owner(
	    "3:fixture",
	    owner_id,
	    "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
	    100,
	    error));
	const auto home = permissions.home_marker_for(owner_id, 100);
	assert(home);
	assert(home->property_id() == "3:fixture");
	assert(home->role() == protocol::PROPERTY_ROLE_OWNER);
	assert(home->position().x() == 10.0F);
	assert(!permissions.home_marker_for(steward_id, 100));
	assert(permissions.authorize(owner_id, 10, capability::enter, 100));
	assert(!permissions.authorize(guest_id, 10, capability::enter, 100));
	assert(permissions.grant_role(
	    owner_id,
	    "3:fixture",
	    steward_id,
	    protocol::PROPERTY_ROLE_STEWARD,
	    "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
	    101,
	    0,
	    error));
	assert(permissions.authorize(
	    steward_id, 11, capability::use_container, 102));
	assert(permissions.grant_role(
	    steward_id,
	    "3:fixture",
	    guest_id,
	    protocol::PROPERTY_ROLE_GUEST,
	    "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
	    103,
	    200,
	    error));
	assert(permissions.authorize(guest_id, 10, capability::enter, 150));
	assert(!permissions.authorize(
	    guest_id, 11, capability::use_container, 150));
	assert(!permissions.authorize(guest_id, 10, capability::enter, 201));
	assert(permissions.revoke_role(
	    owner_id,
	    "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
	    150,
	    error));
	assert(permissions.ledger().assignments_size() == 1);

	const auto root = std::filesystem::temp_directory_path()
	    / ("kcd2o_property_" + server::random_hex(8));
	std::filesystem::create_directories(root);
	const auto pak = root / "level.pak";
	write_fixture_pak(pak);
	protocol::PropertyCatalog discovered;
	assert(scan_level_pak(pak, "3", discovered, error));
	assert(discovered.properties_size() == 1);
	assert(discovered.properties(0).resources_size() == 2);
	assert(discovered.properties(0).marker_entity_guid() == 1);
	assert(discovered.properties(0).has_marker_position());
	assert(discovered.properties(0).marker_position().x() == 10.5F);
	std::filesystem::remove_all(root);
}
