#include "multiplayer/client_resources.hpp"
#include "resources/sha256.hpp"
#include "scripting/lua_sandbox.hpp"

extern "C"
{
#include <lauxlib.h>
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace kcd2o
{
	struct client_resources::package_state
	{
		std::string resource_id;
		std::string version;
		std::string hash;
		std::string client_entry;
		std::uint64_t size{};
	};

	struct client_resources::instance
	{
		client_resources *runtime{};
		resources::unpacked_package package;
		std::unique_ptr<scripting::lua_sandbox> lua;
		std::unordered_map<std::string, std::vector<int>> events;
		std::unordered_map<std::string, std::vector<int>> inputs;
		std::vector<int> ui_callbacks;
		std::uint64_t enabled_features{};
		std::uint32_t errors{};
		bool disabled{};
	};

	namespace
	{
		client_resources::instance *self(lua_State *state)
		{
			auto *sandbox = scripting::lua_sandbox::from(state);
			return sandbox
			    ? static_cast<client_resources::instance *>(sandbox->owner())
			    : nullptr;
		}

		bool has_client_capability(
		    const client_resources::instance &resource,
		    std::string_view capability)
		{
			return std::ranges::find(
			    resource.package.definition.client_capabilities, capability)
			    != resource.package.definition.client_capabilities.end();
		}

		void need_client_capability(lua_State *state, std::string_view capability)
		{
			auto *resource = self(state);
			if (!resource || !has_client_capability(*resource, capability))
				luaL_error(state, "client capability '%s' is not declared",
				    std::string(capability).c_str());
		}

		const resources::event_definition *find_event(
		    const resources::resource_definition &definition,
		    std::string_view name)
		{
			const auto found = std::ranges::find(definition.events, name,
			    &resources::event_definition::name);
			return found == definition.events.end() ? nullptr : &*found;
		}

		int api_events_on(lua_State *state)
		{
			auto *resource = self(state);
			const auto *name = luaL_checkstring(state, 1);
			luaL_checktype(state, 2, LUA_TFUNCTION);
			const auto *event = resource ? find_event(resource->package.definition, name) : nullptr;
			if (!event || event->direction == resources::event_direction::client_to_server)
				return luaL_error(state, "server event is not declared");
			resource->events[name].push_back(resource->lua->reference_function(2));
			return 0;
		}

		int api_events_emit(lua_State *state)
		{
			auto *resource = self(state);
			const auto *name = luaL_checkstring(state, 1);
			const auto *event = resource ? find_event(resource->package.definition, name) : nullptr;
			if (!event || event->direction == resources::event_direction::server_to_client)
				return luaL_error(state, "client event is not declared");
			nlohmann::json payload = nullptr;
			try
			{
				if (!lua_isnoneornil(state, 2))
					payload = scripting::lua_sandbox::to_json(state, 2, event->max_bytes);
			}
			catch (const std::exception &exception)
			{
				return luaL_error(state, "%s", exception.what());
			}
			if (!resource->runtime->emit_script_event(
			        resource->package.definition.id, name, payload, event->reliable))
				return luaL_error(state, "client resource event queue is full");
			return 0;
		}

		int api_features_enable(lua_State *state)
		{
			auto *resource = self(state);
			const auto *name = luaL_checkstring(state, 1);
			const auto feature = resources::builtin_feature_from_name(name ? name : "");
			if (!resource || !feature)
				return luaL_error(state, "unknown built-in feature");
			const auto capability = resources::builtin_feature_capability(*feature);
			if (std::ranges::find(resource->package.definition.client_capabilities,
			        capability)
			    == resource->package.definition.client_capabilities.end())
				return luaL_error(state, "client feature capability is not declared");
			resource->enabled_features |= resources::feature_bit(*feature);
			return 0;
		}

		int api_input_on(lua_State *state)
		{
			need_client_capability(state, "input");
			auto *resource = self(state);
			const auto *action = luaL_checkstring(state, 1);
			luaL_checktype(state, 2, LUA_TFUNCTION);
			if (!resources::valid_resource_event_name(action))
				return luaL_error(state, "invalid local input action id");
			resource->inputs[action].push_back(
			    resource->lua->reference_function(2));
			return 0;
		}

		int api_input_bind(lua_State *state)
		{
			need_client_capability(state, "input");
			auto *resource = self(state);
			const auto *action = luaL_checkstring(state, 1);
			size_t label_size{};
			const auto *label = luaL_checklstring(state, 2, &label_size);
			const auto key = static_cast<std::uint32_t>(luaL_checkinteger(state, 3));
			const bool emit_release = lua_toboolean(state, 4) != 0;
			if (!resources::valid_resource_event_name(action) || label_size == 0
			    || label_size > 384 || key == 0 || key > 255 || key == 0x77)
				return luaL_error(state, "invalid local input binding");
			if (!resource->runtime->register_local_binding(
			        resource->package.definition.id, action,
			        std::string_view(label, label_size), key, emit_release))
				return luaL_error(state, "local input binding limit exceeded");
			return 0;
		}

		protocol::VoiceRange checked_voice_range(lua_State *state, int index)
		{
			if (lua_isnoneornil(state, index))
				return protocol::VOICE_RANGE_NORMAL;
			const auto value = std::string_view(luaL_checkstring(state, index));
			if (value == "whisper") return protocol::VOICE_RANGE_WHISPER;
			if (value == "normal") return protocol::VOICE_RANGE_NORMAL;
			if (value == "shout") return protocol::VOICE_RANGE_SHOUT;
			luaL_error(state, "voice range must be whisper, normal, or shout");
			return protocol::VOICE_RANGE_NORMAL;
		}

		int api_voice_set_transmitting(lua_State *state)
		{
			need_client_capability(state, "voice");
			auto *resource = self(state);
			if ((resource->enabled_features
			        & resources::feature_bit(resources::builtin_feature::voice_chat)) == 0)
				return luaL_error(state, "voice_chat feature is not enabled");
			resource->runtime->set_voice_transmitting(
			    lua_toboolean(state, 1) != 0, checked_voice_range(state, 2));
			return 0;
		}

		int local_ui_update(lua_State *state, std::string_view operation)
		{
			need_client_capability(state, "ui");
			auto *resource = self(state);
			const auto *document = luaL_checkstring(state, 1);
			if (!resources::valid_resource_event_name(document))
				return luaL_error(state, "invalid local UI document id");
			nlohmann::json payload = nlohmann::json::object();
			try
			{
				if (operation != "close")
					payload = scripting::lua_sandbox::to_json(state, 2);
			}
			catch (const std::exception &exception)
			{
				return luaL_error(state, "%s", exception.what());
			}
			if (!resource->runtime->update_local_ui(
			        resource->package.definition.id, document, operation, payload))
				return luaL_error(state, "local UI update is invalid or too large");
			return 0;
		}

		int api_ui_show(lua_State *state) { return local_ui_update(state, "show"); }
		int api_ui_patch(lua_State *state) { return local_ui_update(state, "patch"); }
		int api_ui_close(lua_State *state) { return local_ui_update(state, "close"); }
		int api_ui_toast(lua_State *state) { return local_ui_update(state, "toast"); }

		int api_ui_on(lua_State *state)
		{
			need_client_capability(state, "ui");
			auto *resource = self(state);
			luaL_checktype(state, 1, LUA_TFUNCTION);
			resource->ui_callbacks.push_back(
			    resource->lua->reference_function(1));
			return 0;
		}

		int api_ui_toggle_panel(lua_State *state)
		{
			need_client_capability(state, "ui");
			auto *resource = self(state);
			const auto target = std::string_view(luaL_checkstring(state, 1));
			if (target != "player_hub" && target != "social_menu"
			    && target != "staff_menu")
				return luaL_error(state, "unknown native panel primitive");
			if (!resource->runtime->queue_local_ui_command(
			        resource->package.definition.id, "toggle_panel", target))
				return luaL_error(state, "local UI command limit exceeded");
			return 0;
		}

		void install_api(lua_State *state)
		{
			lua_newtable(state);
			lua_pushcfunction(state, api_events_on);
			lua_setfield(state, -2, "on");
			lua_pushcfunction(state, api_events_emit);
			lua_setfield(state, -2, "emit_server");
			lua_setglobal(state, "events");

			lua_newtable(state);
			lua_pushcfunction(state, api_features_enable);
			lua_setfield(state, -2, "enable");
			lua_setglobal(state, "features");

			lua_newtable(state);
			lua_pushcfunction(state, api_input_on);
			lua_setfield(state, -2, "on");
			lua_pushcfunction(state, api_input_bind);
			lua_setfield(state, -2, "bind");
			lua_setglobal(state, "input");

			lua_newtable(state);
			lua_pushcfunction(state, api_voice_set_transmitting);
			lua_setfield(state, -2, "set_transmitting");
			lua_setglobal(state, "voice");

			lua_newtable(state);
			lua_pushcfunction(state, api_ui_show);
			lua_setfield(state, -2, "show");
			lua_pushcfunction(state, api_ui_patch);
			lua_setfield(state, -2, "patch");
			lua_pushcfunction(state, api_ui_close);
			lua_setfield(state, -2, "close");
			lua_pushcfunction(state, api_ui_toast);
			lua_setfield(state, -2, "toast");
			lua_pushcfunction(state, api_ui_on);
			lua_setfield(state, -2, "on");
			lua_pushcfunction(state, api_ui_toggle_panel);
			lua_setfield(state, -2, "toggle_panel");
			lua_setglobal(state, "ui");
		}

		nlohmann::json base_ui(std::uint64_t revision)
		{
		return {{"revision", revision}, {"documents", nlohmann::json::array()},
			    {"toasts", nlohmann::json::array()},
			    {"bindings", nlohmann::json::array()},
			    {"commands", nlohmann::json::array()}};
		}

		constexpr std::size_t maximum_ui_documents = 64;
		constexpr std::size_t maximum_ui_bindings = 128;
		constexpr std::size_t maximum_ui_state_bytes = 256 * 1024;
		constexpr std::size_t maximum_queued_script_events = 256;
	}

	client_resources::client_resources(std::filesystem::path cache_root,
	    client_resource_callbacks callbacks) :
	    m_cache(std::move(cache_root)),
	    m_callbacks(std::move(callbacks))
	{
	}
	client_resources::~client_resources() { reset(); }

	bool client_resources::accept_manifest(
	    const protocol::ServerResourceManifest &manifest,
	    std::string_view server_id,
	    std::string &error)
	{
		reset();
		if (!resources::valid_sha256_hex(manifest.root_sha256())
		    || manifest.packages_size() > static_cast<int>(resources::maximum_resources)
		    || manifest.total_size() > resources::maximum_resource_set_bytes)
		{
			error = "invalid resource manifest";
			return false;
		}
		m_server_id = server_id;
		m_generation = manifest.generation();
		m_root_hash = manifest.root_sha256();
		std::uint64_t total{};
		std::unordered_set<std::string> resource_ids;
		std::unordered_set<std::string> package_hashes;
		for (const auto &package : manifest.packages())
		{
			if (!resources::valid_resource_id(package.resource_id())
			    || !resources::valid_sha256_hex(package.sha256())
			    || !resource_ids.insert(package.resource_id()).second
			    || !package_hashes.insert(package.sha256()).second
			    || package.size() == 0
			    || package.size() > resources::maximum_resource_package_bytes)
			{
				error = "invalid resource package metadata";
				return false;
			}
			total += package.size();
			m_packages.push_back({package.resource_id(), package.version(),
			    package.sha256(), package.client_entry(), package.size()});
		}
		if (total != manifest.total_size())
		{
			error = "resource manifest size mismatch";
			return false;
		}
		request_next();
		return true;
	}

	void client_resources::request_next()
	{
		while (m_current_package < m_packages.size()
		    && m_cache.load(m_packages[m_current_package].hash).has_value())
			++m_current_package;
		if (m_current_package == m_packages.size())
		{
			std::string error;
			if (!activate(error))
				throw std::runtime_error(error);
			protocol::Envelope ready;
			auto *message = ready.mutable_client_resources_ready();
			message->set_generation(m_generation);
			message->set_root_sha256(m_root_hash);
			queue(std::move(ready));
			return;
		}
		m_download.clear();
		protocol::Envelope request;
		auto *message = request.mutable_client_resource_request();
		message->set_root_sha256(m_root_hash);
		message->set_package_sha256(m_packages[m_current_package].hash);
		message->set_offset(0);
		queue(std::move(request));
	}

	bool client_resources::accept_chunk(
	    const protocol::ServerResourceChunk &chunk,
	    std::string &error)
	{
		if (m_current_package >= m_packages.size())
		{
			error = "unexpected resource chunk";
			return false;
		}
		const auto &package = m_packages[m_current_package];
		if (chunk.package_sha256() != package.hash
		    || chunk.total_size() != package.size
		    || chunk.offset() != m_download.size()
		    || chunk.data().empty()
		    || chunk.data().size() > resources::resource_chunk_bytes
		    || chunk.offset() + chunk.data().size() > package.size
		    || chunk.final() != (chunk.offset() + chunk.data().size() == package.size))
		{
			error = "invalid resource chunk sequence";
			return false;
		}
		const auto *begin = reinterpret_cast<const std::byte *>(chunk.data().data());
		m_download.insert(m_download.end(), begin, begin + chunk.data().size());
		if (!chunk.final())
		{
			protocol::Envelope request;
			auto *message = request.mutable_client_resource_request();
			message->set_root_sha256(m_root_hash);
			message->set_package_sha256(package.hash);
			message->set_offset(m_download.size());
			queue(std::move(request));
			return true;
		}
		try { m_cache.store(package.hash, m_download); }
		catch (const std::exception &exception) { error = exception.what(); return false; }
		++m_current_package;
		request_next();
		return true;
	}

	bool client_resources::activate(std::string &error)
	{
		std::vector<std::string> hashes;
		m_feature_mask.store(0, std::memory_order_release);
		try
		{
			for (const auto &metadata : m_packages)
			{
				auto bytes = m_cache.load(metadata.hash);
				if (!bytes)
					throw std::runtime_error("cached resource disappeared");
				auto package = resources::unpack_client_package(*bytes, metadata.hash);
				if (package.definition.id != metadata.resource_id
				    || package.definition.client_entry.value_or("") != metadata.client_entry)
					throw std::runtime_error("resource metadata does not match package");
				auto instance = std::make_unique<client_resources::instance>();
				instance->runtime = this;
				instance->package = std::move(package);
				auto *raw = instance.get();
				instance->lua = std::make_unique<scripting::lua_sandbox>(
				    raw->package.definition.id, 16 * 1024 * 1024, 150000,
				    [](std::string) {},
				    [raw](std::string_view module)
				        -> std::optional<std::vector<std::byte>>
				    {
					    const auto found = raw->package.files.find("client/" + std::string(module));
					    return found == raw->package.files.end()
					        ? std::nullopt : std::optional{found->second};
				    }, raw);
				install_api(instance->lua->state());
				const auto entry = *instance->package.definition.client_entry;
				const auto found = instance->package.files.find(entry);
				if (found == instance->package.files.end())
					throw std::runtime_error("client entry is absent from resource package");
				std::string script_error;
				const std::string_view source(
				    reinterpret_cast<const char *>(found->second.data()), found->second.size());
				if (!instance->lua->execute(source, "@" + entry, script_error))
					throw std::runtime_error(raw->package.definition.id + ": " + script_error);
				m_feature_mask.fetch_or(
				    instance->enabled_features, std::memory_order_acq_rel);
				m_instances.push_back(std::move(instance));
				hashes.push_back(metadata.hash);
			}
			m_cache.activate(m_server_id, m_root_hash, hashes);
			return true;
		}
		catch (const std::exception &exception)
		{
			error = exception.what();
			m_instances.clear();
			m_feature_mask.store(0, std::memory_order_release);
			return false;
		}
	}

	void client_resources::accept_event(const protocol::ServerResourceEvent &event)
	{
		const auto found = std::ranges::find_if(m_instances,
		    [&](const auto &item) { return item->package.definition.id == event.resource_id(); });
		if (found == m_instances.end())
			return;
		if ((*found)->disabled)
			return;
		const auto callbacks = (*found)->events.find(event.event());
		if (callbacks == (*found)->events.end())
			return;
		nlohmann::json payload;
		try
		{
			payload = scripting::lua_sandbox::parse_json(
			    event.payload_json(), 32 * 1024);
		}
		catch (...) { return; }
		const auto callback_list = callbacks->second;
		for (const auto callback : callback_list)
		{
			std::string error;
			if (!(*found)->lua->call(callback,
			    [&](lua_State *state) { scripting::lua_sandbox::push_json(state, payload); return 1; },
			    0, error)
			    && ++(*found)->errors >= 3)
			{
				(*found)->disabled = true;
				m_feature_mask.fetch_and(
				    ~(*found)->enabled_features, std::memory_order_acq_rel);
				break;
			}
		}
	}

	void client_resources::accept_ui(const protocol::ServerUiUpdate &update)
	{
		std::scoped_lock lock(m_ui_mutex);
		auto state = nlohmann::json::parse(m_ui_json);
		auto &documents = state["documents"];
		const auto found = std::ranges::find_if(documents, [&](const auto &document)
		{
			return document.value("resource_id", "") == update.resource_id()
			    && document.value("document_id", "") == update.document_id();
		});
		if (update.operation() == protocol::SERVER_UI_OPERATION_CLOSE)
		{
			if (found != documents.end()) documents.erase(found);
		}
		else
		{
			nlohmann::json payload;
			try
			{
				payload = scripting::lua_sandbox::parse_json(update.payload_json());
			}
			catch (...) { return; }
			if (update.operation() == protocol::SERVER_UI_OPERATION_TOAST)
			{
				state["toasts"].push_back({{"resource_id", update.resource_id()},
				    {"payload", std::move(payload)}, {"revision", update.revision()}});
				while (state["toasts"].size() > 8) state["toasts"].erase(state["toasts"].begin());
			}
			else if (found == documents.end()
			    && documents.size() < maximum_ui_documents)
				documents.push_back({{"resource_id", update.resource_id()},
				    {"document_id", update.document_id()}, {"spec", std::move(payload)}});
			else if (update.operation() == protocol::SERVER_UI_OPERATION_PATCH)
				(*found)["spec"].merge_patch(payload);
			else
				(*found)["spec"] = std::move(payload);
		}
		state["revision"] = m_ui_revision + 1;
		auto serialized = state.dump();
		if (serialized.size() <= maximum_ui_state_bytes)
		{
			++m_ui_revision;
			m_ui_json = std::move(serialized);
		}
	}

	void client_resources::accept_binding(const protocol::ServerInputBinding &binding)
	{
		std::scoped_lock lock(m_ui_mutex);
		auto state = nlohmann::json::parse(m_ui_json);
		auto &bindings = state["bindings"];
		const auto found = std::ranges::find_if(bindings, [&](const auto &item)
		{
			return item.value("resource_id", "") == binding.resource_id()
			    && item.value("action_id", "") == binding.action_id();
		});
		if (binding.unregister())
		{
			if (found != bindings.end()) bindings.erase(found);
		}
		else
		{
			nlohmann::json value{{"resource_id", binding.resource_id()},
			    {"action_id", binding.action_id()}, {"label", binding.label()},
			    {"virtual_key", binding.default_virtual_key()}};
			if (found == bindings.end())
			{
				if (bindings.size() < maximum_ui_bindings)
					bindings.push_back(std::move(value));
			}
			else
				*found = std::move(value);
		}
		state["revision"] = m_ui_revision + 1;
		auto serialized = state.dump();
		if (serialized.size() <= maximum_ui_state_bytes)
		{
			++m_ui_revision;
			m_ui_json = std::move(serialized);
		}
	}

	void client_resources::connected()
	{
		m_connected = true;
		for (auto &event : m_deferred_events)
			m_outgoing.push_back(std::move(event));
		m_deferred_events.clear();
	}

	void client_resources::tick(std::chrono::steady_clock::time_point) {}

	void client_resources::reset()
	{
		set_voice_transmitting(false, protocol::VOICE_RANGE_NORMAL);
		m_instances.clear();
		m_packages.clear();
		m_outgoing.clear();
		m_deferred_events.clear();
		m_connected = false;
		m_download.clear();
		m_current_package = 0;
		m_generation = 0;
		m_root_hash.clear();
		m_server_id.clear();
		m_feature_mask.store(0, std::memory_order_release);
		std::scoped_lock lock(m_ui_mutex);
		m_ui_json = base_ui(++m_ui_revision).dump();
	}

	std::uint64_t client_resources::feature_mask() const noexcept
	{
		return m_feature_mask.load(std::memory_order_acquire);
	}

	void client_resources::queue(protocol::Envelope envelope, reliability delivery)
	{
		m_outgoing.push_back({std::move(envelope), delivery});
	}

	std::vector<client_resource_outgoing> client_resources::take_outgoing()
	{
		auto result = std::move(m_outgoing);
		m_outgoing.clear();
		return result;
	}

	std::string client_resources::ui_snapshot_json() const
	{
		std::scoped_lock lock(m_ui_mutex);
		return m_ui_json;
	}

	protocol::ClientUiEvent client_resources::make_ui_event(std::string resource,
	    std::string document, std::string control, std::string event,
	    std::string payload_json)
	{
		std::scoped_lock lock(m_ui_mutex);
		protocol::ClientUiEvent message;
		message.set_resource_id(std::move(resource));
		message.set_document_id(std::move(document));
		message.set_control_id(std::move(control));
		message.set_event(std::move(event));
		message.set_payload_json(std::move(payload_json));
		message.set_revision(m_ui_revision);
		message.set_sequence(++m_ui_sequence);
		return message;
	}

	bool client_resources::emit_script_event(std::string_view resource,
	    std::string_view event, const nlohmann::json &payload, bool reliable)
	{
		if (m_outgoing.size() + m_deferred_events.size()
		    >= maximum_queued_script_events)
			return false;
		protocol::Envelope envelope;
		auto *message = envelope.mutable_client_resource_event();
		message->set_resource_id(resource);
		message->set_event(event);
		message->set_payload_json(payload.dump());
		message->set_sequence(++m_event_sequence);
		if (m_connected)
			queue(std::move(envelope),
			    reliable ? reliability::reliable : reliability::unreliable);
		else
			m_deferred_events.push_back({std::move(envelope),
			    reliable ? reliability::reliable : reliability::unreliable});
		return true;
	}

	bool client_resources::dispatch_local_ui_event(
	    std::string_view resource_id, std::string_view document,
	    std::string_view control, std::string_view event,
	    std::string_view payload_json)
	{
		const auto found = std::ranges::find_if(m_instances,
		    [&](const auto &item)
		    {
			    return item->package.definition.id == resource_id;
		    });
		if (found == m_instances.end() || (*found)->disabled)
			return false;

		nlohmann::json payload;
		try
		{
			payload = scripting::lua_sandbox::parse_json(payload_json);
		}
		catch (...) { return false; }

		std::vector<int> callbacks;
		if (event == "key" && document.empty())
		{
			const auto action = (*found)->inputs.find(std::string(control));
			if (action == (*found)->inputs.end()) return false;
			callbacks = action->second;
		}
		else
		{
			if ((*found)->ui_callbacks.empty()) return false;
			callbacks = (*found)->ui_callbacks;
		}

		for (const auto callback : callbacks)
		{
			std::string error;
			const auto called = (*found)->lua->call(callback,
			    [&](lua_State *state)
			    {
				    if (event == "key" && document.empty())
				    {
					    scripting::lua_sandbox::push_json(state, payload);
					    return 1;
				    }
				    lua_pushlstring(state, document.data(), document.size());
				    lua_pushlstring(state, control.data(), control.size());
				    lua_pushlstring(state, event.data(), event.size());
				    scripting::lua_sandbox::push_json(state, payload);
				    return 4;
			    }, 0, error);
			if (!called && ++(*found)->errors >= 3)
			{
				(*found)->disabled = true;
				m_feature_mask.fetch_and(
				    ~(*found)->enabled_features, std::memory_order_acq_rel);
				if (((*found)->enabled_features
				        & resources::feature_bit(
				            resources::builtin_feature::voice_chat)) != 0)
					set_voice_transmitting(false, protocol::VOICE_RANGE_NORMAL);
				break;
			}
		}
		return true;
	}

	bool client_resources::register_local_binding(std::string_view resource,
	    std::string_view action, std::string_view label,
	    std::uint32_t virtual_key, bool emit_release)
	{
		std::scoped_lock lock(m_ui_mutex);
		auto state = nlohmann::json::parse(m_ui_json);
		auto &bindings = state["bindings"];
		const auto found = std::ranges::find_if(bindings, [&](const auto &item)
		{
			return item.value("resource_id", "") == resource
			    && item.value("action_id", "") == action;
		});
		nlohmann::json value{{"resource_id", resource}, {"action_id", action},
		    {"label", label}, {"virtual_key", virtual_key}, {"local", true},
		    {"emit_release", emit_release}};
		if (found == bindings.end())
		{
			if (bindings.size() >= maximum_ui_bindings) return false;
			bindings.push_back(std::move(value));
		}
		else
			*found = std::move(value);
		state["revision"] = m_ui_revision + 1;
		auto serialized = state.dump();
		if (serialized.size() > maximum_ui_state_bytes) return false;
		++m_ui_revision;
		m_ui_json = std::move(serialized);
		return true;
	}

	bool client_resources::update_local_ui(std::string_view resource,
	    std::string_view document, std::string_view operation,
	    const nlohmann::json &payload)
	{
		std::scoped_lock lock(m_ui_mutex);
		auto state = nlohmann::json::parse(m_ui_json);
		auto &documents = state["documents"];
		const auto found = std::ranges::find_if(documents, [&](const auto &item)
		{
			return item.value("resource_id", "") == resource
			    && item.value("document_id", "") == document;
		});
		if (operation == "close")
		{
			if (found != documents.end()) documents.erase(found);
		}
		else if (operation == "toast")
		{
			state["toasts"].push_back({{"resource_id", resource},
			    {"payload", payload}, {"revision", m_ui_revision + 1}});
			while (state["toasts"].size() > 8)
				state["toasts"].erase(state["toasts"].begin());
		}
		else if (!payload.is_object())
			return false;
		else if (operation == "patch" && found != documents.end())
			(*found)["spec"].merge_patch(payload);
		else if (operation == "show")
		{
			if (found == documents.end())
			{
				if (documents.size() >= maximum_ui_documents) return false;
				documents.push_back({{"resource_id", resource},
				    {"document_id", document}, {"spec", payload}});
			}
			else
				(*found)["spec"] = payload;
		}
		else
			return false;

		state["revision"] = m_ui_revision + 1;
		auto serialized = state.dump();
		if (serialized.size() > maximum_ui_state_bytes) return false;
		++m_ui_revision;
		m_ui_json = std::move(serialized);
		return true;
	}

	bool client_resources::queue_local_ui_command(std::string_view resource,
	    std::string_view command, std::string_view target)
	{
		std::scoped_lock lock(m_ui_mutex);
		auto state = nlohmann::json::parse(m_ui_json);
		auto &commands = state["commands"];
		commands.push_back({{"resource_id", resource}, {"command", command},
		    {"target", target}, {"sequence", ++m_ui_sequence}});
		while (commands.size() > 32) commands.erase(commands.begin());
		state["revision"] = m_ui_revision + 1;
		auto serialized = state.dump();
		if (serialized.size() > maximum_ui_state_bytes) return false;
		++m_ui_revision;
		m_ui_json = std::move(serialized);
		return true;
	}

	void client_resources::set_voice_transmitting(
	    bool transmitting, protocol::VoiceRange range) const
	{
		if (m_callbacks.set_voice_transmitting)
			m_callbacks.set_voice_transmitting(transmitting, range);
	}
}
