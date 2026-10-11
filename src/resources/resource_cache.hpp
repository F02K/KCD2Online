#pragma once

#include "resources/resource_package.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace kcd2o::resources
{
	class resource_cache
	{
	public:
		explicit resource_cache(std::filesystem::path root = default_path());

		[[nodiscard]] bool contains(std::string_view hash) const;
		[[nodiscard]] std::optional<std::vector<std::byte>> load(
		    std::string_view hash) const;
		void store(std::string_view hash, std::span<const std::byte> bytes);
		void activate(
		    std::string_view server_id,
		    std::string_view root_hash,
		    std::span<const std::string> package_hashes);
		[[nodiscard]] const std::filesystem::path &root() const noexcept;

		[[nodiscard]] static std::filesystem::path default_path();

	private:
		[[nodiscard]] std::filesystem::path blob_path(
		    std::string_view hash) const;

		std::filesystem::path m_root;
	};
}
