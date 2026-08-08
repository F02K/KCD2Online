#include "kcse/native_world_object_sync.hpp"

#include <cassert>

int main()
{
	using namespace kcd2o::kcse;
	using namespace kcd2o::protocol;

	assert(classify_world_object(world_inventory_source::accessor, false)
	    == WORLD_OBJECT_KIND_CONTAINER);
	assert(classify_world_object(world_inventory_source::direct_stash, false)
	    == WORLD_OBJECT_KIND_CONTAINER);
	assert(classify_world_object(world_inventory_source::none, true)
	    == WORLD_OBJECT_KIND_DOOR);
	assert(classify_world_object(world_inventory_source::accessor, true)
	    == WORLD_OBJECT_KIND_CONTAINER);
	assert(classify_world_object(world_inventory_source::none, false)
	    == WORLD_OBJECT_KIND_UNSPECIFIED);

	assert(effective_world_object_opened(
	    WORLD_OBJECT_KIND_CONTAINER, false, true));
	assert(effective_world_object_opened(
	    WORLD_OBJECT_KIND_CONTAINER, true, false));
	assert(!effective_world_object_opened(
	    WORLD_OBJECT_KIND_CONTAINER, false, false));
	assert(!effective_world_object_opened(
	    WORLD_OBJECT_KIND_DOOR, false, true));
}
