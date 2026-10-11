#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace kcd2o::resources
{
	using sha256_digest = std::array<std::byte, 32>;

	[[nodiscard]] sha256_digest sha256(std::span<const std::byte> bytes);
	[[nodiscard]] std::string sha256_hex(std::span<const std::byte> bytes);
	[[nodiscard]] bool valid_sha256_hex(std::string_view value) noexcept;
}
