#pragma once

#include "kcd2o.pb.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace kcd2o
{
	constexpr double hours_per_day = 24.0;
	constexpr double seconds_per_hour = 3600.0;
	constexpr double seconds_per_day = hours_per_day * seconds_per_hour;
	constexpr double maximum_world_time_seconds = 1.0e12;
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
	[[nodiscard]] double project_world_time_seconds(
	    double anchor_seconds,
	    float time_scale,
	    Duration elapsed) noexcept
	{
		const auto real_seconds =
		    std::chrono::duration<double>(elapsed).count();
		return anchor_seconds
		    + real_seconds * static_cast<double>(time_scale);
	}

	template<typename Duration>
	[[nodiscard]] double project_time_of_day_hours(
	    double anchor_hours,
	    float time_scale,
	    Duration elapsed) noexcept
	{
		return normalize_time_of_day_hours(
		    project_world_time_seconds(
		        anchor_hours * seconds_per_hour,
		        time_scale,
		        elapsed)
		    / seconds_per_hour);
	}

	[[nodiscard]] inline double next_world_time_at_hour(
	    double current_world_time_seconds,
	    double target_hours) noexcept
	{
		const auto current_day =
		    std::floor(current_world_time_seconds / seconds_per_day);
		auto target = current_day * seconds_per_day
		    + normalize_time_of_day_hours(target_hours) * seconds_per_hour;
		if (target + 0.000001 < current_world_time_seconds)
			target += seconds_per_day;
		return target;
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
		    && std::isfinite(state.world_time_seconds())
		    && state.world_time_seconds() >= 0.0
		    && state.world_time_seconds() <= maximum_world_time_seconds
		    && circular_time_distance_hours(
		           state.time_of_day_hours(),
		           state.world_time_seconds() / seconds_per_hour)
		        < 0.001
		    && std::isfinite(state.time_scale()) && state.time_scale() >= 0.0F
		    && state.time_scale() <= maximum_time_scale
		    && state.weather_id() >= minimum_weather_id
		    && state.weather_id() <= maximum_weather_id
		    && state.weather_transition_ms()
		        <= maximum_weather_transition_ms;
	}
}
