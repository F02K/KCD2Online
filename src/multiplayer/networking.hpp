#pragma once

#include "multiplayer/protocol.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace kcd2o::net
{
	constexpr int client_goodbye_reason = 1001;
	constexpr int server_rejected_reason = 1002;
	constexpr int server_shutdown_reason = 1003;
	constexpr int server_kicked_reason = 1004;

	class runtime
	{
	public:
		runtime();
		~runtime();
		runtime(const runtime &) = delete;
		runtime &operator=(const runtime &) = delete;

	private:
		bool m_initialized{};
	};

	struct server_callbacks
	{
		std::function<void(connection_id)> connected;
		std::function<void(connection_id, bool, std::string)> disconnected;
		std::function<void(connection_id, std::span<const std::byte>)> message;
	};

	class server_transport
	{
	public:
		explicit server_transport(server_callbacks callbacks);
		~server_transport();
		server_transport(const server_transport &) = delete;
		server_transport &operator=(const server_transport &) = delete;

		void listen(std::string_view bind_address, std::uint16_t port);
		void poll();
		[[nodiscard]] bool send(
		    connection_id connection,
		    std::span<const std::byte> bytes,
		    reliability delivery,
		    traffic_lane lane,
		    std::string *error = nullptr,
		    bool *congested = nullptr);
		[[nodiscard]] std::optional<std::size_t> pending_send_bytes(
		    connection_id connection,
		    traffic_lane lane) const;
		void close(
		    connection_id connection,
		    int reason,
		    std::string_view message,
		    bool linger);
		[[nodiscard]] std::string connection_description(
		    connection_id connection) const;

	private:
		class implementation;
		std::unique_ptr<implementation> m_impl;
	};

	struct client_callbacks
	{
		std::function<void()> connected;
		std::function<void(bool, std::string)> disconnected;
		std::function<void(std::span<const std::byte>)> message;
	};

	class client_transport
	{
	public:
		explicit client_transport(client_callbacks callbacks);
		~client_transport();
		client_transport(const client_transport &) = delete;
		client_transport &operator=(const client_transport &) = delete;

		void connect(std::string_view address);
		void poll();
		[[nodiscard]] bool send(
		    std::span<const std::byte> bytes,
		    reliability delivery,
		    traffic_lane lane,
		    std::string *error = nullptr);
		void disconnect(std::string_view message = "client disconnected");
		void abort_connection(std::string_view message);
		[[nodiscard]] bool has_connection() const;
		[[nodiscard]] int ping_ms() const;
		[[nodiscard]] float packet_loss_percent() const;

	private:
		class implementation;
		std::unique_ptr<implementation> m_impl;
	};
}
