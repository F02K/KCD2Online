#include "gui/ingame_social_panel.hpp"

#include "gui/gui.hpp"
#include "gui/ingame_social_preferences.hpp"
#include "gui/renderer.hpp"
#ifdef KCD2Online_NATIVE_MULTIPLAYER_MENU
	#include "gui/native_ui_localization.hpp"
#endif
#include "kcse/client_proxy.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <shellapi.h>
#include <string>
#include <string_view>
#include <vector>
#include <Windows.h>

namespace big::ingame_social_panel
{
	namespace
	{
		constexpr std::string_view default_support_url = "https://support.kingdom-online.cc";

		std::atomic_bool g_open{};
		std::atomic_bool g_can_open{};
		std::atomic_bool g_f3_down{};
		std::atomic_bool g_native_bindings_active{};
		std::atomic<std::uint64_t> g_suppress_native_until_ms{};

		struct panel_state
		{
			kcd2o::player_id selected_player{};
			std::string search;
			std::string feedback;
			double feedback_until{};
		};

		panel_state &state()
		{
			static panel_state value;
			return value;
		}

		void set_open(bool open) noexcept
		{
			if (g_open.exchange(open, std::memory_order_acq_rel) == open)
			{
				return;
			}
			if (g_gui)
			{
				g_gui->sync_mouse_capture();
			}
		}

		std::string text(std::string_view key)
		{
#ifdef KCD2Online_NATIVE_MULTIPLAYER_MENU
			return ingame_ui::localized(key);
#else
			if (key == "social.title")
			{
				return "SOCIAL";
			}
			if (key == "social.close_hint")
			{
				return "F3 / Esc: close";
			}
			if (key == "social.players")
			{
				return "PLAYERS";
			}
			if (key == "social.search")
			{
				return "Search player...";
			}
			if (key == "social.no_players")
			{
				return "No matching players.";
			}
			if (key == "social.you")
			{
				return "YOU";
			}
			if (key == "social.status.online")
			{
				return "ONLINE";
			}
			if (key == "social.status.reconnecting")
			{
				return "RECONNECTING";
			}
			if (key == "social.select_player")
			{
				return "Select a player.";
			}
			if (key == "social.profile")
			{
				return "PROFILE";
			}
			if (key == "social.player_id")
			{
				return "PLAYER ID";
			}
			if (key == "social.rp_id")
			{
				return "RP ID";
			}
			if (key == "social.role")
			{
				return "ROLE";
			}
			if (key == "social.role.owner")
			{
				return "OWNER";
			}
			if (key == "social.role.admin")
			{
				return "ADMIN";
			}
			if (key == "social.role.moderator")
			{
				return "MODERATOR";
			}
			if (key == "social.role.supporter")
			{
				return "SUPPORT";
			}
			if (key == "social.role.user")
			{
				return "PLAYER";
			}
			if (key == "social.local_controls")
			{
				return "LOCAL CONTROLS";
			}
			if (key == "social.local_hint")
			{
				return "These settings affect only your client and do not notify the player.";
			}
			if (key == "social.hide_chat")
			{
				return "Hide this player's chat";
			}
			if (key == "social.voice_volume")
			{
				return "Voice volume";
			}
			if (key == "social.voice_mute")
			{
				return "MUTE VOICE";
			}
			if (key == "social.voice_unmute")
			{
				return "UNMUTE VOICE";
			}
			if (key == "social.voice_failed")
			{
				return "Voice preference could not be applied.";
			}
			if (key == "social.report")
			{
				return "REPORT & SUPPORT";
			}
			if (key == "social.report_hint")
			{
				return "Copy the verified reference before opening support.";
			}
			if (key == "social.copy_rp_id")
			{
				return "COPY RP ID";
			}
			if (key == "social.copy_report")
			{
				return "COPY REPORT REFERENCE";
			}
			if (key == "social.open_support")
			{
				return "OPEN SUPPORT";
			}
			if (key == "social.copied_rp_id")
			{
				return "RP ID copied.";
			}
			if (key == "social.copied_report")
			{
				return "Report reference copied.";
			}
			return std::string(key);
#endif
		}

		std::string lower_ascii(std::string_view value)
		{
			std::string result(value);
			std::ranges::transform(result,
			                       result.begin(),
			                       [](unsigned char character)
			                       {
				                       return static_cast<char>(std::tolower(character));
			                       });
			return result;
		}

		bool matches_search(const kcd2o::remote_player_view &player, std::string_view search)
		{
			if (search.empty())
			{
				return true;
			}
			const auto needle = lower_ascii(search);
			return lower_ascii(player.display_name).contains(needle) || lower_ascii(player.persistent_id).contains(needle)
			       || std::to_string(player.id).contains(needle);
		}

		ImVec4 role_color(kcd2o::protocol::NetworkRole role)
		{
			switch (role)
			{
			case kcd2o::protocol::NETWORK_ROLE_OWNER:     return {0.89F, 0.47F, 0.19F, 1.0F};
			case kcd2o::protocol::NETWORK_ROLE_ADMIN:     return {0.85F, 0.25F, 0.20F, 1.0F};
			case kcd2o::protocol::NETWORK_ROLE_MODERATOR: return {0.34F, 0.67F, 0.88F, 1.0F};
			case kcd2o::protocol::NETWORK_ROLE_SUPPORTER: return {0.49F, 0.76F, 0.42F, 1.0F};
			default:                                      return {0.69F, 0.66F, 0.58F, 1.0F};
			}
		}

		std::string role_name(kcd2o::protocol::NetworkRole role)
		{
			switch (role)
			{
			case kcd2o::protocol::NETWORK_ROLE_OWNER:     return text("social.role.owner");
			case kcd2o::protocol::NETWORK_ROLE_ADMIN:     return text("social.role.admin");
			case kcd2o::protocol::NETWORK_ROLE_MODERATOR: return text("social.role.moderator");
			case kcd2o::protocol::NETWORK_ROLE_SUPPORTER: return text("social.role.supporter");
			default:                                      return text("social.role.user");
			}
		}

		void section_title(std::string_view title)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, {0.86F, 0.72F, 0.45F, 1.0F});
			ImGui::TextUnformatted(title.data(), title.data() + title.size());
			ImGui::PopStyleColor();
			ImGui::Separator();
		}

		void label_value(std::string_view label, std::string_view value)
		{
			ImGui::TextDisabled("%.*s", static_cast<int>(label.size()), label.data());
			ImGui::SameLine();
			ImGui::TextWrapped("%.*s", static_cast<int>(value.size()), value.data());
		}

		void set_feedback(panel_state &value, std::string message)
		{
			value.feedback       = std::move(message);
			value.feedback_until = ImGui::GetTime() + 3.0;
		}

		void draw_player_list(panel_state &value, const std::vector<kcd2o::remote_player_view> &players, kcd2o::player_id local_player, float width, float scale)
		{
			ImGui::BeginChild("##SocialPlayers", {width, 0.0F}, ImGuiChildFlags_Border);
			section_title(std::format("{} ({})", text("social.players"), players.size()));
			ImGui::SetNextItemWidth(-1.0F);
			ImGui::InputTextWithHint("##SocialSearch", text("social.search").c_str(), &value.search);
			ImGui::Separator();

			bool found{};
			for (const auto &player : players)
			{
				if (!matches_search(player, value.search))
				{
					continue;
				}
				found = true;
				ImGui::PushID(static_cast<int>(player.id));
				const auto selected = value.selected_player == player.id;
				const auto status   = player.id == local_player ? text("social.you") :
				                      player.connected          ? text("social.status.online") :
				                                                  text("social.status.reconnecting");
				const auto label    = std::format("{}\n#{}  {}", player.display_name, player.id, status);
				if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_None, {0.0F, 48.0F * scale}))
				{
					value.selected_player = player.id;
				}
				const auto minimum = ImGui::GetItemRectMin();
				const auto maximum = ImGui::GetItemRectMax();
				ImGui::GetWindowDrawList()->AddRectFilled({maximum.x - 5.0F * scale, minimum.y + 8.0F * scale},
				                                          {maximum.x - 2.0F * scale, maximum.y - 8.0F * scale},
				                                          ImGui::ColorConvertFloat4ToU32(role_color(player.network_role)));
				ImGui::PopID();
			}
			if (!found)
			{
				ImGui::TextDisabled("%s", text("social.no_players").c_str());
			}
			ImGui::EndChild();
		}

		std::string report_reference(const kcd2o::remote_player_view &player, const kcd2o::client_status &status)
		{
			return std::format("KCD2Online report reference\n"
			                   "Display name: {}\nRP ID: {}\nPlayer ID: {}\n"
			                   "Server: {}\nServer ID: {}\nSession ID: {}\n",
			                   player.display_name,
			                   player.persistent_id,
			                   player.id,
			                   status.server_name,
			                   status.server_id,
			                   status.session_id);
		}

		void draw_player_details(panel_state &value, const kcd2o::client_status &status, const kcd2o::remote_player_view *player, float scale)
		{
			if (!player)
			{
				ImGui::TextDisabled("%s", text("social.select_player").c_str());
				return;
			}

			section_title(text("social.profile"));
			ImGui::TextColored(role_color(player->network_role), "%s", player->display_name.c_str());
			label_value(text("social.player_id"), std::to_string(player->id));
			label_value(text("social.rp_id"), player->persistent_id.empty() ? "-" : player->persistent_id);
			label_value(text("social.role"), role_name(player->network_role));

			const auto local            = player->id == status.local_player_id;
			const auto controls_enabled = !local && status.state == kcd2o::client_state::connected;
			if (!local)
			{
				ImGui::Spacing();
				section_title(text("social.local_controls"));
				ImGui::TextWrapped("%s", text("social.local_hint").c_str());
				ImGui::BeginDisabled(!controls_enabled);

				auto chat_hidden = ingame_social_preferences::chat_hidden(player->id);
				if (ImGui::Checkbox(text("social.hide_chat").c_str(), &chat_hidden))
				{
					ingame_social_preferences::set_chat_hidden(player->id, chat_hidden);
				}

				auto voice_volume = ingame_social_preferences::voice_volume(player->id);
				int voice_percent = static_cast<int>(std::round(voice_volume * 100.0F));
				ImGui::SetNextItemWidth(std::min(440.0F * scale, ImGui::GetContentRegionAvail().x));
				if (ImGui::SliderInt(text("social.voice_volume").c_str(), &voice_percent, 0, 150, "%d%%"))
				{
					voice_volume = static_cast<float>(voice_percent) / 100.0F;
					if (kcd2o::kcse::ui_client().set_player_voice_volume(player->id, voice_volume))
					{
						ingame_social_preferences::set_voice_volume(player->id, voice_volume);
					}
					else
					{
						set_feedback(value, text("social.voice_failed"));
					}
				}
				const auto voice_muted = voice_volume <= 0.001F;
				if (ImGui::Button(text(voice_muted ? "social.voice_unmute" : "social.voice_mute").c_str(), {210.0F * scale, 0.0F}))
				{
					const auto next_volume = voice_muted ? 1.0F : 0.0F;
					if (kcd2o::kcse::ui_client().set_player_voice_volume(player->id, next_volume))
					{
						ingame_social_preferences::set_voice_volume(player->id, next_volume);
					}
					else
					{
						set_feedback(value, text("social.voice_failed"));
					}
				}
				ImGui::EndDisabled();
			}

			ImGui::Spacing();
			section_title(text("social.report"));
			ImGui::TextWrapped("%s", text("social.report_hint").c_str());
			if (ImGui::Button(text("social.copy_rp_id").c_str(), {180.0F * scale, 0.0F}))
			{
				ImGui::SetClipboardText(player->persistent_id.c_str());
				set_feedback(value, text("social.copied_rp_id"));
			}
			ImGui::SameLine();
			if (ImGui::Button(text("social.copy_report").c_str(), {230.0F * scale, 0.0F}))
			{
				ImGui::SetClipboardText(report_reference(*player, status).c_str());
				set_feedback(value, text("social.copied_report"));
			}
			ImGui::SameLine();
			if (ImGui::Button(text("social.open_support").c_str(), {180.0F * scale, 0.0F}))
			{
				const auto url = status.support_url.starts_with("https://") ? status.support_url : std::string(default_support_url);
				(void)ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
		}
	} // namespace

	void render(bool another_panel_open)
	{
		auto &client      = kcd2o::kcse::ui_client();
		const auto status = client.status();
		static std::uint32_t social_action_generation{};
		const auto available = (status.state == kcd2o::client_state::connected || status.state == kcd2o::client_state::reconnecting) && !another_panel_open;
		g_can_open.store(available, std::memory_order_release);
		g_native_bindings_active.store(status.native_keybinds, std::memory_order_release);
		ingame_social_preferences::set_session(status.session_id);

		if (status.social_action_generation != social_action_generation)
		{
			social_action_generation  = status.social_action_generation;
			const auto suppress_until = g_suppress_native_until_ms.exchange(0, std::memory_order_acq_rel);
			if (suppress_until != 0 && GetTickCount64() <= suppress_until)
			{
				// F3 already closed the panel through the window message path.
			}
			else if (available)
			{
				set_open(!g_open.load(std::memory_order_acquire));
			}
		}
		if (!available)
		{
			set_open(false);
			return;
		}
		if (!g_open.load(std::memory_order_acquire))
		{
			return;
		}

		auto players = client.players();
		std::ranges::sort(players, {}, &kcd2o::remote_player_view::display_name);
		auto &value   = state();
		auto selected = std::ranges::find(players, value.selected_player, &kcd2o::remote_player_view::id);
		if (selected == players.end())
		{
			selected = std::ranges::find_if(players,
			                                [&](const auto &player)
			                                {
				                                return player.id != status.local_player_id;
			                                });
			if (selected == players.end() && !players.empty())
			{
				selected = players.begin();
			}
			value.selected_player = selected == players.end() ? 0 : selected->id;
		}
		const auto *player = selected == players.end() ? nullptr : &*selected;

		const auto *viewport = ImGui::GetMainViewport();
		const auto scale     = std::clamp(viewport->WorkSize.y / 1080.0F, 0.78F, 1.18F);
		const auto size =
		    ImVec2(std::min(1080.0F * scale, viewport->WorkSize.x - 36.0F), std::min(690.0F * scale, viewport->WorkSize.y - 36.0F));
		ImGui::SetNextWindowPos(
		    {viewport->WorkPos.x + viewport->WorkSize.x * 0.5F, viewport->WorkPos.y + viewport->WorkSize.y * 0.5F},
		    ImGuiCond_Always,
		    {0.5F, 0.5F});
		ImGui::SetNextWindowSize(size, ImGuiCond_Always);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16.0F * scale, 14.0F * scale});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0F * scale, 7.0F * scale});
		ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.050F, 0.041F, 0.031F, 0.97F});
		ImGui::PushStyleColor(ImGuiCol_Border, {0.53F, 0.40F, 0.22F, 0.82F});
		ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.066F, 0.053F, 0.039F, 0.88F});
		ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.095F, 0.077F, 0.056F, 0.96F});
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, {0.13F, 0.105F, 0.074F, 0.98F});
		ImGui::PushStyleColor(ImGuiCol_Button, {0.17F, 0.13F, 0.080F, 1.0F});
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.29F, 0.21F, 0.11F, 1.0F});
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.38F, 0.27F, 0.13F, 1.0F});
		ImGui::PushStyleColor(ImGuiCol_Header, {0.27F, 0.19F, 0.10F, 0.90F});
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.34F, 0.24F, 0.12F, 0.95F});
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0.42F, 0.29F, 0.13F, 1.0F});

		const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize;
		if (ImGui::Begin("##KCD2OnlineSocialPanel", nullptr, flags))
		{
			const auto position = ImGui::GetWindowPos();
			ImGui::GetWindowDrawList()->AddRectFilled(position, {position.x + size.x, position.y + 3.0F * scale}, IM_COL32(183, 126, 49, 235));
			if (g_renderer && g_renderer->font_small)
			{
				ImGui::PushFont(g_renderer->font_small);
			}

			ImGui::PushStyleColor(ImGuiCol_Text, {0.86F, 0.72F, 0.45F, 1.0F});
			ImGui::TextUnformatted(text("social.title").c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::TextDisabled("  %s", status.server_name.c_str());
			ImGui::SameLine();
			const auto hint       = text("social.close_hint");
			const auto hint_width = ImGui::CalcTextSize(hint.c_str()).x;
			ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), size.x - hint_width - 18.0F * scale));
			ImGui::TextDisabled("%s", hint.c_str());
			ImGui::Separator();

			draw_player_list(value, players, status.local_player_id, 320.0F * scale, scale);
			ImGui::SameLine();
			ImGui::BeginChild("##SocialDetails", {0.0F, 0.0F}, ImGuiChildFlags_Border);
			draw_player_details(value, status, player, scale);
			if (!value.feedback.empty() && value.feedback_until > ImGui::GetTime())
			{
				ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 38.0F * scale));
				ImGui::Separator();
				ImGui::TextWrapped("%s", value.feedback.c_str());
			}
			ImGui::EndChild();

			if (g_renderer && g_renderer->font_small)
			{
				ImGui::PopFont();
			}
		}
		ImGui::End();
		ImGui::PopStyleColor(11);
		ImGui::PopStyleVar(6);
	}

	void on_window_message(std::uint32_t message, std::uintptr_t wparam) noexcept
	{
		if (message == WM_KILLFOCUS)
		{
			g_f3_down.store(false, std::memory_order_release);
			set_open(false);
			return;
		}
		if (wparam == VK_F3)
		{
			if (message == WM_KEYUP)
			{
				g_f3_down.store(false, std::memory_order_release);
				return;
			}
			if (message == WM_KEYDOWN && !g_f3_down.exchange(true, std::memory_order_acq_rel))
			{
				if (g_open.load(std::memory_order_acquire))
				{
					set_open(false);
					if (g_native_bindings_active.load(std::memory_order_acquire))
					{
						g_suppress_native_until_ms.store(GetTickCount64() + 500, std::memory_order_release);
					}
				}
				else if (g_native_bindings_active.load(std::memory_order_acquire))
				{
					return;
				}
				else if (g_can_open.load(std::memory_order_acquire))
				{
					set_open(true);
				}
			}
			return;
		}
		if (message == WM_KEYDOWN && wparam == VK_ESCAPE && g_open.load(std::memory_order_acquire))
		{
			set_open(false);
		}
	}

	bool blocks_game_input() noexcept
	{
		return g_open.load(std::memory_order_acquire);
	}
} // namespace big::ingame_social_panel
