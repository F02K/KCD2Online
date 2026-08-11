#include "gui/ingame_chat.hpp"

#include "gui/renderer.hpp"
#ifdef KCD2Online_NATIVE_MULTIPLAYER_MENU
	#include "gui/native_ui_localization.hpp"
#endif
#include "kcse/client_api.hpp"
#include "kcse/client_proxy.hpp"
#include "multiplayer/emote_catalog.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <string>
#include <string_view>
#include <vector>
#include <Windows.h>

namespace big::ingame_chat
{
	namespace
	{
		constexpr double message_hold_seconds      = 8.0;
		constexpr double message_fade_seconds      = 3.0;
		constexpr std::size_t closed_message_count = 5;
		constexpr std::size_t open_message_count   = 12;
		constexpr std::size_t maximum_input_bytes  = kcd2o::kcse::text_capacity - 1;

		std::atomic_bool g_open{};
		std::atomic_bool g_can_open{};
		std::atomic_bool g_focus_requested{};
		std::atomic_bool g_cancel_requested{};
		std::atomic_bool g_submit_requested{};
		std::atomic_bool g_enter_down{};
		std::atomic_bool g_emote_open{};
		std::atomic_bool g_emote_submit_requested{};
		std::atomic_bool g_emote_key_down{};
		std::atomic_int g_emote_selection{-1};
		std::atomic_bool g_native_bindings_active{};

		struct chat_view_state
		{
			std::string input;
			std::uint64_t input_generation{};
			std::size_t history_size{};
			kcd2o::player_id last_sender{};
			std::uint64_t last_server_time_ms{};
			std::string last_text;
			double last_activity_time{};
			bool scroll_to_bottom{};
		};

		chat_view_state &view_state()
		{
			static chat_view_state value;
			return value;
		}

		ImVec4 with_alpha(ImVec4 color, float alpha)
		{
			color.w *= alpha;
			return color;
		}

		void draw_wheel_segment(ImDrawList *draw, ImVec2 center, float radius, float inner_radius, float middle_angle, ImU32 fill, ImU32 outline, float outline_width)
		{
			constexpr float pi       = 3.14159265F;
			constexpr int arc_steps  = 16;
			const auto angle_padding = 0.035F;
			const auto start         = middle_angle - pi * 0.25F + angle_padding;
			const auto end           = middle_angle + pi * 0.25F - angle_padding;

			for (int step = 0; step < arc_steps; ++step)
			{
				const auto angle0 = start + (end - start) * static_cast<float>(step) / arc_steps;
				const auto angle1 = start + (end - start) * static_cast<float>(step + 1) / arc_steps;
				const ImVec2 points[] = {{center.x + std::cos(angle0) * inner_radius, center.y + std::sin(angle0) * inner_radius}, {center.x + std::cos(angle0) * radius, center.y + std::sin(angle0) * radius}, {center.x + std::cos(angle1) * radius, center.y + std::sin(angle1) * radius}, {center.x + std::cos(angle1) * inner_radius, center.y + std::sin(angle1) * inner_radius}};
				draw->AddConvexPolyFilled(points, 4, fill);
			}

			draw->PathArcTo(center, radius, start, end, arc_steps);
			draw->PathStroke(outline, 0, outline_width);
			for (const auto angle : {start, end})
			{
				draw->AddLine({center.x + std::cos(angle) * inner_radius, center.y + std::sin(angle) * inner_radius}, {center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius}, outline, outline_width);
			}
		}

		void trim_to_input_capacity(std::string &text)
		{
			if (text.size() <= maximum_input_bytes)
			{
				return;
			}

			std::size_t length = maximum_input_bytes;
			while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0U) == 0x80U)
			{
				--length;
			}
			text.resize(length);
		}

		bool history_changed(chat_view_state &state, const std::vector<kcd2o::chat_entry> &history)
		{
			const auto size_changed = state.history_size != history.size();
			const auto last_changed =
			    !history.empty()
			    && (state.last_sender != history.back().sender || state.last_server_time_ms != history.back().server_time_ms
			        || state.last_text != history.back().text);

			state.history_size = history.size();
			if (history.empty())
			{
				state.last_sender         = 0;
				state.last_server_time_ms = 0;
				state.last_text.clear();
			}
			else
			{
				state.last_sender         = history.back().sender;
				state.last_server_time_ms = history.back().server_time_ms;
				state.last_text           = history.back().text;
			}
			return size_changed || last_changed;
		}

		void close_chat(chat_view_state &state, bool clear_input)
		{
			const auto was_open = g_open.exchange(false, std::memory_order_acq_rel);
			g_focus_requested.store(false, std::memory_order_release);
			g_cancel_requested.store(false, std::memory_order_release);
			g_submit_requested.store(false, std::memory_order_release);
			if (clear_input && (was_open || !state.input.empty()))
			{
				state.input.clear();
				++state.input_generation;
			}
		}

		std::string chat_text(std::string_view key)
		{
#ifdef KCD2Online_NATIVE_MULTIPLAYER_MENU
			return ingame_ui::localized(key);
#else
			if (key == "chat.title")
			{
				return "MULTIPLAYER CHAT";
			}
			if (key == "chat.help")
			{
				return "Enter: send   Esc: close";
			}
			if (key == "emote.bow")
			{
				return "Bow";
			}
			if (key == "emote.cheer")
			{
				return "Cheer";
			}
			if (key == "emote.point")
			{
				return "Point";
			}
			if (key == "emote.surrender")
			{
				return "Surrender";
			}
			if (key == "emote.wheel.title")
			{
				return "CHOOSE EMOTE";
			}
			if (key == "emote.wheel.hint")
			{
				return "MOVE MOUSE  |  RELEASE TO PLAY";
			}
			if (key == "emote.wheel.cancel")
			{
				return "ESC TO CANCEL";
			}
			return "Write a message...";
#endif
		}

		void draw_message(const kcd2o::chat_entry &entry, kcd2o::player_id local_player, float alpha)
		{
			const auto server     = entry.sender == 0;
			const auto local      = entry.sender == local_player && local_player != 0;
			const auto ooc        = entry.channel == kcd2o::protocol::CHAT_CHANNEL_OOC;
			const auto quiet      = entry.channel == kcd2o::protocol::CHAT_CHANNEL_WHISPER;
			const auto loud       = entry.channel == kcd2o::protocol::CHAT_CHANNEL_SHOUT;
			const auto name_color = server ? ImVec4(0.88F, 0.65F, 0.27F, alpha) :
			                        ooc    ? ImVec4(0.58F, 0.72F, 0.82F, alpha) :
			                        quiet  ? ImVec4(0.62F, 0.74F, 0.58F, alpha) :
			                        loud   ? ImVec4(0.92F, 0.58F, 0.35F, alpha) :
			                        local  ? ImVec4(0.83F, 0.73F, 0.48F, alpha) :
			                                 ImVec4(0.72F, 0.68F, 0.58F, alpha);
			const auto text_color = ImVec4(0.93F, 0.90F, 0.82F, alpha);
			const char *staff_badge = "";
			auto staff_color = ImVec4(0.72F, 0.68F, 0.58F, alpha);
			switch (entry.network_role)
			{
			case kcd2o::protocol::NETWORK_ROLE_OWNER:
				staff_badge = "[OWNER] ";
				staff_color = ImVec4(1.00F, 0.76F, 0.22F, alpha);
				break;
			case kcd2o::protocol::NETWORK_ROLE_ADMIN:
				staff_badge = "[ADMIN] ";
				staff_color = ImVec4(0.96F, 0.34F, 0.25F, alpha);
				break;
			case kcd2o::protocol::NETWORK_ROLE_MODERATOR:
				staff_badge = "[MOD] ";
				staff_color = ImVec4(0.34F, 0.70F, 0.98F, alpha);
				break;
			case kcd2o::protocol::NETWORK_ROLE_SUPPORTER:
				staff_badge = "[SUPPORT] ";
				staff_color = ImVec4(0.31F, 0.85F, 0.53F, alpha);
				break;
			default: break;
			}
			const char *prefix    = "";
			switch (entry.channel)
			{
			case kcd2o::protocol::CHAT_CHANNEL_WHISPER:      prefix = "[Fluestern] "; break;
			case kcd2o::protocol::CHAT_CHANNEL_SHOUT:        prefix = "[Rufen] "; break;
			case kcd2o::protocol::CHAT_CHANNEL_OOC:          prefix = "[OOC] "; break;
			case kcd2o::protocol::CHAT_CHANNEL_ANNOUNCEMENT: prefix = "[Ankuendigung] "; break;
			case kcd2o::protocol::CHAT_CHANNEL_ADMIN:        prefix = "[GM] "; break;
			default:                                         break;
			}

			if (*staff_badge != '\0')
			{
				ImGui::PushStyleColor(ImGuiCol_Text, staff_color);
				ImGui::TextUnformatted(staff_badge);
				ImGui::PopStyleColor();
				ImGui::SameLine(0.0F, 0.0F);
			}
			ImGui::PushStyleColor(ImGuiCol_Text, name_color);
			ImGui::TextUnformatted(prefix);
			ImGui::SameLine(0.0F, 0.0F);
			if (entry.channel == kcd2o::protocol::CHAT_CHANNEL_EMOTE)
			{
				ImGui::TextUnformatted("* ");
			}
			ImGui::SameLine(0.0F, 0.0F);
			ImGui::TextUnformatted(entry.display_name.c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine(0.0F, 0.0F);
			ImGui::PushStyleColor(ImGuiCol_Text, text_color);
			const auto roleplay_action = entry.channel == kcd2o::protocol::CHAT_CHANNEL_EMOTE || entry.channel == kcd2o::protocol::CHAT_CHANNEL_SCENE;
			ImGui::TextWrapped(roleplay_action ? " %s" : ": %s", entry.text.c_str());
			ImGui::PopStyleColor();
		}

		void draw_emote_wheel(kcd2o::kcse::ui_client_proxy &client)
		{
			constexpr float pi   = 3.14159265F;
			const auto *viewport = ImGui::GetMainViewport();
			const auto center =
			    ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5F, viewport->WorkPos.y + viewport->WorkSize.y * 0.5F);
			const auto scale     = std::clamp(viewport->WorkSize.y / 1080.0F, 0.78F, 1.25F);
			const auto radius    = 156.0F * scale;
			const auto dead_zone = 49.0F * scale;
			const auto mouse     = ImGui::GetIO().MousePos;
			const auto mouse_valid = ImGui::IsMousePosValid(&mouse);
			const auto dx        = mouse_valid ? mouse.x - center.x : 0.0F;
			const auto dy        = mouse_valid ? mouse.y - center.y : 0.0F;
			const auto distance  = std::sqrt(dx * dx + dy * dy);
			const auto pointer_distance = std::min(
			    distance, radius - 10.0F * scale);
			const auto direction = distance > 0.001F
			    ? ImVec2(dx / distance, dy / distance)
			    : ImVec2(0.0F, 0.0F);
			const auto pointer = ImVec2(
			    center.x + direction.x * pointer_distance,
			    center.y + direction.y * pointer_distance);
			int selected         = -1;
			if (pointer_distance >= dead_zone)
			{
				if (std::abs(dx) >= std::abs(dy))
				{
					selected = dx < 0.0F ? 0 : 1;
				}
				else
				{
					selected = dy < 0.0F ? 2 : 3;
				}
			}
			g_emote_selection.store(selected, std::memory_order_release);

			auto *draw = ImGui::GetForegroundDrawList();
			draw->AddRectFilled(viewport->WorkPos,
			                    {viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y},
			                    IM_COL32(7, 6, 5, 68));
			draw->AddCircleFilled(center, radius + 13.0F * scale, IM_COL32(5, 4, 3, 105), 64);
			draw->AddCircleFilled(center, radius + 4.0F * scale, IM_COL32(21, 17, 12, 238), 64);

			const float angles[]     = {pi, 0.0F, -pi * 0.5F, pi * 0.5F};
			const ImVec2 positions[] = {{center.x - radius * 0.66F, center.y},
			                            {center.x + radius * 0.66F, center.y},
			                            {center.x, center.y - radius * 0.66F},
			                            {center.x, center.y + radius * 0.66F}};

			for (std::size_t index = 0; index < kcd2o::emote_catalog.size(); ++index)
			{
				const auto active = selected == static_cast<int>(index);
				if (active)
				{
					draw_wheel_segment(draw, center, radius + 9.0F * scale, dead_zone, angles[index], IM_COL32(117, 76, 28, 78), IM_COL32(189, 132, 54, 90), 2.0F * scale);
				}
				draw_wheel_segment(draw, center, active ? radius + 4.0F * scale : radius, dead_zone, angles[index], active ? IM_COL32(151, 101, 38, 248) : IM_COL32(48, 39, 28, 238), active ? IM_COL32(225, 172, 87, 245) : IM_COL32(107, 87, 58, 205), (active ? 2.2F : 1.2F) * scale);
			}

			draw->AddCircleFilled(center, dead_zone - 2.0F * scale, IM_COL32(15, 12, 9, 252), 48);
			draw->AddCircle(center, dead_zone, IM_COL32(190, 139, 65, 235), 48, 2.0F * scale);
			draw->AddCircle(center, dead_zone - 7.0F * scale, IM_COL32(76, 59, 38, 220), 48, 1.0F * scale);
			if (distance > 0.001F)
			{
				draw->AddLine(
				    center,
				    pointer,
				    selected >= 0 ? IM_COL32(218, 166, 81, 150)
				                  : IM_COL32(133, 118, 88, 115),
				    1.4F * scale);
			}
			draw->AddCircleFilled(
			    pointer,
			    5.0F * scale,
			    selected >= 0 ? IM_COL32(250, 210, 126, 255)
			                  : IM_COL32(190, 177, 145, 235),
			    20);
			draw->AddCircle(
			    pointer,
			    7.5F * scale,
			    selected >= 0 ? IM_COL32(59, 38, 16, 225)
			                  : IM_COL32(42, 35, 25, 190),
			    20,
			    1.4F * scale);

			for (std::size_t index = 0; index < kcd2o::emote_catalog.size(); ++index)
			{
				const auto active = selected == static_cast<int>(index);
				const auto label  = chat_text(kcd2o::emote_catalog[index].label);
				const auto size   = ImGui::CalcTextSize(label.c_str());
				const auto label_position = ImVec2(positions[index].x - size.x * 0.5F, positions[index].y - size.y * 0.5F);
				if (active)
				{
					draw->AddText({label_position.x + 1.0F * scale, label_position.y + 2.0F * scale},
					              IM_COL32(25, 16, 7, 190),
					              label.c_str());
				}
				draw->AddText(label_position,
				              active ? IM_COL32(255, 238, 196, 255) : IM_COL32(208, 196, 168, 245),
				              label.c_str());
			}

			const auto title      = chat_text("emote.wheel.title");
			const auto title_size = ImGui::CalcTextSize(title.c_str());
			draw->AddText({center.x - title_size.x * 0.5F, center.y - radius - 39.0F * scale},
			              IM_COL32(231, 214, 178, 245),
			              title.c_str());

			const auto hint      = chat_text("emote.wheel.hint");
			const auto hint_size = ImGui::CalcTextSize(hint.c_str());
			draw->AddText({center.x - hint_size.x * 0.5F, center.y + radius + 24.0F * scale}, IM_COL32(207, 196, 169, 235), hint.c_str());
			const auto cancel      = chat_text("emote.wheel.cancel");
			const auto cancel_size = ImGui::CalcTextSize(cancel.c_str());
			draw->AddText({center.x - cancel_size.x * 0.5F, center.y + radius + 44.0F * scale},
			              IM_COL32(142, 132, 112, 220),
			              cancel.c_str());

			if (g_emote_submit_requested.exchange(false, std::memory_order_acq_rel))
			{
				const auto choice = g_emote_selection.exchange(-1, std::memory_order_acq_rel);
				if (choice >= 0 && static_cast<std::size_t>(choice) < kcd2o::emote_catalog.size())
				{
					(void)client.play_emote(kcd2o::emote_catalog[choice].kind);
				}
			}
		}

		float closed_alpha(const chat_view_state &state, double now)
		{
			const auto age = now - state.last_activity_time;
			if (age <= message_hold_seconds)
			{
				return 1.0F;
			}
			return std::clamp(static_cast<float>(1.0 - (age - message_hold_seconds) / message_fade_seconds), 0.0F, 1.0F);
		}

		void draw_voice_indicator(const kcd2o::client_status &status)
		{
			static float displayed_level{};
			const auto target    = status.voice_recording ? status.voice_level : 0.0F;
			const auto response  = target > displayed_level ? 0.38F : 0.16F;
			displayed_level     += (target - displayed_level) * response;
			if (!status.voice_recording && displayed_level < 0.01F)
			{
				displayed_level = 0.0F;
				return;
			}

			const auto *viewport = ImGui::GetMainViewport();
			const auto scale     = std::clamp(viewport->WorkSize.y / 1080.0F, 0.78F, 1.25F);
			const auto size      = ImVec2(68.0F * scale, 28.0F * scale);
			const auto center =
			    ImVec2(viewport->WorkPos.x + 38.0F * scale + size.x * 0.5F, viewport->WorkPos.y + viewport->WorkSize.y - 46.0F * scale);
			const auto minimum = ImVec2(center.x - size.x * 0.5F, center.y - size.y * 0.5F);
			const auto maximum = ImVec2(minimum.x + size.x, minimum.y + size.y);
			auto *draw         = ImGui::GetForegroundDrawList();

			ImVec4 tint = ImVec4(0.76F, 0.61F, 0.34F, 1.0F);
			if (status.voice_range == kcd2o::protocol::VOICE_RANGE_WHISPER)
			{
				tint = ImVec4(0.55F, 0.68F, 0.52F, 1.0F);
			}
			else if (status.voice_range == kcd2o::protocol::VOICE_RANGE_SHOUT)
			{
				tint = ImVec4(0.86F, 0.48F, 0.27F, 1.0F);
			}
			const auto active     = status.voice_speaking || displayed_level >= 0.08F;
			const auto foreground = ImGui::ColorConvertFloat4ToU32(with_alpha(tint, active ? 0.92F : 0.60F));
			const auto quiet      = ImGui::ColorConvertFloat4ToU32(with_alpha(tint, active ? 0.38F : 0.18F));

			draw->AddRectFilled(minimum, maximum, IM_COL32(18, 15, 12, active ? 154 : 118), 14.0F * scale);
			draw->AddRect(minimum, maximum, quiet, 14.0F * scale, 0, 1.0F * scale);

			const auto microphone = ImVec2(minimum.x + 21.0F * scale, center.y - 1.0F * scale);
			draw->AddRectFilled({microphone.x - 3.0F * scale, microphone.y - 7.0F * scale},
			                    {microphone.x + 3.0F * scale, microphone.y + 3.0F * scale},
			                    foreground,
			                    3.0F * scale);
			draw->PathArcTo({microphone.x, microphone.y + 1.0F * scale}, 7.0F * scale, 0.0F, 3.14159265F, 10);
			draw->PathStroke(foreground, 0, 1.4F * scale);
			draw->AddLine({microphone.x, microphone.y + 8.0F * scale}, {microphone.x, microphone.y + 11.0F * scale}, foreground, 1.4F * scale);
			draw->AddLine({microphone.x - 4.0F * scale, microphone.y + 11.0F * scale},
			              {microphone.x + 4.0F * scale, microphone.y + 11.0F * scale},
			              foreground,
			              1.4F * scale);

			const auto phase = static_cast<float>(ImGui::GetTime() * 4.0);
			const float factors[] = {0.58F + 0.12F * std::sin(phase), 0.90F + 0.10F * std::sin(phase + 1.7F), 0.70F + 0.14F * std::sin(phase + 3.1F)};
			for (std::size_t index{}; index < 3; ++index)
			{
				const auto height = (2.0F + 13.0F * displayed_level * factors[index]) * scale;
				const auto x      = minimum.x + (40.0F + 7.0F * static_cast<float>(index)) * scale;
				draw->AddRectFilled({x, center.y - height * 0.5F}, {x + 3.0F * scale, center.y + height * 0.5F}, active ? foreground : quiet, 1.5F * scale);
			}
		}
	} // namespace

	void render(bool mod_gui_open)
	{
		auto &client         = kcd2o::kcse::ui_client();
		const auto status    = client.status();
		const auto connected = status.state == kcd2o::client_state::connected;
		static std::uint32_t chat_action_generation{};
		static bool emote_action_held{};
		g_native_bindings_active.store(status.native_keybinds, std::memory_order_release);
		g_can_open.store(connected && !mod_gui_open, std::memory_order_release);

		auto &state = view_state();
		if (!connected || mod_gui_open)
		{
			chat_action_generation = status.chat_action_generation;
			emote_action_held      = status.emote_action_held;
			close_chat(state, mod_gui_open || !connected);
			g_emote_open.store(false, std::memory_order_release);
			g_emote_submit_requested.store(false, std::memory_order_release);
			if (!connected)
			{
				state.history_size        = 0;
				state.last_sender         = 0;
				state.last_server_time_ms = 0;
				state.last_text.clear();
				state.last_activity_time = 0.0;
			}
			return;
		}

		if (g_cancel_requested.exchange(false, std::memory_order_acq_rel))
		{
			close_chat(state, true);
		}

		const auto history = client.chat_history();
		const auto now     = ImGui::GetTime();
		if (status.native_keybinds)
		{
			if (status.chat_action_generation != chat_action_generation)
			{
				chat_action_generation = status.chat_action_generation;
				if (!g_open.load(std::memory_order_acquire))
				{
					g_open.store(true, std::memory_order_release);
					g_focus_requested.store(true, std::memory_order_release);
				}
			}
			if (status.emote_action_held != emote_action_held)
			{
				emote_action_held = status.emote_action_held;
				if (emote_action_held && !g_open.load(std::memory_order_acquire))
				{
					g_emote_open.store(true, std::memory_order_release);
				}
				else if (g_emote_open.exchange(false, std::memory_order_acq_rel))
				{
					g_emote_submit_requested.store(true, std::memory_order_release);
				}
			}
		}
		if (history_changed(state, history))
		{
			state.last_activity_time = now;
			state.scroll_to_bottom   = true;
		}

		const auto emote_open = g_emote_open.load(std::memory_order_acquire);
		if (emote_open || g_emote_submit_requested.load(std::memory_order_acquire))
		{
			draw_emote_wheel(client);
		}

		draw_voice_indicator(status);

		const auto open = g_open.load(std::memory_order_acquire);
		if (!open && history.empty())
		{
			return;
		}

		const auto alpha = open ? 1.0F : closed_alpha(state, now);
		if (alpha <= 0.01F)
		{
			return;
		}

		const auto *viewport        = ImGui::GetMainViewport();
		const auto scale            = std::clamp(viewport->WorkSize.y / 1080.0F, 0.78F, 1.25F);
		const auto width            = 560.0F * scale;
		const auto height           = 286.0F * scale;
		const auto bottom_clearance = 142.0F * scale;
		const auto position = ImVec2(viewport->WorkPos.x + 38.0F * scale, viewport->WorkPos.y + viewport->WorkSize.y - bottom_clearance);

		ImGui::SetNextWindowPos(position, ImGuiCond_Always, {0.0F, 1.0F});
		ImGui::SetNextWindowSize({width, open ? height : 0.0F}, ImGuiCond_Always);

		const auto background    = with_alpha(ImVec4(0.050F, 0.041F, 0.031F, open ? 0.91F : 0.72F), alpha);
		const auto border        = with_alpha(ImVec4(0.53F, 0.40F, 0.22F, open ? 0.76F : 0.45F), alpha);
		const auto frame         = with_alpha(ImVec4(0.095F, 0.077F, 0.056F, 0.96F), alpha);
		const auto frame_hovered = with_alpha(ImVec4(0.13F, 0.105F, 0.074F, 0.98F), alpha);
		const auto selection     = with_alpha(ImVec4(0.52F, 0.37F, 0.17F, 0.55F), alpha);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {15.0F * scale, 12.0F * scale});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {7.0F * scale, 4.0F * scale});
		ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
		ImGui::PushStyleColor(ImGuiCol_Border, border);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, frame);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frame_hovered);
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frame_hovered);
		ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, selection);

		auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
		if (!open)
		{
			flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
		}

		if (ImGui::Begin("##KCD2OnlineIngameChat", nullptr, flags))
		{
			const auto window_position = ImGui::GetWindowPos();
			const auto accent_color = ImGui::ColorConvertFloat4ToU32(with_alpha(ImVec4(0.66F, 0.47F, 0.22F, 0.88F), alpha));
			ImGui::GetWindowDrawList()->AddRectFilled(window_position, {window_position.x + width, window_position.y + 2.0F * scale}, accent_color);

			if (g_renderer && g_renderer->font_small)
			{
				ImGui::PushFont(g_renderer->font_small);
			}

			if (open)
			{
				const auto title = chat_text("chat.title");
				const auto help  = chat_text("chat.help");
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78F, 0.64F, 0.38F, alpha));
				ImGui::TextUnformatted(title.c_str());
				ImGui::PopStyleColor();
				ImGui::SameLine();
				const auto help_width = ImGui::CalcTextSize(help.c_str()).x;
				ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), width - help_width - 17.0F * scale));
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.54F, 0.51F, 0.44F, alpha));
				ImGui::TextUnformatted(help.c_str());
				ImGui::PopStyleColor();
				ImGui::Separator();
			}

			const auto count = open ? open_message_count : closed_message_count;
			const auto first = history.size() > count ? history.size() - count : 0;
			if (open)
			{
				if (ImGui::BeginChild("##KCD2OnlineChatHistory", {0.0F, 182.0F * scale}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysVerticalScrollbar))
				{
					for (std::size_t index = first; index < history.size(); ++index)
					{
						draw_message(history[index], status.local_player_id, alpha);
					}

					if (state.scroll_to_bottom)
					{
						ImGui::SetScrollHereY(1.0F);
						state.scroll_to_bottom = false;
					}
				}
				ImGui::EndChild();
			}
			else
			{
				for (std::size_t index = first; index < history.size(); ++index)
				{
					draw_message(history[index], status.local_player_id, alpha);
				}
			}

			if (open)
			{
				ImGui::Separator();
				if (g_focus_requested.exchange(false, std::memory_order_acq_rel))
				{
					ImGui::SetKeyboardFocusHere();
				}
				ImGui::SetNextItemWidth(-1.0F);
				const auto input_id   = "##KCD2OnlineChatInput" + std::to_string(state.input_generation);
				const auto input_hint = chat_text("chat.input_hint");
				ImGui::InputTextWithHint(input_id.c_str(), input_hint.c_str(), &state.input);
				trim_to_input_capacity(state.input);

				if (g_submit_requested.exchange(false, std::memory_order_acq_rel))
				{
					if (!state.input.empty() && client.send_chat(state.input))
					{
						state.last_activity_time = now;
						state.input.clear();
						++state.input_generation;
						g_focus_requested.store(true, std::memory_order_release);
					}
					else if (!state.input.empty())
					{
						g_focus_requested.store(true, std::memory_order_release);
					}
				}
			}

			if (g_renderer && g_renderer->font_small)
			{
				ImGui::PopFont();
			}
		}
		ImGui::End();

		ImGui::PopStyleColor(6);
		ImGui::PopStyleVar(6);
	}

	void on_window_message(std::uint32_t message, std::uintptr_t wparam) noexcept
	{
		if (message == WM_KILLFOCUS)
		{
			g_enter_down.store(false, std::memory_order_release);
			g_emote_key_down.store(false, std::memory_order_release);
			g_emote_open.store(false, std::memory_order_release);
			g_emote_submit_requested.store(false, std::memory_order_release);
			if (g_open.load(std::memory_order_acquire))
			{
				g_cancel_requested.store(true, std::memory_order_release);
			}
			return;
		}

		if (message == WM_KEYDOWN && wparam == VK_ESCAPE && g_open.load(std::memory_order_acquire))
		{
			g_cancel_requested.store(true, std::memory_order_release);
			return;
		}
		if (message == WM_KEYDOWN && wparam == VK_ESCAPE && g_emote_open.exchange(false, std::memory_order_acq_rel))
		{
			g_emote_selection.store(-1, std::memory_order_release);
			return;
		}

		if (!g_native_bindings_active.load(std::memory_order_acquire) && wparam == 'G')
		{
			if (message == WM_KEYUP)
			{
				g_emote_key_down.store(false, std::memory_order_release);
				if (g_emote_open.exchange(false, std::memory_order_acq_rel))
				{
					g_emote_submit_requested.store(true, std::memory_order_release);
				}
				return;
			}
			if (message == WM_KEYDOWN && !g_emote_key_down.exchange(true, std::memory_order_acq_rel) && g_can_open.load(std::memory_order_acquire)
			    && !g_open.load(std::memory_order_acquire))
			{
				g_emote_open.store(true, std::memory_order_release);
				return;
			}
		}

		if (wparam != VK_RETURN)
		{
			return;
		}

		if (message == WM_KEYUP)
		{
			g_enter_down.store(false, std::memory_order_release);
			return;
		}
		if (message != WM_KEYDOWN || g_enter_down.exchange(true, std::memory_order_acq_rel))
		{
			return;
		}

		if (g_open.load(std::memory_order_acquire))
		{
			g_submit_requested.store(true, std::memory_order_release);
		}
		else if (!g_native_bindings_active.load(std::memory_order_acquire) && g_can_open.load(std::memory_order_acquire))
		{
			g_open.store(true, std::memory_order_release);
			g_focus_requested.store(true, std::memory_order_release);
		}
	}

	bool blocks_game_input() noexcept
	{
		return g_open.load(std::memory_order_acquire) || g_cancel_requested.load(std::memory_order_acquire)
		       || g_submit_requested.load(std::memory_order_acquire) || g_emote_open.load(std::memory_order_acquire)
		       || g_emote_submit_requested.load(std::memory_order_acquire);
	}

	bool allows_blocked_input(std::uint32_t input_state) noexcept
	{
		constexpr std::uint32_t released = 1U << 1;
		return g_native_bindings_active.load(std::memory_order_acquire)
		       && g_emote_open.load(std::memory_order_acquire)
		       && (input_state & released) != 0;
	}
} // namespace big::ingame_chat
