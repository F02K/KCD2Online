#include "multiplayer/runtime_handle.hpp"

#include <cassert>

int main()
{
	kcd2o::runtime_handle_registry handles(7);
	const auto first = handles.allocate();
	assert(first.epoch == 7);
	assert(handles.valid(first));
	assert(handles.release(first));
	assert(!handles.valid(first));
	assert(!handles.release(first));

	const auto replacement = handles.allocate();
	assert(replacement.slot == first.slot);
	assert(replacement.generation != first.generation);
	assert(handles.valid(replacement));

	handles.reset_epoch(8);
	assert(handles.epoch() == 8);
	assert(!handles.valid(replacement));
	const auto after_load = handles.allocate();
	assert(after_load.epoch == 8);
	assert(handles.valid(after_load));

	auto forged = after_load;
	++forged.epoch;
	assert(!handles.valid(forged));
	++forged.slot;
	assert(!handles.valid(forged));
	return 0;
}
