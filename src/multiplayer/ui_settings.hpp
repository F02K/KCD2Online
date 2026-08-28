#pragma once

#include <string>

namespace kcd2o
{
	struct multiplayer_ui_settings
	{
		std::string address;
		std::string display_name;
		std::string account_service_url;
		std::string voice_input_device_id;
		float voice_input_gain{1.0F};
		float voice_output_volume{1.0F};
		int voice_noise_suppression_db{-24};
		bool voice_noise_suppression{true};
		bool voice_automatic_gain{true};
		bool voice_gate{};
		int voice_gate_probability{55};

		void persist_address() const;
		void persist_display_name() const;
		void persist_voice() const;
	};

	[[nodiscard]] multiplayer_ui_settings &ui_settings();
}
