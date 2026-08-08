#include "multiplayer/identity_store.hpp"

#include <algorithm>
#include <fstream>
#include <span>
#include <stdexcept>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <dpapi.h>

namespace kcd2o
{
	namespace
	{
		constexpr std::string_view entropy_text =
		    "KCD2Online multiplayer identity store v1";

		DATA_BLOB blob(std::span<const std::byte> bytes)
		{
			return {
			    static_cast<DWORD>(bytes.size()),
			    reinterpret_cast<BYTE *>(
			        const_cast<std::byte *>(bytes.data()))};
		}

		std::vector<std::byte> protect(std::string_view plaintext)
		{
			auto input = blob({
			    reinterpret_cast<const std::byte *>(plaintext.data()),
			    plaintext.size()});
			auto entropy = blob({
			    reinterpret_cast<const std::byte *>(entropy_text.data()),
			    entropy_text.size()});
			DATA_BLOB output{};
			if (!CryptProtectData(
			        &input,
			        L"KCD2Online identity tokens",
			        &entropy,
			        nullptr,
			        nullptr,
			        CRYPTPROTECT_UI_FORBIDDEN,
			        &output))
			{
				throw std::runtime_error("Windows DPAPI could not protect identities");
			}
			std::vector<std::byte> result(
			    reinterpret_cast<std::byte *>(output.pbData),
			    reinterpret_cast<std::byte *>(output.pbData + output.cbData));
			LocalFree(output.pbData);
			return result;
		}

		std::string unprotect(std::span<const std::byte> ciphertext)
		{
			auto input = blob(ciphertext);
			auto entropy = blob({
			    reinterpret_cast<const std::byte *>(entropy_text.data()),
			    entropy_text.size()});
			DATA_BLOB output{};
			if (!CryptUnprotectData(
			        &input,
			        nullptr,
			        &entropy,
			        nullptr,
			        nullptr,
			        CRYPTPROTECT_UI_FORBIDDEN,
			        &output))
			{
				throw std::runtime_error(
				    "Windows DPAPI could not decrypt the identity store");
			}
			std::string result(
			    reinterpret_cast<const char *>(output.pbData),
			    output.cbData);
			LocalFree(output.pbData);
			return result;
		}

		void atomic_write(
		    const std::filesystem::path &target,
		    std::span<const std::byte> bytes)
		{
			std::filesystem::create_directories(target.parent_path());
			const auto temporary = target.wstring() + L".tmp";
			{
				std::ofstream output(
				    std::filesystem::path(temporary),
				    std::ios::binary | std::ios::trunc);
				output.write(
				    reinterpret_cast<const char *>(bytes.data()),
				    static_cast<std::streamsize>(bytes.size()));
				output.flush();
				if (!output)
				{
					throw std::runtime_error("could not write identity store");
				}
			}
			if (!MoveFileExW(
			        temporary.c_str(),
			        target.c_str(),
			        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				DeleteFileW(temporary.c_str());
				throw std::runtime_error(
				    "could not atomically replace identity store");
			}
		}
	}

	identity_store::identity_store(std::filesystem::path path) :
	    m_path(std::move(path))
	{
		load();
	}

	std::optional<std::string> identity_store::token_for(
	    std::string_view server_id) const
	{
		const auto iterator = m_tokens.find(std::string(server_id));
		return iterator == m_tokens.end()
		    ? std::nullopt
		    : std::optional{iterator->second};
	}

	void identity_store::store(std::string server_id, std::string token)
	{
		if (server_id.empty() || token.empty()
		    || server_id.find_first_of("=\r\n") != std::string::npos
		    || token.find_first_of("=\r\n") != std::string::npos)
		{
			throw std::invalid_argument("identity entry is invalid");
		}
		m_tokens.insert_or_assign(std::move(server_id), std::move(token));
		save();
	}

	void identity_store::erase(std::string_view server_id)
	{
		if (m_tokens.erase(std::string(server_id)) > 0)
		{
			save();
		}
	}

	const std::filesystem::path &identity_store::path() const
	{
		return m_path;
	}

	std::filesystem::path identity_store::default_path()
	{
		std::wstring buffer(32768, L'\0');
		const auto length = GetEnvironmentVariableW(
		    L"LOCALAPPDATA",
		    buffer.data(),
		    static_cast<DWORD>(buffer.size()));
		if (length == 0 || length >= buffer.size())
		{
			throw std::runtime_error("LOCALAPPDATA is unavailable");
		}
		buffer.resize(length);
		return std::filesystem::path(buffer)
		    / "KCD2Online"
		    / "multiplayer-identities.bin";
	}

	void identity_store::load()
	{
		if (!std::filesystem::exists(m_path))
		{
			return;
		}
		std::ifstream input(m_path, std::ios::binary);
		const std::string encrypted_text{
		    std::istreambuf_iterator<char>(input),
		    std::istreambuf_iterator<char>()};
		if ((!input && !input.eof()) || encrypted_text.empty())
		{
			throw std::runtime_error("identity store is empty or unreadable");
		}
		const auto plaintext = unprotect({
		    reinterpret_cast<const std::byte *>(encrypted_text.data()),
		    encrypted_text.size()});
		std::size_t start{};
		while (start < plaintext.size())
		{
			const auto end = plaintext.find('\n', start);
			const auto line = plaintext.substr(
			    start,
			    end == std::string::npos ? std::string::npos : end - start);
			if (!line.empty())
			{
				const auto separator = line.find('=');
				if (separator == std::string::npos || separator == 0
				    || separator + 1 >= line.size())
				{
					throw std::runtime_error("identity store is malformed");
				}
				m_tokens.emplace(
				    line.substr(0, separator),
				    line.substr(separator + 1));
			}
			if (end == std::string::npos)
			{
				break;
			}
			start = end + 1;
		}
	}

	void identity_store::save() const
	{
		std::vector<std::pair<std::string, std::string>> entries(
		    m_tokens.begin(),
		    m_tokens.end());
		std::ranges::sort(entries);
		std::string plaintext;
		for (const auto &[server_id, token] : entries)
		{
			plaintext += server_id + '=' + token + '\n';
		}
		const auto encrypted = protect(plaintext);
		atomic_write(m_path, encrypted);
	}
}
