#pragma once

#include "multiplayer/protocol.hpp"
#include "npc/catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace kcd2o
{
	using remote_avatar_handle = std::uintptr_t;

	struct remote_avatar_snapshot
	{
		player_id id{};
		std::string display_name;
		bool connected{};
		bool has_transform{};
		protocol::TransformState transform;
		protocol::MovementMode movement_mode{
		    protocol::MOVEMENT_MODE_IDLE};
		bool has_avatar{};
		protocol::AvatarDescriptor avatar;
		bool has_activity{};
		protocol::PlayerActivity activity;
	};

	enum class remote_avatar_state
	{
		pending,
		waiting_for_human,
		waiting_for_soul,
		stabilizing_soul,
		waiting_for_inventory,
		ready,
		failed
	};

	[[nodiscard]] constexpr bool is_valid_remote_avatar_state(
	    remote_avatar_state state) noexcept
	{
		switch (state)
		{
		case remote_avatar_state::pending:
		case remote_avatar_state::waiting_for_human:
		case remote_avatar_state::waiting_for_soul:
		case remote_avatar_state::stabilizing_soul:
		case remote_avatar_state::waiting_for_inventory:
		case remote_avatar_state::ready:
		case remote_avatar_state::failed:
			return true;
		}
		return false;
	}

	[[nodiscard]] constexpr const char *to_string(
	    remote_avatar_state state) noexcept
	{
		switch (state)
		{
		case remote_avatar_state::pending: return "pending";
		case remote_avatar_state::waiting_for_human:
			return "waiting-for-human";
		case remote_avatar_state::waiting_for_soul:
			return "waiting-for-soul";
		case remote_avatar_state::stabilizing_soul:
			return "stabilizing-soul";
		case remote_avatar_state::waiting_for_inventory:
			return "waiting-for-inventory";
		case remote_avatar_state::ready: return "ready";
		case remote_avatar_state::failed: return "failed";
		}
		return "invalid";
	}

	[[nodiscard]] constexpr bool is_pending_remote_avatar_state(
	    remote_avatar_state state) noexcept
	{
		return is_valid_remote_avatar_state(state)
		    && state != remote_avatar_state::ready
		    && state != remote_avatar_state::failed;
	}

	[[nodiscard]] constexpr bool is_valid_remote_avatar_transition(
	    remote_avatar_state from,
	    remote_avatar_state to) noexcept
	{
		if (!is_valid_remote_avatar_state(from)
		    || !is_valid_remote_avatar_state(to))
		{
			return false;
		}
		if (from == remote_avatar_state::failed)
			return to == remote_avatar_state::failed;
		if (from == remote_avatar_state::ready)
			return to == remote_avatar_state::ready
			    || to == remote_avatar_state::failed;
		if (to == remote_avatar_state::failed)
			return true;
		const auto rank = [](remote_avatar_state state)
		{
			switch (state)
			{
			case remote_avatar_state::pending: return 0;
			case remote_avatar_state::waiting_for_human: return 1;
			case remote_avatar_state::waiting_for_soul: return 2;
			case remote_avatar_state::stabilizing_soul: return 3;
			case remote_avatar_state::waiting_for_inventory: return 4;
			case remote_avatar_state::ready: return 5;
			case remote_avatar_state::failed: return 6;
			}
			return -1;
		};
		return rank(from) >= 0 && rank(to) >= rank(from);
	}

	struct remote_avatar_backend_status
	{
		remote_avatar_state state{remote_avatar_state::pending};
		std::string diagnostic;
	};

	struct remote_avatar_sync_result
	{
		bool success{true};
		bool degraded{};
		std::string error;
		std::string diagnostic;
		std::size_t spawned{};
		std::size_t updated{};
		std::size_t removed{};
	};

	struct remote_avatar_manager_options
	{
		bool allow_fallback{};
		std::chrono::seconds materialization_timeout{15};
	};

	class remote_avatar_backend
	{
	public:
		virtual ~remote_avatar_backend() = default;
		[[nodiscard]] virtual bool available() const = 0;
		[[nodiscard]] virtual std::string diagnostic() const = 0;
		[[nodiscard]] virtual std::optional<remote_avatar_handle> spawn(
		    const remote_avatar_snapshot &player) = 0;
		[[nodiscard]] virtual remote_avatar_backend_status status(
		    remote_avatar_handle avatar) const = 0;
		[[nodiscard]] virtual bool update(
		    remote_avatar_handle avatar,
		    const remote_avatar_snapshot &player,
		    bool appearance_changed) = 0;
		virtual void remove(remote_avatar_handle avatar) = 0;
	};

	class remote_avatar_manager
	{
	public:
		using clock = std::chrono::steady_clock;

		explicit remote_avatar_manager(
		    remote_avatar_backend &backend,
		    remote_avatar_manager_options options = {}) :
		    m_backend(backend),
		    m_options(options)
		{
		}

		[[nodiscard]] remote_avatar_sync_result sync(
		    std::span<const remote_avatar_snapshot> players,
		    clock::time_point now = clock::now())
		{
			remote_avatar_sync_result result;
			const bool needs_avatar = std::ranges::any_of(
			    players,
			    [](const remote_avatar_snapshot &player)
			    {
				    return player.id != 0 && player.has_transform;
			    });
			if (needs_avatar && !m_backend.available())
			{
				result.success = false;
				result.error = m_backend.diagnostic();
				return result;
			}

			std::unordered_set<player_id> present;
			present.reserve(players.size());
			for (const auto &player : players)
			{
				if (player.id == 0)
					continue;
				present.insert(player.id);
				if (!player.has_transform)
					continue;
				if (!player.has_avatar)
				{
					result.success = false;
					result.error =
					    "remote player has no avatar descriptor";
					return result;
				}

				auto iterator = m_avatars.find(player.id);
				if (iterator == m_avatars.end())
				{
					avatar_entry entry;
					if (!spawn_desired_or_fallback(
					        entry,
					        player,
					        now,
					        result))
					{
						return result;
					}
					iterator = m_avatars.emplace(
					    player.id,
					    std::move(entry)).first;
				}
				if (!sync_entry(
				        iterator->second,
				        player,
				        now,
				        result))
				{
					return result;
				}
			}

			for (auto iterator = m_avatars.begin();
			     iterator != m_avatars.end();)
			{
				if (present.contains(iterator->first))
				{
					++iterator;
					continue;
				}
				remove_entry(iterator->second, result);
				iterator = m_avatars.erase(iterator);
			}
			return result;
		}

		std::size_t clear()
		{
			const auto count = m_avatars.size();
			for (auto &[id, avatar] : m_avatars)
			{
				(void)id;
				if (avatar.candidate)
					m_backend.remove(*avatar.candidate);
				if (avatar.active)
					m_backend.remove(avatar.active);
			}
			m_avatars.clear();
			return count;
		}

		[[nodiscard]] std::size_t size() const
		{
			return m_avatars.size();
		}

	private:
		struct avatar_entry
		{
			remote_avatar_handle active{};
			remote_avatar_state active_state{remote_avatar_state::pending};
			clock::time_point active_started_at{};
			bool active_fallback{};
			std::string active_archetype;
			std::uint64_t active_revision{};
			std::optional<remote_avatar_handle> candidate;
			remote_avatar_state candidate_state{remote_avatar_state::pending};
			clock::time_point candidate_started_at{};
			std::string candidate_archetype;
			std::uint64_t candidate_revision{};
			std::size_t retry_attempt{};
			clock::time_point next_retry{};
		};

		[[nodiscard]] static remote_avatar_snapshot fallback_snapshot(
		    const remote_avatar_snapshot &player)
		{
			auto fallback = player;
			fallback.avatar.set_archetype_id(
			    std::string(npc::default_soul_id));
			return fallback;
		}

		static std::chrono::seconds retry_delay(std::size_t attempt)
		{
			if (attempt >= 5)
				return std::chrono::seconds(30);
			return std::chrono::seconds(std::size_t{1} << attempt);
		}

		static void append_diagnostic(
		    remote_avatar_sync_result &result,
		    std::string message)
		{
			result.degraded = true;
			if (!result.diagnostic.empty())
				result.diagnostic += "; ";
			result.diagnostic += std::move(message);
		}

		void schedule_retry(
		    avatar_entry &entry,
		    clock::time_point now)
		{
			entry.next_retry =
			    now + retry_delay(entry.retry_attempt);
			++entry.retry_attempt;
		}

		bool spawn_fallback(
		    avatar_entry &entry,
		    const remote_avatar_snapshot &player,
		    clock::time_point now,
		    remote_avatar_sync_result &result,
		    std::string diagnostic)
		{
			if (player.avatar.archetype_id()
			    == npc::default_soul_id)
			{
				const auto delay = retry_delay(entry.retry_attempt);
				schedule_retry(entry, now);
				append_diagnostic(
				    result,
				    std::format(
				        "player {}: default Soul {} unavailable{}{}; "
				        "remote visual hidden, next retry in {}s",
				        player.id,
				        npc::default_soul_id,
				        diagnostic.empty() ? "" : ": ",
				        diagnostic,
				        delay.count()));
				return true;
			}
			const auto fallback = fallback_snapshot(player);
			const auto handle = m_backend.spawn(fallback);
			if (!handle)
			{
				const auto delay = retry_delay(entry.retry_attempt);
				schedule_retry(entry, now);
				append_diagnostic(
				    result,
				    std::format(
				        "player {}: fallback Soul {} unavailable for desired "
				        "Soul {}{}{}; remote visual hidden, next retry in {}s",
				        player.id,
				        npc::default_soul_id,
				        player.avatar.archetype_id(),
				        diagnostic.empty() ? "" : ": ",
				        diagnostic,
				        delay.count()));
				return true;
			}
			entry.active = *handle;
			entry.active_state = remote_avatar_state::pending;
			entry.active_started_at = now;
			entry.active_fallback = true;
			entry.active_archetype = fallback.avatar.archetype_id();
			entry.active_revision = fallback.avatar.revision();
			const auto delay = retry_delay(entry.retry_attempt);
			schedule_retry(entry, now);
			++result.spawned;
			append_diagnostic(
			    result,
			    std::format(
			        "player {}: {}; desired Soul {}, fallback Soul {}; next retry in {}s",
			        player.id,
			        diagnostic.empty()
			            ? "using built-in fallback avatar"
			            : std::move(diagnostic),
			        player.avatar.archetype_id(),
			        npc::default_soul_id,
			        delay.count()));
			return true;
		}

		bool spawn_desired_or_fallback(
		    avatar_entry &entry,
		    const remote_avatar_snapshot &player,
		    clock::time_point now,
		    remote_avatar_sync_result &result)
		{
			if (const auto handle = m_backend.spawn(player))
			{
				entry.active = *handle;
				entry.active_state = remote_avatar_state::pending;
				entry.active_started_at = now;
				entry.active_archetype =
				    player.avatar.archetype_id();
				entry.active_revision = player.avatar.revision();
				++result.spawned;
				return true;
			}
			if (!m_options.allow_fallback)
			{
				result.success = false;
				result.error = std::format(
				    "player {}: desired remote-player avatar {} could not be spawned",
				    player.id,
				    player.avatar.archetype_id());
				return false;
			}
			return spawn_fallback(
			    entry,
			    player,
			    now,
			    result,
			    "desired remote-player avatar spawn failed");
		}

		bool handle_active_failure(
		    avatar_entry &entry,
		    const remote_avatar_snapshot &player,
		    clock::time_point now,
		    remote_avatar_sync_result &result,
		    std::string diagnostic)
		{
			if (entry.active)
			{
				m_backend.remove(entry.active);
				entry.active = 0;
				++result.removed;
			}
			entry.active_state = remote_avatar_state::pending;
			entry.active_started_at = {};
			if (entry.candidate)
			{
				m_backend.remove(*entry.candidate);
				entry.candidate.reset();
				++result.removed;
			}
			entry.candidate_state = remote_avatar_state::pending;
			entry.candidate_started_at = {};
			entry.candidate_archetype.clear();
			entry.candidate_revision = 0;
			if (!m_options.allow_fallback)
			{
				entry.active_fallback = false;
				entry.active_archetype.clear();
				entry.active_revision = 0;
				result.success = false;
				result.error = std::format(
				    "player {}: desired remote-player avatar failed{}{}",
				    player.id,
				    diagnostic.empty() ? "" : ": ",
				    diagnostic);
				return false;
			}
			if (entry.active_fallback)
			{
				entry.active_fallback = false;
				entry.active_archetype.clear();
				entry.active_revision = 0;
				return spawn_fallback(
				    entry,
				    player,
				    now,
				    result,
				    diagnostic.empty()
				        ? "fallback remote-player avatar failed"
				        : std::move(diagnostic));
			}
			return spawn_fallback(
			    entry,
			    player,
			    now,
			    result,
			    diagnostic.empty()
			        ? "desired remote-player avatar failed"
			        : std::move(diagnostic));
		}

		bool fail_candidate(
		    avatar_entry &entry,
		    const remote_avatar_snapshot &player,
		    clock::time_point now,
		    remote_avatar_sync_result &result,
		    std::string diagnostic)
		{
			if (entry.candidate)
			{
				m_backend.remove(*entry.candidate);
				entry.candidate.reset();
				++result.removed;
			}
			entry.candidate_archetype.clear();
			entry.candidate_revision = 0;
			entry.candidate_state = remote_avatar_state::pending;
			entry.candidate_started_at = {};
			if (!m_options.allow_fallback)
			{
				result.success = false;
				result.error = std::format(
				    "player {}: desired remote-player avatar replacement failed{}{}",
				    player.id,
				    diagnostic.empty() ? "" : ": ",
				    diagnostic);
				return false;
			}
			const auto delay = retry_delay(entry.retry_attempt);
			schedule_retry(entry, now);
			append_diagnostic(
			    result,
			    std::format(
			        "player {}: desired Soul {} retry failed ({}); fallback Soul {}; next retry in {}s",
			        player.id,
			        player.avatar.archetype_id(),
			        diagnostic.empty() ? "no diagnostic" : std::move(diagnostic),
			        npc::default_soul_id,
			        delay.count()));
			return true;
		}

		bool start_candidate(
		    avatar_entry &entry,
		    const remote_avatar_snapshot &player,
		    clock::time_point now,
		    remote_avatar_sync_result &result)
		{
			const auto candidate = m_backend.spawn(player);
			if (!candidate)
			{
				if (!m_options.allow_fallback)
				{
					result.success = false;
					result.error = std::format(
					    "player {}: desired remote-player avatar replacement could not be spawned",
					    player.id);
					return false;
				}
				const auto delay = retry_delay(entry.retry_attempt);
				schedule_retry(entry, now);
				append_diagnostic(
				    result,
				    std::format(
				        "player {}: desired Soul {} retry spawn failed; fallback Soul {}; next retry in {}s",
				        player.id,
				        player.avatar.archetype_id(),
				        npc::default_soul_id,
				        delay.count()));
				return true;
			}
			entry.candidate = *candidate;
			entry.candidate_state = remote_avatar_state::pending;
			entry.candidate_started_at = now;
			entry.candidate_archetype =
			    player.avatar.archetype_id();
			entry.candidate_revision = player.avatar.revision();
			++result.spawned;
			return true;
		}

		bool sync_candidate(
		    avatar_entry &entry,
		    const remote_avatar_snapshot &player,
		    clock::time_point now,
		    remote_avatar_sync_result &result)
		{
			if (!entry.candidate)
				return true;
			if (entry.candidate_archetype
			    != player.avatar.archetype_id())
			{
				m_backend.remove(*entry.candidate);
				entry.candidate.reset();
				++result.removed;
				entry.candidate_state = remote_avatar_state::pending;
				entry.candidate_started_at = {};
				return start_candidate(entry, player, now, result);
			}
			const auto candidate_status =
			    m_backend.status(*entry.candidate);
			if (!is_valid_remote_avatar_transition(
			        entry.candidate_state,
			        candidate_status.state))
			{
				const auto diagnostic = std::format(
				    "native candidate lifecycle regressed from {} to {}",
				    to_string(entry.candidate_state),
				    to_string(candidate_status.state));
				return fail_candidate(
				    entry,
				    player,
				    now,
				    result,
				    diagnostic);
			}
			entry.candidate_state = candidate_status.state;
			if (candidate_status.state == remote_avatar_state::failed)
			{
				return fail_candidate(
				    entry,
				    player,
				    now,
				    result,
				    candidate_status.diagnostic);
			}
			if (is_pending_remote_avatar_state(candidate_status.state)
			    && now - entry.candidate_started_at
			        >= m_options.materialization_timeout)
			{
				return fail_candidate(
				    entry,
				    player,
				    now,
				    result,
				    candidate_status.diagnostic.empty()
				        ? std::string{"native candidate materialization timed out"}
				        : "native candidate materialization timed out: "
				            + candidate_status.diagnostic);
			}
			if (!m_backend.update(
			        *entry.candidate,
			        player,
			        entry.candidate_revision
			            != player.avatar.revision()))
			{
				return fail_candidate(
				    entry,
				    player,
				    now,
				    result,
				    "remote-player avatar retry update failed");
			}
			entry.candidate_revision = player.avatar.revision();
			++result.updated;
			if (candidate_status.state != remote_avatar_state::ready)
				return true;

			m_backend.remove(entry.active);
			++result.removed;
			entry.active = *entry.candidate;
			entry.active_state = entry.candidate_state;
			entry.active_started_at = entry.candidate_started_at;
			entry.active_fallback = false;
			entry.active_archetype = entry.candidate_archetype;
			entry.active_revision = entry.candidate_revision;
			entry.candidate.reset();
			entry.candidate_state = remote_avatar_state::pending;
			entry.candidate_started_at = {};
			entry.candidate_archetype.clear();
			entry.candidate_revision = 0;
			entry.retry_attempt = 0;
			entry.next_retry = {};
			return true;
		}

		bool sync_entry(
		    avatar_entry &entry,
		    const remote_avatar_snapshot &player,
		    clock::time_point now,
		    remote_avatar_sync_result &result)
		{
			if (!entry.active)
			{
				if (now < entry.next_retry)
					return true;
				return spawn_desired_or_fallback(
				    entry,
				    player,
				    now,
				    result);
			}
			if (entry.active_fallback
			    && player.avatar.archetype_id()
			           == npc::default_soul_id)
			{
				if (entry.candidate)
				{
					m_backend.remove(*entry.candidate);
					entry.candidate.reset();
					entry.candidate_state = remote_avatar_state::pending;
					entry.candidate_started_at = {};
					entry.candidate_archetype.clear();
					entry.candidate_revision = 0;
					++result.removed;
				}
				entry.active_fallback = false;
				entry.active_archetype =
				    player.avatar.archetype_id();
				entry.retry_attempt = 0;
				entry.next_retry = {};
			}
			const auto active_status = m_backend.status(entry.active);
			if (!is_valid_remote_avatar_transition(
			        entry.active_state,
			        active_status.state))
			{
				const auto diagnostic = std::format(
				    "native active-avatar lifecycle regressed from {} to {}",
				    to_string(entry.active_state),
				    to_string(active_status.state));
				return handle_active_failure(
				    entry,
				    player,
				    now,
				    result,
				    diagnostic);
			}
			entry.active_state = active_status.state;
			if (active_status.state == remote_avatar_state::failed)
			{
				return handle_active_failure(
				    entry,
				    player,
				    now,
				    result,
				    active_status.diagnostic);
			}
			if (is_pending_remote_avatar_state(active_status.state)
			    && now - entry.active_started_at
			        >= m_options.materialization_timeout)
			{
				return handle_active_failure(
				    entry,
				    player,
				    now,
				    result,
				    active_status.diagnostic.empty()
				        ? std::string{"native active-avatar materialization timed out"}
				        : "native active-avatar materialization timed out: "
				            + active_status.diagnostic);
			}

			const auto rendered = entry.active_fallback
			    ? fallback_snapshot(player)
			    : player;
			if (!m_backend.update(
			        entry.active,
			        rendered,
			        entry.active_revision
			            != rendered.avatar.revision()))
			{
				return handle_active_failure(
				    entry,
				    player,
				    now,
				    result,
				    "native remote-player avatar update failed");
			}
			entry.active_revision = rendered.avatar.revision();
			++result.updated;

			const bool needs_replacement = entry.active_fallback
			    || entry.active_archetype
			        != player.avatar.archetype_id();
			if (needs_replacement && !entry.candidate
			    && now >= entry.next_retry)
			{
				if (!start_candidate(entry, player, now, result))
					return false;
			}
			return sync_candidate(entry, player, now, result);
		}

		void remove_entry(
		    avatar_entry &entry,
		    remote_avatar_sync_result &result)
		{
			if (entry.candidate)
			{
				m_backend.remove(*entry.candidate);
				++result.removed;
			}
			if (entry.active)
			{
				m_backend.remove(entry.active);
				++result.removed;
			}
		}

		remote_avatar_backend &m_backend;
		remote_avatar_manager_options m_options;
		std::unordered_map<player_id, avatar_entry> m_avatars;
	};
}
