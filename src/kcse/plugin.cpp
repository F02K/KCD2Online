#include "kcse/client_api.hpp"
#include "kcse/join_trace.hpp"
#include "kcse/native_keybinds.hpp"
#include "kcse/native_runtime.hpp"
#include "multiplayer/client.hpp"
#include "multiplayer/emote_catalog.hpp"
#include "multiplayer/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <format>
#include <KCSE/KCSEAPI.h>
#include <limits>
#include <optional>
#include <REL/Relocation.h>
#include <string>
#include <string_view>
#include <utility>

namespace
{
	kcd2o::kcse::native_runtime *g_runtime{};
	kcd2o::multiplayer_client *g_client{};
	KCSE::ITaskInterface *g_tasks{};
	using frame_clock = std::chrono::steady_clock;
	frame_clock::time_point g_next_remote_sync{};
	std::uint32_t g_remote_sync_rate{};
	std::size_t g_last_remote_player_count{};
	std::uintptr_t g_original_blacksmithing_start{};

	void blacksmithing_start_hook(void *session, std::uint32_t station_entity_id)
	{
		using function_type = void (*)(void *, std::uint32_t);
		reinterpret_cast<function_type>(g_original_blacksmithing_start)(session, station_entity_id);
		if (g_runtime)
		{
			g_runtime->on_blacksmithing_started(station_entity_id);
		}
	}

	bool install_activity_hooks()
	{
		g_original_blacksmithing_start = REL::Relocation<>{REL::ID(380'114), 0x2E}.write_call<5>(blacksmithing_start_hook);
		return g_original_blacksmithing_start != 0;
	}

	struct performance_window
	{
		frame_clock::time_point started{};
		std::chrono::nanoseconds frame_total{};
		std::chrono::nanoseconds frame_max{};
		std::chrono::nanoseconds game_tick_total{};
		std::chrono::nanoseconds game_tick_max{};
		std::chrono::nanoseconds remote_sync_total{};
		std::chrono::nanoseconds remote_sync_max{};
		std::uint64_t frames{};
		std::uint64_t sync_runs{};
	};

	performance_window g_performance;

	template<std::size_t N>
	void copy_text(char (&target)[N], std::string_view value) noexcept
	{
		const auto count = std::min(value.size(), N - 1);
		std::memcpy(target, value.data(), count);
		target[count] = '\0';
	}

	template<std::size_t N>
	bool valid_text(const char (&value)[N]) noexcept
	{
		return std::memchr(value, '\0', N) != nullptr;
	}

	std::uint32_t narrow_count(std::size_t count) noexcept
	{
		return static_cast<std::uint32_t>(std::min<std::size_t>(count, std::numeric_limits<std::uint32_t>::max()));
	}

	std::string staff_display_name(kcd2o::protocol::NetworkRole role, std::string_view display_name)
	{
		std::string_view badge;
		switch (role)
		{
		case kcd2o::protocol::NETWORK_ROLE_OWNER:     badge = "OWNER"; break;
		case kcd2o::protocol::NETWORK_ROLE_ADMIN:     badge = "ADMIN"; break;
		case kcd2o::protocol::NETWORK_ROLE_MODERATOR: badge = "MOD"; break;
		case kcd2o::protocol::NETWORK_ROLE_SUPPORTER: badge = "SUPPORT"; break;
		default:                                      return std::string(display_name);
		}
		return std::format("[{}] {}", badge, display_name);
	}

	bool remote_sync_due(frame_clock::time_point now, const kcd2o::client_update_rates &rates)
	{
		// Snapshots arrive at the server cadence, but the interpolated transform
		// changes every rendered frame. Apply it at up to 60 Hz so a 20 Hz
		// snapshot rate does not turn otherwise smooth interpolation into visible
		// 50 ms steps.
		const auto rate = std::clamp(std::max(rates.snapshot_rate, 60U), 1U, 120U);
		if (rate != g_remote_sync_rate)
		{
			g_remote_sync_rate = rate;
			g_next_remote_sync = now;
		}
		if (now < g_next_remote_sync)
		{
			return false;
		}
		const auto interval = std::chrono::duration_cast<frame_clock::duration>(std::chrono::duration<double>(1.0 / rate));
		// Keep the server cadence instead of drifting by one rendered frame
		// whenever the two rates are not clean multiples of each other. Missed
		// slots are skipped; native synchronization never runs more than once in
		// a PostUpdate frame.
		do
		{
			g_next_remote_sync += interval;
		} while (g_next_remote_sync <= now);
		return true;
	}

	void reset_remote_sync_schedule() noexcept
	{
		g_next_remote_sync         = {};
		g_remote_sync_rate         = 0;
		g_last_remote_player_count = 0;
	}

	void record_performance(frame_clock::time_point now, std::chrono::nanoseconds frame_time, std::chrono::nanoseconds game_tick_time, std::chrono::nanoseconds remote_sync_time, bool sync_ran, const kcd2o::client_status &status, const kcd2o::client_update_rates &rates, bool sandbox_active)
	{
		if (!kcd2o::kcse::join_trace::diagnostics_enabled())
		{
			g_performance = {};
			return;
		}
		if (status.state != kcd2o::client_state::connected || !sandbox_active)
		{
			g_performance = {};
			return;
		}
		if (g_performance.started == frame_clock::time_point{})
		{
			g_performance.started = now;
		}
		g_performance.frame_total     += frame_time;
		g_performance.frame_max        = std::max(g_performance.frame_max, frame_time);
		g_performance.game_tick_total += game_tick_time;
		g_performance.game_tick_max    = std::max(g_performance.game_tick_max, game_tick_time);
		if (sync_ran)
		{
			g_performance.remote_sync_total += remote_sync_time;
			g_performance.remote_sync_max    = std::max(g_performance.remote_sync_max, remote_sync_time);
			++g_performance.sync_runs;
		}
		++g_performance.frames;

		const auto elapsed = now - g_performance.started;
		if (elapsed < std::chrono::seconds(1))
		{
			return;
		}
		const auto milliseconds = [](std::chrono::nanoseconds value)
		{
			return std::chrono::duration<double, std::milli>(value).count();
		};
		const auto elapsed_seconds = std::chrono::duration<double>(elapsed).count();
		const auto frame_count     = static_cast<double>(g_performance.frames);
		const auto sync_count      = static_cast<double>(g_performance.sync_runs);
		kcd2o::kcse::join_trace::write_diagnostic("performance.remote-sync",
		                                          std::format("fps={:.1f} frames={} server_tick_rate={} "
		                                                      "server_snapshot_rate={} remote_players={} sync_runs={} "
		                                                      "plugin_work_avg_ms={:.3f} plugin_work_max_ms={:.3f} "
		                                                      "game_tick_avg_ms={:.3f} game_tick_max_ms={:.3f} "
		                                                      "remote_sync_avg_ms={:.3f} remote_sync_max_ms={:.3f}",
		                                                      frame_count / elapsed_seconds,
		                                                      g_performance.frames,
		                                                      rates.tick_rate,
		                                                      rates.snapshot_rate,
		                                                      g_last_remote_player_count,
		                                                      g_performance.sync_runs,
		                                                      milliseconds(g_performance.frame_total) / frame_count,
		                                                      milliseconds(g_performance.frame_max),
		                                                      milliseconds(g_performance.game_tick_total) / frame_count,
		                                                      milliseconds(g_performance.game_tick_max),
		                                                      sync_count == 0.0 ? 0.0 : milliseconds(g_performance.remote_sync_total) / sync_count,
		                                                      milliseconds(g_performance.remote_sync_max)));
		g_performance = {};
	}

	void queue_frame();

	void run_frame_impl()
	{
		const auto frame_started = frame_clock::now();
		std::chrono::nanoseconds game_tick_time{};
		std::chrono::nanoseconds remote_sync_time{};
		bool sync_ran = false;
		KCD2Online_JOIN_TRACE("join.kcse-post-update.enter", std::format("runtime={} client={} task_interface={}", static_cast<void *>(g_runtime), static_cast<void *>(g_client), static_cast<void *>(g_tasks)));
		if (!g_runtime || !g_client || !g_tasks)
		{
			KCD2Online_JOIN_TRACE("join.kcse-post-update.skipped", "runtime, client, or task interface is nil");
			return;
		}
		KCD2Online_JOIN_TRACE("join.kcse-post-update.runtime-frame.begin", "calling native_runtime::on_frame");
		if (g_runtime->on_frame())
		{
			KCD2Online_JOIN_TRACE("join.kcse-post-update.epoch-changed", "notifying multiplayer client");
			g_client->runtime_epoch_changed();
		}

		auto client_status = g_client->status();
		KCD2Online_JOIN_TRACE("join.kcse-post-update.client-state",
		                      std::format("state={} sandbox_active={} game_queue={}",
		                                  static_cast<int>(client_status.state),
		                                  g_runtime->sandbox_active(),
		                                  client_status.game_queue_size));
		if (client_status.state != kcd2o::client_state::disconnected)
		{
			KCD2Online_JOIN_TRACE("join.kcse-post-update.game-tick.begin", "draining network envelopes on KCSE PostUpdate thread");
			const auto now = std::chrono::steady_clock::now();
			std::optional<kcd2o::protocol::AvatarDescriptor> avatar_visual;
			if (g_client->reserve_local_avatar_sample(now))
			{
				avatar_visual = g_runtime->local_avatar_visual();
			}
			const auto game_tick_started = frame_clock::now();
			g_client->game_tick(g_runtime->local_transform(), std::move(avatar_visual), g_runtime->current_level_id(), now);
			game_tick_time = frame_clock::now() - game_tick_started;
			client_status  = g_client->status();
			KCD2Online_JOIN_TRACE(
			    "join.kcse-post-update.game-tick.complete",
			    std::format("state={} game_queue={}", static_cast<int>(client_status.state), client_status.game_queue_size));
		}

		const bool sandbox_active = g_runtime->sandbox_active();
		if (client_status.state == kcd2o::client_state::connected && sandbox_active)
		{
			if (const auto activity = g_runtime->take_local_activity_start())
			{
				if (!g_client->begin_local_activity(activity->kind, activity->station_guid))
				{
					g_runtime->cancel_local_activity();
					g_runtime->show_multiplayer_notice("Die Aktivitaet konnte nicht reserviert werden.");
				}
			}
			if (g_client->take_activity_denial())
			{
				g_runtime->cancel_local_activity();
			}
			if (g_runtime->local_activity_end_pending() && g_client->end_local_activity(g_runtime->local_transform()))
			{
				g_runtime->acknowledge_local_activity_end();
			}
			if (g_runtime->local_player_dead())
			{
				g_client->report_local_death();
			}
			else if (client_status.sleeping && !g_runtime->local_player_laying())
			{
				(void)g_client->set_sleeping(false);
			}
			client_status = g_client->status();
		}
		const auto update_rates    = g_client->update_rates();
		const auto remote_sync_now = frame_clock::now();
		if (client_status.state == kcd2o::client_state::connected && sandbox_active && remote_sync_due(remote_sync_now, update_rates))
		{
			sync_ran                       = true;
			const auto remote_sync_started = frame_clock::now();
			const auto players             = g_client->remote_players();
			g_last_remote_player_count     = players.size();
			KCD2Online_JOIN_TRACE("join.kcse-post-update.remote-sync.prepare", std::format("remote_players={}", players.size()));
			std::vector<kcd2o::remote_avatar_snapshot> snapshots;
			snapshots.reserve(players.size());
			for (const auto &player : players)
			{
				snapshots.push_back({.id            = player.id,
				                     .display_name  = staff_display_name(player.network_role, player.display_name),
				                     .connected     = player.connected,
				                     .has_transform = player.has_transform,
				                     .transform     = player.transform,
				                     .movement_mode = player.movement_mode,
				                     .has_avatar    = player.has_avatar,
				                     .avatar        = player.avatar,
				                     .has_activity  = player.has_activity,
				                     .activity      = player.activity});
			}
			const auto synchronized = g_runtime->sync_remote_players(snapshots);
			remote_sync_time        = frame_clock::now() - remote_sync_started;
			if (!synchronized.success)
			{
				KCD2Online_JOIN_TRACE("join.kcse-post-update.remote-sync.failed", synchronized.error);
				g_client->fail("Native remote-avatar synchronization failed: " + synchronized.error);
			}
		}
		else if (client_status.state != kcd2o::client_state::connected || !sandbox_active)
		{
			reset_remote_sync_schedule();
		}

		if (client_status.state == kcd2o::client_state::connected)
		{
			if (const auto correction = g_client->take_local_correction())
			{
				if (!g_runtime->apply_local_correction(*correction))
				{
					g_client->fail("Server correction requires a runtime-verified native "
					               "transform mutation wrapper.");
				}
			}
		}
		if (client_status.state == kcd2o::client_state::closing || client_status.state == kcd2o::client_state::disconnected)
		{
			if (g_runtime->sandbox_active() || !client_status.error.empty())
			{
				g_runtime->end_sandbox(client_status.error);
			}
			else
			{
				g_runtime->cancel_multiplayer_preparation();
			}
		}
		const auto frame_finished = frame_clock::now();
		record_performance(frame_finished, frame_finished - frame_started, game_tick_time, remote_sync_time, sync_ran, client_status, update_rates, sandbox_active);
		queue_frame();
		KCD2Online_JOIN_TRACE("join.kcse-post-update.complete", "next KCSE PostUpdate task queued");
	}

	void run_frame_with_cpp_exceptions()
	{
		try
		{
			run_frame_impl();
		}
		catch (const std::exception &exception)
		{
			KCD2Online_JOIN_TRACE("join.kcse-post-update.exception",
			                      std::format("type=std::exception what=\"{}\"", exception.what()));
			if (g_client)
			{
				g_client->fail(std::string("KCSE PostUpdate exception: ") + exception.what());
			}
			queue_frame();
		}
		catch (...)
		{
			KCD2Online_JOIN_TRACE("join.kcse-post-update.exception", "type=unknown");
			if (g_client)
			{
				g_client->fail("Unknown exception on KCSE PostUpdate thread");
			}
			queue_frame();
		}
	}

	void recover_from_frame_seh()
	{
		if (g_client)
		{
			g_client->fail("SEH exception on KCSE PostUpdate thread; see "
			               "KCD2Online-join.log");
		}
		queue_frame();
	}

	void run_frame()
	{
		kcd2o::kcse::join_trace::set_thread_role(kcd2o::kcse::join_trace::thread_role::kcse_post_update);
#ifdef _WIN32
		__try
		{
			run_frame_with_cpp_exceptions();
		}
		__except (KCD2Online_JOIN_SEH_FILTER("join.kcse-post-update.seh"))
		{
			recover_from_frame_seh();
		}
#else
		run_frame_with_cpp_exceptions();
#endif
	}

	void queue_frame()
	{
		if (!g_tasks)
		{
			return;
		}
		g_tasks->AddTask(run_frame);
	}

	void on_kcse_message(KCSE::Message *message)
	{
		KCD2Online_JOIN_TRACE("join.kcse.lifecycle",
		                      std::format("message={} sender=\"{}\" data_length={}",
		                                  message ? message->type : 0,
		                                  message && message->sender ? message->sender : "",
		                                  message ? message->dataLen : 0));
		if (message && g_runtime)
		{
			g_runtime->on_lifecycle(message->type);
		}
	}

	std::uint32_t __cdecl abi_get_runtime_status(kcd2o::kcse::runtime_status *result) noexcept
	{
		try
		{
			if (!result || result->struct_size != sizeof(kcd2o::kcse::runtime_status) || !g_runtime)
			{
				return 0;
			}
			const auto descriptor = g_runtime->descriptor();
			const auto gate       = g_runtime->capability();
			result->available     = gate.available ? 1U : 0U;
			result->joinable      = g_runtime->can_start_join() ? 1U : 0U;
			result->kcse_version  = descriptor.kcse_version;
			result->game_version  = descriptor.game_version;
			result->release_index = descriptor.release_index;
			result->epoch         = descriptor.epoch;
			result->capabilities  = descriptor.capabilities;
			copy_text(result->address_library, descriptor.address_library);
			copy_text(result->level_id, g_runtime->current_level_id());
			copy_text(result->diagnostic, gate.diagnostic);
			return 1;
		}
		catch (...)
		{
			KCD2Online_JOIN_TRACE("join.abi-get-runtime-status.exception", "type=unknown");
			return 0;
		}
	}

	std::uint32_t __cdecl abi_connect(const kcd2o::kcse::connect_request *request) noexcept
	{
		kcd2o::kcse::join_trace::set_thread_role(kcd2o::kcse::join_trace::thread_role::abi);
		try
		{
			if (!request || request->struct_size != sizeof(kcd2o::kcse::connect_request) || !valid_text(request->address)
			    || !valid_text(request->display_name) || !valid_text(request->password) || !valid_text(request->content_hash)
			    || !valid_text(request->claim_code) || !valid_text(request->server_id) || !valid_text(request->account_service_url) || !g_client)
			{
				const auto trace_id = kcd2o::kcse::join_trace::begin_join(
				    request && valid_text(request->address) ? std::string_view(request->address) : std::string_view("<invalid-request>"));
				KCD2Online_JOIN_TRACE("join.abi-connect.rejected",
				                      std::format("trace={} request={} struct_size={} expected_size={} "
				                                  "client={}",
				                                  trace_id,
				                                  static_cast<const void *>(request),
				                                  request ? request->struct_size : 0,
				                                  sizeof(kcd2o::kcse::connect_request),
				                                  static_cast<void *>(g_client)));
				kcd2o::kcse::join_trace::finish_join("ABI connect request validation failed");
				return 0;
			}
			KCD2Online_JOIN_TRACE("join.abi-connect.accepted",
			                      std::format("request={} target=\"{}\" display_name_length={}",
			                                  static_cast<const void *>(request),
			                                  request->address,
			                                  strnlen_s(request->display_name, kcd2o::kcse::text_capacity)));
			kcd2o::client_options options;
			options.address             = request->address;
			options.display_name        = request->display_name;
			options.password            = request->password;
			options.content_hash        = request->content_hash;
			options.claim_code          = request->claim_code;
			options.server_id           = request->server_id;
			options.account_service_url = request->account_service_url;
			return g_client->connect(std::move(options)) ? 1U : 0U;
		}
		catch (const std::exception &exception)
		{
			KCD2Online_JOIN_TRACE("join.abi-connect.exception", std::format("type=std::exception what=\"{}\"", exception.what()));
			kcd2o::kcse::join_trace::finish_join(exception.what());
			return 0;
		}
		catch (...)
		{
			KCD2Online_JOIN_TRACE("join.abi-connect.exception", "type=unknown");
			kcd2o::kcse::join_trace::finish_join("unknown ABI connect exception");
			return 0;
		}
	}

	void __cdecl abi_disconnect() noexcept
	{
		kcd2o::kcse::join_trace::set_thread_role(kcd2o::kcse::join_trace::thread_role::abi);
		KCD2Online_CRITICAL_TRACE("join.abi-disconnect.begin", "native UI requested disconnect");
		try
		{
			if (g_client)
			{
				g_client->disconnect();
			}
			KCD2Online_CRITICAL_TRACE("join.abi-disconnect.complete", "disconnect request accepted");
		}
		catch (...)
		{
			KCD2Online_CRITICAL_TRACE("join.abi-disconnect.exception", "type=unknown");
		}
	}

	std::uint32_t __cdecl abi_send_chat(const char *text) noexcept
	{
		try
		{
			if (!text || !g_client)
			{
				return 0;
			}
			const auto length = strnlen_s(text, kcd2o::kcse::text_capacity);
			if (length == 0 || length == kcd2o::kcse::text_capacity)
			{
				return 0;
			}
			return g_client->send_chat(std::string(text, length)) ? 1U : 0U;
		}
		catch (...)
		{
			KCD2Online_JOIN_TRACE("join.abi-send-chat.exception", "type=unknown");
			return 0;
		}
	}

	std::uint32_t __cdecl abi_play_emote(std::uint32_t kind) noexcept
	{
		try
		{
			if (!g_client || !g_runtime || g_client->status().state != kcd2o::client_state::connected)
			{
				return 0;
			}
			const auto *emote = kcd2o::find_emote(static_cast<kcd2o::emote_kind>(kind));
			return emote && g_runtime->play_emote(emote->fragment) ? 1U : 0U;
		}
		catch (...)
		{
			KCD2Online_JOIN_TRACE("join.abi-play-emote.exception", "type=unknown");
			return 0;
		}
	}

	std::uint32_t __cdecl abi_select_avatar(const char *archetype_id) noexcept
	{
		try
		{
			if (!archetype_id || !g_client)
			{
				return 0;
			}
			const auto length = strnlen_s(archetype_id, kcd2o::kcse::short_text_capacity);
			if (length == 0 || length == kcd2o::kcse::short_text_capacity)
			{
				return 0;
			}
			return g_client->select_avatar(std::string(archetype_id, length)) ? 1U : 0U;
		}
		catch (...)
		{
			KCD2Online_JOIN_TRACE("join.abi-select-avatar.exception", "type=unknown");
			return 0;
		}
	}

	std::uint32_t __cdecl abi_attempt_sleep() noexcept
	{
		try
		{
			if (!g_client || !g_runtime)
			{
				return 0;
			}
			if (!g_runtime->local_player_laying())
			{
				g_runtime->show_multiplayer_notice("Warten ist im Multiplayer deaktiviert.");
				return 0;
			}
			const auto accepted = g_client->set_sleeping(true);
			if (accepted)
			{
				const auto status = g_client->status();
				g_runtime->show_multiplayer_notice(
				    std::format("Warte auf schlafende Spieler: {}/{}", status.sleeping_players + 1, status.sleeping_players_required));
			}
			return accepted ? 1U : 0U;
		}
		catch (...)
		{
			return 0;
		}
	}

	std::uint32_t __cdecl abi_request_respawn() noexcept
	{
		try
		{
			return g_client && g_client->request_respawn() ? 1U : 0U;
		}
		catch (...)
		{
			return 0;
		}
	}

	std::uint32_t __cdecl abi_get_status(kcd2o::kcse::client_status_view *result) noexcept
	{
		try
		{
			if (!result || result->struct_size != sizeof(kcd2o::kcse::client_status_view) || !g_client)
			{
				return 0;
			}
			const auto status           = g_client->status();
			result->state               = static_cast<std::uint32_t>(status.state);
			result->local_player_id     = status.local_player_id;
			result->ping_ms             = status.ping_ms;
			result->packet_loss_percent = status.packet_loss_percent;
			result->game_queue_size     = narrow_count(status.game_queue_size);
			copy_text(result->server_name, status.server_name);
			copy_text(result->server_id, status.server_id);
			copy_text(result->session_id, status.session_id);
			copy_text(result->level_id, status.level_id);
			copy_text(result->error, status.error);
			copy_text(result->avatar_archetype_id, status.avatar_archetype_id);
			copy_text(result->default_avatar_archetype_id, status.avatar_policy.default_archetype_id());
			result->sleeping                  = status.sleeping ? 1U : 0U;
			result->sleeping_players          = status.sleeping_players;
			result->sleeping_players_required = status.sleeping_players_required;
			result->dead                      = status.dead ? 1U : 0U;
			result->respawn_pending           = status.respawn_pending ? 1U : 0U;
			if (g_runtime)
			{
				const auto voice        = g_runtime->voice_status();
				result->voice_recording = voice.recording ? 1U : 0U;
				result->voice_speaking  = voice.speaking ? 1U : 0U;
				result->voice_level     = voice.level;
				result->voice_range     = static_cast<std::uint32_t>(voice.range);
			}
			const auto keybinds                  = kcd2o::kcse::native_keybinds::state();
			result->native_keybinds              = keybinds.available ? 1U : 0U;
			result->chat_action_generation       = keybinds.chat_generation;
			result->emote_action_held            = keybinds.emote_held ? 1U : 0U;
			result->player_hub_action_generation = keybinds.player_hub_generation;
			result->social_action_generation     = keybinds.social_generation;
			result->staff_action_generation      = keybinds.staff_generation;
			result->environment_available        = status.environment_available ? 1U : 0U;
			result->time_of_day_hours            = status.time_of_day_hours;
			result->time_scale                   = status.time_scale;
			result->weather_id                   = status.weather_id;
			result->network_role                 = static_cast<std::uint32_t>(status.network_role);
			std::string permissions;
			for (const auto &permission : status.effective_permissions)
			{
				if (!permissions.empty())
				{
					permissions += '\n';
				}
				permissions += permission;
			}
			copy_text(result->effective_permissions, permissions);
			copy_text(result->error_code, status.error_code);
			copy_text(result->restriction_scope, status.restriction_scope);
			copy_text(result->restriction_kind, status.restriction_kind);
			copy_text(result->restriction_reason, status.restriction_reason);
			result->restriction_expires_at_unix_ms = status.restriction_expires_at_unix_ms;
			copy_text(result->restriction_reference_id, status.restriction_reference_id);
			copy_text(result->support_url, status.support_url);
			return 1;
		}
		catch (...)
		{
			KCD2Online_JOIN_TRACE("join.abi-get-status.exception", "type=unknown");
			return 0;
		}
	}

	std::uint32_t __cdecl abi_copy_players(kcd2o::kcse::remote_player_view *output, std::uint32_t capacity) noexcept
	{
		try
		{
			if (!g_client)
			{
				return 0;
			}
			const auto players = g_client->players();
			if (!output || capacity == 0)
			{
				return narrow_count(players.size());
			}
			const auto count = std::min<std::size_t>(players.size(), capacity);
			for (std::size_t index = 0; index < count; ++index)
			{
				output[index]               = {};
				output[index].player_id     = players[index].id;
				output[index].connected     = players[index].connected ? 1U : 0U;
				output[index].movement_mode = static_cast<std::uint32_t>(players[index].movement_mode);
				copy_text(output[index].display_name, players[index].display_name);
				copy_text(output[index].persistent_id, players[index].persistent_id);
				output[index].network_role = static_cast<std::uint32_t>(players[index].network_role);
			}
			return narrow_count(count);
		}
		catch (...)
		{
			KCD2Online_JOIN_TRACE("join.abi-copy-players.exception", "type=unknown");
			return 0;
		}
	}

	std::uint32_t __cdecl abi_copy_chat(kcd2o::kcse::chat_entry_view *output, std::uint32_t capacity) noexcept
	{
		try
		{
			if (!g_client)
			{
				return 0;
			}
			const auto entries = g_client->chat_history();
			if (!output || capacity == 0)
			{
				return narrow_count(entries.size());
			}
			const auto count = std::min<std::size_t>(entries.size(), capacity);
			for (std::size_t index = 0; index < count; ++index)
			{
				output[index]                = {};
				output[index].player_id      = entries[index].sender;
				output[index].server_time_ms = entries[index].server_time_ms;
				output[index].channel        = static_cast<std::uint32_t>(entries[index].channel);
				copy_text(output[index].display_name, entries[index].display_name);
				copy_text(output[index].text, entries[index].text);
				output[index].network_role = static_cast<std::uint32_t>(entries[index].network_role);
			}
			return narrow_count(count);
		}
		catch (...)
		{
			KCD2Online_JOIN_TRACE("join.abi-copy-chat.exception", "type=unknown");
			return 0;
		}
	}

	std::uint32_t __cdecl abi_copy_avatar_archetypes(kcd2o::kcse::fixed_string *output, std::uint32_t capacity) noexcept
	{
		try
		{
			if (!g_client)
			{
				return 0;
			}
			const auto &policy = g_client->status().avatar_policy;
			const auto count   = static_cast<std::size_t>(policy.allowed_archetype_ids_size());
			if (!output || capacity == 0)
			{
				return narrow_count(count);
			}
			const auto written = std::min<std::size_t>(count, capacity);
			for (std::size_t index = 0; index < written; ++index)
			{
				output[index] = {};
				copy_text(output[index].value, policy.allowed_archetype_ids(static_cast<int>(index)));
			}
			return narrow_count(written);
		}
		catch (...)
		{
			KCD2Online_JOIN_TRACE("join.abi-copy-avatar-archetypes.exception", "type=unknown");
			return 0;
		}
	}

	void __cdecl abi_set_diagnostic_logging(std::uint32_t enabled) noexcept
	{
		kcd2o::kcse::join_trace::set_diagnostics_enabled(enabled != 0);
	}

	std::uint32_t __cdecl abi_set_player_voice_volume(std::uint64_t player_id, float volume) noexcept
	{
		try
		{
			return g_runtime && g_runtime->set_player_voice_volume(player_id, volume) ? 1U : 0U;
		}
		catch (...)
		{
			return 0;
		}
	}

	const kcd2o::kcse::client_api g_api{sizeof(kcd2o::kcse::client_api), kcd2o::kcd2o_version_major, kcd2o::kcd2o_version_minor, kcd2o::kcd2o_version_patch, abi_get_runtime_status, abi_connect, abi_disconnect, abi_send_chat, abi_play_emote, abi_select_avatar, abi_attempt_sleep, abi_request_respawn, abi_get_status, abi_copy_players, abi_copy_chat, abi_copy_avatar_archetypes, abi_set_diagnostic_logging, sizeof(kcd2o::kcse::client_status_view), sizeof(kcd2o::kcse::remote_player_view), abi_set_player_voice_volume};
} // namespace

KCSE_EXPORT KCSE::PluginVersionData KCSEPlugin_Version = {
    KCSE::PluginVersionData::kDataVersion,
    4,
    "KCD2OnlineClient",
    "F02K",
    {0x01'05'06'00},
    1,
    KCSE::PluginVersionData::kVersionIndependent_None,
};

KCSE_EXPORT bool KCSEPlugin_Load(const KCSE::IKCSEInterface *kcse)
{
	if (!kcse || kcse->GetKCSEVersion() < 1)
	{
		return false;
	}
	KCSE::Init(kcse);
	g_tasks         = kcse->GetTaskInterface();
	auto *messaging = kcse->GetMessagingInterface();
	if (!g_tasks || !messaging)
	{
		return false;
	}

	// KCSE plugins are process-lifetime objects. Intentionally leak the client
	// so its network thread is never joined from DLL_PROCESS_DETACH.
	g_runtime = new kcd2o::kcse::native_runtime(*kcse);
	g_client  = new kcd2o::multiplayer_client(*g_runtime);
	KCSE::AllocTrampoline(1 << 10);
	(void)kcd2o::kcse::native_keybinds::install();
	if (!install_activity_hooks())
	{
		delete g_client;
		delete g_runtime;
		g_client  = nullptr;
		g_runtime = nullptr;
		return false;
	}
	if (!messaging->RegisterListener("KCSE", on_kcse_message))
	{
		delete g_client;
		delete g_runtime;
		g_client  = nullptr;
		g_runtime = nullptr;
		return false;
	}
	queue_frame();
	return true;
}

KCSE_EXPORT const kcd2o::kcse::client_api *__cdecl KCD2Online_QueryClient(std::uint32_t requested_version_major, std::uint32_t requested_version_minor, std::uint32_t requested_version_patch) noexcept
{
	return requested_version_major == kcd2o::kcd2o_version_major && requested_version_minor == kcd2o::kcd2o_version_minor && requested_version_patch == kcd2o::kcd2o_version_patch ? &g_api : nullptr;
}
