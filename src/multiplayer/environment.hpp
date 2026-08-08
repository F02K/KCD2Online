#pragma once

#include "kcd2o.pb.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace kcd2o
{
	constexpr double hours_per_day = 24.0;
	constexpr float maximum_time_scale = 1000.0F;
	constexpr std::uint32_t minimum_weather_id = 1;
	constexpr std::uint32_t maximum_weather_id = 33;
	constexpr std::uint32_t maximum_weather_transition_ms = 600'000;

	[[nodiscard]] inline double normalize_time_of_day_hours(
	    double hours) noexcept
	{
		hours = std::fmod(hours, hours_per_day);
		return hours < 0.0 ? hours + hours_per_day : hours;
	}

	template<typename Duration>
	[[nodiscard]] double project_time_of_day_hours(
	    double anchor_hours,
	    float time_scale,
	    Duration elapsed) noexcept
	{
		const auto real_seconds =
		    std::chrono::duration<double>(elapsed).count();
		return normalize_time_of_day_hours(
		    anchor_hours + real_seconds * static_cast<double>(time_scale) / 3600.0);
	}

	[[nodiscard]] inline double circular_time_distance_hours(
	    double left,
	    double right) noexcept
	{
		const auto direct = std::abs(
		    normalize_time_of_day_hours(left)
		    - normalize_time_of_day_hours(right));
		return std::min(direct, hours_per_day - direct);
	}

	[[nodiscard]] inline bool is_valid_environment_state(
	    const protocol::EnvironmentState &state) noexcept
	{
		return state.revision() != 0
		    && state.weather_revision() != 0
		    && std::isfinite(state.time_of_day_hours())
		    && state.time_of_day_hours() >= 0.0
		    && state.time_of_day_hours() < hours_per_day
		    && std::isfinite(state.time_scale()) && state.time_scale() >= 0.0F
		    && state.time_scale() <= maximum_time_scale
		    && state.weather_id() >= minimum_weather_id
		    && state.weather_id() <= maximum_weather_id
		    && state.weather_transition_ms()
		        <= maximum_weather_transition_ms;
	}
}
