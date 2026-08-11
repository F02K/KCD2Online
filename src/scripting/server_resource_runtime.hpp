#pragma once

#include "resources/resource_package.hpp"
#include "scripting/lua_sandbox.hpp"

#include <nlohmann/json_fwd.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kcd2o::scripting
{
	struct script_player
	{
		std::uint64_t id{};
		std::string display_name;
		bool connected{};
		std::string role;
	};

	struct server_resource_callbacks
	{
		std::function<void(std::string)> log;
		std::function<std::vector<script_player>()> players;
		std::function<void(std::string, std::optional<std::uint64_t>)> say;
		std::function<void(std::uint64_t, std::string)> kick;
		std::function<void(
		    std::string_view, std::optional<std::uint64_t>, std::string_view,
		    const nlohmann::json &, bool)> send_event;
		std::function<void(
		    std::string_view, std::uint64_t, std::string_view, std::string_view,
		    const nlohmann::json &)> send_ui;
		std::function<void(
		    std::string_view, std::uint64_t, std::string_view, std::string_view,
		    std::uint32_t, bool)> send_binding;
	};

	class server_resource_runtime
	{
	public:
		struct instance;

		server_resource_runtime(
		    const resources::resource_set &resources,
		    std::size_t memory_limit_per_resource,
		    std::uint64_t instruction_limit,
		    std::uint32_t error_limit,
		    server_resource_callbacks callbacks);
		~server_resource_runtime();
		server_resource_runtime(const server_resource_runtime &) = delete;
		server_resource_runtime &operator=(const server_resource_runtime &) = delete;

		void start();
		void stop();
		void tick(std::chrono::steady_clock::time_point now);
		void player_joined(const script_player &player);
		void player_left(const script_player &player, std::string_view reason);
		void chat(std::uint64_t player, std::string_view text);
		void death(std::uint64_t player);
		[[nodiscard]] bool client_event(
		    std::uint64_t player,
		    std::string_view resource,
		    std::string_view event,
		    std::string_view payload_json,
		    std::string &error);
		[[nodiscard]] bool ui_event(
		    std::uint64_t player,
		    std::string_view resource,
		    std::string_view document,
		    std::string_view control,
		    std::string_view event,
		    std::string_view payload_json,
		    std::string &error);

	private:
		const resources::resource_set &m_resources;
		std::size_t m_memory_limit{};
		std::uint64_t m_instruction_limit{};
		std::uint32_t m_error_limit{};
		server_resource_callbacks m_callbacks;
		std::vector<std::unique_ptr<instance>> m_instances;
	};
}
