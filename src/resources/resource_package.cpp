#include "resources/resource_package.hpp"

#include "resources/sha256.hpp"

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <ranges>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace kcd2o::resources
{
	namespace
	{
		constexpr std::array<std::byte, 8> package_magic{
		    std::byte{'K'}, std::byte{'2'}, std::byte{'R'}, std::byte{'E'},
		    std::byte{'S'}, std::byte{1}, std::byte{0}, std::byte{0}};

		[[noreturn]] void invalid(std::string message)
		{
			throw std::runtime_error("invalid resource: " + std::move(message));
		}

		void validate_json_nesting(std::string_view text)
		{
			std::size_t depth{};
			bool quoted{};
			bool escaped{};
			for (const char character : text)
			{
				if (quoted)
				{
					if (escaped) escaped = false;
					else if (character == '\\') escaped = true;
					else if (character == '"') quoted = false;
					continue;
				}
				if (character == '"') quoted = true;
				else if (character == '{' || character == '[')
				{
					if (++depth > 16)
						invalid("client manifest nesting is too deep");
				}
				else if ((character == '}' || character == ']') && depth > 0)
					--depth;
			}
		}

		bool valid_version(std::string_view value)
		{
			return !value.empty() && value.size() <= 32
			    && std::ranges::all_of(value, [](unsigned char character)
			       {
				       return std::isalnum(character) || character == '.'
				           || character == '-' || character == '+';
			       });
		}

		bool safe_relative_path(std::string_view value)
		{
			if (value.empty() || value.size() > 240 || value.front() == '/'
			    || value.front() == '\\' || value.contains(':')
			    || value.contains('\\'))
				return false;
			std::filesystem::path path{value};
			if (path.is_absolute())
				return false;
			for (const auto &part : path)
				if (part == "." || part == ".." || part.empty())
					return false;
			return true;
		}

		std::vector<std::string> string_array(
		    const toml::table &table,
		    std::string_view key)
		{
			std::vector<std::string> result;
			if (const auto *array = table[key].as_array())
			{
				for (const auto &entry : *array)
				{
					const auto value = entry.value<std::string>();
					if (!value)
						invalid(std::string(key) + " must contain strings");
					result.push_back(*value);
				}
			}
			return result;
		}

		std::vector<std::byte> read_file(
		    const std::filesystem::path &path,
		    std::size_t limit)
		{
			std::error_code error;
			const auto size = std::filesystem::file_size(path, error);
			if (error || size > limit)
				invalid("file is unreadable or too large: " + path.string());
			std::ifstream input(path, std::ios::binary);
			if (!input)
				invalid("could not open file: " + path.string());
			std::vector<std::byte> result(static_cast<std::size_t>(size));
			if (!result.empty())
				input.read(
				    reinterpret_cast<char *>(result.data()),
				    static_cast<std::streamsize>(result.size()));
			if (!input)
				invalid("could not read file: " + path.string());
			return result;
		}

		template<typename Integer>
		void append_integer(std::vector<std::byte> &output, Integer value)
		{
			static_assert(std::is_unsigned_v<Integer>);
			for (std::size_t index{}; index < sizeof(Integer); ++index)
				output.push_back(static_cast<std::byte>(
				    (value >> (index * 8)) & static_cast<Integer>(0xFF)));
		}

		void append_bytes(
		    std::vector<std::byte> &output,
		    std::span<const std::byte> bytes)
		{
			output.insert(output.end(), bytes.begin(), bytes.end());
		}

		void append_string(std::vector<std::byte> &output, std::string_view value)
		{
			append_bytes(output, {
			    reinterpret_cast<const std::byte *>(value.data()), value.size()});
		}

		template<typename Integer>
		Integer take_integer(std::span<const std::byte> &input)
		{
			static_assert(std::is_unsigned_v<Integer>);
			if (input.size() < sizeof(Integer))
				invalid("package is truncated");
			Integer result{};
			for (std::size_t index{}; index < sizeof(Integer); ++index)
				result |= static_cast<Integer>(
				    std::to_integer<unsigned char>(input[index])) << (index * 8);
			input = input.subspan(sizeof(Integer));
			return result;
		}

		std::span<const std::byte> take_bytes(
		    std::span<const std::byte> &input,
		    std::size_t count)
		{
			if (count > input.size())
				invalid("package is truncated");
			const auto result = input.first(count);
			input = input.subspan(count);
			return result;
		}

		std::string take_string(
		    std::span<const std::byte> &input,
		    std::size_t count)
		{
			const auto bytes = take_bytes(input, count);
			return {
			    reinterpret_cast<const char *>(bytes.data()), bytes.size()};
		}

		nlohmann::json definition_json(const resource_definition &definition)
		{
			nlohmann::json events = nlohmann::json::array();
			for (const auto &event : definition.events)
			{
				const auto direction = event.direction == event_direction::client_to_server
				    ? "client_to_server"
				    : event.direction == event_direction::server_to_client
				        ? "server_to_client"
				        : "bidirectional";
				events.push_back({
				    {"name", event.name},
				    {"direction", direction},
				    {"reliable", event.reliable},
				    {"max_per_second", event.max_per_second},
				    {"max_bytes", event.max_bytes}});
			}
			return {
			    {"id", definition.id},
			    {"version", definition.version},
			    {"api_version", definition.api_version},
			    {"client_entry", definition.client_entry.value_or("")},
			    {"dependencies", definition.dependencies},
			    {"client_capabilities", definition.client_capabilities},
			    {"events", std::move(events)}};
		}

		client_package build_client_package(const resource_definition &definition)
		{
			if (!definition.client_entry)
				invalid(definition.id + " has no client entry");

			std::vector<std::filesystem::path> sources;
			const auto add_tree = [&](const std::filesystem::path &root)
			{
				if (!std::filesystem::exists(root))
					return;
				if (std::filesystem::is_symlink(
				        std::filesystem::symlink_status(root)))
					invalid(definition.id + " contains a client symlink");
				if (std::filesystem::is_regular_file(root))
				{
					sources.push_back(root);
					return;
				}
				for (const auto &entry :
				     std::filesystem::recursive_directory_iterator(root))
				{
					if (entry.is_symlink())
						invalid(definition.id + " contains a client symlink");
					if (entry.is_regular_file())
						sources.push_back(entry.path());
				}
			};
			add_tree(definition.root / "client");
			for (const auto &shared : definition.shared_client_paths)
			{
				if (!safe_relative_path(shared)
				    || !std::string_view(shared).starts_with("shared/"))
					invalid(definition.id + " has an unsafe shared client path");
				add_tree(definition.root / std::filesystem::path(shared));
			}

			std::ranges::sort(sources, {}, [&](const auto &path)
			{
				return std::filesystem::relative(path, definition.root)
				    .generic_string();
			});
			if (sources.size() > maximum_resource_files)
				invalid(definition.id + " has too many client files");

			std::vector<package_file> files;
			std::size_t total{};
			std::unordered_set<std::string> paths;
			for (const auto &source : sources)
			{
				auto relative = std::filesystem::relative(source, definition.root)
				                    .generic_string();
				if (!safe_relative_path(relative) || !paths.insert(relative).second)
					invalid(definition.id + " contains an unsafe client path");
				auto bytes = read_file(source, maximum_resource_file_bytes);
				total += bytes.size();
				if (total > maximum_resource_package_bytes)
					invalid(definition.id + " client package is too large");
				files.push_back({
				    std::move(relative), std::move(bytes), {}});
				files.back().hash = sha256_hex(files.back().bytes);
			}
			if (!paths.contains(*definition.client_entry))
				invalid(definition.id + " client entry does not exist");

			const auto manifest = definition_json(definition).dump();
			if (manifest.size() > 64 * 1024)
				invalid(definition.id + " client manifest is too large");
			std::vector<std::byte> archive;
			archive.reserve(total + manifest.size() + files.size() * 128 + 32);
			append_bytes(archive, package_magic);
			append_integer<std::uint32_t>(
			    archive, static_cast<std::uint32_t>(manifest.size()));
			append_string(archive, manifest);
			append_integer<std::uint32_t>(
			    archive, static_cast<std::uint32_t>(files.size()));
			for (const auto &file : files)
			{
				append_integer<std::uint16_t>(
				    archive, static_cast<std::uint16_t>(file.path.size()));
				append_string(archive, file.path);
				append_integer<std::uint64_t>(archive, file.bytes.size());
				const auto digest = sha256(file.bytes);
				append_bytes(archive, digest);
				append_bytes(archive, file.bytes);
			}
			if (archive.size() > maximum_resource_package_bytes)
				invalid(definition.id + " packaged client archive is too large");
			client_package result{
			    definition.id,
			    definition.version,
			    *definition.client_entry,
			    {},
			    std::move(archive)};
			result.hash = sha256_hex(result.bytes);
			return result;
		}

		resource_definition definition_from_json(const nlohmann::json &json)
		{
			resource_definition result;
			result.id = json.at("id").get<std::string>();
			result.version = json.at("version").get<std::string>();
			result.api_version = json.at("api_version").get<std::uint32_t>();
			const auto entry = json.at("client_entry").get<std::string>();
			if (!entry.empty())
				result.client_entry = entry;
			result.dependencies = json.value(
			    "dependencies", std::vector<std::string>{});
			result.client_capabilities = json.value(
			    "client_capabilities", std::vector<std::string>{});
			for (const auto &item : json.value("events", nlohmann::json::array()))
			{
				event_definition event;
				event.name = item.at("name").get<std::string>();
				const auto direction = item.value("direction", "bidirectional");
				event.direction = direction == "client_to_server"
				    ? event_direction::client_to_server
				    : direction == "server_to_client"
				        ? event_direction::server_to_client
				        : direction == "bidirectional"
				            ? event_direction::bidirectional
				            : throw std::runtime_error("invalid event direction");
				event.reliable = item.value("reliable", true);
				event.max_per_second = item.value("max_per_second", 10U);
				event.max_bytes = item.value("max_bytes", 4096U);
				result.events.push_back(std::move(event));
			}
			return result;
		}
	}

	const client_package *resource_set::package(std::string_view hash) const noexcept
	{
		const auto found = std::ranges::find(client_packages, hash, &client_package::hash);
		return found == client_packages.end() ? nullptr : &*found;
	}

	const resource_definition *resource_set::definition(
	    std::string_view id) const noexcept
	{
		const auto found = std::ranges::find(definitions, id, &resource_definition::id);
		return found == definitions.end() ? nullptr : &*found;
	}

	bool valid_resource_id(std::string_view value) noexcept
	{
		return !value.empty() && value.size() <= 64
		    && std::isalnum(static_cast<unsigned char>(value.front()))
		    && std::ranges::all_of(value, [](unsigned char character)
		       {
			       return std::islower(character) || std::isdigit(character)
			           || character == '.' || character == '_' || character == '-';
		       });
	}

	bool valid_resource_event_name(std::string_view value) noexcept
	{
		return !value.empty() && value.size() <= 64
		    && std::isalpha(static_cast<unsigned char>(value.front()))
		    && std::ranges::all_of(value, [](unsigned char character)
		       {
			       return std::isalnum(character) || character == '_'
			           || character == '.' || character == '-';
		       });
	}

	resource_definition load_resource_definition(
	    const std::filesystem::path &resource_root)
	{
		for (const auto &entry :
		     std::filesystem::recursive_directory_iterator(resource_root))
			if (entry.is_symlink())
				invalid(resource_root.string() + " contains a symlink");
		const auto manifest_path = resource_root / "resource.toml";
		const auto document = toml::parse_file(manifest_path.string());
		const auto *resource = document["resource"].as_table();
		if (!resource)
			invalid(manifest_path.string() + " is missing [resource]");

		resource_definition result;
		result.root = std::filesystem::absolute(resource_root).lexically_normal();
		result.id = (*resource)["id"].value_or(std::string{});
		result.version = (*resource)["version"].value_or(std::string{});
		result.api_version = (*resource)["api_version"].value<std::uint32_t>()
		                         .value_or(resource_api_version);
		if (!valid_resource_id(result.id))
			invalid(manifest_path.string() + " has an invalid resource id");
		if (!valid_version(result.version))
			invalid(result.id + " has an invalid version");
		if (result.api_version != resource_api_version)
			invalid(result.id + " requires an unsupported API version");
		result.dependencies = string_array(*resource, "dependencies");

		if (const auto *server = document["server"].as_table())
		{
			if (const auto entry = (*server)["entry"].value<std::string>())
				result.server_entry = *entry;
			result.server_capabilities = string_array(*server, "capabilities");
		}
		if (const auto *client = document["client"].as_table())
		{
			if (const auto entry = (*client)["entry"].value<std::string>())
				result.client_entry = *entry;
			result.client_capabilities = string_array(*client, "capabilities");
		}
		if (const auto *shared = document["shared"].as_table())
			result.shared_client_paths = string_array(*shared, "client_paths");

		const std::unordered_set<std::string> known_server_capabilities{
		    "chat", "ui", "input", "players.kick"};
		std::unordered_set<std::string> unique_capabilities;
		for (const auto &capability : result.server_capabilities)
			if (!known_server_capabilities.contains(capability)
			    || !unique_capabilities.insert(capability).second)
				invalid(result.id + " has an invalid or duplicate server capability: "
				    + capability);
		if (!result.client_capabilities.empty())
			invalid(result.id + " requests unsupported client capabilities");
		std::unordered_set<std::string> unique_dependencies;
		for (const auto &dependency : result.dependencies)
			if (!valid_resource_id(dependency)
			    || !unique_dependencies.insert(dependency).second)
				invalid(result.id + " has an invalid or duplicate dependency");

		for (const auto *entry : {result.server_entry ? &*result.server_entry : nullptr,
		                         result.client_entry ? &*result.client_entry : nullptr})
		{
			if (!entry)
				continue;
			if (!safe_relative_path(*entry))
				invalid(result.id + " has an unsafe entry path");
		}
		if (result.server_entry
		    && !std::string_view(*result.server_entry).starts_with("server/"))
			invalid(result.id + " server entry must be below server/");
		if (result.client_entry
		    && !std::string_view(*result.client_entry).starts_with("client/"))
			invalid(result.id + " client entry must be below client/");
		if (result.server_entry
		    && !std::filesystem::is_regular_file(result.root / *result.server_entry))
			invalid(result.id + " server entry does not exist");

		if (const auto *events = document["events"].as_array())
		{
			std::unordered_set<std::string> names;
			for (const auto &node : *events)
			{
				const auto *table = node.as_table();
				if (!table)
					invalid(result.id + " event must be a table");
				event_definition event;
				event.name = (*table)["name"].value_or(std::string{});
				const auto direction = (*table)["direction"].value_or(
				    std::string{"bidirectional"});
				if (direction == "client_to_server")
					event.direction = event_direction::client_to_server;
				else if (direction == "server_to_client")
					event.direction = event_direction::server_to_client;
				else if (direction != "bidirectional")
					invalid(result.id + " event has invalid direction");
				event.reliable = (*table)["reliable"].value_or(true);
				event.max_per_second = (*table)["max_per_second"].value_or<std::uint32_t>(10);
				event.max_bytes = (*table)["max_bytes"].value_or<std::uint32_t>(4096);
				if (!valid_resource_event_name(event.name)
				    || !names.insert(event.name).second || event.max_per_second == 0
				    || event.max_per_second > 100 || event.max_bytes == 0
				    || event.max_bytes > maximum_resource_event_bytes)
					invalid(result.id + " has an invalid event definition");
				result.events.push_back(std::move(event));
			}
		}
		return result;
	}

	resource_set load_resource_set(const std::filesystem::path &directory)
	{
		resource_set result;
		if (!std::filesystem::exists(directory))
		{
			result.root_hash = sha256_hex({});
			return result;
		}
		if (!std::filesystem::is_directory(directory))
			invalid("resource root is not a directory: " + directory.string());
		for (const auto &entry : std::filesystem::directory_iterator(directory))
			if (entry.is_directory()
			    && std::filesystem::is_regular_file(entry.path() / "resource.toml"))
				result.definitions.push_back(load_resource_definition(entry.path()));
		if (result.definitions.size() > maximum_resources)
			invalid("server has too many resources");
		std::ranges::sort(result.definitions, {}, &resource_definition::id);
		for (std::size_t index{1}; index < result.definitions.size(); ++index)
			if (result.definitions[index - 1].id == result.definitions[index].id)
				invalid("duplicate resource id: " + result.definitions[index].id);

		std::unordered_set<std::string> available;
		for (const auto &definition : result.definitions)
			available.insert(definition.id);
		for (const auto &definition : result.definitions)
			for (const auto &dependency : definition.dependencies)
				if (!available.contains(dependency) || dependency == definition.id)
					invalid(definition.id + " has an invalid dependency: " + dependency);

		std::vector<resource_definition> ordered;
		std::unordered_set<std::string> emitted;
		while (ordered.size() != result.definitions.size())
		{
			bool progress = false;
			for (const auto &definition : result.definitions)
			{
				if (emitted.contains(definition.id)
				    || !std::ranges::all_of(definition.dependencies,
				        [&](const auto &dependency) { return emitted.contains(dependency); }))
					continue;
				ordered.push_back(definition);
				emitted.insert(definition.id);
				progress = true;
			}
			if (!progress)
				invalid("resource dependency graph contains a cycle");
		}
		result.definitions = std::move(ordered);

		std::size_t total{};
		for (const auto &definition : result.definitions)
		{
			if (!definition.client_entry)
				continue;
			auto package = build_client_package(definition);
			total += package.bytes.size();
			if (total > maximum_resource_set_bytes)
				invalid("client resource set is too large");
			result.client_packages.push_back(std::move(package));
		}

		nlohmann::json canonical = nlohmann::json::array();
		for (const auto &definition : result.definitions)
		{
			const auto package = std::ranges::find(
			    result.client_packages, definition.id, &client_package::resource_id);
			canonical.push_back({
			    {"id", definition.id},
			    {"version", definition.version},
			    {"api_version", definition.api_version},
			    {"dependencies", definition.dependencies},
			    {"client_hash", package == result.client_packages.end()
			         ? std::string{}
			         : package->hash}});
		}
		const auto canonical_text = canonical.dump();
		result.root_hash = sha256_hex({
		    reinterpret_cast<const std::byte *>(canonical_text.data()),
		    canonical_text.size()});
		std::uint64_t generation{};
		(void)std::from_chars(
		    result.root_hash.data(), result.root_hash.data() + 16,
		    generation, 16);
		result.generation = generation == 0 ? 1 : generation;
		return result;
	}

	unpacked_package unpack_client_package(
	    std::span<const std::byte> bytes,
	    std::string_view expected_hash)
	{
		if (bytes.empty() || bytes.size() > maximum_resource_package_bytes)
			invalid("client package size is invalid");
		if (!expected_hash.empty()
		    && (!valid_sha256_hex(expected_hash)
		        || sha256_hex(bytes) != expected_hash))
			invalid("client package hash mismatch");
		auto input = bytes;
		const auto magic = take_bytes(input, package_magic.size());
		if (!std::ranges::equal(magic, package_magic))
			invalid("client package magic is invalid");
		const auto manifest_size = take_integer<std::uint32_t>(input);
		if (manifest_size == 0 || manifest_size > 64 * 1024)
			invalid("client package manifest size is invalid");
		const auto manifest_text = take_string(input, manifest_size);
		unpacked_package result;
		try
		{
			validate_json_nesting(manifest_text);
			result.definition = definition_from_json(
			    nlohmann::json::parse(manifest_text));
		}
		catch (const std::exception &exception)
		{
			invalid(std::string("client manifest is invalid: ") + exception.what());
		}
		if (!valid_resource_id(result.definition.id)
		    || !valid_version(result.definition.version)
		    || result.definition.api_version != resource_api_version
		    || !result.definition.client_entry
		    || !safe_relative_path(*result.definition.client_entry)
		    || !std::string_view(*result.definition.client_entry).starts_with("client/")
		    || !result.definition.client_capabilities.empty())
			invalid("client manifest fields are invalid");
		std::unordered_set<std::string> dependencies;
		for (const auto &dependency : result.definition.dependencies)
			if (!valid_resource_id(dependency)
			    || !dependencies.insert(dependency).second)
				invalid("client manifest dependencies are invalid");
		std::unordered_set<std::string> events;
		for (const auto &event : result.definition.events)
			if (!valid_resource_event_name(event.name)
			    || !events.insert(event.name).second
			    || event.max_per_second == 0 || event.max_per_second > 100
			    || event.max_bytes == 0
			    || event.max_bytes > maximum_resource_event_bytes)
				invalid("client manifest events are invalid");

		const auto file_count = take_integer<std::uint32_t>(input);
		if (file_count == 0 || file_count > maximum_resource_files)
			invalid("client package file count is invalid");
		std::size_t total{};
		for (std::uint32_t index{}; index < file_count; ++index)
		{
			const auto path_size = take_integer<std::uint16_t>(input);
			const auto path = take_string(input, path_size);
			const auto size = take_integer<std::uint64_t>(input);
			const auto digest = take_bytes(input, 32);
			if (!safe_relative_path(path)
			    || (!std::string_view(path).starts_with("client/")
			        && !std::string_view(path).starts_with("shared/"))
			    || size > maximum_resource_file_bytes
			    || size > input.size())
				invalid("client package file metadata is invalid");
			const auto contents = take_bytes(input, static_cast<std::size_t>(size));
			if (!std::ranges::equal(sha256(contents), digest))
				invalid("client package file hash mismatch: " + path);
			total += static_cast<std::size_t>(size);
			if (total > maximum_resource_package_bytes
			    || !result.files.emplace(path,
			        std::vector<std::byte>(contents.begin(), contents.end())).second)
				invalid("client package contents are invalid");
		}
		if (!input.empty()
		    || !result.files.contains(*result.definition.client_entry))
			invalid("client package has trailing data or no entry point");
		return result;
	}
}
