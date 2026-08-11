#ifdef NDEBUG
#undef NDEBUG
#endif

#include "multiplayer/client_resources.hpp"
#include "resources/resource_cache.hpp"
#include "resources/resource_package.hpp"
#include "resources/sha256.hpp"
#include "scripting/server_resource_runtime.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
	void write(const std::filesystem::path &path, std::string_view value)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary);
		output << value;
		assert(output.good());
	}
}

int main()
{
	using namespace kcd2o::resources;
	const auto root = std::filesystem::temp_directory_path()
	    / ("kcd2o-resource-test-" + std::to_string(GetCurrentProcessId()));
	std::filesystem::remove_all(root);

	write(root / "resources" / "server_only" / "resource.toml",
	    "[resource]\nid='server_only'\nversion='1.0.0'\n"
	    "[server]\nentry='server/main.lua'\ncapabilities=['chat', 'input']\n");
	write(root / "resources" / "server_only" / "server" / "main.lua",
	    "server.on('start', function() server.say('started') end)\n"
	    "input.on('ping', function(player, payload) server.say('key') end)\n");

	write(root / "resources" / "mixed" / "resource.toml",
	    "[resource]\nid='mixed'\nversion='1.2.3'\ndependencies=['server_only']\n"
	    "[server]\nentry='server/main.lua'\ncapabilities=[]\n"
	    "[client]\nentry='client/main.lua'\ncapabilities=[]\n"
	    "[shared]\nclient_paths=['shared/data.json']\n"
	    "[[events]]\nname='hello'\ndirection='bidirectional'\nreliable=false\n");
	write(root / "resources" / "mixed" / "server" / "main.lua", "SECRET=true\n");
	write(root / "resources" / "mixed" / "client" / "main.lua",
	    "events.emit_server('hello', { ready = true })\n");
	write(root / "resources" / "mixed" / "shared" / "data.json", "{\"ok\":true}\n");

	const auto set = load_resource_set(root / "resources");
	assert(set.definitions.size() == 2);
	assert(set.client_packages.size() == 1);
	assert(set.client_packages.front().resource_id == "mixed");
	assert(valid_sha256_hex(set.root_hash));
	const auto unpacked = unpack_client_package(
	    set.client_packages.front().bytes, set.client_packages.front().hash);
	assert(unpacked.files.contains("client/main.lua"));
	assert(unpacked.files.contains("shared/data.json"));
	assert(!unpacked.files.contains("server/main.lua"));
	std::uint32_t says{};
	kcd2o::scripting::server_resource_callbacks callbacks;
	callbacks.say = [&](std::string text, std::optional<std::uint64_t>)
	{
		assert(text == "started" || text == "key");
		++says;
	};
	kcd2o::scripting::server_resource_runtime runtime(
	    set, 4 * 1024 * 1024, 50'000, 3, std::move(callbacks));
	runtime.start();
	assert(says == 1);
	std::string script_error;
	assert(runtime.ui_event(42, "server_only", "", "ping", "key",
	    "{\"pressed\":true}", script_error));
	assert(says == 2);
	kcd2o::scripting::lua_sandbox limited(
	    "limits", 4 * 1024 * 1024, 10'000, {}, {});
	assert(!limited.execute("while true do end", "@loop.lua", script_error));
	assert(script_error.contains("instruction budget"));
	assert(!limited.execute("require('../secret')", "@escape.lua", script_error));
	assert(script_error.contains("invalid or unavailable module"));
	const std::string bytecode{"\x1bLua", 4};
	assert(!limited.execute(bytecode, "@bytecode.lua", script_error));
	assert(script_error.contains("bytecode"));
	try
	{
		(void)kcd2o::scripting::lua_sandbox::parse_json(
		    std::string(17, '[') + "0" + std::string(17, ']'));
		assert(false);
	}
	catch (const std::exception &) {}

	kcd2o::client_resources client(root / "client-cache");
	kcd2o::protocol::ServerResourceManifest manifest;
	manifest.set_generation(set.generation);
	manifest.set_root_sha256(set.root_hash);
	manifest.set_total_size(set.client_packages.front().bytes.size());
	auto *manifest_package = manifest.add_packages();
	manifest_package->set_resource_id(set.client_packages.front().resource_id);
	manifest_package->set_version(set.client_packages.front().version);
	manifest_package->set_sha256(set.client_packages.front().hash);
	manifest_package->set_size(set.client_packages.front().bytes.size());
	manifest_package->set_client_entry(set.client_packages.front().client_entry);
	std::string client_error;
	assert(client.accept_manifest(manifest, "test-server", client_error));
	auto outgoing = client.take_outgoing();
	assert(outgoing.size() == 1
	    && outgoing.front().envelope.has_client_resource_request());
	std::size_t offset{};
	while (offset < set.client_packages.front().bytes.size())
	{
		const auto count = std::min<std::size_t>(resource_chunk_bytes,
		    set.client_packages.front().bytes.size() - offset);
		kcd2o::protocol::ServerResourceChunk chunk;
		chunk.set_package_sha256(set.client_packages.front().hash);
		chunk.set_offset(offset);
		chunk.set_total_size(set.client_packages.front().bytes.size());
		chunk.set_data(set.client_packages.front().bytes.data() + offset, count);
		chunk.set_final(offset + count == set.client_packages.front().bytes.size());
		assert(client.accept_chunk(chunk, client_error));
		offset += count;
		outgoing = client.take_outgoing();
		assert(outgoing.size() == 1);
		assert(offset == set.client_packages.front().bytes.size()
		    ? outgoing.front().envelope.has_client_resources_ready()
		    : outgoing.front().envelope.has_client_resource_request());
	}
	assert(std::filesystem::exists(root / "client-cache" / "servers"));
	client.connected();
	outgoing = client.take_outgoing();
	assert(outgoing.size() == 1
	    && outgoing.front().envelope.has_client_resource_event());
	assert(outgoing.front().delivery == kcd2o::reliability::unreliable);

	resource_cache cache(root / "cache");
	cache.store(set.client_packages.front().hash,
	    set.client_packages.front().bytes);
	assert(cache.load(set.client_packages.front().hash).has_value());
	const std::vector<std::string> hashes{set.client_packages.front().hash};
	cache.activate("test-server", set.root_hash, hashes);
	assert(std::filesystem::exists(root / "cache" / "servers"));

	std::filesystem::remove_all(root);
	return 0;
}
