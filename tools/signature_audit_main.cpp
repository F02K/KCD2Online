#include "signatures/signature_core.hpp"

#include <array>
#include <cctype>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include <bcrypt.h>

namespace
{
	std::string sha256(std::span<const uint8_t> data)
	{
		BCRYPT_ALG_HANDLE algorithm{};
		BCRYPT_HASH_HANDLE hash{};
		DWORD object_size{};
		DWORD result_size{};
		std::vector<uint8_t> object;
		std::array<uint8_t, 32> digest{};

		if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
		    || BCryptGetProperty(
		           algorithm,
		           BCRYPT_OBJECT_LENGTH,
		           reinterpret_cast<PUCHAR>(&object_size),
		           sizeof(object_size),
		           &result_size,
		           0)
		        < 0)
		{
			if (algorithm)
			{
				BCryptCloseAlgorithmProvider(algorithm, 0);
			}
			return {};
		}
		object.resize(object_size);
		const bool success =
		    BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0
		    && BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) >= 0
		    && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
		if (hash)
		{
			BCryptDestroyHash(hash);
		}
		BCryptCloseAlgorithmProvider(algorithm, 0);
		if (!success)
		{
			return {};
		}

		std::string text;
		text.reserve(digest.size() * 2);
		for (const auto byte : digest)
		{
			text += std::format("{:02X}", byte);
		}
		return text;
	}

	std::string json_escape(std::string_view value)
	{
		std::string escaped;
		for (const char character : value)
		{
			switch (character)
			{
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if (static_cast<unsigned char>(character) >= 0x20)
				{
					escaped += character;
				}
				break;
			}
		}
		return escaped;
	}

	void print_json(
	    const kcd2::signatures::pe_image &image,
	    std::string_view digest,
	    const kcd2::signatures::resolution_report &report)
	{
		std::cout << "{\n"
		          << "  \"timestamp\": " << image.timestamp() << ",\n"
		          << "  \"image_size\": " << image.size() << ",\n"
		          << "  \"sha256\": \"" << digest << "\",\n"
		          << "  \"signatures_requested\": " << report.signatures_requested << ",\n"
		          << "  \"signatures_resolved\": " << report.signatures_resolved << ",\n"
		          << "  \"derived_requested\": " << report.derived_requested << ",\n"
		          << "  \"derived_resolved\": " << report.derived_resolved << ",\n"
		          << "  \"success\": " << (report.success() ? "true" : "false") << ",\n"
		          << "  \"addresses\": {";
		bool first_address = true;
		for (const auto &address : report.addresses)
		{
			std::cout << (first_address ? "\n" : ",\n")
			          << "    \"" << json_escape(address.name) << "\": {\"match_rva\": "
			          << address.match_rva << ", \"target_rva\": " << address.target_rva << "}";
			first_address = false;
		}
		std::cout << (first_address ? "" : "\n  ") << "},\n"
		          << "  \"derived_addresses\": {";
		first_address = true;
		for (const auto &address : report.derived_addresses)
		{
			std::cout << (first_address ? "\n" : ",\n")
			          << "    \"" << json_escape(address.name) << "\": {\"source_rva\": "
			          << address.match_rva << ", \"target_rva\": " << address.target_rva << "}";
			first_address = false;
		}
		std::cout << (first_address ? "" : "\n  ") << "},\n"
		          << "  \"diagnostics\": [";
		for (size_t index = 0; index < report.diagnostics.size(); ++index)
		{
			const auto &diagnostic = report.diagnostics[index];
			std::cout << (index ? ",\n" : "\n")
			          << "    {\"name\": \"" << json_escape(diagnostic.name)
			          << "\", \"kind\": \"" << kcd2::signatures::failure_kind_name(diagnostic.kind)
			          << "\", \"match_count\": " << diagnostic.match_count
			          << ", \"detail\": \"" << json_escape(diagnostic.detail) << "\"}";
		}
		std::cout << (report.diagnostics.empty() ? "" : "\n  ") << "]\n}\n";
	}
}

int wmain(int argc, wchar_t **argv)
{
	bool json{};
	std::filesystem::path input;
	for (int index = 1; index < argc; ++index)
	{
		const std::wstring_view argument(argv[index]);
		if (argument == L"--json")
		{
			json = true;
		}
		else if (input.empty())
		{
			input = argument;
		}
		else
		{
			std::cerr << "ERROR: unexpected argument\n";
			return 2;
		}
	}
	if (input.empty())
	{
		std::cerr << "Usage: KCD2OnlineSignatureAudit <WHGame.dll> [--json]\n";
		return 2;
	}

	std::string error;
	const auto image = kcd2::signatures::pe_image::from_file(input, error);
	if (!image)
	{
		std::cerr << "ERROR: " << error << '\n';
		return 2;
	}
	const auto digest = sha256(image->file_bytes());
	if (digest.empty())
	{
		std::cerr << "ERROR: SHA-256 calculation failed\n";
		return 2;
	}
	if (image->timestamp() != kcd2::signatures::supported_timestamp
	    || image->size() != kcd2::signatures::supported_image_size
	    || digest != kcd2::signatures::supported_sha256)
	{
		if (json)
		{
			std::cout << std::format(
			    "{{\"success\":false,\"error\":\"unsupported build\",\"timestamp\":{},\"image_size\":{},\"sha256\":\"{}\"}}\n",
			    image->timestamp(),
			    image->size(),
			    digest);
		}
		else
		{
			std::cerr << std::format(
			    "ERROR: unsupported WHGame.dll: TimeDateStamp=0x{:08X}, SizeOfImage=0x{:X}, SHA-256={}\n",
			    image->timestamp(),
			    image->size(),
			    digest);
		}
		return 3;
	}

	const auto report = kcd2::signatures::resolve_all(*image);
	if (json)
	{
		print_json(*image, digest, report);
	}
	else
	{
		std::cout << std::format(
		    "WHGame.dll: TimeDateStamp=0x{:08X}, SizeOfImage=0x{:X}\nSHA-256: {}\n",
		    image->timestamp(),
		    image->size(),
		    digest);
		for (const auto &diagnostic : report.diagnostics)
		{
			std::cerr << std::format(
			    "ERROR: {}: {} ({})\n",
			    diagnostic.name,
			    diagnostic.detail,
			    kcd2::signatures::failure_kind_name(diagnostic.kind));
		}
		std::cout << std::format(
		    "Signatures: {}/{} uniquely resolved\nDerived targets: {}/{} valid\n",
		    report.signatures_resolved,
		    report.signatures_requested,
		    report.derived_resolved,
		    report.derived_requested);
	}
	return report.success() ? 0 : 1;
}
