#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace kcd2o::resources
{
	enum class builtin_feature : std::uint64_t
	{
		text_chat = 1ULL << 0,
		voice_chat = 1ULL << 1,
		player_hub = 1ULL << 2,
		social_menu = 1ULL << 3,
		staff_menu = 1ULL << 4,
		properties = 1ULL << 5
	};

	struct builtin_feature_definition
	{
		builtin_feature feature;
		std::string_view name;
		std::string_view capability;
	};

	inline constexpr std::array builtin_features{
	    builtin_feature_definition{builtin_feature::text_chat,
	        "text_chat", "feature.text_chat"},
	    builtin_feature_definition{builtin_feature::voice_chat,
	        "voice_chat", "feature.voice_chat"},
	    builtin_feature_definition{builtin_feature::player_hub,
	        "player_hub", "feature.player_hub"},
	    builtin_feature_definition{builtin_feature::social_menu,
	        "social_menu", "feature.social_menu"},
	    builtin_feature_definition{builtin_feature::staff_menu,
	        "staff_menu", "feature.staff_menu"},
	    builtin_feature_definition{builtin_feature::properties,
	        "properties", "feature.properties"}};

	[[nodiscard]] constexpr std::uint64_t feature_bit(
	    builtin_feature feature) noexcept
	{
		return static_cast<std::uint64_t>(feature);
	}

	[[nodiscard]] constexpr std::optional<builtin_feature>
	builtin_feature_from_name(std::string_view name) noexcept
	{
		for (const auto &definition : builtin_features)
			if (definition.name == name)
				return definition.feature;
		return std::nullopt;
	}

	[[nodiscard]] constexpr std::optional<builtin_feature>
	builtin_feature_from_capability(std::string_view capability) noexcept
	{
		for (const auto &definition : builtin_features)
			if (definition.capability == capability)
				return definition.feature;
		return std::nullopt;
	}

	[[nodiscard]] constexpr std::string_view builtin_feature_capability(
	    builtin_feature feature) noexcept
	{
		for (const auto &definition : builtin_features)
			if (definition.feature == feature)
				return definition.capability;
		return {};
	}
}
