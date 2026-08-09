#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kcd2o::account
{
	struct credential_material
	{
		std::vector<std::byte> private_key_blob;
		std::string public_key_spki;
	};

	[[nodiscard]] std::string base64url_encode(std::span<const std::byte> bytes);
	[[nodiscard]] std::vector<std::byte> base64url_decode(std::string_view text);
	[[nodiscard]] credential_material generate_credential();
	[[nodiscard]] std::string sign_payload(
	    std::span<const std::byte> private_key_blob,
	    std::string_view encoded_payload);
	[[nodiscard]] std::string derive_device_evidence();
}
