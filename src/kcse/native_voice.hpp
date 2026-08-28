#pragma once

#include "multiplayer/protocol.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
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
		bool testing{};
		bool available{};
		float level{};
		protocol::VoiceRange range{protocol::VOICE_RANGE_NORMAL};
		std::string device_name;
		std::string diagnostic;
	};

	struct voice_settings
	{
		std::string input_device_id;
		float input_gain{1.0F};
		float output_volume{1.0F};
		bool noise_suppression{true};
		int noise_suppression_db{-24};
		bool automatic_gain{true};
		bool voice_gate{};
		int voice_gate_probability{55};
		bool microphone_test{};
	};

	struct voice_input_device
	{
		std::string id;
		std::string name;
		bool is_default{};
	};

	// Owns the platform capture thread, Opus state, adaptive receive queues and
	// the live FMOD user streams. All KCD2-facing work is performed by tick() on
	// the game thread; receive() and poll_outbound() are thread-safe.
	class native_voice
	{
	public:
		native_voice();
		~native_voice();
		native_voice(const native_voice &)            = delete;
		native_voice &operator=(const native_voice &) = delete;

		void set_active(bool active) noexcept;
		[[nodiscard]] voice_capture_state capture_state() const;
		[[nodiscard]] voice_settings settings() const;
		[[nodiscard]] bool set_settings(const voice_settings &settings);
		[[nodiscard]] std::vector<voice_input_device> input_devices() const;
		void refresh_input_devices() noexcept;
		void set_server_config(const protocol::VoiceConfig &config) noexcept;
		[[nodiscard]] std::vector<protocol::ClientVoiceFrame> poll_outbound();
		void receive(const protocol::ServerVoiceFrame &frame);
		[[nodiscard]] bool set_player_volume(player_id player, float volume) noexcept;
		void update_players(std::span<const voice_player_pose> players);
		void tick();
		void reset();

	private:
		class implementation;
		std::unique_ptr<implementation> m_impl;
	};
} // namespace kcd2o::kcse
