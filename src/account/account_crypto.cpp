#include "account/account_crypto.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

namespace kcd2o::account
{
	namespace
	{
		constexpr std::array<std::byte, 27> p256_spki_prefix{
		    std::byte{0x30}, std::byte{0x59}, std::byte{0x30}, std::byte{0x13},
		    std::byte{0x06}, std::byte{0x07}, std::byte{0x2A}, std::byte{0x86},
		    std::byte{0x48}, std::byte{0xCE}, std::byte{0x3D}, std::byte{0x02},
		    std::byte{0x01}, std::byte{0x06}, std::byte{0x08}, std::byte{0x2A},
		    std::byte{0x86}, std::byte{0x48}, std::byte{0xCE}, std::byte{0x3D},
		    std::byte{0x03}, std::byte{0x01}, std::byte{0x07}, std::byte{0x03},
		    std::byte{0x42}, std::byte{0x00}, std::byte{0x04}};

		void check(NTSTATUS status, const char *operation)
		{
			if (status < 0)
				throw std::runtime_error(operation);
		}

		struct algorithm_deleter
		{
			void operator()(void *handle) const noexcept
			{
				if (handle)
					BCryptCloseAlgorithmProvider(handle, 0);
			}
		};

		struct key_deleter
		{
			void operator()(void *handle) const noexcept
			{
				if (handle)
					BCryptDestroyKey(handle);
			}
		};

		struct hash_deleter
		{
			void operator()(void *handle) const noexcept
			{
				if (handle)
					BCryptDestroyHash(handle);
			}
		};

		using algorithm_handle = std::unique_ptr<void, algorithm_deleter>;
		using key_handle = std::unique_ptr<void, key_deleter>;
		using hash_handle = std::unique_ptr<void, hash_deleter>;

		algorithm_handle open_algorithm(const wchar_t *identifier)
		{
			BCRYPT_ALG_HANDLE raw{};
			check(
			    BCryptOpenAlgorithmProvider(&raw, identifier, nullptr, 0),
			    "BCryptOpenAlgorithmProvider failed");
			return algorithm_handle(raw);
		}

		std::array<std::byte, 32> sha256(std::span<const std::byte> input)
		{
			auto algorithm = open_algorithm(BCRYPT_SHA256_ALGORITHM);
			DWORD object_size{};
			DWORD copied{};
			check(
			    BCryptGetProperty(
			        algorithm.get(),
			        BCRYPT_OBJECT_LENGTH,
			        reinterpret_cast<PUCHAR>(&object_size),
			        sizeof(object_size),
			        &copied,
			        0),
			    "BCryptGetProperty failed");
			std::vector<UCHAR> object(object_size);
			BCRYPT_HASH_HANDLE raw_hash{};
			check(
			    BCryptCreateHash(
			        algorithm.get(),
			        &raw_hash,
			        object.data(),
			        static_cast<ULONG>(object.size()),
			        nullptr,
			        0,
			        0),
			    "BCryptCreateHash failed");
			hash_handle hash(raw_hash);
			check(
			    BCryptHashData(
			        hash.get(),
			        reinterpret_cast<PUCHAR>(const_cast<std::byte *>(input.data())),
			        static_cast<ULONG>(input.size()),
			        0),
			    "BCryptHashData failed");
			std::array<std::byte, 32> result{};
			check(
			    BCryptFinishHash(
			        hash.get(),
			        reinterpret_cast<PUCHAR>(result.data()),
			        static_cast<ULONG>(result.size()),
			        0),
			    "BCryptFinishHash failed");
			SecureZeroMemory(object.data(), object.size());
			return result;
		}

		std::string utf8(std::wstring_view value)
		{
			if (value.empty())
				return {};
			const auto length = WideCharToMultiByte(
			    CP_UTF8,
			    WC_ERR_INVALID_CHARS,
			    value.data(),
			    static_cast<int>(value.size()),
			    nullptr,
			    0,
			    nullptr,
			    nullptr);
			if (length <= 0)
				throw std::runtime_error("Could not encode Windows identifier");
			std::string result(static_cast<std::size_t>(length), '\0');
			WideCharToMultiByte(
			    CP_UTF8,
			    WC_ERR_INVALID_CHARS,
			    value.data(),
			    static_cast<int>(value.size()),
			    result.data(),
			    length,
			    nullptr,
			    nullptr);
			return result;
		}

		std::string machine_guid()
		{
			std::array<wchar_t, 256> value{};
			DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
			const auto result = RegGetValueW(
			    HKEY_LOCAL_MACHINE,
			    L"SOFTWARE\\Microsoft\\Cryptography",
			    L"MachineGuid",
			    RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
			    nullptr,
			    value.data(),
			    &bytes);
			if (result != ERROR_SUCCESS || bytes < sizeof(wchar_t))
				return {};
			const auto characters = bytes / sizeof(wchar_t);
			return utf8(std::wstring_view(
			    value.data(),
			    characters > 0 && value[characters - 1] == L'\0'
			        ? characters - 1
			        : characters));
		}

		std::vector<std::byte> smbios_uuid()
		{
			constexpr DWORD provider = static_cast<DWORD>('R')
			    | (static_cast<DWORD>('S') << 8)
			    | (static_cast<DWORD>('M') << 16)
			    | (static_cast<DWORD>('B') << 24);
			const auto size = GetSystemFirmwareTable(provider, 0, nullptr, 0);
			if (size < 8)
				return {};
			std::vector<std::byte> buffer(size);
			if (GetSystemFirmwareTable(provider, 0, buffer.data(), size) != size)
				return {};
			DWORD table_size{};
			std::memcpy(&table_size, buffer.data() + 4, sizeof(table_size));
			const auto end = std::min<std::size_t>(buffer.size(), 8ULL + table_size);
			std::size_t offset = 8;
			while (offset + 4 <= end)
			{
				const auto type = std::to_integer<unsigned char>(buffer[offset]);
				const auto length = std::to_integer<unsigned char>(buffer[offset + 1]);
				if (length < 4 || offset + length > end)
					break;
				if (type == 1 && length >= 0x19)
				{
					std::vector<std::byte> uuid(
					    buffer.begin() + static_cast<std::ptrdiff_t>(offset + 8),
					    buffer.begin() + static_cast<std::ptrdiff_t>(offset + 24));
					const auto all_zero = std::ranges::all_of(
					    uuid, [](std::byte value) { return value == std::byte{}; });
					const auto all_ff = std::ranges::all_of(
					    uuid, [](std::byte value) { return value == std::byte{0xFF}; });
					if (!all_zero && !all_ff)
						return uuid;
				}
				std::size_t next = offset + length;
				while (next + 1 < end
				    && (buffer[next] != std::byte{} || buffer[next + 1] != std::byte{}))
					++next;
				offset = next + 2;
			}
			return {};
		}

		std::string system_volume_id()
		{
			std::array<wchar_t, MAX_PATH> windows{};
			const auto length = GetWindowsDirectoryW(
			    windows.data(), static_cast<UINT>(windows.size()));
			if (length < 3 || length >= windows.size())
				return {};
			std::array<wchar_t, 4> root{windows[0], L':', L'\\', L'\0'};
			DWORD serial{};
			if (!GetVolumeInformationW(
			        root.data(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0))
				return {};
			char encoded[9]{};
			sprintf_s(encoded, "%08lX", serial);
			return encoded;
		}
	}

	std::string base64url_encode(std::span<const std::byte> bytes)
	{
		static constexpr std::string_view alphabet =
		    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
		std::string result;
		result.reserve((bytes.size() * 4 + 2) / 3);
		std::uint32_t accumulator{};
		int bits{};
		for (const auto value : bytes)
		{
			accumulator = (accumulator << 8) | std::to_integer<unsigned char>(value);
			bits += 8;
			while (bits >= 6)
			{
				bits -= 6;
				result.push_back(alphabet[(accumulator >> bits) & 0x3F]);
			}
		}
		if (bits > 0)
			result.push_back(alphabet[(accumulator << (6 - bits)) & 0x3F]);
		return result;
	}

	std::vector<std::byte> base64url_decode(std::string_view text)
	{
		std::vector<std::byte> result;
		result.reserve(text.size() * 3 / 4);
		std::uint32_t accumulator{};
		int bits{};
		for (const auto character : text)
		{
			int value = character >= 'A' && character <= 'Z' ? character - 'A'
			    : character >= 'a' && character <= 'z'       ? character - 'a' + 26
			    : character >= '0' && character <= '9'       ? character - '0' + 52
			    : character == '-'                           ? 62
			    : character == '_'                           ? 63
			                                                 : -1;
			if (value < 0)
				throw std::invalid_argument("Invalid base64url value");
			accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
			bits += 6;
			if (bits >= 8)
			{
				bits -= 8;
				result.push_back(
				    static_cast<std::byte>((accumulator >> bits) & 0xFF));
			}
		}
		if (bits >= 6 || (bits > 0 && (accumulator & ((1U << bits) - 1U)) != 0))
			throw std::invalid_argument("Invalid base64url tail");
		return result;
	}

	credential_material generate_credential()
	{
		auto algorithm = open_algorithm(BCRYPT_ECDSA_P256_ALGORITHM);
		BCRYPT_KEY_HANDLE raw_key{};
		check(
		    BCryptGenerateKeyPair(algorithm.get(), &raw_key, 256, 0),
		    "BCryptGenerateKeyPair failed");
		key_handle key(raw_key);
		check(BCryptFinalizeKeyPair(key.get(), 0), "BCryptFinalizeKeyPair failed");

		ULONG size{};
		check(
		    BCryptExportKey(
		        key.get(), nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &size, 0),
		    "BCryptExportKey size failed");
		credential_material result;
		result.private_key_blob.resize(size);
		check(
		    BCryptExportKey(
		        key.get(),
		        nullptr,
		        BCRYPT_ECCPRIVATE_BLOB,
		        reinterpret_cast<PUCHAR>(result.private_key_blob.data()),
		        size,
		        &size,
		        0),
		    "BCryptExportKey failed");
		if (result.private_key_blob.size() < sizeof(BCRYPT_ECCKEY_BLOB) + 64)
			throw std::runtime_error("Generated P-256 key blob is incomplete");
		const auto *header = reinterpret_cast<const BCRYPT_ECCKEY_BLOB *>(
		    result.private_key_blob.data());
		if (header->dwMagic != BCRYPT_ECDSA_PRIVATE_P256_MAGIC
		    || header->cbKey != 32)
			throw std::runtime_error("Generated credential is not P-256");
		std::vector<std::byte> spki(p256_spki_prefix.begin(), p256_spki_prefix.end());
		spki.insert(
		    spki.end(),
		    result.private_key_blob.begin()
		        + static_cast<std::ptrdiff_t>(sizeof(BCRYPT_ECCKEY_BLOB)),
		    result.private_key_blob.begin()
		        + static_cast<std::ptrdiff_t>(sizeof(BCRYPT_ECCKEY_BLOB) + 64));
		result.public_key_spki = base64url_encode(spki);
		return result;
	}

	std::string sign_payload(
	    std::span<const std::byte> private_key_blob,
	    std::string_view encoded_payload)
	{
		auto algorithm = open_algorithm(BCRYPT_ECDSA_P256_ALGORITHM);
		BCRYPT_KEY_HANDLE raw_key{};
		check(
		    BCryptImportKeyPair(
		        algorithm.get(),
		        nullptr,
		        BCRYPT_ECCPRIVATE_BLOB,
		        &raw_key,
		        reinterpret_cast<PUCHAR>(
		            const_cast<std::byte *>(private_key_blob.data())),
		        static_cast<ULONG>(private_key_blob.size()),
		        0),
		    "BCryptImportKeyPair failed");
		key_handle key(raw_key);
		auto payload = base64url_decode(encoded_payload);
		const auto digest = sha256(payload);
		ULONG signature_size{};
		check(
		    BCryptSignHash(
		        key.get(),
		        nullptr,
		        reinterpret_cast<PUCHAR>(
		            const_cast<std::byte *>(digest.data())),
		        static_cast<ULONG>(digest.size()),
		        nullptr,
		        0,
		        &signature_size,
		        0),
		    "BCryptSignHash size failed");
		std::vector<std::byte> signature(signature_size);
		check(
		    BCryptSignHash(
		        key.get(),
		        nullptr,
		        reinterpret_cast<PUCHAR>(
		            const_cast<std::byte *>(digest.data())),
		        static_cast<ULONG>(digest.size()),
		        reinterpret_cast<PUCHAR>(signature.data()),
		        signature_size,
		        &signature_size,
		        0),
		    "BCryptSignHash failed");
		signature.resize(signature_size);
		SecureZeroMemory(payload.data(), payload.size());
		if (signature.size() != 64)
			throw std::runtime_error("P-256 signature has an unexpected size");
		return base64url_encode(signature);
	}

	std::string derive_device_evidence()
	{
		std::vector<std::byte> material;
		const auto append_text = [&material](std::string_view label, std::string_view value)
		{
			if (value.empty())
				return;
			material.insert(
			    material.end(),
			    reinterpret_cast<const std::byte *>(label.data()),
			    reinterpret_cast<const std::byte *>(label.data() + label.size()));
			material.push_back(std::byte{});
			material.insert(
			    material.end(),
			    reinterpret_cast<const std::byte *>(value.data()),
			    reinterpret_cast<const std::byte *>(value.data() + value.size()));
			material.push_back(std::byte{});
		};
		append_text("kcd2online-device-v1", "windows");
		append_text("machine-guid", machine_guid());
		append_text("system-volume", system_volume_id());
		const auto firmware = smbios_uuid();
		if (!firmware.empty())
		{
			append_text("smbios-uuid", base64url_encode(firmware));
		}
		if (material.size() <= std::string_view("kcd2online-device-v1").size() + 10)
			throw std::runtime_error("No stable Windows device evidence is available");
		const auto digest = sha256(material);
		SecureZeroMemory(material.data(), material.size());
		return base64url_encode(digest);
	}
}
