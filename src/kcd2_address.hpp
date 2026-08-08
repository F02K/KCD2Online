#pragma once

#include "signatures/signature_core.hpp"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <Windows.h>

/**
 * Runtime facade over the shared KCD2Online signature registry.
 *
 * Required WHGame addresses are preflighted through signature_core before
 * kcd2_init creates any hook objects. scan()/get_call() remain only for the
 * two optional Lua-extension patterns that are outside the 67-entry registry.
 */
struct kcd2_address
{
	enum class scan_status
	{
		resolved,
		missing,
		ambiguous,
		invalid_pattern,
		invalid_target,
		module_unavailable,
	};

	struct scan_diagnostic
	{
		std::string name;
		std::string pattern;
		scan_status status;
		size_t match_count;
		std::string detail;
	};

	struct scan_summary
	{
		size_t requested{};
		size_t resolved{};
		size_t derived_requested{};
		size_t derived_resolved{};
		std::vector<scan_diagnostic> failures;
	};

	uint64_t m_value{};

	kcd2_address() = default;
	kcd2_address(uint64_t value) :
	    m_value(value)
	{
	}

private:
	struct pattern_byte
	{
		uint8_t value{};
		bool wildcard{};
	};

	static inline std::optional<kcd2::signatures::pe_image> s_image;
	static inline kcd2::signatures::resolution_report s_report;
	static inline scan_summary s_summary;

	static bool parse_pattern(std::string_view text, std::vector<pattern_byte> &output)
	{
		output.clear();
		for (size_t cursor = 0; cursor < text.size();)
		{
			while (cursor < text.size() && text[cursor] == ' ')
			{
				++cursor;
			}
			if (cursor == text.size())
			{
				break;
			}
			const auto end = text.find(' ', cursor);
			const auto token = text.substr(
			    cursor,
			    end == std::string_view::npos ? text.size() - cursor : end - cursor);
			if (token == "?" || token == "??")
			{
				output.push_back({0, true});
			}
			else
			{
				if (token.size() != 2)
				{
					return false;
				}
				unsigned int value{};
				const auto parsed =
				    std::from_chars(token.data(), token.data() + token.size(), value, 16);
				if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()
				    || value > 0xFF)
				{
					return false;
				}
				output.push_back({static_cast<uint8_t>(value), false});
			}
			cursor = end == std::string_view::npos ? text.size() : end + 1;
		}
		return !output.empty();
	}

	static kcd2_address scan_optional(std::string_view pattern_text)
	{
		if (!s_image)
		{
			return {};
		}
		std::vector<pattern_byte> pattern;
		if (!parse_pattern(pattern_text, pattern))
		{
			return {};
		}
		uint64_t match{};
		size_t count{};
		for (const auto &section : s_image->sections())
		{
			if (!(section.characteristics & IMAGE_SCN_MEM_EXECUTE)
			    || section.size < pattern.size())
			{
				continue;
			}
			const auto *bytes = s_image->data(section.rva);
			for (size_t offset = 0; offset <= section.size - pattern.size(); ++offset)
			{
				bool found = true;
				for (size_t index = 0; index < pattern.size(); ++index)
				{
					if (!pattern[index].wildcard && bytes[offset + index] != pattern[index].value)
					{
						found = false;
						break;
					}
				}
				if (found)
				{
					match = section.rva + offset;
					++count;
				}
			}
		}
		return count == 1 ? kcd2_address(s_image->rva_to_virtual_address(match)) : kcd2_address{};
	}

	static scan_status map_failure(kcd2::signatures::failure_kind kind)
	{
		using enum kcd2::signatures::failure_kind;
		switch (kind)
		{
		case missing: return scan_status::missing;
		case ambiguous:
		case ambiguous_derivation: return scan_status::ambiguous;
		case invalid_pattern: return scan_status::invalid_pattern;
		default: return scan_status::invalid_target;
		}
	}

public:
	static void begin_scan_session()
	{
		s_image.reset();
		s_report = {};
		s_summary = {};

		std::string error;
		auto image =
		    kcd2::signatures::pe_image::from_loaded_module(GetModuleHandleA("WHGame.dll"), error);
		if (!image)
		{
			s_summary.failures.push_back({
			    "WHGame.dll",
			    {},
			    scan_status::module_unavailable,
			    0,
			    std::move(error),
			});
			return;
		}
		s_image.emplace(std::move(*image));
		s_report = kcd2::signatures::resolve_all(*s_image);
		s_summary.requested = s_report.signatures_requested;
		s_summary.resolved = s_report.signatures_resolved;
		s_summary.derived_requested = s_report.derived_requested;
		s_summary.derived_resolved = s_report.derived_resolved;
		for (const auto &diagnostic : s_report.diagnostics)
		{
			s_summary.failures.push_back({
			    diagnostic.name,
			    {},
			    map_failure(diagnostic.kind),
			    diagnostic.match_count,
			    diagnostic.detail,
			});
		}
	}

	static scan_summary get_scan_summary()
	{
		return s_summary;
	}

	static void report_invalid_target(std::string_view name, uint64_t address)
	{
		++s_summary.derived_requested;
		s_summary.failures.push_back({
		    std::string(name),
		    std::format("derived address 0x{:X}", address),
		    scan_status::invalid_target,
		    0,
		    "address is outside its expected PE range",
		});
		LOGF(ERROR, "Derived target '{}' is outside its expected PE range (0x{:X}).", name, address);
	}

	static bool contains(uint64_t address, size_t size = 1)
	{
		if (!s_image)
		{
			return false;
		}
		const auto rva = s_image->virtual_address_to_rva(address);
		return rva && s_image->contains(*rva, size);
	}

	static bool is_executable(uint64_t address)
	{
		if (!s_image)
		{
			return false;
		}
		const auto rva = s_image->virtual_address_to_rva(address);
		return rva && s_image->is_executable(*rva);
	}

	static kcd2_address resolved(std::string_view name)
	{
		if (!s_image)
		{
			return {};
		}
		const auto rva = s_report.target(name);
		return rva ? kcd2_address(s_image->rva_to_virtual_address(*rva)) : kcd2_address{};
	}

	static kcd2_address derived(std::string_view name)
	{
		if (!s_image)
		{
			return {};
		}
		const auto rva = s_report.derived(name);
		return rva ? kcd2_address(s_image->rva_to_virtual_address(*rva)) : kcd2_address{};
	}

	static kcd2_address scan(const char *pattern_text, const char *debug_name = nullptr)
	{
		if (debug_name)
		{
			const auto match = s_report.match(debug_name);
			if (match && s_image)
			{
				return kcd2_address(s_image->rva_to_virtual_address(*match));
			}
			return {};
		}
		return scan_optional(pattern_text);
	}

	kcd2_address offset(int32_t value) const
	{
		if (!m_value)
		{
			return {};
		}
		uint64_t result{};
		if (value >= 0)
		{
			if (m_value > std::numeric_limits<uint64_t>::max() - static_cast<uint32_t>(value))
			{
				return {};
			}
			result = m_value + static_cast<uint32_t>(value);
		}
		else
		{
			const auto magnitude = static_cast<uint64_t>(-static_cast<int64_t>(value));
			if (m_value < magnitude)
			{
				return {};
			}
			result = m_value - magnitude;
		}
		return contains(result) ? kcd2_address(result) : kcd2_address{};
	}

	kcd2_address rip(int32_t value = 0) const
	{
		if (!s_image)
		{
			return {};
		}
		const auto instruction = offset(value);
		const auto rva = s_image->virtual_address_to_rva(instruction.m_value);
		if (!rva)
		{
			return {};
		}
		std::string error;
		const auto target = kcd2::signatures::resolve_rip_relative_memory(*s_image, *rva, error);
		return target ? kcd2_address(s_image->rva_to_virtual_address(*target)) : kcd2_address{};
	}

	kcd2_address get_call() const
	{
		if (!s_image)
		{
			return {};
		}
		const auto rva = s_image->virtual_address_to_rva(m_value);
		if (!rva)
		{
			return {};
		}
		std::string error;
		const auto target = kcd2::signatures::resolve_relative_call(*s_image, *rva, error);
		return target ? kcd2_address(s_image->rva_to_virtual_address(*target)) : kcd2_address{};
	}

	template<typename T>
	T as() const
	{
		return reinterpret_cast<T>(m_value);
	}

	template<typename T>
	T *as_func() const
	{
		return as<T *>();
	}

	kcd2_address &operator=(uint64_t value)
	{
		m_value = value;
		return *this;
	}

	explicit operator bool() const
	{
		return m_value != 0;
	}

	operator uint64_t() const
	{
		return m_value;
	}

	operator void *() const
	{
		return reinterpret_cast<void *>(m_value);
	}
};
