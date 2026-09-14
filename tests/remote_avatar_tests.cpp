#include "kcse/remote_avatar_readiness.hpp"
#include "multiplayer/emote_catalog.hpp"
#include "multiplayer/remote_avatar.hpp"
#include "multiplayer/remote_locomotion_animation.hpp"
#include "multiplayer/remote_transform_sequence.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace
{
	class fake_backend final : public kcd2o::remote_avatar_backend
	{
	public:
		bool available() const override
		{
			return enabled;
		}

		std::string diagnostic() const override
		{
			return "avatar backend unavailable";
		}

		std::optional<kcd2o::remote_avatar_handle> spawn(
		    const kcd2o::remote_avatar_snapshot &player) override
		{
			++spawn_attempts;
			if ((!desired_spawns_succeed
			        && player.avatar.archetype_id()
			            != kcd2o::npc::default_soul_id)
			    || (!fallback_spawns_succeed
			        && player.avatar.archetype_id()
			            == kcd2o::npc::default_soul_id))
			{
				return std::nullopt;
			}
			const auto handle = next_handle++;
			players[handle] = player;
			states[handle] = spawned_state;
			return handle;
		}

		kcd2o::remote_avatar_backend_status status(
		    kcd2o::remote_avatar_handle avatar) const override
		{
			const auto found = states.find(avatar);
			return found == states.end()
			    ? kcd2o::remote_avatar_backend_status{
			          kcd2o::remote_avatar_state::failed,
			          "avatar is missing"}
			    : kcd2o::remote_avatar_backend_status{
			          found->second,
			          found->second == kcd2o::remote_avatar_state::failed
			              ? "injected failure"
			              : ""};
		}

		bool update(
		    kcd2o::remote_avatar_handle avatar,
		    const kcd2o::remote_avatar_snapshot &player,
		    bool appearance_changed) override
		{
			++updates;
			players[avatar] = player;
			if (appearance_changed)
				++appearance_updates;
			return updates_succeed;
		}

		void remove(kcd2o::remote_avatar_handle avatar) override
		{
			players.erase(avatar);
			states.erase(avatar);
			++removed;
		}

		bool enabled{true};
		bool desired_spawns_succeed{true};
		bool fallback_spawns_succeed{true};
		bool updates_succeed{true};
		kcd2o::remote_avatar_state spawned_state{
		    kcd2o::remote_avatar_state::ready};
		kcd2o::remote_avatar_handle next_handle{1};
		std::size_t removed{};
		std::size_t spawn_attempts{};
		std::size_t updates{};
		std::size_t appearance_updates{};
		std::unordered_map<
		    kcd2o::remote_avatar_handle,
		    kcd2o::remote_avatar_snapshot>
		    players;
		std::unordered_map<
		    kcd2o::remote_avatar_handle,
		    kcd2o::remote_avatar_state>
		    states;
	};

	kcd2o::remote_avatar_snapshot player(
	    kcd2o::player_id id,
	    float x,
	    kcd2o::protocol::MovementMode mode)
	{
		kcd2o::remote_avatar_snapshot result;
		result.id = id;
		result.display_name = "Remote";
		result.connected = true;
		result.has_transform = true;
		result.transform.mutable_position()->set_x(x);
		result.transform.mutable_rotation()->set_w(1.0F);
		result.transform.mutable_velocity();
		result.movement_mode = mode;
		result.has_avatar = true;
		result.avatar.set_archetype_id(
		    "763db0bb-4469-497d-bdc9-712b3df91b5a");
		result.avatar.set_revision(1);
		return result;
	}
}

int main()
{
	using namespace kcd2o;
	using namespace std::chrono_literals;
	const auto *bow = find_emote(emote_kind::bow);
	assert(bow && bow->tags == "bowBig");
	assert(bow->skeleton_clip == "greetings_bow");
	assert(find_emote_fragment("Greetings") == bow);
	assert(find_emote(emote_kind::cheer)->skeleton_clip
	    == "soldier_speech_cheer08");
	assert(find_emote(emote_kind::point)->skeleton_clip
	    == "crowd_peasant_male_audience_point_03");
	assert(find_emote(emote_kind::surrender)->skeleton_clip
	    == "dlg_male_neutral_stand_disown_01");
	assert(remote_locomotion_animation_name(
	           remote_locomotion_animation::idle)
	    == "relaxed_idle_both");
	assert(remote_locomotion_animation_name(
	           remote_locomotion_animation::sprint)
	    == "3d_relaxed_sprint_turn_strafe");
	assert(remote_locomotion_animation_for_mode(
	           protocol::MOVEMENT_MODE_RUN)
	    == remote_locomotion_animation::run);
	assert(select_remote_locomotion_animation(
	           0.99F,
	           remote_locomotion_animation::idle)
	    == remote_locomotion_animation::idle);
	assert(select_remote_locomotion_animation(
	           1.00F,
	           remote_locomotion_animation::idle)
	    == remote_locomotion_animation::walk);
	assert(select_remote_locomotion_animation(
	           0.50F,
	           remote_locomotion_animation::walk)
	    == remote_locomotion_animation::walk);
	assert(select_remote_locomotion_animation(
	           2.50F,
	           remote_locomotion_animation::walk)
	    == remote_locomotion_animation::run);
	assert(select_remote_locomotion_animation(
	           2.00F,
	           remote_locomotion_animation::run)
	    == remote_locomotion_animation::run);
	assert(select_remote_locomotion_animation(
	           4.00F,
	           remote_locomotion_animation::run)
	    == remote_locomotion_animation::sprint);
	assert(select_remote_locomotion_animation(
	           3.30F,
	           remote_locomotion_animation::sprint)
	    == remote_locomotion_animation::sprint);
	assert(select_remote_locomotion_animation(
	           0.10F,
	           remote_locomotion_animation::walk)
	    == remote_locomotion_animation::idle);
	assert(is_valid_remote_avatar_transition(
	    remote_avatar_state::pending,
	    remote_avatar_state::waiting_for_human));
	assert(is_valid_remote_avatar_transition(
	    remote_avatar_state::waiting_for_human,
	    remote_avatar_state::waiting_for_inventory));
	assert(!is_valid_remote_avatar_transition(
	    remote_avatar_state::waiting_for_inventory,
	    remote_avatar_state::waiting_for_soul));
	assert(is_pending_remote_avatar_state(
	    remote_avatar_state::stabilizing_soul));
	assert(!is_pending_remote_avatar_state(remote_avatar_state::ready));
	assert(!is_valid_remote_avatar_state(
	    static_cast<remote_avatar_state>(999)));
	remote_transform_sequence transform_sequence;
	assert(transform_sequence.accept(100));
	assert(!transform_sequence.accept(1));
	transform_sequence.reset();
	assert(transform_sequence.accept(1));
	const auto soul_applied_at = std::chrono::steady_clock::now();
	assert(!kcse::evaluate_remote_soul_settle(
	             10,
	             10,
	             soul_applied_at,
	             soul_applied_at)
	             .ready);
	assert(!kcse::evaluate_remote_soul_settle(
	             13,
	             10,
	             soul_applied_at + 249ms,
	             soul_applied_at)
	             .ready);
	assert(!kcse::evaluate_remote_soul_settle(
	             12,
	             10,
	             soul_applied_at + 250ms,
	             soul_applied_at)
	             .ready);
	assert(kcse::evaluate_remote_soul_settle(
	           13,
	           10,
	           soul_applied_at + 250ms,
	           soul_applied_at)
	           .ready);
	fake_backend backend;
	remote_avatar_manager manager(backend);
	std::vector players{
	    player(1, 10.0F, protocol::MOVEMENT_MODE_IDLE),
	    player(2, 20.0F, protocol::MOVEMENT_MODE_WALK)};

	auto result = manager.sync(players);
	assert(result.success);
	assert(result.spawned == 2);
	assert(manager.size() == 2);
	assert(std::ranges::all_of(
	    backend.players,
	    [](const auto &entry)
	    {
		    return entry.second.display_name == "Remote";
	    }));

	players[0].display_name = "Renamed Remote";
	players[0].transform.mutable_position()->set_x(11.0F);
	players[0].movement_mode = protocol::MOVEMENT_MODE_RUN;
	players[0].connected = false;
	players.pop_back();
	result = manager.sync(players);
	assert(result.success);
	assert(result.updated == 1);
	assert(backend.appearance_updates == 0);
	assert(result.removed == 1);
	assert(manager.size() == 1);
	assert(backend.players.begin()->second.display_name
	    == "Renamed Remote");

	players[0].avatar.set_revision(2);
	result = manager.sync(players);
	assert(result.success);
	assert(backend.appearance_updates == 1);

	players[0].avatar.set_archetype_id(
	    "11111111-2222-4333-8444-555555555555");
	result = manager.sync(players);
	assert(result.success);
	assert(result.spawned == 1);
	assert(result.removed == 1);
	assert(manager.size() == 1);

	assert(manager.clear() == 1);
	assert(manager.size() == 0);

	result = manager.sync(players);
	assert(result.success);
	const auto removals_before_abandon = backend.removed;
	assert(manager.abandon_world() == 1);
	assert(manager.size() == 0);
	assert(backend.removed == removals_before_abandon);

	backend.enabled = false;
	result = manager.sync(players);
	assert(!result.success);
	assert(result.error == "avatar backend unavailable");
	assert(manager.size() == 0);

	fake_backend fallback_backend;
	fallback_backend.desired_spawns_succeed = false;
	remote_avatar_manager fallback_manager(
	    fallback_backend,
	    {.allow_fallback = true});
	auto fallback_players = std::vector{
	    player(3, 30.0F, protocol::MOVEMENT_MODE_IDLE)};
	fallback_players[0].avatar.set_archetype_id(
	    "11111111-2222-4333-8444-555555555555");
	const auto start = remote_avatar_manager::clock::now();
	result = fallback_manager.sync(fallback_players, start);
	assert(result.success);
	assert(result.degraded);
	assert(fallback_manager.size() == 1);
	assert(fallback_backend.players.begin()->second.avatar.archetype_id()
	    == npc::default_soul_id);
	assert(fallback_backend.players.begin()->second.display_name
	    == "Remote");

	fallback_backend.desired_spawns_succeed = true;
	fallback_backend.spawned_state = remote_avatar_state::pending;
	result = fallback_manager.sync(fallback_players, start + 500ms);
	assert(result.success);
	assert(fallback_backend.players.size() == 1);
	result = fallback_manager.sync(fallback_players, start + 1s);
	assert(result.success);
	assert(fallback_backend.players.size() == 2);
	for (auto &[handle, state] : fallback_backend.states)
	{
		if (fallback_backend.players.at(handle).avatar.archetype_id()
		    == fallback_players[0].avatar.archetype_id())
			state = remote_avatar_state::ready;
	}
	result = fallback_manager.sync(fallback_players, start + 1100ms);
	assert(result.success);
	assert(fallback_backend.players.size() == 1);
	assert(fallback_backend.players.begin()->second.avatar.archetype_id()
	    == fallback_players[0].avatar.archetype_id());

	const auto fallback_handle = fallback_backend.players.begin()->first;
	fallback_backend.states[fallback_handle] =
	    remote_avatar_state::failed;
	result = fallback_manager.sync(fallback_players, start + 2s);
	assert(result.success);
	assert(result.degraded);
	for (auto &[handle, state] : fallback_backend.states)
	{
		if (fallback_backend.players.at(handle).avatar.archetype_id()
		    == npc::default_soul_id)
			state = remote_avatar_state::failed;
	}
	result = fallback_manager.sync(fallback_players, start + 2500ms);
	assert(result.success);
	assert(result.degraded);
	assert(result.diagnostic.contains("player 3"));
	assert(fallback_manager.size() == 1);

	fake_backend backoff_backend;
	backoff_backend.desired_spawns_succeed = false;
	remote_avatar_manager backoff_manager(
	    backoff_backend,
	    {.allow_fallback = true});
	auto backoff_players = std::vector{
	    player(4, 40.0F, protocol::MOVEMENT_MODE_IDLE)};
	backoff_players[0].avatar.set_archetype_id(
	    "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
	const auto backoff_start = remote_avatar_manager::clock::now();
	result = backoff_manager.sync(backoff_players, backoff_start);
	assert(result.success);
	assert(backoff_backend.spawn_attempts == 2);
	for (const auto [elapsed, expected_attempts] :
	     std::array{
	         std::pair{500ms, std::size_t{2}},
	         std::pair{1000ms, std::size_t{3}},
	         std::pair{2000ms, std::size_t{3}},
	         std::pair{3000ms, std::size_t{4}},
	         std::pair{7000ms, std::size_t{5}},
	         std::pair{15000ms, std::size_t{6}},
	         std::pair{31000ms, std::size_t{7}},
	         std::pair{61000ms, std::size_t{8}}})
	{
		result = backoff_manager.sync(
		    backoff_players,
		    backoff_start + elapsed);
		assert(result.success);
		assert(backoff_backend.spawn_attempts == expected_attempts);
	}

	fake_backend default_failure_backend;
	default_failure_backend.fallback_spawns_succeed = false;
	remote_avatar_manager default_failure_manager(
	    default_failure_backend,
	    {.allow_fallback = true});
	auto default_failure_players = std::vector{
	    player(5, 50.0F, protocol::MOVEMENT_MODE_IDLE)};
	result = default_failure_manager.sync(default_failure_players);
	assert(result.success);
	assert(result.degraded);
	assert(result.diagnostic.contains("default Soul"));
	assert(default_failure_backend.spawn_attempts == 1);
	assert(default_failure_manager.size() == 1);
	result = default_failure_manager.sync(
	    default_failure_players,
	    remote_avatar_manager::clock::now() + 500ms);
	assert(result.success);
	assert(default_failure_backend.spawn_attempts == 1);

	fake_backend strict_backend;
	strict_backend.desired_spawns_succeed = false;
	remote_avatar_manager strict_manager(strict_backend);
	auto strict_players = std::vector{
	    player(6, 60.0F, protocol::MOVEMENT_MODE_IDLE)};
	strict_players[0].avatar.set_archetype_id(
	    "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff");
	result = strict_manager.sync(strict_players);
	assert(!result.success);
	assert(!result.degraded);
	assert(result.error.contains("could not be spawned"));
	assert(strict_backend.spawn_attempts == 1);
	assert(strict_backend.players.empty());

	fake_backend timeout_backend;
	timeout_backend.spawned_state = remote_avatar_state::waiting_for_soul;
	remote_avatar_manager timeout_manager(
	    timeout_backend,
	    {.materialization_timeout = 2s});
	auto timeout_players = std::vector{
	    player(7, 70.0F, protocol::MOVEMENT_MODE_IDLE)};
	const auto timeout_start = remote_avatar_manager::clock::now();
	result = timeout_manager.sync(timeout_players, timeout_start);
	assert(result.success);
	assert(timeout_backend.updates == 0);
	result = timeout_manager.sync(timeout_players, timeout_start + 2s);
	assert(!result.success);
	assert(result.error.contains("timed out"));
	assert(timeout_backend.updates == 0);
	assert(timeout_backend.players.empty());

	fake_backend pending_backend;
	pending_backend.spawned_state = remote_avatar_state::waiting_for_human;
	remote_avatar_manager pending_manager(pending_backend);
	auto pending_players = std::vector{
	    player(9, 90.0F, protocol::MOVEMENT_MODE_WALK)};
	const auto pending_start = remote_avatar_manager::clock::now();
	result = pending_manager.sync(pending_players, pending_start);
	assert(result.success);
	assert(pending_backend.updates == 0);
	pending_backend.states.begin()->second = remote_avatar_state::ready;
	result = pending_manager.sync(pending_players, pending_start + 1ms);
	assert(result.success);
	assert(pending_backend.updates == 1);

	fake_backend regression_backend;
	regression_backend.spawned_state =
	    remote_avatar_state::waiting_for_inventory;
	remote_avatar_manager regression_manager(regression_backend);
	auto regression_players = std::vector{
	    player(8, 80.0F, protocol::MOVEMENT_MODE_IDLE)};
	const auto regression_start = remote_avatar_manager::clock::now();
	result = regression_manager.sync(regression_players, regression_start);
	assert(result.success);
	regression_backend.states.begin()->second =
	    remote_avatar_state::waiting_for_soul;
	result = regression_manager.sync(
	    regression_players,
	    regression_start + 1ms);
	assert(!result.success);
	assert(result.error.contains("regressed"));
	return 0;
}
