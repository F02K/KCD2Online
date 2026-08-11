#pragma once

#include "kcd2o.pb.h"
#include "multiplayer/protocol.hpp"
#include "resources/resource_cache.hpp"

#include <nlohmann/json_fwd.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace kcd2o
{
	struct client_resource_outgoing
	{
		protocol::Envelope envelope;
		reliability delivery{reliability::reliable};
	};

	class client_resources
	{
	public:
		struct instance;

		explicit client_resources(
		    std::filesystem::path cache_root = resources::resource_cache::default_path());
		~client_resources();
		client_resources(const client_resources &) = delete;
		client_resources &operator=(const client_resources &) = delete;

		[[nodiscard]] bool accept_manifest(
		    const protocol::ServerResourceManifest &manifest,
		    std::string_view server_id,
		    std::string &error);
		[[nodiscard]] bool accept_chunk(
		    const protocol::ServerResourceChunk &chunk,
		    std::string &error);
		void accept_event(const protocol::ServerResourceEvent &event);
		void accept_ui(const protocol::ServerUiUpdate &update);
		void accept_binding(const protocol::ServerInputBinding &binding);
		void connected();
		void tick(std::chrono::steady_clock::time_point now);
		void reset();

		[[nodiscard]] std::vector<client_resource_outgoing> take_outgoing();
		[[nodiscard]] std::string ui_snapshot_json() const;
		[[nodiscard]] protocol::ClientUiEvent make_ui_event(
		    std::string resource,
		    std::string document,
		    std::string control,
		    std::string event,
		    std::string payload_json);
		[[nodiscard]] bool emit_script_event(
		    std::string_view resource,
		    std::string_view event,
		    const nlohmann::json &payload,
		    bool reliable);

	private:
		struct package_state;
		void request_next();
		[[nodiscard]] bool activate(std::string &error);
		void queue(protocol::Envelope envelope,
		    reliability delivery = reliability::reliable);

		resources::resource_cache m_cache;
		std::string m_server_id;
		std::uint64_t m_generation{};
		std::string m_root_hash;
		std::vector<package_state> m_packages;
		std::size_t m_current_package{};
		std::vector<std::byte> m_download;
		std::vector<std::unique_ptr<instance>> m_instances;
		std::vector<client_resource_outgoing> m_outgoing;
		std::vector<client_resource_outgoing> m_deferred_events;
		bool m_connected{};
		std::uint64_t m_event_sequence{};

		mutable std::mutex m_ui_mutex;
		std::string m_ui_json{R"({"revision":0,"documents":[],"toasts":[],"bindings":[]})"};
		std::uint64_t m_ui_revision{};
		std::uint64_t m_ui_sequence{};
	};
}
