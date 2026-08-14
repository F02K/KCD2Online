#include "gui/ingame_player_hub.hpp"

#include "gui/gui.hpp"
#include "gui/renderer.hpp"
#ifdef KCD2Online_NATIVE_MULTIPLAYER_MENU
	#include "gui/native_ui_localization.hpp"
#endif
#include "kcse/client_proxy.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <format>
#include <imgui.h>
#include <shellapi.h>
#include <string>
#include <string_view>
#include <vector>
#include <Windows.h>

namespace big::ingame_player_hub
{
	namespace
	{
		constexpr std::string_view default_support_url = "https://support.kingdom-online.cc";

		std::atomic_bool g_open{};
		std::atomic_bool g_can_open{};
		std::atomic_bool g_f2_down{};
		std::atomic_bool g_native_bindings_active{};
		std::atomic<std::uint64_t> g_suppress_native_until_ms{};

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
			if (key == "hub.title")
			{
				return "PLAYER HUB";
			}
			if (key == "hub.close_hint")
			{
				return "F2 / Esc: close";
			}
			if (key == "hub.tab.overview")
			{
				return "OVERVIEW";
			}
			if (key == "hub.tab.connection")
			{
				return "CONNECTION";
			}
			if (key == "hub.tab.controls")
			{
				return "CONTROLS";
			}
			if (key == "hub.server")
			{
				return "SERVER";
			}
			if (key == "hub.status")
			{
				return "STATUS";
			}
			if (key == "hub.status.connected")
			{
				return "CONNECTED";
			}
			if (key == "hub.status.reconnecting")
			{
				return "RECONNECTING";
			}
			if (key == "hub.players")
			{
				return "PLAYERS ONLINE";
			}
			if (key == "hub.level")
			{
				return "LEVEL";
			}
			if (key == "hub.time")
			{
				return "SERVER TIME";
			}
			if (key == "hub.weather")
			{
				return "WEATHER PROFILE";
			}
			if (key == "hub.time_scale")
			{
				return "TIME SCALE";
			}
			if (key == "hub.sleep")
			{
				return "SLEEP VOTE";
			}
			if (key == "hub.sleep.active")
			{
				return "YOU ARE SLEEPING";
			}
			if (key == "hub.sleep.waiting")
			{
				return "WAITING FOR PLAYERS";
			}
			if (key == "hub.identity")
			{
				return "IDENTITY";
			}
			if (key == "hub.display_name")
			{
				return "DISPLAY NAME";
			}
			if (key == "hub.player_id")
			{
				return "PLAYER ID";
			}
			if (key == "hub.rp_id")
			{
				return "RP ID";
			}
			if (key == "hub.role")
			{
				return "ROLE";
			}
			if (key == "hub.role.owner")
			{
				return "OWNER";
			}
			if (key == "hub.role.admin")
			{
				return "ADMIN";
			}
			if (key == "hub.role.moderator")
			{
				return "MODERATOR";
			}
			if (key == "hub.role.supporter")
			{
				return "SUPPORT";
			}
			if (key == "hub.role.user")
			{
				return "PLAYER";
			}
			if (key == "hub.session")
			{
				return "SESSION";
			}
			if (key == "hub.server_id")
			{
				return "SERVER ID";
			}
			if (key == "hub.session_id")
			{
				return "SESSION ID";
			}
			if (key == "hub.copy_diagnostic")
			{
				return "COPY DIAGNOSTIC";
			}
			if (key == "hub.copy_success")
			{
				return "Diagnostic copied to clipboard.";
			}
			if (key == "hub.support")
			{
				return "OPEN SUPPORT";
			}
			if (key == "hub.disconnect")
			{
				return "DISCONNECT";
			}
			if (key == "hub.disconnect_confirm")
			{
				return "Disconnect from this server?";
			}
			if (key == "hub.confirm")
			{
				return "CONFIRM";
			}
			if (key == "hub.cancel")
			{
				return "CANCEL";
			}
			if (key == "hub.ping")
			{
				return "PING";
			}
			if (key == "hub.loss")
			{
				return "PACKET LOSS";
			}
			if (key == "hub.queue")
			{
				return "GAME QUEUE";
			}
			if (key == "hub.quality.good")
			{
				return "GOOD";
			}
			if (key == "hub.quality.fair")
			{
				return "FAIR";
			}
			if (key == "hub.quality.poor")
			{
				return "POOR";
			}
			if (key == "hub.voice")
			{
				return "VOICE";
			}
			if (key == "hub.voice.idle")
			{
				return "READY";
			}
			if (key == "hub.voice.recording")
			{
				return "TRANSMITTING";
			}
			if (key == "hub.voice.speaking")
			{
				return "RECEIVING";
			}
			if (key == "hub.voice.normal")
			{
				return "NORMAL";
			}
			if (key == "hub.voice.whisper")
			{
				return "WHISPER";
			}
			if (key == "hub.voice.shout")
			{
				return "SHOUT";
			}
			if (key == "hub.controls.chat")
			{
				return "Enter - multiplayer chat";
			}
			if (key == "hub.controls.voice")
			{
				return "V - push-to-talk";
			}
			if (key == "hub.controls.voice_modes")
			{
				return "Ctrl+V - whisper   Shift+V - shout";
			}
			if (key == "hub.controls.emote")
			{
				return "Hold G - emote wheel";
			}
			if (key == "hub.controls.hub")
			{
				return "F2 - player hub";
			}
			if (key == "hub.controls.social")
			{
				return "F3 - social panel";
			}
			if (key == "hub.controls.staff")
			{
				return "F7 - staff panel (authorized staff only)";
			}
			if (key == "hub.controls.rebind")
			{
				return "All KCD2Online actions can be rebound in the game controls.";
			}
			if (key == "hub.restriction")
			{
				return "ACTIVE RESTRICTION";
			}
			if (key == "hub.restriction.expires")
			{
				return "Expires";
			}
			if (key == "hub.unavailable")
			{
				return "UNAVAILABLE";
			}
			return std::string(key);
#endif
		}

		std::string role_name(kcd2o::protocol::NetworkRole role)
		{
			switch (role)
			{
			case kcd2o::protocol::NETWORK_ROLE_OWNER:     return text("hub.role.owner");
			case kcd2o::protocol::NETWORK_ROLE_ADMIN:     return text("hub.role.admin");
			case kcd2o::protocol::NETWORK_ROLE_MODERATOR: return text("hub.role.moderator");
			case kcd2o::protocol::NETWORK_ROLE_SUPPORTER: return text("hub.role.supporter");
			default:                                      return text("hub.role.user");
			}
		}

		ImVec4 quality_color(int ping_ms, float packet_loss)
		{
			if (ping_ms < 0 || ping_ms > 180 || packet_loss >= 5.0F)
			{
				return {0.90F, 0.30F, 0.24F, 1.0F};
			}
			if (ping_ms > 90 || packet_loss >= 1.0F)
			{
				return {0.91F, 0.66F, 0.22F, 1.0F};
			}
			return {0.35F, 0.78F, 0.39F, 1.0F};
		}

		std::string quality_name(int ping_ms, float packet_loss)
		{
			if (ping_ms < 0 || ping_ms > 180 || packet_loss >= 5.0F)
			{
				return text("hub.quality.poor");
			}
			if (ping_ms > 90 || packet_loss >= 1.0F)
			{
				return text("hub.quality.fair");
			}
			return text("hub.quality.good");
		}

		std::string time_text(const kcd2o::client_status &status)
		{
			if (!status.environment_available || !std::isfinite(status.time_of_day_hours))
			{
				return text("hub.unavailable");
			}
			const auto total_minutes = static_cast<int>(std::round(status.time_of_day_hours * 60.0)) % (24 * 60);
			return std::format("{:02}:{:02}", total_minutes / 60, total_minutes % 60);
		}

		std::string voice_range_text(kcd2o::protocol::VoiceRange range)
		{
			switch (range)
			{
			case kcd2o::protocol::VOICE_RANGE_WHISPER: return text("hub.voice.whisper");
			case kcd2o::protocol::VOICE_RANGE_SHOUT:   return text("hub.voice.shout");
			default:                                   return text("hub.voice.normal");
			}
		}

		std::string restriction_expiry(std::uint64_t unix_ms)
		{
			if (unix_ms == 0)
			{
				return "-";
			}
			const auto seconds = static_cast<__time64_t>(unix_ms / 1000);
			tm local{};
			if (_localtime64_s(&local, &seconds) != 0)
			{
				return "-";
			}
			return std::format("{:02}.{:02}.{:04} {:02}:{:02}",
			                   local.tm_mday,
			                   local.tm_mon + 1,
			                   local.tm_year + 1900,
			                   local.tm_hour,
			                   local.tm_min);
		}

		void label_value(std::string_view label, std::string_view value)
		{
			ImGui::TextDisabled("%.*s", static_cast<int>(label.size()), label.data());
			ImGui::SameLine();
			ImGui::TextWrapped("%.*s", static_cast<int>(value.size()), value.data());
		}

		void section_title(std::string_view title)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, {0.86F, 0.72F, 0.45F, 1.0F});
			ImGui::TextUnformatted(title.data(), title.data() + title.size());
			ImGui::PopStyleColor();
			ImGui::Separator();
		}

		const kcd2o::remote_player_view *local_player(const std::vector<kcd2o::remote_player_view> &players, std::uint64_t local_player_id)
		{
			const auto found = std::ranges::find(players, local_player_id, &kcd2o::remote_player_view::id);
			return found == players.end() ? nullptr : &*found;
		}

		std::string diagnostic_text(const kcd2o::client_status &status, std::size_t players)
		{
			return std::format("KCD2Online player diagnostic\n"
			                   "Server: {}\nServer ID: {}\nSession ID: {}\nLevel: {}\n"
			                   "Players: {}\nPing: {} ms\nPacket loss: {:.1f}%\nGame queue: {}\n"
			                   "Server time: {}\nWeather profile: {}\n",
			                   status.server_name,
			                   status.server_id,
			                   status.session_id,
			                   status.level_id,
			                   players,
			                   status.ping_ms,
			                   status.packet_loss_percent,
			                   status.game_queue_size,
			                   time_text(status),
			                   status.environment_available ? std::to_string(status.weather_id) : text("hub.unavailable"));
		}

		void draw_overview(const kcd2o::client_status &status, const std::vector<kcd2o::remote_player_view> &players, float scale)
		{
			const auto *player      = local_player(players, status.local_player_id);
			const auto half_width   = (ImGui::GetContentRegionAvail().x - 10.0F * scale) * 0.5F;
			const auto child_height = ImGui::GetContentRegionAvail().y;

			ImGui::BeginChild("##HubWorld", {half_width, child_height}, ImGuiChildFlags_Border);
			section_title(text("hub.server"));
			label_value(text("hub.server"), status.server_name);
			label_value(text("hub.status"), text(status.state == kcd2o::client_state::reconnecting ? "hub.status.reconnecting" : "hub.status.connected"));
			label_value(text("hub.players"), std::to_string(players.size()));
			label_value(text("hub.level"), status.level_id);
			label_value(text("hub.time"), time_text(status));
			label_value(text("hub.weather"), status.environment_available ? std::to_string(status.weather_id) : text("hub.unavailable"));
			label_value(text("hub.time_scale"), status.environment_available ? std::format("{:.1f}x", status.time_scale) : text("hub.unavailable"));

			ImGui::Spacing();
			section_title(text("hub.sleep"));
			const auto sleep_count = std::format("{} / {}", status.sleeping_players, status.sleeping_players_required);
			label_value(text("hub.sleep"), sleep_count);
			ImGui::TextColored(status.sleeping ? ImVec4(0.35F, 0.78F, 0.39F, 1.0F) : ImVec4(0.73F, 0.69F, 0.60F, 1.0F),
			                   "%s",
			                   text(status.sleeping ? "hub.sleep.active" : "hub.sleep.waiting").c_str());
			ImGui::EndChild();

			ImGui::SameLine();
			ImGui::BeginChild("##HubIdentity", {0.0F, child_height}, ImGuiChildFlags_Border);
			section_title(text("hub.identity"));
			label_value(text("hub.display_name"), player ? player->display_name : text("hub.unavailable"));
			label_value(text("hub.player_id"), std::to_string(status.local_player_id));
			label_value(text("hub.rp_id"), player && !player->persistent_id.empty() ? player->persistent_id : text("hub.unavailable"));
			label_value(text("hub.role"), role_name(status.network_role));

			ImGui::Spacing();
			section_title(text("hub.session"));
			label_value(text("hub.server_id"), status.server_id);
			label_value(text("hub.session_id"), status.session_id);

			if (!status.restriction_kind.empty())
			{
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, {0.94F, 0.38F, 0.28F, 1.0F});
				ImGui::TextUnformatted(text("hub.restriction").c_str());
				ImGui::PopStyleColor();
				ImGui::Separator();
				ImGui::TextWrapped("%s", status.restriction_reason.c_str());
				label_value(text("hub.restriction.expires"), restriction_expiry(status.restriction_expires_at_unix_ms));
			}
			ImGui::EndChild();
		}

		void draw_connection(const kcd2o::client_status &status, std::size_t player_count, bool &disconnect_confirmation, double &feedback_until, float scale)
		{
			const auto color = quality_color(status.ping_ms, status.packet_loss_percent);
			ImGui::TextColored(color,
			                   "%s  -  %s",
			                   text("hub.status").c_str(),
			                   quality_name(status.ping_ms, status.packet_loss_percent).c_str());
			ImGui::Separator();
			label_value(text("hub.ping"), status.ping_ms >= 0 ? std::format("{} ms", status.ping_ms) : text("hub.unavailable"));
			label_value(text("hub.loss"), std::format("{:.1f}%", status.packet_loss_percent));
			label_value(text("hub.queue"), std::to_string(status.game_queue_size));

			ImGui::Spacing();
			section_title(text("hub.voice"));
			const auto voice_state = status.voice_recording ? text("hub.voice.recording") :
			                         status.voice_speaking  ? text("hub.voice.speaking") :
			                                                  text("hub.voice.idle");
			label_value(text("hub.status"), voice_state);
			label_value(text("hub.voice"), voice_range_text(status.voice_range));
			ImGui::ProgressBar(std::clamp(status.voice_level, 0.0F, 1.0F),
			                   {std::min(420.0F * scale, ImGui::GetContentRegionAvail().x), 8.0F * scale},
			                   "");

			ImGui::Spacing();
			ImGui::Separator();
			if (ImGui::Button(text("hub.copy_diagnostic").c_str(), {210.0F * scale, 0.0F}))
			{
				ImGui::SetClipboardText(diagnostic_text(status, player_count).c_str());
				feedback_until = ImGui::GetTime() + 3.0;
			}
			ImGui::SameLine();
			if (ImGui::Button(text("hub.support").c_str(), {180.0F * scale, 0.0F}))
			{
				const auto url = status.support_url.starts_with("https://") ? status.support_url : std::string(default_support_url);
				(void)ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Button, {0.42F, 0.12F, 0.08F, 1.0F});
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.60F, 0.18F, 0.11F, 1.0F});
			if (ImGui::Button(text("hub.disconnect").c_str(), {180.0F * scale, 0.0F}))
			{
				disconnect_confirmation = true;
				ImGui::OpenPopup("##HubDisconnect");
			}
			ImGui::PopStyleColor(2);

			if (feedback_until > ImGui::GetTime())
			{
				ImGui::TextColored({0.35F, 0.78F, 0.39F, 1.0F}, "%s", text("hub.copy_success").c_str());
			}

			if (disconnect_confirmation)
			{
				ImGui::SetNextWindowSize({420.0F, 150.0F}, ImGuiCond_Appearing);
				if (ImGui::BeginPopupModal("##HubDisconnect", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
				{
					ImGui::TextWrapped("%s", text("hub.disconnect_confirm").c_str());
					ImGui::Spacing();
					if (ImGui::Button(text("hub.confirm").c_str(), {180.0F, 0.0F}))
					{
						disconnect_confirmation = false;
						ImGui::CloseCurrentPopup();
						set_open(false);
						kcd2o::kcse::ui_client().disconnect();
					}
					ImGui::SameLine();
					if (ImGui::Button(text("hub.cancel").c_str(), {180.0F, 0.0F}))
					{
						disconnect_confirmation = false;
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
			}
		}

		void draw_controls()
		{
			section_title(text("hub.tab.controls"));
			for (const auto key : {"hub.controls.chat", "hub.controls.voice", "hub.controls.voice_modes", "hub.controls.emote", "hub.controls.hub", "hub.controls.social", "hub.controls.staff"})
			{
				ImGui::BulletText("%s", text(key).c_str());
			}
			ImGui::Spacing();
			ImGui::TextWrapped("%s", text("hub.controls.rebind").c_str());
		}
	} // namespace

	void render(bool another_panel_open)
	{
		auto &client      = kcd2o::kcse::ui_client();
		const auto status = client.status();
		static std::uint32_t player_hub_action_generation{};
		static bool disconnect_confirmation{};
		static double feedback_until{};
		const auto available = (status.state == kcd2o::client_state::connected || status.state == kcd2o::client_state::reconnecting) && !another_panel_open;
		g_can_open.store(available, std::memory_order_release);
		g_native_bindings_active.store(status.native_keybinds, std::memory_order_release);
		if (status.player_hub_action_generation != player_hub_action_generation)
		{
			player_hub_action_generation = status.player_hub_action_generation;
			const auto suppress_until    = g_suppress_native_until_ms.exchange(0, std::memory_order_acq_rel);
			if (suppress_until != 0 && GetTickCount64() <= suppress_until)
			{
				// F2 already closed the input-capturing panel through the window
				// message path. Consume the matching native action generation.
			}
			else if (available)
			{
				set_open(!g_open.load(std::memory_order_acquire));
			}
		}
		if (!available)
		{
			set_open(false);
			disconnect_confirmation = false;
			return;
		}
		if (!g_open.load(std::memory_order_acquire))
		{
			return;
		}

		const auto players   = client.players();
		const auto *viewport = ImGui::GetMainViewport();
		const auto scale     = std::clamp(viewport->WorkSize.y / 1080.0F, 0.78F, 1.18F);
		const auto size =
		    ImVec2(std::min(980.0F * scale, viewport->WorkSize.x - 36.0F), std::min(650.0F * scale, viewport->WorkSize.y - 36.0F));
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
		ImGui::PushStyleColor(ImGuiCol_Button, {0.17F, 0.13F, 0.080F, 1.0F});
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.29F, 0.21F, 0.11F, 1.0F});
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.38F, 0.27F, 0.13F, 1.0F});
		ImGui::PushStyleColor(ImGuiCol_Header, {0.27F, 0.19F, 0.10F, 0.90F});
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0.34F, 0.24F, 0.12F, 0.95F});
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0.42F, 0.29F, 0.13F, 1.0F});
		ImGui::PushStyleColor(ImGuiCol_Tab, {0.10F, 0.08F, 0.055F, 1.0F});
		ImGui::PushStyleColor(ImGuiCol_TabHovered, {0.27F, 0.19F, 0.10F, 1.0F});
		ImGui::PushStyleColor(ImGuiCol_TabActive, {0.34F, 0.23F, 0.11F, 1.0F});

		const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize;
		if (ImGui::Begin("##KCD2OnlinePlayerHub", nullptr, flags))
		{
			const auto position = ImGui::GetWindowPos();
			ImGui::GetWindowDrawList()->AddRectFilled(position, {position.x + size.x, position.y + 3.0F * scale}, IM_COL32(183, 126, 49, 235));
			if (g_renderer && g_renderer->font_small)
			{
				ImGui::PushFont(g_renderer->font_small);
			}

			ImGui::PushStyleColor(ImGuiCol_Text, {0.86F, 0.72F, 0.45F, 1.0F});
			ImGui::TextUnformatted(text("hub.title").c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::TextDisabled("  %s", status.server_name.c_str());
			ImGui::SameLine();
			const auto hint       = text("hub.close_hint");
			const auto hint_width = ImGui::CalcTextSize(hint.c_str()).x;
			ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), size.x - hint_width - 18.0F * scale));
			ImGui::TextDisabled("%s", hint.c_str());
			ImGui::Separator();

			if (ImGui::BeginTabBar("##HubTabs"))
			{
				if (ImGui::BeginTabItem(text("hub.tab.overview").c_str()))
				{
					draw_overview(status, players, scale);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem(text("hub.tab.connection").c_str()))
				{
					draw_connection(status, players.size(), disconnect_confirmation, feedback_until, scale);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem(text("hub.tab.controls").c_str()))
				{
					draw_controls();
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}

			if (g_renderer && g_renderer->font_small)
			{
				ImGui::PopFont();
			}
		}
		ImGui::End();
		ImGui::PopStyleColor(13);
		ImGui::PopStyleVar(6);
	}

	void on_window_message(std::uint32_t message, std::uintptr_t wparam) noexcept
	{
		if (message == WM_KILLFOCUS)
		{
			g_f2_down.store(false, std::memory_order_release);
			set_open(false);
			return;
		}
		if (wparam == VK_F2)
		{
			if (message == WM_KEYUP)
			{
				g_f2_down.store(false, std::memory_order_release);
				return;
			}
			if (message == WM_KEYDOWN && !g_f2_down.exchange(true, std::memory_order_acq_rel))
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
} // namespace big::ingame_player_hub
