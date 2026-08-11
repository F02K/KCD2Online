#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kcd2o::resources
{
	inline constexpr std::uint32_t resource_api_version = 1;
	inline constexpr std::size_t maximum_resources = 128;
	inline constexpr std::size_t maximum_resource_files = 512;
	inline constexpr std::size_t maximum_resource_file_bytes = 16 * 1024 * 1024;
	inline constexpr std::size_t maximum_resource_package_bytes = 64 * 1024 * 1024;
	inline constexpr std::size_t maximum_resource_set_bytes = 256 * 1024 * 1024;
	inline constexpr std::size_t resource_chunk_bytes = 48 * 1024;
	inline constexpr std::size_t maximum_resource_event_bytes = 32 * 1024;

	enum class event_direction
	{
		client_to_server,
		server_to_client,
		bidirectional
	};

	struct event_definition
	{
		std::string name;
		event_direction direction{event_direction::bidirectional};
		bool reliable{true};
		std::uint32_t max_per_second{10};
		std::uint32_t max_bytes{4096};
	};

	struct resource_definition
	{
		std::string id;
		std::string version;
		std::uint32_t api_version{resource_api_version};
		std::filesystem::path root;
		std::optional<std::string> server_entry;
		std::optional<std::string> client_entry;
		std::vector<std::string> dependencies;
		std::vector<std::string> server_capabilities;
		std::vector<std::string> client_capabilities;
		std::vector<std::string> shared_client_paths;
		std::vector<event_definition> events;
	};

	struct package_file
	{
		std::string path;
		std::vector<std::byte> bytes;
		std::string hash;
	};

	struct client_package
	{
		std::string resource_id;
		std::string version;
		std::string client_entry;
		std::string hash;
		std::vector<std::byte> bytes;
	};

	struct unpacked_package
	{
		resource_definition definition;
		std::unordered_map<std::string, std::vector<std::byte>> files;
	};

	struct resource_set
	{
		std::uint64_t generation{1};
		std::string root_hash;
		std::vector<resource_definition> definitions;
		std::vector<client_package> client_packages;

		[[nodiscard]] const client_package *package(
		    std::string_view hash) const noexcept;
		[[nodiscard]] const resource_definition *definition(
		    std::string_view id) const noexcept;
	};

	[[nodiscard]] bool valid_resource_id(std::string_view value) noexcept;
	[[nodiscard]] bool valid_resource_event_name(std::string_view value) noexcept;
	[[nodiscard]] resource_definition load_resource_definition(
	    const std::filesystem::path &resource_root);
	[[nodiscard]] resource_set load_resource_set(
	    const std::filesystem::path &directory);
	[[nodiscard]] unpacked_package unpack_client_package(
	    std::span<const std::byte> bytes,
	    std::string_view expected_hash = {});
}
