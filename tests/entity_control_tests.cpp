#include "multiplayer/entity_control.hpp"

#include <cassert>
#include <unordered_map>
#include <vector>

namespace
{
	struct fake_entity
	{
		bool active{};
		bool hidden{};
		bool writable{true};
		bool eligible{true};
	};

	class fake_backend final : public kcd2o::entity_control_backend
	{
	public:
		bool should_disable(
		    kcd2o::controlled_entity entity) const override
		{
			return static_cast<fake_entity *>(entity)->eligible;
		}

		bool is_active(kcd2o::controlled_entity entity) const override
		{
			return static_cast<fake_entity *>(entity)->active;
		}

		bool is_hidden(kcd2o::controlled_entity entity) const override
		{
			return static_cast<fake_entity *>(entity)->hidden;
		}

		bool set_active(
		    kcd2o::controlled_entity entity,
		    bool active) override
		{
			auto &value = *static_cast<fake_entity *>(entity);
			if (!value.writable)
			{
				return false;
			}
			value.active = active;
			return true;
		}

		bool set_hidden(
		    kcd2o::controlled_entity entity,
		    bool hidden) override
		{
			auto &value = *static_cast<fake_entity *>(entity);
			if (!value.writable)
			{
				return false;
			}
			value.hidden = hidden;
			return true;
		}
	};
}

int main()
{
	using namespace kcd2o;
	fake_backend backend;
	entity_controller controller(backend);
	fake_entity local_player{true, false};
	fake_entity remote_player{true, false};
	fake_entity active_visible{true, false};
	fake_entity inactive_hidden{false, true};
	fake_entity destroyed_while_disabled{true, false};
	fake_entity ui_helper{true, false, true, false};
	std::vector<controlled_entity> entities{
	    &local_player,
	    &remote_player,
	    &active_visible,
	    &inactive_hidden,
	    &destroyed_while_disabled,
	    &ui_helper};

	assert(controller.register_player(&local_player).failed == 0);
	assert(controller.register_player(&remote_player).failed == 0);
	const auto disabled = controller.set_disabled(true, entities);
	assert(disabled.changed);
	assert(disabled.affected == 3);
	assert(local_player.active && !local_player.hidden);
	assert(remote_player.active && !remote_player.hidden);
	assert(!active_visible.active && active_visible.hidden);
	assert(!inactive_hidden.active && inactive_hidden.hidden);
	assert(ui_helper.active && !ui_helper.hidden);

	fake_entity spawned{true, false};
	assert(controller.entity_created(&spawned).affected == 1);
	assert(!spawned.active && spawned.hidden);
	assert(controller.register_player(&spawned).restored == 1);
	assert(spawned.active && !spawned.hidden);

	controller.entity_destroyed(&destroyed_while_disabled);
	const auto enabled = controller.set_disabled(false);
	assert(enabled.changed);
	assert(enabled.restored == 2);
	assert(active_visible.active && !active_visible.hidden);
	assert(!inactive_hidden.active && inactive_hidden.hidden);
	assert(ui_helper.active && !ui_helper.hidden);
	assert(!controller.disabled());

	fake_entity unwritable{true, false, false};
	assert(controller.set_disabled(true).changed);
	assert(controller.entity_created(&unwritable).failed == 1);
	assert(controller.controlled_count() == 0);
	return 0;
}
