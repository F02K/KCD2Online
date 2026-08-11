#pragma once

#include "multiplayer/protocol.hpp"

#include <array>
#include <string_view>

namespace kcd2o
{
	enum class remote_locomotion_animation
	{
		idle,
		walk,
		run,
		sprint
	};

	struct remote_locomotion_animation_definition
	{
		remote_locomotion_animation state;
		std::string_view name;
	};

	// These are in-place human character clips. Networked world transforms stay
	// authoritative; the clip supplies only the visible pose and limb motion.
	inline constexpr std::array remote_locomotion_animations{
	    remote_locomotion_animation_definition{
	        remote_locomotion_animation::idle,
	        "relaxed_idle_both"},
	    remote_locomotion_animation_definition{
	        remote_locomotion_animation::walk,
	        "3d_relaxed_walk_turn_strafe"},
	    remote_locomotion_animation_definition{
	        remote_locomotion_animation::run,
	        "3d_relaxed_run_turn_strafe"},
	    remote_locomotion_animation_definition{
	        remote_locomotion_animation::sprint,
	        "3d_relaxed_sprint_turn_strafe"}};

	[[nodiscard]] constexpr std::string_view remote_locomotion_animation_name(
	    remote_locomotion_animation state) noexcept
	{
		for (const auto &animation : remote_locomotion_animations)
		{
			if (animation.state == state)
				return animation.name;
		}
		return remote_locomotion_animations.front().name;
	}

	[[nodiscard]] constexpr remote_locomotion_animation
	remote_locomotion_animation_for_mode(
	    protocol::MovementMode mode) noexcept
	{
		switch (mode)
		{
		case protocol::MOVEMENT_MODE_WALK:
			return remote_locomotion_animation::walk;
		case protocol::MOVEMENT_MODE_RUN:
			return remote_locomotion_animation::run;
		case protocol::MOVEMENT_MODE_SPRINT:
			return remote_locomotion_animation::sprint;
		case protocol::MOVEMENT_MODE_IDLE:
		default:
			return remote_locomotion_animation::idle;
		}
	}

	// Separate entry and exit thresholds prevent walk/run/sprint from flickering
	// when interpolation leaves the rendered speed close to a boundary.
	[[nodiscard]] constexpr remote_locomotion_animation
	select_remote_locomotion_animation(
	    float rendered_speed,
	    remote_locomotion_animation current) noexcept
	{
		// Values used by the proven Ghost renderer. They are intentionally based
		// on rendered metres/second, not the sender's requested input state.
		constexpr float walk_enter = 1.00F;
		constexpr float walk_exit = 0.40F;
		constexpr float run_enter = 2.50F;
		constexpr float run_exit = 1.80F;
		constexpr float sprint_enter = 4.00F;
		constexpr float sprint_exit = 3.20F;

		if (rendered_speed < 0.0F)
			rendered_speed = 0.0F;
		switch (current)
		{
		case remote_locomotion_animation::sprint:
			if (rendered_speed >= sprint_exit)
				return remote_locomotion_animation::sprint;
			return rendered_speed >= run_exit
			    ? remote_locomotion_animation::run
			    : rendered_speed >= walk_exit
			    ? remote_locomotion_animation::walk
			    : remote_locomotion_animation::idle;
		case remote_locomotion_animation::run:
			if (rendered_speed >= sprint_enter)
				return remote_locomotion_animation::sprint;
			if (rendered_speed >= run_exit)
				return remote_locomotion_animation::run;
			return rendered_speed >= walk_exit
			    ? remote_locomotion_animation::walk
			    : remote_locomotion_animation::idle;
		case remote_locomotion_animation::walk:
			if (rendered_speed >= sprint_enter)
				return remote_locomotion_animation::sprint;
			if (rendered_speed >= run_enter)
				return remote_locomotion_animation::run;
			return rendered_speed >= walk_exit
			    ? remote_locomotion_animation::walk
			    : remote_locomotion_animation::idle;
		case remote_locomotion_animation::idle:
		default:
			if (rendered_speed >= sprint_enter)
				return remote_locomotion_animation::sprint;
			if (rendered_speed >= run_enter)
				return remote_locomotion_animation::run;
			return rendered_speed >= walk_enter
			    ? remote_locomotion_animation::walk
			    : remote_locomotion_animation::idle;
		}
	}
}
