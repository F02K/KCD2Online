#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace kcd2o
{
	enum class emote_kind : std::uint32_t
	{
		bow = 1,
		cheer = 2,
		point = 3,
		surrender = 4
	};

	struct emote_definition
	{
		emote_kind kind;
		std::string_view id;
		std::string_view label;
		std::string_view fragment;
		std::string_view tags;
		std::string_view skeleton_clip;
		std::uint32_t duration_ms;
	};

	// The fragment/tags pair drives the local player through Mannequin. The
	// skeleton clip is the concrete animation selected by that fragment in the
	// retail kcd_male_database.adb and is used for replicated visual actors,
	// whose Human Mannequin action controller is not authoritative. Keep this
	// list restricted to non-looping, unaligned animations with zero root
	// movement and no item dependency; the native boundary never accepts an
	// arbitrary fragment or clip name.
	inline constexpr std::array emote_catalog{
	    emote_definition{emote_kind::bow, "bow", "emote.bow", "Greetings", "bowBig", "greetings_bow", 3'400},
	    emote_definition{emote_kind::cheer, "cheer", "emote.cheer", "Soldier_Cheers", "", "soldier_speech_cheer08", 4'500},
	    emote_definition{emote_kind::point, "point", "emote.point", "CrowdPeasantPoint", "", "crowd_peasant_male_audience_point_03", 4'900},
	    emote_definition{emote_kind::surrender, "surrender", "emote.surrender", "NoWeaponSurrender", "", "dlg_male_neutral_stand_disown_01", 4'300}};

	[[nodiscard]] constexpr const emote_definition *find_emote(
	    emote_kind kind) noexcept
	{
		for (const auto &emote : emote_catalog)
		{
			if (emote.kind == kind)
				return &emote;
		}
		return nullptr;
	}

	[[nodiscard]] constexpr const emote_definition *find_emote_fragment(
	    std::string_view fragment) noexcept
	{
		for (const auto &emote : emote_catalog)
		{
			if (emote.fragment == fragment)
				return &emote;
		}
		return nullptr;
	}
}
