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

		void install_events(lua_State *state)
		{
			lua_newtable(state);
			lua_pushcfunction(state, api_events_on);
			lua_setfield(state, -2, "on");
			lua_pushcfunction(state, api_events_emit);
			lua_setfield(state, -2, "emit_server");
			lua_setglobal(state, "events");
		}

		nlohmann::json base_ui(std::uint64_t revision)
		{
			return {{"revision", revision}, {"documents", nlohmann::json::array()},
			    {"toasts", nlohmann::json::array()},
			    {"bindings", nlohmann::json::array()}};
		}

		constexpr std::size_t maximum_ui_documents = 64;
		constexpr std::size_t maximum_ui_bindings = 128;
		constexpr std::size_t maximum_ui_state_bytes = 256 * 1024;
		constexpr std::size_t maximum_queued_script_events = 256;
	}

	client_resources::client_resources(std::filesystem::path cache_root) :
	    m_cache(std::move(cache_root))
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
				install_events(instance->lua->state());
				const auto entry = *instance->package.definition.client_entry;
				const auto found = instance->package.files.find(entry);
				if (found == instance->package.files.end())
					throw std::runtime_error("client entry is absent from resource package");
				std::string script_error;
				const std::string_view source(
				    reinterpret_cast<const char *>(found->second.data()), found->second.size());
				if (!instance->lua->execute(source, "@" + entry, script_error))
					throw std::runtime_error(raw->package.definition.id + ": " + script_error);
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
		std::scoped_lock lock(m_ui_mutex);
		m_ui_json = base_ui(++m_ui_revision).dump();
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
}
