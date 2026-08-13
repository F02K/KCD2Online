#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>

namespace kcd2::signatures
{
	constexpr uint32_t supported_timestamp = 0x6A350E20;
	constexpr uint32_t supported_image_size = 0x5B2D000;
	constexpr std::string_view supported_sha256 =
	    "BDF8F9E4A11257A72B64C84700E284C29E4C4CCAF5B8D4BFA7D0B2A7294479F7";
	constexpr size_t expected_signature_count = 67;

	enum class resolution_kind
	{
		direct,
		relative_call,
		second_relative_call,
		rip_relative_memory,
	};

	enum class target_region
	{
		executable,
		readable,
	};

	struct signature_spec
	{
		std::string_view name;
		std::string_view pattern;
		resolution_kind resolution;
		target_region target;
		size_t expected_matches{1};
	};

	struct section_view
	{
		std::string name;
		uint32_t rva{};
		uint32_t size{};
		uint32_t characteristics{};
	};

	class pe_image
	{
	public:
		static std::optional<pe_image> from_loaded_module(HMODULE module, std::string &error);
		static std::optional<pe_image> from_file(const std::filesystem::path &path, std::string &error);
		static pe_image from_test_image(
		    std::vector<uint8_t> bytes,
		    std::vector<section_view> sections,
		    uint64_t virtual_base = 0x180000000);

		[[nodiscard]] const uint8_t *data(uint64_t rva = 0) const;
		[[nodiscard]] size_t size() const;
		[[nodiscard]] uint32_t timestamp() const;
		[[nodiscard]] uint64_t preferred_base() const;
		[[nodiscard]] uint64_t virtual_base() const;
		[[nodiscard]] bool is_file_image() const;
		[[nodiscard]] bool contains(uint64_t rva, size_t length = 1) const;
		[[nodiscard]] bool is_executable(uint64_t rva, size_t length = 1) const;
		[[nodiscard]] bool is_readable(uint64_t rva, size_t length = 1) const;
		[[nodiscard]] std::optional<uint64_t> virtual_address_to_rva(uint64_t address) const;
		[[nodiscard]] uint64_t rva_to_virtual_address(uint64_t rva) const;
		[[nodiscard]] const std::vector<section_view> &sections() const;
		[[nodiscard]] const std::vector<uint8_t> &file_bytes() const;

	private:
		const uint8_t *m_image{};
		size_t m_size{};
		uint32_t m_timestamp{};
		uint64_t m_preferred_base{};
		uint64_t m_virtual_base{};
		bool m_file_image{};
		std::vector<uint8_t> m_virtual_storage;
		std::vector<uint8_t> m_file_storage;
		std::vector<section_view> m_sections;
	};

	enum class failure_kind
	{
		missing,
		ambiguous,
		invalid_pattern,
		decode_error,
		invalid_target,
		ambiguous_derivation,
	};

	struct diagnostic
	{
		std::string name;
		failure_kind kind{};
		size_t match_count{};
		std::string detail;
	};

	struct resolved_address
	{
		std::string name;
		uint64_t match_rva{};
		uint64_t target_rva{};
	};

	struct resolution_report
	{
		size_t signatures_requested{};
		size_t signatures_resolved{};
		size_t derived_requested{};
		size_t derived_resolved{};
		std::vector<resolved_address> addresses;
		std::vector<resolved_address> derived_addresses;
		std::vector<diagnostic> diagnostics;

		[[nodiscard]] bool success() const;
		[[nodiscard]] std::optional<uint64_t> match(std::string_view name) const;
		[[nodiscard]] std::optional<uint64_t> target(std::string_view name) const;
		[[nodiscard]] std::optional<uint64_t> derived(std::string_view name) const;
	};

	[[nodiscard]] std::span<const signature_spec> registry();
	[[nodiscard]] resolution_report resolve_all(const pe_image &image);
	[[nodiscard]] std::vector<uint64_t> scan_pattern(
	    const pe_image &image,
	    std::string_view pattern,
	    std::string &error);
	[[nodiscard]] std::string failure_kind_name(failure_kind kind);
	[[nodiscard]] std::optional<uint64_t> resolve_relative_call(
	    const pe_image &image,
	    uint64_t instruction_rva,
	    std::string &error);
	[[nodiscard]] std::optional<uint64_t> resolve_rip_relative_memory(
	    const pe_image &image,
	    uint64_t instruction_rva,
	    std::string &error);
	[[nodiscard]] std::optional<uint64_t> resolve_constructor_vtable_assignment(
	    const pe_image &image,
	    uint64_t constructor_rva,
	    size_t max_bytes,
	    std::string &error);
	[[nodiscard]] std::optional<uint64_t> resolve_unique_rip_data_reference(
	    const pe_image &image,
	    uint64_t function_rva,
	    size_t max_bytes,
	    std::string &error);
}
