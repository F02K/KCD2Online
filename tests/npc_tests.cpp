#include "npc/npc.hpp"
#include "npc/xgen_lifecycle.hpp"

#include <cassert>
#include <chrono>
#include <unordered_map>

namespace
{
	using namespace std::chrono_literals;
	using namespace kcd2o::npc;

	class fake_backend final : public backend
	{
	public:
		capability get_capability() const override
		{
			return {available, available ? "" : "unavailable"};
		}

		std::optional<native_handle> spawn(const spawn_request &request, std::string &error) override
		{
			if (!spawn_succeeds)
			{
				error = "spawn failed";
				return std::nullopt;
			}
			const auto result = next++;
			entries.emplace(result, request);
			return result;
		}

		status poll(native_handle) override
		{
			return {ready ? state::ready : state::pending};
		}

		bool set_transform(native_handle, const transform &value) override
		{
			last_transform = value;
			++transform_updates;
			return updates_succeed;
		}

		bool set_motion(native_handle, const motion &value) override
		{
			last_motion = value;
			++locomotion_updates;
			return updates_succeed;
		}

		bool set_appearance(native_handle, const appearance &value) override
		{
			last_appearance = value;
			++appearance_updates;
			return updates_succeed;
		}

		void remove(native_handle npc) override
		{
			entries.erase(npc);
			++removed;
		}

		bool available{true};
		bool spawn_succeeds{true};
		bool updates_succeed{true};
		bool ready{};
		native_handle next{1};
		std::size_t removed{};
		std::size_t transform_updates{};
		std::size_t locomotion_updates{};
		std::size_t appearance_updates{};
		transform last_transform;
		motion last_motion;
		appearance last_appearance;
		std::unordered_map<native_handle, spawn_request> entries;
	};

	spawn_request request()
	{
		spawn_request value;
		value.archetype_id = "human";
		return value;
	}
} // namespace

int main()
{
	using namespace kcd2o::npc;
	const auto start = manager::clock::now();
	fake_backend backend;
	manager npcs(backend, 2, 10s);

	auto first = npcs.spawn(request(), 1, start);
	assert(first);
	assert(npcs.get_status(first.npc).value == state::pending);
	npcs.tick(start);
	assert(backend.entries.size() == 1);
	assert(npcs.get_status(first.npc).value == state::pending);

	transform latest;
	latest.position[0] = 1.0F;
	assert(npcs.set_transform(first.npc, transform{}));
	assert(npcs.set_transform(first.npc, latest));
	assert(npcs.set_locomotion(first.npc, locomotion::run));
	backend.ready = true;
	npcs.tick(start + 1s);
	assert(npcs.get_status(first.npc).value == state::ready);
	assert(backend.transform_updates == 1);
	assert(backend.last_transform.position[0] == 1.0F);
	assert(backend.locomotion_updates == 1);
	assert(backend.last_motion.mode == locomotion::run);

	assert(npcs.remove(first.npc));
	assert(npcs.remove(first.npc));
	npcs.tick(start + 2s);
	assert(backend.removed == 1);
	assert(npcs.get_status(first.npc).error == error_code::invalid_handle);

	auto reused = npcs.spawn(request(), 2, start + 3s);
	assert(reused);
	assert(reused.npc.slot == first.npc.slot);
	assert(reused.npc.generation != first.npc.generation);
	assert(!npcs.set_transform(first.npc, latest));

	backend.ready = false;
	npcs.tick(start + 3s);
	npcs.tick(start + 14s);
	assert(npcs.get_status(reused.npc).error == error_code::timed_out);

	assert(npcs.clear_owner(2) == 1);
	npcs.tick(start + 15s);
	assert(npcs.get_status(reused.npc).value == state::removed);

	backend.ready             = true;
	auto externally_destroyed = npcs.spawn(request(), 4, start + 16s);
	assert(externally_destroyed);
	npcs.tick(start + 16s);
	assert(npcs.get_status(externally_destroyed.npc).value == state::ready);
	const auto native = backend.entries.begin()->first;
	backend.entries.erase(native);
	npcs.native_destroyed(native);
	assert(npcs.get_status(externally_destroyed.npc).error == error_code::externally_destroyed);
	assert(npcs.clear_owner(4) == 1);
	npcs.tick(start + 17s);

	appearance invalid;
	invalid.items.push_back({"item.one", "right_hand"});
	invalid.items.push_back({"item.two", "right_hand"});
	auto invalid_request   = request();
	invalid_request.visual = invalid;
	assert(!npcs.spawn(invalid_request, 5));

	backend.available      = false;
	const auto unavailable = npcs.spawn(request(), 3);
	assert(!unavailable);
	assert(unavailable.error == error_code::unavailable);

	auto xgen_request                     = request();
	xgen_request.archetype_id             = "763db0bb-4469-497d-bdc9-712b3df91b5a";
	xgen_request.world_transform.position = {1.0F, 2.0F, 3.0F};
	const auto parameters                 = make_xgen_spawn_parameters(xgen_request, 42);
	assert(parameters.name == "KCD2Online_Remote_42");
	assert(parameters.class_name == "NPC");
	assert(parameters.shared_soul_guid == xgen_request.archetype_id);
	assert(parameters.position == xgen_request.world_transform.position);
	assert((parameters.rotation == std::array<float, 3>{}));
	assert(parameters.no_ai);
	assert(parameters.idle_until_first_patch);
	assert(!parameters.perceptor_object_ai);
	assert(!parameters.perceptible_object_ai);
	return 0;
}
