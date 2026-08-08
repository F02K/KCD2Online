#pragma once

#include "npc/npc.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

namespace kcd2o::npc
{
	struct xgen_spawn_parameters
	{
		std::string name;
		std::string class_name{"NPC"};
		std::string shared_soul_guid;
		std::array<float, 3> position{};
		std::array<float, 3> rotation{};
		bool no_ai{true};
		bool idle_until_first_patch{true};
		bool perceptor_object_ai{};
		bool perceptible_object_ai{};
	};

	[[nodiscard]] inline xgen_spawn_parameters make_xgen_spawn_parameters(const spawn_request &request, native_handle handle)
	{
		const auto [x, y, z, w] = request.world_transform.rotation;
		const auto sin_roll     = 2.0F * (w * x + y * z);
		const auto cos_roll     = 1.0F - 2.0F * (x * x + y * y);
		const auto roll         = std::atan2(sin_roll, cos_roll);
		const auto sin_pitch    = std::clamp(2.0F * (w * y - z * x), -1.0F, 1.0F);
		const auto pitch        = std::asin(sin_pitch);
		const auto sin_yaw      = 2.0F * (w * z + x * y);
		const auto cos_yaw      = 1.0F - 2.0F * (y * y + z * z);
		return {.name             = std::format("KCD2Online_Remote_{}", handle),
		        .shared_soul_guid = request.archetype_id,
		        .position         = request.world_transform.position,
		        .rotation         = {roll, pitch, std::atan2(sin_yaw, cos_yaw)}};
	}
} // namespace kcd2o::npc
