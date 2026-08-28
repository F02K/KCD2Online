#include "server/dashboard_server.hpp"

#include "server/dashboard_assets.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace kcd2o::server
{
	namespace
	{
		using request_clock = std::chrono::steady_clock;

		std::string trim(std::string value)
		{
			while (!value.empty()
			    && std::isspace(static_cast<unsigned char>(value.back())))
				value.pop_back();
			auto first = value.begin();
			while (first != value.end()
			    && std::isspace(static_cast<unsigned char>(*first)))
				++first;
			value.erase(value.begin(), first);
			return value;
		}

		std::string lower(std::string value)
		{
			std::ranges::transform(
			    value,
			    value.begin(),
			    [](unsigned char character)
			    { return static_cast<char>(std::tolower(character)); });
			return value;
		}

		bool constant_time_equal(std::string_view left, std::string_view right)
		{
			std::size_t difference = left.size() ^ right.size();
			const auto count = std::max(left.size(), right.size());
			for (std::size_t index{}; index < count; ++index)
			{
				const auto left_byte = index < left.size()
				    ? static_cast<unsigned char>(left[index])
				    : 0U;
				const auto right_byte = index < right.size()
				    ? static_cast<unsigned char>(right[index])
				    : 0U;
				difference |= left_byte ^ right_byte;
			}
			return difference == 0;
		}

		std::string create_token()
		{
			std::array<unsigned char, 32> random{};
			if (BCryptGenRandom(
			        nullptr,
			        random.data(),
			        static_cast<ULONG>(random.size()),
			        BCRYPT_USE_SYSTEM_PREFERRED_RNG)
			    != 0)
			{
				throw std::runtime_error(
				    "could not create a secure dashboard access token");
			}
			constexpr std::string_view alphabet = "0123456789abcdef";
			std::string result;
			result.reserve(random.size() * 2);
			for (const auto byte : random)
			{
				result.push_back(alphabet[byte >> 4U]);
				result.push_back(alphabet[byte & 0x0FU]);
			}
			return result;
		}

		std::string load_or_create_token(const dashboard_config &config)
		{
			if (!config.require_token)
				return {};
			std::ifstream input(config.token_file, std::ios::binary);
			if (input)
			{
				std::string value{
				    std::istreambuf_iterator<char>(input),
				    std::istreambuf_iterator<char>()};
				value = trim(std::move(value));
				if (value.size() < 32 || value.size() > 256)
				{
					throw std::runtime_error(
					    "dashboard access token must contain 32 to 256 characters");
				}
				return value;
			}

			const auto token = create_token();
			if (const auto parent = config.token_file.parent_path(); !parent.empty())
				std::filesystem::create_directories(parent);
			std::ofstream output(
			    config.token_file,
			    std::ios::binary | std::ios::trunc);
			if (!output)
			{
				throw std::runtime_error(
				    "could not create the configured dashboard token file");
			}
			output << token << '\n';
			output.close();
			if (!output)
				throw std::runtime_error("could not write the dashboard token file");
			return token;
		}

		struct http_request
		{
			std::string method;
			std::string target;
			std::unordered_map<std::string, std::string> headers;
		};

		std::optional<http_request> parse_request(std::string_view raw)
		{
			const auto first_end = raw.find("\r\n");
			if (first_end == std::string_view::npos)
				return std::nullopt;
			const auto first = raw.substr(0, first_end);
			const auto method_end = first.find(' ');
			const auto target_end = method_end == std::string_view::npos
			    ? std::string_view::npos
			    : first.find(' ', method_end + 1);
			if (method_end == std::string_view::npos
			    || target_end == std::string_view::npos
			    || first.substr(target_end + 1) != "HTTP/1.1")
				return std::nullopt;

			http_request result{
			    std::string(first.substr(0, method_end)),
			    std::string(first.substr(method_end + 1, target_end - method_end - 1)),
			    {}};
			if (result.target.empty() || result.target.size() > 2048
			    || result.target.find_first_of("\r\n") != std::string::npos)
				return std::nullopt;

			auto cursor = first_end + 2;
			while (cursor < raw.size())
			{
				const auto end = raw.find("\r\n", cursor);
				if (end == std::string_view::npos || end == cursor)
					break;
				const auto line = raw.substr(cursor, end - cursor);
				const auto separator = line.find(':');
				if (separator == std::string_view::npos)
					return std::nullopt;
				auto key = lower(std::string(line.substr(0, separator)));
				auto value = trim(std::string(line.substr(separator + 1)));
				result.headers.insert_or_assign(std::move(key), std::move(value));
				cursor = end + 2;
			}
			return result;
		}

		std::string response(
		    int status,
		    std::string_view reason,
		    std::string_view type,
		    std::string_view body,
		    std::string_view extra_headers = {})
		{
			return "HTTP/1.1 " + std::to_string(status) + " "
			    + std::string(reason)
			    + "\r\nContent-Type: " + std::string(type)
			    + "\r\nContent-Length: " + std::to_string(body.size())
			    + "\r\nConnection: close"
			      "\r\nCache-Control: no-store"
			      "\r\nX-Content-Type-Options: nosniff"
			      "\r\nX-Frame-Options: DENY"
			      "\r\nReferrer-Policy: no-referrer"
			      "\r\nPermissions-Policy: camera=(), microphone=(), geolocation=()"
			      "\r\nContent-Security-Policy: default-src 'none'; script-src 'self'; style-src-elem 'self'; style-src-attr 'unsafe-inline'; connect-src 'self'; img-src 'self'; font-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"
			    + std::string(extra_headers) + "\r\n\r\n" + std::string(body);
		}

		bool send_all(SOCKET socket, std::string_view bytes)
		{
			while (!bytes.empty())
			{
				const auto chunk = static_cast<int>(std::min<std::size_t>(
				    bytes.size(),
				    static_cast<std::size_t>(std::numeric_limits<int>::max())));
				const auto sent = send(socket, bytes.data(), chunk, 0);
				if (sent == SOCKET_ERROR || sent == 0)
					return false;
				bytes.remove_prefix(static_cast<std::size_t>(sent));
			}
			return true;
		}
	}

	class dashboard_server::implementation
	{
	public:
		implementation(
		    dashboard_config config_value,
		    snapshot_provider provider_value) :
		    config(std::move(config_value)),
		    provider(std::move(provider_value))
		{
		}

		~implementation()
		{
			stop();
		}

		void start()
		{
			if (worker.joinable())
				return;
			token = load_or_create_token(config);
			WSADATA data{};
			if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
				throw std::runtime_error("could not initialize the dashboard network stack");
			winsock_started = true;

			addrinfo hints{};
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_protocol = IPPROTO_TCP;
			hints.ai_flags = AI_NUMERICHOST;
			addrinfo *addresses{};
			const auto port = std::to_string(config.port);
			const auto host = config.bind_address == "localhost"
			    ? "127.0.0.1"
			    : config.bind_address.c_str();
			if (getaddrinfo(host, port.c_str(), &hints, &addresses) != 0)
			{
				cleanup_winsock();
				throw std::runtime_error("dashboard bind_address must be a numeric IP address");
			}
			listen_socket = socket(
			    addresses->ai_family,
			    addresses->ai_socktype,
			    addresses->ai_protocol);
			if (listen_socket == INVALID_SOCKET
			    || bind(listen_socket, addresses->ai_addr,
			           static_cast<int>(addresses->ai_addrlen)) == SOCKET_ERROR
			    || listen(listen_socket, SOMAXCONN) == SOCKET_ERROR)
			{
				freeaddrinfo(addresses);
				close_socket();
				cleanup_winsock();
				throw std::runtime_error("could not bind the dashboard HTTP endpoint");
			}
			freeaddrinfo(addresses);
			worker = std::jthread([this](std::stop_token stop) { run(stop); });
		}

		void stop()
		{
			if (worker.joinable())
			{
				worker.request_stop();
				close_socket();
				worker.join();
			}
			cleanup_winsock();
		}

		std::string endpoint() const
		{
			const auto host = config.bind_address == "0.0.0.0"
			    ? std::string("127.0.0.1")
			    : config.bind_address;
			return "http://" + (host.find(':') == std::string::npos
			        ? host
			        : "[" + host + "]")
			    + ":" + std::to_string(config.port);
		}

	private:
		void close_socket()
		{
			const auto socket = std::exchange(listen_socket, INVALID_SOCKET);
			if (socket != INVALID_SOCKET)
			{
				shutdown(socket, SD_BOTH);
				closesocket(socket);
			}
		}

		void cleanup_winsock()
		{
			if (std::exchange(winsock_started, false))
				WSACleanup();
		}

		void run(std::stop_token stop)
		{
			while (!stop.stop_requested())
			{
				fd_set readable;
				FD_ZERO(&readable);
				FD_SET(listen_socket, &readable);
				timeval timeout{0, 250'000};
				const auto selected = select(0, &readable, nullptr, nullptr, &timeout);
				if (selected <= 0 || stop.stop_requested())
					continue;
				sockaddr_storage remote{};
				int remote_size = sizeof(remote);
				const auto client = accept(
				    listen_socket,
				    reinterpret_cast<sockaddr *>(&remote),
				    &remote_size);
				if (client == INVALID_SOCKET)
					continue;
				handle(client, remote);
				shutdown(client, SD_BOTH);
				closesocket(client);
			}
		}

		void handle(SOCKET client, const sockaddr_storage &remote)
		{
			constexpr DWORD timeout_ms = 1500;
			setsockopt(
			    client,
			    SOL_SOCKET,
			    SO_RCVTIMEO,
			    reinterpret_cast<const char *>(&timeout_ms),
			    sizeof(timeout_ms));
			std::string raw;
			raw.reserve(4096);
			std::array<char, 4096> buffer{};
			while (raw.size() <= 16 * 1024
			    && raw.find("\r\n\r\n") == std::string::npos)
			{
				const auto received = recv(
				    client,
				    buffer.data(),
				    static_cast<int>(buffer.size()),
				    0);
				if (received <= 0)
					return;
				raw.append(buffer.data(), static_cast<std::size_t>(received));
			}
			if (raw.size() > 16 * 1024)
			{
				send_all(client, response(431, "Request Header Fields Too Large", "text/plain; charset=utf-8", "Request too large."));
				return;
			}
			const auto request = parse_request(raw);
			if (!request)
			{
				send_all(client, response(400, "Bad Request", "text/plain; charset=utf-8", "Malformed request."));
				return;
			}
			if (request->method != "GET")
			{
				send_all(client, response(405, "Method Not Allowed", "text/plain; charset=utf-8", "Read-only endpoint.", "\r\nAllow: GET"));
				return;
			}
			if (!rate_allowed(remote))
			{
				send_all(client, response(429, "Too Many Requests", "text/plain; charset=utf-8", "Rate limit exceeded.", "\r\nRetry-After: 30"));
				return;
			}
			if (request->target == "/" || request->target == "/index.html")
			{
				send_all(client, response(200, "OK", "text/html; charset=utf-8", dashboard_assets::index_html));
				return;
			}
			if (request->target == "/assets/dashboard.css")
			{
				send_all(client, response(200, "OK", "text/css; charset=utf-8", dashboard_assets::stylesheet));
				return;
			}
			if (request->target == "/assets/resources.css")
			{
				send_all(client, response(200, "OK", "text/css; charset=utf-8", dashboard_assets::resource_stylesheet));
				return;
			}
			if (request->target == "/assets/dashboard.js")
			{
				send_all(client, response(200, "OK", "text/javascript; charset=utf-8", dashboard_assets::javascript));
				return;
			}
			if (request->target != "/api/v1/snapshot")
			{
				send_all(client, response(404, "Not Found", "text/plain; charset=utf-8", "Not found."));
				return;
			}
			if (const auto fetch = request->headers.find("sec-fetch-site");
			    fetch != request->headers.end() && fetch->second == "cross-site")
			{
				send_all(client, response(403, "Forbidden", "text/plain; charset=utf-8", "Cross-site requests are not allowed."));
				return;
			}
			if (config.require_token)
			{
				const auto auth = request->headers.find("authorization");
				constexpr std::string_view prefix = "Bearer ";
				if (auth == request->headers.end()
				    || !std::string_view(auth->second).starts_with(prefix)
				    || !constant_time_equal(
				        std::string_view(auth->second).substr(prefix.size()), token))
				{
					send_all(client, response(401, "Unauthorized", "application/json; charset=utf-8", "{\"error\":\"unauthorized\"}", "\r\nWWW-Authenticate: Bearer"));
					return;
				}
			}
			const auto body = provider ? provider() : std::string("{}");
			send_all(client, response(200, "OK", "application/json; charset=utf-8", body));
		}

		bool rate_allowed(const sockaddr_storage &remote)
		{
			std::array<char, INET6_ADDRSTRLEN> address{};
			const void *source{};
			if (remote.ss_family == AF_INET)
				source = &reinterpret_cast<const sockaddr_in *>(&remote)->sin_addr;
			else if (remote.ss_family == AF_INET6)
				source = &reinterpret_cast<const sockaddr_in6 *>(&remote)->sin6_addr;
			const auto key = source && InetNtopA(
			    remote.ss_family,
			    const_cast<void *>(source),
			    address.data(),
			    static_cast<DWORD>(address.size()))
			    ? std::string(address.data())
			    : std::string("unknown");
			const auto now = request_clock::now();
			if (request_times.size() > 1024)
			{
				std::erase_if(
				    request_times,
				    [now](const auto &entry)
				    {
					    return entry.second.empty()
					        || now - entry.second.back() > std::chrono::minutes(1);
				    });
			}
			auto &requests = request_times[key];
			while (!requests.empty() && now - requests.front() > std::chrono::minutes(1))
				requests.pop_front();
			if (requests.size() >= config.max_requests_per_minute)
				return false;
			requests.push_back(now);
			return true;
		}

		dashboard_config config;
		snapshot_provider provider;
		std::string token;
		SOCKET listen_socket{INVALID_SOCKET};
		bool winsock_started{};
		std::jthread worker;
		std::unordered_map<std::string, std::deque<request_clock::time_point>>
		    request_times;
	};

	dashboard_server::dashboard_server(
	    dashboard_config config,
	    snapshot_provider provide_snapshot) :
	    m_impl(std::make_unique<implementation>(
	        std::move(config),
	        std::move(provide_snapshot)))
	{
	}

	dashboard_server::~dashboard_server() = default;

	void dashboard_server::start()
	{
		m_impl->start();
	}

	void dashboard_server::stop()
	{
		m_impl->stop();
	}

	std::string dashboard_server::endpoint() const
	{
		return m_impl->endpoint();
	}
}
