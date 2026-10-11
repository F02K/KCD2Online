#include "resources/resource_cache.hpp"

#include "resources/sha256.hpp"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace kcd2o::resources
{
	namespace
	{
		void atomic_write(
		    const std::filesystem::path &target,
		    std::span<const std::byte> bytes)
		{
			std::filesystem::create_directories(target.parent_path());
			const auto temporary = target.wstring() + L".part";
			{
				std::ofstream output(
				    std::filesystem::path(temporary),
				    std::ios::binary | std::ios::trunc);
				if (!bytes.empty())
					output.write(
					    reinterpret_cast<const char *>(bytes.data()),
					    static_cast<std::streamsize>(bytes.size()));
				output.flush();
				if (!output)
					throw std::runtime_error("could not write resource cache");
			}
			if (!MoveFileExW(
			        temporary.c_str(), target.c_str(),
			        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				DeleteFileW(temporary.c_str());
				throw std::runtime_error("could not commit resource cache file");
			}
		}

		std::string safe_server_directory(std::string_view value)
		{
			const auto digest = sha256_hex({
			    reinterpret_cast<const std::byte *>(value.data()), value.size()});
			return digest.substr(0, 32);
		}
	}

	resource_cache::resource_cache(std::filesystem::path root) :
	    m_root(std::move(root))
	{
	}

	bool resource_cache::contains(std::string_view hash) const
	{
		if (!valid_sha256_hex(hash))
			return false;
		std::error_code error;
		const auto path = blob_path(hash);
		const auto size = std::filesystem::file_size(path, error);
		return !error && size > 0 && size <= maximum_resource_package_bytes;
	}

	std::optional<std::vector<std::byte>> resource_cache::load(
	    std::string_view hash) const
	{
		if (!contains(hash))
			return std::nullopt;
		const auto path = blob_path(hash);
		const auto size = std::filesystem::file_size(path);
		std::ifstream input(path, std::ios::binary);
		std::vector<std::byte> bytes(static_cast<std::size_t>(size));
		if (!bytes.empty())
			input.read(
			    reinterpret_cast<char *>(bytes.data()),
			    static_cast<std::streamsize>(bytes.size()));
		if ((!input && !input.eof()) || sha256_hex(bytes) != hash)
		{
			std::error_code ignored;
			std::filesystem::remove(blob_path(hash), ignored);
			return std::nullopt;
		}
		return bytes;
	}

	void resource_cache::store(
	    std::string_view hash,
	    std::span<const std::byte> bytes)
	{
		if (!valid_sha256_hex(hash) || bytes.empty()
		    || bytes.size() > maximum_resource_package_bytes
		    || sha256_hex(bytes) != hash)
			throw std::invalid_argument("resource cache package is invalid");
		(void)unpack_client_package(bytes, hash);
		atomic_write(blob_path(hash), bytes);
	}

	void resource_cache::activate(
	    std::string_view server_id,
	    std::string_view root_hash,
	    std::span<const std::string> package_hashes)
	{
		if (server_id.empty() || !valid_sha256_hex(root_hash))
			throw std::invalid_argument("resource activation identity is invalid");
		for (const auto &hash : package_hashes)
			if (!contains(hash))
				throw std::invalid_argument("resource activation package is missing");
		const nlohmann::json document{
		    {"server_id", server_id},
		    {"root_hash", root_hash},
		    {"packages", package_hashes}};
		const auto text = document.dump(2) + '\n';
		atomic_write(
		    m_root / "servers" / safe_server_directory(server_id) / "active.json",
		    {reinterpret_cast<const std::byte *>(text.data()), text.size()});
	}

	const std::filesystem::path &resource_cache::root() const noexcept
	{
		return m_root;
	}

	std::filesystem::path resource_cache::default_path()
	{
		std::wstring buffer(32768, L'\0');
		const auto length = GetEnvironmentVariableW(
		    L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0 || length >= buffer.size())
			throw std::runtime_error("LOCALAPPDATA is unavailable");
		buffer.resize(length);
		return std::filesystem::path(buffer) / "KCD2Online" / "resources";
	}

	std::filesystem::path resource_cache::blob_path(
	    std::string_view hash) const
	{
		return m_root / "blobs" / std::string(hash.substr(0, 2))
		    / (std::string(hash) + ".k2res");
	}
}
