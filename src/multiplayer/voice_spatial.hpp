#pragma once

#include "multiplayer/protocol.hpp"

namespace kcd2o
{
	struct voice_audio_coordinates
	{
		float x{};
		float y{};
		float z{};
	};

	// KCD2/CryEngine uses Z-up coordinates while FMOD uses Y-up coordinates.
	// Feeding the game vector through unchanged turns a world-space Y value into
	// a huge vertical distance and makes an otherwise valid 3D voice inaudible.
	[[nodiscard]] inline voice_audio_coordinates to_voice_audio_coordinates(
	    const protocol::Vec3 &value,
	    float vertical_offset = 0.0F) noexcept
	{
		return {value.x(), value.z() + vertical_offset, value.y()};
	}
}
