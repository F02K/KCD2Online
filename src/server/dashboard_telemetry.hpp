#pragma once

#include "multiplayer/networking.hpp"
#include "server/dashboard_config.hpp"
#include "server/server_core.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace kcd2o::server
{
	class dashboard_telemetry
	{
	public:
		dashboard_telemetry(
		    const server_config &server,
		    const dashboard_config &dashboard);

		void record_received(std::size_t bytes);
		void record_malformed();
		void record_sent(
		    std::size_t bytes,
		    traffic_lane lane,
		    bool sent,
		    bool congested,
		    reliability delivery);
		void record_tick(std::chrono::duration<double, std::milli> elapsed);
		void publish(
		    const server_core &core,
		    const std::vector<net::connection_statistics> &connections,
		    time_point now);
		[[nodiscard]] std::string snapshot() const;

	private:
		std::string server_name;
		std::string level_id;
		std::uint32_t max_players{};
		std::uint32_t tick_rate{};
		std::uint32_t snapshot_rate{};
		std::uint32_t refresh_interval_ms{};
		time_point started_at{clock::now()};
		time_point last_publish{started_at};
		std::uint64_t rx_bytes{};
		std::uint64_t tx_bytes{};
		std::uint64_t rx_messages{};
		std::uint64_t tx_messages{};
		std::uint64_t malformed_messages{};
		std::uint64_t congestion_drops{};
		std::uint64_t reliable_send_failures{};
		std::uint64_t ticks{};
		std::uint64_t tick_budget_overruns{};
		std::uint64_t previous_rx_bytes{};
		std::uint64_t previous_tx_bytes{};
		std::uint64_t previous_rx_messages{};
		std::uint64_t previous_tx_messages{};
		std::uint64_t previous_ticks{};
		std::uint64_t previous_process_cpu_time{};
		std::uint64_t previous_system_cpu_time{};
		std::uint64_t previous_system_idle_time{};
		bool cpu_sample_initialized{};
		double latest_tick_ms{};
		std::deque<double> tick_durations;
		mutable std::mutex snapshot_mutex;
		std::string published_snapshot{"{}"};
	};
}
