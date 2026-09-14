#include "multiplayer/ui_settings.hpp"

#include <config/config.hpp>

namespace kcd2o
{
	namespace
	{
		auto *address_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer",
			    "Address",
			    std::string{"127.0.0.1:27020"},
			    "Last Direct-IP server address.");
			return entry;
		}

		auto *display_name_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer",
			    "Display Name",
			    std::string{"Henry"},
			    "Last multiplayer display name.");
			return entry;
		}

		auto *account_service_url_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer",
			    "Account Service URL",
			    std::string{"https://api.kingdom-online.cc"},
			    "KCD2Online account and server-browser service URL.");
			return entry;
		}

		auto *voice_device_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer Voice", "Input Device", std::string{},
			    "Preferred Windows microphone endpoint; empty uses the communications default.");
			return entry;
		}

		auto *voice_gain_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer Voice", "Input Gain", 1.0,
			    "Microphone gain applied before voice processing.");
			return entry;
		}

		auto *voice_suppression_db_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer Voice", "Noise Suppression dB", -24,
			    "Maximum stationary-noise attenuation in decibels.");
			return entry;
		}

		auto *voice_output_volume_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer Voice", "Output Volume", 1.0,
			    "Master playback volume for incoming multiplayer voice.");
			return entry;
		}

		auto *voice_suppression_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer Voice", "Noise Suppression", true,
			    "Remove stationary background noise from microphone audio.");
			return entry;
		}

		auto *voice_agc_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer Voice", "Automatic Gain", true,
			    "Automatically normalize quiet and loud microphones.");
			return entry;
		}

		auto *voice_gate_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer Voice", "Voice Gate", false,
			    "Transmit only audio classified as speech while push-to-talk is held.");
			return entry;
		}

		auto *voice_gate_probability_entry()
		{
			static auto *entry = big::config::general().bind(
			    "Multiplayer Voice", "Voice Gate Probability", 55,
			    "Speech probability required to open the voice gate.");
			return entry;
		}
	}

	void multiplayer_ui_settings::persist_address() const
	{
		address_entry()->set_value(address);
	}

	void multiplayer_ui_settings::persist_display_name() const
	{
		display_name_entry()->set_value(display_name);
	}

	void multiplayer_ui_settings::persist_voice() const
	{
		voice_device_entry()->set_value(voice_input_device_id);
		voice_gain_entry()->set_value(static_cast<double>(voice_input_gain));
		voice_output_volume_entry()->set_value(static_cast<double>(voice_output_volume));
		voice_suppression_db_entry()->set_value(voice_noise_suppression_db);
		voice_suppression_entry()->set_value(voice_noise_suppression);
		voice_agc_entry()->set_value(voice_automatic_gain);
		voice_gate_entry()->set_value(voice_gate);
		voice_gate_probability_entry()->set_value(voice_gate_probability);
	}

	multiplayer_ui_settings &ui_settings()
	{
		static multiplayer_ui_settings settings{
		    address_entry()->get_value(),
		    display_name_entry()->get_value(),
		    account_service_url_entry()->get_value(),
		    voice_device_entry()->get_value(),
		    static_cast<float>(voice_gain_entry()->get_value()),
		    static_cast<float>(voice_output_volume_entry()->get_value()),
		    voice_suppression_db_entry()->get_value(),
		    voice_suppression_entry()->get_value(),
		    voice_agc_entry()->get_value(),
		    voice_gate_entry()->get_value(),
		    voice_gate_probability_entry()->get_value()};
		return settings;
	}
}
