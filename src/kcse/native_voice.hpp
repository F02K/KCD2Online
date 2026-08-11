#pragma once

#include "multiplayer/protocol.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace kcd2o::kcse
{
	struct voice_player_pose
	{
		player_id id{};
		std::uint32_t entity_id{};
		protocol::Vec3 position;
		protocol::Vec3 velocity;
	};

	struct voice_capture_state
	{
		bool recording{};
		bool speaking{};
		float level{};
		protocol::VoiceRange range{protocol::VOICE_RANGE_NORMAL};
	};

	// Owns the platform capture thread, Opus state, adaptive receive queues and
	// the live FMOD user streams. All KCD2-facing work is performed by tick() on
	// the game thread; receive() and poll_outbound() are thread-safe.
	class native_voice
	{
	public:
		native_voice();
		~native_voice();
		native_voice(const native_voice &) = delete;
		native_voice &operator=(const native_voice &) = delete;

		void set_active(bool active) noexcept;
		[[nodiscard]] voice_capture_state capture_state() const noexcept;
		[[nodiscard]] std::vector<protocol::ClientVoiceFrame> poll_outbound();
		void receive(const protocol::ServerVoiceFrame &frame);
		void update_players(std::span<const voice_player_pose> players);
		void tick();
		void reset();

	private:
		class implementation;
		std::unique_ptr<implementation> m_impl;
	};
}
