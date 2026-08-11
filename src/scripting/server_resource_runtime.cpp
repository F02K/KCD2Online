#include "scripting/server_resource_runtime.hpp"

extern "C"
{
#include <lauxlib.h>
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace kcd2o::scripting
{
	namespace
	{
		using clock = std::chrono::steady_clock;

		std::string read_text(const std::filesystem::path &path)
		{
			std::ifstream input(path, std::ios::binary);
			if (!input)
				throw std::runtime_error("could not open Lua source: " + path.string());
			return {std::istreambuf_iterator<char>(input), {}};
		}

		bool has_capability(
		    const resources::resource_definition &definition,
		    std::string_view capability)
		{
			return std::ranges::find(definition.server_capabilities, capability)
			    != definition.server_capabilities.end();
		}

		const resources::event_definition *find_event(
		    const resources::resource_definition &definition,
		    std::string_view name)
		{
			const auto found = std::ranges::find(definition.events, name,
			    &resources::event_definition::name);
			return found == definition.events.end() ? nullptr : &*found;
		}
	}

	struct server_resource_runtime::instance
	{
		struct timer
		{
			std::uint64_t id{};
			clock::time_point due;
			std::chrono::milliseconds interval{};
			int callback{LUA_NOREF};
		};

		server_resource_runtime *runtime{};
		server_resource_callbacks *callbacks{};
		const resources::resource_definition *definition{};
		std::unique_ptr<lua_sandbox> lua;
		std::unordered_map<std::string, std::vector<int>> lifecycle;
		std::unordered_map<std::string, std::vector<int>> events;
		std::unordered_map<std::string, std::vector<int>> inputs;
		std::vector<timer> timers;
		std::uint64_t next_timer{1};
		std::uint64_t next_ui_revision{1};
		std::uint32_t errors{};
		bool disabled{};
	};

	namespace
	{
		server_resource_runtime::instance *self(lua_State *state)
		{
			auto *sandbox = lua_sandbox::from(state);
			return sandbox
			    ? static_cast<server_resource_runtime::instance *>(sandbox->owner())
			    : nullptr;
		}

		void need_capability(lua_State *state, std::string_view capability)
		{
			auto *resource = self(state);
			if (!resource || !has_capability(*resource->definition, capability))
				luaL_error(state, "resource capability '%s' is not declared",
				    std::string(capability).c_str());
		}

		std::optional<std::uint64_t> optional_player(lua_State *state, int index)
		{
			if (lua_isnoneornil(state, index))
				return std::nullopt;
			const auto value = luaL_checkinteger(state, index);
			if (value == 0)
				return std::nullopt;
			if (value < 0)
				luaL_error(state, "player id must be positive");
			return static_cast<std::uint64_t>(value);
		}

		std::uint64_t required_player(lua_State *state, int index)
		{
			const auto value = luaL_checkinteger(state, index);
			if (value <= 0)
				luaL_error(state, "player id must be positive");
			return static_cast<std::uint64_t>(value);
		}

		using callback_registry = std::unordered_map<std::string, std::vector<int>>;

		int register_callback(
		    lua_State *state,
		    callback_registry server_resource_runtime::instance::*member)
		{
			auto *resource = self(state);
			const auto *name = luaL_checkstring(state, 1);
			luaL_checktype(state, 2, LUA_TFUNCTION);
			if (!resource || !resources::valid_resource_event_name(name))
				return luaL_error(state, "invalid callback name");
			(resource->*member)[name].push_back(resource->lua->reference_function(2));
			return 0;
		}

		int api_server_on(lua_State *state)
		{
			static const std::unordered_set<std::string_view> lifecycle_events{
			    "start", "player_joined", "player_left", "chat",
			    "player_death", "ui"};
			const auto *name = luaL_checkstring(state, 1);
			if (!lifecycle_events.contains(name))
				return luaL_error(state, "unknown server lifecycle event");
			if (std::string_view(name) == "ui")
				need_capability(state, "ui");
			return register_callback(state, &server_resource_runtime::instance::lifecycle);
		}

		int api_events_on(lua_State *state)
		{
			auto *resource = self(state);
			const auto *name = luaL_checkstring(state, 1);
			const auto *event = resource ? find_event(*resource->definition, name) : nullptr;
			if (!event || event->direction == resources::event_direction::server_to_client)
				return luaL_error(state, "client event is not declared");
			return register_callback(state, &server_resource_runtime::instance::events);
		}

		int api_input_on(lua_State *state)
		{
			need_capability(state, "input");
			return register_callback(state, &server_resource_runtime::instance::inputs);
		}

		int api_server_players(lua_State *state)
		{
			auto *resource = self(state);
			const auto players = resource->callbacks->players
			    ? resource->callbacks->players()
			    : std::vector<script_player>{};
			nlohmann::json json = nlohmann::json::array();
			for (const auto &player : players)
				json.push_back({{"id", player.id}, {"name", player.display_name},
				    {"connected", player.connected}, {"role", player.role}});
			lua_sandbox::push_json(state, json);
			return 1;
		}

		int api_server_say(lua_State *state)
		{
			need_capability(state, "chat");
			auto *resource = self(state);
			const auto *text = luaL_checkstring(state, 1);
			const auto player = optional_player(state, 2);
			if (resource->callbacks->say)
				resource->callbacks->say(text, player);
			return 0;
		}

		int api_server_kick(lua_State *state)
		{
			need_capability(state, "players.kick");
			auto *resource = self(state);
			const auto player = required_player(state, 1);
			const auto *reason = luaL_optstring(state, 2, "removed by a resource");
			if (resource->callbacks->kick)
				resource->callbacks->kick(player, reason);
			return 0;
		}

		int api_events_emit(lua_State *state)
		{
			auto *resource = self(state);
			const auto player = optional_player(state, 1);
			const auto *name = luaL_checkstring(state, 2);
			const auto *event = resource ? find_event(*resource->definition, name) : nullptr;
			if (!event || event->direction == resources::event_direction::client_to_server)
				return luaL_error(state, "server event is not declared");
			nlohmann::json payload = nullptr;
			try
			{
				if (!lua_isnoneornil(state, 3))
					payload = lua_sandbox::to_json(state, 3, event->max_bytes);
			}
			catch (const std::exception &exception)
			{
				return luaL_error(state, "%s", exception.what());
			}
			if (resource->callbacks->send_event)
				resource->callbacks->send_event(
				    resource->definition->id, player, name, payload, event->reliable);
			return 0;
		}

		int send_ui(lua_State *state, std::string_view operation)
		{
			need_capability(state, "ui");
			auto *resource = self(state);
			const auto player = required_player(state, 1);
			const auto *document = luaL_checkstring(state, 2);
			if (!resources::valid_resource_event_name(document))
				return luaL_error(state, "invalid UI document id");
			nlohmann::json payload = nlohmann::json::object();
			try
			{
				if (!lua_isnoneornil(state, 3))
					payload = lua_sandbox::to_json(state, 3);
			}
			catch (const std::exception &exception)
			{
				return luaL_error(state, "%s", exception.what());
			}
			if (resource->callbacks->send_ui)
				resource->callbacks->send_ui(resource->definition->id,
				    player, document, operation, payload);
			return 0;
		}

		int api_ui_show(lua_State *state) { return send_ui(state, "show"); }
		int api_ui_patch(lua_State *state) { return send_ui(state, "patch"); }
		int api_ui_close(lua_State *state) { return send_ui(state, "close"); }
		int api_ui_toast(lua_State *state) { return send_ui(state, "toast"); }

		int api_input_register(lua_State *state)
		{
			need_capability(state, "input");
			auto *resource = self(state);
			const auto player = required_player(state, 1);
			const auto *action = luaL_checkstring(state, 2);
			std::size_t label_length{};
			const auto *label = luaL_checklstring(state, 3, &label_length);
			const auto key = static_cast<std::uint32_t>(luaL_checkinteger(state, 4));
			if (!resources::valid_resource_event_name(action) || key == 0
			    || key > 255 || key == 0x77 || label_length == 0
			    || label_length > 384)
				return luaL_error(state, "invalid input binding");
			if (resource->callbacks->send_binding)
				resource->callbacks->send_binding(resource->definition->id,
				    player, action, label, key, false);
			return 0;
		}

		int api_input_unregister(lua_State *state)
		{
			need_capability(state, "input");
			auto *resource = self(state);
			const auto player = required_player(state, 1);
			const auto *action = luaL_checkstring(state, 2);
			if (!resources::valid_resource_event_name(action))
				return luaL_error(state, "invalid input action id");
			if (resource->callbacks->send_binding)
				resource->callbacks->send_binding(resource->definition->id,
				    player, action, {}, 0, true);
			return 0;
		}

		int add_timer(lua_State *state, bool repeating)
		{
			auto *resource = self(state);
			const auto delay = luaL_checkinteger(state, 1);
			luaL_checktype(state, 2, LUA_TFUNCTION);
			if (delay < 1 || delay > 24 * 60 * 60 * 1000)
				return luaL_error(state, "timer delay must be 1..86400000 ms");
			const auto id = resource->next_timer++;
			resource->timers.push_back({id, clock::now() + std::chrono::milliseconds(delay),
			    repeating ? std::chrono::milliseconds(delay) : std::chrono::milliseconds{},
			    resource->lua->reference_function(2)});
			lua_pushnumber(state, static_cast<lua_Number>(id));
			return 1;
		}

		int api_timer_after(lua_State *state) { return add_timer(state, false); }
		int api_timer_every(lua_State *state) { return add_timer(state, true); }

		void install_table(lua_State *state, const char *name,
		    std::initializer_list<std::pair<const char *, lua_CFunction>> functions)
		{
			lua_newtable(state);
			for (const auto &[function_name, function] : functions)
			{
				lua_pushcfunction(state, function);
				lua_setfield(state, -2, function_name);
			}
			lua_setglobal(state, name);
		}
	}

	server_resource_runtime::server_resource_runtime(
	    const resources::resource_set &resources,
	    std::size_t memory_limit_per_resource,
	    std::uint64_t instruction_limit,
	    std::uint32_t error_limit,
	    server_resource_callbacks callbacks) :
	    m_resources(resources),
	    m_memory_limit(memory_limit_per_resource),
	    m_instruction_limit(instruction_limit),
	    m_error_limit(error_limit),
	    m_callbacks(std::move(callbacks))
	{
	}

	server_resource_runtime::~server_resource_runtime() { stop(); }

	void server_resource_runtime::start()
	{
		stop();
		for (const auto &definition : m_resources.definitions)
		{
			if (!definition.server_entry)
				continue;
			auto resource = std::make_unique<instance>();
			resource->runtime = this;
			resource->callbacks = &m_callbacks;
			resource->definition = &definition;
			const auto root = definition.root;
			resource->lua = std::make_unique<lua_sandbox>(definition.id,
			    m_memory_limit, m_instruction_limit, m_callbacks.log,
			    [root](std::string_view module) -> std::optional<std::vector<std::byte>>
			    {
				const auto path = root / "server" / std::filesystem::path(module);
				if (!std::filesystem::is_regular_file(path))
					return std::nullopt;
				const auto text = read_text(path);
				return std::vector<std::byte>(
				    reinterpret_cast<const std::byte *>(text.data()),
				    reinterpret_cast<const std::byte *>(text.data() + text.size()));
			    }, resource.get());
			auto *state = resource->lua->state();
			install_table(state, "server", {{"on", api_server_on},
			    {"players", api_server_players}, {"say", api_server_say},
			    {"kick", api_server_kick}});
			install_table(state, "events", {{"on", api_events_on},
			    {"emit_client", api_events_emit}});
			install_table(state, "ui", {{"show", api_ui_show}, {"patch", api_ui_patch},
			    {"close", api_ui_close}, {"toast", api_ui_toast}});
			install_table(state, "input", {{"on", api_input_on},
			    {"register", api_input_register}, {"unregister", api_input_unregister}});
			install_table(state, "timer", {{"after", api_timer_after},
			    {"every", api_timer_every}});
			std::string error;
			const auto source = read_text(definition.root / *definition.server_entry);
			if (!resource->lua->execute(source, "@" + *definition.server_entry, error))
				throw std::runtime_error("resource " + definition.id + " failed: " + error);
			m_instances.push_back(std::move(resource));
		}
		for (auto &resource : m_instances)
		{
			const auto found = resource->lifecycle.find("start");
			if (found == resource->lifecycle.end())
				continue;
			const auto callbacks = found->second;
			for (const auto callback : callbacks)
			{
				std::string error;
				if (!resource->lua->call(callback, {}, 0, error) && m_callbacks.log)
					m_callbacks.log("[" + resource->definition->id + "] start: " + error);
			}
		}
	}

	void server_resource_runtime::stop()
	{
		for (auto &resource : m_instances)
			for (auto &timer : resource->timers)
				resource->lua->release(timer.callback);
		m_instances.clear();
	}

	void server_resource_runtime::tick(clock::time_point now)
	{
		for (auto &resource : m_instances)
		{
			if (resource->disabled)
				continue;
			for (std::size_t index{}; index < resource->timers.size();)
			{
				if (now < resource->timers[index].due)
				{
					++index;
					continue;
				}
				const auto timer_id = resource->timers[index].id;
				const auto callback = resource->timers[index].callback;
				std::string error;
				if (!resource->lua->call(callback, {}, 0, error))
				{
					if (m_callbacks.log)
						m_callbacks.log("[" + resource->definition->id + "] timer: " + error);
					if (++resource->errors >= m_error_limit)
						resource->disabled = true;
				}
				const auto current = std::ranges::find(
				    resource->timers, timer_id, &instance::timer::id);
				if (current == resource->timers.end())
					continue;
				index = static_cast<std::size_t>(
				    std::distance(resource->timers.begin(), current));
				if (current->interval.count() > 0 && !resource->disabled)
				{
					current->due = now + current->interval;
					++index;
				}
				else
				{
					resource->lua->release(current->callback);
					resource->timers.erase(resource->timers.begin() + index);
				}
			}
		}
	}

	namespace
	{
		void dispatch(server_resource_runtime::instance &resource,
		    std::unordered_map<std::string, std::vector<int>> &registry,
		    std::string_view name,
		    const std::function<int(lua_State *)> &arguments,
		    std::uint32_t error_limit,
		    const server_resource_callbacks &callbacks)
		{
			if (resource.disabled)
				return;
			const auto found = registry.find(std::string(name));
			if (found == registry.end())
				return;
			const auto callback_list = found->second;
			for (const auto callback : callback_list)
			{
				std::string error;
				if (!resource.lua->call(callback, arguments, 0, error))
				{
					if (callbacks.log)
						callbacks.log("[" + resource.definition->id + "] "
						    + std::string(name) + ": " + error);
					if (++resource.errors >= error_limit)
					{
						resource.disabled = true;
						break;
					}
				}
			}
		}

		void push_player(lua_State *state, const script_player &player)
		{
			lua_sandbox::push_json(state, {{"id", player.id},
			    {"name", player.display_name}, {"connected", player.connected},
			    {"role", player.role}});
		}
	}

	void server_resource_runtime::player_joined(const script_player &player)
	{
		for (auto &resource : m_instances)
			dispatch(*resource, resource->lifecycle, "player_joined",
			    [&](lua_State *state) { push_player(state, player); return 1; },
			    m_error_limit, m_callbacks);
	}

	void server_resource_runtime::player_left(
	    const script_player &player, std::string_view reason)
	{
		for (auto &resource : m_instances)
			dispatch(*resource, resource->lifecycle, "player_left",
			    [&](lua_State *state) { push_player(state, player);
				lua_pushlstring(state, reason.data(), reason.size()); return 2; },
			    m_error_limit, m_callbacks);
	}

	void server_resource_runtime::chat(std::uint64_t player, std::string_view text)
	{
		for (auto &resource : m_instances)
			dispatch(*resource, resource->lifecycle, "chat",
			    [&](lua_State *state) { lua_pushnumber(state, player);
				lua_pushlstring(state, text.data(), text.size()); return 2; },
			    m_error_limit, m_callbacks);
	}

	void server_resource_runtime::death(std::uint64_t player)
	{
		for (auto &resource : m_instances)
			dispatch(*resource, resource->lifecycle, "player_death",
			    [&](lua_State *state) { lua_pushnumber(state, player); return 1; },
			    m_error_limit, m_callbacks);
	}

	bool server_resource_runtime::client_event(std::uint64_t player,
	    std::string_view resource_id, std::string_view event_name,
	    std::string_view payload_json, std::string &error)
	{
		const auto found = std::ranges::find_if(m_instances,
		    [&](const auto &item) { return item->definition->id == resource_id; });
		if (found == m_instances.end())
		{
			error = "unknown server resource";
			return false;
		}
		const auto *event = find_event(*(*found)->definition, event_name);
		if (!event || event->direction == resources::event_direction::server_to_client
		    || payload_json.size() > event->max_bytes)
		{
			error = "resource event is not declared for client-to-server use";
			return false;
		}
		nlohmann::json payload;
		try
		{
			payload = lua_sandbox::parse_json(payload_json, event->max_bytes);
		}
		catch (...) { error = "resource event payload is not valid JSON"; return false; }
		dispatch(**found, (*found)->events, event_name,
		    [&](lua_State *state) { lua_pushnumber(state, player);
			lua_sandbox::push_json(state, payload); return 2; },
		    m_error_limit, m_callbacks);
		return true;
	}

	bool server_resource_runtime::ui_event(std::uint64_t player,
	    std::string_view resource_id, std::string_view document,
	    std::string_view control, std::string_view event,
	    std::string_view payload_json, std::string &error)
	{
		const auto found = std::ranges::find_if(m_instances,
		    [&](const auto &item) { return item->definition->id == resource_id; });
		const bool key_event = event == "key";
		if (found == m_instances.end()
		    || !has_capability(*(*found)->definition,
		        key_event ? "input" : "ui"))
		{
			error = "unknown resource UI or input action";
			return false;
		}
		nlohmann::json payload;
		try
		{
			payload = lua_sandbox::parse_json(payload_json);
		}
		catch (...) { error = "UI event payload is not valid JSON"; return false; }
		auto &registry = key_event ? (*found)->inputs : (*found)->lifecycle;
		const auto callback_name = key_event ? std::string(control) : std::string("ui");
		dispatch(**found, registry, callback_name,
		    [&](lua_State *state)
		    {
			lua_pushnumber(state, player);
			if (key_event)
			{
				lua_sandbox::push_json(state, payload);
				return 2;
			}
			lua_pushlstring(state, document.data(), document.size());
			lua_pushlstring(state, control.data(), control.size());
			lua_pushlstring(state, event.data(), event.size());
			lua_sandbox::push_json(state, payload);
			return 5;
		    },
		    m_error_limit, m_callbacks);
		return true;
	}
}
