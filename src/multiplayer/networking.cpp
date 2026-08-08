#include "multiplayer/networking.hpp"

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace kcd2o::net
{
	namespace
	{
		constexpr std::array<int, traffic_lane_count> lane_priorities{
		    10, // player_realtime
		    10, // ordered_state
		    10, // npc_realtime
		    0   // interactive
		};
		constexpr std::array<uint16, traffic_lane_count> lane_weights{
		    8, // player_realtime
		    6, // ordered_state
		    2, // npc_realtime
		    1  // ignored while interactive has a unique priority
		};

		static_assert(
		    static_cast<std::size_t>(traffic_lane::interactive) + 1
		    == traffic_lane_count);

		void set_error(std::string *error, std::string message)
		{
			if (error)
			{
				*error = std::move(message);
			}
		}

		bool configure_lanes(
		    ISteamNetworkingSockets *sockets,
		    HSteamNetConnection connection)
		{
			return sockets->ConfigureConnectionLanes(
			           connection,
			           static_cast<int>(traffic_lane_count),
			           lane_priorities.data(),
			           lane_weights.data())
		    == k_EResultOK;
		}

		bool send_message(
		    ISteamNetworkingSockets *sockets,
		    HSteamNetConnection connection,
		    std::span<const std::byte> bytes,
		    reliability delivery,
		    traffic_lane lane,
		    std::string *error,
		    bool *congested)
		{
			if (congested)
				*congested = false;
			if (bytes.empty() || bytes.size() > max_application_message_size)
			{
				set_error(error, "message size is invalid");
				return false;
			}
			const auto lane_index = static_cast<std::size_t>(lane);
			if (lane_index >= traffic_lane_count)
			{
				set_error(error, "message lane is invalid");
				return false;
			}

			auto *utils = SteamNetworkingUtils();
			auto *message = utils
			    ? utils->AllocateMessage(static_cast<int>(bytes.size()))
			    : nullptr;
			if (!message)
			{
				set_error(error, "could not allocate networking message");
				return false;
			}
			std::memcpy(message->m_pData, bytes.data(), bytes.size());
			message->m_conn = connection;
			message->m_nFlags = delivery == reliability::reliable
			    ? k_nSteamNetworkingSend_ReliableNoNagle
			    : k_nSteamNetworkingSend_UnreliableNoDelay;
			message->m_idxLane = static_cast<uint16>(lane_index);

			int64 result{};
			sockets->SendMessages(1, &message, &result, true);
			if (result > 0)
				return true;

			const auto code = result < 0
			    ? static_cast<EResult>(-result)
			    : k_EResultFail;
			if (congested)
				*congested = code == k_EResultLimitExceeded
				    || code == k_EResultIgnored;
			set_error(
			    error,
			    "SendMessages returned "
			        + std::to_string(static_cast<int>(code)));
			return false;
		}
	}

	runtime::runtime()
	{
		SteamDatagramErrMsg error{};
		if (!GameNetworkingSockets_Init(nullptr, error))
		{
			throw std::runtime_error(
			    std::string("GameNetworkingSockets initialization failed: ") + error);
		}
		m_initialized = true;
	}

	runtime::~runtime()
	{
		if (m_initialized)
		{
			GameNetworkingSockets_Kill();
		}
	}

	class server_transport::implementation
	{
	public:
		explicit implementation(server_callbacks callbacks) :
		    callbacks(std::move(callbacks)),
		    sockets(SteamNetworkingSockets())
		{
			if (!sockets)
			{
				throw std::runtime_error("SteamNetworkingSockets interface is unavailable");
			}
			if (callback_instance)
			{
				throw std::runtime_error("only one server transport may exist per process");
			}
			callback_instance = this;
		}

		~implementation()
		{
			if (listen_socket != k_HSteamListenSocket_Invalid)
			{
				sockets->CloseListenSocket(listen_socket);
			}
			if (poll_group != k_HSteamNetPollGroup_Invalid)
			{
				sockets->DestroyPollGroup(poll_group);
			}
			if (callback_instance == this)
			{
				callback_instance = nullptr;
			}
		}

		static void connection_status_changed(
		    SteamNetConnectionStatusChangedCallback_t *info)
		{
			if (callback_instance)
			{
				callback_instance->OnSteamNetConnectionStatusChanged(info);
			}
		}

		void OnSteamNetConnectionStatusChanged(
		    SteamNetConnectionStatusChangedCallback_t *info)
		{
			const auto connection = static_cast<connection_id>(info->m_hConn);
			switch (info->m_info.m_eState)
			{
			case k_ESteamNetworkingConnectionState_Connecting:
				if (sockets->AcceptConnection(info->m_hConn) != k_EResultOK
				    || !configure_lanes(sockets, info->m_hConn)
				    || !sockets->SetConnectionPollGroup(info->m_hConn, poll_group))
				{
					sockets->CloseConnection(
					    info->m_hConn,
					    server_rejected_reason,
					    "connection setup failed",
					    false);
					return;
				}
				if (callbacks.connected)
				{
					callbacks.connected(connection);
				}
				break;
			case k_ESteamNetworkingConnectionState_ClosedByPeer:
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			{
				const auto reason = info->m_info.m_eEndReason;
				const bool allow_reconnect = reason != client_goodbye_reason
				    && reason != server_rejected_reason
				    && reason != server_shutdown_reason
				    && reason != server_kicked_reason;
				if (callbacks.disconnected)
				{
					callbacks.disconnected(
					    connection,
					    allow_reconnect,
					    info->m_info.m_szEndDebug);
				}
				sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
				break;
			}
			default:
				break;
			}
		}

		server_callbacks callbacks;
		ISteamNetworkingSockets *sockets{};
		HSteamListenSocket listen_socket{k_HSteamListenSocket_Invalid};
		HSteamNetPollGroup poll_group{k_HSteamNetPollGroup_Invalid};
		static inline implementation *callback_instance{};
	};

	server_transport::server_transport(server_callbacks callbacks) :
	    m_impl(std::make_unique<implementation>(std::move(callbacks)))
	{
	}

	server_transport::~server_transport() = default;

	void server_transport::listen(
	    std::string_view bind_address,
	    std::uint16_t port)
	{
		if (m_impl->listen_socket != k_HSteamListenSocket_Invalid)
		{
			throw std::runtime_error("server transport is already listening");
		}

		SteamNetworkingIPAddr address;
		address.Clear();
		if (bind_address != "0.0.0.0" && bind_address != "::"
		    && !address.ParseString(std::string(bind_address).c_str()))
		{
			throw std::runtime_error("invalid bind address");
		}
		address.m_port = port;
		SteamNetworkingConfigValue_t callback_option;
		callback_option.SetPtr(
		    k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
		    reinterpret_cast<void *>(implementation::connection_status_changed));
		m_impl->listen_socket =
		    m_impl->sockets->CreateListenSocketIP(address, 1, &callback_option);
		if (m_impl->listen_socket == k_HSteamListenSocket_Invalid)
		{
			throw std::runtime_error("failed to create the listen socket");
		}
		m_impl->poll_group = m_impl->sockets->CreatePollGroup();
		if (m_impl->poll_group == k_HSteamNetPollGroup_Invalid)
		{
			m_impl->sockets->CloseListenSocket(m_impl->listen_socket);
			m_impl->listen_socket = k_HSteamListenSocket_Invalid;
			throw std::runtime_error("failed to create the server poll group");
		}
	}

	void server_transport::poll()
	{
		m_impl->sockets->RunCallbacks();
		if (m_impl->poll_group == k_HSteamNetPollGroup_Invalid)
		{
			return;
		}

		std::array<ISteamNetworkingMessage *, 32> messages{};
		while (true)
		{
			const auto count = m_impl->sockets->ReceiveMessagesOnPollGroup(
			    m_impl->poll_group,
			    messages.data(),
			    static_cast<int>(messages.size()));
			if (count <= 0)
			{
				break;
			}
			for (int index = 0; index < count; ++index)
			{
				auto *message = messages[static_cast<std::size_t>(index)];
				if (message && m_impl->callbacks.message)
				{
					m_impl->callbacks.message(
					    static_cast<connection_id>(message->m_conn),
					    {static_cast<const std::byte *>(message->m_pData),
					     static_cast<std::size_t>(message->m_cbSize)});
				}
				if (message)
				{
					message->Release();
				}
			}
		}
	}

	bool server_transport::send(
	    connection_id connection,
	    std::span<const std::byte> bytes,
	    reliability delivery,
	    traffic_lane lane,
	    std::string *error,
	    bool *congested)
	{
		return send_message(
		    m_impl->sockets,
		    static_cast<HSteamNetConnection>(connection),
		    bytes,
		    delivery,
		    lane,
		    error,
		    congested);
	}

	std::optional<std::size_t> server_transport::pending_send_bytes(
	    connection_id connection,
	    traffic_lane lane) const
	{
		const auto lane_index = static_cast<std::size_t>(lane);
		if (lane_index >= traffic_lane_count)
			return std::nullopt;
		SteamNetConnectionRealTimeStatus_t status{};
		std::array<SteamNetConnectionRealTimeLaneStatus_t, traffic_lane_count>
		    lanes{};
		if (m_impl->sockets->GetConnectionRealTimeStatus(
		        static_cast<HSteamNetConnection>(connection),
		        &status,
		        static_cast<int>(lanes.size()),
		        lanes.data())
		    != k_EResultOK)
			return std::nullopt;
		const auto &lane_status = lanes[lane_index];
		return static_cast<std::size_t>(
		           std::max(0, lane_status.m_cbPendingReliable))
		    + static_cast<std::size_t>(
		        std::max(0, lane_status.m_cbPendingUnreliable));
	}

	void server_transport::close(
	    connection_id connection,
	    int reason,
	    std::string_view message,
	    bool linger)
	{
		m_impl->sockets->CloseConnection(
		    static_cast<HSteamNetConnection>(connection),
		    reason,
		    std::string(message).c_str(),
		    linger);
	}

	std::string server_transport::connection_description(
	    connection_id connection) const
	{
		SteamNetConnectionInfo_t info{};
		if (!m_impl->sockets->GetConnectionInfo(
		        static_cast<HSteamNetConnection>(connection),
		        &info))
		{
			return "<closed>";
		}
		return info.m_szConnectionDescription;
	}

	class client_transport::implementation
	{
	public:
		explicit implementation(client_callbacks callbacks) :
		    callbacks(std::move(callbacks)),
		    sockets(SteamNetworkingSockets())
		{
			if (!sockets)
			{
				throw std::runtime_error("SteamNetworkingSockets interface is unavailable");
			}
			if (callback_instance)
			{
				throw std::runtime_error("only one client transport may exist per process");
			}
			callback_instance = this;
		}

		~implementation()
		{
			if (connection != k_HSteamNetConnection_Invalid)
			{
				sockets->CloseConnection(
				    connection,
				    client_goodbye_reason,
				    "client transport destroyed",
				    false);
			}
			if (callback_instance == this)
			{
				callback_instance = nullptr;
			}
		}

		static void connection_status_changed(
		    SteamNetConnectionStatusChangedCallback_t *info)
		{
			if (callback_instance)
			{
				callback_instance->OnSteamNetConnectionStatusChanged(info);
			}
		}

		void OnSteamNetConnectionStatusChanged(
		    SteamNetConnectionStatusChangedCallback_t *info)
		{
			if (info->m_hConn != connection)
			{
				return;
			}
			switch (info->m_info.m_eState)
			{
			case k_ESteamNetworkingConnectionState_Connected:
				if (!configure_lanes(sockets, connection))
				{
					const auto failed_connection = connection;
					connection = k_HSteamNetConnection_Invalid;
					connected = false;
					intentional_disconnect = true;
					sockets->CloseConnection(
					    failed_connection,
					    client_goodbye_reason,
					    "could not configure transport lanes",
					    false);
					if (callbacks.disconnected)
						callbacks.disconnected(
						    false,
						    "could not configure transport lanes");
					return;
				}
				intentional_disconnect = false;
				connected = true;
				if (callbacks.connected)
				{
					callbacks.connected();
				}
				break;
			case k_ESteamNetworkingConnectionState_ClosedByPeer:
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			{
				const bool retry = !intentional_disconnect
				    && info->m_info.m_eEndReason != server_rejected_reason
				    && info->m_info.m_eEndReason != server_shutdown_reason
				    && info->m_info.m_eEndReason != server_kicked_reason;
				connected = false;
				sockets->CloseConnection(connection, 0, nullptr, false);
				connection = k_HSteamNetConnection_Invalid;
				if (callbacks.disconnected)
				{
					callbacks.disconnected(retry, info->m_info.m_szEndDebug);
				}
				break;
			}
			default:
				break;
			}
		}

		client_callbacks callbacks;
		ISteamNetworkingSockets *sockets{};
		HSteamNetConnection connection{k_HSteamNetConnection_Invalid};
		bool intentional_disconnect{};
		bool connected{};
		static inline implementation *callback_instance{};
	};

	client_transport::client_transport(client_callbacks callbacks) :
	    m_impl(std::make_unique<implementation>(std::move(callbacks)))
	{
	}

	client_transport::~client_transport() = default;

	void client_transport::connect(std::string_view address_text)
	{
		if (m_impl->connection != k_HSteamNetConnection_Invalid)
		{
			throw std::runtime_error("client transport already has a connection");
		}
		SteamNetworkingIPAddr address;
		address.Clear();
		if (!address.ParseString(std::string(address_text).c_str())
		    || address.m_port == 0)
		{
			throw std::runtime_error(
			    "server address must be a numeric IP address with a port");
		}

		SteamNetworkingConfigValue_t callback_option;
		callback_option.SetPtr(
		    k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
		    reinterpret_cast<void *>(implementation::connection_status_changed));
		m_impl->intentional_disconnect = false;
		m_impl->connected = false;
		m_impl->connection =
		    m_impl->sockets->ConnectByIPAddress(address, 1, &callback_option);
		if (m_impl->connection == k_HSteamNetConnection_Invalid)
		{
			throw std::runtime_error("failed to create the client connection");
		}
	}

	void client_transport::poll()
	{
		m_impl->sockets->RunCallbacks();
		if (m_impl->connection == k_HSteamNetConnection_Invalid)
		{
			return;
		}
		std::array<ISteamNetworkingMessage *, 32> messages{};
		while (true)
		{
			const auto count = m_impl->sockets->ReceiveMessagesOnConnection(
			    m_impl->connection,
			    messages.data(),
			    static_cast<int>(messages.size()));
			if (count <= 0)
			{
				break;
			}
			for (int index = 0; index < count; ++index)
			{
				auto *message = messages[static_cast<std::size_t>(index)];
				if (message && m_impl->callbacks.message)
				{
					m_impl->callbacks.message(
					    {static_cast<const std::byte *>(message->m_pData),
					     static_cast<std::size_t>(message->m_cbSize)});
				}
				if (message)
				{
					message->Release();
				}
			}
		}
	}

	bool client_transport::send(
	    std::span<const std::byte> bytes,
	    reliability delivery,
	    traffic_lane lane,
	    std::string *error)
	{
		if (m_impl->connection == k_HSteamNetConnection_Invalid
		    || !m_impl->connected)
		{
			set_error(error, "client is not connected");
			return false;
		}
		return send_message(
		    m_impl->sockets,
		    m_impl->connection,
		    bytes,
		    delivery,
		    lane,
		    error,
		    nullptr);
	}

	void client_transport::disconnect(std::string_view message)
	{
		if (m_impl->connection == k_HSteamNetConnection_Invalid)
		{
			return;
		}
		m_impl->intentional_disconnect = true;
		m_impl->connected = false;
		m_impl->sockets->CloseConnection(
		    m_impl->connection,
		    client_goodbye_reason,
		    std::string(message).c_str(),
		    true);
		m_impl->connection = k_HSteamNetConnection_Invalid;
	}

	void client_transport::abort_connection(std::string_view message)
	{
		if (m_impl->connection == k_HSteamNetConnection_Invalid)
		{
			return;
		}
		m_impl->intentional_disconnect = true;
		m_impl->connected = false;
		m_impl->sockets->CloseConnection(
		    m_impl->connection,
		    client_goodbye_reason,
		    std::string(message).c_str(),
		    false);
		m_impl->connection = k_HSteamNetConnection_Invalid;
	}

	bool client_transport::has_connection() const
	{
		return m_impl->connection != k_HSteamNetConnection_Invalid
		    && m_impl->connected;
	}

	int client_transport::ping_ms() const
	{
		if (!has_connection())
		{
			return -1;
		}
		SteamNetConnectionRealTimeStatus_t status{};
		if (m_impl->sockets->GetConnectionRealTimeStatus(
		        m_impl->connection,
		        &status,
		        0,
		        nullptr)
		    != k_EResultOK)
		{
			return -1;
		}
		return status.m_nPing;
	}

	float client_transport::packet_loss_percent() const
	{
		if (!has_connection())
		{
			return 0.0F;
		}
		SteamNetConnectionRealTimeStatus_t status{};
		if (m_impl->sockets->GetConnectionRealTimeStatus(
		        m_impl->connection,
		        &status,
		        0,
		        nullptr)
		    != k_EResultOK)
		{
			return 0.0F;
		}
		float quality = 1.0F;
		if (status.m_flConnectionQualityLocal >= 0.0F
		    && status.m_flConnectionQualityRemote >= 0.0F)
		{
			quality = std::min(
			    status.m_flConnectionQualityLocal,
			    status.m_flConnectionQualityRemote);
		}
		else if (status.m_flConnectionQualityLocal >= 0.0F)
		{
			quality = status.m_flConnectionQualityLocal;
		}
		else if (status.m_flConnectionQualityRemote >= 0.0F)
		{
			quality = status.m_flConnectionQualityRemote;
		}
		quality = std::clamp(quality, 0.0F, 1.0F);
		return (1.0F - quality) * 100.0F;
	}
}
