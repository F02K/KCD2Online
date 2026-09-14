#include "gui/ingame_staff_panel.hpp"

#include "gui/gui.hpp"
#include "gui/renderer.hpp"
#ifdef KCD2Online_NATIVE_MULTIPLAYER_MENU
	#include "gui/native_ui_localization.hpp"
#endif
#include "kcse/client_api.hpp"
#include "kcse/client_proxy.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <format>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <Windows.h>

namespace big::ingame_staff_panel
{
	namespace
	{
		constexpr std::size_t maximum_reason_bytes = 120;
		constexpr std::size_t maximum_announcement_bytes = 220;

		std::atomic_bool g_open{};
		std::atomic_bool g_can_open{};
		std::atomic_bool g_f7_down{};
		std::atomic_bool g_native_bindings_active{};
		std::atomic<std::uint64_t> g_suppress_native_until_ms{};

		void set_open(bool open) noexcept
		{
			if (g_open.exchange(open, std::memory_order_acq_rel) == open)
				return;
			if (g_gui)
				g_gui->sync_mouse_capture();
		}

		struct pending_action
		{
			std::string title;
			std::string command;
			bool dangerous{};
			bool open_popup{};
		};

		struct panel_state
		{
			std::uint64_t selected_player{};
			std::string search;
			std::string reason;
			std::string announcement;
			std::string account_target;
			std::string permission_scope{"admin.players"};
			int duration{};
			std::string feedback;
			double feedback_until{};
			std::optional<pending_action> pending;
		};

		panel_state &state()
		{
			static panel_state value;
			return value;
		}

		std::string text(std::string_view key)
		{
#ifdef KCD2Online_NATIVE_MULTIPLAYER_MENU
			return ingame_ui::localized(key);
#else
			if (key == "staff.title") return "STAFF PANEL";
			if (key == "staff.close_hint") return "F7 / Esc: close";
			if (key == "staff.players") return "PLAYERS";
			if (key == "staff.search") return "Search player...";
			if (key == "staff.no_players") return "No matching players.";
			if (key == "staff.select_player") return "Select a player on the left.";
			if (key == "staff.tab.player") return "PLAYER";
			if (key == "staff.tab.announce") return "ANNOUNCEMENT";
			if (key == "staff.tab.permissions") return "PERMISSIONS";
			if (key == "staff.tab.log") return "GM LOG";
			if (key == "staff.status.online") return "ONLINE";
			if (key == "staff.status.reconnecting") return "RECONNECTING";
			if (key == "staff.account") return "ACCOUNT";
			if (key == "staff.session") return "SESSION ID";
			if (key == "staff.quick_actions") return "QUICK ACTIONS";
			if (key == "staff.moderation") return "MODERATION";
			if (key == "staff.reason") return "Reason (required)...";
			if (key == "staff.goto") return "GO TO";
			if (key == "staff.bring") return "BRING HERE";
			if (key == "staff.freeze") return "FREEZE";
			if (key == "staff.unfreeze") return "UNFREEZE";
			if (key == "staff.warn") return "WARN";
			if (key == "staff.kick") return "KICK";
			if (key == "staff.chat_mute") return "MUTE CHAT";
			if (key == "staff.voice_mute") return "MUTE VOICE";
			if (key == "staff.chat_unmute") return "UNMUTE CHAT";
			if (key == "staff.voice_unmute") return "UNMUTE VOICE";
			if (key == "staff.ban") return "BAN";
			if (key == "staff.unban") return "UNBAN";
			if (key == "staff.duration") return "DURATION";
			if (key == "staff.offline_target") return "Player or account UUID";
			if (key == "staff.announce_hint") return "Announcement text...";
			if (key == "staff.announce_send") return "SEND ANNOUNCEMENT";
			if (key == "staff.permission_scope") return "Permission scope, e.g. admin.kick";
			if (key == "staff.permission_list") return "LIST";
			if (key == "staff.permission_grant") return "GRANT";
			if (key == "staff.permission_revoke") return "REVOKE";
			if (key == "staff.confirm") return "CONFIRM ACTION";
			if (key == "staff.confirm_yes") return "CONFIRM";
			if (key == "staff.cancel") return "CANCEL";
			if (key == "staff.feedback.sent") return "Request sent. The result appears in the GM log.";
			if (key == "staff.feedback.reason") return "Enter a reason with at least 3 characters.";
			if (key == "staff.feedback.target") return "Select a player or enter an account UUID.";
			if (key == "staff.feedback.text") return "Enter text first.";
			if (key == "staff.feedback.failed") return "The request could not be sent.";
			if (key == "staff.no_permission") return "You do not have permission for this action.";
			if (key == "staff.role.owner") return "OWNER";
			if (key == "staff.role.admin") return "ADMIN";
			if (key == "staff.role.moderator") return "MODERATOR";
			if (key == "staff.role.supporter") return "SUPPORT";
			return "USER";
#endif
		}

		void trim_input(std::string &value, std::size_t maximum)
		{
			std::replace(value.begin(), value.end(), '\r', ' ');
			std::replace(value.begin(), value.end(), '\n', ' ');
			if (value.size() <= maximum)
				return;
			std::size_t length = maximum;
			while (length > 0
			       && (static_cast<unsigned char>(value[length]) & 0xC0U) == 0x80U)
				--length;
			value.resize(length);
		}

		bool permission_matches(std::string_view granted, std::string_view required)
		{
			if (granted == "*" || granted == required)
				return true;
			if (granted.size() > 2 && granted.ends_with(".*"))
			{
				const auto prefix = granted.substr(0, granted.size() - 1);
				return required.starts_with(prefix);
			}
			return false;
		}

		bool has_permission(
		    const kcd2o::client_status &status,
		    std::string_view required)
		{
			return std::ranges::any_of(
			    status.effective_permissions,
			    [&](const auto &granted)
			    {
				    return permission_matches(granted, required);
			    });
		}

		bool has_staff_access(const kcd2o::client_status &status)
		{
			if (status.network_role >= kcd2o::protocol::NETWORK_ROLE_ADMIN)
				return true;
			return std::ranges::any_of(
			    status.effective_permissions,
			    [](const auto &permission)
			    {
				    return permission == "*" || permission == "admin.*"
				        || permission.starts_with("admin.");
			    });
		}

		std::string role_name(kcd2o::protocol::NetworkRole role)
		{
			switch (role)
			{
			case kcd2o::protocol::NETWORK_ROLE_OWNER: return text("staff.role.owner");
			case kcd2o::protocol::NETWORK_ROLE_ADMIN: return text("staff.role.admin");
			case kcd2o::protocol::NETWORK_ROLE_MODERATOR: return text("staff.role.moderator");
			case kcd2o::protocol::NETWORK_ROLE_SUPPORTER: return text("staff.role.supporter");
			default: return text("staff.role.user");
			}
		}

		ImVec4 role_color(kcd2o::protocol::NetworkRole role)
		{
			switch (role)
			{
			case kcd2o::protocol::NETWORK_ROLE_OWNER: return {1.00F, 0.76F, 0.22F, 1.0F};
			case kcd2o::protocol::NETWORK_ROLE_ADMIN: return {0.96F, 0.34F, 0.25F, 1.0F};
			case kcd2o::protocol::NETWORK_ROLE_MODERATOR: return {0.34F, 0.70F, 0.98F, 1.0F};
			case kcd2o::protocol::NETWORK_ROLE_SUPPORTER: return {0.31F, 0.85F, 0.53F, 1.0F};
			default: return {0.72F, 0.68F, 0.58F, 1.0F};
			}
		}

		bool contains_case_insensitive(std::string_view value, std::string_view query)
		{
			return std::ranges::search(
			           value,
			           query,
			           [](char left, char right)
			           {
				           return std::tolower(static_cast<unsigned char>(left))
				               == std::tolower(static_cast<unsigned char>(right));
			           })
			       .begin()
			    != value.end();
		}

		void set_feedback(panel_state &value, std::string message)
		{
			value.feedback = std::move(message);
			value.feedback_until = ImGui::GetTime() + 4.5;
		}

		bool send_command(panel_state &value, std::string command)
		{
			trim_input(command, kcd2o::kcse::text_capacity - 1);
			if (kcd2o::kcse::ui_client().send_chat(std::move(command)))
			{
				set_feedback(value, text("staff.feedback.sent"));
				return true;
			}
			set_feedback(value, text("staff.feedback.failed"));
			return false;
		}

		void request_confirmation(
		    panel_state &value,
		    std::string title,
		    std::string command,
		    bool dangerous = true)
		{
			value.pending = pending_action{
			    std::move(title), std::move(command), dangerous, true};
		}

		bool action_button(
		    std::string_view label,
		    bool allowed,
		    ImVec2 size = {})
		{
			ImGui::BeginDisabled(!allowed);
			const auto pressed = ImGui::Button(
			    std::string(label).c_str(), size);
			ImGui::EndDisabled();
			if (!allowed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", text("staff.no_permission").c_str());
			return pressed;
		}

		const char *duration_value(int index)
		{
			constexpr const char *values[] = {
			    "10", "60", "1440", "10080", "permanent"};
			return values[std::clamp(index, 0, 4)];
		}

		void draw_player_list(
		    panel_state &value,
		    const std::vector<kcd2o::remote_player_view> &players,
		    float width,
		    float scale)
		{
			ImGui::BeginChild("##StaffPlayerList", {width, 0.0F}, ImGuiChildFlags_Border);
			ImGui::PushStyleColor(ImGuiCol_Text, {0.82F, 0.68F, 0.40F, 1.0F});
			ImGui::TextUnformatted(text("staff.players").c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::TextDisabled("%zu", players.size());
			ImGui::SetNextItemWidth(-1.0F);
			ImGui::InputTextWithHint(
			    "##StaffSearch", text("staff.search").c_str(), &value.search);
			ImGui::Separator();

			bool found{};
			for (const auto &player : players)
			{
				if (!value.search.empty()
				    && !contains_case_insensitive(player.display_name, value.search)
				    && !contains_case_insensitive(std::to_string(player.id), value.search))
					continue;
				found = true;
				ImGui::PushID(static_cast<int>(player.id));
				const auto selected = value.selected_player == player.id;
				const auto label = std::format(
				    "{}\n#{}  {}",
				    player.display_name,
				    player.id,
				    player.connected ? text("staff.status.online")
				                     : text("staff.status.reconnecting"));
				if (ImGui::Selectable(
				        label.c_str(), selected, ImGuiSelectableFlags_None,
				        {0.0F, 48.0F * scale}))
				{
					value.selected_player = player.id;
					value.account_target = player.persistent_id;
				}
				const auto color = role_color(player.network_role);
				const auto minimum = ImGui::GetItemRectMin();
				const auto maximum = ImGui::GetItemRectMax();
				ImGui::GetWindowDrawList()->AddRectFilled(
				    {maximum.x - 5.0F * scale, minimum.y + 8.0F * scale},
				    {maximum.x - 2.0F * scale, maximum.y - 8.0F * scale},
				    ImGui::ColorConvertFloat4ToU32(color));
				ImGui::PopID();
			}
			if (!found)
				ImGui::TextDisabled("%s", text("staff.no_players").c_str());
			ImGui::EndChild();
		}

		void draw_player_tab(
		    panel_state &value,
		    const kcd2o::client_status &status,
		    const kcd2o::remote_player_view *player,
		    float scale)
		{
			if (!player)
			{
				ImGui::TextDisabled("%s", text("staff.select_player").c_str());
				return;
			}

			ImGui::TextColored(role_color(player->network_role), "%s", player->display_name.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("#%llu  |  %s",
			    static_cast<unsigned long long>(player->id),
			    role_name(player->network_role).c_str());
			ImGui::TextDisabled("%s: %s", text("staff.account").c_str(), player->persistent_id.c_str());
			ImGui::Separator();

			const auto half = (ImGui::GetContentRegionAvail().x - 8.0F * scale) * 0.5F;
			ImGui::PushStyleColor(ImGuiCol_Text, {0.82F, 0.68F, 0.40F, 1.0F});
			ImGui::TextUnformatted(text("staff.quick_actions").c_str());
			ImGui::PopStyleColor();
			if (action_button(text("staff.goto"), has_permission(status, "admin.teleport"), {half, 0.0F}))
				send_command(value, std::format("/goto {}", player->id));
			ImGui::SameLine();
			if (action_button(text("staff.bring"), has_permission(status, "admin.teleport"), {half, 0.0F}))
				send_command(value, std::format("/bring {}", player->id));
			if (action_button(text("staff.freeze"), has_permission(status, "admin.freeze"), {half, 0.0F}))
				send_command(value, std::format("/freeze {}", player->id));
			ImGui::SameLine();
			if (action_button(text("staff.unfreeze"), has_permission(status, "admin.freeze"), {half, 0.0F}))
				send_command(value, std::format("/unfreeze {}", player->id));

			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, {0.82F, 0.68F, 0.40F, 1.0F});
			ImGui::TextUnformatted(text("staff.moderation").c_str());
			ImGui::PopStyleColor();
			ImGui::SetNextItemWidth(-1.0F);
			ImGui::InputTextWithHint("##StaffReason", text("staff.reason").c_str(), &value.reason);
			trim_input(value.reason, maximum_reason_bytes);

			if (action_button(text("staff.warn"), has_permission(status, "admin.warn"), {half, 0.0F}))
			{
				if (value.reason.size() < 3)
					set_feedback(value, text("staff.feedback.reason"));
				else
					send_command(value, std::format("/warn {} {}", player->id, value.reason));
			}
			ImGui::SameLine();
			if (action_button(text("staff.kick"), has_permission(status, "admin.kick"), {half, 0.0F}))
			{
				if (value.reason.size() < 3)
					set_feedback(value, text("staff.feedback.reason"));
				else
					request_confirmation(value, text("staff.kick"), std::format("/kick {} {}", player->id, value.reason));
			}

			const char *duration_labels[] = {"10 min", "1 h", "24 h", "7 d", "Permanent"};
			ImGui::TextDisabled("%s", text("staff.duration").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(150.0F * scale);
			ImGui::Combo("##StaffDuration", &value.duration, duration_labels, 5);

			if (action_button(text("staff.chat_mute"), has_permission(status, "admin.mute"), {half, 0.0F}))
			{
				if (value.reason.size() < 3) set_feedback(value, text("staff.feedback.reason"));
				else request_confirmation(value, text("staff.chat_mute"), std::format("/mute chat {} {} {}", player->id, duration_value(value.duration), value.reason), false);
			}
			ImGui::SameLine();
			if (action_button(text("staff.voice_mute"), has_permission(status, "admin.mute"), {half, 0.0F}))
			{
				if (value.reason.size() < 3) set_feedback(value, text("staff.feedback.reason"));
				else request_confirmation(value, text("staff.voice_mute"), std::format("/mute voice {} {} {}", player->id, duration_value(value.duration), value.reason), false);
			}
			if (action_button(text("staff.chat_unmute"), has_permission(status, "admin.mute"), {half, 0.0F}))
			{
				if (value.reason.size() < 3) set_feedback(value, text("staff.feedback.reason"));
				else send_command(value, std::format("/unmute chat {} {}", player->id, value.reason));
			}
			ImGui::SameLine();
			if (action_button(text("staff.voice_unmute"), has_permission(status, "admin.mute"), {half, 0.0F}))
			{
				if (value.reason.size() < 3) set_feedback(value, text("staff.feedback.reason"));
				else send_command(value, std::format("/unmute voice {} {}", player->id, value.reason));
			}
		}

		void draw_announcement_tab(
		    panel_state &value,
		    const kcd2o::client_status &status)
		{
			ImGui::TextWrapped("%s", text("staff.tab.announce").c_str());
			ImGui::SetNextItemWidth(-1.0F);
			ImGui::InputTextMultiline(
			    "##StaffAnnouncement", &value.announcement, {0.0F, 150.0F});
			trim_input(value.announcement, maximum_announcement_bytes);
			ImGui::TextDisabled("%zu / %zu", value.announcement.size(), maximum_announcement_bytes);
			if (action_button(text("staff.announce_send"), has_permission(status, "admin.announce"), {-1.0F, 0.0F}))
			{
				if (value.announcement.empty())
					set_feedback(value, text("staff.feedback.text"));
				else
					request_confirmation(value, text("staff.announce_send"), "/announce " + value.announcement, false);
			}
		}

		void draw_offline_and_permissions_tab(
		    panel_state &value,
		    const kcd2o::client_status &status,
		    bool permissions)
		{
			ImGui::SetNextItemWidth(-1.0F);
			ImGui::InputTextWithHint(
			    "##StaffAccountTarget", text("staff.offline_target").c_str(), &value.account_target);
			trim_input(value.account_target, 64);

			if (permissions)
			{
				ImGui::SetNextItemWidth(-1.0F);
				ImGui::InputTextWithHint(
				    "##StaffPermission", text("staff.permission_scope").c_str(), &value.permission_scope);
				trim_input(value.permission_scope, 64);
				const auto allowed = has_permission(status, "admin.permissions")
				    && value.selected_player != 0;
				if (action_button(text("staff.permission_list"), allowed, {110.0F, 0.0F}))
					send_command(value, std::format("/perm list {}", value.selected_player));
				ImGui::SameLine();
				if (action_button(text("staff.permission_grant"), allowed && !value.permission_scope.empty(), {110.0F, 0.0F}))
					request_confirmation(value, text("staff.permission_grant"), std::format("/perm grant {} {}", value.selected_player, value.permission_scope));
				ImGui::SameLine();
				if (action_button(text("staff.permission_revoke"), allowed && !value.permission_scope.empty(), {110.0F, 0.0F}))
					request_confirmation(value, text("staff.permission_revoke"), std::format("/perm revoke {} {}", value.selected_player, value.permission_scope));
				return;
			}

			ImGui::SetNextItemWidth(-1.0F);
			ImGui::InputTextWithHint("##StaffOfflineReason", text("staff.reason").c_str(), &value.reason);
			trim_input(value.reason, maximum_reason_bytes);
			const auto valid = !value.account_target.empty() && value.reason.size() >= 3;
			const auto half = (ImGui::GetContentRegionAvail().x - 8.0F) * 0.5F;
			if (action_button(text("staff.ban"), has_permission(status, "admin.ban"), {half, 0.0F}))
			{
				if (!valid) set_feedback(value, value.account_target.empty() ? text("staff.feedback.target") : text("staff.feedback.reason"));
				else request_confirmation(value, text("staff.ban"), std::format("/ban {} {} {}", value.account_target, duration_value(value.duration), value.reason));
			}
			ImGui::SameLine();
			if (action_button(text("staff.unban"), has_permission(status, "admin.ban"), {half, 0.0F}))
			{
				if (!valid) set_feedback(value, value.account_target.empty() ? text("staff.feedback.target") : text("staff.feedback.reason"));
				else send_command(value, std::format("/unban {} {}", value.account_target, value.reason));
			}
			ImGui::TextDisabled("%s", text("staff.duration").c_str());
			ImGui::SameLine();
			const char *duration_labels[] = {"10 min", "1 h", "24 h", "7 d", "Permanent"};
			ImGui::SetNextItemWidth(150.0F);
			ImGui::Combo("##StaffOfflineDuration", &value.duration, duration_labels, 5);
		}

		void draw_log()
		{
			const auto history = kcd2o::kcse::ui_client().chat_history();
			std::vector<const kcd2o::chat_entry *> entries;
			for (const auto &entry : history)
			{
				if (entry.channel == kcd2o::protocol::CHAT_CHANNEL_ADMIN
				    || entry.channel == kcd2o::protocol::CHAT_CHANNEL_ANNOUNCEMENT)
					entries.push_back(&entry);
			}
			const auto first = entries.size() > 12 ? entries.size() - 12 : 0;
			for (std::size_t index = first; index < entries.size(); ++index)
			{
				const auto &entry = *entries[index];
				ImGui::PushStyleColor(ImGuiCol_Text, {0.78F, 0.64F, 0.38F, 1.0F});
				ImGui::TextUnformatted(entry.display_name.c_str());
				ImGui::PopStyleColor();
				ImGui::SameLine();
				ImGui::TextWrapped(": %s", entry.text.c_str());
			}
			if (entries.empty())
				ImGui::TextDisabled("-");
		}

		void draw_confirmation(panel_state &value)
		{
			if (!value.pending)
				return;
			if (value.pending->open_popup)
			{
				ImGui::OpenPopup("##StaffConfirmation");
				value.pending->open_popup = false;
			}
			ImGui::SetNextWindowSize({430.0F, 175.0F}, ImGuiCond_Appearing);
			if (!ImGui::BeginPopupModal(
			        "##StaffConfirmation", nullptr,
			        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
				return;
			ImGui::TextColored(
			    value.pending->dangerous ? ImVec4(0.94F, 0.38F, 0.28F, 1.0F)
			                             : ImVec4(0.82F, 0.68F, 0.40F, 1.0F),
			    "%s", text("staff.confirm").c_str());
			ImGui::Separator();
			ImGui::TextWrapped("%s", value.pending->title.c_str());
			ImGui::Spacing();
			if (ImGui::Button(text("staff.confirm_yes").c_str(), {190.0F, 0.0F}))
			{
				auto command = std::move(value.pending->command);
				value.pending.reset();
				ImGui::CloseCurrentPopup();
				send_command(value, std::move(command));
			}
			ImGui::SameLine();
			if (ImGui::Button(text("staff.cancel").c_str(), {190.0F, 0.0F}))
			{
				value.pending.reset();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	} // namespace

	void render(bool mod_gui_open)
	{
		auto &client = kcd2o::kcse::ui_client();
		const auto status = client.status();
		static std::uint32_t staff_action_generation{};
		const auto available = status.state == kcd2o::client_state::connected
		    && has_staff_access(status) && !mod_gui_open;
		g_can_open.store(available, std::memory_order_release);
		g_native_bindings_active.store(
		    status.native_keybinds, std::memory_order_release);
		if (status.staff_action_generation != staff_action_generation)
		{
			staff_action_generation = status.staff_action_generation;
			const auto suppress_until = g_suppress_native_until_ms.exchange(
			    0, std::memory_order_acq_rel);
			if (suppress_until != 0 && GetTickCount64() <= suppress_until)
			{
				// F7 already closed the input-capturing panel through the window
				// message path. Consume the matching native action generation.
			}
			else if (available)
				set_open(!g_open.load(std::memory_order_acquire));
		}
		if (!available)
		{
			set_open(false);
			state().pending.reset();
			return;
		}
		if (!g_open.load(std::memory_order_acquire))
			return;

		auto &value = state();
		auto players = client.players();
		std::ranges::sort(players, {}, &kcd2o::remote_player_view::display_name);
		const auto selected = std::ranges::find(
		    players, value.selected_player, &kcd2o::remote_player_view::id);
		const auto *player = selected == players.end() ? nullptr : &*selected;
		if (!player && value.selected_player != 0)
			value.selected_player = 0;

		const auto *viewport = ImGui::GetMainViewport();
		const auto scale = std::clamp(viewport->WorkSize.y / 1080.0F, 0.78F, 1.18F);
		const auto size = ImVec2(
		    std::min(1180.0F * scale, viewport->WorkSize.x - 36.0F),
		    std::min(720.0F * scale, viewport->WorkSize.y - 36.0F));
		ImGui::SetNextWindowPos(
		    {viewport->WorkPos.x + viewport->WorkSize.x * 0.5F,
		     viewport->WorkPos.y + viewport->WorkSize.y * 0.5F},
		    ImGuiCond_Always, {0.5F, 0.5F});
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
		ImGui::PushStyleColor(ImGuiCol_Tab, {0.10F, 0.08F, 0.055F, 1.0F});
		ImGui::PushStyleColor(ImGuiCol_TabHovered, {0.27F, 0.19F, 0.10F, 1.0F});
		ImGui::PushStyleColor(ImGuiCol_TabActive, {0.34F, 0.23F, 0.11F, 1.0F});

		const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
		    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize;
		if (ImGui::Begin("##KCD2OnlineStaffPanel", nullptr, flags))
		{
			const auto position = ImGui::GetWindowPos();
			ImGui::GetWindowDrawList()->AddRectFilled(
			    position, {position.x + size.x, position.y + 3.0F * scale},
			    IM_COL32(183, 126, 49, 235));
			if (g_renderer && g_renderer->font_small)
				ImGui::PushFont(g_renderer->font_small);

			ImGui::PushStyleColor(ImGuiCol_Text, {0.86F, 0.72F, 0.45F, 1.0F});
			ImGui::TextUnformatted(text("staff.title").c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::TextColored(role_color(status.network_role), "  %s", role_name(status.network_role).c_str());
			ImGui::SameLine();
			const auto hint = text("staff.close_hint");
			const auto hint_width = ImGui::CalcTextSize(hint.c_str()).x;
			ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), size.x - hint_width - 18.0F * scale));
			ImGui::TextDisabled("%s", hint.c_str());
			ImGui::Separator();

			draw_player_list(value, players, 310.0F * scale, scale);
			ImGui::SameLine();
			ImGui::BeginChild("##StaffWorkspace", {0.0F, 0.0F}, ImGuiChildFlags_Border);
			if (ImGui::BeginTabBar("##StaffTabs"))
			{
				if (ImGui::BeginTabItem(text("staff.tab.player").c_str()))
				{
					draw_player_tab(value, status, player, scale);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem(text("staff.tab.announce").c_str()))
				{
					draw_announcement_tab(value, status);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem(text("staff.moderation").c_str()))
				{
					draw_offline_and_permissions_tab(value, status, false);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem(text("staff.tab.permissions").c_str()))
				{
					draw_offline_and_permissions_tab(value, status, true);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem(text("staff.tab.log").c_str()))
				{
					draw_log();
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			if (!value.feedback.empty() && ImGui::GetTime() < value.feedback_until)
			{
				ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 32.0F * scale);
				ImGui::Separator();
				ImGui::TextWrapped("%s", value.feedback.c_str());
			}
			ImGui::EndChild();
			draw_confirmation(value);

			if (g_renderer && g_renderer->font_small)
				ImGui::PopFont();
		}
		ImGui::End();
		ImGui::PopStyleColor(14);
		ImGui::PopStyleVar(6);
	}

	void on_window_message(std::uint32_t message, std::uintptr_t wparam) noexcept
	{
		if (message == WM_KILLFOCUS)
		{
			g_f7_down.store(false, std::memory_order_release);
			set_open(false);
			return;
		}
		if (wparam == VK_F7)
		{
			if (message == WM_KEYUP)
			{
				g_f7_down.store(false, std::memory_order_release);
				return;
			}
			if (message == WM_KEYDOWN
			    && !g_f7_down.exchange(true, std::memory_order_acq_rel))
			{
				if (g_open.load(std::memory_order_acquire))
				{
					set_open(false);
					if (g_native_bindings_active.load(std::memory_order_acquire))
						g_suppress_native_until_ms.store(
						    GetTickCount64() + 500,
						    std::memory_order_release);
				}
				else if (g_native_bindings_active.load(std::memory_order_acquire))
					return;
				else if (g_can_open.load(std::memory_order_acquire))
					set_open(true);
			}
			return;
		}
		if (message == WM_KEYDOWN && wparam == VK_ESCAPE
		    && g_open.load(std::memory_order_acquire))
		{
			set_open(false);
			state().pending.reset();
		}
	}

	bool blocks_game_input() noexcept
	{
		return g_open.load(std::memory_order_acquire);
	}
} // namespace big::ingame_staff_panel
