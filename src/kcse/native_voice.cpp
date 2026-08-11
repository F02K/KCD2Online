#include "kcse/native_voice.hpp"

#include "kcse/join_trace.hpp"
#include "kcse/native_keybinds.hpp"
#include "multiplayer/voice_spatial.hpp"

#include <REL/Relocation.h>
#include <crysystem/SSystemGlobalEnvironment.h>
#include <Offsets/vtables/IScriptSystem.h>

#include <Windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <opus/opus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kcd2o::kcse
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;

		constexpr int sample_rate = 48'000;
		constexpr int frame_samples = 960;
		constexpr std::size_t click_fade_samples = sample_rate / 200;
		constexpr int opus_bitrate = 32'000;
		constexpr std::size_t pcm_ring_samples = sample_rate * 2;
		constexpr std::size_t receive_queue_limit = 512;
		constexpr std::size_t outbound_queue_limit = 150;
		constexpr float voice_min_distance = 1.5F;
		constexpr float head_height = 1.65F;
		constexpr auto minimum_jitter_frames = 3U;
		constexpr auto maximum_jitter_frames = 6U;
		constexpr auto speaker_timeout = 10s;
		constexpr auto talking_timeout = 350ms;

		std::uint64_t now_ms()
		{
			return static_cast<std::uint64_t>(
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        clock::now().time_since_epoch()).count());
		}

		class pcm_ring
		{
		public:
			[[nodiscard]] std::size_t readable() const noexcept
			{
				const auto write = m_write.load(std::memory_order_acquire);
				const auto read = m_read.load(std::memory_order_relaxed);
				return write - read;
			}

			[[nodiscard]] std::size_t writable() const noexcept
			{
				return m_data.size() - readable();
			}

			bool push(std::span<const std::int16_t> samples) noexcept
			{
				const auto read = m_read.load(std::memory_order_acquire);
				auto write = m_write.load(std::memory_order_relaxed);
				if (samples.size() > m_data.size() - (write - read))
					return false;
				for (const auto sample : samples)
					m_data[write++ % m_data.size()] = sample;
				m_write.store(write, std::memory_order_release);
				return true;
			}

			void pop(std::span<std::int16_t> output) noexcept
			{
				auto read = m_read.load(std::memory_order_relaxed);
				const auto write = m_write.load(std::memory_order_acquire);
				const auto count = std::min(output.size(), write - read);
				for (std::size_t index{}; index < count; ++index)
					output[index] = m_data[read++ % m_data.size()];
				if (m_starved && count != 0)
				{
					const auto fade = std::min(count, click_fade_samples);
					for (std::size_t index{}; index < fade; ++index)
					{
						output[index] = static_cast<std::int16_t>(
						    static_cast<std::int32_t>(output[index])
						    * static_cast<std::int32_t>(index + 1)
						    / static_cast<std::int32_t>(fade));
					}
					m_starved = false;
				}
				if (count != 0)
					m_last_output = output[count - 1];
				if (count < output.size())
				{
					const auto missing = output.size() - count;
					const auto fade = std::min(missing, click_fade_samples);
					for (std::size_t index{}; index < fade; ++index)
					{
						output[count + index] = static_cast<std::int16_t>(
						    static_cast<std::int32_t>(m_last_output)
						    * static_cast<std::int32_t>(fade - index - 1)
						    / static_cast<std::int32_t>(fade));
					}
					std::fill(
					    output.begin()
					        + static_cast<std::ptrdiff_t>(count + fade),
					    output.end(),
					    0);
					if (!m_starved)
						m_starvations.fetch_add(1, std::memory_order_relaxed);
					m_starved = true;
					m_last_output = 0;
				}
				m_read.store(read, std::memory_order_release);
			}

			[[nodiscard]] std::uint32_t take_starvations() noexcept
			{
				return m_starvations.exchange(0, std::memory_order_acq_rel);
			}

			void clear() noexcept
			{
				const auto write = m_write.load(std::memory_order_acquire);
				m_read.store(write, std::memory_order_release);
			}

		private:
			std::array<std::int16_t, pcm_ring_samples> m_data{};
			std::atomic<std::size_t> m_read{};
			std::atomic<std::size_t> m_write{};
			std::atomic<std::uint32_t> m_starvations{};
			std::int16_t m_last_output{};
			bool m_starved{true};
		};

		std::string visemes_for(std::span<const std::int16_t> pcm)
		{
			std::array<unsigned char, voice_viseme_count> weights{};
			double energy{};
			double derivative{};
			std::size_t crossings{};
			for (std::size_t index{}; index < pcm.size(); ++index)
			{
				const auto sample = static_cast<double>(pcm[index]) / 32768.0;
				energy += sample * sample;
				if (index != 0)
				{
					derivative += std::abs(
					    static_cast<int>(pcm[index])
					    - static_cast<int>(pcm[index - 1]));
					crossings += (pcm[index] < 0) != (pcm[index - 1] < 0);
				}
			}
			const auto rms = std::sqrt(energy / std::max<std::size_t>(1, pcm.size()));
			const auto open = std::clamp((rms - 0.008) * 9.0, 0.0, 1.0);
			const auto brightness = std::clamp(
			    derivative / std::max<std::size_t>(1, pcm.size()) / 8000.0,
			    0.0,
			    1.0);
			const auto sibilance = std::clamp(
			    static_cast<double>(crossings) / pcm.size() * 7.0,
			    0.0,
			    1.0);
			const auto byte = [](double value)
			{
				return static_cast<unsigned char>(
				    std::clamp(std::lround(value * 255.0), 0L, 255L));
			};

			weights[0] = byte(1.0 - open);
			weights[1] = byte((1.0 - open) * 0.55);
			weights[2] = byte(open * brightness * 0.55);
			weights[3] = byte(open * brightness * 0.35);
			weights[4] = byte(open * (1.0 - brightness) * 0.25);
			weights[5] = byte(open * brightness * 0.3);
			weights[6] = byte(open * sibilance * 0.45);
			weights[7] = byte(open * sibilance);
			weights[8] = byte(open * (1.0 - sibilance) * 0.25);
			weights[9] = byte(open * (1.0 - brightness) * 0.35);
			weights[10] = byte(open * (1.0 - brightness));
			weights[11] = byte(open * brightness);
			weights[12] = byte(open * (0.4 + brightness * 0.4));
			weights[13] = byte(open * (1.0 - brightness) * 0.75);
			weights[14] = byte(open * (1.0 - brightness) * 0.55);
			return {reinterpret_cast<const char *>(weights.data()), weights.size()};
		}

		float voice_level(std::span<const std::int16_t> pcm)
		{
			double energy{};
			for (const auto sample : pcm)
			{
				const auto normalized = static_cast<double>(sample) / 32768.0;
				energy += normalized * normalized;
			}
			const auto rms = std::sqrt(
				energy / std::max<std::size_t>(1, pcm.size()));
			return static_cast<float>(
				std::clamp((rms - 0.008) * 9.0, 0.0, 1.0));
		}

		bool is_float_format(const WAVEFORMATEX &format)
		{
			if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
				return true;
			if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE
			    || format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
				return false;
			return reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format).SubFormat
			    == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
		}

		bool is_pcm_format(const WAVEFORMATEX &format)
		{
			if (format.wFormatTag == WAVE_FORMAT_PCM)
				return true;
			if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE
			    || format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
				return false;
			return reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format).SubFormat
			    == KSDATAFORMAT_SUBTYPE_PCM;
		}

		float read_input_sample(
		    const std::byte *frame,
		    std::uint16_t channel,
		    const WAVEFORMATEX &format)
		{
			const auto bytes = format.wBitsPerSample / 8;
			const auto *source = frame + static_cast<std::size_t>(channel) * bytes;
			if (is_float_format(format) && format.wBitsPerSample == 32)
			{
				float value{};
				std::memcpy(&value, source, sizeof(value));
				return std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
			}
			if (!is_pcm_format(format))
				return 0.0F;
			if (format.wBitsPerSample == 16)
			{
				std::int16_t value{};
				std::memcpy(&value, source, sizeof(value));
				return static_cast<float>(value) / 32768.0F;
			}
			if (format.wBitsPerSample == 24)
			{
				std::int32_t value = std::to_integer<unsigned char>(source[0])
				    | (std::to_integer<unsigned char>(source[1]) << 8)
				    | (std::to_integer<unsigned char>(source[2]) << 16);
				if ((value & 0x800000) != 0)
					value |= ~0xFFFFFF;
				return static_cast<float>(value) / 8388608.0F;
			}
			if (format.wBitsPerSample == 32)
			{
				std::int32_t value{};
				std::memcpy(&value, source, sizeof(value));
				return static_cast<float>(value / 2147483648.0);
			}
			return 0.0F;
		}

		struct fmod_system;
		struct fmod_sound;
		struct fmod_channel;
		struct fmod_channel_group;
		struct fmod_studio_system;
		struct fmod_studio_bus;
		using fmod_result = std::int32_t;
		using fmod_mode = std::uint32_t;
		struct fmod_vector { float x{}, y{}, z{}; };
		using pcm_read_callback = fmod_result(__stdcall *)(
		    fmod_sound *, void *, unsigned int);

		struct fmod_create_sound_ex_info
		{
			std::int32_t cbsize{};
			std::uint32_t length{};
			std::uint32_t fileoffset{};
			std::int32_t numchannels{};
			std::int32_t defaultfrequency{};
			std::int32_t format{};
			std::uint32_t decodebuffersize{};
			std::int32_t initialsubsound{};
			std::int32_t numsubsounds{};
			std::uint32_t alignment_pad{};
			std::int32_t *inclusionlist{};
			std::int32_t inclusionlistnum{};
			std::uint32_t callback_pad{};
			pcm_read_callback pcmreadcallback{};
			void *pcmsetposcallback{};
			void *nonblockcallback{};
			const char *dlsname{};
			const char *encryptionkey{};
			std::int32_t maxpolyphony{};
			std::uint32_t userdata_pad{};
			void *userdata{};
			std::byte reserved[0x70]{};
		};
		static_assert(sizeof(fmod_create_sound_ex_info) == 0xE0);
		static_assert(offsetof(fmod_create_sound_ex_info, pcmreadcallback) == 0x38);
		static_assert(offsetof(fmod_create_sound_ex_info, userdata) == 0x68);

		constexpr fmod_result fmod_ok = 0;
		constexpr fmod_mode fmod_loop_normal = 0x00000002;
		constexpr fmod_mode fmod_3d = 0x00000010;
		constexpr fmod_mode fmod_create_stream = 0x00000080;
		constexpr fmod_mode fmod_open_user = 0x00000400;
		constexpr std::int32_t fmod_pcm16 = 2;

		struct fmod_api
		{
			fmod_result (*system_create_sound)(fmod_system *, const char *, fmod_mode,
			    fmod_create_sound_ex_info *, fmod_sound **){};
			fmod_result (*system_play_sound)(fmod_system *, fmod_sound *,
			    fmod_channel_group *, int, fmod_channel **){};
			fmod_result (*sound_release)(fmod_sound *){};
			fmod_result (*sound_get_user_data)(fmod_sound *, void **){};
			fmod_result (*channel_stop)(fmod_channel *){};
			fmod_result (*channel_set_paused)(fmod_channel *, int){};
			fmod_result (*channel_set_volume)(fmod_channel *, float){};
			fmod_result (*channel_set_mode)(fmod_channel *, fmod_mode){};
			fmod_result (*channel_set_3d_attributes)(fmod_channel *,
			    const fmod_vector *, const fmod_vector *){};
			fmod_result (*channel_set_3d_min_max_distance)(fmod_channel *, float, float){};
			fmod_result (*studio_get_bus)(fmod_studio_system *, const char *,
			    fmod_studio_bus **){};
			fmod_result (*studio_bus_lock)(fmod_studio_bus *){};
			fmod_result (*studio_bus_unlock)(fmod_studio_bus *){};
			fmod_result (*studio_flush)(fmod_studio_system *){};
			fmod_result (*studio_bus_get_group)(fmod_studio_bus *,
			    fmod_channel_group **){};

			bool resolve()
			{
				auto core = GetModuleHandleW(L"fmod.dll");
				auto studio = GetModuleHandleW(L"fmodstudio.dll");
				if (!core || !studio)
					return false;
				const auto load = [](HMODULE module, const char *name)
				{ return GetProcAddress(module, name); };
#define KCD2O_FMOD_CORE(member, name) \
	member = reinterpret_cast<decltype(member)>(load(core, name)); if (!member) return false
#define KCD2O_FMOD_STUDIO(member, name) \
	member = reinterpret_cast<decltype(member)>(load(studio, name)); if (!member) return false
				KCD2O_FMOD_CORE(system_create_sound, "FMOD_System_CreateSound");
				KCD2O_FMOD_CORE(system_play_sound, "FMOD_System_PlaySound");
				KCD2O_FMOD_CORE(sound_release, "FMOD_Sound_Release");
				KCD2O_FMOD_CORE(sound_get_user_data, "FMOD_Sound_GetUserData");
				KCD2O_FMOD_CORE(channel_stop, "FMOD_Channel_Stop");
				KCD2O_FMOD_CORE(channel_set_paused, "FMOD_Channel_SetPaused");
				KCD2O_FMOD_CORE(channel_set_volume, "FMOD_Channel_SetVolume");
				KCD2O_FMOD_CORE(channel_set_mode, "FMOD_Channel_SetMode");
				KCD2O_FMOD_CORE(channel_set_3d_attributes, "FMOD_Channel_Set3DAttributes");
				KCD2O_FMOD_CORE(channel_set_3d_min_max_distance,
				    "FMOD_Channel_Set3DMinMaxDistance");
				KCD2O_FMOD_STUDIO(studio_get_bus, "FMOD_Studio_System_GetBus");
				KCD2O_FMOD_STUDIO(studio_bus_lock, "FMOD_Studio_Bus_LockChannelGroup");
				KCD2O_FMOD_STUDIO(studio_bus_unlock, "FMOD_Studio_Bus_UnlockChannelGroup");
				KCD2O_FMOD_STUDIO(studio_flush, "FMOD_Studio_System_FlushCommands");
				KCD2O_FMOD_STUDIO(studio_bus_get_group,
				    "FMOD_Studio_Bus_GetChannelGroup");
#undef KCD2O_FMOD_CORE
#undef KCD2O_FMOD_STUDIO
				return true;
			}
		};

		struct queued_voice
		{
			std::uint32_t sequence{};
			std::string opus;
			std::string visemes;
			protocol::VoiceRange range{protocol::VOICE_RANGE_NORMAL};
			bool end{};
			clock::time_point received_at{clock::now()};
		};
	}

	class native_voice::implementation
	{
	public:
		implementation()
		{
			m_capture = std::jthread([this](std::stop_token stop)
			{
				capture_loop(stop);
			});
		}

		~implementation()
		{
			m_capture.request_stop();
			if (m_capture.joinable())
				m_capture.join();
			reset_game_thread();
		}

		void set_active(bool active) noexcept
		{
			m_active.store(active, std::memory_order_release);
			if (!active)
			{
				m_recording.store(false, std::memory_order_release);
				m_speaking.store(false, std::memory_order_release);
				m_capture_level.store(0.0F, std::memory_order_release);
			}
		}

		[[nodiscard]] voice_capture_state capture_state() const noexcept
		{
			voice_capture_state result;
			result.recording = m_recording.load(std::memory_order_acquire);
			result.speaking = m_speaking.load(std::memory_order_acquire);
			result.level = m_capture_level.load(std::memory_order_acquire);
			result.range = static_cast<protocol::VoiceRange>(
				m_capture_range.load(std::memory_order_acquire));
			return result;
		}

		std::vector<protocol::ClientVoiceFrame> poll_outbound()
		{
			std::scoped_lock lock(m_outbound_mutex);
			std::vector<protocol::ClientVoiceFrame> result;
			result.reserve(m_outbound.size());
			while (!m_outbound.empty())
			{
				result.push_back(std::move(m_outbound.front()));
				m_outbound.pop_front();
			}
			return result;
		}

		void receive(const protocol::ServerVoiceFrame &frame)
		{
			std::scoped_lock lock(m_inbound_mutex);
			if (m_inbound.size() >= receive_queue_limit)
				m_inbound.pop_front();
			m_inbound.push_back(frame);
		}

		void update_players(std::span<const voice_player_pose> players)
		{
			std::unordered_set<player_id> present;
			present.reserve(players.size());
			for (const auto &player : players)
			{
				present.insert(player.id);
				auto &pose = m_poses[player.id];
				pose = player;
			}
			for (auto iterator = m_poses.begin(); iterator != m_poses.end();)
				iterator = present.contains(iterator->first)
				    ? std::next(iterator) : m_poses.erase(iterator);
		}

		void tick()
		{
			if (m_clear_requested.exchange(false, std::memory_order_acq_rel))
				reset_game_thread();
			drain_inbound();
			(void)ensure_fmod();
			const auto now = clock::now();
			for (auto iterator = m_speakers.begin(); iterator != m_speakers.end();)
			{
				auto &speaker = *iterator->second;
				if (const auto pose = m_poses.find(speaker.id); pose != m_poses.end())
				{
					speaker.entity_id = pose->second.entity_id;
					const auto position = to_voice_audio_coordinates(
					    pose->second.position, head_height);
					const auto velocity = to_voice_audio_coordinates(
					    pose->second.velocity);
					speaker.position = {position.x, position.y, position.z};
					speaker.velocity = {velocity.x, velocity.y, velocity.z};
				}
				pump_speaker(speaker, now);
				if (now - speaker.last_packet > speaker_timeout)
				{
					stop_face(speaker);
					destroy_stream(speaker);
					if (speaker.decoder)
						opus_decoder_destroy(speaker.decoder);
					iterator = m_speakers.erase(iterator);
				}
				else
				{
					++iterator;
				}
			}
		}

		void reset()
		{
			m_active.store(false, std::memory_order_release);
			m_recording.store(false, std::memory_order_release);
			m_speaking.store(false, std::memory_order_release);
			m_capture_level.store(0.0F, std::memory_order_release);
			{
				std::scoped_lock lock(m_outbound_mutex);
				m_outbound.clear();
			}
			{
				std::scoped_lock lock(m_inbound_mutex);
				m_inbound.clear();
			}
			m_reset_requested.store(true, std::memory_order_release);
			m_clear_requested.store(true, std::memory_order_release);
		}

	private:
		struct speaker_state
		{
			player_id id{};
			std::uint32_t entity_id{};
			OpusDecoder *decoder{};
			std::map<std::uint32_t, queued_voice> frames;
			std::uint32_t expected_sequence{};
			bool primed{};
			bool talkspurt_ending{};
			pcm_ring pcm;
			fmod_sound *sound{};
			fmod_channel *channel{};
			bool playback_started{};
			fmod_vector position{};
			fmod_vector velocity{};
			protocol::VoiceRange range{protocol::VOICE_RANGE_NORMAL};
			clock::time_point last_packet{clock::now()};
			clock::time_point previous_arrival{};
			std::uint64_t previous_capture_time_ms{};
			double jitter_ms{};
			std::uint32_t jitter_frames{minimum_jitter_frames};
			clock::time_point last_decode{};
			clock::time_point last_voiced{};
			bool face_active{};
		};

		static fmod_result __stdcall pcm_read(
		    fmod_sound *sound,
		    void *data,
		    unsigned int bytes)
		{
			void *userdata{};
			if (!s_callback_api || !s_callback_api->sound_get_user_data
			    || s_callback_api->sound_get_user_data(sound, &userdata) != fmod_ok
			    || !userdata)
			{
				std::memset(data, 0, bytes);
				return fmod_ok;
			}
			auto &speaker = *static_cast<speaker_state *>(userdata);
			speaker.pcm.pop({
			    static_cast<std::int16_t *>(data),
			    bytes / sizeof(std::int16_t)});
			return fmod_ok;
		}

		[[nodiscard]] bool push_to_talk_pressed() const noexcept
		{
			return m_active.load(std::memory_order_acquire)
			    && (native_keybinds::voice_held()
			        || (GetAsyncKeyState('V') & 0x8000) != 0);
		}

		[[nodiscard]] static bool select_default_capture_device(
		    IMMDeviceEnumerator &enumerator,
		    ComPtr<IMMDevice> &device,
		    ERole &selected_role) noexcept
		{
			// Resolve the endpoint for every talkspurt. Windows can expose distinct
			// communications, console and multimedia defaults, and a previously
			// selected endpoint may have been disconnected or disabled meanwhile.
			for (const auto role : {eCommunications, eConsole, eMultimedia})
			{
				ComPtr<IMMDevice> candidate;
				if (FAILED(enumerator.GetDefaultAudioEndpoint(
				        eCapture,
				        role,
				        &candidate)))
					continue;
				DWORD state{};
				if (FAILED(candidate->GetState(&state))
				    || (state & DEVICE_STATE_ACTIVE) == 0)
					continue;
				device = std::move(candidate);
				selected_role = role;
				return true;
			}
			return false;
		}

		void queue_talkspurt_end(
		    std::uint32_t &sequence,
		    protocol::VoiceRange range)
		{
			protocol::ClientVoiceFrame end;
			end.set_sequence(sequence++);
			if (sequence == 0)
				sequence = 1;
			end.set_capture_time_ms(now_ms());
			end.set_range(range);
			end.set_visemes(std::string(voice_viseme_count, '\0'));
			end.set_end_of_talkspurt(true);
			queue_outbound(std::move(end));
		}

		void capture_loop(std::stop_token stop)
		{
			const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			if (FAILED(initialized))
				return;
			const auto uninitialize = std::unique_ptr<void, void(*)(void *)>(
			    reinterpret_cast<void *>(1), [](void *) { CoUninitialize(); });

			int opus_error{};
			auto *encoder = opus_encoder_create(
			    sample_rate, 1, OPUS_APPLICATION_VOIP, &opus_error);
			if (!encoder || opus_error != OPUS_OK)
				return;
			const auto destroy_encoder = std::unique_ptr<OpusEncoder, void(*)(OpusEncoder *)>(
			    encoder, opus_encoder_destroy);
			(void)opus_encoder_ctl(encoder, OPUS_SET_BITRATE(opus_bitrate));
			(void)opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
			(void)opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
			(void)opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));
			(void)opus_encoder_ctl(encoder, OPUS_SET_DTX(1));

			std::uint32_t sequence = 1;
			while (!stop.stop_requested())
			{
				if (m_reset_requested.exchange(false, std::memory_order_acq_rel))
					(void)opus_encoder_ctl(encoder, OPUS_RESET_STATE);
				if (!push_to_talk_pressed())
				{
					m_recording.store(false, std::memory_order_release);
					std::this_thread::sleep_for(20ms);
					continue;
				}

				ComPtr<IMMDeviceEnumerator> enumerator;
				ComPtr<IMMDevice> device;
				ComPtr<IAudioClient> client;
				ComPtr<IAudioCaptureClient> capture;
				ERole selected_role = eCommunications;
				if (FAILED(CoCreateInstance(
				        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				        IID_PPV_ARGS(&enumerator)))
				    || !select_default_capture_device(
				        *enumerator.Get(), device, selected_role)
				    || FAILED(device->Activate(
				        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
				{
					std::this_thread::sleep_for(100ms);
					continue;
				}

				WAVEFORMATEX *allocated_format{};
				if (FAILED(client->GetMixFormat(&allocated_format))
				    || !allocated_format)
				{
					std::this_thread::sleep_for(100ms);
					continue;
				}
				const auto free_format =
				    std::unique_ptr<WAVEFORMATEX, void(*)(WAVEFORMATEX *)>(
				        allocated_format,
				        [](WAVEFORMATEX *value) { CoTaskMemFree(value); });
				if ((!is_float_format(*allocated_format)
				        && !is_pcm_format(*allocated_format))
				    || allocated_format->nChannels == 0
				    || allocated_format->nSamplesPerSec == 0)
				{
					std::this_thread::sleep_for(100ms);
					continue;
				}

				const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				if (!event)
				{
					std::this_thread::sleep_for(100ms);
					continue;
				}
				const auto close_event = std::unique_ptr<void, void(*)(void *)>(
				    event, [](void *value) { CloseHandle(value); });
				if (FAILED(client->Initialize(
				        AUDCLNT_SHAREMODE_SHARED,
				        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
				        0,
				        0,
				        allocated_format,
				        nullptr))
				    || FAILED(client->SetEventHandle(event))
				    || FAILED(client->GetService(IID_PPV_ARGS(&capture)))
				    || FAILED(client->Start()))
				{
					std::this_thread::sleep_for(100ms);
					continue;
				}

				KCD2Online_JOIN_TRACE(
				    "voice.capture.endpoint-selected",
				    std::format(
				        "role={} channels={} sample_rate={}",
				        static_cast<int>(selected_role),
				        allocated_format->nChannels,
				        allocated_format->nSamplesPerSec));
				m_recording.store(true, std::memory_order_release);
				std::vector<std::int16_t> pending;
				pending.reserve(frame_samples * 3);
				std::uint64_t resample_phase{};
				bool talking{};
				bool capture_healthy{true};
				protocol::VoiceRange talk_range =
				    protocol::VOICE_RANGE_NORMAL;
				(void)opus_encoder_ctl(encoder, OPUS_RESET_STATE);

				while (!stop.stop_requested() && push_to_talk_pressed())
				{
					const auto wait_result = WaitForSingleObject(event, 50);
					if (wait_result != WAIT_OBJECT_0
					    && wait_result != WAIT_TIMEOUT)
					{
						capture_healthy = false;
						break;
					}
					if (m_reset_requested.exchange(
					        false, std::memory_order_acq_rel))
						break;
					if (!push_to_talk_pressed())
						break;

					UINT32 packet_frames{};
					while (true)
					{
						const auto available =
						    capture->GetNextPacketSize(&packet_frames);
						if (FAILED(available))
						{
							capture_healthy = false;
							break;
						}
						if (packet_frames == 0)
							break;
						BYTE *data{};
						DWORD flags{};
						UINT64 device_position{};
						UINT64 qpc_position{};
						if (FAILED(capture->GetBuffer(
						        &data,
						        &packet_frames,
						        &flags,
						        &device_position,
						        &qpc_position)))
						{
							capture_healthy = false;
							break;
						}
						for (UINT32 frame{}; frame < packet_frames; ++frame)
						{
							float mono{};
							if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0)
							{
								const auto *source =
								    reinterpret_cast<const std::byte *>(data)
								    + static_cast<std::size_t>(frame)
								        * allocated_format->nBlockAlign;
								for (std::uint16_t channel{};
								     channel < allocated_format->nChannels;
								     ++channel)
								{
									mono += read_input_sample(
									    source,
									    channel,
									    *allocated_format);
								}
								mono /= allocated_format->nChannels;
							}
							resample_phase += sample_rate;
							while (resample_phase
							    >= allocated_format->nSamplesPerSec)
							{
								resample_phase -=
								    allocated_format->nSamplesPerSec;
								pending.push_back(
								    static_cast<std::int16_t>(
								        std::clamp(mono, -1.0F, 1.0F)
								        * 32767.0F));
							}
						}
						if (FAILED(capture->ReleaseBuffer(packet_frames)))
						{
							capture_healthy = false;
							break;
						}
					}
					if (!capture_healthy)
						break;

					const bool control =
					    (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
					const bool shift =
					    (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
					talk_range = control ? protocol::VOICE_RANGE_WHISPER
					    : shift ? protocol::VOICE_RANGE_SHOUT
					            : protocol::VOICE_RANGE_NORMAL;
					m_capture_range.store(
					    static_cast<std::uint32_t>(talk_range),
					    std::memory_order_release);

					while (pending.size() >= frame_samples)
					{
						const auto pcm = std::span<const std::int16_t>{
						    pending.data(), frame_samples};
						const auto level = voice_level(pcm);
						m_capture_level.store(
						    level, std::memory_order_release);
						m_speaking.store(
						    level >= 0.08F, std::memory_order_release);
						std::array<unsigned char, max_voice_opus_bytes>
						    compressed{};
						const auto encoded = opus_encode(
						    encoder,
						    pending.data(),
						    frame_samples,
						    compressed.data(),
						    static_cast<opus_int32>(compressed.size()));
						if (encoded > 0)
						{
							protocol::ClientVoiceFrame frame;
							frame.set_sequence(sequence++);
							if (sequence == 0)
								sequence = 1;
							frame.set_capture_time_ms(now_ms());
							frame.set_range(talk_range);
							frame.set_opus(
							    compressed.data(), encoded);
							frame.set_visemes(visemes_for(pcm));
							queue_outbound(std::move(frame));
							talking = true;
						}
						pending.erase(
						    pending.begin(),
						    pending.begin() + frame_samples);
					}
				}

				(void)client->Stop();
				m_recording.store(false, std::memory_order_release);
				m_speaking.store(false, std::memory_order_release);
				m_capture_level.store(0.0F, std::memory_order_release);
				if (talking)
					queue_talkspurt_end(sequence, talk_range);
				(void)opus_encoder_ctl(encoder, OPUS_RESET_STATE);
				if (!capture_healthy && push_to_talk_pressed())
					std::this_thread::sleep_for(100ms);
			}
			m_recording.store(false, std::memory_order_release);
			m_speaking.store(false, std::memory_order_release);
			m_capture_level.store(0.0F, std::memory_order_release);
		}

		void queue_outbound(protocol::ClientVoiceFrame frame)
		{
			std::scoped_lock lock(m_outbound_mutex);
			while (m_outbound.size() >= outbound_queue_limit)
				m_outbound.pop_front();
			m_outbound.push_back(std::move(frame));
		}

		void drain_inbound()
		{
			std::deque<protocol::ServerVoiceFrame> inbound;
			{
				std::scoped_lock lock(m_inbound_mutex);
				inbound.swap(m_inbound);
			}
			for (auto &frame : inbound)
			{
				auto iterator = m_speakers.find(frame.player_id());
				if (iterator == m_speakers.end())
				{
					int error{};
					auto speaker = std::make_unique<speaker_state>();
					speaker->id = frame.player_id();
					speaker->decoder = opus_decoder_create(
					    sample_rate, 1, &error);
					if (!speaker->decoder || error != OPUS_OK)
						continue;
					iterator = m_speakers.emplace(
					    frame.player_id(), std::move(speaker)).first;
				}
				auto &speaker = *iterator->second;
				const auto arrival = clock::now();
				if (speaker.previous_arrival != clock::time_point{}
				    && frame.capture_time_ms() > speaker.previous_capture_time_ms)
				{
					const auto arrival_delta = std::chrono::duration<double, std::milli>(
					    arrival - speaker.previous_arrival).count();
					const auto capture_delta = static_cast<double>(
					    frame.capture_time_ms() - speaker.previous_capture_time_ms);
					const auto sample = std::abs(arrival_delta - capture_delta);
					speaker.jitter_ms += (sample - speaker.jitter_ms) * 0.1;
					speaker.jitter_frames = std::clamp(
					    minimum_jitter_frames
					        + static_cast<std::uint32_t>(
					            std::ceil(speaker.jitter_ms / 20.0)),
					    minimum_jitter_frames,
					    maximum_jitter_frames);
				}
				speaker.previous_arrival = arrival;
				speaker.previous_capture_time_ms = frame.capture_time_ms();
				speaker.last_packet = arrival;
				queued_voice queued{
				    frame.sequence(), frame.opus(), frame.visemes(), frame.range(),
				    frame.end_of_talkspurt(), clock::now()};
				speaker.frames.try_emplace(queued.sequence, std::move(queued));
				while (speaker.frames.size() > 24)
					speaker.frames.erase(speaker.frames.begin());
			}
		}

		bool ensure_fmod()
		{
			if (m_core && m_studio && m_api.system_create_sound)
				return true;
			if (!m_api.resolve())
				return false;
			static REL::Relocation<void **> wrapper_global{REL::ID(1257963)};
			auto *wrapper = *wrapper_global;
			if (!wrapper)
				return false;
			auto *bytes = static_cast<std::byte *>(wrapper);
			const auto init_state = *reinterpret_cast<std::uint32_t *>(bytes + 0xD8);
			if (init_state != 2)
				return false;
			m_studio = *reinterpret_cast<fmod_studio_system **>(bytes + 0xE0);
			m_core = *reinterpret_cast<fmod_system **>(bytes + 0xE8);
			if (!m_studio || !m_core)
				return false;

			fmod_studio_bus *bus{};
			if (m_api.studio_get_bus(m_studio, "bus:/dieg/w_obj", &bus) == fmod_ok
			    && bus && m_api.studio_bus_lock(bus) == fmod_ok)
			{
				(void)m_api.studio_flush(m_studio);
				if (m_api.studio_bus_get_group(bus, &m_group) == fmod_ok)
					m_bus = bus;
				else
					(void)m_api.studio_bus_unlock(bus);
			}
			s_callback_api = &m_api;
			return true;
		}

		bool create_stream(speaker_state &speaker)
		{
			if (speaker.sound && speaker.channel)
				return true;
			if (!ensure_fmod())
				return false;
			fmod_create_sound_ex_info info{};
			info.cbsize = sizeof(info);
			info.length = sample_rate * sizeof(std::int16_t);
			info.numchannels = 1;
			info.defaultfrequency = sample_rate;
			info.format = fmod_pcm16;
			info.decodebuffersize = frame_samples;
			info.pcmreadcallback = pcm_read;
			info.userdata = &speaker;
			const auto mode = fmod_open_user | fmod_create_stream
			    | fmod_loop_normal | fmod_3d;
			if (m_api.system_create_sound(
			        m_core, nullptr, mode, &info, &speaker.sound) != fmod_ok)
				return false;
			if (m_api.system_play_sound(
			        m_core, speaker.sound, m_group, 1, &speaker.channel) != fmod_ok)
			{
				m_api.sound_release(speaker.sound);
				speaker.sound = nullptr;
				return false;
			}
			(void)m_api.channel_set_mode(
			    speaker.channel, fmod_loop_normal | fmod_3d);
			(void)m_api.channel_set_3d_attributes(
			    speaker.channel, &speaker.position, &speaker.velocity);
			apply_range(speaker);
			return true;
		}

		void apply_range(speaker_state &speaker)
		{
			if (!speaker.channel)
				return;
			float maximum = 15.0F;
			float volume = 1.0F;
			if (speaker.range == protocol::VOICE_RANGE_WHISPER)
			{
				maximum = 3.0F;
				volume = 0.72F;
			}
			else if (speaker.range == protocol::VOICE_RANGE_SHOUT)
			{
				maximum = 40.0F;
				volume = 1.12F;
			}
			(void)m_api.channel_set_3d_min_max_distance(
			    speaker.channel, voice_min_distance, maximum);
			(void)m_api.channel_set_volume(speaker.channel, volume);
		}

		void pump_speaker(speaker_state &speaker, clock::time_point now)
		{
			const auto starvations = speaker.pcm.take_starvations();
			if (starvations != 0 && !speaker.talkspurt_ending)
			{
				if (speaker.channel && speaker.playback_started)
					(void)m_api.channel_set_paused(speaker.channel, 1);
				speaker.playback_started = false;
				speaker.primed = false;
				speaker.jitter_frames = std::min(
				    maximum_jitter_frames,
				    speaker.jitter_frames + 1);
				KCD2Online_JOIN_TRACE(
				    "voice.playback.rebuffer",
				    std::format(
				        "player_id={} starvations={} jitter_frames={}",
				        speaker.id,
				        starvations,
				        speaker.jitter_frames));
			}
			if (speaker.talkspurt_ending)
			{
				if (!speaker.playback_started)
				{
					if (speaker.channel && speaker.pcm.readable() != 0)
					{
						(void)m_api.channel_set_paused(speaker.channel, 0);
						speaker.playback_started = true;
					}
					else
					{
						speaker.pcm.clear();
						(void)speaker.pcm.take_starvations();
						speaker.talkspurt_ending = false;
					}
				}
				else if (speaker.pcm.readable() == 0)
				{
					if (speaker.channel)
						(void)m_api.channel_set_paused(speaker.channel, 1);
					speaker.playback_started = false;
					(void)speaker.pcm.take_starvations();
					speaker.talkspurt_ending = false;
				}
				if (speaker.talkspurt_ending)
					return;
			}
			if (!speaker.primed
			    && !speaker.frames.empty()
			    && speaker.frames.begin()->second.end)
			{
				speaker.expected_sequence = speaker.frames.begin()->first;
				speaker.primed = true;
			}
			else if (!speaker.primed
			    && speaker.frames.size() >= speaker.jitter_frames)
			{
				speaker.expected_sequence = speaker.frames.begin()->first;
				speaker.primed = true;
			}
			if (!speaker.primed)
				return;
			if (!m_poses.contains(speaker.id))
				return;
			if (!create_stream(speaker))
				return;

			std::array<std::int16_t, frame_samples> decoded{};
			for (int iterations{}; iterations < 8
			    && speaker.pcm.writable() >= decoded.size(); ++iterations)
			{
				auto found = speaker.frames.find(speaker.expected_sequence);
				if (found != speaker.frames.end() && found->second.end)
				{
					speaker.frames.erase(found);
					++speaker.expected_sequence;
					if (speaker.expected_sequence == 0)
						speaker.expected_sequence = 1;
					speaker.last_decode = now;
					speaker.last_voiced = {};
					speaker.primed = false;
					speaker.talkspurt_ending = true;
					stop_face(speaker);
					break;
				}
				const bool loss_due = found == speaker.frames.end()
				    && !speaker.frames.empty()
				    && (speaker.frames.begin()->first > speaker.expected_sequence)
				    && (speaker.last_decode == clock::time_point{}
				        || now - speaker.last_decode >= 20ms);
				if (found == speaker.frames.end() && !loss_due)
					break;
				decoded.fill(0);
				int samples{};
				if (found != speaker.frames.end())
				{
					speaker.range = found->second.range;
					samples = opus_decode(
					    speaker.decoder,
					    reinterpret_cast<const unsigned char *>(found->second.opus.data()),
					    static_cast<opus_int32>(found->second.opus.size()),
					    decoded.data(), frame_samples, 0);
					if (samples > 0)
					{
						speaker.last_voiced = now;
						start_face(speaker, found->second.visemes);
					}
					speaker.frames.erase(found);
					apply_range(speaker);
				}
				else
				{
					const auto next = speaker.frames.begin();
					const bool fec_available = !next->second.end
					    && !next->second.opus.empty()
					    && next->first == speaker.expected_sequence + 1;
					samples = fec_available
					    ? opus_decode(
					          speaker.decoder,
					          reinterpret_cast<const unsigned char *>(
					              next->second.opus.data()),
					          static_cast<opus_int32>(next->second.opus.size()),
					          decoded.data(),
					          frame_samples,
					          1)
					    : opus_decode(
					          speaker.decoder,
					          nullptr,
					          0,
					          decoded.data(),
					          frame_samples,
					          0);
				}
				if (samples < 0)
				{
					decoded.fill(0);
					samples = opus_decode(
					    speaker.decoder,
					    nullptr,
					    0,
					    decoded.data(),
					    frame_samples,
					    0);
					if (samples < 0)
					{
						decoded.fill(0);
						samples = frame_samples;
					}
				}
				if (samples < frame_samples)
					std::fill(decoded.begin() + samples, decoded.end(), 0);
				(void)speaker.pcm.push(decoded);
				++speaker.expected_sequence;
				if (speaker.expected_sequence == 0)
					speaker.expected_sequence = 1;
					speaker.last_decode = now;
			}

			(void)m_api.channel_set_3d_attributes(
			    speaker.channel, &speaker.position, &speaker.velocity);
			if (speaker.talkspurt_ending)
				return;
			if (!speaker.playback_started
			    && speaker.pcm.readable()
			        >= frame_samples * speaker.jitter_frames)
			{
				(void)m_api.channel_set_paused(speaker.channel, 0);
				speaker.playback_started = true;
			}
			if (speaker.face_active && speaker.last_voiced != clock::time_point{}
			    && now - speaker.last_voiced > talking_timeout)
				stop_face(speaker);
		}

		void start_face(speaker_state &speaker, std::string_view visemes)
		{
			if (speaker.face_active || speaker.entity_id == 0
			    || visemes.size() != voice_viseme_count)
				return;
			const auto peak = *std::max_element(visemes.begin() + 1, visemes.end(),
			    [](char left, char right)
			    {
				    return static_cast<unsigned char>(left)
				        < static_cast<unsigned char>(right);
			    });
			if (static_cast<unsigned char>(peak) < 20)
				return;
			const auto script = std::format(
			    "local e=System.GetEntity({}) if e then "
			    "e:EnableProceduralFacialAnimation(true) "
			    "e:PlayFacialAnimation('facial_chewing_01',true) end",
			    speaker.entity_id);
			if (execute_script(script))
				speaker.face_active = true;
		}

		void stop_face(speaker_state &speaker)
		{
			if (!speaker.face_active || speaker.entity_id == 0)
				return;
			// Replaying the same sequence as a one-shot replaces the looping
			// channel and lets KCD2 blend back to its procedural neutral face.
			const auto script = std::format(
			    "local e=System.GetEntity({}) if e then "
			    "e:PlayFacialAnimation('facial_chewing_01',false) end",
			    speaker.entity_id);
			(void)execute_script(script);
			speaker.face_active = false;
		}

		bool execute_script(std::string_view script)
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pScriptSystem)
				return false;
#ifdef _WIN32
			__try
			{
#endif
				return environment->pScriptSystem->ExecuteBuffer(
				    script.data(), script.size(), "KCD2Online VOIP viseme", nullptr);
#ifdef _WIN32
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
#endif
		}

		void destroy_stream(speaker_state &speaker)
		{
			if (speaker.channel)
				(void)m_api.channel_stop(speaker.channel);
			speaker.channel = nullptr;
			speaker.playback_started = false;
			if (speaker.sound)
				(void)m_api.sound_release(speaker.sound);
			speaker.sound = nullptr;
			speaker.pcm.clear();
		}

		void reset_game_thread()
		{
			for (auto &[id, speaker] : m_speakers)
			{
				(void)id;
				stop_face(*speaker);
				destroy_stream(*speaker);
				if (speaker->decoder)
					opus_decoder_destroy(speaker->decoder);
			}
			m_speakers.clear();
			m_poses.clear();
			if (m_bus && m_api.studio_bus_unlock)
				(void)m_api.studio_bus_unlock(m_bus);
			m_bus = nullptr;
			m_group = nullptr;
			m_core = nullptr;
			m_studio = nullptr;
			if (s_callback_api == &m_api)
				s_callback_api = nullptr;
		}

		std::atomic_bool m_active{};
		std::atomic_bool m_recording{};
		std::atomic_bool m_speaking{};
		std::atomic<float> m_capture_level{};
		std::atomic<std::uint32_t> m_capture_range{
			protocol::VOICE_RANGE_NORMAL};
		std::atomic_bool m_reset_requested{};
		std::atomic_bool m_clear_requested{};
		std::jthread m_capture;
		std::mutex m_outbound_mutex;
		std::deque<protocol::ClientVoiceFrame> m_outbound;
		std::mutex m_inbound_mutex;
		std::deque<protocol::ServerVoiceFrame> m_inbound;
		std::unordered_map<player_id, voice_player_pose> m_poses;
		std::unordered_map<player_id, std::unique_ptr<speaker_state>> m_speakers;
		fmod_api m_api;
		fmod_system *m_core{};
		fmod_studio_system *m_studio{};
		fmod_studio_bus *m_bus{};
		fmod_channel_group *m_group{};
		static inline fmod_api *s_callback_api{};
	};

	native_voice::native_voice() : m_impl(std::make_unique<implementation>()) {}
	native_voice::~native_voice() = default;

	void native_voice::set_active(bool active) noexcept
	{
		m_impl->set_active(active);
	}

	voice_capture_state native_voice::capture_state() const noexcept
	{
		return m_impl->capture_state();
	}

	std::vector<protocol::ClientVoiceFrame> native_voice::poll_outbound()
	{
		return m_impl->poll_outbound();
	}

	void native_voice::receive(const protocol::ServerVoiceFrame &frame)
	{
		m_impl->receive(frame);
	}

	void native_voice::update_players(std::span<const voice_player_pose> players)
	{
		m_impl->update_players(players);
	}

	void native_voice::tick()
	{
		m_impl->tick();
	}

	void native_voice::reset()
	{
		m_impl->reset();
	}
}
