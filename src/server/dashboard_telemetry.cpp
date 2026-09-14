#include "server/dashboard_telemetry.hpp"

#include <windows.h>
#include <psapi.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <numeric>

namespace kcd2o::server
{
	namespace
	{
		struct resource_snapshot
		{
			double process_cpu_percent{};
			double system_cpu_percent{};
			double process_cpu_seconds{};
			std::uint64_t process_cpu_time{};
			std::uint64_t system_cpu_time{};
			std::uint64_t system_idle_time{};
			std::uint64_t process_working_set_bytes{};
			std::uint64_t process_private_bytes{};
			std::uint64_t process_peak_working_set_bytes{};
			std::uint64_t system_memory_total_bytes{};
			std::uint64_t system_memory_available_bytes{};
			double system_memory_used_percent{};
			std::uint32_t process_id{};
			std::uint32_t handle_count{};
			std::uint32_t logical_processors{};
		};

		std::uint64_t file_time_value(const FILETIME &value)
		{
			ULARGE_INTEGER result{};
			result.LowPart = value.dwLowDateTime;
			result.HighPart = value.dwHighDateTime;
			return result.QuadPart;
		}

		resource_snapshot sample_resources(
		    std::uint64_t previous_process_cpu_time,
		    std::uint64_t previous_system_cpu_time,
		    std::uint64_t previous_system_idle_time,
		    bool have_previous_sample)
		{
			resource_snapshot result{};
			const auto process = GetCurrentProcess();
			result.process_id = GetCurrentProcessId();

			SYSTEM_INFO system_info{};
			GetSystemInfo(&system_info);
			result.logical_processors = system_info.dwNumberOfProcessors;

			FILETIME created{}, exited{}, process_kernel{}, process_user{};
			if (GetProcessTimes(
			        process,
			        &created,
			        &exited,
			        &process_kernel,
			        &process_user))
			{
				result.process_cpu_time = file_time_value(process_kernel)
				    + file_time_value(process_user);
				result.process_cpu_seconds =
				    static_cast<double>(result.process_cpu_time) / 10'000'000.0;
			}

			FILETIME system_idle{}, system_kernel{}, system_user{};
			if (GetSystemTimes(&system_idle, &system_kernel, &system_user))
			{
				result.system_idle_time = file_time_value(system_idle);
				result.system_cpu_time = file_time_value(system_kernel)
				    + file_time_value(system_user);
			}

			if (have_previous_sample
			    && result.process_cpu_time >= previous_process_cpu_time
			    && result.system_cpu_time > previous_system_cpu_time
			    && result.system_idle_time >= previous_system_idle_time)
			{
				const auto system_delta =
				    result.system_cpu_time - previous_system_cpu_time;
				const auto idle_delta =
				    result.system_idle_time - previous_system_idle_time;
				const auto process_delta =
				    result.process_cpu_time - previous_process_cpu_time;
				result.process_cpu_percent = std::clamp(
				    100.0 * static_cast<double>(process_delta)
				        / static_cast<double>(system_delta),
				    0.0,
				    100.0);
				result.system_cpu_percent = std::clamp(
				    100.0 * static_cast<double>(system_delta - std::min(system_delta, idle_delta))
				        / static_cast<double>(system_delta),
				    0.0,
				    100.0);
			}

			PROCESS_MEMORY_COUNTERS_EX memory_counters{};
			memory_counters.cb = static_cast<DWORD>(sizeof(memory_counters));
			if (GetProcessMemoryInfo(
			        process,
			        reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory_counters),
			        static_cast<DWORD>(sizeof(memory_counters))))
			{
				result.process_working_set_bytes = memory_counters.WorkingSetSize;
				result.process_private_bytes = memory_counters.PrivateUsage;
				result.process_peak_working_set_bytes =
				    memory_counters.PeakWorkingSetSize;
			}

			MEMORYSTATUSEX memory_status{};
			memory_status.dwLength = static_cast<DWORD>(sizeof(memory_status));
			if (GlobalMemoryStatusEx(&memory_status))
			{
				result.system_memory_total_bytes = memory_status.ullTotalPhys;
				result.system_memory_available_bytes = memory_status.ullAvailPhys;
				result.system_memory_used_percent = memory_status.dwMemoryLoad;
			}

			DWORD handles{};
			if (GetProcessHandleCount(process, &handles))
				result.handle_count = handles;
			return result;
		}

		double percentile_95(const std::deque<double> &values)
		{
			if (values.empty())
				return 0.0;
			auto sorted = std::vector(values.begin(), values.end());
			std::ranges::sort(sorted);
			const auto index = std::min(
			    sorted.size() - 1,
			    static_cast<std::size_t>(sorted.size() * 0.95));
			return sorted[index];
		}
	}

	dashboard_telemetry::dashboard_telemetry(
	    const server_config &server_config_value,
	    const dashboard_config &dashboard) :
	    server_name(server_config_value.name),
	    level_id(server_config_value.level_id),
	    max_players(server_config_value.max_players),
	    tick_rate(server_config_value.tick_rate),
	    snapshot_rate(server_config_value.snapshot_rate),
	    refresh_interval_ms(dashboard.refresh_interval_ms)
	{
	}

	void dashboard_telemetry::record_received(std::size_t bytes)
	{
		rx_bytes += bytes;
		++rx_messages;
	}

	void dashboard_telemetry::record_malformed()
	{
		++malformed_messages;
	}

	void dashboard_telemetry::record_sent(
	    std::size_t bytes,
	    traffic_lane,
	    bool sent,
	    bool congested,
	    reliability delivery)
	{
		if (sent)
		{
			tx_bytes += bytes;
			++tx_messages;
		}
		else if (congested)
			++congestion_drops;
		else if (delivery == reliability::reliable)
			++reliable_send_failures;
	}

	void dashboard_telemetry::record_tick(
	    std::chrono::duration<double, std::milli> elapsed)
	{
		latest_tick_ms = std::max(0.0, elapsed.count());
		tick_durations.push_back(latest_tick_ms);
		while (tick_durations.size() > 240)
			tick_durations.pop_front();
		if (tick_rate > 0 && latest_tick_ms > 1000.0 / tick_rate)
			++tick_budget_overruns;
		++ticks;
	}

	void dashboard_telemetry::publish(
	    const server_core &core,
	    const std::vector<net::connection_statistics> &connections,
	    time_point now)
	{
		const auto elapsed = std::max(
		    0.001,
		    std::chrono::duration<double>(now - last_publish).count());
		const auto rate = [elapsed](std::uint64_t current, std::uint64_t previous)
		{
			return static_cast<double>(current - previous) / elapsed;
		};

		double ping_sum{};
		double loss_sum{};
		std::size_t quality_count{};
		std::size_t pending_bytes{};
		std::array<std::size_t, traffic_lane_count> lane_bytes{};
		for (const auto &connection : connections)
		{
			pending_bytes += connection.pending_send_bytes;
			if (connection.ping_ms >= 0)
			{
				ping_sum += connection.ping_ms;
				loss_sum += connection.packet_loss_percent;
				++quality_count;
			}
			for (std::size_t index{}; index < lane_bytes.size(); ++index)
				lane_bytes[index] += connection.pending_lane_bytes[index];
		}

		const auto players = core.players();
		const auto connected_players = std::ranges::count_if(
		    players,
		    [](const player_view &player)
		    { return player.connected || player.dummy; });
		constexpr std::array<std::string_view, traffic_lane_count> lane_names{
		    "Player realtime",
		    "Ordered state",
		    "NPC realtime",
		    "Interactive",
		    "Voice realtime"};
		nlohmann::json lanes = nlohmann::json::array();
		for (std::size_t index{}; index < lane_bytes.size(); ++index)
		{
			lanes.push_back({
			    {"name", lane_names[index]},
			    {"pending_bytes", lane_bytes[index]}});
		}
		const auto resources = sample_resources(
		    previous_process_cpu_time,
		    previous_system_cpu_time,
		    previous_system_idle_time,
		    cpu_sample_initialized);
		const auto tick_budget_ms = tick_rate == 0 ? 0.0 : 1000.0 / tick_rate;

		const auto document = nlohmann::json{
		    {"schema_version", 2},
		    {"refresh_interval_ms", refresh_interval_ms},
		    {"server",
		     {{"name", server_name},
		      {"version", std::string(kcd2o_version)},
		      {"level_id", level_id},
		      {"uptime_seconds",
		       std::chrono::duration_cast<std::chrono::seconds>(now - started_at)
		           .count()},
		      {"players_connected", connected_players},
		      {"max_players", max_players},
		      {"pending_connections", core.pending_connection_count()},
		      {"tick_rate_target", tick_rate},
		      {"snapshot_rate_target", snapshot_rate}}},
		    {"performance",
		     {{"ticks_per_second", rate(ticks, previous_ticks)},
		      {"tick_ms_current", latest_tick_ms},
		      {"tick_ms_p95", percentile_95(tick_durations)},
		      {"tick_budget_ms", tick_budget_ms},
		      {"tick_budget_used_percent",
		       tick_budget_ms <= 0.0
		           ? 0.0
		           : latest_tick_ms / tick_budget_ms * 100.0},
		      {"tick_budget_overruns", tick_budget_overruns},
		      {"messages_in_per_second",
		       rate(rx_messages, previous_rx_messages)},
		      {"messages_out_per_second",
		       rate(tx_messages, previous_tx_messages)}}},
		    {"network",
		     {{"connections", connections.size()},
		      {"average_ping_ms",
		       quality_count == 0 ? -1.0 : ping_sum / quality_count},
		      {"packet_loss_percent",
		       quality_count == 0 ? 0.0 : loss_sum / quality_count},
		      {"rx_bytes_per_second", rate(rx_bytes, previous_rx_bytes)},
		      {"tx_bytes_per_second", rate(tx_bytes, previous_tx_bytes)},
		      {"total_rx_bytes", rx_bytes},
		      {"total_tx_bytes", tx_bytes},
		      {"send_queue_bytes", pending_bytes},
		      {"malformed_messages", malformed_messages},
		      {"congestion_drops", congestion_drops},
		      {"reliable_send_failures", reliable_send_failures},
		      {"lanes", std::move(lanes)}}},
		    {"resources",
		     {{"process_cpu_percent", resources.process_cpu_percent},
		      {"system_cpu_percent", resources.system_cpu_percent},
		      {"process_cpu_seconds", resources.process_cpu_seconds},
		      {"process_working_set_bytes", resources.process_working_set_bytes},
		      {"process_private_bytes", resources.process_private_bytes},
		      {"process_peak_working_set_bytes",
		       resources.process_peak_working_set_bytes},
		      {"system_memory_total_bytes", resources.system_memory_total_bytes},
		      {"system_memory_available_bytes",
		       resources.system_memory_available_bytes},
		      {"system_memory_used_percent",
		       resources.system_memory_used_percent},
		      {"process_id", resources.process_id},
		      {"handle_count", resources.handle_count},
		      {"logical_processors", resources.logical_processors}}}};
		{
			std::scoped_lock lock(snapshot_mutex);
			published_snapshot = document.dump();
		}
		previous_rx_bytes = rx_bytes;
		previous_tx_bytes = tx_bytes;
		previous_rx_messages = rx_messages;
		previous_tx_messages = tx_messages;
		previous_ticks = ticks;
		previous_process_cpu_time = resources.process_cpu_time;
		previous_system_cpu_time = resources.system_cpu_time;
		previous_system_idle_time = resources.system_idle_time;
		cpu_sample_initialized = resources.process_cpu_time != 0
		    && resources.system_cpu_time != 0;
		last_publish = now;
	}

	std::string dashboard_telemetry::snapshot() const
	{
		std::scoped_lock lock(snapshot_mutex);
		return published_snapshot;
	}
}
