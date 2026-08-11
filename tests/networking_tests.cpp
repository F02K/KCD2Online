#include "multiplayer/networking.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>

int main()
{
	using namespace std::chrono_literals;
	using namespace kcd2o;

	net::runtime runtime;
	std::optional<connection_id> server_connection;
	bool client_connected{};
	std::array<bool, traffic_lane_count> server_received{};
	std::array<bool, traffic_lane_count> client_received{};
	const auto all_received = [](const auto &received)
	{
		return std::ranges::all_of(received, [](bool value) { return value; });
	};

	net::server_transport *server_ptr{};
	net::server_transport server({
	    .connected =
	        [&](connection_id connection)
	        {
		        server_connection = connection;
	        },
	    .disconnected =
	        [&](connection_id, bool, std::string)
	        {
		        server_connection.reset();
	        },
	    .message =
	        [&](connection_id connection, std::span<const std::byte> bytes)
	        {
		        if (bytes.size() != 4 || bytes[0] != std::byte{0x4B}
		            || bytes[1] != std::byte{0x43}
		            || bytes[2] != std::byte{0x44})
				return;
		        const auto lane_index = std::to_integer<std::size_t>(bytes[3]);
		        if (lane_index >= traffic_lane_count)
				return;
		        server_received[lane_index] = true;
		        const auto lane = static_cast<traffic_lane>(lane_index);
		        const auto delivery = lane == traffic_lane::player_realtime
		                || lane == traffic_lane::npc_realtime
		                || lane == traffic_lane::voice_realtime
		            ? reliability::unreliable
		            : reliability::reliable;
		        std::string error;
		        assert(server_ptr->send(
		            connection,
		            bytes,
		            delivery,
		            lane,
		            &error));
	        }});
	server_ptr = &server;

	net::client_transport client({
	    .connected =
	        [&]
	        {
		        client_connected = true;
	        },
	    .disconnected =
	        [&](bool, std::string)
	        {
		        client_connected = false;
	        },
	    .message =
	        [&](std::span<const std::byte> bytes)
	        {
		        if (bytes.size() != 4 || bytes[0] != std::byte{0x4B}
		            || bytes[1] != std::byte{0x43}
		            || bytes[2] != std::byte{0x44})
				return;
		        const auto lane_index = std::to_integer<std::size_t>(bytes[3]);
		        if (lane_index < traffic_lane_count)
				client_received[lane_index] = true;
	        }});

	const auto port =
	    static_cast<std::uint16_t>(40000 + GetCurrentProcessId() % 10000);
	server.listen("127.0.0.1", port);
	client.connect("127.0.0.1:" + std::to_string(port));
	assert(!client.has_connection());
	{
		const std::array premature_message{std::byte{0x01}};
		std::string error;
		assert(!client.send(
		    premature_message,
		    reliability::reliable,
		    traffic_lane::ordered_state,
		    &error));
		assert(error == "client is not connected");
	}

	const auto deadline = std::chrono::steady_clock::now() + 5s;
	bool sent{};
	while (std::chrono::steady_clock::now() < deadline
	    && !all_received(client_received))
	{
		server.poll();
		client.poll();
		if (client_connected && server_connection && !sent)
		{
			for (std::size_t lane_index{};
			     lane_index < traffic_lane_count;
			     ++lane_index)
			{
				const std::array message{
				    std::byte{0x4B},
				    std::byte{0x43},
				    std::byte{0x44},
				    static_cast<std::byte>(lane_index)};
				const auto lane = static_cast<traffic_lane>(lane_index);
				const auto delivery = lane == traffic_lane::player_realtime
				        || lane == traffic_lane::npc_realtime
				        || lane == traffic_lane::voice_realtime
				    ? reliability::unreliable
				    : reliability::reliable;
				std::string error;
				assert(client.send(message, delivery, lane, &error));
			}
			sent = true;
		}
		std::this_thread::sleep_for(1ms);
	}

	assert(client_connected);
	assert(server_connection.has_value());
	assert(all_received(server_received));
	assert(all_received(client_received));
	assert(client.ping_ms() >= -1);
	assert(client.packet_loss_percent() >= 0.0F);
	assert(client.packet_loss_percent() <= 100.0F);
	client.disconnect("networking test complete");
	return 0;
}
